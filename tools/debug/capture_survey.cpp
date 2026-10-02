// capture_survey: boots a cartridge with scripted input and reports how the
// frames it shows are covered by native draw capture: redrawn from the Flash
// player's display list, from the tile-plane engine, or not at all (those
// fall back to pixel-motion smoothing). Uncovered frames can be dumped.
// Usage: capture_survey BIOS CART FRAMES [dump_prefix]
// SAVE_AT=frame SAVE_TO=file saves a state on the way (the scripted input
// reaches some games' play).
#include <cstdio>
#include <cstdlib>
#include <string>
#include "core/image.h"
#include "core/machine.h"
#include "core/movie.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: capture_survey BIOS CART FRAMES [dump_prefix]\n"); return 2; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  m.reset();
  m.set_native_capture(true);
  const int frames = std::atoi(argv[3]);
  std::vector<u32> prev(160 * 160, 0);
  int changed = 0, flash = 0, tile = 0, none = 0, dumped = 0;
  std::shared_ptr<const NativeFrame> last;  // (held: a freed frame's address can be reused)
  u32 rng = 12345;
  for (int f = 0; f < frames; f++) {
    // Scripted input: tap somewhere every 2.5 s, press A or a direction now and then.
    InputFrame in;
    rng = rng * 1103515245 + 12345;
    if (f % 150 < 5) { in.touch = true; in.x = u8(40 + (f / 150 * 37) % 80); in.y = u8(40 + (f / 150 * 53) % 80); }
    if (f % 90 >= 45 && f % 90 < 48) in.buttons |= kBtnA;
    const u32 dirs[] = {kBtnRight, kBtnLeft, kBtnUp, kBtnDown};
    if (f % 240 >= 120 && f % 240 < 160) in.buttons |= dirs[(f / 240) % 4];
    apply_input(m, in);
    if (!m.run_frame()) break;
    if (const char* at = std::getenv("SAVE_AT"); at && f + 1 == std::atoi(at)) {  // SAVE_AT=frame SAVE_TO=file
      const char* to = std::getenv("SAVE_TO");
      if (!to) std::fprintf(stderr, "SAVE_AT needs SAVE_TO=file\n");
      else if (!m.save_state_file(to, &e)) std::fprintf(stderr, "%s\n", e.c_str());
    }
    const u32* fb = m.framebuffer();
    int diff = 0;
    for (int i = 0; i < 160 * 160; i++) diff += fb[i] != prev[i];
    std::copy(fb, fb + 160 * 160, prev.begin());
    const auto nf = m.draw_capture().latest();
    const bool fresh = nf && nf != last;
    if (nf) last = nf;
    if (diff < 20) continue;  // (a static screen, or a few pixels of blinking)
    changed++;
    if (fresh && nf->flash) flash++;
    else if (fresh && !nf->planes.empty()) tile++;
    else {
      none++;
      if (argc > 4 && dumped < 6 && none % 60 == 1) {
        char name[512];
        std::snprintf(name, sizeof(name), "%s-%05d.png", argv[4], f);
        write_png(name, fb, 160, 160);
        dumped++;
      }
    }
  }
  std::printf("%d changing frames: flash %d, tile engine %d, uncovered %d (%.0f%%)\n", changed, flash, tile, none,
              changed ? 100.0 * none / changed : 0.0);
}
