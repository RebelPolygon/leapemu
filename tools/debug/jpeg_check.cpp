// jpeg_check: decodes every JPEG bitmap (DefineBits, DefineBitsJPEG2/3) in the
// SWF movies of a cartridge, reporting failures and progressive images.
// Usage: jpeg_check BIOS CART [OUT_PREFIX]   (PNGs of progressive images)
#include <cstdio>
#include <string>
#include "core/flash/swf.h"
#include "core/image.h"
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: jpeg_check BIOS CART [OUT_PREFIX]\n"); return 2; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  const flash::RomView rom = m.rom_view();
  const u32 base = 0x80000000u;
  const u8* img = rom.ptr(base);
  const size_t n = rom.avail(base);
  int total = 0, bad = 0, prog = 0;
  for (size_t o = 0; o + 8 < n; o++) {
    if (img[o] != 'F' || img[o + 1] != 'W' || img[o + 2] != 'S' || img[o + 3] < 3 || img[o + 3] > 10) continue;
    swf::Movie mv;
    if (!mv.load(img + o, n - o)) continue;
    for (const auto& [id, t] : mv.dict) {
      if (t.code != 6 && t.code != 21 && t.code != 35) continue;
      total++;
      // Progressive: an SOF2 marker in the image data.
      bool sof2 = false;
      for (size_t k = 0; k + 1 < t.len; k++)
        if (t.body[k] == 0xff && t.body[k + 1] == 0xc2) { sof2 = true; break; }
      swf::Bitmap b;
      const bool ok = swf::decode_jpeg_bits(t.body, t.len, t.code, mv, b);
      if (!ok) bad++;
      if (sof2) prog++;
      if (!ok || sof2) {
        std::printf("swf %08zx id %u tag %u%s: %s %dx%d\n", base + o, id, t.code, sof2 ? " (progressive)" : "", ok ? "ok" : "FAILED", b.w, b.h);
        if (ok && argc > 3) {
          char name[512];
          std::snprintf(name, sizeof(name), "%s-%08zx-%u.png", argv[3], base + o, id);
          write_png(name, b.px.data(), b.w, b.h);
        }
      }
    }
  }
  std::printf("%d JPEG bitmaps, %d progressive, %d failed\n", total, prog, bad);
  return bad ? 1 : 0;
}
