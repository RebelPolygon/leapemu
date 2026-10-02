#pragma once

// A minimal x86-64 machine-code emitter for the JIT (core/arc/jit.cpp): only
// the instructions it uses, 32-bit operations unless named otherwise.
// Encodings follow the Intel 64 and IA-32 Architectures Software Developer's
// Manual, volume 2.

#include <cstring>
#include <vector>

#include "core/common.h"

namespace leap::arc::x64 {

enum Reg : u8 { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

// Condition codes (the low nibble of Jcc / SETcc / CMOVcc).
enum Cond : u8 { O, NO, B, AE, E, NE, BE, A, S, NS, P, NP, L, GE, LE, G };

// A memory operand: [base + index * 2^scale + disp].
struct Mem {
  Reg base;
  s32 disp = 0;
  int index = -1;  // register number, or -1
  int scale = 0;
};
inline Mem mem(Reg base, s32 disp = 0) { return Mem{base, disp}; }
inline Mem mem(Reg base, Reg index, s32 disp = 0) { return Mem{base, disp, int(index), 0}; }

struct Label {
  int pos = -1;
  std::vector<int> fixups;  // offsets of rel32 fields to patch
};

class Emitter {
 public:
  std::vector<u8> code;
  size_t size() const { return code.size(); }

  void byte(u8 b) { code.push_back(b); }
  void u32le(u32 v) { for (int i = 0; i < 4; i++) byte(u8(v >> (8 * i))); }
  void u64le(u64 v) { for (int i = 0; i < 8; i++) byte(u8(v >> (8 * i))); }

  // ---- data movement ----
  void mov(Reg dst, Mem m) { op_m(0x8b, dst, m); }                    // mov r32, [m]
  void mov(Mem m, Reg src) { op_m(0x89, src, m); }                    // mov [m], r32
  void mov(Reg dst, Reg src) { op_r(0x89, src, dst); }                // mov r32, r32
  void mov(Mem m, u32 imm) { op_m(0xc7, 0, m); u32le(imm); }          // mov dword [m], imm32
  void mov(Reg dst, u32 imm) { rex(false, 0, 0, dst); byte(u8(0xb8 + (dst & 7))); u32le(imm); }
  void mov64(Reg dst, u64 imm) { rex(true, 0, 0, dst); byte(u8(0xb8 + (dst & 7))); u64le(imm); }
  void mov64(Reg dst, Mem m) { op_m(0x8b, dst, m, true); }            // mov r64, [m]
  void mov64(Reg dst, Reg src) { op_r(0x89, src, dst, true); }
  void mov64(Mem m, Reg src) { op_m(0x89, src, m, true); }            // mov [m], r64
  void mov8(Mem m, Reg src) { op_m(0x88, src, m, false, src >= 4); }  // mov [m], r8
  void mov16(Mem m, Reg src) { byte(0x66); op_m(0x89, src, m); }      // mov [m], r16
  void mov8(Mem m, u8 imm) { op_m(0xc6, 0, m); byte(imm); }           // mov byte [m], imm8
  void movzx8(Reg dst, Mem m) { op_m2(0xb6, dst, m); }
  void movzx16(Reg dst, Mem m) { op_m2(0xb7, dst, m); }
  void movsx8(Reg dst, Mem m) { op_m2(0xbe, dst, m); }
  void movsx16(Reg dst, Mem m) { op_m2(0xbf, dst, m); }
  void movzx8(Reg dst, Reg src) { op_r2(0xb6, dst, src, src >= 4); }  // movzx r32, r8
  void movzx16(Reg dst, Reg src) { op_r2(0xb7, dst, src); }
  void movsx8(Reg dst, Reg src) { op_r2(0xbe, dst, src, src >= 4); }
  void movsx16(Reg dst, Reg src) { op_r2(0xbf, dst, src); }
  void lea(Reg dst, Mem m) { op_m(0x8d, dst, m); }
  void lea64(Reg dst, Mem m) { op_m(0x8d, dst, m, true); }

  // ---- arithmetic: group-1 operations ----
  enum Alu : u8 { ADD = 0, OR = 1, ADC = 2, SBB = 3, AND = 4, SUB = 5, XOR = 6, CMP = 7 };
  void alu(Alu op, Reg dst, Reg src) { op_r(u8(op * 8 + 1), src, dst); }
  void alu(Alu op, Reg dst, Mem m) { op_m(u8(op * 8 + 3), dst, m); }
  void alu(Alu op, Mem m, Reg src) { op_m(u8(op * 8 + 1), src, m); }
  void alu(Alu op, Reg dst, u32 imm) {
    if (fits8(imm)) { op_r(0x83, op, dst); byte(u8(imm)); }
    else { op_r(0x81, op, dst); u32le(imm); }
  }
  void alu(Alu op, Mem m, u32 imm) {
    if (fits8(imm)) { op_m(0x83, op, m); byte(u8(imm)); }
    else { op_m(0x81, op, m); u32le(imm); }
  }
  void alu64(Alu op, Reg dst, u32 imm) {
    if (fits8(imm)) { op_r(0x83, op, dst, true); byte(u8(imm)); }
    else { op_r(0x81, op, dst, true); u32le(imm); }
  }
  void alu64(Alu op, Mem m, Reg src) { op_m(u8(op * 8 + 1), src, m, true); }
  void alu64(Alu op, Reg dst, Mem m) { op_m(u8(op * 8 + 3), dst, m, true); }
  void alu64(Alu op, Reg dst, Reg src) { op_r(u8(op * 8 + 1), src, dst, true); }
  void test(Reg a, Reg b) { op_r(0x85, b, a); }
  void test(Reg a, u32 imm) { op_r(0xf7, 0, a); u32le(imm); }
  void test64(Reg a, Reg b) { op_r(0x85, b, a, true); }
  void test8(Mem m, u8 imm) { op_m(0xf6, 0, m); byte(imm); }
  void cmp8(Mem m, u8 imm) { op_m(0x80, 7, m); byte(imm); }
  void not_(Reg r) { op_r(0xf7, 2, r); }
  void neg(Reg r) { op_r(0xf7, 3, r); }
  void imul(Reg dst, Reg src) { op_r2(0xaf, dst, src); }
  void imul64(Reg dst, Reg src) { rex(true, dst, 0, src); byte(0x0f); byte(0xaf); byte(u8(0xc0 | (dst & 7) << 3 | (src & 7))); }
  void movsxd(Reg dst, Reg src) { op_r(0x63, dst, src, true); }  // movsxd r64, r32

  // ---- shifts ----
  enum Shift : u8 { SHL = 4, SHR = 5, SAR = 7 };
  void shift(Shift s, Reg r, u8 n) { op_r(0xc1, s, r); byte(n); }
  void shift_cl(Shift s, Reg r) { op_r(0xd3, s, r); }
  void shift64(Shift s, Reg r, u8 n) { op_r(0xc1, s, r, true); byte(n); }
  void bt(Reg base, Reg bit) { op_r2(0xa3, bit, base); }  // CF = bit `bit` of `base`
  void bt(Mem m, u8 bit) { op_m2(0xba, 4, m); byte(bit); }  // CF = bit `bit` of dword [m]
  enum Rot1 : u8 { ROL1 = 0, ROR1 = 1, RCL1 = 2, RCR1 = 3 };
  void rot1(Rot1 k, Reg r) { op_r(0xd1, k, r); }          // rotate by one

  // ---- conditions ----
  void setcc(Cond c, Reg r8) { op_r2(u8(0x90 + c), Reg(0), r8, r8 >= 4); }
  void cmov(Cond c, Reg dst, Reg src) { op_r2(u8(0x40 + c), dst, src); }

  // ---- control flow ----
  void jcc(Cond c, Label& l) { byte(0x0f); byte(u8(0x80 + c)); ref(l); }
  void jmp(Label& l) { byte(0xe9); ref(l); }
  void jmp(Reg r) { rex(false, 0, 0, r); byte(0xff); byte(u8(0xe0 | (r & 7))); }
  // jmp qword [rip + slot]: through an 8-byte address stored at label `slot`.
  void jmp_indirect(Label& slot) { byte(0xff); byte(0x25); ref(slot); }
  void align(size_t n) { while (code.size() % n) byte(0xcc); }
  void call(Reg r) { rex(false, 0, 0, r); byte(0xff); byte(u8(0xd0 | (r & 7))); }
  void call(const void* fn) { mov64(RAX, u64(reinterpret_cast<uintptr_t>(fn))); call(RAX); }
  void ret() { byte(0xc3); }
  void push(Reg r) { if (r >= 8) byte(0x41); byte(u8(0x50 + (r & 7))); }
  void pop(Reg r) { if (r >= 8) byte(0x41); byte(u8(0x58 + (r & 7))); }
  size_t last_bind = ~size_t(0);  // where a label was last bound (control flow may join there)
  void bind(Label& l) {
    last_bind = code.size();
    l.pos = int(code.size());
    for (int f : l.fixups) patch(f, l.pos);
    l.fixups.clear();
  }

 private:
  static bool fits8(u32 imm) { return s32(imm) >= -128 && s32(imm) <= 127; }
  void ref(Label& l) {
    const int at = int(code.size());
    u32le(0);
    if (l.pos >= 0) patch(at, l.pos);
    else l.fixups.push_back(at);
  }
  void patch(int at, int target) {
    const s32 rel = target - (at + 4);
    std::memcpy(&code[size_t(at)], &rel, 4);
  }
  // REX prefix: W (64-bit operand), R (reg field), X (index), B (base / rm).
  // `force` emits an empty REX for byte access to SPL/BPL/SIL/DIL.
  void rex(bool w, unsigned reg, unsigned index, unsigned base, bool force = false) {
    const u8 r = u8(0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((index >> 3) & 1) << 1 | ((base >> 3) & 1));
    if (r != 0x40 || force) byte(r);
  }
  void modrm_mem(unsigned reg, const Mem& m) {
    const unsigned base = m.base & 7;
    const bool sib = m.index >= 0 || base == 4;
    const int mod = (m.disp == 0 && base != 5) ? 0 : (m.disp >= -128 && m.disp <= 127) ? 1 : 2;
    if (sib) {
      byte(u8(mod << 6 | (reg & 7) << 3 | 4));
      const unsigned idx = m.index >= 0 ? unsigned(m.index & 7) : 4;
      byte(u8(m.scale << 6 | idx << 3 | base));
    } else {
      byte(u8(mod << 6 | (reg & 7) << 3 | base));
    }
    if (mod == 1) byte(u8(s8(m.disp)));
    else if (mod == 2) u32le(u32(m.disp));
  }
  // One-byte opcode with a memory operand / a register operand (reg field =
  // `reg`, which is an opcode extension for group instructions).
  void op_m(u8 opc, unsigned reg, const Mem& m, bool w = false, bool force = false) {
    rex(w, reg, m.index >= 0 ? unsigned(m.index) : 0, m.base, force);
    byte(opc);
    modrm_mem(reg, m);
  }
  void op_r(u8 opc, unsigned reg, unsigned rm, bool w = false) {
    rex(w, reg, 0, rm);
    byte(opc);
    byte(u8(0xc0 | (reg & 7) << 3 | (rm & 7)));
  }
  // Two-byte (0F xx) opcodes.
  void op_m2(u8 opc, unsigned reg, const Mem& m) {
    rex(false, reg, m.index >= 0 ? unsigned(m.index) : 0, m.base);
    byte(0x0f);
    byte(opc);
    modrm_mem(reg, m);
  }
  void op_r2(u8 opc, unsigned reg, unsigned rm, bool force = false) {
    rex(false, reg, 0, rm, force);
    byte(0x0f);
    byte(opc);
    byte(u8(0xc0 | (reg & 7) << 3 | (rm & 7)));
  }
};

}  // namespace leap::arc::x64
