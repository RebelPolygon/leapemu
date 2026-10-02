#pragma once

#include <array>
#include <functional>
#include <bit>
#include <cstring>
#include <memory>
#include <vector>

#include "core/common.h"

namespace leap {

namespace arc { class Jit; }

// A device occupying a range of the physical address space with side effects
// on access. Addresses passed in are absolute. `size` is 1, 2 or 4 and the
// address is naturally aligned for that size.
class MmioDevice {
 public:
  virtual ~MmioDevice() = default;
  virtual u32 mmio_read(u32 addr, int size) = 0;
  virtual void mmio_write(u32 addr, u32 value, int size) = 0;
};

// 32-bit little-endian physical address space, split into 64 KiB pages.
// Pages backed by host memory are accessed through direct pointers; anything
// else (MMIO, unmapped, write-ignored ROM) goes through the slow path.
class Bus {
 public:
  static constexpr unsigned kPageBits = 16;
  static constexpr u32 kPageSize = 1u << kPageBits;
  static constexpr u32 kPageMask = kPageSize - 1;
  static constexpr size_t kPageCount = size_t(1) << (32 - kPageBits);

  Bus();

  void clear();

  // Map host memory. `data` must stay valid and cover `size` bytes, where
  // `size` is a multiple of kPageSize. If `mirror_len` > size, the range
  // [base, base+mirror_len) repeats the backing memory.
  void map_memory(u32 base, u32 size, u8* data, bool writable, u32 mirror_len = 0);
  void map_mmio(u32 base, u32 size, MmioDevice* dev);
  // Region that reads as zero and silently drops writes.
  void map_nop(u32 base, u32 size);

#ifdef LEAPEMU_ANALYSIS
  // Analysis builds only (tools/abi): observe every CPU-visible access.
  std::function<void(u32 addr, u32 value, int size, bool write)> watch;
#define LEAP_WATCH(a, v, n, w) \
  do { if (watch) watch(a, v, n, w); } while (0)
#else
#define LEAP_WATCH(a, v, n, w) do {} while (0)
#endif

  u8 read8(u32 a) {
    const Page& p = pages_[a >> kPageBits];
#ifdef LEAPEMU_ANALYSIS
    if (watch) { const u8 v = p.rd ? p.rd[a & kPageMask] : u8(slow_read(a, 1)); LEAP_WATCH(a, v, 1, false); return v; }
#endif
    if (p.rd) return p.rd[a & kPageMask];
    return static_cast<u8>(slow_read(a, 1));
  }
  u16 read16(u32 a) {
    const Page& p = pages_[a >> kPageBits];
#ifdef LEAPEMU_ANALYSIS
    if (watch) { const u16 v = p.rd ? load<u16>(p.rd + (a & kPageMask)) : u16(slow_read(a, 2)); LEAP_WATCH(a, v, 2, false); return v; }
#endif
    if (p.rd) return load<u16>(p.rd + (a & kPageMask));
    return static_cast<u16>(slow_read(a, 2));
  }
  u32 read32(u32 a) {
    const Page& p = pages_[a >> kPageBits];
#ifdef LEAPEMU_ANALYSIS
    if (watch) { const u32 v = p.rd ? load<u32>(p.rd + (a & kPageMask)) : slow_read(a, 4); LEAP_WATCH(a, v, 4, false); return v; }
#endif
    if (p.rd) return load<u32>(p.rd + (a & kPageMask));
    return slow_read(a, 4);
  }
  void write8(u32 a, u8 v) {
    const Page& p = pages_[a >> kPageBits];
    LEAP_WATCH(a, v, 1, true);
    if (p.wr) { p.wr[a & kPageMask] = v; return; }
    slow_write(a, v, 1);
  }
  void write16(u32 a, u16 v) {
    const Page& p = pages_[a >> kPageBits];
    LEAP_WATCH(a, v, 2, true);
    if (p.wr) { store<u16>(p.wr + (a & kPageMask), v); return; }
    slow_write(a, v, 2);
  }
  void write32(u32 a, u32 v) {
    const Page& p = pages_[a >> kPageBits];
    LEAP_WATCH(a, v, 4, true);
    if (p.wr) { store<u32>(p.wr + (a & kPageMask), v); return; }
    slow_write(a, v, 4);
  }

  // Wait states (extra CPU time per access, in 1/8 cycles) for a range of
  // pages. Used by the CPU timing model; call after mapping.
  void set_wait(u32 base, u32 size, u8 wait16, u8 wait32) {
    for (u64 off = 0; off < size; off += kPageSize) {
      Page& p = pages_[(base + off) >> kPageBits];
      p.wait16 = wait16;
      p.wait32 = wait32;
    }
  }
  u8 wait16(u32 a) const { return pages_[a >> kPageBits].wait16; }
  u8 wait32(u32 a) const { return pages_[a >> kPageBits].wait32; }
  // Cacheable memory (ROM, RAM) and the cost of one 16-bit transfer during a
  // cache-line fill, in 1/8 cycles. Used when the CPU cache model is enabled.
  void set_cacheable(u32 base, u32 size, bool cacheable, u8 fill16) {
    for (u64 off = 0; off < size; off += kPageSize) {
      Page& p = pages_[(base + off) >> kPageBits];
      p.cacheable = cacheable;
      p.fill16 = fill16;
    }
  }
  bool cacheable(u32 a) const { return pages_[a >> kPageBits].cacheable; }
  u8 fill16(u32 a) const { return pages_[a >> kPageBits].fill16; }

  // Side-effect-free read for debuggers/disassemblers. MMIO reads as 0.
  u8 peek8(u32 a) const {
    const Page& p = pages_[a >> kPageBits];
    return p.rd ? p.rd[a & kPageMask] : 0;
  }
  u16 peek16(u32 a) const { return u16(peek8(a) | (peek8(a + 1) << 8)); }
  u32 peek32(u32 a) const { return u32(peek16(a)) | (u32(peek16(a + 2)) << 16); }

  // True if the address is backed by read-only memory (BIOS / cartridge ROM),
  // whose contents can be cached (e.g. decoded instructions).
  bool is_rom(u32 a) const {
    const Page& p = pages_[a >> kPageBits];
    return p.rd && !p.wr && !p.dev;
  }

  // Host pointer for a physical address if it is memory-backed, else nullptr.
  u8* host_ptr(u32 a) const {
    const Page& p = pages_[a >> kPageBits];
    return p.rd ? p.rd + (a & kPageMask) : nullptr;
  }

  // Code in writable memory: a page of RAM whose instructions the CPU caches.
  // Its writes leave the fast path (stores then reach slow_write, compiled ones
  // too), and each one is reported to the hook after it is made.
  using CodeHook = void (*)(void* ctx, u32 addr, int size);
  void set_code_hook(CodeHook fn, void* ctx) { code_hook_ = fn; code_ctx_ = ctx; }
  bool watch_code(u32 a) {  // false: not plain writable memory
    Page& p = pages_[a >> kPageBits];
    if (p.code) return true;
    if (!p.rd || p.wr != p.rd || p.dev || !code_hook_) return false;
    p.code = true;
    p.wr = nullptr;
    watched_.push_back(a >> kPageBits);
    return true;
  }
  void unwatch_code() {
    for (u32 i : watched_) {
      Page& p = pages_[i];
      if (p.code) { p.code = false; p.wr = p.rd; }
    }
    watched_.clear();
  }
  bool code_watched() const { return !watched_.empty(); }

  // Count of distinct unmapped accesses reported (to rate-limit logging).
  u64 unmapped_accesses() const { return unmapped_count_; }

 private:
  friend class arc::Jit;  // walks the page table from compiled code
  struct Page {
    u8* rd = nullptr;
    u8* wr = nullptr;
    MmioDevice* dev = nullptr;
    bool nop = false;
    u8 wait16 = 0;  // extra CPU time per 8/16-bit access, in 1/8 cycles
    u8 wait32 = 0;  // extra CPU time per 32-bit access, in 1/8 cycles
    bool cacheable = false;
    u8 fill16 = 0;  // cache-line fill cost per 16-bit transfer, in 1/8 cycles
    bool code = false;  // writable memory being watched for code (watch_code)
  };

  template <typename T>
  static T bswap(T v) {
    if constexpr (sizeof(T) == 2) return T((v >> 8) | (v << 8));
    else return T((v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24));
  }
  template <typename T>
  static T load(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) v = bswap(v);
    return v;
  }
  template <typename T>
  static void store(u8* p, T v) {
    if constexpr (std::endian::native == std::endian::big) v = bswap(v);
    std::memcpy(p, &v, sizeof(T));
  }

  u32 slow_read(u32 a, int size);
  void slow_write(u32 a, u32 v, int size);

  std::vector<Page> pages_;
  u64 unmapped_count_ = 0;
  CodeHook code_hook_ = nullptr;
  void* code_ctx_ = nullptr;
  std::vector<u32> watched_;
};

}  // namespace leap
