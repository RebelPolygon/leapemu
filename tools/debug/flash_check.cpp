// flash_check: redraws the BaseROM Flash player's frames from its display
// list and compares them with the emulated LCD.
// Usage: flash_check BIOS CART STATE|- TICKS [dump_prefix]   ("-": boot from reset)
//   Prints, per new Flash frame, the fraction of pixels that differ clearly
//   from the LCD. With dump_prefix, writes LCD | redraw (1x) | redraw (4x,
//   downscaled view) images for the first frames. FLASH_MOVIE=file.lmv plays
//   an input movie from power-on (STATE "-").
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "core/flash/render.h"
#include "core/image.h"
#include "core/machine.h"
#include "core/movie.h"
using namespace leap;


int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: flash_check BIOS CART STATE TICKS [dump_prefix]\n"); return 2; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  m.reset();
  if (std::string(argv[3]) != "-" && !m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }  // "-": boot
  // FLASH_MOVIE=file.lmv: play an input movie (from power-on, with STATE "-").
  Movie movie;
  const char* movie_path = std::getenv("FLASH_MOVIE");
  if (movie_path && (!movie.load(movie_path, &e) || !movie.start(m, &e))) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  m.set_native_capture(true);  // the emulator's own capture (display list, text fields)
  const flash::RomView rom = m.rom_view();
  flash::Renderer r;
  r.set_rom(rom);
  const int ticks = std::atoi(argv[4]);
  int frames = 0, dumped = 0;
  std::vector<u32> img1, img4;
  std::shared_ptr<const NativeFrame> last;  // (held: a freed frame's address can be reused)
  for (int t = 0; t < ticks; t++) {
    if (movie_path && m.frame_count() < movie.frames.size()) apply_input(m, movie.frames[m.frame_count()]);
    m.run_frame();
    const auto nf = m.draw_capture().latest();
    if (!nf || !nf->flash || nf == last) continue;
    last = nf;
    const flash::Frame& f = *nf->flash;
    const bool ok = r.render(f, nullptr, 1.0, 1, img1);
    const u32* lcd = m.framebuffer();
    int bad = 0;
    for (int i = 0; i < 160 * 160; i++) {
      const u32 a = lcd[i], b = img1[i];
      int d = 0;
      for (int s = 0; s < 24; s += 8) d = std::max(d, std::abs(int((a >> s) & 255) - int((b >> s) & 255)));
      bad += d > 48;
    }
    double worst_field = 0;  // clearly differing pixels in the worst text field
    for (const auto& fr : r.captured_fields()) {
      int fbad = 0, area = 0;
      for (int y = fr[1]; y < fr[3]; y++)
        for (int x = fr[0]; x < fr[2]; x++, area++) {
          const u32 a = lcd[y * 160 + x], b = img1[y * 160 + x];
          int d = 0;
          for (int s = 0; s < 24; s += 8) d = std::max(d, std::abs(int((a >> s) & 255) - int((b >> s) & 255)));
          fbad += d > 48;
        }
      if (area) worst_field = std::max(worst_field, 100.0 * fbad / area);
    }
    if (std::getenv("FLASH_FIELDS"))  // text fields and their captured glyphs
      for (const auto& o : f.objects)
        if (o.type == flash::Frame::kTypeEditText) {
          const auto it = f.fields.find(o.id);
          std::printf("  field obj %08x id %u vis %d: %s", o.id, o.char_id, o.visible, it == f.fields.end() ? "not captured" : "");
          if (it != f.fields.end()) {
            std::printf("%zu glyphs", it->second.glyphs.size());
            for (size_t k = 0; k < std::min<size_t>(20, it->second.glyphs.size()); k++)
              std::printf(" (%d,%d %06x)", it->second.glyphs[k].x, it->second.glyphs[k].y, it->second.glyphs[k].rgb);
          }
          std::printf("\n");
        }
    static bool listed = false;
    bool has_type = false;
    if (const char* want = std::getenv("FLASH_LIST"))
      for (const auto& o : f.objects) has_type |= !*want || o.type == std::atoi(want);
    // (FLASH_LIST_AT=tick: list the first frame from that tick on.)
    if (const char* at = std::getenv("FLASH_LIST_AT"); at && t < std::atoi(at)) has_type = false;
    if (has_type && !listed && (listed = true))
      for (size_t i = 0; i < f.objects.size(); i++) {
        const auto& o = f.objects[i];
        std::printf("  #%zu obj %08x parent %d depth %u clip %u type %u tag %u id %u vis %d cx mul %.2f %.2f %.2f %.2f add %.0f %.0f %.0f %.0f  movie %08x m %.3f %.3f %.3f %.3f %.0f %.0f\n", i, o.id, o.parent,
                    o.depth, o.clip_depth, o.type, o.tag, o.char_id, o.visible, o.local_cx.mul[0], o.local_cx.mul[1], o.local_cx.mul[2],
                    o.local_cx.mul[3], o.local_cx.add[0], o.local_cx.add[1], o.local_cx.add[2], o.local_cx.add[3], o.movie, o.local.a, o.local.b,
                    o.local.c, o.local.d, o.local.tx, o.local.ty);
      }
    frames++;
    std::string other;
    for (const auto& o : f.objects)
      if (o.visible && o.type != 0 && o.type != 2 && o.type != 4 && o.type != 6 && o.type != 98 && o.type != 128) {
        char b[32];
        std::snprintf(b, sizeof(b), " type%u/tag%u", o.type, o.tag);
        if (other.find(b) == std::string::npos) other += b;
      }
    std::printf("tick %d: %zu objects, %s, %.1f%% of pixels differ, worst text field %.1f%%%s\n", t, f.objects.size(),
                ok ? "complete" : "INCOMPLETE", 100.0 * bad / (160 * 160), worst_field, (other + (ok ? "" : "  [" + r.issue() + "]")).c_str());
    const char* over = std::getenv("FLASH_DUMP_OVER");
    const char* from = std::getenv("FLASH_DUMP_FROM");  // first tick to dump
    const bool want = (!from || t >= std::atoi(from)) && (over ? 100.0 * bad / (160 * 160) > std::atof(over) : t % 8 == 0);
    const int dump_max = std::getenv("FLASH_DUMP_MAX") ? std::atoi(std::getenv("FLASH_DUMP_MAX")) : 4;
    if (argc > 5 && dumped < dump_max && want) {
      r.render(f, nullptr, 1.0, 4, img4);
      std::vector<u32> out(size_t(160 * 3 + 640) * 640, 0xff202020u);
      const int W = 160 * 3 + 640;
      for (int y = 0; y < 640; y++)
        for (int x = 0; x < 640; x++) out[size_t(y) * W + 480 + x] = img4[size_t(y) * 640 + x];
      for (int y = 0; y < 160; y++)
        for (int x = 0; x < 160; x++) {
          out[size_t(y) * W + x] = lcd[y * 160 + x];
          out[size_t(y) * W + 160 + x] = img1[size_t(y) * 160 + x];
        }
      char name[512];
      std::snprintf(name, sizeof(name), "%s-%02d.png", argv[5], dumped++);
      write_png(name, out.data(), W, 640);
    }
  }
  std::printf("%d Flash frames\n", frames);
}
