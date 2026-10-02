// swf_play: plays a SWF movie stored in a cartridge with flash::Timeline (its
// control tags only, no ActionScript) and renders frames to PNG: a reference
// for what the movie's frames look like, independent of the Leapster player.
// Usage: swf_play BIOS CART SWF_ADDR FIRST LAST SCALE OUT_PREFIX
#include <cstdio>
#include <cstdlib>
#include <string>
#include "core/flash/render.h"
#include "core/flash/timeline.h"
#include "core/image.h"
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 8) { std::fprintf(stderr, "usage: swf_play BIOS CART SWF_ADDR FIRST LAST SCALE OUT_PREFIX\n"); return 2; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  const u32 addr = u32(std::strtoul(argv[3], nullptr, 16));
  const int first = std::atoi(argv[4]), last = std::atoi(argv[5]), scale = std::atoi(argv[6]);
  const flash::RomView rom = m.rom_view();
  swf::Movie movie;
  if (!movie.load(rom.ptr(addr), rom.avail(addr))) { std::fprintf(stderr, "no SWF at %08x\n", addr); return 2; }
  flash::Timeline tl(movie, addr);
  flash::Renderer r;
  r.set_rom(rom);
  std::vector<u32> img;
  for (int f = 1; f <= last && f <= tl.frame_count(); f++) {
    tl.step();
    if (f < first) continue;
    const flash::Frame fr = tl.snapshot();
    int morphs = 0;
    for (const auto& o : fr.objects) morphs += o.type == flash::Frame::kTypeMorph;
    const bool ok = r.render(fr, nullptr, 1.0, scale, img);
    std::printf("frame %d: %zu objects (%d morph) %s %s\n", f, fr.objects.size(), morphs, ok ? "ok" : "INCOMPLETE", ok ? "" : r.issue().c_str());
    char name[512];
    std::snprintf(name, sizeof(name), "%s-%04d.png", argv[7], f);
    write_png(name, img.data(), 160 * scale, 160 * scale);
  }
}
