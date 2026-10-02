#pragma once

// Writing a ROM's assets (rom.h: list_rom_contents) to files: Flash movies as
// .swf, speech (with the BaseROM's codebook) and A-law sounds as .wav, SYN
// music as MIDI (.mid), and the rest as stored (.bin); or all as stored.

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "core/rom.h"

namespace leap {

class CelpDecoder;
class SoundtrackDecoder;

// The file name an asset is written as: <handle> plus the extension for its
// converted form (.swf, .wav, .mid, .bmp, .avi) or, if `raw` or it cannot be
// converted, as stored (.bin, .syn, ...).
std::string asset_file_name(const std::vector<u8>& image, const RomAsset& a, bool raw);

// Bitmaps (PEG bitmaps, type 9; Flash bitmaps, type 0xD), decoded to
// straight-alpha 0xAARRGGBB pixels. The formats (observed; every bitmap of
// the 54 cartridge dumps decodes to exactly its size):
//   PEG: u8 1, u8 bits per pixel (16: 0x?RGB, 0xFFFF transparent; 8:
//     RGB332), u16 width, u16 height, 6 bytes 0, u32 address of the data.
//     Data: a byte c < 0x80 is a run of c+1 of the next pixel, else c-0x7F
//     literal pixels follow.
//   Flash: u32 address of the data, u32 format, u32 width, u32 height, u32
//     bytes per row. Format 2 (16-bit 0xTRGB, T = transparency 0-15) is
//     coded per row: 00 n (n transparent), 01 n p (n of p), 02 n / 03 n (n
//     pixels follow), 04 (next row), 05 (end). One encoder (Ratatouille's)
//     also puts runs (01 n p) inside a literal's pixels. Format 3 is 8-bit
//     indexed with a palette not found yet (not decoded).
struct AssetImage {
  int w = 0, h = 0;
  std::vector<u32> argb;
};
bool asset_is_image(const RomAsset& a);
bool decode_asset_image(const std::vector<u8>& image, const RomAsset& a, AssetImage* out);
// A 32-bit BMP with an alpha channel.
std::vector<u8> bmp_bytes(const AssetImage& im);

// Flash fonts (type 1): the bitmap fonts of the BaseROM's text engine
// (observed; every font of the 54 cartridge dumps decodes):
//   +0x04 u32 address of the family name, +0x08 of the full name;
//   +0x10 u8 height; +0x19 u8 character set (0x41 ISO 8859-15, 0x3F Mac Roman);
//   +0x2C 256 glyph records of 5 bytes: width, height, top (signed, above
//     the baseline), left (signed), advance;
//   +0x52C 256 u32 addresses of the glyphs' bitmaps (0: none). A bitmap has
//     rows of (width+1)/2 bytes, 4-bit coverage, high nibble first.
struct AssetFont {
  std::string family, full_name;
  int height = 0;
  struct Glyph { int w = 0, h = 0, top = 0, left = 0, advance = 0; bool present = false; std::vector<u8> coverage; };  // 0-15
  std::array<Glyph, 256> glyphs;
  std::array<u16, 256> unicode{};  // each code's character (0: none)
};
bool asset_is_font(const RomAsset& a);
bool decode_asset_font(const std::vector<u8>& image, const RomAsset& a, AssetFont* out);
// A TrueType font of the glyphs' pixels (pixels at least half covered).
std::vector<u8> ttf_bytes(const AssetFont& f);
// The glyphs 0x20-0xFF in a 16-column sheet, white with their coverage as
// alpha (for a preview).
AssetImage font_sheet(const AssetFont& f);

// The asset in its converted form (see above), or as stored if `raw`.
// `speech` decodes speech; without it (or its codebook) speech is stored raw.
// `soundtracks` decodes soundtracks (type 0xF, as .wav); without it they
// cannot be converted. `title` names a MIDI file's song. Returns false if it
// cannot be converted.
bool convert_asset(const std::vector<u8>& image, const RomAsset& a, CelpDecoder* speech, bool raw, const std::string& title,
                   std::vector<u8>* out, SoundtrackDecoder* soundtracks = nullptr);

// Writes the asset to `path`.
bool export_asset(const std::vector<u8>& image, const RomAsset& a, CelpDecoder* speech, bool raw, const std::string& title,
                  const std::filesystem::path& path, SoundtrackDecoder* soundtracks = nullptr);

// Whether an asset can be listened to, and its sound: speech and A-law audio
// at 8 kHz, SYN music rendered (approximately) at `music_rate`, soundtracks
// at 11025 Hz (with `soundtracks`).
bool asset_is_audio(const RomAsset& a);
bool asset_audio(const std::vector<u8>& image, const RomAsset& a, CelpDecoder* speech, unsigned music_rate,
                 std::vector<s16>* pcm, unsigned* rate, SoundtrackDecoder* soundtracks = nullptr);

// The folder for an asset type under `root` ("SWF (Flash)", ...).
std::filesystem::path asset_folder(const std::filesystem::path& root, u16 type);

// A name usable as a file or folder name.
std::string safe_file_name(std::string s);

}  // namespace leap
