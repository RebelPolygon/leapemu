#pragma once

// The ARC XY-memory DSP option, as the Leapster's CPU has it: two local
// memories (X and Y) read and written through core registers 32-55 with
// address generators, a burst DMA between them and system memory, and dual
// 16x16 multiply-accumulate instructions.
//
// Register and instruction names are from the GNU binutils ARC tables. How
// they behave is not publicly documented; this model is worked out from how
// games use them (the audio mixer of Schoolhouse Rock and 21 other titles
// share) and checked by the sound it produces. See docs/hardware.md.
//
//   Core registers:  r32-r47  xN_u0 xN_u1 ... yN_u0 yN_u1 (N = 0-3): the word at
//                    pointer AXN / AYN, which then moves by its modifier 0 or 1
//                    r48-r55  x0_nu ... y3_nu: the same without moving
//   Aux registers:   0x80-0x87 AX0-AX3, AY0-AY3 (pointers)
//                    0x88-0x97 MX00, MX01, ... MY31 (modifiers)
//                    0x98 XYCONFIG (bit 4: burst busy), 0x99 BURSTSYS (system
//                    address), 0x9a BURSTXYM (XY word address), 0x9b BURSTSZ
//                    (bytes - 1; bit 30: system to XY; bit 29: Y memory rather
//                    than X), 0x41 MACMODE
//   A modifier:      bits 15-0 signed step; bits 27-16 wrap-around length (in
//                    the pointer's units, 0: none); bit 29: 16-bit data (the
//                    pointer counts halfwords, the value is in the upper 16
//                    bits).
//                    A wrap-around buffer starts at the pointer rounded down to
//                    a power of two at least as long.

#include <array>

#include "core/common.h"

namespace leap {

class Bus;

namespace arc {

class XyUnit {
 public:
  static constexpr u32 kWords = 0x1000;  // per memory (16 KiB); the known uses fit in 1 KiB

  static bool is_aux(u32 reg) { return reg == 0x41 || (reg >= 0x80 && reg <= 0x9e); }
  static bool is_port(unsigned r) { return r >= 32 && r <= 55; }

  u32 aux_read(u32 reg) const;
  void aux_write(u32 reg, u32 v, Bus& bus);  // (a burst runs at once)

  u32 read_port(unsigned r);
  void write_port(unsigned r, u32 v);

  // Dual 16x16 multiply-accumulate (major 5 sub-opcodes 0x0c muldw, 0x10
  // macdw, 0x14 msubdw). acc_hi / acc_lo are the accumulators (ACC1 = r56,
  // ACC2 = r57). Returns the packed 16-bit results.
  u32 dual_mac(unsigned sub, u32 b, u32 c, u32& acc_hi, u32& acc_lo) const;

  bool used() const { return used_; }
  // (For debugging tools.)
  u32 x(u32 w) const { return x_[w & (kWords - 1)]; }
  u32 y(u32 w) const { return y_[w & (kWords - 1)]; }

  template <class Ar>
  void serialize(Ar& ar) {
    ar.io(used_);
    for (u32& v : x_) ar.io(v);
    for (u32& v : y_) ar.io(v);
    for (u32& v : ax_) ar.io(v);
    for (u32& v : ay_) ar.io(v);
    for (auto& m : mx_) for (u32& v : m) ar.io(v);
    for (auto& m : my_) for (u32& v : m) ar.io(v);
    ar.io(burst_sys_); ar.io(burst_xym_); ar.io(burst_sz_); ar.io(macmode_); ar.io(xyconfig_);
  }

 private:
  u32 access(bool y, unsigned n, int mod, bool write, u32 v);

  bool used_ = false;
  std::array<u32, kWords> x_{}, y_{};
  u32 ax_[4]{}, ay_[4]{};
  u32 mx_[4][2]{}, my_[4][2]{};
  u32 burst_sys_ = 0, burst_xym_ = 0, burst_sz_ = 0, macmode_ = 0, xyconfig_ = 0;
};

}  // namespace arc
}  // namespace leap
