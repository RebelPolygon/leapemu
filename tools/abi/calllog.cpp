// calllog: logs entries to the given functions (hex PCs, comma-separated) with
// r0-r3 and the return address, in execution order, marking framebuffer DMAs.
// Usage: calllog BIOS CART STATE PC[,PC...] [ticks]
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: calllog BIOS CART STATE PC[,PC...] [ticks]\n"); return 1; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  std::set<u32> pcs;
  for (const char* p = argv[4]; *p;) { pcs.insert(u32(std::strtoul(p, const_cast<char**>(&p), 16))); if (*p == ',') p++; }
  const int ticks = argc > 5 ? std::atoi(argv[5]) : 12;
  u64 last_dma = m.dma_count();
  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    if (m.dma_count() != last_dma) { last_dma = m.dma_count(); std::printf("---- DMA\n"); }
    if (!pcs.count(c.pc())) return;
    std::printf("%08x from %08x  r0 %08x r1 %08x r2 %08x r3 %08x\n", c.pc(), c.reg(31), c.reg(0), c.reg(1), c.reg(2), c.reg(3));
  };
  for (int t = 0; t < ticks; t++) m.run_frame();
}
