// code_sig: prints the values leapemu uses to recognise a run of ROM code
// (core/codesig.h), so that the code's bytes need not be copied into the
// source. Offsets are into the image file (after unwrapping and joining
// development-image blocks), in hex.
// Usage: code_sig IMAGE OFFSET LENGTH             a CodeSig, and where it matches
//        code_sig IMAGE AT OFF:LEN [OFF:LEN...]   a code_check() of runs from AT
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/codesig.h"
#include "core/rom.h"
using namespace leap;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: code_sig IMAGE OFFSET LENGTH\n       code_sig IMAGE AT OFF:LEN [OFF:LEN...]\n");
    return 2;
  }
  std::vector<u8> img;
  std::string err;
  if (!load_rom_file(argv[1], &img, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  join_rom_blocks(&img);
  const size_t at = std::strtoull(argv[2], nullptr, 16);
  if (std::string(argv[3]).find(':') == std::string::npos) {
    const size_t len = std::strtoull(argv[3], nullptr, 16);
    if (len == 0 || at + len > img.size()) { std::fprintf(stderr, "out of range\n"); return 2; }
    const CodeSig sig{u32(len), code_hash(img.data() + at, len)};
    std::printf("{%u, 0x%016llx}\n", sig.len, static_cast<unsigned long long>(sig.hash));
    const auto found = find_code(img, {sig}, 1, 16);
    for (size_t o : found[0]) std::printf("  matches at %zx\n", o);
    return 0;
  }
  std::vector<CodeRun> runs;
  for (int a = 3; a < argc; a++) {
    char* end = nullptr;
    const u32 off = u32(std::strtoul(argv[a], &end, 16));
    if (!end || *end != ':') { std::fprintf(stderr, "bad run %s (want OFF:LEN)\n", argv[a]); return 2; }
    runs.push_back({off, u32(std::strtoul(end + 1, nullptr, 16))});
  }
  const u64 h = code_check(img, at, runs);
  if (!h) { std::fprintf(stderr, "out of range\n"); return 2; }
  std::printf("0x%016llx\n", static_cast<unsigned long long>(h));
  return 0;
}
