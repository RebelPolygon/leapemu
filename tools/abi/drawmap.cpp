// drawmap: finds the code that draws each frame. Records the framebuffer DMA
// source buffer and counts, per writing PC, stores that land in it (or in VRAM).
// Usage: drawmap BIOS CART STATE [frames]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: drawmap BIOS CART STATE [frames]\n"); return 1; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  const int frames = argc > 4 ? std::atoi(argv[4]) : 120;
  u32 pc = 0, src = 0, stride = 0, lines = 0;
  std::map<u32, u32> dma_srcs;                  // source -> bytes
  std::map<u32, std::map<u32, u64>> writes_by_page;  // 4 KiB page -> pc -> count
  m.cpu().trace_hook = [&](const arc::Cpu& c) { pc = c.pc(); };
  m.bus().watch = [&](u32 a, u32 v, int, bool w) {
    if (!w) return;
    if (a == 0x01808804) src = v;
    else if (a == 0x01808808) stride = v;
    else if (a == 0x0180880c) lines = v;
    else if (a == 0x01808800 && v == 0x1b) dma_srcs[src] = std::max(dma_srcs[src], stride * lines * 4);
    else if ((a >> 24) == 0x3c || (a >> 24) == 0x03) writes_by_page[a >> 12][pc]++;
  };
  for (int f = 0; f < frames; f++) m.run_frame();
  std::printf("framebuffer DMA sources:\n");
  for (auto& [s, n] : dma_srcs) std::printf("  %08x, %u bytes\n", s, n);
  // Writers into the DMA source buffers and VRAM.
  std::map<u32, u64> writers;
  for (auto& [page, pcs] : writes_by_page) {
    const u32 a = page << 12;
    bool hit = (a >> 24) == 0x03;
    for (auto& [s, n] : dma_srcs) hit |= a + 0x1000 > s && a < s + n;
    if (hit) for (auto& [p, c] : pcs) writers[p] += c;
  }
  std::vector<std::pair<u64, u32>> v;
  u64 total = 0;
  for (auto& [p, c] : writers) { v.push_back({c, p}); total += c; }
  std::sort(v.rbegin(), v.rend());
  std::printf("stores into framebuffers: %llu over %d frames; top writers:\n", (unsigned long long)total, frames);
  for (size_t i = 0; i < v.size() && i < 25; i++) std::printf("  %08x  %5.1f%%\n", v[i].second, 100.0 * v[i].first / total);
  // Busiest RAM regions overall (e.g. an off-screen working buffer), with their writers.
  std::map<u32, std::map<u32, u64>> regions;  // 64 KiB region -> pc -> count
  for (auto& [page, pcs] : writes_by_page) for (auto& [p, c] : pcs) regions[page >> 4][p] += c;
  std::vector<std::pair<u64, u32>> r;
  for (auto& [reg, pcs] : regions) { u64 n = 0; for (auto& [p, c] : pcs) n += c; r.push_back({n, reg}); }
  std::sort(r.rbegin(), r.rend());
  for (size_t i = 0; i < r.size() && i < 4; i++) {
    std::printf("region %08x: %llu stores; writers:", r[i].second << 16, (unsigned long long)r[i].first);
    std::vector<std::pair<u64, u32>> w;
    for (auto& [p, c] : regions[r[i].second]) w.push_back({c, p});
    std::sort(w.rbegin(), w.rend());
    for (size_t j = 0; j < w.size() && j < 6; j++) std::printf(" %08x(%.0f%%)", w[j].second, 100.0 * w[j].first / r[i].first);
    std::printf("\n");
  }
}
