#include "core/flash/raster.h"

#include <algorithm>
#include <cmath>

namespace leap::flash {

void Coverage::resize(int w, int h, int band0, int band1) {
  w_ = w;
  h_ = h;
  band0_ = std::clamp(band0, 0, h);
  band1_ = std::clamp(band1, band0_, h);
  acc_.assign(size_t(w + 2) * size_t(band1_ - band0_), 0.0f);
  cov_.assign(size_t(w), 0.0f);
  y0_ = 1;
  y1_ = 0;
}

void Coverage::begin() {
  if (!empty() && x1_ >= x0_)
    for (int y = y0_; y < y1_; y++)
      std::fill(acc_.begin() + size_t(y - band0_) * (w_ + 2) + x0_, acc_.begin() + size_t(y - band0_) * (w_ + 2) + std::min(x1_ + 1, w_ + 2), 0.0f);
  y0_ = h_;
  y1_ = 0;
  x0_ = w_ + 2;
  x1_ = -1;
}

void Coverage::line(float x0, float y0, float x1, float y1) {
  if (y0 == y1 || !std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return;
  // Split at the left and right edges: area left of the canvas collapses onto
  // x = 0 (it still counts for the prefix sums); area right of it onto x = w.
  const float lim[2] = {0.0f, float(w_)};
  for (float xl : lim) {
    if ((x0 < xl && x1 > xl) || (x0 > xl && x1 < xl)) {  // strictly crosses
      const float t = (xl - x0) / (x1 - x0);
      const float ym = y0 + (y1 - y0) * t;
      line(x0, y0, xl, ym);
      line(xl, ym, x1, y1);
      return;
    }
  }
  segment(std::clamp(x0, 0.0f, float(w_)), y0, std::clamp(x1, 0.0f, float(w_)), y1);
}

void Coverage::segment(float px0, float py0, float px1, float py1) {
  float dir = 1.0f;
  if (py0 > py1) { std::swap(px0, px1); std::swap(py0, py1); dir = -1.0f; }
  const float top = float(band0_), bottom = float(band1_);
  if (py1 <= top || py0 >= bottom) return;
  const float dxdy = (px1 - px0) / (py1 - py0);
  float x = px0;
  if (py0 < top) { x += (top - py0) * dxdy; py0 = top; }
  const int ystart = int(py0);
  const int yend = std::min(int(bottom), int(std::ceil(py1)));
  y0_ = std::min(y0_, ystart);
  y1_ = std::max(y1_, yend);
  const int stride = w_ + 2;
  for (int y = ystart; y < yend; y++) {
    float* a = &acc_[size_t(y - band0_) * stride];
    const float dy = std::min(float(y + 1), py1) - std::max(float(y), py0);
    const float xnext = x + dxdy * dy;
    const float d = dy * dir;
    // Clamp against rounding drift (endpoints are already within [0, w]).
    const float xa = std::clamp(std::min(x, xnext), 0.0f, float(w_)), xb = std::clamp(std::max(x, xnext), 0.0f, float(w_));
    const float xaf = std::floor(xa);
    const int xai = int(xaf);
    const float xbc = std::ceil(xb);
    const int xbi = int(xbc);
    x0_ = std::min(x0_, xai);
    x1_ = std::max(x1_, xbi + 1);
    if (xbi <= xai + 1) {
      const float xmf = std::clamp(0.5f * (x + xnext) - xaf, 0.0f, 1.0f);
      a[xai] += d - d * xmf;
      a[xai + 1] += d * xmf;
    } else {
      const float s = 1.0f / (xb - xa);
      const float x0f = xa - xaf;
      const float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
      const float x1f = xb - xbc + 1.0f;
      const float am = 0.5f * s * x1f * x1f;
      a[xai] += d * a0;
      if (xbi == xai + 2) {
        a[xai + 1] += d * (1.0f - a0 - am);
      } else {
        const float a1 = s * (1.5f - x0f);
        a[xai + 1] += d * (a1 - a0);
        for (int xi = xai + 2; xi < xbi - 1; xi++) a[xi] += d * s;
        const float a2 = a1 + float(xbi - xai - 3) * s;
        a[xbi - 1] += d * (1.0f - a2 - am);
      }
      a[xbi] += d * am;
    }
    x = xnext;
  }
}

void Coverage::quad(float x0, float y0, float cx, float cy, float x1, float y1, float tolerance) {
  // (The curve lies within its control points' hull.)
  if (std::max({y0, cy, y1}) <= float(band0_) || std::min({y0, cy, y1}) >= float(band1_)) return;
  const float ddx = x0 - 2 * cx + x1, ddy = y0 - 2 * cy + y1;
  const float dd = std::sqrt(ddx * ddx + ddy * ddy);
  const int n = std::clamp(int(std::ceil(std::sqrt(dd / (8.0f * tolerance)))), 1, 64);
  float px = x0, py = y0;
  for (int i = 1; i <= n; i++) {
    const float t = float(i) / n, u = 1 - t;
    const float qx = u * u * x0 + 2 * u * t * cx + t * t * x1;
    const float qy = u * u * y0 + 2 * u * t * cy + t * t * y1;
    line(px, py, qx, qy);
    px = qx;
    py = qy;
  }
}

const float* Coverage::row(int y) {
  const float* a = &acc_[size_t(y - band0_) * (w_ + 2)];
  float sum = 0;
  const int end = col_end();
  for (int x = x0_; x < end; x++) {
    sum += a[x];
    cov_[size_t(x)] = std::min(1.0f, std::abs(sum));
  }
  return cov_.data();
}

}  // namespace leap::flash
