#pragma once

// Image scaling filters for presenting the 160x160 screen larger. All work on
// ARGB images (0xAARRGGBB; alpha is kept, for layers with transparent parts)
// and write w*k x h*k (or 2w x 2h) images.

#include <vector>

#include "core/common.h"

namespace leap::scale {

// Separable resampling by an integer factor k: Catmull-Rom bicubic, and
// Lanczos with a = 2 (sharper, with slight halos on hard edges).
void bicubic(const u32* src, int w, int h, int k, std::vector<u32>& out);
void lanczos(const u32* src, int w, int h, int k, std::vector<u32>& out);

// MMPX 2x pixel-art magnification: Morgan McGuire and Mara Gagiu, "MMPX
// Style-Preserving Pixel Art Magnification", JCGT 10(2), 2021 (reference code
// under the MIT license). Rebuilds diagonals and curves from exact colour
// matches, so it suits pixel art, not anti-aliased or photographic images.
void mmpx2x(const u32* src, int w, int h, std::vector<u32>& out);

// An LCD look: each pixel a k x k cell with darker gaps between cells.
void lcd(const u32* src, int w, int h, int k, std::vector<u32>& out);
// Just the LCD's gaps, as a w*k x h*k overlay (black, partly transparent) to
// draw over an image of w x h pixels.
void lcd_grid(int w, int h, int k, std::vector<u32>& out);

}  // namespace leap::scale
