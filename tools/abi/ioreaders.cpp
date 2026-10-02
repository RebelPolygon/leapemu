// ioreaders: which instructions read (or write) an address, and how often.
// Usage: ioreaders BIOS CART|- ADDR_HEX FRAMES [w]   (CART "-": no cartridge)
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: ioreaders BIOS CART|- ADDR FRAMES [w]\n"); return 1; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || (std::string(argv[2]) != "-" && !m.load_cart(argv[2], &e))) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.reset();
  const u32 addr = u32(std::strtoul(argv[3], nullptr, 16));
  const int frames = std::atoi(argv[4]);
  const bool writes = argc > 5;
  u32 pc = 0;
  std::map<u32, std::pair<u64, u64>> by_pc;  // pc -> (count, first frame)
  m.cpu().trace_hook = [&](const arc::Cpu& c) { pc = c.pc(); };
  m.bus().watch = [&](u32 a, u32, int, bool w) {
    if ((a & ~3u) != (addr & ~3u) || w != writes) return;
    auto& e = by_pc[pc];
    if (!e.first) e.second = m.frame_count();
    e.first++;
  };
  for (int f = 0; f < frames; f++) m.run_frame();
  for (auto& [p, e] : by_pc) std::printf("%08x  %llu times, first at frame %llu\n", p, static_cast<unsigned long long>(e.first), static_cast<unsigned long long>(e.second));
}
