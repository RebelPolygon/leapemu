// calltree: logs the calls made inside a function (entry PC), nested by
// depth, with r0-r3: function entries are detected as the instruction after
// a branch-and-link (blink set to the return address). Logs the first N
// invocations after the state.
// Usage: calltree BIOS CART STATE ENTRY_HEX [invocations] [ticks] [max_depth]
// R0=hex: only invocations with that first argument.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: calltree BIOS CART STATE ENTRY [invocations] [ticks] [max_depth]\n"); return 1; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  const u32 entry = u32(std::strtoul(argv[4], nullptr, 16));
  const int invocations = argc > 5 ? std::atoi(argv[5]) : 1, ticks = argc > 6 ? std::atoi(argv[6]) : 120;
  const int max_depth = argc > 7 ? std::atoi(argv[7]) : 3;
  int done = 0;
  bool active = false;
  u32 base_sp = 0, ret = 0, prev_blink = 0;
  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    const u32 pc = c.pc(), blink = c.reg(31), sp = c.reg(28);
    static const char* want = std::getenv("R0");  // only invocations with this r0 (hex)
    if (!active && pc == entry && done < invocations && (!want || c.reg(0) == u32(std::strtoul(want, nullptr, 16)))) {
      active = true;
      base_sp = sp;
      ret = blink;
      prev_blink = blink;
      std::printf("== call %08x(r0=%08x r1=%08x r2=%08x r3=%08x) from %08x\n", pc, c.reg(0), c.reg(1), c.reg(2), c.reg(3), blink);
      return;
    }
    if (!active) return;
    if (pc == ret && sp >= base_sp) { active = false; done++; std::printf("== return\n"); return; }
    if (blink != prev_blink) {
      // A call: the new blink points just past the call instruction.
      const int depth = int((base_sp - sp) / 16);
      if (depth <= max_depth * 4)
        std::printf("%*s%08x(r0=%08x r1=%08x r2=%08x r3=%08x)\n", std::min(depth, 40), "", pc, c.reg(0), c.reg(1), c.reg(2), c.reg(3));
      prev_blink = blink;
    }
  };
  for (int f = 0; f < ticks && done < invocations; f++) m.run_frame();
}
