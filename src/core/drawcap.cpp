#include "core/drawcap.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/arc/cpu.h"
#include "core/bus.h"
#include "core/codesig.h"
#include "core/log.h"

namespace leap {

namespace {

constexpr int kW = NativeFrame::kW, kH = NativeFrame::kH;

// The routines hooked are found by signatures of their code (core/codesig.h),
// taken from runs free of relocated addresses (see docs/interpolation.md).
//
// Cartridge tile-plane engine: DrawPlane, by its entry sequence, and
// DrawSpriteLayer, by a run 28 bytes past its entry. A second hash covers each
// signature and the instructions at its hook points, so that only the known
// version of the engine is hooked.
constexpr CodeSig kSigDrawPlane{40, 0x166352e6ccc0f4c4}, kSigSpriteLayer{24, 0x178e2a2dce99a289};
constexpr u32 kSpriteLayerSigOffset = 28;
// Inside DrawPlane: after it has streamed newly exposed tiles into the ring
// tilemap (and wrapped the scroll), before drawing. The arguments are in
// r14 (plane), r7 (surface), r15 (mode) and r8 (palettes) by then.
constexpr u32 kPlaneReady = 0xb8;
// Inside DrawSpriteLayer: the loop body once an object passed the priority
// filter (object in r14), and the loop's step to the next object after drawing.
constexpr u32 kSpriteBegin = 0x32, kSpriteEnd = 0xe8;
constexpr u64 kCheckDrawPlane = 0x0525a124ac02f9b8, kCheckSpriteLayer = 0x0e412fed97b61cae;

// BaseROM Flash player: the routine that draws one display object (object in
// r0, its world transform in r1), found by a run 0x30 bytes past its entry.
constexpr CodeSig kSigFlashObject{24, 0x01166ca4cd5e89d6};
constexpr u32 kFlashObjectSigOffset = 0x30;
// BaseROM text engine: drawing a dynamic text field (field instance in r0,
// its display object at +0x18), found 0x20 bytes past its entry; and its
// device-font glyph blitter (surface in r0, glyph bits r1, width r3, height
// r4, x r5, y r6, colour r7), 0x2e bytes past its entry.
constexpr CodeSig kSigTextField{24, 0x0f2e9d45b41be4be}, kSigGlyph{24, 0x16dd948fbeb1738a};
constexpr u32 kTextFieldSigOffset = 0x20, kGlyphSigOffset = 0x2e;
constexpr u32 kFieldObject = 0x18, kSurfaceClip = 0x14;
// Its loop over the glyphs of text in an embedded outline font, up to the
// call that builds one glyph's shape (font glyph offsets in r3, this glyph's
// offset r4, the field's display object r21; the glyph's matrix on the stack
// at +0x34: a, b, c, d in 16.16 and tx, ty in twips), hooked right after the
// run. The whole text is placed each time the field is drawn.
constexpr CodeSig kSigOutlineGlyph{76, 0x0892485bf6e04a78};
constexpr u32 kOutlineMatrix = 0x34;

// Cars race engine, in each build known (compiled differently): its version
// of DrawPlane (plane in r0, surface r1, mode r2, palettes r3) and its floor
// routine (surface in r0), each found by a run some way past its entry; the
// floor confirmed by a second hash (including its row range). The floor's
// data is at addresses the routine loads as long immediates: its context,
// the sine table, the depth table and the pointer to the cosine table; others
// are checked against the context (field offset). Its sprite-layer loop (as
// DrawSpriteLayer): found by the run where an object's draw begins (object
// in r14), confirmed with its step to the next object further on.
struct RaceBuild {
  CodeSig plane;
  u32 plane_at;
  CodeSig floor;
  u32 floor_at;
  std::array<CodeRun, 4> floor_runs;
  u64 floor_check;
  u32 ctx_imm, sine_imm, depth_imm, cosine_imm;
  std::array<std::pair<u32, u32>, 5> ctx_imms;  // (offset, context field)
  int depth_row0;                               // depth table row of the first floor row
  CodeSig sprite;
  u32 sprite_end;
  u64 sprite_check;
};
constexpr RaceBuild kRaceBuilds[] = {
    // Cars (USA), and the Cars Supercharged prototype of January 2007.
    {{68, 0x0ab4a6a43f03d5d5}, 0xce, {32, 0x149f63525428c387}, 0xd2,
     {{{0xd2, 0x20}, {0x13c, 0x18}, {0x16a, 0x0a}, {0x84, 0x0c}}}, 0x1d6ea7091a86ff21,
     0x0c, 0xa2, 0xb6, 0xc6, {{{0xf8, 0x38}, {0x10e, 0x3c}, {0x132, 0x28}, {0x158, 0x20}, {0x178, 0x0c}}}, 10,
     {48, 0x13023d86c828cf63}, 0xae, 0x00d015980d21c437},
    // Cars (Germany).
    {{56, 0x1292023a90f1d78c}, 0xbe, {70, 0x12782a7017ec4dd3}, 0xd4,
     {{{0xd4, 0x46}, {0x128, 0x32}, {0x160, 0x2c}, {0x86, 0x0e}}}, 0x04239a00013f133a,
     0x72, 0xce, 0x9e, 0xb6, {{{0xac, 0}, {0x11c, 0}, {0x15c, 0}, {0x190, 0}, {0xac, 0}}}, 10,
     {48, 0x1cf25547c128c8e8}, 0xc0, 0x1e8872b217fc13a4},
    // Cars Supercharged (USA).
    {{56, 0x0faa965b1e20fd29}, 0x64, {66, 0x03fd30b1c72ab027}, 0x6c,
     {{{0x6c, 0x42}, {0xb4, 0x3a}, {0xf4, 0x2c}, {0x1c, 0x0e}}}, 0x04b4d48ed1ddba77,
     0x08, 0x64, 0x34, 0x4c, {{{0x42, 0}, {0xb0, 0}, {0xf0, 0}, {0x122, 0}, {0x42, 0}}}, 9,
     {48, 0x1cf25547c128c8e8}, 0xc8, 0x1c4998aa1bd3c067},
};
constexpr u32 kRaceObjScale = 0x30, kRaceObjScaleSet = 0x34, kRaceObjScaleMul = 0x38;
// Floor context fields: palettes (16 x 16 colours), tiles, the 128x128 ring
// map, the horizon row, the camera (1/4096 texel) and heading (1/4096 step).
constexpr u32 kFloorPalette = 0x0c, kFloorTiles = 0x20, kFloorMap = 0x28, kFloorHorizon = 0x34, kFloorCamX = 0x38,
              kFloorCamY = 0x3c, kFloorHeading = 0x40;
// Race plane fields: scroll (whole pixels), map, tiles, clip (x0, y0, x1, y1).
constexpr u32 kRacePlaneScrollX = 0x00, kRacePlaneScrollY = 0x04, kRacePlaneMap = 0x0c, kRacePlaneTiles = 0x14,
              kRacePlaneClip = 0x20;

// Sprite object fields.
constexpr u32 kObjX = 0x24, kObjY = 0x28;
// Plane object fields.
constexpr u32 kPlaneScrollX = 0x00, kPlaneScrollY = 0x04, kPlaneMap = 0x0c, kPlaneTiles = 0x14, kPlaneClip = 0x28,
              kPlaneAlpha = 0x38;

u16 blend(u16 dst, u16 src, u32 a) {
  auto ch = [&](int s) { return u16((((dst >> s) & 15) * (256 - a) + ((src >> s) & 15) * a) >> 8) << s; };
  return u16(ch(8) | ch(4) | ch(0));
}

}  // namespace

u32 rgb12_to_argb(u16 c) {
  return 0xff000000u | (((c >> 8) & 15) * 17u) << 16 | (((c >> 4) & 15) * 17u) << 8 | (c & 15) * 17u;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

u16 NativeFrame::Plane::sample(int wx, int wy) const {
  const u16 e = map[size_t((wy >> 3) & 31) * 32 + size_t((wx >> 3) & 31)];
  int tx = wx & 7, ty = wy & 7;
  if (e & 0x400) tx = 7 - tx;
  if (e & 0x800) ty = 7 - ty;
  const size_t o = size_t(e & 0x3ff) * 32 + size_t(ty) * 4 + size_t(tx >> 1);
  if (o >= tiles.size()) return 0;
  const int idx = (tx & 1) ? tiles[o] >> 4 : tiles[o] & 15;
  if (idx == 0 && mode != 0) return 0;
  return u16(kDrawn | (palette[size_t(e >> 12) * 16 + size_t(idx)] & 0x0fff));
}

u16 NativeFrame::Floor::texel(s32 u, s32 v) const {
  const u16 e = map[size_t(((v >> 15) & 127) << 7 | ((u >> 15) & 127))];
  const size_t o = size_t(e & 0xfff) * 32 + size_t(((v >> 12) & 7) << 2 | ((u >> 13) & 3));
  if (o >= tiles.size()) return 0;
  const int idx = ((u >> 12) & 1) ? tiles[o] >> 4 : tiles[o] & 15;
  return palette[size_t(idx | ((e >> 8) & 0xf0))] & 0x0fff;
}

NativeFrame::Floor::View NativeFrame::Floor::view() const {
  const size_t a = (heading >> 12) & 255;
  return {double(cam_x), double(cam_y), double(sine[a]), double(cosine[a]), double(horizon)};
}

bool NativeFrame::Floor::between(const Floor& a, const Floor& b, double t, View& out) {
  // Larger steps than these between two game frames are cuts, not movement.
  constexpr s32 kMaxStep = 64 * 4096;
  constexpr int kMaxTurn = 32, kMaxTilt = 48;
  const int ia = int(a.heading >> 12) & 255, ib = int(b.heading >> 12) & 255;
  const int turn = ((ib - ia + 128) & 255) - 128;
  if (std::abs(b.horizon - a.horizon) > kMaxTilt || std::abs(turn) > kMaxTurn || std::abs(b.cam_x - a.cam_x) > kMaxStep ||
      std::abs(b.cam_y - a.cam_y) > kMaxStep)
    return false;
  const double angle = ia + t * turn, i0 = std::floor(angle), f = angle - i0;
  const size_t k0 = size_t(int(i0) & 255), k1 = (k0 + 1) & 255;
  out.x = a.cam_x + t * (double(b.cam_x) - a.cam_x);
  out.y = a.cam_y + t * (double(b.cam_y) - a.cam_y);
  out.sine = b.sine[k0] + f * (b.sine[k1] - b.sine[k0]);
  out.cosine = b.cosine[k0] + f * (b.cosine[k1] - b.cosine[k0]);
  out.horizon = a.horizon + t * (b.horizon - a.horizon);
  return true;
}

void NativeFrame::Floor::draw(const View& v, u16* out) const {
  const s32 cx = s32(std::lround(v.x)), cy = s32(std::lround(v.y));
  const s32 sn = s32(std::lround(v.sine)), cs = s32(std::lround(v.cosine));
  for (int y = std::max(0, first_row()); y < kH; y++) {
    const int r = y - first_row() + depth_row0;  // depth table row
    if (r < 0 || r >= int(depth.size())) continue;
    // As the engine: the low words of the products, then fixed-point steps.
    const s32 d80 = depth[size_t(r)] * 80;
    const s32 a = s32(u32(-d80) * u32(cs)), b = s32(u32(sn) * u32(d80));
    const s32 du = a >> 17, dv = b >> 17;
    u32 u = u32(cx - du * 80 + ((b >> 10) - (b >> 13))), w = u32(cy - dv * 80 - ((a >> 10) - (a >> 13)));
    u16* row = out + size_t(y - first_row()) * kW;
    for (int x = 0; x < kW; x++, u += u32(du), w += u32(dv)) row[x] = texel(s32(u), s32(w));
  }
}

void NativeFrame::Floor::draw_scaled(const View& v, int scale, int top, u32* out) const {
  const int W = kW * scale, rows = (kH - top) * scale;
  for (int Y = 0; Y < rows; Y++) {
    // The screen row of this output row's centre, and its depth (the table
    // is 65536 / (row + 1): interpolated as 1 / depth, exact for that shape).
    const double sy = top + (Y + 0.5) / scale - 0.5, r = sy - (v.horizon - 1) + depth_row0;
    u32* row = out + size_t(Y) * W;
    if (r < depth_row0 - 0.5) {  // above the floor
      std::fill(row, row + W, 0u);
      continue;
    }
    const int r0 = std::clamp(int(std::floor(r)), 1, int(depth.size()) - 2);
    const double f = r - r0;
    const double inv = (1.0 - f) / std::max(1, depth[size_t(r0)]) + f / std::max(1, depth[size_t(r0) + 1]);
    const double d80 = inv > 0 ? 80.0 / inv : 0;
    const double a = -d80 * v.cosine, b = v.sine * d80;
    const double du = a / 131072.0, dv = b / 131072.0;
    // Along the row in 64-bit fixed point (16 more fraction bits).
    const double sx0 = 0.5 / scale - 0.5 - 80.0;
    s64 u = s64(std::floor((v.x + b * (7.0 / 8192.0) + du * sx0) * 65536.0));
    s64 w = s64(std::floor((v.y - a * (7.0 / 8192.0) + dv * sx0) * 65536.0));
    const s64 su = s64(std::llround(du / scale * 65536.0)), sw = s64(std::llround(dv / scale * 65536.0));
    for (int X = 0; X < W; X++, u += su, w += sw) row[X] = rgb12_to_argb(texel(s32(u >> 16), s32(w >> 16)));
  }
}

void NativeFrame::compose(u16* out) const {
  std::fill(out, out + kW * kH, u16(0));
  for (const Op& op : ops) {
    if (op.kind == OpKind::kFloor) {
      const Floor& fl = floors[op.index];
      std::vector<u16> rows(size_t(kH - fl.first_row()) * kW);
      fl.draw(fl.view(), rows.data());
      for (int y = std::max(0, fl.first_row()); y < kH; y++)
        std::copy_n(&rows[size_t(y - fl.first_row()) * kW], kW, out + size_t(y) * kW);
    } else if (op.kind == OpKind::kPlane) {
      const Plane& p = planes[op.index];
      const int px = p.scroll_x >> 12, py = p.scroll_y >> 12;
      const int x0 = std::max(0, p.clip[0]), y0 = std::max(0, p.clip[1]);
      const int x1 = std::min(kW, p.clip[2]), y1 = std::min(kH, p.clip[3]);
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          const u16 s = p.sample(x + px, y + py);
          if (!s) continue;
          u16& d = out[size_t(y) * kW + size_t(x)];
          d = p.mode == 2 ? blend(d, s & 0x0fff, p.alpha) : u16(s & 0x0fff);
        }
    } else {
      const Sprite& s = sprites[op.index];
      for (int y = 0; y < s.bh; y++)
        for (int x = 0; x < s.bw; x++)
          if (const u16 v = s.px[size_t(y) * size_t(s.bw) + size_t(x)])
            out[size_t(s.by + y) * kW + size_t(s.bx + x)] = v & 0x0fff;
    }
  }
}

// ---------------------------------------------------------------------------
// Interpolated layers
// ---------------------------------------------------------------------------

void native_layers(const NativeFrame& b, const NativeFrame* a, double t, std::vector<std::vector<u32>>& storage,
                   std::vector<NativeLayer>& layers, int scale) {
  scale = std::clamp(scale, 1, 8);
  t = std::clamp(t, 0.0, 1.0);
  layers.clear();
  size_t used = 0;
  auto buffer = [&](size_t n) -> std::vector<u32>& {
    if (storage.size() <= used) storage.emplace_back();
    auto& v = storage[used++];
    v.assign(n, 0);
    return v;
  };
  // Motion larger than this between two game frames is a cut, not movement.
  constexpr double kMaxPlaneStep = 64.0 * 4096, kMaxSpriteStep = 48;

  for (const NativeFrame::Op& op : b.ops) {
    NativeLayer L;
    if (op.kind == NativeFrame::OpKind::kFloor) {
      const auto& fl = b.floors[op.index];
      NativeFrame::Floor::View v = fl.view();
      if (a && !a->floors.empty() && !NativeFrame::Floor::between(a->floors.front(), fl, t, v)) v = fl.view();
      const int top = std::clamp(int(std::floor(v.horizon - 1)), 0, kH - 1), rows = kH - top;
      auto& img = buffer(size_t(kW) * scale * rows * scale);
      if (scale == 1 && v.horizon == fl.horizon) {  // (exact, as the engine)
        std::vector<u16> px(size_t(kW) * rows);
        fl.draw(v, px.data());
        for (size_t i = 0; i < px.size(); i++) img[i] = rgb12_to_argb(px[i]);
      } else {
        fl.draw_scaled(v, scale, top, img.data());
      }
      L.px = img.data();
      L.w = kW * scale; L.h = rows * scale;
      L.texel = 1.0f / float(scale);
      L.y = float(top);
    } else if (op.kind == NativeFrame::OpKind::kPlane) {
      const auto& p = b.planes[op.index];
      double sx = p.scroll_x, sy = p.scroll_y;
      s32 clip[4] = {p.clip[0], p.clip[1], p.clip[2], p.clip[3]};
      if (a) {
        for (const auto& q : a->planes) {
          if (q.id != p.id) continue;
          const double dx = double(p.scroll_x) - q.scroll_x, dy = double(p.scroll_y) - q.scroll_y;
          if (std::abs(dx) < kMaxPlaneStep && std::abs(dy) < kMaxPlaneStep) {
            sx = q.scroll_x + t * dx;
            sy = q.scroll_y + t * dy;
            // A clip that changes (the Cars sky, cut at the horizon while the
            // camera tilts) covers both: what is drawn after it covers the rest.
            for (int k = 0; k < 4; k++) clip[k] = k < 2 ? std::min(clip[k], q.clip[k]) : std::max(clip[k], q.clip[k]);
          }
          break;
        }
      }
      // Render at the integer part of the scroll; the fraction becomes the
      // layer's sub-pixel position.
      const double fx = std::floor(sx / 4096.0), fy = std::floor(sy / 4096.0);
      const int px = int(fx), py = int(fy);
      const int w = kW + 1, h = kH + 1;
      auto& img = buffer(size_t(w) * h);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          if (const u16 s = p.sample(x + px, y + py)) img[size_t(y) * w + x] = rgb12_to_argb(s);
      L.px = img.data();
      L.w = w; L.h = h;
      L.x = float(fx - sx / 4096.0);
      L.y = float(fy - sy / 4096.0);
      for (int k = 0; k < 4; k++) L.clip[k] = clip[k];
      L.opacity = p.mode == 2 ? u8(std::min(255u, u32(p.alpha) * 255u / 256u)) : 255;
    } else {
      const auto& s = b.sprites[op.index];
      // Its position and scale between the two frames; the pixels drawn in
      // `b` are scaled around the position.
      double x = s.x, y = s.y, k = 1;
      if (a) {
        for (const auto& q : a->sprites) {
          if (q.id != s.id) continue;
          const double dx = s.x - q.x, dy = s.y - q.y, ratio = s.scale > 0 ? double(q.scale) / s.scale : 0;
          if (std::abs(dx) < kMaxSpriteStep && std::abs(dy) < kMaxSpriteStep && ratio > 0.5 && ratio < 2) {
            x = q.x + t * dx;
            y = q.y + t * dy;
            k = ratio + t * (1 - ratio);
          }
          break;
        }
      }
      auto& img = buffer(size_t(s.bw) * s.bh);
      for (size_t i = 0; i < img.size(); i++) if (s.px[i]) img[i] = rgb12_to_argb(s.px[i]);
      L.px = img.data();
      L.w = s.bw; L.h = s.bh;
      L.texel = float(k);
      L.x = float(x + (s.bx - s.x) * k);
      L.y = float(y + (s.by - s.y) * k);
    }
    layers.push_back(L);
  }
  // Whatever the ops don't explain (text drawn straight to the LCD
  // framebuffer, effects), as in `b`, on top.
  if (b.residual_count) {
    auto& img = buffer(size_t(kW) * kH);
    for (size_t i = 0; i < img.size(); i++) if (b.residual[i]) img[i] = rgb12_to_argb(b.residual[i]);
    NativeLayer L;
    L.px = img.data();
    L.w = kW; L.h = kH;
    layers.push_back(L);
  }
}

void composite_layers(const std::vector<NativeLayer>& layers, int scale, std::vector<u32>& out) {
  const int W = kW * scale, H = kH * scale;
  out.assign(size_t(W) * H, 0xff000000u);
  for (const NativeLayer& L : layers) {
    const int cx0 = std::max(0, L.clip[0] * scale), cy0 = std::max(0, L.clip[1] * scale);
    const int cx1 = std::min(W, L.clip[2] * scale), cy1 = std::min(H, L.clip[3] * scale);
    // Output pixel (ox, oy) samples the layer at screen position
    // ((ox + 0.5) / scale, (oy + 0.5) / scale).
    const double tw = L.w * double(L.texel), th = L.h * double(L.texel);
    const int ox0 = std::max(cx0, int(std::floor(L.x * scale))), ox1 = std::min(cx1, int(std::ceil((L.x + tw) * scale)));
    const int oy0 = std::max(cy0, int(std::floor(L.y * scale))), oy1 = std::min(cy1, int(std::ceil((L.y + th) * scale)));
    for (int oy = oy0; oy < oy1; oy++) {
      const int ly = int(std::floor(((oy + 0.5) / scale - L.y) / L.texel));
      if (ly < 0 || ly >= L.h) continue;
      for (int ox = ox0; ox < ox1; ox++) {
        const int lx = int(std::floor(((ox + 0.5) / scale - L.x) / L.texel));
        if (lx < 0 || lx >= L.w) continue;
        const u32 s = L.px[size_t(ly) * L.w + lx];
        if (!(s >> 24)) continue;
        u32& d = out[size_t(oy) * W + ox];
        if (L.opacity == 255) { d = s; continue; }
        const u32 a = L.opacity;
        auto ch = [&](int sh) { return (((d >> sh) & 255) * (255 - a) + ((s >> sh) & 255) * a) / 255 << sh; };
        d = 0xff000000u | ch(16) | ch(8) | ch(0);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

DrawCapture::Sites DrawCapture::locate(const std::vector<u8>& cart, u32 cart_base, const std::vector<u8>& bios,
                                       u32 bios_base) {
  Sites s;
  // Cartridge tile-plane engine.
  const auto c = find_code(cart, {kSigDrawPlane, kSigSpriteLayer});
  if (!c[0].empty() && !c[1].empty() && c[1][0] >= kSpriteLayerSigOffset) {
    const size_t plane = c[0][0], layer = c[1][0] - kSpriteLayerSigOffset;
    if (code_check(cart, plane, {{0, kSigDrawPlane.len}, {kPlaneReady, 4}}) == kCheckDrawPlane &&
        code_check(cart, layer, {{kSpriteLayerSigOffset, kSigSpriteLayer.len}, {kSpriteBegin, 4}, {kSpriteEnd, 4}}) ==
            kCheckSpriteLayer) {
      s.plane = cart_base + u32(plane);
      s.sprite_layer = cart_base + u32(layer);
    }
  }
  // Cars race engine.
  for (const RaceBuild& rb : kRaceBuilds) {
    const auto r = find_code(cart, {rb.plane, rb.floor, rb.sprite});
    if (r[0].empty() || r[1].empty() || r[0][0] < rb.plane_at || r[1][0] < rb.floor_at) continue;
    const size_t fl = r[1][0] - rb.floor_at;
    // An ARCompact long immediate: two halfwords, the high one first.
    auto imm = [&](u32 off) -> u32 {
      const size_t o = fl + off;
      if (o + 4 > cart.size()) return 0;
      return u32(cart[o] | cart[o + 1] << 8) << 16 | u32(cart[o + 2] | cart[o + 3] << 8);
    };
    const u32 ctx = imm(rb.ctx_imm);
    bool ok = ctx && code_check(cart, fl, {rb.floor_runs.begin(), rb.floor_runs.end()}) == rb.floor_check;
    for (const auto& [off, field] : rb.ctx_imms) ok = ok && imm(off) == ctx + field;
    if (!ok) continue;
    s.race_plane = cart_base + u32(r[0][0] - rb.plane_at);
    s.floor = cart_base + u32(fl);
    s.floor_ctx = ctx;
    s.floor_sine = imm(rb.sine_imm);
    s.floor_depth = imm(rb.depth_imm);
    s.floor_cosine = imm(rb.cosine_imm);
    s.floor_row0 = u32(rb.depth_row0);
    if (!r[2].empty() && code_check(cart, r[2][0], {{0, rb.sprite.len}, {rb.sprite_end, 4}}) == rb.sprite_check) {
      s.race_sprite = cart_base + u32(r[2][0]);
      s.race_sprite_end = s.race_sprite + rb.sprite_end;
    }
    break;
  }
  // BaseROM Flash player and its text engine.
  const auto b = find_code(bios, {kSigFlashObject, kSigTextField, kSigGlyph, kSigOutlineGlyph});
  if (!b[0].empty() && b[0][0] >= kFlashObjectSigOffset) {
    s.flash_object = bios_base + u32(b[0][0] - kFlashObjectSigOffset);
    if (!b[1].empty() && b[1][0] >= kTextFieldSigOffset && !b[2].empty() && b[2][0] >= kGlyphSigOffset) {
      s.text_field = bios_base + u32(b[1][0] - kTextFieldSigOffset);
      s.glyph = bios_base + u32(b[2][0] - kGlyphSigOffset);
      if (!b[3].empty()) s.outline_glyph = bios_base + u32(b[3][0] + kSigOutlineGlyph.len);
    }
  }
  return s;
}

bool DrawCapture::install(const Bus& bus, arc::Cpu& cpu, const Sites& s) {
  uninstall(cpu);
  bus_ = &bus;
  sites_ = s;
  if (s.plane && s.sprite_layer) {
    cpu.set_pc_hook(s.plane + kPlaneReady, &DrawCapture::hook_plane, this);
    cpu.set_pc_hook(s.sprite_layer + kSpriteBegin, &DrawCapture::hook_sprite_begin, this);
    cpu.set_pc_hook(s.sprite_layer + kSpriteEnd, &DrawCapture::hook_sprite_end, this);
    engine_ = "tile-plane engine";
    LOG_I("native draw capture: tile-plane engine (DrawPlane %08x, DrawSpriteLayer %08x)", s.plane, s.sprite_layer);
  }
  if (s.race_plane && s.floor) {
    cpu.set_pc_hook(s.race_plane, &DrawCapture::hook_race_plane, this);
    cpu.set_pc_hook(s.floor, &DrawCapture::hook_floor, this);
    if (s.race_sprite) {
      cpu.set_pc_hook(s.race_sprite, &DrawCapture::hook_race_sprite, this);
      cpu.set_pc_hook(s.race_sprite_end, &DrawCapture::hook_sprite_end, this);
    }
    engine_ = "Cars race engine";
    LOG_I("native draw capture: Cars race engine (DrawPlane %08x, floor %08x)", s.race_plane, s.floor);
  }
  if (s.flash_object) {
    cpu.set_pc_hook(s.flash_object, &DrawCapture::hook_flash_object, this);
    flash_hooked_ = true;
    engine_ += engine_.empty() ? "Flash player" : " + Flash player";
    if (s.text_field && s.glyph) {
      cpu.set_pc_hook(s.text_field, &DrawCapture::hook_text_field, this);
      cpu.set_pc_hook(s.glyph, &DrawCapture::hook_glyph, this);
      if (s.outline_glyph) cpu.set_pc_hook(s.outline_glyph, &DrawCapture::hook_outline_glyph, this);
    }
    LOG_I("native draw capture: BaseROM Flash player (draw object %08x)", s.flash_object);
  }
  installed_ = !engine_.empty();
  reset();
  return installed_;
}

void DrawCapture::uninstall(arc::Cpu& cpu) {
  cpu.clear_pc_hooks();
  installed_ = false;
  flash_hooked_ = false;
  engine_.clear();
  reset();
  done_.reset();
}

void DrawCapture::reset() {
  cur_.reset();
  done_.reset();
  surface_px_ = 0;
  race_surface_ = 0;
  pending_ = false;
  unsupported_ = false;
  flash_drawn_ = false;
  flash_ctx_ = 0;
  flash_last_.reset();
  fields_.clear();
  glyph_surface_ = 0;
  text_obj_ = 0;
  text_drawn_ = false;
  stray_glyphs_ = false;
}

void DrawCapture::hook_text_field(void* self_, const arc::Cpu& cpu) {
  auto* self = static_cast<DrawCapture*>(self_);
  const u32 obj = self->bus_->peek32(cpu.reg(0) + kFieldObject);
  FieldCapture& f = self->fields_[obj];
  f.gen = self->flash_gen_;
  // The player redraws dirty regions, and a field overlapping one is drawn
  // clipped to it: glyphs outside it are not drawn again but stay on screen.
  // So only the glyphs inside the region are replaced: they are stale unless
  // drawn again (removed when the frame is published).
  f.stale.resize(f.glyphs.size(), 0);
  f.drawn.resize(f.glyphs.size(), 0);
  if (self->glyph_surface_) {
    s16 c[4];
    for (int i = 0; i < 4; i++) c[i] = s16(self->bus_->peek16(self->glyph_surface_ + kSurfaceClip + 2 * u32(i)));
    for (size_t i = 0; i < f.glyphs.size(); i++) {
      const auto& g = f.glyphs[i];
      if (g.x <= c[2] && g.x + g.w - 1 >= c[0] && g.y <= c[3] && g.y + g.h - 1 >= c[1]) f.stale[i] = 1;
    }
  } else {
    std::fill(f.stale.begin(), f.stale.end(), 1);
  }
  f.outlines.clear();  // (an outline-font text is placed whole every time)
  self->text_obj_ = obj;
  self->text_drawn_ = true;
}

void DrawCapture::hook_outline_glyph(void* self_, const arc::Cpu& cpu) {
  auto* self = static_cast<DrawCapture*>(self_);
  const Bus& b = *self->bus_;
  const auto it = self->fields_.find(cpu.reg(21));
  if (it == self->fields_.end()) { self->stray_glyphs_ = true; return; }
  const u32 m = cpu.reg(28) + kOutlineMatrix;
  auto fix = [&](u32 i) { return double(s32(b.peek32(m + 4 * i))) / 65536.0; };
  flash::Frame::Outline g;
  g.shape = cpu.reg(3) + cpu.reg(4);
  g.m = swf::Matrix{fix(0), fix(1), fix(2), fix(3), double(s32(b.peek32(m + 16))), double(s32(b.peek32(m + 20)))};
  if (it->second.outlines.size() < 4096) it->second.outlines.push_back(g);
}

void DrawCapture::hook_glyph(void* self_, const arc::Cpu& cpu) {
  auto* self = static_cast<DrawCapture*>(self_);
  if (!self->text_obj_) { self->stray_glyphs_ = true; return; }
  const Bus& b = *self->bus_;
  self->glyph_surface_ = cpu.reg(0);
  flash::Frame::Glyph g;
  g.bits = cpu.reg(1);
  g.w = u16(cpu.reg(3));
  g.h = u16(cpu.reg(4));
  g.x = s16(cpu.reg(5));
  g.y = s16(cpu.reg(6));
  g.rgb = cpu.reg(7) & 0xffffff;
  for (int i = 0; i < 4; i++) g.clip[i] = s16(b.peek16(cpu.reg(0) + kSurfaceClip + 2 * u32(i)));
  FieldCapture& f = self->fields_[self->text_obj_];
  auto& glyphs = f.glyphs;
  f.stale.resize(glyphs.size(), 0);
  f.drawn.resize(glyphs.size(), 0);
  // The field is drawn once per dirty region: keep one of each glyph, with
  // the union of the clip rectangles it was drawn with.
  for (size_t i = 0; i < glyphs.size(); i++) {
    auto& e = glyphs[i];
    if (e.bits == g.bits && e.x == g.x && e.y == g.y) {
      e.clip[0] = std::min(e.clip[0], g.clip[0]);
      e.clip[1] = std::min(e.clip[1], g.clip[1]);
      e.clip[2] = std::max(e.clip[2], g.clip[2]);
      e.clip[3] = std::max(e.clip[3], g.clip[3]);
      e.rgb = g.rgb;
      f.stale[i] = 0;
      f.drawn[i] = self->flash_gen_;
      return;
    }
  }
  if (glyphs.size() < 4096) {
    glyphs.push_back(g);
    f.stale.push_back(0);
    f.drawn.push_back(self->flash_gen_);
  }
}

void DrawCapture::hook_flash_object(void* self_, const arc::Cpu& cpu) {
  auto* self = static_cast<DrawCapture*>(self_);
  self->flash_ctx_ = self->bus_->peek32(cpu.reg(0) + 8);  // display object -> player context
  self->flash_drawn_ = true;
}

namespace {
bool same_flash(const flash::Frame& a, const flash::Frame& b) {
  if (a.objects.size() != b.objects.size() || a.background != b.background || a.unsupported != b.unsupported ||
      std::memcmp(&a.view, &b.view, sizeof(a.view)) != 0) return false;
  for (size_t i = 0; i < a.objects.size(); i++) {
    const flash::Object& p = a.objects[i];
    const flash::Object& q = b.objects[i];
    if (p.id != q.id || p.parent != q.parent || p.def != q.def || p.movie != q.movie || p.type != q.type || p.tag != q.tag ||
        p.char_id != q.char_id || p.ratio != q.ratio || p.visible != q.visible || p.depth != q.depth || p.clip_depth != q.clip_depth ||
        std::memcmp(&p.local, &q.local, sizeof(p.local)) != 0 || std::memcmp(&p.local_cx, &q.local_cx, sizeof(p.local_cx)) != 0)
      return false;
  }
  return true;
}
}  // namespace

void DrawCapture::publish_flash(u64 frame_index) {
  flash_drawn_ = false;
  if (!flash_ctx_) return;
  auto f = std::make_shared<flash::Frame>();
  if (!flash::snapshot(*bus_, flash_ctx_, *f)) return;
  // Text fields: the glyphs of their latest drawing, and where the field was
  // then (its stage -> screen transform), so they can follow it.
  // A field drawn somewhere else than before (it moved), or that is a
  // different field in a reused display object, is drawn whole: its older
  // glyphs are gone (where it was is redrawn without it).
  for (size_t i = 0; i < f->objects.size(); i++) {
    const flash::Object& o = f->objects[i];
    if (o.type != flash::Frame::kTypeEditText) continue;
    const auto it = fields_.find(o.id);
    if (it == fields_.end() || it->second.gen != flash_gen_) continue;  // not drawn in this frame
    FieldCapture& fc = it->second;
    swf::Matrix w = o.local;
    for (s32 p = o.parent; p >= 0; p = f->objects[size_t(p)].parent) w = f->objects[size_t(p)].local * w;
    const swf::Matrix placed = f->view * w;
    const bool moved = fc.def != o.def || std::abs(placed.tx - fc.placed.tx) > 0.01 || std::abs(placed.ty - fc.placed.ty) > 0.01 ||
                       std::abs(placed.a - fc.placed.a) > 1e-6 || std::abs(placed.b - fc.placed.b) > 1e-6 ||
                       std::abs(placed.c - fc.placed.c) > 1e-6 || std::abs(placed.d - fc.placed.d) > 1e-6;
    fc.drawn.resize(fc.glyphs.size(), 0);
    fc.stale.resize(fc.glyphs.size(), 0);
    if (moved)
      for (size_t k = 0; k < fc.glyphs.size(); k++)
        if (fc.drawn[k] != flash_gen_) fc.stale[k] = 1;
    fc.def = o.def;
    fc.placed = placed;
    fc.placed_set = true;
  }
  for (auto& [id, fc] : fields_) {  // glyphs redrawn over (stale) are gone
    fc.stale.resize(fc.glyphs.size(), 0);
    fc.drawn.resize(fc.glyphs.size(), 0);
    size_t n = 0;
    for (size_t i = 0; i < fc.glyphs.size(); i++)
      if (!fc.stale[i]) { fc.glyphs[n] = fc.glyphs[i]; fc.drawn[n] = fc.drawn[i]; n++; }
    fc.glyphs.resize(n);
    fc.drawn.resize(n);
    fc.stale.assign(n, 0);
  }
  for (size_t i = 0; i < f->objects.size(); i++) {
    const flash::Object& o = f->objects[i];
    if (o.type != flash::Frame::kTypeEditText) continue;
    const auto it = fields_.find(o.id);
    if (it == fields_.end() || !it->second.placed_set) continue;
    f->fields[o.id] = flash::Frame::Field{it->second.placed, it->second.glyphs, it->second.outlines};
  }
  if (fields_.size() > 512) {  // forget fields that no longer exist
    for (auto it = fields_.begin(); it != fields_.end();)
      if (!f->fields.count(it->first)) it = fields_.erase(it); else ++it;
  }
  // The player redraws dirty regions one after another (several DMAs per
  // frame); the display list is the same until the timeline advances or a
  // text field changes.
  const bool text_changed = text_drawn_;
  text_drawn_ = false;
  if (stray_glyphs_) f->unsupported = true;  // text drawn by some other path: not in the redraw
  stray_glyphs_ = false;
  text_obj_ = 0;
  flash_gen_++;
  if (!text_changed && flash_last_ && same_flash(*flash_last_, *f)) return;
  flash_last_ = f;
  auto nf = std::make_shared<NativeFrame>();
  nf->flash = f;
  nf->frame_index = frame_index;
  done_ = std::move(nf);
}

bool DrawCapture::read_u16s(u32 addr, u16* out, size_t n) const {
  const u8* p = bus_->host_ptr(addr);
  if (p && bus_->host_ptr(addr + u32(n * 2) - 1) == p + n * 2 - 1) {  // contiguous host memory
    for (size_t i = 0; i < n; i++) out[i] = u16(p[2 * i] | (p[2 * i + 1] << 8));
    return true;
  }
  for (size_t i = 0; i < n; i++) out[i] = bus_->peek16(addr + u32(2 * i));
  return true;
}

bool DrawCapture::read_surface(std::vector<u16>& out) const {
  if (!surface_px_) return false;
  out.resize(size_t(kW) * kH);
  return read_u16s(surface_px_, out.data(), out.size());
}

// The planes' tiles: up to the highest one the map uses.
static void read_tiles(const Bus& b, u32 tiles, size_t n, std::vector<u8>& out) {
  out.resize(n);
  const u8* hp = b.host_ptr(tiles);
  if (n && hp && b.host_ptr(tiles + u32(n) - 1) == hp + n - 1)
    std::memcpy(out.data(), hp, n);
  else
    for (size_t i = 0; i < n; i++) out[i] = b.peek8(tiles + u32(i));
}

// Starts a frame's capture if needed and checks the surface drawn to: only
// frames composed in one 160x160 16-bit surface are supported. Returns false
// if the draw goes to another surface than the frame's (and is not captured).
bool DrawCapture::begin_op(u32 surf, bool other_surface_ok) {
  const Bus& b = *bus_;
  flush_sprite();
  if (!cur_) cur_ = std::make_shared<NativeFrame>();
  const u32 px = b.peek32(surf + 0xc);
  if (b.peek8(surf + 1) != 16 || b.peek16(surf + 2) != kW || b.peek16(surf + 4) != kH) {
    unsupported_ = true;
    return false;
  }
  if (surface_px_ && px != surface_px_ && !cur_->ops.empty()) {
    if (other_surface_ok) return false;
    unsupported_ = true;
  }
  surface_px_ = px;
  return true;
}

void DrawCapture::hook_plane(void* self_, const arc::Cpu& cpu) {
  auto* self = static_cast<DrawCapture*>(self_);
  const Bus& b = *self->bus_;
  const u32 p = cpu.reg(14), surf = cpu.reg(7), mode = cpu.reg(15), pal = cpu.reg(8);
  self->begin_op(surf, false);
  if (mode > 2) self->unsupported_ = true;
  NativeFrame::Plane pl;
  pl.id = p;
  pl.scroll_x = s32(b.peek32(p + kPlaneScrollX));
  pl.scroll_y = s32(b.peek32(p + kPlaneScrollY));
  for (int k = 0; k < 4; k++) pl.clip[k] = s32(b.peek32(p + kPlaneClip + 4 * u32(k)));
  pl.mode = u8(mode);
  pl.alpha = u16(std::min<u32>(256, b.peek32(p + kPlaneAlpha)));
  self->read_u16s(b.peek32(p + kPlaneMap), pl.map.data(), pl.map.size());
  self->read_u16s(pal, pl.palette.data(), pl.palette.size());
  u32 max_tile = 0;
  for (u16 e : pl.map) max_tile = std::max<u32>(max_tile, e & 0x3ff);
  read_tiles(b, b.peek32(p + kPlaneTiles), (size_t(max_tile) + 1) * 32, pl.tiles);
  NativeFrame& f = *self->cur_;
  f.ops.push_back({NativeFrame::OpKind::kPlane, u32(f.planes.size())});
  f.planes.push_back(std::move(pl));
}

// Cars: the sky planes. Draws to other surfaces (off-screen images) are not
// part of the frame.
void DrawCapture::hook_race_plane(void* self_, const arc::Cpu& cpu) {
  auto* self = static_cast<DrawCapture*>(self_);
  const Bus& b = *self->bus_;
  const u32 p = cpu.reg(0), surf = cpu.reg(1), mode = cpu.reg(2), pal = cpu.reg(3);
  if (self->race_surface_ && b.peek32(surf + 0xc) != self->race_surface_) return;
  if (!self->begin_op(surf, true)) return;
  if (mode > 1) self->unsupported_ = true;  // (its alpha mode is not used by the race)
  NativeFrame::Plane pl;
  pl.id = p;
  pl.scroll_x = s32(b.peek32(p + kRacePlaneScrollX)) * 4096;
  pl.scroll_y = s32(b.peek32(p + kRacePlaneScrollY)) * 4096;
  for (int k = 0; k < 4; k++) pl.clip[k] = s32(b.peek32(p + kRacePlaneClip + 4 * u32(k)));
  pl.mode = u8(mode);
  self->read_u16s(b.peek32(p + kRacePlaneMap), pl.map.data(), pl.map.size());
  self->read_u16s(pal, pl.palette.data(), pl.palette.size());
  u32 max_tile = 0;
  for (u16 e : pl.map) max_tile = std::max<u32>(max_tile, e & 0x3ff);
  read_tiles(b, b.peek32(p + kRacePlaneTiles), (size_t(max_tile) + 1) * 32, pl.tiles);
  NativeFrame& f = *self->cur_;
  f.ops.push_back({NativeFrame::OpKind::kPlane, u32(f.planes.size())});
  f.planes.push_back(std::move(pl));
}

// Cars: the floor, as the routine is about to draw it.
void DrawCapture::hook_floor(void* self_, const arc::Cpu& cpu) {
  auto* self = static_cast<DrawCapture*>(self_);
  const Bus& b = *self->bus_;
  const Sites& st = self->sites_;
  const u32 surf = cpu.reg(0), c = st.floor_ctx;
  self->race_surface_ = b.peek32(surf + 0xc);
  if (!self->begin_op(surf, false)) return;
  NativeFrame::Floor fl;
  fl.horizon = s32(b.peek32(c + kFloorHorizon));
  fl.cam_x = s32(b.peek32(c + kFloorCamX));
  fl.cam_y = s32(b.peek32(c + kFloorCamY));
  fl.heading = b.peek32(c + kFloorHeading);
  fl.depth_row0 = int(st.floor_row0);
  if (fl.horizon < 1 || fl.horizon > kH || (fl.heading >> 12) > 255) { self->unsupported_ = true; return; }
  fl.map.resize(128 * 128);
  self->read_u16s(b.peek32(c + kFloorMap), fl.map.data(), fl.map.size());
  self->read_u16s(b.peek32(c + kFloorPalette), fl.palette.data(), fl.palette.size());
  u32 max_tile = 0;
  for (u16 e : fl.map) max_tile = std::max<u32>(max_tile, e & 0xfff);
  read_tiles(b, b.peek32(c + kFloorTiles), (size_t(max_tile) + 1) * 32, fl.tiles);
  const u32 cosine = b.peek32(st.floor_cosine);
  for (u32 i = 0; i < 256; i++) {
    fl.sine[i] = s32(b.peek32(st.floor_sine + 4 * i));
    fl.cosine[i] = s32(b.peek32(cosine + 4 * i));
  }
  for (u32 i = 0; i < fl.depth.size(); i++) fl.depth[i] = s32(b.peek32(st.floor_depth + 4 * i));
  NativeFrame& f = *self->cur_;
  f.ops.push_back({NativeFrame::OpKind::kFloor, u32(f.floors.size())});
  f.floors.push_back(std::move(fl));
}

void DrawCapture::hook_sprite_begin(void* self_, const arc::Cpu& cpu) {
  auto* self = static_cast<DrawCapture*>(self_);
  self->flush_sprite();
  if (!self->cur_ || !self->read_surface(self->before_)) return;  // sprites before any plane: not ours
  const u32 obj = cpu.reg(14);
  self->pend_ = {};
  self->pend_.id = obj;
  self->pend_.x = s32(self->bus_->peek32(obj + kObjX));
  self->pend_.y = s32(self->bus_->peek32(obj + kObjY));
  self->pending_ = true;
}

void DrawCapture::hook_race_sprite(void* self_, const arc::Cpu& cpu) {
  auto* self = static_cast<DrawCapture*>(self_);
  self->flush_sprite();
  if (!self->cur_ || !self->read_surface(self->before_)) return;
  const Bus& b = *self->bus_;
  const u32 obj = cpu.reg(14);
  self->pend_ = {};
  self->pend_.id = obj;
  self->pend_.x = s32(b.peek32(obj + kObjX));
  self->pend_.y = s32(b.peek32(obj + kObjY));
  const s32 set = s32(b.peek32(obj + kRaceObjScaleSet)), mul = s32(b.peek32(obj + kRaceObjScaleMul));
  const s32 scale = s32(b.peek32(obj + kRaceObjScale));
  self->pend_.scale = set ? set : mul == 0x10000 ? scale : s32((s64(scale) * mul) >> 16);
  self->pending_ = true;
}

void DrawCapture::hook_sprite_end(void* self_, const arc::Cpu&) { static_cast<DrawCapture*>(self_)->flush_sprite(); }

void DrawCapture::flush_sprite() {
  if (!pending_) return;
  pending_ = false;
  if (!cur_ || !read_surface(after_)) return;
  int x0 = kW, y0 = kH, x1 = -1, y1 = -1;
  for (int y = 0; y < kH; y++)
    for (int x = 0; x < kW; x++)
      if (after_[size_t(y) * kW + x] != before_[size_t(y) * kW + x]) {
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
      }
  if (x1 < 0) return;  // drew nothing visible
  NativeFrame::Sprite& s = pend_;
  s.bx = x0; s.by = y0; s.bw = x1 - x0 + 1; s.bh = y1 - y0 + 1;
  s.px.assign(size_t(s.bw) * s.bh, 0);
  for (int y = 0; y < s.bh; y++)
    for (int x = 0; x < s.bw; x++) {
      const size_t i = size_t(y0 + y) * kW + size_t(x0 + x);
      if (after_[i] != before_[i]) s.px[size_t(y) * s.bw + x] = u16(NativeFrame::kDrawn | (after_[i] & 0x0fff));
    }
  cur_->ops.push_back({NativeFrame::OpKind::kSprite, u32(cur_->sprites.size())});
  cur_->sprites.push_back(std::move(s));
}

void DrawCapture::on_dma(u32 src, u32 bytes, u64 frame_index) {
  if (!installed_) return;
  flush_sprite();
  if ((!cur_ || cur_->ops.empty()) && flash_drawn_) { publish_flash(frame_index); return; }
  if (!cur_ || cur_->ops.empty()) return;  // nothing drawn since the last frame (a repeated DMA)
  std::shared_ptr<NativeFrame> f = std::move(cur_);
  cur_.reset();
  const bool bad = unsupported_ || bytes != u32(kW * kH * 3 / 2);
  unsupported_ = false;
  if (bad) { done_.reset(); return; }
  // The LCD image: 12 bpp, two pixels in three bytes (as Machine::render_frame).
  f->screen.resize(size_t(kW) * kH);
  for (int y = 0; y < kH; y++)
    for (int x = 0; x < kW / 2; x++) {
      const u32 a = src + u32(y * kW * 3 / 2 + x * 3);
      const u16 b0 = bus_->peek8(a), b1 = bus_->peek8(a + 1), b2 = bus_->peek8(a + 2);
      f->screen[size_t(y) * kW + size_t(2 * x)] = u16(((b1 >> 4) << 8) | b0);
      f->screen[size_t(y) * kW + size_t(2 * x + 1)] = u16(((b1 & 15) << 8) | b2);
    }
  std::vector<u16> mine(size_t(kW) * kH);
  f->compose(mine.data());
  f->residual.assign(mine.size(), 0);
  f->residual_count = 0;
  for (size_t i = 0; i < mine.size(); i++)
    if (mine[i] != f->screen[i]) { f->residual[i] = u16(NativeFrame::kDrawn | f->screen[i]); f->residual_count++; }
  f->frame_index = frame_index;
  done_ = std::move(f);
}

}  // namespace leap
