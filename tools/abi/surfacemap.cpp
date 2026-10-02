// surfacemap: who draws each pixel of a game's drawing surface. Runs a save
// state to the end of a game frame (a framebuffer DMA) and records, for every
// pixel of a 160-pixel-wide 16-bit surface, the call chain (innermost DEPTH
// functions) of the last store to it in that frame. Writes a PNG with one
// colour per chain and lists the chains, in the order they first drew, with
// their colour, store count and visible pixels.
// Usage: surfacemap BIOS CART STATE SURFACE_HEX OUT.png [rows=160] [frames=2] [depth=3]
//   SURFACE_HEX: the address of the surface's first pixel. FRAMES: which game
//   frame to map (counted in DMAs from the state; the first may start mid-frame).
// The call chains are rebuilt from call and return instructions, so code that
// returns some other way can be credited to its caller: a chain names where to
// look, not a proof.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "core/arc/cpu.h"
#include "core/image.h"
#include "core/machine.h"
using namespace leap;

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr, "usage: surfacemap BIOS CART STATE SURFACE_HEX OUT.png [rows] [frames] [depth]\n");
    return 1;
  }
  Machine m;
  std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  const u32 base = u32(std::strtoul(argv[4], nullptr, 16));
  const int rows = argc > 6 ? std::atoi(argv[6]) : 160, frames = argc > 7 ? std::atoi(argv[7]) : 2;
  const size_t depth = argc > 8 ? size_t(std::atoi(argv[8])) : 3;
  constexpr int kW = 160;

  // A shadow call stack from call and return instructions (as framecalls).
  auto peek = [&](u32 a) { return m.bus().peek16(a); };
  std::vector<std::pair<u32, u32>> stack;  // (function, return address)
  u32 ret = 0;
  bool pending = false;
  int delay = 0;
  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    const u32 pc = c.pc();
    while (!stack.empty() && pc == stack.back().second) stack.pop_back();
    if (pending && delay == 0) {
      pending = false;
      stack.push_back({pc, ret});
      if (stack.size() > 256) stack.erase(stack.begin());
    } else if (pending) {
      delay--;
    }
    unsigned len;
    const std::string d = arc::disassemble(pc, peek, &len);
    if (d.rfind("bl", 0) == 0 || d.rfind("jl", 0) == 0) {
      const bool ds = d.find(".d ") != std::string::npos || d.rfind("bl_s.d", 0) == 0 || d.rfind("jl_s.d", 0) == 0;
      pending = true;
      delay = ds ? 1 : 0;
      unsigned l2 = 0;
      ret = pc + len + (ds ? (arc::disassemble(pc + len, peek, &l2), l2) : 0);
    }
  };

  std::vector<int> who(size_t(kW) * rows, -1);
  std::map<std::string, int> ids;
  std::vector<std::string> names;
  std::vector<long> stores;
  std::vector<u64> first;
  u64 seq = 0;
  m.bus().watch = [&](u32 a, u32, int n, bool write) {
    if (!write || a < base || a >= base + u32(kW * rows * 2)) return;
    std::string k;
    for (size_t i = stack.size() > depth ? stack.size() - depth : 0; i < stack.size(); i++) {
      char b[12];
      std::snprintf(b, sizeof b, "%08x ", stack[i].first);
      k += b;
    }
    auto it = ids.find(k);
    const int id = it != ids.end() ? it->second : int(names.size());
    if (it == ids.end()) { ids[k] = id; names.push_back(k); stores.push_back(0); first.push_back(seq); }
    for (int b = 0; b < n; b += 2)
      if (const u32 p = (a + u32(b) - base) / 2; p < who.size()) who[p] = id;
    stores[size_t(id)]++;
    seq++;
  };

  // Map the frame ending at the FRAMES-th DMA (forgetting earlier frames).
  u64 dmas = m.dma_count();
  for (int seen = 0; seen < frames;) {
    m.run_frame();
    if (m.dma_count() == dmas) continue;
    dmas = m.dma_count();
    if (++seen < frames) { std::fill(who.begin(), who.end(), -1); std::fill(stores.begin(), stores.end(), 0); }
  }

  static const u32 kColours[] = {0xffe6194b, 0xff3cb44b, 0xffffe119, 0xff4363d8, 0xfff58231, 0xff911eb4,
                                 0xff46f0f0, 0xfff032e6, 0xffbcf60c, 0xfffabebe, 0xff008080, 0xffe6beff,
                                 0xff9a6324, 0xfffffac8, 0xff800000, 0xffaaffc3, 0xff808000, 0xffffd8b1};
  constexpr size_t kNumColours = sizeof(kColours) / sizeof(kColours[0]);
  std::vector<int> order;
  for (size_t i = 0; i < names.size(); i++) if (stores[i]) order.push_back(int(i));
  std::sort(order.begin(), order.end(), [&](int x, int y) { return first[size_t(x)] < first[size_t(y)]; });
  std::vector<u32> colour(names.size(), 0xff000000);
  for (size_t i = 0; i < order.size(); i++) colour[size_t(order[i])] = kColours[i % kNumColours];
  std::vector<u32> img(who.size());
  for (size_t i = 0; i < img.size(); i++) img[i] = who[i] < 0 ? 0xff000000 : colour[size_t(who[i])];
  write_png(argv[5], img.data(), kW, rows);
  for (int i : order) {
    const long px = std::count(who.begin(), who.end(), i);
    std::printf("#%06x  %s stores %6ld  visible px %6ld\n", colour[size_t(i)] & 0xffffff, names[size_t(i)].c_str(),
                stores[size_t(i)], px);
  }
  return 0;
}
