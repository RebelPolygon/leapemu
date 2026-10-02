#pragma once

// JPEG decoder (ITU-T T.81: Huffman-coded, 8-bit, sequential or progressive
// DCT), for the JPEG bitmaps in SWF movies (DefineBits, DefineBitsJPEG2/3).
// Lossless and arithmetic-coded images are not supported.

#include <vector>

#include "core/common.h"

namespace leap::swf {

// Decodes the concatenated JPEG streams in `parts` (e.g. a movie's
// JPEGTables followed by a DefineBits image; SOI/EOI markers between streams
// are ignored) into opaque ARGB pixels.
bool decode_jpeg(const std::vector<std::pair<const u8*, size_t>>& parts, int& w, int& h, std::vector<u32>& argb);

}  // namespace leap::swf
