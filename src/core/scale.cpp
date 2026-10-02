#include "core/scale.h"

#include <algorithm>
#include <cmath>

namespace leap::scale {

namespace {

// Resamples by k with a symmetric kernel of radius r (in source pixels), in
// two passes, with 14-bit fixed-point weights. Source edges are clamped.
// Colours are filtered premultiplied by alpha, so transparent pixels (layers
// with holes) do not bleed their colour into the edges.
template <class Kernel>
void resample(const u32* src, int w, int h, int k, int r, Kernel kernel, std::vector<u32>& out) {
  const int taps = 2 * r;
  // The weights repeat every k output pixels (one set per phase).
  std::vector<int> weight(size_t(k) * size_t(taps), 0);
  std::vector<int> first(size_t(k), 0);
  for (int p = 0; p < k; p++) {
    const double sx = (p + 0.5) / k - 0.5;  // source position of output pixel p (of pixel 0)
    const int base = int(std::floor(sx)) - r + 1;
    first[size_t(p)] = base;
    double wsum = 0, wv[16];
    for (int t = 0; t < taps; t++) wsum += (wv[t] = kernel(sx - (base + t)));
    int isum = 0;
    for (int t = 0; t < taps; t++) isum += (weight[size_t(p * taps + t)] = int(std::lround(wv[t] / wsum * 16384)));
    weight[size_t(p * taps + r)] += 16384 - isum;  // exact sum
  }
  auto clamp8 = [](int v) { return u32(std::clamp((v + 8192) >> 14, 0, 255)); };
  const int W = w * k, H = h * k;
  // Horizontal: w*k x h, channels kept as ints.
  std::vector<int> mid(size_t(W) * size_t(h) * 4);
  for (int y = 0; y < h; y++) {
    const u32* row = src + size_t(y) * size_t(w);
    for (int ox = 0; ox < W; ox++) {
      const int p = ox % k, sx0 = ox / k + first[size_t(p)];
      int c[4] = {0, 0, 0, 0};
      for (int t = 0; t < taps; t++) {
        const u32 px = row[std::clamp(sx0 + t, 0, w - 1)];
        const int wt = weight[size_t(p * taps + t)];
        const int a = int(px >> 24);
        c[0] += wt * (int((px >> 16) & 255) * a / 255);
        c[1] += wt * (int((px >> 8) & 255) * a / 255);
        c[2] += wt * (int(px & 255) * a / 255);
        c[3] += wt * a;
      }
      int* m = &mid[(size_t(y) * size_t(W) + size_t(ox)) * 4];
      for (int i = 0; i < 4; i++) m[i] = int(clamp8(c[i]));
    }
  }
  // Vertical.
  out.resize(size_t(W) * size_t(H));
  for (int oy = 0; oy < H; oy++) {
    const int p = oy % k, sy0 = oy / k + first[size_t(p)];
    for (int x = 0; x < W; x++) {
      int c[4] = {0, 0, 0, 0};
      for (int t = 0; t < taps; t++) {
        const int* m = &mid[(size_t(std::clamp(sy0 + t, 0, h - 1)) * size_t(W) + size_t(x)) * 4];
        const int wt = weight[size_t(p * taps + t)];
        for (int i = 0; i < 4; i++) c[i] += wt * m[i];
      }
      const u32 a = clamp8(c[3]);
      u32 rgb[3];
      for (int i = 0; i < 3; i++) rgb[i] = a ? std::min<u32>(255, clamp8(c[i]) * 255 / a) : 0;  // (un-premultiply)
      out[size_t(oy) * size_t(W) + size_t(x)] = a << 24 | rgb[0] << 16 | rgb[1] << 8 | rgb[2];
    }
  }
}

}  // namespace

void bicubic(const u32* src, int w, int h, int k, std::vector<u32>& out) {
  resample(src, w, h, k, 2, [](double x) {  // Catmull-Rom (B = 0, C = 0.5)
    x = std::abs(x);
    if (x < 1) return 1.5 * x * x * x - 2.5 * x * x + 1;
    if (x < 2) return -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2;
    return 0.0;
  }, out);
}

void lanczos(const u32* src, int w, int h, int k, std::vector<u32>& out) {
  resample(src, w, h, k, 2, [](double x) {
    constexpr double a = 2, pi = 3.14159265358979323846;
    x = std::abs(x);
    if (x < 1e-9) return 1.0;
    if (x >= a) return 0.0;
    return a * std::sin(pi * x) * std::sin(pi * x / a) / (pi * pi * x * x);
  }, out);
}

void lcd(const u32* src, int w, int h, int k, std::vector<u32>& out) {
  const int W = w * k;
  out.resize(size_t(W) * size_t(h) * size_t(k));
  // The gap between cells: one output pixel, darker (less so at small sizes,
  // where it would dominate).
  const int keep = k >= 4 ? 160 : k == 3 ? 185 : 215;  // of 256
  for (int y = 0; y < h * k; y++) {
    const bool gy = k >= 2 && y % k == k - 1;
    for (int x = 0; x < W; x++) {
      const u32 p = src[size_t(y / k) * size_t(w) + size_t(x / k)];
      if (gy || (k >= 2 && x % k == k - 1)) {
        const u32 r = ((p >> 16) & 255) * u32(keep) >> 8, g = ((p >> 8) & 255) * u32(keep) >> 8, b = (p & 255) * u32(keep) >> 8;
        out[size_t(y) * size_t(W) + size_t(x)] = 0xff000000u | r << 16 | g << 8 | b;
      } else {
        out[size_t(y) * size_t(W) + size_t(x)] = p;
      }
    }
  }
}

void lcd_grid(int w, int h, int k, std::vector<u32>& out) {
  const int W = w * k, H = h * k;
  out.assign(size_t(W) * size_t(H), 0);
  if (k < 2) return;
  const u32 keep = k >= 4 ? 160 : k == 3 ? 185 : 215;  // as lcd()
  const u32 shade = (256 - keep) << 24;               // black at 1 - keep/256
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if (y % k == k - 1 || x % k == k - 1) out[size_t(y) * size_t(W) + size_t(x)] = shade;
}

// ---------------------------------------------------------------------------
// MMPX (https://casual-effects.com/research/McGuire2021PixelArt/). The rules
// follow the authors' reference code; the image border is clamped. The
// reference code's license (also in THIRD-PARTY.md):
//
// Copyright 2020 Morgan McGuire & Mara Gagiu
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// ---------------------------------------------------------------------------

namespace {

inline u32 luma(u32 c) {
  const u32 alpha = c >> 24;
  return (((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255) + 1) * (256 - alpha);
}
inline bool all_eq2(u32 b, u32 a0, u32 a1) { return ((b ^ a0) | (b ^ a1)) == 0; }
inline bool all_eq3(u32 b, u32 a0, u32 a1, u32 a2) { return ((b ^ a0) | (b ^ a1) | (b ^ a2)) == 0; }
inline bool all_eq4(u32 b, u32 a0, u32 a1, u32 a2, u32 a3) { return ((b ^ a0) | (b ^ a1) | (b ^ a2) | (b ^ a3)) == 0; }
inline bool any_eq3(u32 b, u32 a0, u32 a1, u32 a2) { return b == a0 || b == a1 || b == a2; }
inline bool none_eq2(u32 b, u32 a0, u32 a1) { return b != a0 && b != a1; }
inline bool none_eq4(u32 b, u32 a0, u32 a1, u32 a2, u32 a3) { return b != a0 && b != a1 && b != a2 && b != a3; }

}  // namespace

void mmpx2x(const u32* s, int w, int h, std::vector<u32>& out) {
  out.resize(size_t(w) * size_t(h) * 4);
  auto src = [&](int x, int y) { return s[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))]; };
  const size_t W = size_t(w) * 2;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const u32 A = src(x - 1, y - 1), B = src(x, y - 1), C = src(x + 1, y - 1);
      const u32 D = src(x - 1, y), E = src(x, y), F = src(x + 1, y);
      const u32 G = src(x - 1, y + 1), H = src(x, y + 1), I = src(x + 1, y + 1);
      const u32 Q = src(x - 2, y), R = src(x + 2, y);
      u32 J = E, K = E, L = E, M = E;  // outputs: top-left, top-right, bottom-left, bottom-right
      if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0) {
        const u32 P = src(x, y - 2), S = src(x, y + 2);
        const u32 Bl = luma(B), Dl = luma(D), El = luma(E), Fl = luma(F), Hl = luma(H);
        // 1:1 slope rules
        if ((D == B && D != H && D != F) && (El >= Dl || E == A) && any_eq3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q)) J = D;
        if ((B == F && B != D && B != H) && (El >= Bl || E == C) && any_eq3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R)) K = B;
        if ((H == D && H != F && H != B) && (El >= Hl || E == G) && any_eq3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q)) L = H;
        if ((F == H && F != B && F != D) && (El >= Fl || E == I) && any_eq3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S)) M = F;
        // Intersection rules
        if ((E != F && all_eq4(E, C, I, D, Q) && all_eq2(F, B, H)) && (F != src(x + 3, y))) K = M = F;
        if ((E != D && all_eq4(E, A, G, F, R) && all_eq2(D, B, H)) && (D != src(x - 3, y))) J = L = D;
        if ((E != H && all_eq4(E, G, I, B, P) && all_eq2(H, D, F)) && (H != src(x, y + 3))) L = M = H;
        if ((E != B && all_eq4(E, A, C, H, S) && all_eq2(B, D, F)) && (B != src(x, y - 3))) J = K = B;
        if (Bl < El && all_eq4(E, G, H, I, S) && none_eq4(E, A, D, C, F)) J = K = B;
        if (Hl < El && all_eq4(E, A, B, C, P) && none_eq4(E, D, G, I, F)) L = M = H;
        if (Fl < El && all_eq4(E, A, D, G, Q) && none_eq4(E, B, C, I, H)) K = M = F;
        if (Dl < El && all_eq4(E, C, F, I, R) && none_eq4(E, B, A, G, H)) J = L = D;
        // 2:1 slope rules
        if (H != B) {
          if (H != A && H != E && H != C) {
            if (all_eq3(H, G, F, R) && none_eq2(H, D, src(x + 2, y - 1))) L = M;
            if (all_eq3(H, I, D, Q) && none_eq2(H, F, src(x - 2, y - 1))) M = L;
          }
          if (B != I && B != G && B != E) {
            if (all_eq3(B, A, F, R) && none_eq2(B, D, src(x + 2, y + 1))) J = K;
            if (all_eq3(B, C, D, Q) && none_eq2(B, F, src(x - 2, y + 1))) K = J;
          }
        }
        if (F != D) {
          if (D != I && D != E && D != C) {
            if (all_eq3(D, A, H, S) && none_eq2(D, B, src(x + 1, y + 2))) J = L;
            if (all_eq3(D, G, B, P) && none_eq2(D, H, src(x + 1, y - 2))) L = J;
          }
          if (F != E && F != A && F != G) {
            if (all_eq3(F, C, H, S) && none_eq2(F, B, src(x - 1, y + 2))) K = M;
            if (all_eq3(F, I, B, P) && none_eq2(F, H, src(x - 1, y - 2))) M = K;
          }
        }
      }
      u32* d = &out[size_t(y) * 2 * W + size_t(x) * 2];
      d[0] = J;
      d[1] = K;
      d[W] = L;
      d[W + 1] = M;
    }
  }
}

}  // namespace leap::scale
