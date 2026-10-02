// lfc2wav: list and decode LeapFrog "LFC" speech assets (Leapster voice 7) to
// 8 kHz mono WAV files, using leapemu's CelpDecoder.
//
// Build: the lfc2wav target (cmake -DLEAPEMU_BUILD_TOOLS=ON), or by hand from the root:
//   c++ -std=c++20 -O2 -Isrc tools/speech/lfc2wav.cpp src/core/celp.cpp
//       src/core/rom.cpp -o build/lfc2wav   (one command line)
//
// Usage:
//   lfc2wav --bios BIOS --rom IMAGE --list
//   lfc2wav --bios BIOS --rom IMAGE --index N  -o out.wav
//   lfc2wav --bios BIOS --rom IMAGE --addr HEX -o out.wav   (address or file offset)
//   lfc2wav --bios BIOS --rom IMAGE --all DIR
//
// BIOS supplies the codebook (BaseROM offset 0x900, 0x4f00 bytes). IMAGE is a
// cartridge or BaseROM (.bin, .zip or WAV-wrapped); its LFC table is found via
// the ROM header -> "LEAP" RIB -> asset group 0x1006 -> asset id 4.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/celp.h"
#include "core/rom.h"

using namespace leap;

namespace {

u16 le16(const std::vector<u8>& v, size_t o) { return o + 2 <= v.size() ? u16(v[o] | v[o + 1] << 8) : 0; }
u32 le32(const std::vector<u8>& v, size_t o) { return o + 4 <= v.size() ? u32(le16(v, o) | u32(le16(v, o + 2)) << 16) : 0; }

struct Entry {
  u32 handle;
  u32 addr;
};

// Returns the LFC table entries of a ROM image (empty if none).
std::vector<Entry> lfc_table(const std::vector<u8>& img, u32 base) {
  std::vector<Entry> out;
  const RomHeader h = parse_rom_header(img);
  if (!h.valid || h.rib_table < base) return out;
  size_t r = h.rib_table - base;
  if (r + 32 > img.size() || std::memcmp(&img[r], "LEAP", 4) != 0) return out;
  const unsigned groups = le16(img, r + 6);
  r += 32;
  for (unsigned g = 0; g < groups; g++, r += 8) {
    if (le16(img, r) != 0x1006) continue;
    const unsigned count = le16(img, r + 2);
    size_t q = le32(img, r + 4) - base;
    for (unsigned i = 0; i < count; i++, q += 8) {
      if (le16(img, q) != 4) continue;
      const size_t t = le32(img, q + 4) - base;
      const u32 first = le32(img, t + 8), n = le32(img, t + 12);
      for (u32 k = 0; k < n && k < 0x10000; k++) out.push_back({first + k, le32(img, t + 16 + 4 * k)});
    }
  }
  return out;
}

struct Decoded {
  std::vector<s16> pcm;
  u32 bytes = 0;
  unsigned mode = 0;
};

Decoded decode(CelpDecoder& dec, const std::vector<u8>& img, u32 off) {
  Decoded d;
  d.mode = le16(img, off);
  auto rd = [&](u32 a) -> u8 { return a < img.size() ? img[a] : 0; };
  dec.start(off);
  while (dec.active()) d.pcm.push_back(dec.next_sample(rd));
  d.bytes = dec.position() - off;
  return d;
}

bool write_wav(const std::string& path, const std::vector<s16>& pcm) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  auto w32 = [&](u32 v) { u8 b[4] = {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24)}; std::fwrite(b, 1, 4, f); };
  auto w16 = [&](u16 v) { u8 b[2] = {u8(v), u8(v >> 8)}; std::fwrite(b, 1, 2, f); };
  const u32 bytes = u32(pcm.size() * 2);
  std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
  w32(16); w16(1); w16(1); w32(CelpDecoder::kSampleRate); w32(CelpDecoder::kSampleRate * 2); w16(2); w16(16);
  std::fwrite("data", 1, 4, f); w32(bytes);
  for (s16 s : pcm) w16(u16(s));
  std::fclose(f);
  return true;
}

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: lfc2wav --bios BIOS --rom IMAGE (--list | --index N | --addr HEX | --all DIR) [-o out.wav]\n");
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::string bios_path, rom_path, out = "out.wav", all_dir;
  bool list = false;
  long index = -1;
  long long addr = -1;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) usage(); return argv[++i]; };
    if (a == "--bios") bios_path = next();
    else if (a == "--rom") rom_path = next();
    else if (a == "--list") list = true;
    else if (a == "--index") index = std::strtol(next().c_str(), nullptr, 0);
    else if (a == "--addr") addr = std::strtoll(next().c_str(), nullptr, 16);
    else if (a == "--all") all_dir = next();
    else if (a == "-o") out = next();
    else usage();
  }
  if (bios_path.empty() || rom_path.empty()) usage();

  std::vector<u8> bios, img;
  std::string err;
  if (!load_rom_file(bios_path, &bios, &err) || !load_rom_file(rom_path, &img, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  const u32 cb_off = CelpDecoder::kCodebookPageOffset;
  if (bios.size() < cb_off + CelpDecoder::kCodebookSize) {
    std::fprintf(stderr, "BIOS image too small for the codebook\n");
    return 1;
  }
  CelpDecoder dec;
  dec.set_codebook(&bios[cb_off], CelpDecoder::kCodebookSize);

  const RomHeader h = parse_rom_header(img);
  const u32 base = h.valid ? h.device_start : 0;
  const std::vector<Entry> table = lfc_table(img, base);

  auto to_off = [&](u32 a) { return a >= base ? a - base : a; };

  if (list) {
    std::printf("%zu LFC entries (image base %08x)\n", table.size(), base);
    std::printf("%6s %8s %8s %4s %7s %8s\n", "handle", "address", "offset", "mode", "bytes", "seconds");
    for (const Entry& e : table) {
      const Decoded d = decode(dec, img, to_off(e.addr));
      std::printf("%6u %08x %8x %4u %7u %8.3f\n", e.handle, e.addr, to_off(e.addr), d.mode, d.bytes,
                  d.pcm.size() / double(CelpDecoder::kSampleRate));
    }
    return 0;
  }
  if (!all_dir.empty()) {
    for (const Entry& e : table) {
      const Decoded d = decode(dec, img, to_off(e.addr));
      char name[64];
      std::snprintf(name, sizeof name, "/lfc_%04u_%08x.wav", e.handle, e.addr);
      write_wav(all_dir + name, d.pcm);
    }
    std::printf("wrote %zu files to %s\n", table.size(), all_dir.c_str());
    return 0;
  }
  u32 off;
  if (index >= 0) {
    const Entry* e = nullptr;
    for (const Entry& x : table)
      if (x.handle == u32(index)) e = &x;
    if (!e && size_t(index) < table.size()) e = &table[index];
    if (!e) { std::fprintf(stderr, "no LFC entry %ld\n", index); return 1; }
    off = to_off(e->addr);
  } else if (addr >= 0) {
    off = to_off(u32(addr));
  } else {
    usage();
  }
  const Decoded d = decode(dec, img, off);
  if (!write_wav(out, d.pcm)) { std::perror(out.c_str()); return 1; }
  std::printf("offset %x: mode %u, %u bytes, %zu samples (%.3f s) -> %s\n", off, d.mode, d.bytes, d.pcm.size(),
              d.pcm.size() / double(CelpDecoder::kSampleRate), out.c_str());
  return 0;
}
