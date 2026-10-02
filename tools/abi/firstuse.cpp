// firstuse: stop at the first time cartridge code reads memory in [LO,HI) and
// print the preceding cartridge instructions with register state, to learn how
// the cartridge computed that address. Usage: firstuse BIOS CART NVRAM LO HI [N]
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include "core/arc/cpu.h"
#include "core/machine.h"
using namespace leap;
struct Step { u32 pc; u32 r[32]; std::string dis; };
int main(int argc, char** argv) {
  if (argc < 6) { std::fprintf(stderr, "usage: firstuse BIOS CART NVRAM_DIR LO HI [N]\n"); return 2; }
  Machine m; std::string e;
  m.load_bios(argv[1], &e); m.load_cart(argv[2], &e); m.reset(); m.load_nvram(argv[3]);
  const u32 lo = u32(std::strtoul(argv[4], nullptr, 16)), hi = u32(std::strtoul(argv[5], nullptr, 16));
  const size_t keep = argc > 6 ? std::atoi(argv[6]) : 24;
  std::deque<Step> ring;
  bool hit = false;
  u32 hit_addr = 0, hit_val = 0;
  auto fetch = [&](u32 a) { return m.bus().peek16(a); };
  u32 cur = 0;
  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    cur = c.pc();
    if ((cur >> 28) != 8 || hit) return;
    Step s; s.pc = cur; for (int i = 0; i < 32; i++) s.r[i] = c.reg(i);
    unsigned len; s.dis = arc::disassemble(cur, fetch, &len);
    ring.push_back(s); if (ring.size() > keep) ring.pop_front();
  };
  m.bus().watch = [&](u32 a, u32 v, int, bool w) {
    if (!hit && !w && (cur >> 28) == 8 && a >= lo && a < hi) { hit = true; hit_addr = a; hit_val = v; m.cpu().request_exit(); }
  };
  for (int f = 0; f < 60 * 60 && !hit; f++) m.run_frame();
  if (!hit) { std::printf("no cartridge read in range\n"); return 1; }
  std::printf("first cartridge read of %08x (value %08x); preceding cartridge instructions:\n", hit_addr, hit_val);
  for (auto& s : ring)
    std::printf("  %08x  %-30s r0=%08x r1=%08x r2=%08x r13=%08x gp=%08x sp=%08x\n", s.pc, s.dis.c_str(), s.r[0], s.r[1], s.r[2], s.r[13], s.r[26], s.r[28]);
}
