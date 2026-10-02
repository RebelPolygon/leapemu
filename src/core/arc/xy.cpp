#include "core/arc/xy.h"

#include <algorithm>

#include "core/bus.h"
#include "core/log.h"

namespace leap::arc {

u32 XyUnit::aux_read(u32 reg) const {
  if (reg >= 0x80 && reg <= 0x83) return ax_[reg - 0x80];
  if (reg >= 0x84 && reg <= 0x87) return ay_[reg - 0x84];
  if (reg >= 0x88 && reg <= 0x8f) return mx_[(reg - 0x88) / 2][(reg - 0x88) % 2];
  if (reg >= 0x90 && reg <= 0x97) return my_[(reg - 0x90) / 2][(reg - 0x90) % 2];
  switch (reg) {
    case 0x41: return macmode_;
    case 0x98: return xyconfig_;  // (bursts finish at once: never busy)
    case 0x99: return burst_sys_;
    case 0x9a: return burst_xym_;
    case 0x9b: return burst_sz_;
    default: return 0;
  }
}

void XyUnit::aux_write(u32 reg, u32 v, Bus& bus) {
  used_ = true;
  if (reg >= 0x80 && reg <= 0x83) { ax_[reg - 0x80] = v; return; }
  if (reg >= 0x84 && reg <= 0x87) { ay_[reg - 0x84] = v; return; }
  if (reg >= 0x88 && reg <= 0x8f) { mx_[(reg - 0x88) / 2][(reg - 0x88) % 2] = v; return; }
  if (reg >= 0x90 && reg <= 0x97) { my_[(reg - 0x90) / 2][(reg - 0x90) % 2] = v; return; }
  switch (reg) {
    case 0x41: macmode_ = v; return;
    case 0x98: xyconfig_ = v & ~0x10u; return;
    case 0x99: burst_sys_ = v; return;
    case 0x9a: burst_xym_ = v; return;
    case 0x9b: {
      // A burst: (size + 1) bytes between system memory and XY memory
      // (bit 30: to XY memory; bit 29: Y rather than X).
      burst_sz_ = v;
      const u32 words = ((v & 0xffff) + 1 + 3) / 4;
      const bool to_xy = v & 0x4000'0000u;
      std::array<u32, kWords>& mem = (v & 0x2000'0000u) ? y_ : x_;  // (bit 29: Y memory)
      for (u32 i = 0; i < words; i++) {
        const u32 w = (burst_xym_ + i) & (kWords - 1), a = burst_sys_ + 4 * i;
        if (to_xy) mem[w] = bus.read32(a);
        else bus.write32(a, mem[w]);
      }
      return;
    }
    default:
      LOG_W("unhandled XY aux write %03x = %08x", reg, v);
  }
}

// One access through pointer N of X or Y, moving it by modifier `mod` (0, 1;
// -1: no move).
u32 XyUnit::access(bool y, unsigned n, int mod, bool write, u32 v) {
  u32& p = y ? ay_[n] : ax_[n];
  const u32 m = mod < 0 ? (y ? my_[n][0] : mx_[n][0]) : (y ? my_[n][mod] : mx_[n][mod]);
  const bool half = m & 0x2000'0000u;
  std::array<u32, kWords>& mem = y ? y_ : x_;
  u32& word = mem[(half ? p >> 1 : p) & (kWords - 1)];
  u32 out = 0;
  if (half) {
    const unsigned sh = (p & 1) ? 16 : 0;  // (an even halfword is the low half, as in memory)
    if (write) word = (word & ~(0xffffu << sh)) | ((v >> 16) << sh);
    else out = ((word >> sh) & 0xffff) << 16;
  } else {
    if (write) word = v;
    else out = word;
  }
  if (mod >= 0) {
    const s32 step = s16(m & 0xffff);
    const u32 len = (m >> 16) & 0xfff;  // (in the pointer's units: halfwords in 16-bit mode)
    if (len == 0) {
      p += u32(step);
    } else {
      u32 k = 1;
      while (k < len) k <<= 1;
      const u32 base = p & ~(k - 1);
      s64 off = s64(p - base) + step;
      off %= s64(len);
      if (off < 0) off += len;
      p = base + u32(off);
    }
  }
  return out;
}

u32 XyUnit::read_port(unsigned r) {
  used_ = true;
  if (r < 48) { const unsigned i = r - 32; return access(i >= 8, (i % 8) / 2, int(i % 2), false, 0); }
  const unsigned i = r - 48;
  return access(i >= 4, i % 4, -1, false, 0);
}

void XyUnit::write_port(unsigned r, u32 v) {
  used_ = true;
  if (r < 48) { const unsigned i = r - 32; access(i >= 8, (i % 8) / 2, int(i % 2), true, v); return; }
  const unsigned i = r - 48;
  access(i >= 4, i % 4, -1, true, v);
}

namespace {

s32 sat32(s64 v) { return s32(std::clamp<s64>(v, INT32_MIN, INT32_MAX)); }

}  // namespace

u32 XyUnit::dual_mac(unsigned sub, u32 b, u32 c, u32& acc_hi, u32& acc_lo) const {
  // Fractional (Q15 x Q15 -> Q31) with saturation: what MACMODE 0x0c selects
  // in the known uses.
  auto lane = [&](s16 x, s16 y, u32& acc) -> u32 {
    const s64 prod = (x == -32768 && y == -32768) ? s64(INT32_MAX) : (s64(x) * y) * 2;
    s64 a = s32(acc);
    if (sub == 0x0c) a = prod;
    else if (sub == 0x10) a += prod;
    else a -= prod;
    acc = u32(sat32(a));
    const s64 r = (s64(s32(acc)) + 0x8000) >> 16;
    return u16(s16(std::clamp<s64>(r, -32768, 32767)));
  };
  const u32 hi = lane(s16(b >> 16), s16(c >> 16), acc_hi);
  const u32 lo = lane(s16(b), s16(c), acc_lo);
  return (hi << 16) | lo;
}

}  // namespace leap::arc
