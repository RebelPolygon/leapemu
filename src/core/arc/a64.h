#pragma once

// A minimal AArch64 machine-code emitter for the JIT (core/arc/jit_a64.cpp):
// only the instructions it uses. Encodings follow the Arm Architecture
// Reference Manual for A-profile (A64 instruction set). Registers are
// numbered 0-30; 31 is the zero register or the stack pointer, as the
// instruction defines.

#include <cstring>
#include <vector>

#include "core/common.h"

namespace leap::arc::a64 {

using Reg = unsigned;
constexpr Reg ZR = 31, SP = 31;

// Condition codes.
enum Cond : u32 { EQ, NE, HS, LO, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };

struct Label {
  int pos = -1;
  struct Fixup { int at; int kind; };  // kind: 0 imm26, 1 imm19, 2 imm14
  std::vector<Fixup> fixups;
};

class Emitter {
 public:
  std::vector<u8> code;
  bool bad = false;  // an operand did not fit its encoding (the code is not used)
  size_t size() const { return code.size(); }

  void word(u32 w) { for (int i = 0; i < 4; i++) code.push_back(u8(w >> (8 * i))); }
  void u64le(u64 v) { for (int i = 0; i < 8; i++) code.push_back(u8(v >> (8 * i))); }
  void align(size_t n) { while (code.size() % n) word(0xd503201f); }  // (nop)

  // ---- moves ----
  void movz(Reg d, u32 imm16, unsigned shift, bool x) { word((x ? 0xd2800000u : 0x52800000u) | (shift / 16) << 21 | (imm16 & 0xffff) << 5 | d); }
  void movk(Reg d, u32 imm16, unsigned shift, bool x) { word((x ? 0xf2800000u : 0x72800000u) | (shift / 16) << 21 | (imm16 & 0xffff) << 5 | d); }
  void mov32(Reg d, u32 v) {  // wd = v
    movz(d, v & 0xffff, 0, false);
    if (v >> 16) movk(d, v >> 16, 16, false);
  }
  void mov64(Reg d, u64 v) {  // xd = v
    movz(d, u32(v & 0xffff), 0, true);
    for (unsigned s = 16; s < 64; s += 16)
      if ((v >> s) & 0xffff) movk(d, u32((v >> s) & 0xffff), s, true);
  }
  void mov(Reg d, Reg m) { word(0x2a0003e0u | m << 16 | d); }    // wd = wm (orr wd, wzr, wm)
  void movx(Reg d, Reg m) { word(0xaa0003e0u | m << 16 | d); }   // xd = xm

  // ---- arithmetic and logic (32-bit unless named x) ----
  // op: 0 add, 1 adds, 2 sub, 3 subs; immediate 0-4095.
  // (shift12: the immediate is shifted left 12.)
  void addsub_imm(unsigned op, Reg d, Reg n, u32 imm, bool x = false, bool shift12 = false) {
    static const u32 k[4] = {0x11000000u, 0x31000000u, 0x51000000u, 0x71000000u};
    word(k[op] | (x ? 0x80000000u : 0) | (shift12 ? 0x00400000u : 0) | (imm & 0xfff) << 10 | n << 5 | d);
  }
  // op: 0 add, 1 adds, 2 sub, 3 subs; m shifted left by `lsl`.
  void addsub(unsigned op, Reg d, Reg n, Reg m, unsigned lsl = 0, bool x = false) {
    static const u32 k[4] = {0x0b000000u, 0x2b000000u, 0x4b000000u, 0x6b000000u};
    word(k[op] | (x ? 0x80000000u : 0) | m << 16 | (lsl & 63) << 10 | n << 5 | d);
  }
  void adcs(Reg d, Reg n, Reg m, bool sub) { word((sub ? 0x7a000000u : 0x3a000000u) | m << 16 | n << 5 | d); }  // adcs / sbcs
  void adc(Reg d, Reg n, Reg m, bool sub) { word((sub ? 0x5a000000u : 0x1a000000u) | m << 16 | n << 5 | d); }   // adc / sbc
  // op: 0 and, 1 orr, 2 eor, 3 ands, 4 bic (and not), 5 orn.
  void logic(unsigned op, Reg d, Reg n, Reg m) {
    static const u32 k[6] = {0x0a000000u, 0x2a000000u, 0x4a000000u, 0x6a000000u, 0x0a200000u, 0x2a200000u};
    word(k[op] | m << 16 | n << 5 | d);
  }
  void shiftv(unsigned op, Reg d, Reg n, Reg m) {  // op: 0 lsl, 1 lsr, 2 asr, 3 ror (by m & 31)
    word(0x1ac02000u | op << 10 | m << 16 | n << 5 | d);
  }
  void ubfm(Reg d, Reg n, unsigned immr, unsigned imms, bool x = false) {
    word((x ? 0xd3400000u : 0x53000000u) | immr << 16 | imms << 10 | n << 5 | d);
  }
  void sbfm(Reg d, Reg n, unsigned immr, unsigned imms, bool x = false) {
    word((x ? 0x93400000u : 0x13000000u) | immr << 16 | imms << 10 | n << 5 | d);
  }
  void bfi(Reg d, Reg n, unsigned lsb, unsigned width) {  // d[lsb, +width) = n[0, width)
    word(0x33000000u | ((32 - lsb) & 31) << 16 | (width - 1) << 10 | n << 5 | d);
  }
  void ubfx(Reg d, Reg n, unsigned lsb, unsigned width) { ubfm(d, n, lsb, lsb + width - 1); }
  void lsl(Reg d, Reg n, unsigned s, bool x = false) { const unsigned w = x ? 64 : 32; ubfm(d, n, (w - s) % w, w - 1 - s, x); }
  void lsr(Reg d, Reg n, unsigned s, bool x = false) { ubfm(d, n, s, x ? 63 : 31, x); }
  void asr(Reg d, Reg n, unsigned s) { sbfm(d, n, s, 31); }
  void uxtb(Reg d, Reg n) { ubfm(d, n, 0, 7); }
  void uxth(Reg d, Reg n) { ubfm(d, n, 0, 15); }
  void sxtb(Reg d, Reg n) { sbfm(d, n, 0, 7); }
  void sxth(Reg d, Reg n) { sbfm(d, n, 0, 15); }
  void smull(Reg d, Reg n, Reg m) { word(0x9b207c00u | m << 16 | n << 5 | d); }  // xd = wn * wm (signed)
  void umull(Reg d, Reg n, Reg m) { word(0x9ba07c00u | m << 16 | n << 5 | d); }  // xd = wn * wm (unsigned)
  void cset(Reg d, Cond c) { word(0x1a9f07e0u | (c ^ 1) << 12 | d); }
  void csel(Reg d, Reg n, Reg m, Cond c) { word(0x1a800000u | m << 16 | c << 12 | n << 5 | d); }
  void mrs_nzcv(Reg t) { word(0xd53b4200u | t); }
  void msr_nzcv(Reg t) { word(0xd51b4200u | t); }

  // ---- memory: [n + imm], imm a multiple of the access size ----
  // size 1, 2, 4, 8; load: zero-extends (sext: sign-extends to 32 bits).
  void ldr(Reg t, Reg n, u32 imm, unsigned size, bool sext = false) {
    const unsigned sc = size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0;
    u32 op = size == 8 ? 0xf9400000u : size == 4 ? 0xb9400000u : size == 2 ? 0x79400000u : 0x39400000u;
    if (sext) op |= 0x00800000u;  // (ldrsb / ldrsh, 32-bit destination)
    if ((imm >> sc) > 0xfff || (imm & ((1u << sc) - 1))) bad = true;
    word(op | ((imm >> sc) & 0xfff) << 10 | n << 5 | t);
  }
  void str(Reg t, Reg n, u32 imm, unsigned size) {
    const unsigned sc = size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0;
    const u32 op = size == 8 ? 0xf9000000u : size == 4 ? 0xb9000000u : size == 2 ? 0x79000000u : 0x39000000u;
    if ((imm >> sc) > 0xfff || (imm & ((1u << sc) - 1))) bad = true;
    word(op | ((imm >> sc) & 0xfff) << 10 | n << 5 | t);
  }
  // [xn + xm]
  void ldr_reg(Reg t, Reg n, Reg m, unsigned size, bool sext = false) {
    u32 op = size == 8 ? 0xf8606800u : size == 4 ? 0xb8606800u : size == 2 ? 0x78606800u : 0x38606800u;
    if (sext) op |= 0x00800000u;
    word(op | m << 16 | n << 5 | t);
  }
  void str_reg(Reg t, Reg n, Reg m, unsigned size) {
    const u32 op = size == 8 ? 0xf8206800u : size == 4 ? 0xb8206800u : size == 2 ? 0x78206800u : 0x38206800u;
    word(op | m << 16 | n << 5 | t);
  }
  void stp_pre(Reg a, Reg b, Reg n, int imm) { word(0xa9800000u | (u32(imm / 8) & 0x7f) << 15 | b << 10 | n << 5 | a); }
  void ldp_post(Reg a, Reg b, Reg n, int imm) { word(0xa8c00000u | (u32(imm / 8) & 0x7f) << 15 | b << 10 | n << 5 | a); }
  void stp(Reg a, Reg b, Reg n, int imm) { word(0xa9000000u | (u32(imm / 8) & 0x7f) << 15 | b << 10 | n << 5 | a); }
  void ldp(Reg a, Reg b, Reg n, int imm) { word(0xa9400000u | (u32(imm / 8) & 0x7f) << 15 | b << 10 | n << 5 | a); }
  void ldr_lit(Reg t, Label& l) { ref(l, 1, 0x58000000u | t); }  // xt = [pc + label] (64-bit)

  // ---- control flow ----
  void b(Label& l) { ref(l, 0, 0x14000000u); }
  void bcond(Cond c, Label& l) { ref(l, 1, 0x54000000u | c); }
  void cbz(Reg t, Label& l, bool x = false) { ref(l, 1, (x ? 0xb4000000u : 0x34000000u) | t); }
  void cbnz(Reg t, Label& l, bool x = false) { ref(l, 1, (x ? 0xb5000000u : 0x35000000u) | t); }
  void tbz(Reg t, unsigned bit, Label& l) { ref(l, 2, 0x36000000u | (bit >> 5) << 31 | (bit & 31) << 19 | t); }
  void tbnz(Reg t, unsigned bit, Label& l) { ref(l, 2, 0x37000000u | (bit >> 5) << 31 | (bit & 31) << 19 | t); }
  void br(Reg n) { word(0xd61f0000u | n << 5); }
  void blr(Reg n) { word(0xd63f0000u | n << 5); }
  void ret() { word(0xd65f03c0u); }

  size_t last_bind = ~size_t(0);
  void bind(Label& l) {
    last_bind = code.size();
    l.pos = int(code.size());
    for (const Label::Fixup& f : l.fixups) patch(f.at, f.kind, l.pos);
    l.fixups.clear();
  }

 private:
  void ref(Label& l, int kind, u32 base) {
    const int at = int(code.size());
    word(base);
    if (l.pos >= 0) patch(at, kind, l.pos);
    else l.fixups.push_back({at, kind});
  }
  void patch(int at, int kind, int target) {
    const s32 d = (target - at) / 4;
    u32 w;
    std::memcpy(&w, &code[size_t(at)], 4);
    if (kind == 0) w |= u32(d) & 0x03ffffffu;
    else if (kind == 1) w |= (u32(d) & 0x7ffffu) << 5;
    else w |= (u32(d) & 0x3fffu) << 5;
    std::memcpy(&code[size_t(at)], &w, 4);
  }
};

}  // namespace leap::arc::a64
