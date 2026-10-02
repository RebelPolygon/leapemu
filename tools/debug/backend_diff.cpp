// Finds the first instruction where the cached interpreter diverges from the
// reference interpreter. Usage: backend_diff BIOS CART NVRAM_DIR [frames]
#include <cstdio>
#include <cstring>
#include <string>

#include "core/arc/cpu.h"
#include "core/machine.h"

using namespace leap;
using B = arc::Cpu::Backend;

static bool same(Machine& a, Machine& b) {
  auto& x = a.cpu(); auto& y = b.cpu();
  if (x.pc() != y.pc() || x.status32() != y.status32() || x.cycles() != y.cycles()) return false;
  for (unsigned r = 0; r < 64; r++) if (r != 62 && r != 63 && x.reg(r) != y.reg(r)) return false;
  return true;
}

static void dump(Machine& a, Machine& b) {
  auto& x = a.cpu(); auto& y = b.cpu();
  std::printf("  interp: pc=%08x st=%08x cyc=%llu   cached: pc=%08x st=%08x cyc=%llu\n", x.pc(), x.status32(),
              (unsigned long long)x.cycles(), y.pc(), y.status32(), (unsigned long long)y.cycles());
  for (unsigned r = 0; r < 62; r++)
    if (x.reg(r) != y.reg(r)) std::printf("  r%-2u interp=%08x cached=%08x\n", r, x.reg(r), y.reg(r));
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: backend_diff BIOS CART NVRAM_DIR [frames]\n"); return 2; }
  const int frames = argc > 4 ? std::atoi(argv[4]) : 600;
  Machine a, b;
  std::string err;
  for (Machine* m : {&a, &b}) {
    m->load_bios(argv[1], &err); m->load_cart(argv[2], &err); m->reset(); m->load_nvram(argv[3]);
  }
  for (Machine* m : {&a, &b}) {  // exercise the timing model too
    Machine::Timing& t = m->timing;
    t.rom16 = 2; t.rom32 = 4; t.cart16 = 1.25f; t.cart32 = 2.5f; t.ram16 = 1.375f; t.ram32 = 2.625f;
    t.sram16 = t.sram32 = 0; t.io16 = t.io32 = 3;
    if (argc > 5 && std::string(argv[5]) == "cache") {  // cache model: 8 KiB I, 4 KiB D
      m->timing.icache_kb = 8; m->timing.dcache_kb = 4;
      m->timing.rom_fill16 = 5.5f; m->timing.ram_fill16 = 1.25f;
    }
    m->apply_timing();
  }
  a.cpu().set_backend(B::Interpreter);
  b.cpu().set_backend(B::CachedInterpreter);
  std::vector<u8> start;
  int f = 0;
  for (; f < frames; f++) {
    start = a.save_state();
    a.run_frame(); b.run_frame();
    if (!same(a, b) || a.save_state() != b.save_state()) break;
  }
  if (f == frames) { std::printf("identical for %d frames\n", frames); return 0; }
  std::printf("first divergent frame: %d\n", f);
  // Binary search the smallest cycle count N (single run_cycles call from the
  // frame start) after which the two differ.
  u64 lo = 0, hi = Machine::kCpuHz / Machine::kFps;
  while (hi - lo > 1) {
    const u64 mid = (lo + hi) / 2;
    a.load_state(start, &err); b.load_state(start, &err);
    a.run_cycles(mid); b.run_cycles(mid);
    if (same(a, b) && a.save_state() == b.save_state()) lo = mid; else hi = mid;
  }
  a.load_state(start, &err); b.load_state(start, &err);
  a.run_cycles(lo); b.run_cycles(lo);
  auto fetch = [&](u32 x) { return a.bus().peek16(x); };
  unsigned len;
  std::printf("identical after %llu cycles; next instruction %08x: %s\n", (unsigned long long)lo, a.cpu().pc(),
              arc::disassemble(a.cpu().pc(), fetch, &len).c_str());
  a.load_state(start, &err); b.load_state(start, &err);
  a.run_cycles(hi); b.run_cycles(hi);
  std::printf("after %llu cycles:\n", (unsigned long long)hi);
  dump(a, b);
  if (a.save_state() != b.save_state() && same(a, b)) std::printf("  (registers equal; memory/device state differs)\n");
  return 1;
}
