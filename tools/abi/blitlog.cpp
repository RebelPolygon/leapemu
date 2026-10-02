// blitlog: logs calls to a blit routine Blit(ctx, x | y << 16, bitmap*, clip*)
// with the bitmap header fields, grouped by framebuffer DMA (= game frame).
// Usage: blitlog BIOS CART STATE ENTRY_HEX [ticks]
#include <cstdio>
#include <cstdlib>
#include <string>
#include "core/machine.h"
using namespace leap;
int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: blitlog BIOS CART STATE ENTRY [ticks]\n"); return 1; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  const u32 entry = u32(std::strtoul(argv[4], nullptr, 16));
  const int ticks = argc > 5 ? std::atoi(argv[5]) : 12;
  auto& b = m.bus();
  u64 last_dma = m.dma_count();
  int tick = 0;
  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    if (m.dma_count() != last_dma) { last_dma = m.dma_count(); std::printf("---- DMA (tick %d)\n", tick); }
    if (c.pc() != entry) return;
    const u32 pos = c.reg(1), bm = c.reg(2), clip = c.reg(3);
    std::printf("blit from %08x ctx %08x at (%4d,%4d) bm %08x flags %02x bpp %2u %3ux%-3u data %08x clip (%d,%d)-(%d,%d)\n",
                c.reg(31), c.reg(0), int(s16(pos & 0xffff)), int(s16(pos >> 16)), bm, b.peek8(bm), b.peek8(bm + 1),
                b.peek16(bm + 2), b.peek16(bm + 4), b.peek32(bm + 0xc), s16(b.peek16(clip)), s16(b.peek16(clip + 2)),
                s16(b.peek16(clip + 4)), s16(b.peek16(clip + 6)));
  };
  for (tick = 0; tick < ticks; tick++) m.run_frame();
}
