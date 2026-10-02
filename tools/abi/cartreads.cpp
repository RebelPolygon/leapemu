// cartreads: records which cartridge ROM addresses BaseROM code reads before
// the first time cartridge code executes (i.e. during validation / boot).
// Black-box: prints addresses and access sizes only.
#include <cstdio>
#include <map>
#include <vector>
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: cartreads BIOS CART NVRAM_DIR\n"); return 2; }
  Machine m; std::string e;
  m.load_bios(argv[1], &e); m.load_cart(argv[2], &e); m.reset(); m.load_nvram(argv[3]);
  u32 cur = 0; bool in_cart = false;
  std::map<u32, int> reads;  // addr -> size
  std::vector<std::pair<u32, int>> order;  // first-read order
  m.bus().watch = [&](u32 a, u32, int size, bool w) {
    if (!w && !in_cart && a >= 0x80000000u && a < 0xc0000000u && (cur >> 28) == 4) { if (!reads.count(a)) order.push_back({a, size}); reads[a] = size; }
  };
  m.cpu().trace_hook = [&](const arc::Cpu& c) { cur = c.pc(); if ((cur >> 28) == 8) in_cart = true; };
  for (int f = 0; f < 60 * 40 && !in_cart; f++) m.run_frame();
  std::printf("%zu distinct cart addresses read by BaseROM before cart code ran\n", reads.size());
  if (argc > 4) {  // chronological mode: collapse consecutive runs
    u32 rs = 0, pv = 0; int ps = 0; bool h = false; int shown = 0;
    for (auto& [a, sz] : order) {
      if (h && a == pv + ps && sz == ps) { pv = a; continue; }
      if (h && ((rs & 0xfff) != 0xf94 || shown < 3) && shown++ < 400) std::printf("  %08x-%08x (%d)\n", rs, pv + ps - 1, ps);
      rs = pv = a; ps = sz; h = true;
    }
    if (h) std::printf("  %08x-%08x (%d)\n", rs, pv + ps - 1, ps);
    return 0;
  }
  // Summarise as runs.
  u32 run_start = 0, prev = 0; int prev_size = 0; bool have = false; int shown = 0;
  auto flush = [&]() { if (have && shown++ < 80) std::printf("  %08x-%08x (%d-byte reads)\n", run_start, prev + prev_size - 1, prev_size); };
  for (auto& [a, s] : reads) {
    if (have && a == prev + prev_size && s == prev_size) { prev = a; continue; }
    flush(); run_start = prev = a; prev_size = s; have = true;
  }
  flush();
}
