#pragma once

#include <string>

#include "core/common.h"

namespace leap {

// Write 0xAARRGGBB pixels as an 8-bit RGB PNG (uncompressed deflate blocks).
bool write_png(const std::string& path, const u32* pixels, int width, int height);

}  // namespace leap
