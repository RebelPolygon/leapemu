#pragma once

// Anti-aliased polygon coverage by signed-area accumulation: each line
// segment adds the area it covers to a per-row accumulation buffer; a prefix
// sum along a row gives each pixel's coverage. Overlapping regions of the
// same orientation clamp to full coverage (non-zero union).

#include <algorithm>
#include <vector>

#include "core/common.h"

namespace leap::flash {

class Coverage {
 public:
  // A w x h canvas, of which rows [band0, band1) are drawn (buffers are
  // kept for those rows only).
  void resize(int w, int h, int band0 = 0, int band1 = 1 << 30);
  int width() const { return w_; }
  int height() const { return h_; }
  int band_begin() const { return band0_; }
  int band_end() const { return band1_; }

  // Start a new polygon set (clears only what the previous set touched).
  void begin();
  // Add a directed line segment in pixel coordinates.
  void line(float x0, float y0, float x1, float y1);
  // Add a quadratic Bezier, flattened to `tolerance` pixels.
  void quad(float x0, float y0, float cx, float cy, float x1, float y1, float tolerance = 0.2f);
  bool empty() const { return y1_ < y0_; }
  int row_begin() const { return y0_; }
  int row_end() const { return y1_; }  // exclusive
  // Columns that can have coverage: [col_begin, col_end).
  int col_begin() const { return x0_; }
  int col_end() const { return std::min(x1_, w_); }
  // Coverage (0..1) of row `y`; valid for [col_begin, col_end).
  const float* row(int y);

 private:
  void segment(float x0, float y0, float x1, float y1);
  int w_ = 0, h_ = 0;
  std::vector<float> acc_;   // (w + 2) per row of the band
  std::vector<float> cov_;   // one row of prefix sums
  int y0_ = 1, y1_ = 0;      // rows touched
  int x0_ = 1, x1_ = 0;      // columns touched
  int band0_ = 0, band1_ = 1 << 30;
};

}  // namespace leap::flash
