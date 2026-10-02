#include "core/bus.h"

#include "core/log.h"

namespace leap {

Bus::Bus() : pages_(kPageCount) {}

void Bus::clear() {
  std::fill(pages_.begin(), pages_.end(), Page{});
  watched_.clear();
  unmapped_count_ = 0;
}

void Bus::map_memory(u32 base, u32 size, u8* data, bool writable, u32 mirror_len) {
  if (mirror_len < size) mirror_len = size;
  for (u64 off = 0; off < mirror_len; off += kPageSize) {
    Page& p = pages_[(base + off) >> kPageBits];
    p = Page{};
    p.rd = data + (off % size);
    p.wr = writable ? p.rd : nullptr;
    // Read-only pages silently drop writes (ROM behaviour).
    p.nop = !writable;
  }
}

void Bus::map_mmio(u32 base, u32 size, MmioDevice* dev) {
  for (u64 off = 0; off < size; off += kPageSize) {
    Page& p = pages_[(base + off) >> kPageBits];
    p = Page{};
    p.dev = dev;
  }
}

void Bus::map_nop(u32 base, u32 size) {
  for (u64 off = 0; off < size; off += kPageSize) {
    Page& p = pages_[(base + off) >> kPageBits];
    p = Page{};
    p.nop = true;
  }
}

u32 Bus::slow_read(u32 a, int size) {
  const Page& p = pages_[a >> kPageBits];
  if (p.dev) return p.dev->mmio_read(a, size);
  if (!p.nop && unmapped_count_++ < 64)
    LOG_W("unmapped read%d @ %08x", size * 8, a);
  return 0;
}

void Bus::slow_write(u32 a, u32 v, int size) {
  const Page& p = pages_[a >> kPageBits];
  if (p.dev) { p.dev->mmio_write(a, v, size); return; }
  if (p.code) {
    u8* m = p.rd + (a & kPageMask);
    if (size == 4) store<u32>(m, v);
    else if (size == 2) store<u16>(m, u16(v));
    else *m = u8(v);
    code_hook_(code_ctx_, a, size);
    return;
  }
  if (!p.nop && unmapped_count_++ < 64)
    LOG_W("unmapped write%d @ %08x = %08x", size * 8, a, v);
}

}  // namespace leap
