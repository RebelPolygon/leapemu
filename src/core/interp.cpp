#include "core/interp.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace leap {

namespace {

int luma(u32 p) { return int(((p >> 16) & 0xff) * 2 + ((p >> 8) & 0xff) * 5 + (p & 0xff)); }  // 0..2040

int color_dist(u32 a, u32 b) {
  return std::abs(int((a >> 16) & 0xff) - int((b >> 16) & 0xff)) +
         std::abs(int((a >> 8) & 0xff) - int((b >> 8) & 0xff)) + std::abs(int(a & 0xff) - int(b & 0xff));
}

}  // namespace

void FrameInterpolator::reset() {
  fq_.clear();
  fq_before_.reset();
  fb_.reset();
  flash_ok_ = false;
  flash_mismatch_ = 0;
  show_t_ = clock_t_ = 0;
  drawn_ = {};
  checked_.reset();
  have_a_ = have_b_ = false;
  cut_ = false;
  objects_.clear();
}

// A new captured frame (Flash or a native draw list) joins the queue the
// display clock plays back.
void FrameInterpolator::enqueue(std::shared_ptr<const NativeFrame> f, double t) {
  if (fq_.empty() || t - fq_.back().t > max_capture_gap || t < fq_.back().t || bool(fq_.back().f->flash) != bool(f->flash)) {
    // The first frame, one after a pause (a menu waiting for input), or a
    // different kind of content: shown at once, with no motion into it.
    fq_.clear();
    fq_before_.reset();
    show_t_ = clock_t_ = t;
  } else {
    advance_clock(t);
  }
  fq_.push_back({f, t, false});
  fb_ = std::move(f);
  // Keep the frames the clock can still reach.
  while (fq_.size() > 2 && fq_[1].t <= show_t_) { fq_before_ = fq_.front().f; fq_.pop_front(); }
  while (fq_.size() > 8) { fq_before_ = fq_.front().f; fq_.pop_front(); }
}

// Native draw lists (tile planes, the Cars race): usable while the LCD shows
// exactly the image they reproduce.
void FrameInterpolator::push_drawn(const u32* fb, u64 frame_index, std::shared_ptr<const NativeFrame> native) {
  if (native != fb_) enqueue(native, double(frame_index));
  bool shown = native->trusted() && native->screen.size() == size_t(kW) * kH;
  for (size_t i = 0; shown && i < native->screen.size(); i++) shown = rgb12_to_argb(native->screen[i]) == (fb[i] | 0xff000000u);
  fq_.back().ok = shown;
}

void FrameInterpolator::push_flash(const u32* fb, u64 frame_index, std::shared_ptr<const NativeFrame> native) {
  if (native != fb_) {
    if (!fq_.empty()) fq_.back().ok = flash_ok_;
    enqueue(native, double(frame_index));
  }
  // Trust the redraw while it matches what the LCD shows (it stops matching
  // when the game shows something the player didn't draw). The 1x redraw is
  // made once per capture; each new LCD image is compared with it.
  if (checked_.get() != fb_.get()) {
    checked_ = fb_;
    check_complete_ = flash_.render(*fb_->flash, nullptr, 1.0, 1, flash_check_);
    check_uncaptured_ = flash_.uncaptured_fields();
    check_captured_ = flash_.captured_fields();
  }
  bool complete = check_complete_;
  auto differs = [&](size_t i) {
    const u32 p = fb[i], q = flash_check_[i];
    int d = 0;
    for (int s = 0; s < 24; s += 8) d = std::max(d, std::abs(int((p >> s) & 255) - int((q >> s) & 255)));
    return d > 48;
  };
  int differ = 0;
  for (size_t i = 0; i < flash_check_.size(); i++) differ += differs(i);
  auto field_differs = [&](const std::array<int, 4>& r) {
    int bad = 0, area = 0;
    for (int y = r[1]; y < r[3]; y++)
      for (int x = r[0]; x < r[2]; x++, area++) bad += differs(size_t(y) * kW + size_t(x));
    return area && bad * 10 > area;
  };
  // A text field whose text was never seen (e.g. after loading a state) must
  // really be empty on the LCD.
  for (const auto& r : check_uncaptured_)
    if (field_differs(r)) complete = false;
  // Text drawn some other way (outline fonts, or by the game itself over the
  // Flash frame) would be missing: small, so the whole-screen comparison
  // alone lets it through. At 1x captured text matches the LCD exactly.
  bool text_differs = false;
  for (const auto& r : check_captured_) text_differs = text_differs || field_differs(r);
  // The LCD is often mid-update (the player sends dirty regions in several
  // DMAs), so only a mismatch that persists means the redraw is not usable.
  if (differ < kW * kH / 4 && !text_differs) flash_mismatch_ = 0;
  else flash_mismatch_ = std::min(flash_mismatch_ + 1, 4);
  flash_ok_ = complete && flash_mismatch_ < 4;
  if (!fq_.empty()) fq_.back().ok = flash_ok_;
}

// The typical interval between captured frames: the median of the recent ones.
double FrameInterpolator::capture_period() const {
  std::vector<double> gaps;
  for (size_t i = 1; i < fq_.size(); i++) gaps.push_back(fq_[i].t - fq_[i - 1].t);
  if (gaps.empty()) return 7;
  std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
  return std::clamp(gaps[gaps.size() / 2], 3.0, 12.0);
}

// Moves the display clock to time t: at normal speed while it lags the newest
// frame by about one interval, up to 1.5x when further behind, down to 0.75x
// when about to run out of frames, and never past the newest frame.
void FrameInterpolator::advance_clock(double t) {
  double dt = t - clock_t_;
  if (dt < 0 || dt > 30) dt = 0;  // time went back (rewind) or jumped: resynchronise
  clock_t_ = t;
  if (fq_.empty()) return;
  const double period = capture_period();
  const double ahead = fq_.back().t - show_t_;
  const double rate = std::clamp(1.0 + 0.5 * (ahead - period) / period, 0.75, 1.5);
  show_t_ = std::clamp(show_t_ + dt * rate, fq_.front().t, fq_.back().t);
}

void FrameInterpolator::push(const u32* fb, u64 frame_index, std::shared_ptr<const NativeFrame> native) {
  const size_t n = size_t(kW) * kH;
  if (mode_ == Mode::Native) {
    if (native && native->flash) push_flash(fb, frame_index, native);
    else if (native) push_drawn(fb, frame_index, native);
    else if (fb_ && fb_->flash) push_flash(fb, frame_index, fb_);  // (the LCD still showing it?)
    else if (fb_) fq_.back().ok = false;  // no draw list for what the LCD shows now
  }
  if (have_b_ && std::memcmp(fb, b_.data(), n * sizeof(u32)) == 0) return;  // no new game frame
  if (have_b_) {
    a_.swap(b_);
    la_.swap(lb_);
    ta_ = tb_;
    have_a_ = true;
  }
  b_.assign(fb, fb + n);
  lb_.resize(n);
  for (size_t i = 0; i < n; i++) lb_[i] = luma(b_[i]);
  tb_ = double(frame_index);
  have_b_ = true;
  if (have_a_) {
    // Long pauses (menus, static screens) switch instantly.
    cut_ = (tb_ - ta_) > max_gap;
    if (!cut_ && (mode_ == Mode::Motion || (mode_ == Mode::Native && !captured_ready()))) analyse();
  }
}

// Motion analysis (pixel-art oriented, two layers):
//  1. camera motion: the global displacement that best maps the previous
//     frame onto the newest one;
//  2. foreground: pixels the camera motion does not explain, grouped into
//     8-connected objects, each with its own displacement.
void FrameInterpolator::analyse() {
  const int n = kW * kH;
  constexpr int kTol = 24;  // colour distance treated as "same pixel"

  // 1. Camera motion, coarse search on a sub-sampled grid.
  constexpr int kRange = 16;
  u64 best = ULLONG_MAX;
  global_ = {};
  for (int dy = -kRange; dy <= kRange; dy++) {
    for (int dx = -kRange; dx <= kRange; dx++) {
      u64 sad = 0;
      int count = 0;
      for (int y = 8; y < kH - 8; y += 3) {
        const int sy = y - dy;
        if (sy < 0 || sy >= kH) continue;
        for (int x = 8; x < kW - 8; x += 3) {
          const int sx = x - dx;
          if (sx < 0 || sx >= kW) continue;
          sad += u64(std::abs(lb_[size_t(y) * kW + x] - la_[size_t(sy) * kW + sx]));
          count++;
        }
      }
      if (count < 200) continue;
      const u64 score = sad * 64 / u64(count) + u64(std::abs(dx) + std::abs(dy));
      if (score < best) { best = score; global_ = {dx, dy}; }
    }
  }

  // 2. Foreground masks.
  fg_a_.assign(n, 0);
  fg_b_.assign(n, 0);
  int fg_count = 0;
  for (int y = 0; y < kH; y++)
    for (int x = 0; x < kW; x++) {
      const int i = y * kW + x;
      const int ax = x - global_.dx, ay = y - global_.dy;  // where b's pixel came from
      if (ax >= 0 && ay >= 0 && ax < kW && ay < kH && color_dist(b_[i], a_[ay * kW + ax]) > kTol) { fg_b_[i] = 1; fg_count++; }
      const int bx = x + global_.dx, by = y + global_.dy;  // where a's pixel went
      if (bx >= 0 && by >= 0 && bx < kW && by < kH && color_dist(a_[i], b_[by * kW + bx]) > kTol) fg_a_[i] = 1;
    }
  // Mostly-foreground frames are scene changes: switch instead of warping.
  if (fg_count > n / 2) { cut_ = true; objects_.clear(); return; }

  // 3. Objects: 8-connected components of the newest frame's foreground.
  objects_.clear();
  std::vector<u8> seen(n, 0);
  std::vector<int> stack;
  for (int start = 0; start < n; start++) {
    if (!fg_b_[start] || seen[start]) continue;
    Object obj;
    stack.assign(1, start);
    seen[start] = 1;
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      obj.pixels.push_back(i);
      const int x = i % kW, y = i / kW;
      // Connect across gaps of up to 2 pixels so a sprite's separate parts
      // (e.g. shoes and body) move as one object.
      for (int oy = -3; oy <= 3; oy++)
        for (int ox = -3; ox <= 3; ox++) {
          const int nx = x + ox, ny = y + oy;
          if (nx < 0 || ny < 0 || nx >= kW || ny >= kH) continue;
          const int j = ny * kW + nx;
          if (fg_b_[j] && !seen[j]) { seen[j] = 1; stack.push_back(j); }
        }
    }
    // Best displacement for the whole object (coarse-to-fine search).
    auto cost = [&](Vec d) {
      long c = 0;
      for (int i : obj.pixels) {
        const int x = i % kW - d.dx, y = i / kW - d.dy;
        c += (x < 0 || y < 0 || x >= kW || y >= kH) ? 255 : color_dist(b_[i], a_[y * kW + x]);
      }
      return c;
    };
    Vec bd = global_;
    long bc = cost(bd);
    for (int dy = -16; dy <= 16; dy += 2)
      for (int dx = -16; dx <= 16; dx += 2) {
        const long c = cost(Vec{dx, dy});
        if (c < bc) { bc = c; bd = {dx, dy}; }
      }
    const Vec c0 = bd;
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        const long c = cost(Vec{c0.dx + dx, c0.dy + dy});
        if (c < bc) { bc = c; bd = {c0.dx + dx, c0.dy + dy}; }
      }
    obj.d = bd;
    obj.explained = bc <= long(obj.pixels.size()) * 40;
    objects_.push_back(std::move(obj));
  }
}

void FrameInterpolator::compose(double t) {
  const size_t n = size_t(kW) * kH;
  out_.resize(n);
  // Background: camera-displaced sample from the nearer frame, avoiding pixels
  // covered by foreground in that frame (take the other frame there).
  const int gax = -int(std::lround(t * global_.dx)), gay = -int(std::lround(t * global_.dy));
  const int gbx = int(std::lround((1.0 - t) * global_.dx)), gby = int(std::lround((1.0 - t) * global_.dy));
  const bool prefer_a = t < 0.5;
  for (int y = 0; y < kH; y++) {
    for (int x = 0; x < kW; x++) {
      const int ax = std::clamp(x + gax, 0, kW - 1), ay = std::clamp(y + gay, 0, kH - 1);
      const int bx = std::clamp(x + gbx, 0, kW - 1), by = std::clamp(y + gby, 0, kH - 1);
      const int ia = ay * kW + ax, ib = by * kW + bx;
      const bool ok_a = !fg_a_[ia], ok_b = !fg_b_[ib];
      u32 v;
      if (prefer_a && ok_a) v = a_[ia];
      else if (ok_b) v = b_[ib];
      else if (ok_a) v = a_[ia];
      else {
        // Background hidden in both frames (under the object's old and new
        // positions): take the nearest visible background pixel.
        v = prefer_a ? a_[ia] : b_[ib];
        bool found = false;
        for (int r = 1; r <= 6 && !found; r++)
          for (int oy = -r; oy <= r && !found; oy++)
            for (int ox = -r; ox <= r && !found; ox++) {
              if (std::max(std::abs(ox), std::abs(oy)) != r) continue;
              const int sx = std::clamp(bx + ox, 0, kW - 1), sy = std::clamp(by + oy, 0, kH - 1);
              const int j = sy * kW + sx;
              if (!fg_b_[j]) { v = b_[j]; found = true; }
            }
      }
      out_[size_t(y) * kW + x] = v;
    }
  }
  // Foreground objects, drawn on top. Explained objects glide from their old
  // to their new position; others switch at the midpoint.
  for (const Object& obj : objects_) {
    if (obj.explained) {
      const int ox = -int(std::lround((1.0 - t) * obj.d.dx)), oy = -int(std::lround((1.0 - t) * obj.d.dy));
      for (int i : obj.pixels) {
        const int x = i % kW + ox, y = i / kW + oy;
        if (x >= 0 && y >= 0 && x < kW && y < kH) out_[size_t(y) * kW + x] = b_[i];
      }
    } else {
      for (int i : obj.pixels) out_[i] = prefer_a ? a_[i] : b_[i];
    }
  }
}

// Progress from A (0) to B (1): B animates in over one game-frame interval,
// starting when it arrived.
double FrameInterpolator::alpha_at(double t) const {
  const double span = std::max(1.0, tb_ - ta_);
  return (t - tb_) / span;
}

bool FrameInterpolator::layers(double t, int scale, std::vector<NativeLayer>& out) {
  out.clear();
  if (mode_ != Mode::Native || !captured_ready()) return false;
  // The two frames around the display clock, and how far between them it is.
  // Motion into a frame lasts at most 1.5 typical intervals, ending at the
  // frame: a longer gap is the previous frame held, then motion.
  std::shared_ptr<const NativeFrame> show = fb_, prev, before;
  double alpha = 1.0;
  if (smoothing) {
    advance_clock(t);
    size_t q = 0;
    while (q + 1 < fq_.size() && fq_[q].t < show_t_) q++;
    if (!fq_[q].ok) return false;
    show = fq_[q].f;
    if (q > 0) {
      const double span = std::min(fq_[q].t - fq_[q - 1].t, 1.5 * capture_period());
      alpha = span > 0 ? std::clamp(1.0 - (fq_[q].t - show_t_) / span, 0.0, 1.0) : 1.0;
      if (alpha < 1.0) prev = fq_[q - 1].f;  // (at 1, the frame alone)
      if (alpha < 1.0) before = q > 1 ? fq_[q - 2].f : fq_before_;
    }
  }
  scale = std::clamp(scale, 1, 8);
  if (!show->flash) {
    native_layers(*show, prev.get(), alpha, layer_storage_, out, scale);
    return true;
  }
  if (show != drawn_.show || prev != drawn_.prev || before != drawn_.before || alpha != drawn_.alpha || scale != drawn_.scale) {
    drawn_ = {};
    if (!flash_.render(*show->flash, prev ? prev->flash.get() : nullptr, alpha, scale, flash_img_, before ? before->flash.get() : nullptr))
      return false;
    drawn_ = {show, prev, before, alpha, scale};
    flash_version_++;
  }
  NativeLayer L;
  L.px = flash_img_.data();
  L.w = L.h = kW * scale;
  L.texel = 1.0f / float(scale);
  L.version = flash_version_;
  out.push_back(L);
  return true;
}

const u32* FrameInterpolator::render(double t) {
  if (!have_b_) return nullptr;
  if (mode_ == Mode::Off || !smoothing || !have_a_ || cut_) return b_.data();
  if (mode_ == Mode::Native && captured_ready()) {
    if (!layers(t, 1, layer_tmp_)) return b_.data();
    composite_layers(layer_tmp_, 1, composite_);
    return composite_.data();
  }
  const double alpha = alpha_at(t);
  if (alpha >= 1.0) return b_.data();
  if (alpha <= 0.0) return a_.data();
  compose(alpha);  // Motion, or Native without draw lists (analysed in push())
  return out_.data();
}

}  // namespace leap
