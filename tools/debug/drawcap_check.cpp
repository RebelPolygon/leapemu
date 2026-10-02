// drawcap_check: runs a save state with native draw capture enabled and
// reports, per captured game frame, how many screen pixels the replayed draw
// list fails to reproduce (0 = exact). Holds right and jumps once a second.
// Usage: drawcap_check BIOS CART STATE TICKS [dump.png]
//   dump.png: screen | replay | residual (magenta) of the first frame with
//   a residual, if any.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "core/image.h"
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: drawcap_check BIOS CART STATE TICKS [dump.png]\n"); return 2; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  m.reset();
  m.set_native_capture(true);
  if (!m.draw_capture().installed()) { std::printf("no supported engine in this cartridge\n"); return 1; }
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  const int ticks = std::atoi(argv[4]);
  std::shared_ptr<const NativeFrame> last;  // (held: a freed frame's address can be reused)
  int frames = 0, exact = 0; bool dumped = false;
  for (int t = 0; t < ticks; t++) {
    m.set_buttons(kBtnRight | ((t % 60) < 6 ? u32(kBtnA) : 0u));
    m.run_frame();
    auto f = m.draw_capture().latest();
    if (!f || f == last) continue;
    last = f;
    if (frames++ == 0) continue;  // the state was saved mid-frame
    if (f->residual_count == 0) { exact++; continue; }
    std::printf("frame %llu: %zu planes, %zu sprites, %u unexplained pixels\n", (unsigned long long)f->frame_index,
                f->planes.size(), f->sprites.size(), f->residual_count);
    if (argc > 5 && !dumped) {
      dumped = true;
      std::vector<u16> c(160 * 160);
      f->compose(c.data());
      std::vector<u32> img(160 * 480);
      for (int y = 0; y < 160; y++)
        for (int x = 0; x < 160; x++) {
          img[y * 480 + x] = rgb12_to_argb(f->screen[y * 160 + x]);
          img[y * 480 + 160 + x] = rgb12_to_argb(c[y * 160 + x]);
          img[y * 480 + 320 + x] = f->residual[y * 160 + x] ? 0xffff00ffu : 0xff000000u;
        }
      write_png(argv[5], img.data(), 480, 160);
    }
  }
  std::printf("%s: %d of %d frames reproduced exactly\n", m.draw_capture().engine().c_str(), exact, frames - 1);
  return exact == frames - 1 ? 0 : 1;
}
