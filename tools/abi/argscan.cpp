// argscan: logs function entries (detected as the instruction after a
// branch-and-link) whose r0-r7 point into an address range, e.g. a font's
// definition, with the caller and all eight arguments.
// Usage: argscan BIOS CART STATE|- LO_HEX HI_HEX [ticks] [max_lines]
// ("-": from power-on; UNSIGNED=1 in the environment: with the unsigned-cartridge patch)
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 6) { std::fprintf(stderr, "usage: argscan BIOS CART STATE LO HI [ticks] [max_lines]\n"); return 1; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.allow_unsigned = std::getenv("UNSIGNED") != nullptr;  // (prototypes, homebrew)
  m.reset();
  if (std::string(argv[3]) != "-" && !m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }  // "-": boot
  const u32 lo = u32(std::strtoul(argv[4], nullptr, 16)), hi = u32(std::strtoul(argv[5], nullptr, 16));
  const int ticks = argc > 6 ? std::atoi(argv[6]) : 30, max_lines = argc > 7 ? std::atoi(argv[7]) : 200;
  u32 prev_blink = 0;
  int lines = 0;
  std::map<u32, int> count;
  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    const u32 blink = c.reg(31);
    if (blink == prev_blink) return;
    prev_blink = blink;
    for (unsigned r = 0; r < 8; r++)
      if (c.reg(r) >= lo && c.reg(r) < hi) {
        count[c.pc()]++;
        if (lines++ < max_lines) {
          std::printf("%08x from %08x:", c.pc(), blink);
          for (unsigned k = 0; k < 8; k++) std::printf(" %08x", c.reg(k));
          std::printf("\n");
        }
        return;
      }
  };
  for (int f = 0; f < ticks; f++) m.run_frame();
  std::printf("entries by function:\n");
  for (auto& [pc, n] : count) std::printf("  %08x %d\n", pc, n);
}
