// interp_timing: how evenly frame interpolation moves over time. Runs the
// emulator from a state like the GUI does (FrameInterpolator in Native mode,
// fed after every emulated frame) and samples the displayed image `sub` times
// per emulated frame, as a faster display would. Motion should change
// gradually from sample to sample; a pop (a sample that changes much more than
// its neighbours) or a stall shows as uneven change.
// Usage: interp_timing BIOS CART STATE TICKS [sub] [dump_prefix]
// Prints per-sample change (DETAIL=1), then: pops, and the unevenness (sum of
// changes in change / sum of change; 0 = perfectly even).
// dump_prefix: writes consecutive samples around each pop as a strip.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "core/image.h"
#include "core/interp.h"
#include "core/machine.h"
using namespace leap;

static double diff(const std::vector<u32>& a, const std::vector<u32>& b) {
  double s = 0;
  for (size_t i = 0; i < a.size(); i++)
    for (int sh = 0; sh < 24; sh += 8) s += std::abs(int((a[i] >> sh) & 255) - int((b[i] >> sh) & 255));
  return s / (a.size() * 3.0);
}

int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: interp_timing BIOS CART STATE TICKS [sub] [dump_prefix]\n"); return 2; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  m.set_native_capture(true);
  const int ticks = std::atoi(argv[4]), sub = argc > 5 ? std::max(1, std::atoi(argv[5])) : 4;
  const char* dump = argc > 6 ? argv[6] : nullptr;
  const bool detail = std::getenv("DETAIL") != nullptr;
  FrameInterpolator ip;
  ip.set_mode(FrameInterpolator::Mode::Native);
  ip.set_rom(m.rom_view());

  std::vector<double> d;                  // change per sample
  std::vector<std::vector<u32>> frames;   // samples (kept for dumps)
  std::vector<u32> prev, cur;
  std::vector<NativeLayer> layers;
  // SCALE=k: also time the redraw at k x (as the GUI draws for its window).
  const int timed = std::getenv("SCALE") ? std::atoi(std::getenv("SCALE")) : 0;
  std::vector<double> ms;
  // GOLD=file (with SCALE): saves every timed redraw to `file`, or, if it
  // exists, compares with it (renderer changes that must not change output).
  const char* gold = std::getenv("GOLD");
  FILE* gf = nullptr;
  bool compare = false;
  if (gold && timed >= 1) {
    gf = std::fopen(gold, "rb");
    compare = gf != nullptr;
    if (!gf) gf = std::fopen(gold, "wb");
  }
  long long differing = 0;
  int worst = 0;
  std::vector<u32> ref, flat;
  for (int t = 0; t < ticks; t++) {
    m.run_frame();
    ip.push(m.framebuffer(), m.frame_count(), m.draw_capture().latest());
    for (int s = 0; s < sub; s++) {
      const double time = double(m.frame_count()) + double(s) / sub;
      if (timed >= 1) {
        const auto t0 = std::chrono::steady_clock::now();
        ip.layers(time, timed, layers);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (gf) {
          composite_layers(layers, timed, flat);
          if (!compare) std::fwrite(flat.data(), 4, flat.size(), gf);
          else {
            ref.resize(flat.size());
            if (std::fread(ref.data(), 4, ref.size(), gf) != ref.size()) { std::fprintf(stderr, "gold file too short\n"); return 1; }
            for (size_t i = 0; i < flat.size(); i++) {
              int dm = 0;
              for (int sh = 0; sh < 24; sh += 8) dm = std::max(dm, std::abs(int((flat[i] >> sh) & 255) - int((ref[i] >> sh) & 255)));
              if (dm) { differing++; worst = std::max(worst, dm); }
            }
          }
        }
      }
      if (!ip.layers(time, 1, layers)) continue;
      composite_layers(layers, 1, cur);
      if (!prev.empty()) {
        d.push_back(diff(prev, cur));
        if (dump) frames.push_back(cur);
        if (detail) std::printf("tick %4d.%d  change %6.3f\n", t, s, d.back());
      }
      prev = cur;
    }
  }
  // A pop: a sample changing over 3x the median change of its neighbourhood
  // (while things move there).
  int pops = 0, dumped = 0;
  double tv = 0, sum = 0;
  for (size_t i = 0; i < d.size(); i++) {
    sum += d[i];
    if (i) tv += std::abs(d[i] - d[i - 1]);
    std::vector<double> w;
    for (size_t j = i >= 12 ? i - 12 : 0; j < std::min(d.size(), i + 13); j++)
      if (j != i) w.push_back(d[j]);
    std::nth_element(w.begin(), w.begin() + w.size() / 2, w.end());
    const double med = w.empty() ? 0 : w[w.size() / 2];
    if (d[i] > 0.4 && med > 0.02 && d[i] > 3 * med) {
      pops++;
      std::printf("pop at sample %zu (tick %zu): change %.3f, neighbourhood %.3f\n", i, i / size_t(sub), d[i], med);
      if (dump && dumped < 30 && i >= 3 && i + 3 < frames.size()) {
        const int W = FrameInterpolator::kW, n = 6;
        std::vector<u32> strip(size_t(W) * n * W);
        for (int k = 0; k < n; k++)
          for (int y = 0; y < W; y++)
            for (int x = 0; x < W; x++) strip[size_t(y) * n * W + size_t(k) * W + x] = frames[i - 3 + size_t(k)][size_t(y) * W + x];
        char name[512];
        std::snprintf(name, sizeof(name), "%s-%05zu.png", dump, i);
        write_png(name, strip.data(), n * W, W);
        dumped++;
      }
    }
  }
  if (compare) std::printf("vs gold: %lld pixels differ, by at most %d\n", differing, worst);
  if (gf) std::fclose(gf);
  if (!ms.empty()) {
    std::sort(ms.begin(), ms.end());
    std::printf("redraw at %dx: median %.2f ms, 90%% %.2f ms, 99%% %.2f ms, max %.2f ms\n", timed, ms[ms.size() / 2],
                ms[ms.size() * 9 / 10], ms[ms.size() * 99 / 100], ms.back());
  }
  std::printf("samples %zu, pops %d, unevenness %.3f\n", d.size(), pops, sum > 0 ? tv / sum : 0.0);
}
