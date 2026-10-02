#include "core/flash/render.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <unordered_set>

#include "core/bus.h"

namespace leap::flash {

namespace {

constexpr int kScreen = 160;  // screen size in pixels (3200 twips)

struct Rgba { float r, g, b, a; };  // 0..255, straight alpha

Rgba unpack(u32 rgba) {
  return Rgba{float(rgba >> 24), float((rgba >> 16) & 255), float((rgba >> 8) & 255), float(rgba & 255)};
}

Rgba apply(const swf::CxForm& cx, Rgba c) {
  auto ch = [&](float v, int i) { return std::clamp(v * cx.mul[i] + cx.add[i], 0.0f, 255.0f); };
  return Rgba{ch(c.r, 0), ch(c.g, 1), ch(c.b, 2), ch(c.a, 3)};
}

void blend(u32& d, const Rgba& s, float cov) {
  const float a = s.a / 255.0f * cov;
  if (a <= 0) return;
  // (Rounds halves up; the channels are never negative.)
  auto ch = [&](int sh, float v) { return u32(int(float((d >> sh) & 255) * (1 - a) + v * a + 0.5f)) << sh; };
  d = 0xff000000u | ch(16, s.r) | ch(8, s.g) | ch(0, s.b);
}

inline int floor_int(double v) {
  const int i = int(v);
  return v < i ? i - 1 : i;
}

// Worker threads for drawing bands in parallel, kept for the whole run.
class Pool {
 public:
  static Pool& get() {
    static Pool pool;
    return pool;
  }
  int threads() const { return int(workers_.size()) + 1; }
  // Calls fn(i) for every i in [0, n), on the workers and the calling thread.
  void run(int n, const std::function<void(int)>& fn) {
    std::lock_guard<std::mutex> one(run_mutex_);
    {
      std::lock_guard<std::mutex> lk(m_);
      fn_ = &fn;
      n_ = n;
      next_ = 0;
      busy_ = int(workers_.size());
      gen_++;
    }
    cv_.notify_all();
    work();
    std::unique_lock<std::mutex> lk(m_);
    done_.wait(lk, [&] { return busy_ == 0; });
    fn_ = nullptr;
  }

 private:
  Pool() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 1; i < std::min(hw, 16u); i++) workers_.emplace_back([this] { loop(); });
  }
  ~Pool() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }
  void work() {
    for (int i; (i = next_.fetch_add(1)) < n_;) (*fn_)(i);
  }
  void loop() {
    u64 seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return stop_ || gen_ != seen; });
        if (stop_) return;
        seen = gen_;
      }
      work();
      {
        std::lock_guard<std::mutex> lk(m_);
        busy_--;
      }
      done_.notify_one();
    }
  }
  std::mutex run_mutex_, m_;
  std::condition_variable cv_, done_;
  const std::function<void(int)>* fn_ = nullptr;
  int n_ = 0, busy_ = 0;
  std::atomic<int> next_{0};
  u64 gen_ = 0;
  bool stop_ = false;
  std::vector<std::thread> workers_;
};

// Gradient colour table (after the colour transform).
void gradient_lut(const swf::FillStyle& f, const swf::CxForm& cx, Rgba* lut) {
  for (int i = 0; i < 256; i++) {
    const auto& st = f.stops;
    Rgba c;
    if (i <= st.front().ratio) c = unpack(st.front().rgba);
    else if (i >= st.back().ratio) c = unpack(st.back().rgba);
    else {
      size_t k = 1;
      while (k < st.size() && st[k].ratio < i) k++;
      const auto& p = st[k - 1];
      const auto& q = st[k];
      const float t = q.ratio == p.ratio ? 0.0f : float(i - p.ratio) / float(q.ratio - p.ratio);
      const Rgba a = unpack(p.rgba), b = unpack(q.rgba);
      c = Rgba{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
    }
    lut[i] = apply(cx, c);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Display list snapshot (layout of the BaseROM player's objects)
// ---------------------------------------------------------------------------

namespace {
// Display object.
constexpr u32 kObjParent = 0x0c, kObjNext = 0x10, kObjChild = 0x14, kObjChar = 0x18, kObjDepth = 0x1c, kObjRatio = 0x20,
              kObjMatrix = 0x2c, kObjCxForm = 0x44, kObjVisible = 0x6c, kObjClipDepth = 0x6e;
// Character record.
constexpr u32 kCharMovie = 0x04, kCharId = 0x08, kCharType = 0x0a, kCharTag = 0x0b, kCharDef = 0x0c;
// Player context: its view matrix (stage twips -> render units; a, b, c, d
// in 16.16, tx, ty), the render rectangle in those units (x0, x1, y0, y1;
// the units are a fraction of a screen pixel, for sub-pixel precision), and
// the root display object.
constexpr u32 kCtxView = 0x00, kCtxRect = 0xf0, kCtxRoot = 0x2c;
}  // namespace

bool snapshot(const Bus& bus, u32 ctx, Frame& out) {
  out = {};
  std::unordered_set<u32> seen;
  struct Item { u32 obj; s32 parent; };
  std::vector<Item> stack{{ctx + kCtxRoot, -1}};
  while (!stack.empty()) {
    const Item it = stack.back();
    stack.pop_back();
    if (!it.obj || !seen.insert(it.obj).second || out.objects.size() >= 8192) { out.unsupported = true; continue; }
    Object o;
    o.id = it.obj;
    o.parent = it.parent;
    const u32 ch = bus.peek32(it.obj + kObjChar);
    if (ch) {
      o.type = bus.peek8(ch + kCharType);
      o.tag = bus.peek8(ch + kCharTag);
      o.char_id = bus.peek16(ch + kCharId);
      o.def = bus.peek32(ch + kCharDef);
      if (const u32 mv = bus.peek32(ch + kCharMovie)) o.movie = bus.peek32(mv);
    } else {
      o.type = Frame::kTypeRoot;
    }
    o.depth = bus.peek32(it.obj + kObjDepth);
    o.clip_depth = bus.peek16(it.obj + kObjClipDepth);
    o.ratio = bus.peek16(it.obj + kObjRatio);
    o.visible = bus.peek8(it.obj + kObjVisible) != 0;
    const u32 m = it.obj + kObjMatrix;
    o.local = swf::Matrix{s32(bus.peek32(m)) / 65536.0, s32(bus.peek32(m + 4)) / 65536.0, s32(bus.peek32(m + 8)) / 65536.0,
                          s32(bus.peek32(m + 12)) / 65536.0, double(s32(bus.peek32(m + 16))), double(s32(bus.peek32(m + 20)))};
    const u32 x = it.obj + kObjCxForm;
    if (bus.peek32(x)) {
      // Four (multiplier 8.8, offset) pairs of s16, in the order A, R, G, B.
      static constexpr int kChannel[4] = {3, 0, 1, 2};
      for (int i = 0; i < 4; i++) {
        o.local_cx.mul[kChannel[i]] = float(s16(bus.peek16(x + 4 + 4 * u32(i)))) / 256.0f;
        o.local_cx.add[kChannel[i]] = float(s16(bus.peek16(x + 6 + 4 * u32(i))));
      }
    }
    const s32 index = s32(out.objects.size());
    out.objects.push_back(o);
    // Children, pushed in reverse so they are visited in list (depth) order.
    std::vector<u32> kids;
    for (u32 c = bus.peek32(it.obj + kObjChild); c && kids.size() < 4096; c = bus.peek32(c + kObjNext)) kids.push_back(c);
    for (auto k = kids.rbegin(); k != kids.rend(); ++k) stack.push_back({*k, index});
  }
  out.background = 0;  // from the movie (see Renderer::render)
  // View: stage twips -> screen pixels (the render rectangle maps to the
  // 160x160 screen).
  const u32 v = ctx + kCtxView;
  const swf::Matrix pm{s32(bus.peek32(v)) / 65536.0, s32(bus.peek32(v + 4)) / 65536.0, s32(bus.peek32(v + 8)) / 65536.0,
                       s32(bus.peek32(v + 12)) / 65536.0, double(s32(bus.peek32(v + 16))), double(s32(bus.peek32(v + 20)))};
  const double x0 = s32(bus.peek32(ctx + kCtxRect)), x1 = s32(bus.peek32(ctx + kCtxRect + 4));
  const double y0 = s32(bus.peek32(ctx + kCtxRect + 8)), y1 = s32(bus.peek32(ctx + kCtxRect + 12));
  if (x1 > x0 && y1 > y0 && std::abs(pm.a * pm.d - pm.b * pm.c) > 1e-9) {
    const double sx = 160.0 / (x1 - x0), sy = 160.0 / (y1 - y0);
    out.view = swf::Matrix{sx, 0, 0, sy, -x0 * sx, -y0 * sy} * pm;
  }
  return !out.objects.empty();
}

struct Renderer::Ctx {
  const Frame* b = nullptr;
  swf::Matrix view;  // stage -> screen pixels at this moment (b's, or gliding towards a's)
  int scale = 1, w = 0;
  std::vector<u32>* out = nullptr;
  const std::vector<swf::Matrix>* world = nullptr;
  const std::vector<swf::CxForm>* cx = nullptr;
  const std::vector<std::vector<int>>* children = nullptr;
  Coverage* cov = nullptr;
  int band0 = 0, band1 = 0;                // rows drawn by this context
  // Masks, one per nesting level (stable addresses), covering the band's
  // rows only: row y of a mask is at (y - band0) * w.
  std::deque<std::vector<float>>* masks = nullptr;
  int mask_level = 0;
  bool ok = true;
  std::string issue;
};

// ---------------------------------------------------------------------------
// Definition caches. The create-on-demand lookups run single-threaded in
// prepare(); drawing (possibly on several threads) only reads the caches.
// ---------------------------------------------------------------------------

const swf::Movie* Renderer::movie(u32 addr) {
  auto& m = movies_[addr];
  if (!m) {
    m = std::make_unique<swf::Movie>();
    if (const u8* p = rom_.ptr(addr)) m->load(p, rom_.avail(addr));
  }
  return m->data ? m.get() : nullptr;
}

const swf::Shape* Renderer::shape(const Object& o) {
  auto& s = shapes_[o.def];
  if (!s) {
    s = std::make_unique<swf::Shape>();
    const u8* p = rom_.ptr(o.def);
    const int version = o.tag == 2 ? 1 : o.tag == 22 ? 2 : o.tag == 32 ? 3 : 0;
    if (!p || !version || !swf::parse_shape_with_style(p, rom_.avail(o.def), version, *s)) s->edges.clear(), s->fills.clear();
  }
  return s->edges.empty() ? nullptr : s.get();
}

const swf::Bitmap* Renderer::bitmap(u32 movie_addr, u16 id) {
  auto& bm = bitmaps_[{movie_addr, id}];
  if (!bm) {
    bm = std::make_unique<swf::Bitmap>();
    if (const swf::Movie* mv = movie(movie_addr)) {
      if (const swf::TagRef* t = mv->find(id)) {
        if (t->code == 20 || t->code == 36) swf::decode_lossless(t->body, t->len, t->code == 36 ? 2 : 1, *bm);
        else if (t->code == 6 || t->code == 21 || t->code == 35) swf::decode_jpeg_bits(t->body, t->len, t->code, *mv, *bm);
      }
    }
  }
  return bm->px.empty() ? nullptr : bm.get();
}

const swf::Text* Renderer::text(const Object& o) {
  auto& t = texts_[{o.movie, o.char_id}];
  if (!t) {
    auto parsed = std::make_unique<swf::Text>();
    if (const swf::Movie* mv = movie(o.movie))
      if (const swf::TagRef* tag = mv->find(o.char_id))
        if (swf::parse_text(*tag, *parsed)) t = std::move(parsed);  // may have no runs (empty text)
    if (!t) t = std::make_unique<swf::Text>(), t->runs.push_back({}), t->runs.back().font = 0xffff;  // marks "unparsable"
  }
  return !t->runs.empty() && t->runs.front().font == 0xffff && t->runs.front().glyphs.empty() ? nullptr : t.get();
}

const swf::MorphShape* Renderer::morph(const Object& o) {
  auto& m = morphs_[{o.movie, o.char_id}];
  if (!m) {
    m = std::make_unique<swf::MorphShape>();
    if (const swf::Movie* mv = movie(o.movie))
      if (const swf::TagRef* tag = mv->find(o.char_id))
        if (!swf::parse_morph(*tag, *m)) m->start.edges.clear();
  }
  return m->start.edges.empty() ? nullptr : m.get();
}

const swf::Font* Renderer::font(u32 movie_addr, u16 id) {
  auto& f = fonts_[{movie_addr, id}];
  if (!f) {
    f = std::make_unique<swf::Font>();
    if (const swf::Movie* mv = movie(movie_addr))
      if (const swf::TagRef* tag = mv->find(id)) swf::parse_font(*tag, *f);
  }
  return f->glyphs.empty() ? nullptr : f.get();
}

template <class Map, class Key>
static auto cached(const Map& m, const Key& k) -> decltype(m.begin()->second.get()) {
  const auto it = m.find(k);
  return it == m.end() ? nullptr : it->second.get();
}

template <class Map, class Key>
static auto cached_ptr(const Map& m, const Key& k) -> const typename Map::mapped_type* {
  const auto it = m.find(k);
  return it == m.end() ? nullptr : &it->second;
}

void Renderer::prepare(const Frame& b) {
  for (const Object& o : b.objects) {
    if (o.type == Frame::kTypeShape) {
      if (const swf::Shape* s = shape(o))
        for (const auto& f : s->fills)
          if (f.type == swf::FillStyle::kBitmap) bitmap(o.movie, f.bitmap_id);
    } else if (o.type == Frame::kTypeText) {
      if (const swf::Text* t = text(o))
        for (const auto& run : t->runs) font(o.movie, run.font);
    } else if (o.type == Frame::kTypeMorph) {
      if (const swf::MorphShape* ms = morph(o))
        for (const auto& f : ms->fill_start)
          if (f.type == swf::FillStyle::kBitmap) bitmap(o.movie, f.bitmap_id);
    } else if (o.type == Frame::kTypeEditText) {
      const auto it = b.fields.find(o.id);
      if (it == b.fields.end() || it->second.outlines.empty()) continue;
      if (!field_rgba_.count({o.movie, o.char_id})) {
        u32 rgba = 0x000000ff;  // (black without a colour of its own)
        if (const swf::Movie* mv = movie(o.movie))
          if (const swf::TagRef* tag = mv->find(o.char_id)) swf::edit_text_color(*tag, &rgba);
        field_rgba_[{o.movie, o.char_id}] = rgba;
      }
      for (const Frame::Outline& g : it->second.outlines) {
        auto& s = glyphs_[g.shape];
        if (s) continue;
        s = std::make_unique<swf::Shape>();
        const u8* p = rom_.ptr(g.shape);
        if (!p || !swf::parse_glyph(p, rom_.avail(g.shape), *s)) s->edges.clear();
      }
    }
  }
}

void Renderer::fail(Ctx& c, const char* what, const Object* o, u32 detail) {
  if (c.ok) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s (type %u tag %u id %u def %08x, %u)", what, o ? o->type : 0, o ? o->tag : 0, o ? o->char_id : 0,
                  o ? o->def : 0, detail);
    c.issue = buf;
  }
  c.ok = false;
}

// ---------------------------------------------------------------------------
// Frame rendering
// ---------------------------------------------------------------------------

void Renderer::find_carriers(const Frame& b, const Frame& a, const std::unordered_map<u64, int>& in_a, const std::vector<u64>& kb,
                             const std::vector<char>& glides) {
  const size_t n = b.objects.size();
  struct Part { size_t i; const swf::Shape* s; double area; swf::Matrix from; };
  std::unordered_map<s32, std::vector<Part>> groups;
  for (size_t i = 0; i < n; i++) {
    const Object& o = b.objects[i];
    if (!glides[i] || o.parent < 0 || o.type != Frame::kTypeShape) continue;
    const swf::Shape* sh = shape(o);
    if (!sh || sh->x0 >= sh->x1 || sh->y0 >= sh->y1) continue;
    const double area = double(sh->x1 - sh->x0) * (sh->y1 - sh->y0) * std::abs(o.local.a * o.local.d - o.local.b * o.local.c);
    const auto it = in_a.find(kb[i]);
    groups[o.parent].push_back({i, sh, area, a.objects[size_t(it->second)].local});
  }
  auto inside = [](const swf::Shape& sh, double x, double y) {
    const double mx = 0.1 * (sh.x1 - sh.x0), my = 0.1 * (sh.y1 - sh.y0);
    return x >= sh.x0 - mx && x <= sh.x1 + mx && y >= sh.y0 - my && y <= sh.y1 + my;
  };
  for (auto& [parent, g] : groups) {
    std::stable_sort(g.begin(), g.end(), [](const Part& x, const Part& y) { return x.area > y.area; });
    for (size_t jj = 1; jj < g.size(); jj++) {
      const Part& pj = g[jj];
      const Object& oj = b.objects[pj.i];
      int best = -1;
      double best_score = 1e30;
      Carrier bestc;
      auto invertible = [](const swf::Matrix& m) { return std::abs(m.a * m.d - m.b * m.c) > 1e-9; };
      if (!invertible(oj.local) || !invertible(pj.from)) continue;
      for (size_t ii = 0; ii < jj; ii++) {
        const Part& pi = g[ii];
        const Object& oi = b.objects[pi.i];
        if (!invertible(oi.local) || !invertible(pi.from)) continue;
        // j relative to i in this frame and in the other; A: the change.
        const swf::Matrix inv_i = oi.local.inverse();
        const swf::Matrix rb = inv_i * oj.local, ra = pi.from.inverse() * pj.from;
        const swf::Matrix A = ra * rb.inverse();
        double score;
        Carrier cand{int(pi.i), rb, ra};
        const double lin = std::abs(A.a - 1) + std::abs(A.b) + std::abs(A.c) + std::abs(A.d - 1);
        double jx, jy;  // centre of j, in i's space
        rb.apply((pj.s->x0 + pj.s->x1) / 2, (pj.s->y0 + pj.s->y1) / 2, jx, jy);
        if (lin < 2e-3) {
          // Keeps its relation (a layer of the same limb), if it moves
          // with i and they touch.
          double mx, my;
          A.apply(jx, jy, mx, my);
          if (std::hypot(mx - jx, my - jy) * std::sqrt(std::abs(oi.local.a * oi.local.d - oi.local.b * oi.local.c)) > 10 ||
              !inside(*pi.s, jx, jy))
            continue;
          score = 0;
        } else {
          const double l00 = 1 - A.a, l01 = -A.c, l10 = -A.b, l11 = 1 - A.d, det = l00 * l11 - l01 * l10;
          if (std::abs(det) < 1e-6) continue;  // sliding, not turning
          const double qx = (l11 * A.tx - l01 * A.ty) / det, qy = (-l10 * A.tx + l00 * A.ty) / det;
          if (!inside(*pi.s, qx, qy)) continue;
          double px, py;  // the joint in j's space
          rb.inverse().apply(qx, qy, px, py);
          if (!inside(*pj.s, px, py)) continue;
          const double turn = std::abs(std::atan2(A.b, A.a));
          if (turn > 1.75) continue;  // over 100 degrees: not a joint
          score = 1 + turn;
          cand.joint = true;
          cand.qx = qx;
          cand.qy = qy;
          cand.px = px;
          cand.py = py;
        }
        if (score < best_score) { best_score = score; best = int(pi.i); bestc = cand; }
      }
      if (best >= 0) carriers_[pj.i] = bestc;
    }
  }
  // Placement order: every carrier before the parts it carries (each group
  // is sorted largest first, and carriers are only chosen among larger parts).
  order_.clear();
  std::vector<char> placed(n, 0);
  std::function<void(size_t)> place = [&](size_t i) {
    if (placed[i]) return;
    placed[i] = 1;
    if (carriers_[i].by >= 0) place(size_t(carriers_[i].by));
    order_.push_back(i);
  };
  for (size_t i = 0; i < n; i++) place(i);
}

bool Renderer::render(const Frame& next, const Frame* prev, double t, int scale, std::vector<u32>& out, const Frame* before) {
  issue_.clear();
  // Each frame's drawings are shown for their half of the transition: before
  // the midpoint, the previous frame's display list moving towards the next
  // one; from the midpoint, the next frame's arriving from the previous one.
  // Objects in both glide continuously across the switch; drawings that
  // change (a blink, a mouth or hand pose) or that appear or vanish change at
  // the midpoint, the closest match to when the frames actually change.
  const bool from_prev = prev && t < 0.5;
  const Frame& b = from_prev ? *prev : next;       // drawn frame
  const Frame* a = from_prev ? &next : prev;       // the frame it is moving from / to
  const double w = from_prev ? t : 1.0 - t;        // weight of `a`'s transforms
  const int W = kScreen * scale;
  u32 bg = b.background;
  if (!bg)
    for (const Object& o : b.objects)
      if (o.movie)
        if (const swf::Movie* mv = movie(o.movie)) { bg = mv->background; break; }
  out.assign(size_t(W) * W, 0xff000000u | (bg >> 8));
  prepare(b);
  // The view (the player's camera: zooms and pans) moves smoothly too, unless
  // it jumps (a cut: a new scene or movie).
  swf::Matrix view_t = b.view;
  if (a && w > 0.0) {
    const swf::Matrix& v0 = b.view;
    const swf::Matrix& v1 = a->view;
    const double d0 = v0.a * v0.d - v0.b * v0.c, d1 = v1.a * v1.d - v1.b * v1.c;
    const double ratio = d0 != 0 ? std::sqrt(std::abs(d1 / d0)) : 0;
    const double turn = std::abs(std::remainder(std::atan2(v1.b, v1.a) - std::atan2(v0.b, v0.a), 6.28318530717959));
    if (d0 * d1 > 0 && ratio > 0.5 && ratio < 2 && turn < 1.05 && std::abs(v1.tx - v0.tx) < 64 && std::abs(v1.ty - v0.ty) < 64)
      view_t = swf::Matrix::lerp(v0, v1, w);
  }

  // World transforms, each object's own transform interpolated from its state
  // in `a`. Objects are matched between frames by their timeline slot: the
  // path of depths from the root (level, clip, part), so a clip the player
  // re-creates keeps gliding and its children stay in step with it.
  auto slot_keys = [](const Frame& f) {
    std::vector<u64> k(f.objects.size());
    for (size_t i = 0; i < f.objects.size(); i++) {
      const Object& o = f.objects[i];
      const u64 parent = o.parent >= 0 ? k[size_t(o.parent)] : 0x9e3779b97f4a7c15ull;
      k[i] = (parent ^ (u64(o.depth) + 0x9e3779b97f4a7c15ull + (parent << 6) + (parent >> 2))) * 0xff51afd7ed558ccdull;
    }
    return k;
  };
  auto instance_keys = [](const Frame& f) {
    std::vector<u64> k(f.objects.size());
    for (size_t i = 0; i < f.objects.size(); i++) {
      const Object& o = f.objects[i];
      const u64 parent = o.parent >= 0 ? f.objects[size_t(o.parent)].id : 0;
      k[i] = (u64(o.id) << 32) ^ (parent * 0x9e3779b97f4a7c15ull) ^ o.depth;
    }
    return k;
  };
  std::unordered_map<u64, int> in_a;
  if (a) {
    const auto ka = match == Match::Slot ? slot_keys(*a) : instance_keys(*a);
    for (size_t i = 0; i < ka.size(); i++) in_a.emplace(ka[i], int(i));
  }
  const auto kb = match == Match::Slot ? slot_keys(b) : instance_keys(b);
  const size_t n = b.objects.size();
  // The frame before the previous one: a part that keeps moving as it already
  // was (a thrown ball) is motion, whatever its size. Without it (or for a part
  // that was not there), nothing is suspected.
  std::unordered_map<u64, int> in_before;
  if (a && prev && before && reuse_check) {
    const auto k0 = match == Match::Slot ? slot_keys(*before) : instance_keys(*before);
    for (size_t i = 0; i < k0.size(); i++) in_before.emplace(k0[i], int(i));
  }
  // Each object's own transform for this moment. Only the same drawing in the
  // same slot glides; a jump of more than a few tiles is a cut, not motion.
  std::vector<swf::Matrix> local_t(n);
  std::vector<swf::CxForm> cx_t(n);
  std::vector<double> ratio_t(n);
  std::vector<char> glides(n, 0), suspect(n, 0), dropped(n, 0), posed(n, 0);
  std::vector<std::vector<int>> children(n);
  std::vector<int> roots;
  for (size_t i = 0; i < n; i++) {
    const Object& o = b.objects[i];
    local_t[i] = o.local;
    cx_t[i] = o.local_cx;
    ratio_t[i] = o.ratio / 65535.0;
    if (o.parent >= 0) children[size_t(o.parent)].push_back(int(i));
    else roots.push_back(int(i));
    if (!a || w <= 0.0) continue;
    const auto it = in_a.find(kb[i]);
    if (it == in_a.end()) continue;
    const Object& p = a->objects[size_t(it->second)];
    // Likewise a turn of more than 60 degrees, or a flip, between two frames
    // is a redraw (a drawing reused for another limb), not a turn.
    auto turn = [](const swf::Matrix& x, const swf::Matrix& y) {
      const double d = std::atan2(y.b, y.a) - std::atan2(x.b, x.a);
      return std::abs(std::remainder(d, 6.28318530717959));
    };
    const double det_o = o.local.a * o.local.d - o.local.b * o.local.c, det_p = p.local.a * p.local.d - p.local.b * p.local.c;
    // A part glides only inside a clip that itself matched (the same clip,
    // possibly re-created): when the timeline replaces a container, what is
    // at the same depths inside it is new content, not motion.
    const bool parent_ok = o.parent < 0 || glides[size_t(o.parent)];
    if (parent_ok && p.def == o.def && p.type == o.type && p.movie == o.movie && std::abs(p.local.tx - o.local.tx) < 64 * 20 &&
        std::abs(p.local.ty - o.local.ty) < 64 * 20 && det_o * det_p >= 0 && turn(o.local, p.local) < 1.05) {
      local_t[i] = swf::Matrix::lerp(o.local, p.local, w);
      cx_t[i] = swf::CxForm::lerp(o.local_cx, p.local_cx, w);
      ratio_t[i] = (o.ratio + (double(p.ratio) - o.ratio) * w) / 65535.0;  // shape tweens morph smoothly
      glides[i] = 1;
      // A small part that moves much further than its own size between two
      // frames is usually a drawing the animator reused for another part (a
      // pupil becoming a drop of spit): frame-by-frame animation re-places
      // drawings, and the movie stores that exactly like motion. It glides
      // only if a sibling carries it (a hand on its forearm).
      if (reuse_check && o.type == Frame::kTypeShape)
        if (const swf::Shape* sh = shape(o); sh && sh->x0 < sh->x1) {
          double bx0 = 1e30, by0 = 1e30, bx1 = -1e30, by1 = -1e30;
          for (int c = 0; c < 4; c++) {
            double X, Y;
            o.local.apply((c & 1) ? sh->x1 : sh->x0, (c & 2) ? sh->y1 : sh->y0, X, Y);
            bx0 = std::min(bx0, X); bx1 = std::max(bx1, X); by0 = std::min(by0, Y); by1 = std::max(by1, Y);
          }
          const double size = std::hypot(bx1 - bx0, by1 - by0);
          double ax, ay, qx, qy;
          const double mx = (sh->x0 + sh->x1) / 2.0, my = (sh->y0 + sh->y1) / 2.0;
          o.local.apply(mx, my, qx, qy);
          p.local.apply(mx, my, ax, ay);
          const auto it0 = in_before.find(kb[i]);
          if (std::hypot(qx - ax, qy - ay) > std::max(1.5 * size, 4.0 * 20) && it0 != in_before.end() &&
              before->objects[size_t(it0->second)].def == o.def) {
            suspect[i] = 1;  // (only with its history: without it, motion is the better guess)
            {
              // Positions in time order: before, previous frame, next frame.
              double x0, y0;
              before->objects[size_t(it0->second)].local.apply(mx, my, x0, y0);
              const double x1 = from_prev ? qx : ax, y1 = from_prev ? qy : ay, x2 = from_prev ? ax : qx, y2 = from_prev ? ay : qy;
              const double ex = x1 - x0, ey = y1 - y0, dx = x2 - x1, dy = y2 - y1;
              const double e = std::hypot(ex, ey), d = std::hypot(dx, dy);
              if (e > 0.5 * 20 && std::hypot(dx - ex, dy - ey) < 0.5 * std::max(d, e) + 0.5 * 20) suspect[i] = 0;
            }
          }
        }
    }
  }
  // Characters are often flat sets of sibling parts (upper arm, forearm,
  // hand) that each turn about the joints between them. Interpolated
  // independently, joints come apart mid-way. So the skeleton is inferred
  // from the two frames: a gliding part is carried by a larger gliding sibling
  // it keeps still relative to (a layer of the same limb), or else one it
  // turns about a joint with (a point of it that stays in place relative to
  // the sibling, inside both). Its motion relative to the carrier is then
  // interpolated as a turn about that joint, so the joint holds throughout.
  // Larger parts are placed first, so carriers never form a loop.
  if (a && w > 0.0 && skeleton) {
    // The carriers depend only on the two frames: found once per pair.
    u64 key = 0xcbf29ce484222325ull;
    auto mix = [&key](double v) {
      u64 bits;
      std::memcpy(&bits, &v, sizeof bits);
      key = (key ^ bits) * 0x100000001b3ull;
    };
    // Include every input to matching, glide eligibility and carrier geometry.
    // In particular c/d-only scale or shear changes used to reuse old rb/ra.
    mix(double(match == Match::Instance));
    for (const Frame* f : {&b, a}) {
      mix(double(f->objects.size()));
      for (const Object& o : f->objects) {
        mix(o.local.a); mix(o.local.b); mix(o.local.c); mix(o.local.d); mix(o.local.tx); mix(o.local.ty);
        mix(double(o.id)); mix(double(o.parent)); mix(double(o.depth)); mix(double(o.def)); mix(double(o.type));
      }
    }
    if (key != carriers_key_ || carriers_.size() != n || order_.size() != n) {
      carriers_key_ = key;
      carriers_.assign(n, Carrier{});
      find_carriers(b, *a, in_a, kb, glides);
    }
    for (size_t i = 0; i < n; i++)  // (carriers come first in placement order)
      if (suspect[order_[i]]) {
        const int by = carriers_[order_[i]].by;
        dropped[order_[i]] = by < 0 || dropped[size_t(by)];
      }
    for (size_t i = 0; i < n; i++)  // in placement order: carriers first
      if (const Carrier& c = carriers_[order_[i]]; c.by >= 0 && !dropped[size_t(c.by)]) {
        // The relative transform's shape (turn, scale, shear) is interpolated;
        // its position then puts the joint exactly where the carrier has it,
        // whatever the scale or shear (the arc alone keeps it only for turns).
        swf::Matrix rel = swf::Matrix::lerp(c.rb, c.ra, w);
        if (c.joint) {
          double x, y;
          rel.tx = rel.ty = 0;
          rel.apply(c.px, c.py, x, y);
          rel.tx = c.qx - x;
          rel.ty = c.qy - y;
        }
        local_t[order_[i]] = local_t[size_t(c.by)] * rel;
      }
  }
  // Suspected reuses that nothing carries change at the midpoint.
  if (a && w > 0.0)
    for (size_t i = 0; i < n; i++)
      if (skeleton ? dropped[i] : suspect[i]) {
        const Object& o = b.objects[i];
        glides[i] = 0;
        local_t[i] = o.local;
        cx_t[i] = o.local_cx;
        ratio_t[i] = o.ratio / 65535.0;
      }
  // Pose changes. Frame-by-frame characters are loose parts, and a new pose
  // swaps many of their drawings while others move a little. Drawings swap at
  // the midpoint while the rest glide, so in between, one pose's eyes would sit
  // on the other's arm. The parts that change in this transition (moving,
  // swapped or new) are grouped by touching, within each clip; a group whose
  // area is mostly swapped drawings changes whole at the midpoint. A blink or
  // a mouth is a small part of its character, which keeps gliding.
  if (a && w > 0.0 && pose_switch) {
    struct Box { double x0, y0, x1, y1; };
    std::vector<Box> box(n);
    std::vector<char> active(n, 0), moving(n, 0);
    std::vector<double> area(n, 0.0);
    for (size_t i = 0; i < n; i++) {
      const Object& o = b.objects[i];
      if (o.parent < 0 || !o.visible || (o.type != Frame::kTypeShape && o.type != Frame::kTypeText && o.type != Frame::kTypeMorph)) continue;
      const swf::Shape* sh = o.type == Frame::kTypeShape ? shape(o) : nullptr;
      if (o.type == Frame::kTypeShape && (!sh || sh->x0 >= sh->x1)) continue;
      if (glides[i]) {
        const Object& p = a->objects[size_t(in_a.at(kb[i]))];
        const double dl = std::abs(p.local.a - o.local.a) + std::abs(p.local.b - o.local.b) + std::abs(p.local.c - o.local.c) +
                          std::abs(p.local.d - o.local.d);
        if (std::hypot(p.local.tx - o.local.tx, p.local.ty - o.local.ty) < 5 && dl < 1e-3) continue;  // still
        moving[i] = 1;
      }
      active[i] = 1;
      Box& bx = box[i];
      if (!sh) { bx = {o.local.tx, o.local.ty, o.local.tx, o.local.ty}; continue; }
      bx = {1e30, 1e30, -1e30, -1e30};
      for (int c = 0; c < 4; c++) {
        double X, Y;
        o.local.apply((c & 1) ? sh->x1 : sh->x0, (c & 2) ? sh->y1 : sh->y0, X, Y);
        bx.x0 = std::min(bx.x0, X); bx.x1 = std::max(bx.x1, X); bx.y0 = std::min(bx.y0, Y); bx.y1 = std::max(bx.y1, Y);
      }
      area[i] = (bx.x1 - bx.x0) * (bx.y1 - bx.y0);
    }
    std::vector<size_t> up(n);
    for (size_t i = 0; i < n; i++) up[i] = i;
    std::function<size_t(size_t)> root = [&](size_t i) { return up[i] == i ? i : up[i] = root(up[i]); };
    constexpr double kTouch = 40;  // (2 px of slack, in twips)
    for (size_t pi = 0; pi < n; pi++) {
      const auto& kids = children[pi];
      for (size_t x = 0; x < kids.size(); x++) {
        const size_t i = size_t(kids[x]);
        if (!active[i]) continue;
        for (size_t y = x + 1; y < kids.size(); y++) {
          const size_t j = size_t(kids[y]);
          if (!active[j]) continue;
          if (box[i].x0 <= box[j].x1 + kTouch && box[j].x0 <= box[i].x1 + kTouch && box[i].y0 <= box[j].y1 + kTouch &&
              box[j].y0 <= box[i].y1 + kTouch)
            up[root(i)] = root(j);
        }
      }
    }
    struct Group { double swapped = 0, moved = 0; std::vector<double> dx, dy; };
    std::unordered_map<size_t, Group> groups;
    for (size_t i = 0; i < n; i++) {
      if (!active[i]) continue;
      Group& g = groups[root(i)];
      if (!moving[i]) { g.swapped += area[i]; continue; }
      g.moved += area[i];
      const Object& p = a->objects[size_t(in_a.at(kb[i]))];
      g.dx.push_back(p.local.tx - b.objects[i].local.tx);
      g.dy.push_back(p.local.ty - b.objects[i].local.ty);
    }
    // A pose change keeps the movement its parts share (the median step: a
    // character swinging across the screen while its drawings change still
    // travels smoothly), and swapped drawings travel with it. A part that also
    // moves a long way of its own (Mr. Krabs' claw going from his head to
    // his side) changes at the midpoint with the drawings, keeping only the
    // shared movement; a small movement of its own still glides.
    for (auto& [r, g] : groups) {
      if (g.swapped < 0.3 * (g.swapped + g.moved) || g.dx.empty()) { g.dx.clear(); continue; }
      for (auto* v : {&g.dx, &g.dy}) {
        std::nth_element(v->begin(), v->begin() + v->size() / 2, v->end());
        const double m = (*v)[v->size() / 2];
        v->assign(1, m);
      }
    }
    for (size_t i = 0; i < n; i++) {
      if (!active[i]) continue;
      const auto it = groups.find(root(i));
      if (it == groups.end() || it->second.dx.empty()) continue;
      const Object& o = b.objects[i];
      if (moving[i]) {  // its own movement on top: a small one glides (a leg in a walk)
        const Object& p = a->objects[size_t(in_a.at(kb[i]))];
        const double rx = p.local.tx - o.local.tx - it->second.dx[0], ry = p.local.ty - o.local.ty - it->second.dy[0];
        const double size = std::hypot(box[i].x1 - box[i].x0, box[i].y1 - box[i].y0);
        if (std::hypot(rx, ry) <= std::max(0.5 * size, 4.0 * 20)) continue;
      }
      local_t[i] = o.local;
      local_t[i].tx += w * it->second.dx[0];
      local_t[i].ty += w * it->second.dy[0];
      if (moving[i]) {
        cx_t[i] = o.local_cx;
        ratio_t[i] = o.ratio / 65535.0;
      }
      glides[i] = 0;
      posed[i] = 1;
    }
  }
  // A part that appears, or that the timeline swaps in (a new eye or mouth
  // pose), has no earlier placement. Characters are often built of many
  // sibling parts that each move, so it moves with the gliding sibling it sits
  // on (the face under the eyes): it takes that sibling's in-between offset,
  // in their parent's space. Without one underneath, the nearest gliding
  // sibling close by; otherwise it keeps its place.
  if (a && w > 0.0) {
    auto box = [&](size_t i, double& x0, double& y0, double& x1, double& y1) {
      const Object& o = b.objects[i];
      const swf::Shape* sh = o.type == Frame::kTypeShape ? shape(o) : nullptr;
      if (!sh || sh->x0 > sh->x1) {  // no outline: its origin
        x0 = x1 = o.local.tx;
        y0 = y1 = o.local.ty;
        return;
      }
      x0 = y0 = 1e30;
      x1 = y1 = -1e30;
      for (int c = 0; c < 4; c++) {
        double X, Y;
        o.local.apply((c & 1) ? sh->x1 : sh->x0, (c & 2) ? sh->y1 : sh->y0, X, Y);
        x0 = std::min(x0, X); x1 = std::max(x1, X);
        y0 = std::min(y0, Y); y1 = std::max(y1, Y);
      }
    };
    for (size_t i = 0; i < n; i++) {
      const Object& o = b.objects[i];
      if (glides[i] || posed[i] || o.parent < 0) continue;
      double ix0, iy0, ix1, iy1;
      box(i, ix0, iy0, ix1, iy1);
      const double cx = (ix0 + ix1) / 2, cy = (iy0 + iy1) / 2;
      int best = -1;
      double best_score = 1e30;
      for (int s : children[size_t(o.parent)]) {
        if (!glides[size_t(s)]) continue;
        double sx0, sy0, sx1, sy1;
        box(size_t(s), sx0, sy0, sx1, sy1);
        const bool under = cx >= sx0 && cx <= sx1 && cy >= sy0 && cy <= sy1 && sx1 > sx0;
        const double dist = std::hypot(cx - (sx0 + sx1) / 2, cy - (sy0 + sy1) / 2);
        const double depth_gap = std::abs(double(b.objects[size_t(s)].depth) - double(o.depth));
        const double score = under ? depth_gap : dist < 30 * 20 ? 1e9 + dist : 1e30;
        if (score < best_score) { best_score = score; best = s; }
      }
      if (best >= 0) local_t[i] = local_t[size_t(best)] * b.objects[size_t(best)].local.inverse() * o.local;
    }
  }
  locals_ = local_t;
  std::vector<swf::Matrix> world(n);
  std::vector<swf::CxForm> cx(n);
  for (size_t i = 0; i < n; i++) {
    const Object& o = b.objects[i];
    world[i] = o.parent >= 0 ? world[size_t(o.parent)] * local_t[i] : local_t[i];
    cx[i] = o.parent >= 0 ? cx[size_t(o.parent)] * cx_t[i] : cx_t[i];
  }

  // Morph shapes at this moment's ratio.
  morph_now_.clear();
  morph_now_.resize(n);
  for (size_t i = 0; i < n; i++)
    if (b.objects[i].type == Frame::kTypeMorph)
      if (const swf::MorphShape* ms = morph(b.objects[i])) {
        morph_now_[i] = std::make_unique<swf::Shape>();
        ms->at(std::clamp(ratio_t[i], 0.0, 1.0), *morph_now_[i]);
      }

  // Where the text fields are (to check them against the LCD), and which
  // were drawn without captured text.
  uncaptured_.clear();
  field_rects_.clear();
  for (size_t i = 0; i < n; i++) {
    const Object& o = b.objects[i];
    if (o.type != Frame::kTypeEditText) continue;
    const bool captured = b.fields.count(o.id) != 0;
    s32 r[4];
    const swf::Movie* mv = movie(o.movie);
    const swf::TagRef* tag = mv ? mv->find(o.char_id) : nullptr;
    if (!tag || !swf::edit_text_bounds(*tag, r)) {
      if (!captured) uncaptured_.push_back({0, 0, kScreen, kScreen});
      continue;
    }
    const swf::Matrix m = b.view * world[i];
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int c = 0; c < 4; c++) {
      double X, Y;
      m.apply(r[c & 1], r[2 + (c >> 1)], X, Y);
      x0 = std::min(x0, X); x1 = std::max(x1, X); y0 = std::min(y0, Y); y1 = std::max(y1, Y);
    }
    const int ix0 = std::clamp(int(std::floor(x0)), 0, kScreen), iy0 = std::clamp(int(std::floor(y0)), 0, kScreen);
    const int ix1 = std::clamp(int(std::ceil(x1)), 0, kScreen), iy1 = std::clamp(int(std::ceil(y1)), 0, kScreen);
    if (ix1 <= ix0 || iy1 <= iy0) continue;
    if (captured) field_rects_.push_back({ix0, iy0, ix1, iy1});
    else uncaptured_.push_back({ix0, iy0, ix1, iy1});
  }

  // Horizontal bands, drawn in parallel at larger sizes: a few per thread,
  // handed out as threads become free (the bands' costs differ widely).
  Pool& pool = Pool::get();
  const int band_h = scale <= 1 ? W : std::max(32, (W + pool.threads() * 2 - 1) / (pool.threads() * 2));
  const int bands = (W + band_h - 1) / band_h;
  while (int(bands_.size()) < bands) bands_.emplace_back();
  std::vector<Ctx> ctx;
  ctx.resize(static_cast<size_t>(bands));
  for (int i = 0; i < bands; i++) {
    Ctx& c = ctx[size_t(i)];
    c.b = &b;
    c.view = view_t;
    c.scale = scale;
    c.w = W;
    c.out = &out;
    c.world = &world;
    c.cx = &cx;
    c.children = &children;
    c.band0 = i * band_h;
    c.band1 = std::min(W, c.band0 + band_h);
    Band& band = bands_[size_t(i)];
    c.cov = &band.cov;
    if (c.cov->width() != W || c.cov->height() != W || c.cov->band_begin() != c.band0 || c.cov->band_end() != c.band1)
      c.cov->resize(W, W, c.band0, c.band1);
    c.masks = &band.masks;
  }
  const std::function<void(int)> run = [&](int i) { for (int r : roots) draw_node(ctx[size_t(i)], r, nullptr); };
  if (bands == 1) run(0);
  else pool.run(bands, run);
  bool ok = !b.unsupported;
  for (const Ctx& c : ctx)
    if (!c.ok) { if (ok) issue_ = c.issue; ok = false; }
  return ok;
}

void Renderer::draw_node(Ctx& c, int index, const float* mask) {
  const Object& o = c.b->objects[size_t(index)];
  if (!o.visible) return;
  const swf::Matrix& m = (*c.world)[size_t(index)];
  const swf::CxForm& x = (*c.cx)[size_t(index)];
  switch (o.type) {
    case Frame::kTypeShape:
      if (const swf::Shape* s = cached(shapes_, o.def); s && !s->edges.empty()) draw_shape(c, *s, m, x, o.movie, mask, nullptr);
      else fail(c, "shape did not parse", &o);
      break;
    case Frame::kTypeText:
      if (!draw_text(c, o, m, x, mask)) fail(c, "text", &o);
      break;
    case Frame::kTypeMorph:
      if (const swf::Shape* s = morph_now_[size_t(index)].get()) draw_shape(c, *s, m, x, o.movie, mask, nullptr);
      else fail(c, "morph shape did not parse", &o);
      break;
    case Frame::kTypeEditText:
      if (c.b->fields.count(o.id) && !draw_field(c, o, m, x, mask)) fail(c, "text field glyphs unreadable", &o);
      break;  // a field never drawn is empty (checked against the LCD, see uncaptured_fields())
    case Frame::kTypeSprite: case Frame::kTypeLoaded: case Frame::kTypeLevel: case Frame::kTypeRoot: case 2:  // containers (2: button)
      break;
    default:
      fail(c, "unsupported character type", &o);  // morph shapes, edit text, ...: not drawn yet
      break;
  }
  const auto& kids = (*c.children)[size_t(index)];
  // Clip-depth ranges can overlap or cross: a mask inside an active range
  // is another mask, never visible artwork. Keep each range until its own
  // end depth, and intersect all active masks when drawing a sibling.
  struct ActiveMask { u32 end; int slot; };
  std::vector<ActiveMask> active;
  std::vector<int> free_slots;
  const int base = c.mask_level;
  int slots = 0;  // base is scratch for the intersection; base+1... hold masks
  auto buffer = [&](int slot) -> std::vector<float>& {
    while (slot >= int(c.masks->size())) c.masks->emplace_back();
    auto& v = (*c.masks)[size_t(slot)];
    if (v.size() != size_t(c.band1 - c.band0) * c.w) v.resize(size_t(c.band1 - c.band0) * c.w);
    return v;
  };
  const size_t count = size_t(c.band1 - c.band0) * c.w;
  for (int kid : kids) {
    const Object& ch = c.b->objects[size_t(kid)];
    for (auto it = active.begin(); it != active.end();) {
      if (it->end < ch.depth) { free_slots.push_back(it->slot); it = active.erase(it); }
      else ++it;
    }
    if (!ch.clip_depth) {
      const float* clip = mask;
      if (active.size() == 1 && !mask) clip = (*c.masks)[size_t(active[0].slot)].data();
      else if (!active.empty()) {
        auto& combined = buffer(base);
        for (size_t i = 0; i < count; i++) {
          float v = mask ? mask[i] : 1.0f;
          for (const auto& m : active) v *= (*c.masks)[size_t(m.slot)][i];
          combined[i] = v;
        }
        clip = combined.data();
      }
      c.mask_level = base + (slots ? slots + 1 : 0);
      draw_node(c, kid, clip);
      continue;
    }
    const int slot = free_slots.empty() ? base + (++slots) : free_slots.back();
    if (!free_slots.empty()) free_slots.pop_back();
    auto& mk = buffer(slot);
    std::fill(mk.begin(), mk.end(), 0.0f);
    std::vector<int> stack{kid};
    while (!stack.empty()) {
      const int i = stack.back();
      stack.pop_back();
      const Object& mo = c.b->objects[size_t(i)];
      if (!mo.visible) continue;
      // Internal masks in a mask container need their own coverage traversal.
      // Do not claim a faithful redraw by silently treating them as a union.
      if (i != kid && mo.clip_depth) { fail(c, "nested mask geometry", &mo); continue; }
      if (mo.type == Frame::kTypeShape) {
        if (const swf::Shape* s = cached(shapes_, mo.def); s && !s->edges.empty())
          draw_shape(c, *s, (*c.world)[size_t(i)], (*c.cx)[size_t(i)], mo.movie, nullptr, &mk);
        else fail(c, "mask shape did not parse", &mo);
      } else if (mo.type == Frame::kTypeMorph) {
        if (morph_now_[size_t(i)]) draw_shape(c, *morph_now_[size_t(i)], (*c.world)[size_t(i)], (*c.cx)[size_t(i)], mo.movie, nullptr, &mk);
        else fail(c, "mask morph did not parse", &mo);
      } else if (mo.type != Frame::kTypeSprite && mo.type != Frame::kTypeLoaded && mo.type != Frame::kTypeLevel &&
                 mo.type != Frame::kTypeRoot && mo.type != 2) {
        fail(c, "unsupported mask geometry", &mo);
      }
      for (int child : (*c.children)[size_t(i)]) stack.push_back(child);
    }
    active.push_back({ch.clip_depth, slot});
  }
  c.mask_level = base;
}

// A dynamic text field: the glyphs the text engine drew, following the
// field's current transform (translation), their coverage upscaled smoothly.
bool Renderer::draw_field(Ctx& c, const Object& o, const swf::Matrix& m, const swf::CxForm& cx, const float* mask) {
  const auto it = c.b->fields.find(o.id);
  if (it == c.b->fields.end()) return false;
  const Frame::Field& f = it->second;
  if (!f.outlines.empty()) {  // embedded outline font: shapes, placed in the field's space
    const u32* rgba = cached_ptr(field_rgba_, std::pair<u32, u16>{o.movie, o.char_id});
    for (const Frame::Outline& g : f.outlines) {
      const swf::Shape* s = cached(glyphs_, g.shape);
      if (!s || !rgba) return false;
      if (!s->edges.empty()) draw_shape(c, *s, m * g.m, cx, o.movie, mask, nullptr, rgba);
    }
  }
  const swf::Matrix now = c.view * m;  // stage -> screen pixels, as displayed
  const swf::Matrix back = f.placed.inverse();
  const int W = c.w;
  const double k = c.scale;
  static constexpr float kCoverage[16] = {0, 5, 9, 13, 17, 22, 26, 30, 34, 38, 43, 47, 51, 56, 60, 64};  // of 64
  // Upscaled 4-bit coverage is soft; steepen it around 50% (bilinear
  // interpolation of the coverage then acts like an outline).
  constexpr float kSharpen = 2.0f;
  for (const Frame::Glyph& g : f.glyphs) {
    const u8* bits = rom_.ptr(g.bits);
    const size_t row = (size_t(g.w) + 1) / 2;
    if (!bits || rom_.avail(g.bits) < row * g.h) return false;
    // Where the glyph's origin is now.
    double lx, ly, px, py;
    back.apply(g.x, g.y, lx, ly);
    now.apply(lx, ly, px, py);
    const double dx = px - g.x, dy = py - g.y;
    auto cov = [&](int i, int j) -> float {
      if (i < 0 || j < 0 || i >= g.w || j >= g.h) return 0.0f;
      const u8 b = bits[size_t(j) * row + size_t(i) / 2];
      return kCoverage[(i & 1) ? (b & 15) : (b >> 4)] / 64.0f;
    };
    const double cx0 = (g.clip[0] + dx) * k, cy0 = (g.clip[1] + dy) * k;
    const double cx1 = (g.clip[2] + 1 + dx) * k, cy1 = (g.clip[3] + 1 + dy) * k;
    const int ox0 = std::max({0, int(std::floor(px * k)), int(std::ceil(cx0 - 0.5))});
    const int ox1 = std::min({W, int(std::ceil((px + g.w) * k)), int(std::floor(cx1 - 0.5)) + 1});
    const int oy0 = std::max({c.band0, int(std::floor(py * k)), int(std::ceil(cy0 - 0.5))});
    const int oy1 = std::min({c.band1, int(std::ceil((py + g.h) * k)), int(std::floor(cy1 - 0.5)) + 1});
    const Rgba col{float(g.rgb & 255), float((g.rgb >> 8) & 255), float((g.rgb >> 16) & 255), 255.0f};
    for (int oy = oy0; oy < oy1; oy++) {
      const double v = (oy + 0.5) / k - py - 0.5;
      const int j0 = int(std::floor(v));
      const float fy = float(v - j0);
      u32* orow = &(*c.out)[size_t(oy) * W];
      const float* mrow = mask ? mask + size_t(oy - c.band0) * W : nullptr;
      for (int ox = ox0; ox < ox1; ox++) {
        const double u = (ox + 0.5) / k - px - 0.5;
        const int i0 = int(std::floor(u));
        const float fx = float(u - i0);
        float a = (cov(i0, j0) * (1 - fx) + cov(i0 + 1, j0) * fx) * (1 - fy) + (cov(i0, j0 + 1) * (1 - fx) + cov(i0 + 1, j0 + 1) * fx) * fy;
        if (k > 1) a = std::clamp((a - 0.5f) * kSharpen + 0.5f, 0.0f, 1.0f);  // crisper edges when enlarged
        if (mrow) a *= mrow[ox];
        if (a > 0.0f) blend(orow[ox], col, a);
      }
    }
  }
  return true;
}

bool Renderer::draw_text(Ctx& c, const Object& o, const swf::Matrix& m, const swf::CxForm& cx, const float* mask) {
  const swf::Text* t = cached(texts_, std::pair<u32, u16>{o.movie, o.char_id});
  if (!t || (!t->runs.empty() && t->runs.front().font == 0xffff && t->runs.front().glyphs.empty())) return false;
  bool ok = true;
  for (const swf::Text::Run& run : t->runs) {
    const swf::Font* f = cached(fonts_, std::pair<u32, u16>{o.movie, run.font});
    if (!f || f->glyphs.empty()) { ok = false; continue; }
    const double k = run.height / 1024.0;  // glyphs use a 1024-unit EM square
    for (const swf::Text::Glyph& g : run.glyphs) {
      if (g.index >= f->glyphs.size()) { ok = false; continue; }
      const swf::Matrix gm = m * t->matrix * swf::Matrix{k, 0, 0, k, g.x, g.y};
      draw_shape(c, f->glyphs[g.index], gm, cx, o.movie, mask, nullptr, &run.rgba);
    }
  }
  return ok;
}

void Renderer::draw_shape(Ctx& c, const swf::Shape& s, const swf::Matrix& m, const swf::CxForm& cx, u32 movie_addr,
                          const float* mask, std::vector<float>* mask_out, const u32* fill_rgba) {
  Coverage& cov = *c.cov;
  const swf::Matrix M = swf::Matrix{double(c.scale), 0, 0, double(c.scale), 0, 0} * c.view * m;  // -> output pixels
  const double k = double(c.scale) * std::sqrt(std::abs(c.view.a * c.view.d - c.view.b * c.view.c));  // twips -> output pixels
  auto tf = [&](float x, float y, float& ox, float& oy) {
    double X, Y;
    M.apply(x, y, X, Y);
    ox = float(X);
    oy = float(Y);
  };
  const int W = c.w;

  // Nothing to do in this band if the shape (with its widest stroke) is
  // outside it.
  if (s.x0 <= s.x1) {
    double lo = 1e30, hi = -1e30;
    for (int i = 0; i < 4; i++) {
      double X, Y;
      M.apply((i & 1) ? s.x1 : s.x0, (i & 2) ? s.y1 : s.y0, X, Y);
      lo = std::min(lo, Y);
      hi = std::max(hi, Y);
    }
    double pad = 1;
    if (!mask_out) {
      const double obj = std::sqrt(std::abs(m.a * m.d - m.b * m.c));
      for (const swf::LineStyle& ls : s.lines) pad = std::max(pad, std::max(double(c.scale) * 0.5, ls.width * obj * k * 0.5) + 1);
    }
    if (hi + pad <= c.band0 || lo - pad >= c.band1) return;
  }

  // Fills: each fill's region is bounded by the edges that have it on their
  // right (fill1) or, reversed, on their left (fill0).
  const bool indexed = s.fill_edges.size() == s.fills.size() + 1;
  const bool plain_cx = cx.identity();
  // Style groups in order: each one's fills, then its lines.
  const size_t n_groups = s.groups.empty() ? 1 : s.groups.size();
  for (size_t gi = 0; gi < n_groups; gi++) {
  const size_t f_begin = gi ? s.groups[gi - 1].fills : 0, l_begin = gi ? s.groups[gi - 1].lines : 0;
  const size_t f_end = s.groups.empty() ? s.fills.size() : s.groups[gi].fills;
  const size_t l_end = s.groups.empty() ? s.lines.size() : s.groups[gi].lines;
  for (size_t f = f_begin + 1; f <= f_end; f++) {
    cov.begin();
    const size_t n_edges = indexed ? s.fill_edges[f].size() : s.edges.size();
    for (size_t ei = 0; ei < n_edges; ei++) {
      const swf::Edge& e = s.edges[indexed ? s.fill_edges[f][ei] : ei];
      const bool fwd = e.fill1 == f, rev = e.fill0 == f;
      if (fwd == rev) continue;  // not on this fill's boundary (or on both sides)
      float x0, y0, cx0, cy0, x1, y1;
      tf(e.x0, e.y0, x0, y0);
      tf(e.x1, e.y1, x1, y1);
      if (e.curve) {
        tf(e.cx, e.cy, cx0, cy0);
        if (fwd) cov.quad(x0, y0, cx0, cy0, x1, y1);
        else cov.quad(x1, y1, cx0, cy0, x0, y0);
      } else {
        if (fwd) cov.line(x0, y0, x1, y1);
        else cov.line(x1, y1, x0, y0);
      }
    }
    if (cov.empty()) continue;
    const int xa = cov.col_begin(), xb = cov.col_end();
    const swf::FillStyle& fs = s.fills[f - 1];
    if (mask_out) {
      for (int y = cov.row_begin(); y < cov.row_end(); y++) {
        const float* cv = cov.row(y);
        float* mrow = &(*mask_out)[size_t(y - c.band0) * W];
        for (int x = xa; x < xb; x++) mrow[x] = std::max(mrow[x], cv[x]);
      }
      continue;
    }
    Rgba solid{};
    Rgba lut[256];
    swf::Matrix inv;
    const swf::Bitmap* bm = nullptr;
    if (fs.type == swf::FillStyle::kSolid) {
      solid = apply(cx, unpack(fill_rgba ? *fill_rgba : fs.rgba));
    } else {
      inv = (M * fs.matrix).inverse();
      if (fs.type == swf::FillStyle::kBitmap) {
        bm = cached(bitmaps_, std::pair<u32, u16>{movie_addr, fs.bitmap_id});
        if (!bm || bm->px.empty()) { fail(c, "bitmap missing", nullptr, fs.bitmap_id); continue; }
      } else {
        gradient_lut(fs, cx, lut);
      }
    }
    const u32 opaque = 0xff000000u | (u32(std::lround(solid.r)) << 16) | (u32(std::lround(solid.g)) << 8) | u32(std::lround(solid.b));
    const bool bilinear = bm && fs.smooth && c.scale > 1;  // bilinear when enlarged (the player samples nearest)
    auto texel = [&](int bx, int by) {
      if (fs.repeat) { bx = ((bx % bm->w) + bm->w) % bm->w; by = ((by % bm->h) + bm->h) % bm->h; }
      else { bx = std::clamp(bx, 0, bm->w - 1); by = std::clamp(by, 0, bm->h - 1); }
      return bm->px[size_t(by) * bm->w + size_t(bx)];
    };
    for (int y = cov.row_begin(); y < cov.row_end(); y++) {
      const float* cv = cov.row(y);
      u32* orow = &(*c.out)[size_t(y) * W];
      const float* mrow = mask ? mask + size_t(y - c.band0) * W : nullptr;
      if (fs.type == swf::FillStyle::kSolid) {
        const bool solid_opaque = solid.a >= 255.0f;
        for (int x = xa; x < xb; x++) {
          float a = cv[x];
          if (a <= 0.0f) continue;
          if (mrow) { a *= mrow[x]; if (a <= 0.0f) continue; }
          if (a >= 1.0f && solid_opaque) orow[x] = opaque;
          else blend(orow[x], solid, a);
        }
        continue;
      }
      // Fill-space coordinates of this row's pixel centres: (rx, ry) + x * (inv.a, inv.b).
      const double yc = y + 0.5;
      const double rx = inv.c * yc + inv.tx, ry = inv.d * yc + inv.ty;
      if (fs.type != swf::FillStyle::kBitmap) {
        const bool linear = fs.type == swf::FillStyle::kLinear;
        for (int x = xa; x < xb; x++) {
          float a = cv[x];
          if (a <= 0.0f) continue;
          if (mrow) { a *= mrow[x]; if (a <= 0.0f) continue; }
          const double xc = x + 0.5, gx = inv.a * xc + rx, gy = inv.b * xc + ry;
          const double r = linear ? (gx + 16384.0) / 32768.0 : std::sqrt(gx * gx + gy * gy) / 16384.0;
          blend(orow[x], lut[std::clamp(int(r * 255.0 + 0.5), 0, 255)], a);
        }
        continue;
      }
      const int bw = bm->w, bh = bm->h;
      const u32* bpx = bm->px.data();
      for (int x = xa; x < xb; x++) {
        float a = cv[x];
        if (a <= 0.0f) continue;
        if (mrow) { a *= mrow[x]; if (a <= 0.0f) continue; }
        const double xc = x + 0.5, gx = inv.a * xc + rx, gy = inv.b * xc + ry;
        float pr, pg, pb, pa;  // premultiplied
        if (bilinear) {
          const double fx = gx - 0.5, fy = gy - 0.5;
          const int x0 = floor_int(fx), y0 = floor_int(fy);
          const float wx = float(fx - x0), wy = float(fy - y0);
          u32 q[4];
          if (x0 >= 0 && y0 >= 0 && x0 + 1 < bw && y0 + 1 < bh) {
            const u32* p = bpx + size_t(y0) * size_t(bw) + size_t(x0);
            q[0] = p[0]; q[1] = p[1]; q[2] = p[bw]; q[3] = p[bw + 1];
          } else {
            q[0] = texel(x0, y0); q[1] = texel(x0 + 1, y0); q[2] = texel(x0, y0 + 1); q[3] = texel(x0 + 1, y0 + 1);
          }
          const float w[4] = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy};
          pr = pg = pb = pa = 0;
          for (int i = 0; i < 4; i++) {
            pa += w[i] * float(q[i] >> 24);
            pr += w[i] * float((q[i] >> 16) & 255);
            pg += w[i] * float((q[i] >> 8) & 255);
            pb += w[i] * float(q[i] & 255);
          }
        } else {
          const u32 p = texel(floor_int(gx), floor_int(gy));
          pa = float(p >> 24); pr = float((p >> 16) & 255); pg = float((p >> 8) & 255); pb = float(p & 255);
        }
        if (pa <= 0.5f) continue;
        if (pa == 255.0f && a >= 1.0f && plain_cx) {  // opaque: premultiplied is straight
          orow[x] = 0xff000000u | u32(int(pr + 0.5f)) << 16 | u32(int(pg + 0.5f)) << 8 | u32(int(pb + 0.5f));
          continue;
        }
        const float un = 255.0f / pa;  // un-premultiply
        const Rgba col{pr * un, pg * un, pb * un, pa};
        blend(orow[x], plain_cx ? col : apply(cx, col), a);
      }
    }
  }

  // Lines: round-capped strokes, scaled with the object (at least one
  // screen pixel wide).
  const double obj_scale = std::sqrt(std::abs(m.a * m.d - m.b * m.c));
  for (size_t l = l_begin + 1; !mask_out && l <= l_end; l++) {  // (masks use fills only)
    const swf::LineStyle& ls = s.lines[l - 1];
    const float hw = float(std::max(double(c.scale) * 0.5, ls.width * obj_scale * k * 0.5));
    const int cap_n = std::clamp(int(hw * 2), 6, 24);
    cov.begin();
    auto cap = [&](float px, float py) {
      float lx = px + hw, ly = py;
      for (int i = 1; i <= cap_n; i++) {
        const float ang = -6.2831853f * float(i) / cap_n;  // same orientation as the segment quads
        const float nx = px + hw * std::cos(ang), ny = py + hw * std::sin(ang);
        cov.line(lx, ly, nx, ny);
        lx = nx;
        ly = ny;
      }
    };
    auto seg = [&](float x0, float y0, float x1, float y1) {
      const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
      if (len > 1e-4f) {
        const float nx = -dy / len * hw, ny = dx / len * hw;
        cov.line(x0 + nx, y0 + ny, x1 + nx, y1 + ny);
        cov.line(x1 + nx, y1 + ny, x1 - nx, y1 - ny);
        cov.line(x1 - nx, y1 - ny, x0 - nx, y0 - ny);
        cov.line(x0 - nx, y0 - ny, x0 + nx, y0 + ny);
      }
      cap(x1, y1);
    };
    bool any = false;
    for (const swf::Edge& e : s.edges) {
      if (e.line != l) continue;
      any = true;
      float x0, y0, x1, y1;
      tf(e.x0, e.y0, x0, y0);
      tf(e.x1, e.y1, x1, y1);
      cap(x0, y0);
      if (!e.curve) { seg(x0, y0, x1, y1); continue; }
      float cxp, cyp;
      tf(e.cx, e.cy, cxp, cyp);
      const float ddx = x0 - 2 * cxp + x1, ddy = y0 - 2 * cyp + y1;
      const int nseg = std::clamp(int(std::ceil(std::sqrt(std::sqrt(ddx * ddx + ddy * ddy) / 1.6f))), 1, 32);
      float px = x0, py = y0;
      for (int i = 1; i <= nseg; i++) {
        const float t = float(i) / nseg, u = 1 - t;
        const float qx = u * u * x0 + 2 * u * t * cxp + t * t * x1, qy = u * u * y0 + 2 * u * t * cyp + t * t * y1;
        seg(px, py, qx, qy);
        px = qx;
        py = qy;
      }
    }
    if (!any || cov.empty()) continue;
    const Rgba col = apply(cx, unpack(ls.rgba));
    const int xa = cov.col_begin(), xb = cov.col_end();
    for (int y = cov.row_begin(); y < cov.row_end(); y++) {
      const float* cv = cov.row(y);
      u32* orow = &(*c.out)[size_t(y) * W];
      const float* mrow = mask ? mask + size_t(y - c.band0) * W : nullptr;
      for (int x = xa; x < xb; x++) {
        float a = cv[x];
        if (mrow) a *= mrow[x];
        if (a > 0.0f) blend(orow[x], col, a);
      }
    }
  }
  }  // style groups
}

}  // namespace leap::flash
