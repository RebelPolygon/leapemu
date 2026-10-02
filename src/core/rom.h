#pragma once

#include <string>
#include <vector>

#include "core/common.h"

namespace leap {

// Load a ROM/BIOS image from disk. Handles:
//  - raw .bin images
//  - images wrapped in a RIFF/WAVE container (how the community BIOS dumps
//    circulate): the contents of the "data" chunk are returned
//  - .zip archives: the first .bin member (stored or deflated) is returned
// On failure returns false and fills *err.
bool load_rom_file(const std::string& path, std::vector<u8>* out, std::string* err);

// The first n bytes of the image load_rom_file would give, reading only
// those (and, for a zip, its directory): to tell what a file is cheaply.
bool peek_rom_file(const std::string& path, size_t n, std::vector<u8>* out, std::string* err);

// Some prototype images are development uploads rather than plain dumps: each
// 4 MiB block after the first starts with a 24-byte ASCII block header ("LBK"
// and hex fields: the block's start and last offset) that the cartridge's own
// addresses do not count, so everything past 4 MiB sits 24 bytes too far on.
// Removes those headers; returns how many it removed. (Identify an image by
// the CRC of the file as loaded, before this.)
unsigned join_rom_blocks(std::vector<u8>* img);

// Cartridge/BaseROM header ("AppTable"), as documented by LeapFrog-Tools'
// LeapSplit. The fixed header lives at file offset 0x100.
struct RomHeader {
  bool valid = false;
  u32 device_start = 0;  // Address the image expects to be mapped at.
  u32 device_end = 0;
  u32 rib_table = 0;     // Address of the "LEAP" resource index block.
  std::string title;     // Part name, if present.
  std::string part_number;
  std::string version;
  std::string build_date;
  std::string build_tool;
  std::string copyright;
};

RomHeader parse_rom_header(const std::vector<u8>& image);

// A dump made with an address line stuck: for some address bit within the
// range the header says the image uses, every part with that bit set is a
// copy of the part with it clear, so half the ROM is missing (MAME flags the
// German v2.1 BaseROM dump this way: A19). Returns the bit, or -1.
int stuck_address_line(const std::vector<u8>& img, const RomHeader& h);

// What the RIB table lists (docs/cart-bios-abi.md): its groups, and the
// assets of the asset group (0x1006), each table's entries by handle.
struct RomGroup { u16 id = 0, count = 0; u32 addr = 0; };
struct RomAsset {
  u16 type = 0;        // asset table id (rom_asset_type_name)
  u32 handle = 0;
  u32 offset = 0;      // in the image
  u32 size = 0;        // exact if `exact`; else up to the next asset (or the image end)
  bool exact = false;
};
struct RomContents {
  std::vector<RomGroup> groups;
  std::vector<RomAsset> assets;  // by type, then handle
  u32 full_checksum = 0, sparse_checksum = 0;  // pointers from the header
  u32 product_id = 0, rom_version = 0;
  std::string build_tool;
};
RomContents list_rom_contents(const std::vector<u8>& image);
// "SWF (Flash)", ...; nullptr if unknown.
const char* rom_group_name(u16 id);
const char* rom_asset_type_name(u16 type);
const char* rom_asset_extension(u16 type);  // ".swf", ".bin", ...

u32 crc32(const u8* data, size_t len);

// Decompress a zlib stream (RFC 1950: 2-byte header + DEFLATE). The Adler-32
// trailer is not checked.
bool inflate_zlib(const u8* src, size_t len, std::vector<u8>* out);

}  // namespace leap
