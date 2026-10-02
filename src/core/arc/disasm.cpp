// ARCompact (ARCtangent-A5 / ARC600 subset) disassembler.
//
// Written from the ARCompact encoding tables; the set of decoded sub-opcodes
// and their operand layouts were cross-checked against MAME's arcompact
// disassembler and execution dispatch (BSD-3-Clause, David Haywood).
//
// Output is GNU-ish lowercase: "add.f r0,r1,0x10", "ld.ab r2,[sp,4]",
// "b.d 0x4000123c", "mov_s r0,0x12345678", "j.f [ilink1]".

#include <cstdio>
#include <string>

#include "core/arc/cpu.h"

namespace leap::arc {

namespace {

const char* const kRegNames[64] = {
    "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",     "r7",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14",    "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22",    "r23",
    "r24", "r25", "gp",  "fp",  "sp",  "ilink1", "ilink2", "blink",
    "r32", "r33", "r34", "r35", "r36", "r37", "r38",    "r39",
    "r40", "r41", "r42", "r43", "r44", "r45", "r46",    "r47",
    "r48", "r49", "r50", "r51", "r52", "r53", "r54",    "r55",
    "r56", "mlo", "mmid", "mhi", "lp_count", "r61", "limm", "pcl",
};

constexpr unsigned kLimm = 62;

const char* const kCondNames[16] = {
    "",   "eq", "ne", "pl", "mi", "cs", "cc", "vs",
    "vc", "gt", "ge", "lt", "le", "hi", "ls", "pnz",
};

std::string hex(u32 v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%x", v);
  return b;
}

std::string addr(u32 v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}

// Small values in decimal, everything else in hex.
std::string uimm(u32 v) { return v < 10 ? std::to_string(v) : hex(v); }
std::string simm(u32 v) {
  const s32 s = static_cast<s32>(v);
  return s < 0 ? "-" + uimm(0u - v) : uimm(v);
}

std::string cond_name(unsigned cc) {
  cc &= 31;
  if (cc < 16) return kCondNames[cc];
  char b[8];
  std::snprintf(b, sizeof b, "cc%02x", cc);
  return b;
}

// Condition appended directly to a branch/jump/loop mnemonic ("beq", "jne").
std::string bcond(unsigned cc) {
  cc &= 31;
  return cc < 16 ? std::string(kCondNames[cc]) : "." + cond_name(cc);
}

// Condition as a dotted suffix on ALU ops ("mov.eq").
std::string dcond(unsigned cc) {
  const std::string c = cond_name(cc);
  return c.empty() ? c : "." + c;
}

const char* flag(bool f) { return f ? ".f" : ""; }
const char* dly(bool d) { return d ? ".d" : ""; }

// Per-instruction decode context: tracks the LIMM so every operand that
// references r62 shares one fetch and the length is accounted for.
struct Ctx {
  u32 pc;
  const std::function<u16(u32)>& fetch;
  unsigned base;  // 2 or 4
  bool have_limm = false;
  bool bad = false;
  u32 limm_val = 0;

  u32 limm() {
    if (!have_limm) {
      limm_val = (u32(fetch(pc + base)) << 16) | fetch(pc + base + 2);
      have_limm = true;
    }
    return limm_val;
  }
  unsigned len() const { return base + ((have_limm && !bad) ? 4 : 0); }
  u32 pcl() const { return pc & ~3u; }

  // Source register operand (r62 = long immediate).
  std::string src(unsigned r) { return r == kLimm ? hex(limm()) : kRegNames[r & 63]; }
  // Destination register operand (r62 = discard result).
  static std::string dst(unsigned r) { return r == kLimm ? "0" : kRegNames[r & 63]; }
  // Memory base register ([limm] is an absolute address).
  std::string base_reg(unsigned r) { return r == kLimm ? hex(limm()) : kRegNames[r & 63]; }
};

std::string word32(Ctx& c, u32 op) {
  c.bad = true;
  char b[24];
  std::snprintf(b, sizeof b, ".word 0x%08x", op);
  return b;
}

std::string word16(Ctx& c, u16 op) {
  c.bad = true;
  char b[24];
  std::snprintf(b, sizeof b, ".word 0x%04x", op);
  return b;
}

std::string mem(const std::string& base, const std::string& off) {
  return "[" + base + (off.empty() || off == "0" ? "" : "," + off) + "]";
}

// ----------------------------------------------------------------------------
// 32-bit instruction fields.
// ----------------------------------------------------------------------------
struct Op32 {
  u32 op;
  unsigned major() const { return op >> 27; }
  unsigned a() const { return bits(op, 0, 6); }
  unsigned b() const { return bits(op, 24, 3) | (bits(op, 12, 3) << 3); }
  unsigned c() const { return bits(op, 6, 6); }
  unsigned sub() const { return bits(op, 16, 6); }
  unsigned p() const { return bits(op, 22, 2); }
  bool f() const { return bit(op, 15); }
  bool m() const { return bit(op, 5); }
  unsigned cc() const { return bits(op, 0, 5); }
  u32 u6() const { return c(); }
  u32 s12() const { return sext(c() | (a() << 6), 12); }
};

// Operand forms of the general (major 0x04 / 0x05) encodings.
enum class Form : u8 {
  None,    // not a valid sub-opcode
  Alu,     // op<.cc><.f> a,b,c
  NoDst,   // op<.cc> b,c      (tst/cmp/rcmp/btst: flags implied)
  NoDstF,  // op<.cc><.f> b,c  (mul64/mulu64: a is ignored)
  Mov,     // mov<.cc><.f> b,c
  Jump,    // j/jl<cc><.d><.f> [c]
  Flag,    // flag<.cc> c
  Lp,      // lp<cc> target
  Lr,      // lr b,[c]
  Sr,      // sr b,[c]
  Sop,     // single-operand group (0x2f)
  LdRR,    // ld a,[b,c] (0x30..0x37)
};

struct GenEntry {
  const char* name;
  Form form;
};

const GenEntry kOp04[64] = {
    /*00*/ {"add", Form::Alu},    {"adc", Form::Alu},    {"sub", Form::Alu},    {"sbc", Form::Alu},
    /*04*/ {"and", Form::Alu},    {"or", Form::Alu},     {"bic", Form::Alu},    {"xor", Form::Alu},
    /*08*/ {"max", Form::Alu},    {"min", Form::Alu},    {"mov", Form::Mov},    {"tst", Form::NoDst},
    /*0c*/ {"cmp", Form::NoDst},  {"rcmp", Form::NoDst}, {"rsub", Form::Alu},   {"bset", Form::Alu},
    /*10*/ {"bclr", Form::Alu},   {"btst", Form::NoDst}, {"bxor", Form::Alu},   {"bmsk", Form::Alu},
    /*14*/ {"add1", Form::Alu},   {"add2", Form::Alu},   {"add3", Form::Alu},   {"sub1", Form::Alu},
    /*18*/ {"sub2", Form::Alu},   {"sub3", Form::Alu},   {"mpy", Form::Alu},    {"mpyh", Form::Alu},
    /*1c*/ {"mpyhu", Form::Alu},  {"mpyu", Form::Alu},   {nullptr, Form::None}, {nullptr, Form::None},
    /*20*/ {"j", Form::Jump},     {"j", Form::Jump},     {"jl", Form::Jump},    {"jl", Form::Jump},
    /*24*/ {nullptr, Form::None}, {nullptr, Form::None}, {nullptr, Form::None}, {nullptr, Form::None},
    /*28*/ {"lp", Form::Lp},      {"flag", Form::Flag},  {"lr", Form::Lr},      {"sr", Form::Sr},
    /*2c*/ {nullptr, Form::None}, {nullptr, Form::None}, {nullptr, Form::None}, {nullptr, Form::Sop},
    /*30*/ {"ld", Form::LdRR},    {"ld", Form::LdRR},    {"ld", Form::LdRR},    {"ld", Form::LdRR},
    /*34*/ {"ld", Form::LdRR},    {"ld", Form::LdRR},    {"ld", Form::LdRR},    {"ld", Form::LdRR},
    /*38*/ {nullptr, Form::None}, {nullptr, Form::None}, {nullptr, Form::None}, {nullptr, Form::None},
    /*3c*/ {nullptr, Form::None}, {nullptr, Form::None}, {nullptr, Form::None}, {nullptr, Form::None},
};

const GenEntry kOp05[64] = {
    /*00*/ {"asl", Form::Alu},     {"lsr", Form::Alu},     {"asr", Form::Alu},    {"ror", Form::Alu},
    /*04*/ {"mul64", Form::NoDstF}, {"mulu64", Form::NoDstF}, {"adds", Form::Alu}, {"subs", Form::Alu},
    /*08*/ {"divaw", Form::Alu},   {nullptr, Form::None},  {"asls", Form::Alu},   {"asrs", Form::Alu},
    // 0x0c/0x10/0x14: undocumented extension ops used by the Leapster BIOS
    // (MAME decodes the operands but does not know the semantics).
    /*0c*/ {"op05_0c", Form::Alu}, {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*10*/ {"op05_10", Form::Alu}, {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*14*/ {"op05_14", Form::Alu}, {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*18*/ {nullptr, Form::None},  {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*1c*/ {nullptr, Form::None},  {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*20*/ {nullptr, Form::None},  {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*24*/ {nullptr, Form::None},  {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*28*/ {"addsdw", Form::Alu},  {"subsdw", Form::Alu},  {nullptr, Form::None}, {nullptr, Form::None},
    /*2c*/ {nullptr, Form::None},  {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::Sop},
    /*30*/ {nullptr, Form::None},  {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*34*/ {nullptr, Form::None},  {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*38*/ {nullptr, Form::None},  {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
    /*3c*/ {nullptr, Form::None},  {nullptr, Form::None},  {nullptr, Form::None}, {nullptr, Form::None},
};

// Single-operand ops, major 0x04 sub 0x2f (indexed by the A field).
const char* const kSop04[16] = {
    "asl", "asr", "lsr", "ror", "rrc", "sexb", "sexw", "extb",
    "extw", "abs", "not", "rlc", "ex", nullptr, nullptr, nullptr,
};

// Single-operand ops, major 0x05 sub 0x2f.
const char* const kSop05[16] = {
    "swap", "norm", "sat16", "rnd16", "abssw", "abss", "negsw", "negs",
    "normw", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

// Second (C) operand of a general-format instruction for the P/M modes.
std::string gen_c(Ctx& c, const Op32& o) {
  switch (o.p()) {
    case 0: return c.src(o.c());
    case 1: return uimm(o.u6());
    case 2: return simm(o.s12());
    default: return o.m() ? uimm(o.u6()) : c.src(o.c());
  }
}

// Condition suffix for P=3 forms, empty otherwise.
unsigned gen_cc(const Op32& o) { return o.p() == 3 ? o.cc() : 0; }

std::string dis_general(Ctx& c, const Op32& o, const GenEntry& e) {
  const unsigned p = o.p();
  switch (e.form) {
    case Form::Alu: {
      const std::string cc = dcond(gen_cc(o));
      const std::string head = std::string(e.name) + cc + flag(o.f()) + " ";
      if (p <= 1) return head + Ctx::dst(o.a()) + "," + c.src(o.b()) + "," + gen_c(c, o);
      return head + Ctx::dst(o.b()) + "," + c.src(o.b()) + "," + gen_c(c, o);
    }
    case Form::NoDst:
    case Form::NoDstF: {
      const std::string cc = dcond(gen_cc(o));
      const bool show_f = e.form == Form::NoDstF && o.f();
      return std::string(e.name) + cc + flag(show_f) + " " + c.src(o.b()) + "," + gen_c(c, o);
    }
    case Form::Mov: {
      if (p == 1 && o.b() == kLimm && !o.f() && o.u6() == 0) return "nop";
      const std::string cc = dcond(gen_cc(o));
      return std::string("mov") + cc + flag(o.f()) + " " + Ctx::dst(o.b()) + "," + gen_c(c, o);
    }
    case Form::Jump: {
      const bool d = o.sub() & 1;
      std::string head = std::string(e.name) + bcond(gen_cc(o)) + dly(d) + flag(o.f()) + " ";
      const bool reg_form = (p == 0) || (p == 3 && !o.m());
      if (reg_form) {
        if (o.c() == kLimm) return head + addr(c.limm());
        return head + "[" + kRegNames[o.c()] + "]";
      }
      if (p == 2) return head + addr(o.s12());
      return head + addr(o.u6());
    }
    case Form::Flag: {
      const std::string cc = dcond(gen_cc(o));
      return std::string("flag") + cc + " " + gen_c(c, o);
    }
    case Form::Lp: {
      if (p == 2) return "lp " + addr(c.pcl() + o.s12() * 2);
      if (p == 3) return "lp" + bcond(o.cc()) + " " + addr(c.pcl() + o.u6() * 2);
      return word32(c, o.op);
    }
    case Form::Lr:
    case Form::Sr: {
      std::string aux;
      switch (p) {
        case 0: aux = o.c() == kLimm ? hex(c.limm()) : kRegNames[o.c()]; break;
        case 1: aux = hex(o.u6()); break;
        case 2: aux = hex(o.s12()); break;
        default: return word32(c, o.op);
      }
      if (e.form == Form::Lr) return "lr " + Ctx::dst(o.b()) + ",[" + aux + "]";
      return "sr " + c.src(o.b()) + ",[" + aux + "]";
    }
    case Form::LdRR: {
      static const char* const kSize[4] = {"", "b", "w", "?"};
      static const char* const kAA[4] = {"", ".a", ".ab", ".as"};
      const unsigned zz = bits(o.op, 17, 2);
      const bool x = bit(o.op, 16);
      if (zz == 3) return word32(c, o.op);
      std::string s = std::string("ld") + kSize[zz] + (x ? ".x" : "") + kAA[p] + (o.f() ? ".di" : "");
      return s + " " + Ctx::dst(o.a()) + ",[" + c.base_reg(o.b()) + "," + c.src(o.c()) + "]";
    }
    case Form::Sop:
    case Form::None: break;
  }
  return word32(c, o.op);
}

std::string dis_sop(Ctx& c, const Op32& o, bool major05) {
  const unsigned sub = o.a();
  const unsigned p = o.p();
  if (sub == 0x3f) {
    // Zero-operand ops, selected by the B field.
    if (major05) return word32(c, o.op);
    switch (o.b()) {
      case 1: {
        std::string s = "sleep";
        if (p == 1 && o.u6() != 0) s += " " + uimm(o.u6());
        return s;
      }
      case 2: return "swi";
      case 3: return "sync";
      case 4: return "rtie";
      case 5: return "brk";
      default: return word32(c, o.op);
    }
  }
  const char* name = sub < 16 ? (major05 ? kSop05[sub] : kSop04[sub]) : nullptr;
  if (!name || p > 1) return word32(c, o.op);
  const std::string operand = p == 0 ? c.src(o.c()) : uimm(o.u6());
  if (!major05 && sub == 0x0c) {  // EX b,[c]: the F bit is .di
    return std::string("ex") + (o.f() ? ".di" : "") + " " + Ctx::dst(o.b()) + ",[" + operand + "]";
  }
  return std::string(name) + flag(o.f()) + " " + Ctx::dst(o.b()) + "," + operand;
}

std::string dis32(Ctx& c, u32 op) {
  const Op32 o{op};
  static const char* const kSize[4] = {"", "b", "w", "?"};
  static const char* const kAA[4] = {"", ".a", ".ab", ".as"};

  switch (o.major()) {
    case 0x00: {  // Bcc s21 / B s25
      u32 off = bits(op, 17, 10) | (bits(op, 6, 10) << 10);
      const bool d = bit(op, 5);
      if (!bit(op, 16)) {
        off = sext(off, 20) * 2;
        return "b" + bcond(o.cc()) + dly(d) + " " + addr(c.pcl() + off);
      }
      off = sext(off | (bits(op, 0, 4) << 20), 24) * 2;
      return std::string("b") + dly(d) + " " + addr(c.pcl() + off);
    }

    case 0x01: {
      const bool d = bit(op, 5);
      if (!bit(op, 16)) {  // BLcc s21 / BL s25 (32-bit aligned targets)
        u32 off = (bits(op, 18, 9) << 1) | (bits(op, 6, 10) << 10);
        if (!bit(op, 17)) {
          off = sext(off, 20) * 2;
          return "bl" + bcond(o.cc()) + dly(d) + " " + addr(c.pcl() + off);
        }
        off = sext(off | (bits(op, 0, 4) << 20), 24) * 2;
        return std::string("bl") + dly(d) + " " + addr(c.pcl() + off);
      }
      // BRcc / BBITn
      static const char* const kBr[16] = {
          "breq", "brne", "brlt", "brge", "brlo", "brhs", nullptr, nullptr,
          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "bbit0", "bbit1"};
      const char* name = kBr[op & 15];
      if (!name) return word32(c, op);
      const u32 off = sext(bits(op, 17, 7) | (bits(op, 15, 1) << 7), 8) * 2;
      const std::string second = bit(op, 4) ? uimm(o.u6()) : c.src(o.c());
      return std::string(name) + dly(d) + " " + c.src(o.b()) + "," + second + "," +
             addr(c.pcl() + off);
    }

    case 0x02: {  // LD<zz><.x><.aa><.di> a,[b,s9]
      const unsigned zz = bits(op, 7, 2);
      if (zz == 3) return word32(c, op);
      const bool x = bit(op, 6);
      const unsigned aa = bits(op, 9, 2);
      const bool di = bit(op, 11);
      const u32 s9 = sext(bits(op, 16, 8) | (bits(op, 15, 1) << 8), 9);
      std::string s = std::string("ld") + kSize[zz] + (x ? ".x" : "") + kAA[aa] + (di ? ".di" : "");
      return s + " " + Ctx::dst(o.a()) + "," + mem(c.base_reg(o.b()), simm(s9));
    }

    case 0x03: {  // ST<zz><.aa><.di> c,[b,s9]
      const unsigned zz = bits(op, 1, 2);
      if (zz == 3) return word32(c, op);
      const unsigned aa = bits(op, 3, 2);
      const bool di = bit(op, 5);
      const u32 s9 = sext(bits(op, 16, 8) | (bits(op, 15, 1) << 8), 9);
      std::string s = std::string("st") + kSize[zz] + kAA[aa] + (di ? ".di" : "");
      return s + " " + c.src(o.c()) + "," + mem(c.base_reg(o.b()), simm(s9));
    }

    case 0x04:
    case 0x05: {
      const bool is05 = o.major() == 0x05;
      const GenEntry& e = is05 ? kOp05[o.sub()] : kOp04[o.sub()];
      if (e.form == Form::Sop) return dis_sop(c, o, is05);
      return dis_general(c, o, e);
    }

    default: {  // 0x06..0x0b: extension instruction space
      char b[32];
      std::snprintf(b, sizeof b, "ext%02x 0x%08x", o.major(), op);
      return b;
    }
  }
}

// ----------------------------------------------------------------------------
// 16-bit instructions.
// ----------------------------------------------------------------------------
unsigned r16(unsigned f) { return (f & 7) > 3 ? (f & 7) + 8 : (f & 7); }

std::string dis16(Ctx& c, u16 op) {
  const unsigned major = op >> 11;
  const unsigned b = r16(bits(op, 8, 3));
  const unsigned cr = r16(bits(op, 5, 3));
  const unsigned a = r16(bits(op, 0, 3));
  const char* rb = kRegNames[b];
  const char* rc = kRegNames[cr];
  const char* ra = kRegNames[a];
  auto s = [](const char* x) { return std::string(x); };

  switch (major) {
    case 0x0c: {  // LD_S / LDB_S / LDW_S a,[b,c]; ADD_S a,b,c
      static const char* const k[4] = {"ld_s", "ldb_s", "ldw_s", "add_s"};
      const unsigned i = bits(op, 3, 2);
      if (i == 3) return s("add_s ") + ra + "," + rb + "," + rc;
      return s(k[i]) + " " + ra + ",[" + rb + "," + rc + "]";
    }

    case 0x0d: {  // op_S c,b,u3
      static const char* const k[4] = {"add_s", "sub_s", "asl_s", "asr_s"};
      return s(k[bits(op, 3, 2)]) + " " + rc + "," + rb + "," + uimm(bits(op, 0, 3));
    }

    case 0x0e: {  // ADD_S b,b,h / MOV_S b,h / CMP_S b,h / MOV_S h,b
      const unsigned h = bits(op, 5, 3) | (bits(op, 0, 3) << 3);
      switch (bits(op, 3, 2)) {
        case 0: return s("add_s ") + rb + "," + rb + "," + c.src(h);
        case 1: return s("mov_s ") + rb + "," + c.src(h);
        case 2: return s("cmp_s ") + rb + "," + c.src(h);
        default: return s("mov_s ") + kRegNames[h] + "," + rb;
      }
    }

    case 0x0f: {
      const unsigned sub = bits(op, 0, 5);
      if (sub == 0x00) {
        switch (bits(op, 5, 3)) {
          case 0: return s("j_s [") + rb + "]";
          case 1: return s("j_s.d [") + rb + "]";
          case 2: return s("jl_s [") + rb + "]";
          case 3: return s("jl_s.d [") + rb + "]";
          case 6: return s("sub_s.ne ") + rb + "," + rb + "," + rb;
          case 7:
            switch (bits(op, 8, 3)) {
              case 0: return "nop_s";
              case 1: return "unimp_s";
              case 4: return "jeq_s [blink]";
              case 5: return "jne_s [blink]";
              case 6: return "j_s [blink]";
              case 7: return "j_s.d [blink]";
              default: return word16(c, op);
            }
          default: return word16(c, op);
        }
      }
      if (sub == 0x1e) return "trap_s " + uimm(bits(op, 5, 6));
      if (sub == 0x1f) return bits(op, 5, 6) == 0x3f ? "brk_s" : word16(c, op);
      // 0 = invalid, 2 = op_S b,b,c, 1 = op_S b,c
      static const struct { const char* name; u8 form; } k[32] = {
          {nullptr, 0},   {nullptr, 0},   {"sub_s", 2},   {nullptr, 0},
          {"and_s", 2},   {"or_s", 2},    {"bic_s", 2},   {"xor_s", 2},
          {nullptr, 0},   {nullptr, 0},   {nullptr, 0},   {"tst_s", 1},
          {"mul64_s", 1}, {"sexb_s", 1},  {"sexw_s", 1},  {"extb_s", 1},
          {"extw_s", 1},  {"abs_s", 1},   {"not_s", 1},   {"neg_s", 1},
          {"add1_s", 2},  {"add2_s", 2},  {"add3_s", 2},  {nullptr, 0},
          {"asl_s", 2},   {"lsr_s", 2},   {"asr_s", 2},   {"asl_s", 1},
          {"asr_s", 1},   {"lsr_s", 1},   {nullptr, 0},   {nullptr, 0},
      };
      if (k[sub].form == 0) return word16(c, op);
      if (k[sub].form == 2) return s(k[sub].name) + " " + rb + "," + rb + "," + rc;
      return s(k[sub].name) + " " + rb + "," + rc;
    }

    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: {  // LD_S/ST_S c,[b,u]
      static const char* const k[7] = {"ld_s", "ldb_s", "ldw_s", "ldw_s.x", "st_s", "stb_s", "stw_s"};
      static const unsigned shift[7] = {2, 0, 1, 1, 2, 0, 1};
      const unsigned i = major - 0x10;
      const u32 u = bits(op, 0, 5) << shift[i];
      return s(k[i]) + " " + rc + "," + mem(rb, uimm(u));
    }

    case 0x17: {  // shift/sub/bit b,b,u5
      static const char* const k[8] = {"asl_s", "lsr_s", "asr_s", "sub_s", "bset_s", "bclr_s", "bmsk_s", "btst_s"};
      const unsigned i = bits(op, 5, 3);
      const std::string u = uimm(bits(op, 0, 5));
      if (i == 7) return s(k[i]) + " " + rb + "," + u;
      return s(k[i]) + " " + rb + "," + rb + "," + u;
    }

    case 0x18: {  // stack-pointer based
      const std::string u7 = uimm(bits(op, 0, 5) << 2);
      switch (bits(op, 5, 3)) {
        case 0: return s("ld_s ") + rb + "," + mem("sp", u7);
        case 1: return s("ldb_s ") + rb + "," + mem("sp", u7);
        case 2: return s("st_s ") + rb + "," + mem("sp", u7);
        case 3: return s("stb_s ") + rb + "," + mem("sp", u7);
        case 4: return s("add_s ") + rb + ",sp," + u7;
        case 5:
          switch (bits(op, 8, 3)) {
            case 0: return "add_s sp,sp," + u7;
            case 1: return "sub_s sp,sp," + u7;
            default: return word16(c, op);
          }
        case 6:
          switch (bits(op, 0, 5)) {
            case 0x01: return s("pop_s ") + rb;
            case 0x11: return "pop_s blink";
            default: return word16(c, op);
          }
        default:
          switch (bits(op, 0, 5)) {
            case 0x01: return s("push_s ") + rb;
            case 0x11: return "push_s blink";
            default: return word16(c, op);
          }
      }
    }

    case 0x19: {  // gp-relative
      const u32 s9 = sext(bits(op, 0, 9), 9);
      switch (bits(op, 9, 2)) {
        case 0: return "ld_s r0," + mem("gp", simm(s9 << 2));
        case 1: return "ldb_s r0," + mem("gp", simm(s9));
        case 2: return "ldw_s r0," + mem("gp", simm(s9 << 1));
        default: return "add_s r0,gp," + simm(s9 << 2);
      }
    }

    case 0x1a:  // LD_S b,[pcl,u10]
      return s("ld_s ") + rb + "," + mem("pcl", uimm(bits(op, 0, 8) << 2));

    case 0x1b:  // MOV_S b,u8
      return s("mov_s ") + rb + "," + uimm(bits(op, 0, 8));

    case 0x1c: {  // ADD_S b,b,u7 / CMP_S b,u7
      const std::string u = uimm(bits(op, 0, 7));
      if (bit(op, 7)) return s("cmp_s ") + rb + "," + u;
      return s("add_s ") + rb + "," + rb + "," + u;
    }

    case 0x1d: {  // BREQ_S / BRNE_S b,0,s8
      const u32 off = sext(bits(op, 0, 7), 7) * 2;
      return s(bit(op, 7) ? "brne_s " : "breq_s ") + rb + ",0," + addr(c.pcl() + off);
    }

    case 0x1e: {  // Bcc_S
      const unsigned i = bits(op, 9, 2);
      if (i < 3) {
        static const char* const k[3] = {"b_s", "beq_s", "bne_s"};
        const u32 off = sext(bits(op, 0, 9), 9) * 2;
        return s(k[i]) + " " + addr(c.pcl() + off);
      }
      static const char* const k[8] = {"bgt_s", "bge_s", "blt_s", "ble_s", "bhi_s", "bhs_s", "blo_s", "bls_s"};
      const u32 off = sext(bits(op, 0, 6), 6) * 2;
      return s(k[bits(op, 6, 3)]) + " " + addr(c.pcl() + off);
    }

    default: {  // 0x1f: BL_S s13
      const u32 off = sext(bits(op, 0, 11), 11) * 4;
      return "bl_s " + addr(c.pcl() + off);
    }
  }
}

}  // namespace

const char* reg_name(unsigned r) { return kRegNames[r & 63]; }

std::string disassemble(u32 pc, const std::function<u16(u32)>& fetch16, unsigned* len) {
  const u16 hw = fetch16(pc);
  const unsigned major = hw >> 11;
  std::string text;
  unsigned n;
  if (major < 0x0c) {
    Ctx c{pc, fetch16, 4};
    const u32 op = (u32(hw) << 16) | fetch16(pc + 2);
    text = dis32(c, op);
    n = c.len();
  } else {
    Ctx c{pc, fetch16, 2};
    text = dis16(c, hw);
    n = c.len();
  }
  if (len) *len = n;
  return text;
}

}  // namespace leap::arc
