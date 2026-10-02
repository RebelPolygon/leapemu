// regionmap: which code writes a screen rectangle. Counts, per writing PC,
// stores into the framebuffer DMA source (12-bit packed, 240 bytes a row)
// that land in the rectangle x0,y0-x1,y1 (screen pixels, exclusive end).
// Usage: regionmap BIOS CART STATE FRAMES X0 Y0 X1 Y1
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 9) { std::fprintf(stderr, "usage: regionmap BIOS CART STATE FRAMES X0 Y0 X1 Y1\n"); return 1; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  const int frames = std::atoi(argv[4]), x0 = std::atoi(argv[5]), y0 = std::atoi(argv[6]), x1 = std::atoi(argv[7]), y1 = std::atoi(argv[8]);
  u32 pc = 0, src = 0, fb = 0;
  std::map<u32, u64> by_pc;
  std::map<std::vector<u32>, u64> by_chain;  // callers (innermost function first)
  struct Frame { u32 entry, ret; };
  std::vector<Frame> stack;
  u32 prev_blink = 0;
  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    pc = c.pc();
    while (!stack.empty() && pc == stack.back().ret) stack.pop_back();
    const u32 blink = c.reg(31);
    if (blink != prev_blink) {
      if (stack.size() > 64) stack.erase(stack.begin());
      stack.push_back({pc, blink});
      prev_blink = blink;
    }
  };
  m.bus().watch = [&](u32 a, u32 v, int size, bool w) {
    if (!w) return;
    if (a == 0x01808804) src = v;
    else if (a == 0x01808800 && v == 0x1b) fb = src;
    else if (fb && a >= fb && a < fb + 38400) {
      for (int k = 0; k < size; k++) {
        const u32 o = a + u32(k) - fb;
        const int y = int(o / 240), x = int((o % 240) * 2 / 3);
        if (x >= x0 && x < x1 && y >= y0 && y < y1) {
          by_pc[pc]++;
          std::vector<u32> chain;
          for (size_t i = stack.size(); i-- > 0 && chain.size() < (std::getenv("CHAIN") ? size_t(std::atoi(std::getenv("CHAIN"))) : 6);) chain.push_back(stack[i].entry);
          by_chain[chain]++;
          break;
        }
      }
    }
  };
  for (int f = 0; f < frames; f++) m.run_frame();
  std::vector<std::pair<u64, u32>> top;
  u64 total = 0;
  for (auto& [p, n] : by_pc) { top.push_back({n, p}); total += n; }
  std::sort(top.rbegin(), top.rend());
  std::printf("framebuffer %08x: %llu stores in the rectangle\n", fb, static_cast<unsigned long long>(total));
  for (size_t i = 0; i < std::min<size_t>(top.size(), 25); i++) std::printf("  %08x %5.1f%%\n", top[i].second, 100.0 * top[i].first / total);
  std::vector<std::pair<u64, std::vector<u32>>> chains;
  for (auto& [ch, n] : by_chain) chains.push_back({n, ch});
  std::sort(chains.rbegin(), chains.rend());
  std::printf("call chains (innermost first):\n");
  for (size_t i = 0; i < std::min<size_t>(chains.size(), 12); i++) {
    std::printf("  %5.1f%%", 100.0 * chains[i].first / total);
    for (u32 f : chains[i].second) std::printf(" %08x", f);
    std::printf("\n");
  }
}
