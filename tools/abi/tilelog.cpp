// tilelog: logs tile draws of the Torus engine (TileCmd struct in r0) for the
// game frame(s) completed in the given ticks, summarised by (caller, struct).
// Usage: tilelog BIOS CART STATE ENTRY[,ENTRY...] [ticks] [-v]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: tilelog BIOS CART STATE ENTRY[,ENTRY...] [ticks] [-v]\n"); return 1; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  std::set<u32> pcs;
  for (const char* p = argv[4]; *p;) { pcs.insert(u32(std::strtoul(p, const_cast<char**>(&p), 16))); if (*p == ',') p++; }
  const int ticks = argc > 5 ? std::atoi(argv[5]) : 8;
  const bool verbose = argc > 6 && !std::strcmp(argv[6], "-v");
  auto& b = m.bus();
  struct G { u64 n = 0; int x0 = 99999, y0 = 99999, x1 = -99999, y1 = -99999; u32 fn = 0; std::set<u32> tilesets; };
  std::map<std::tuple<u32, u32>, G> groups;  // (caller, struct) -> summary
  u64 last_dma = m.dma_count(); int frame = 0;
  auto flush = [&]() {
    std::printf("==== game frame %d: %zu groups\n", frame++, groups.size());
    for (auto& [k, g] : groups)
      std::printf("  caller %08x struct %08x fn %08x: %4llu tiles, x %d..%d y %d..%d, tilesets %zu\n", std::get<0>(k), std::get<1>(k), g.fn,
                  (unsigned long long)g.n, g.x0, g.x1, g.y0, g.y1, g.tilesets.size());
    groups.clear();
  };
  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    if (m.dma_count() != last_dma) { last_dma = m.dma_count(); flush(); }
    if (!pcs.count(c.pc())) return;
    const u32 s = c.reg(0);
    const int x = int(b.peek32(s + 8)), y = int(b.peek32(s + 0xc));
    const u32 attr = b.peek16(s + 0x10);
    if (verbose) std::printf("%08x from %08x struct %08x x %4d y %4d attr %04x tileset %08x pal %08x\n", c.pc(), c.reg(31), s, x, y, attr, b.peek32(s), b.peek32(s + 0x14));
    G& g = groups[{c.reg(31), s}];
    g.n++; g.fn = c.pc(); g.x0 = std::min(g.x0, x); g.x1 = std::max(g.x1, x); g.y0 = std::min(g.y0, y); g.y1 = std::max(g.y1, y);
    g.tilesets.insert(b.peek32(s));
  };
  for (int t = 0; t < ticks; t++) m.run_frame();
}
