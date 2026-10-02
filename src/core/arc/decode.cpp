// Decode cache for the ARCompact interpreter.
//
// Instructions fetched from ROM are decoded once into a DecodedOp: a handler
// pointer plus pre-extracted operands (16-bit register fields expanded,
// immediates sign-extended, branch targets made absolute, masks precomputed).
// The most frequently executed forms (~95% of dynamic instructions in the
// games profiled) get specialised handlers; everything else is routed to the
// reference implementation in cpu.cpp (exec16 / exec32), so semantics live in
// one place and the fast paths can be checked against it.

#include <algorithm>
#include <bit>
#include <unordered_map>

#include "core/arc/cpu.h"
#include "core/arc/jit.h"

namespace leap::arc {

namespace {

constexpr unsigned r16(unsigned f) { return f > 3 ? f + 8 : f; }
constexpr unsigned f_a(u32 op) { return op & 63; }
constexpr unsigned f_b(u32 op) { return ((op >> 24) & 7) | (((op >> 12) & 7) << 3); }
constexpr unsigned f_c(u32 op) { return (op >> 6) & 63; }
constexpr unsigned f_p(u32 op) { return (op >> 22) & 3; }
constexpr u32 f_s12(u32 op) { return sext(((op >> 6) & 63) | ((op & 63) << 6), 12); }

enum Alu {
  kAdd, kSub, kAnd, kOr, kBic, kXor, kRsub, kBset, kBclr, kBmsk,
  kAdd1, kAdd2, kAdd3, kSub1, kSub2, kSub3, kAslM, kLsrM, kAsrM,
};

}  // namespace

struct Handlers {
  // ---- generic fallbacks: the reference implementation ----
  static void gen16(Cpu& c, const DecodedOp& o) {
    c.r_[Cpu::kPCL] = c.pc_ & ~3u;
    c.len_ = 2;
    c.limm_loaded_ = false;
    c.exec16(u16(o.raw));
    // Charge fetch cost of any long immediate found at run time.
    if (c.len_ > o.len) c.stall_ += ((c.len_ - o.len) / 2) * c.fetch_wait16(c.pc_);
  }
  static void gen32(Cpu& c, const DecodedOp& o) {
    c.r_[Cpu::kPCL] = c.pc_ & ~3u;
    c.len_ = 4;
    c.limm_loaded_ = false;
    c.exec32(o.raw);
    if (c.len_ > o.len) c.stall_ += ((c.len_ - o.len) / 2) * c.fetch_wait16(c.pc_);
  }

  // An instruction naming XY-memory ports (r32-r55).
  static void xy16(Cpu& c, const DecodedOp& o) {
    const Cpu::XyUse u = c.xy_before(o.raw, true);
    gen16(c, o);
    c.xy_after(u);
  }
  static void xy32(Cpu& c, const DecodedOp& o) {
    const Cpu::XyUse u = c.xy_before(o.raw, false);
    gen32(c, o);
    c.xy_after(u);
  }

  static void nop(Cpu&, const DecodedOp&) {}

  // ---- moves and immediate arithmetic (16-bit forms never set flags) ----
  static void mov_r(Cpu& c, const DecodedOp& o) { c.setr(o.a, c.r_[o.b]); }
  static void mov_i(Cpu& c, const DecodedOp& o) { c.setr(o.a, o.imm); }
  static void add_rrr(Cpu& c, const DecodedOp& o) { c.setr(o.a, c.r_[o.b] + c.r_[o.c]); }
  static void sub_rrr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] - c.r_[o.c]; }
  static void and_rrr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] & c.r_[o.c]; }
  static void or_rrr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] | c.r_[o.c]; }
  static void bic_rrr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] & ~c.r_[o.c]; }
  static void xor_rrr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] ^ c.r_[o.c]; }
  template <unsigned N>
  static void addn_rrr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] + (c.r_[o.c] << N); }
  static void asl_rrr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] << (c.r_[o.c] & 31); }
  static void lsr_rrr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] >> (c.r_[o.c] & 31); }
  static void asr_rrr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = u32(s32(c.r_[o.b]) >> (c.r_[o.c] & 31)); }
  static void add_i(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] + o.imm; }
  static void sub_i(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] - o.imm; }
  static void asl_i(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] << o.imm; }
  static void lsr_i(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] >> o.imm; }
  static void asr_i(Cpu& c, const DecodedOp& o) { c.r_[o.a] = u32(s32(c.r_[o.b]) >> o.imm); }
  static void or_i(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] | o.imm; }
  static void and_i(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] & o.imm; }  // bmsk/bclr
  static void xor_i(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] ^ o.imm; }
  static void btst_i(Cpu& c, const DecodedOp& o) { c.flags_nz(c.r_[o.b] & o.imm); }
  static void tst_rr(Cpu& c, const DecodedOp& o) { c.flags_nz(c.r_[o.b] & c.r_[o.c]); }
  static void cmp_rr(Cpu& c, const DecodedOp& o) { c.alu_sub(c.r_[o.b], c.r_[o.c], 0, true); }
  static void cmp_i(Cpu& c, const DecodedOp& o) { c.alu_sub(c.r_[o.b], o.imm, 0, true); }
  static void sexb(Cpu& c, const DecodedOp& o) { c.r_[o.a] = sext(c.r_[o.b], 8); }
  static void sexw(Cpu& c, const DecodedOp& o) { c.r_[o.a] = sext(c.r_[o.b], 16); }
  static void extb(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] & 0xff; }
  static void extw(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.r_[o.b] & 0xffff; }
  static void not_r(Cpu& c, const DecodedOp& o) { c.r_[o.a] = ~c.r_[o.b]; }
  static void neg_r(Cpu& c, const DecodedOp& o) { c.r_[o.a] = 0u - c.r_[o.b]; }
  static void abs_r(Cpu& c, const DecodedOp& o) { const u32 v = c.r_[o.b]; c.r_[o.a] = (v >> 31) ? 0u - v : v; }

  // ---- loads / stores with a base register and constant offset ----
  static void ld32_abs(Cpu& c, const DecodedOp& o) { c.setr(o.a, c.rd32(o.imm)); }
  static void ld32(Cpu& c, const DecodedOp& o) { c.setr(o.a, c.rd32(c.r_[o.b] + o.imm)); }
  static void ld16(Cpu& c, const DecodedOp& o) { c.setr(o.a, c.rd16(c.r_[o.b] + o.imm)); }
  static void ld16x(Cpu& c, const DecodedOp& o) { c.setr(o.a, sext(c.rd16(c.r_[o.b] + o.imm), 16)); }
  static void ld8(Cpu& c, const DecodedOp& o) { c.setr(o.a, c.rd8(c.r_[o.b] + o.imm)); }
  static void ld8x(Cpu& c, const DecodedOp& o) { c.setr(o.a, sext(c.rd8(c.r_[o.b] + o.imm), 8)); }
  static void st32(Cpu& c, const DecodedOp& o) { c.wr32(c.r_[o.b] + o.imm, c.r_[o.c]); }
  static void st16(Cpu& c, const DecodedOp& o) { c.wr16(c.r_[o.b] + o.imm, u16(c.r_[o.c])); }
  static void st8(Cpu& c, const DecodedOp& o) { c.wr8(c.r_[o.b] + o.imm, u8(c.r_[o.c])); }
  static void ld32_rr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.rd32(c.r_[o.b] + c.r_[o.c]); }
  static void ld16_rr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.rd16(c.r_[o.b] + c.r_[o.c]); }
  static void ld8_rr(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.rd8(c.r_[o.b] + c.r_[o.c]); }
  static void push(Cpu& c, const DecodedOp& o) { c.r_[Cpu::kSP] -= 4; c.wr32(c.r_[Cpu::kSP], c.r_[o.b]); }
  static void pop(Cpu& c, const DecodedOp& o) { c.r_[o.a] = c.rd32(c.r_[Cpu::kSP]); c.r_[Cpu::kSP] += 4; }

  // ---- control flow (targets precomputed as absolute addresses) ----
  static void br(Cpu& c, const DecodedOp& o) {  // B / BL / Bcc / BLcc, 16- and 32-bit
    if (o.cc == 0 || c.cond(o.cc)) c.jump(o.imm, o.delay, o.a);
  }
  static void brz(Cpu& c, const DecodedOp& o) { if (c.r_[o.b] == 0) c.jump(o.imm, false, false); }
  static void brnz(Cpu& c, const DecodedOp& o) { if (c.r_[o.b] != 0) c.jump(o.imm, false, false); }
  static void jmp_r(Cpu& c, const DecodedOp& o) { c.jump(c.r_[o.b], o.delay, o.a); }
  template <unsigned CC, bool IMM>
  static void brcc(Cpu& c, const DecodedOp& o) {
    const u32 vb = c.r_[o.b], vc = IMM ? u32(o.c) : c.r_[o.c];
    bool take;
    if constexpr (CC == 0x0) take = vb == vc;
    else if constexpr (CC == 0x1) take = vb != vc;
    else if constexpr (CC == 0x2) take = s32(vb) < s32(vc);
    else if constexpr (CC == 0x3) take = s32(vb) >= s32(vc);
    else if constexpr (CC == 0x4) take = vb < vc;
    else if constexpr (CC == 0x5) take = vb >= vc;
    else if constexpr (CC == 0xe) take = !((vb >> (vc & 31)) & 1);
    else take = (vb >> (vc & 31)) & 1;
    if (take) c.jump(o.imm, o.delay, false);
  }

  // ---- 32-bit general ALU ops, P = 0/1/2 (register or immediate operand) ----
  template <int K, bool F>
  static u32 alu(Cpu& c, u32 s1, u32 s2) {
    u32 r;
    if constexpr (K == kAdd) return c.alu_add(s1, s2, 0, F);
    else if constexpr (K == kSub) return c.alu_sub(s1, s2, 0, F);
    else if constexpr (K == kRsub) return c.alu_sub(s2, s1, 0, F);
    else if constexpr (K == kAdd1) return c.alu_add(s1, s2 << 1, 0, F);
    else if constexpr (K == kAdd2) return c.alu_add(s1, s2 << 2, 0, F);
    else if constexpr (K == kAdd3) return c.alu_add(s1, s2 << 3, 0, F);
    else if constexpr (K == kSub1) return c.alu_sub(s1, s2 << 1, 0, F);
    else if constexpr (K == kSub2) return c.alu_sub(s1, s2 << 2, 0, F);
    else if constexpr (K == kSub3) return c.alu_sub(s1, s2 << 3, 0, F);
    else if constexpr (K == kAslM || K == kLsrM || K == kAsrM) {
      const unsigned n = s2 & 31;
      if constexpr (K == kAslM) r = s1 << n;
      else if constexpr (K == kLsrM) r = s1 >> n;
      else r = u32(s32(s1) >> n);
      if constexpr (F) {
        c.flags_nz(r);
        if (n) c.set_flag(Cpu::kC, K == kAslM ? (s1 >> (32 - n)) & 1 : (s1 >> (n - 1)) & 1);
      }
      return r;
    } else {
      if constexpr (K == kAnd) r = s1 & s2;
      else if constexpr (K == kOr) r = s1 | s2;
      else if constexpr (K == kBic) r = s1 & ~s2;
      else if constexpr (K == kXor) r = s1 ^ s2;
      else if constexpr (K == kBset) r = s1 | (1u << (s2 & 31));
      else if constexpr (K == kBclr) r = s1 & ~(1u << (s2 & 31));
      else {  // kBmsk
        const unsigned n = s2 & 31;
        r = s1 & (n == 31 ? 0xffffffffu : ((2u << n) - 1));
      }
      if constexpr (F) c.flags_nz(r);
      return r;
    }
  }
  template <int K, bool F, bool IMM>
  static void alu32(Cpu& c, const DecodedOp& o) {
    c.setr(o.a, alu<K, F>(c, c.r_[o.b], IMM ? o.imm : c.r_[o.c]));
  }
  template <bool F, bool IMM>
  static void mov32(Cpu& c, const DecodedOp& o) {
    const u32 v = IMM ? o.imm : c.r_[o.c];
    c.setr(o.a, v);
    if constexpr (F) c.flags_nz(v);
  }
  template <int K, bool IMM>  // 0 = TST, 1 = CMP, 2 = BTST, 3 = RCMP
  static void test32(Cpu& c, const DecodedOp& o) {
    const u32 s1 = c.r_[o.b], s2 = IMM ? o.imm : c.r_[o.c];
    if constexpr (K == 0) c.flags_nz(s1 & s2);
    else if constexpr (K == 1) c.alu_sub(s1, s2, 0, true);
    else if constexpr (K == 2) c.flags_nz(s1 & (1u << (s2 & 31)));
    else c.alu_sub(s2, s1, 0, true);
  }
};

namespace {

using Fn = void (*)(Cpu&, const DecodedOp&);

template <int K>
Fn alu_fn(bool f, bool imm) {
  if (f) return imm ? &Handlers::alu32<K, true, true> : &Handlers::alu32<K, true, false>;
  return imm ? &Handlers::alu32<K, false, true> : &Handlers::alu32<K, false, false>;
}
template <int K>
Fn test_fn(bool imm) {
  return imm ? &Handlers::test32<K, true> : &Handlers::test32<K, false>;
}
template <bool IMM>
Fn brcc_fn(unsigned cc) {
  switch (cc) {
    case 0x0: return &Handlers::brcc<0x0, IMM>;
    case 0x1: return &Handlers::brcc<0x1, IMM>;
    case 0x2: return &Handlers::brcc<0x2, IMM>;
    case 0x3: return &Handlers::brcc<0x3, IMM>;
    case 0x4: return &Handlers::brcc<0x4, IMM>;
    case 0x5: return &Handlers::brcc<0x5, IMM>;
    case 0xe: return &Handlers::brcc<0xe, IMM>;
    case 0xf: return &Handlers::brcc<0xf, IMM>;
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// What each handler does, for the JIT (jit.h). Unlisted handlers are called.

namespace {

struct ClassifyTable {
  std::unordered_map<Fn, JitOp> ops;
  void add(Fn fn, JitOp j) { ops[fn] = j; }
  template <int K>
  void alu() {
    for (int f = 0; f < 2; f++)
      for (int imm = 0; imm < 2; imm++) {
        JitOp j; j.kind = JitOp::Alu; j.op = u8(K); j.f = f; j.imm = imm;
        add(alu_fn<K>(f, imm), j);
      }
  }
  template <int K>
  void test() {
    for (int imm = 0; imm < 2; imm++) {
      JitOp j; j.kind = JitOp::Test; j.op = u8(K); j.imm = imm;
      add(test_fn<K>(imm), j);
    }
  }
  ClassifyTable() {
    using H = Handlers;
    auto kind = [](JitOp::Kind k, u8 op = 0, bool imm = false) { JitOp j; j.kind = k; j.op = op; j.imm = imm; return j; };
    auto load = [](u8 size, bool sx, bool rr) { JitOp j; j.kind = JitOp::Load; j.size = size; j.sext = sx; j.imm = !rr; return j; };
    auto store = [](u8 size) { JitOp j; j.kind = JitOp::Store; j.size = size; j.imm = true; return j; };
    add(&H::nop, kind(JitOp::Nop));
    add(&H::mov_r, kind(JitOp::Mov));
    add(&H::mov_i, kind(JitOp::Mov, 0, true));
    add(&H::add_rrr, kind(JitOp::Alu, JitOp::kAdd));
    add(&H::sub_rrr, kind(JitOp::Alu, JitOp::kSub));
    add(&H::and_rrr, kind(JitOp::Alu, JitOp::kAnd));
    add(&H::or_rrr, kind(JitOp::Alu, JitOp::kOr));
    add(&H::bic_rrr, kind(JitOp::Alu, JitOp::kBic));
    add(&H::xor_rrr, kind(JitOp::Alu, JitOp::kXor));
    add(&H::addn_rrr<1>, kind(JitOp::Alu, JitOp::kAdd1));
    add(&H::addn_rrr<2>, kind(JitOp::Alu, JitOp::kAdd2));
    add(&H::addn_rrr<3>, kind(JitOp::Alu, JitOp::kAdd3));
    add(&H::asl_rrr, kind(JitOp::Alu, JitOp::kAsl));
    add(&H::lsr_rrr, kind(JitOp::Alu, JitOp::kLsr));
    add(&H::asr_rrr, kind(JitOp::Alu, JitOp::kAsr));
    add(&H::add_i, kind(JitOp::Alu, JitOp::kAdd, true));
    add(&H::sub_i, kind(JitOp::Alu, JitOp::kSub, true));
    add(&H::asl_i, kind(JitOp::Alu, JitOp::kAsl, true));
    add(&H::lsr_i, kind(JitOp::Alu, JitOp::kLsr, true));
    add(&H::asr_i, kind(JitOp::Alu, JitOp::kAsr, true));
    add(&H::or_i, kind(JitOp::Alu, JitOp::kOr, true));
    add(&H::and_i, kind(JitOp::Alu, JitOp::kAnd, true));
    add(&H::xor_i, kind(JitOp::Alu, JitOp::kXor, true));
    add(&H::btst_i, kind(JitOp::Test, 0, true));
    add(&H::tst_rr, kind(JitOp::Test, 0));
    add(&H::cmp_rr, kind(JitOp::Test, 1));
    add(&H::cmp_i, kind(JitOp::Test, 1, true));
    add(&H::sexb, kind(JitOp::Unary, JitOp::kSexb));
    add(&H::sexw, kind(JitOp::Unary, JitOp::kSexw));
    add(&H::extb, kind(JitOp::Unary, JitOp::kExtb));
    add(&H::extw, kind(JitOp::Unary, JitOp::kExtw));
    add(&H::not_r, kind(JitOp::Unary, JitOp::kNot));
    add(&H::neg_r, kind(JitOp::Unary, JitOp::kNeg));
    add(&H::abs_r, kind(JitOp::Unary, JitOp::kAbs));
    add(&H::ld32_abs, load(4, false, false));
    add(&H::ld32, load(4, false, false));
    add(&H::ld16, load(2, false, false));
    add(&H::ld16x, load(2, true, false));
    add(&H::ld8, load(1, false, false));
    add(&H::ld8x, load(1, true, false));
    add(&H::st32, store(4));
    add(&H::st16, store(2));
    add(&H::st8, store(1));
    add(&H::ld32_rr, load(4, false, true));
    add(&H::ld16_rr, load(2, false, true));
    add(&H::ld8_rr, load(1, false, true));
    add(&H::push, kind(JitOp::Push));
    add(&H::pop, kind(JitOp::Pop));
    add(&H::br, kind(JitOp::Branch));
    add(&H::brz, kind(JitOp::BranchZ, 0));
    add(&H::brnz, kind(JitOp::BranchZ, 1));
    add(&H::jmp_r, kind(JitOp::Jump));
    for (unsigned cc : {0x0u, 0x1u, 0x2u, 0x3u, 0x4u, 0x5u, 0xeu, 0xfu}) {
      add(brcc_fn<false>(cc), kind(JitOp::BranchCmp, u8(cc), false));
      add(brcc_fn<true>(cc), kind(JitOp::BranchCmp, u8(cc), true));
    }
    alu<kAdd>(); alu<kSub>(); alu<kAnd>(); alu<kOr>(); alu<kBic>(); alu<kXor>(); alu<kRsub>();
    alu<kBset>(); alu<kBclr>(); alu<kBmsk>(); alu<kAdd1>(); alu<kAdd2>(); alu<kAdd3>();
    alu<kSub1>(); alu<kSub2>(); alu<kSub3>(); alu<kAslM>(); alu<kLsrM>(); alu<kAsrM>();
    test<0>(); test<1>(); test<2>(); test<3>();
    for (int f = 0; f < 2; f++)
      for (int imm = 0; imm < 2; imm++) {
        JitOp j = kind(JitOp::Mov, 0, imm);
        j.f = f;
        add(f ? (imm ? &H::mov32<true, true> : &H::mov32<true, false>) : (imm ? &H::mov32<false, true> : &H::mov32<false, false>), j);
      }
    add(&H::gen16, kind(JitOp::Generic));
    add(&H::gen32, kind(JitOp::Generic));
    add(&H::xy16, kind(JitOp::Generic));
    add(&H::xy32, kind(JitOp::Generic));
  }
};

static_assert(int(JitOp::kAsl) == int(kAslM) && int(JitOp::kAsr) == int(kAsrM) && int(JitOp::kSub3) == int(kSub3));

}  // namespace

// Instructions the interpreter runs on its reference path (gen32) that the
// JIT translates anyway, decoded from the instruction word. Semantics as in
// cpu.cpp (exec32 / exec_op04 / do_load / do_store).
static JitOp classify_gen32(u32 op) {
  JitOp j;
  j.kind = JitOp::Generic;
  const unsigned b = f_b(op), c = f_c(op), a = f_a(op), p = f_p(op);
  const bool F = op & 0x8000;
  // The decoder did not fetch long immediates for these forms: leave them to it.
  if (b == Cpu::kLIMM || c == Cpu::kLIMM) return j;
  auto scale_of = [](unsigned zz) { return u8(zz == 0 ? 2 : zz == 2 ? 1 : 0); };
  auto size_of = [](unsigned zz) { return u8(zz == 0 ? 4 : zz == 1 ? 1 : 2); };
  // (General operand forms, below: MUL64 / MULU64 are major 5, sub-op 4 / 5.)
  const bool mul = (op >> 27) == 0x05 && (((op >> 16) & 63) == 0x04 || ((op >> 16) & 63) == 0x05);
  switch (op >> 27) {
    case 0x02: {  // LD a,[b,s9] with address writeback / scaling
      const unsigned aa = (op >> 9) & 3, zz = (op >> 7) & 3;
      if (zz == 3 || b == Cpu::kPCL) return j;
      const u32 s9 = sext(((op >> 16) & 0xff) | (((op >> 15) & 1) << 8), 9);
      j.kind = JitOp::Load; j.size = size_of(zz); j.sext = (op >> 6) & 1;
      j.dst = int(a); j.s1 = int(b); j.imm = true;
      j.aa = u8(aa == 3 ? 0 : aa); j.k = aa == 3 ? s9 << scale_of(zz) : s9;
      return j;
    }
    case 0x03: {  // ST c,[b,s9] with address writeback / scaling
      const unsigned aa = (op >> 3) & 3, zz = (op >> 1) & 3;
      if (zz == 3 || b == Cpu::kPCL || c == Cpu::kPCL) return j;
      const u32 s9 = sext(((op >> 16) & 0xff) | (((op >> 15) & 1) << 8), 9);
      j.kind = JitOp::Store; j.size = size_of(zz);
      j.s1 = int(b); j.val = int(c); j.imm = true;
      j.aa = u8(aa == 3 ? 0 : aa); j.k = aa == 3 ? s9 << scale_of(zz) : s9;
      return j;
    }
    case 0x04: break;
    case 0x05: if (mul) break; return j;
    default: return j;
  }
  const unsigned sub = (op >> 16) & 63;
  if (b == Cpu::kPCL) return j;
  if (sub >= 0x30 && sub <= 0x37) {  // LD a,[b,c]
    const unsigned zz = (op >> 17) & 3;
    if (zz == 3 || c == Cpu::kPCL) return j;
    j.kind = JitOp::Load; j.size = size_of(zz); j.sext = (op >> 16) & 1;
    j.dst = int(a); j.s1 = int(b); j.s2 = int(c);
    j.aa = u8(p == 3 ? 0 : p); j.scale = p == 3 ? scale_of(zz) : 0;
    return j;
  }
  // The general operand forms: dst, s1, s2 (and the condition for p == 3).
  auto general = [&](JitOp& g) {
    if (p == 3) {
      if ((op & 0x1f) >= 0x10) return false;  // extension conditions: reference path
      g.xcc = u8(op & 0x1f);
      g.dst = int(b); g.s1 = int(b);
      if (op & 0x20) { g.imm = true; g.k = c; }
      else { if (c == Cpu::kPCL) return false; g.s2 = int(c); }
      return true;
    }
    if (p == 0) { if (c == Cpu::kPCL) return false; g.dst = int(a); g.s1 = int(b); g.s2 = int(c); }
    else if (p == 1) { g.dst = int(a); g.s1 = int(b); g.imm = true; g.k = c; }
    else { g.dst = int(b); g.s1 = int(b); g.imm = true; g.k = f_s12(op); }
    return true;
  };
  if (mul) {  // (results to MLO / MMID / MHI only; no flags)
    JitOp g; g.kind = JitOp::Mul; g.op = u8(sub == 0x05);
    if (!general(g)) return j;
    g.dst = -1;
    return g;
  }
  static const int kAluSub[0x1a] = {
      JitOp::kAdd, JitOp::kAdc, JitOp::kSub, JitOp::kSbc, JitOp::kAnd, JitOp::kOr, JitOp::kBic, JitOp::kXor, -1, -1, -1, -1, -1, -1,
      JitOp::kRsub, JitOp::kBset, JitOp::kBclr, -1, JitOp::kBxor, JitOp::kBmsk, JitOp::kAdd1, JitOp::kAdd2, JitOp::kAdd3,
      JitOp::kSub1, JitOp::kSub2, JitOp::kSub3};
  if (sub < 0x1a && kAluSub[sub] >= 0) {
    JitOp g; g.kind = JitOp::Alu; g.op = u8(kAluSub[sub]); g.f = F;
    return general(g) ? g : j;
  }
  if (sub == 0x0b || sub == 0x0c || sub == 0x0d || sub == 0x11) {  // TST CMP RCMP BTST
    JitOp g; g.kind = JitOp::Test; g.op = u8(sub == 0x0b ? 0 : sub == 0x0c ? 1 : sub == 0x0d ? 3 : 2);
    return general(g) ? g : j;
  }
  if (sub == 0x0a && p == 3) {  // conditional MOV
    JitOp g; g.kind = JitOp::Mov; g.f = F;
    if (!general(g)) return j;
    g.s1 = -1;
    return g;
  }
  if (sub >= 0x20 && sub <= 0x23 && p == 3 && !F && !(op & 0x20) && c != Cpu::kPCL && (op & 0x1f) < 0x10) {  // Jcc [c]
    JitOp g; g.kind = JitOp::Jump; g.xcc = u8(op & 0x1f); g.s1 = int(c); g.link = sub & 2; g.delay = sub & 1;
    return g;
  }
  if (sub == 0x2f && p <= 1) {  // single-operand group: b = op(c / u6)
    static const int kSop[12] = {JitOp::kAsl1, JitOp::kAsr1, JitOp::kLsr1, JitOp::kRor1, JitOp::kRrc, JitOp::kSexb,
                                 JitOp::kSexw, JitOp::kExtb, JitOp::kExtw, -1, JitOp::kNot, JitOp::kRlc};
    const unsigned sop = op & 63;
    if (sop >= 12 || kSop[sop] < 0 || (p == 0 && c == Cpu::kPCL)) return j;
    JitOp g; g.kind = JitOp::Unary; g.op = u8(kSop[sop]); g.f = F; g.dst = int(b);
    if (p == 0) g.s2 = int(c); else { g.imm = true; g.k = c; }
    return g;
  }
  return j;
}

JitOp jit_classify(const DecodedOp& o) {
  static const ClassifyTable table;
  if (o.fn == &Handlers::gen32) return classify_gen32(o.raw);
  if (o.fn == &Handlers::gen16 && ((o.raw >> 11) & 31) == 0x0f && (o.raw & 31) == 0x0c) {  // MUL64_S b,c
    static const int kR3[8] = {0, 1, 2, 3, 12, 13, 14, 15};
    JitOp g; g.kind = JitOp::Mul; g.op = 0;
    g.s1 = kR3[(o.raw >> 8) & 7]; g.s2 = kR3[(o.raw >> 5) & 7];
    return g;
  }
  const auto it = table.ops.find(o.fn);
  if (it == table.ops.end()) return JitOp{};
  JitOp j = it->second;
  // Operands, as each handler reads them.
  switch (j.kind) {
    case JitOp::Alu: j.dst = o.a; j.s1 = o.b; if (j.imm) j.k = o.imm; else j.s2 = o.c; break;
    case JitOp::Unary: j.dst = o.a; j.s2 = o.b; break;
    case JitOp::Mov:
      j.dst = o.a;
      if (j.imm) j.k = o.imm;
      else j.s2 = (o.fn == &Handlers::mov_r) ? o.b : o.c;
      break;
    case JitOp::Test: j.s1 = o.b; if (j.imm) j.k = o.imm; else j.s2 = o.c; break;
    case JitOp::Load:
      j.dst = o.a;
      if (o.fn == &Handlers::ld32_abs) { j.s1 = -1; j.k = o.imm; }
      else { j.s1 = o.b; if (j.imm) j.k = o.imm; else j.s2 = o.c; }
      break;
    case JitOp::Store: j.s1 = o.b; j.k = o.imm; j.val = o.c; break;
    case JitOp::Push: j.val = o.b; break;
    case JitOp::Pop: j.dst = o.a; break;
    case JitOp::Branch: j.cc = o.cc; j.k = o.imm; j.link = o.a; j.delay = o.delay; break;
    case JitOp::BranchZ: j.s1 = o.b; j.k = o.imm; break;
    case JitOp::Jump: j.s1 = o.b; j.link = o.a; j.delay = o.delay; break;
    case JitOp::BranchCmp:
      j.s1 = o.b; j.k = o.imm; j.delay = o.delay;
      if (j.imm) j.k2 = o.c; else j.s2 = o.c;
      break;
    default: break;
  }
  return j;
}

void Cpu::flush_decode_cache() {
  if (jit_) jit_->flush();  // compiled blocks point at decoded instructions
  bus_.unwatch_code();
  code_writes_.clear();
  cur_page_ = ~0u;
  cur_ops_ = nullptr;
  dcache_.clear();
  dcache_.resize(size_t(1) << 16);
  dcache_state_.assign(size_t(1) << 16, 0);
}

void Cpu::set_ram_code(u32 lo, u32 hi) {
  if (lo == ram_code_lo_ && hi == ram_code_hi_) return;
  ram_code_lo_ = lo;
  ram_code_hi_ = hi;
  flush_decode_cache();
}

// Bus::CodeHook: a write to a watched page. Only writes over instructions that
// have been decoded matter (data shares the pages); for those, the burst loop
// and compiled blocks are asked to stop after this instruction, and
// apply_code_writes() runs before the next.
void Cpu::code_written(void* ctx, u32 a, int size) {
  Cpu& c = *static_cast<Cpu*>(ctx);
  const u32 page = a >> 16;
  if (c.dcache_state_[page] != 1) return;
  const DecodedOp* ops = c.dcache_[page].get();
  const u32 first = std::max(a & ~1u, (page << 16) + 6) - 6;  // (an instruction is at most 8 bytes)
  for (u32 pc = first; pc < a + u32(size); pc += 2) {
    const DecodedOp& o = ops[(pc & Bus::kPageMask) >> 1];
    if (o.fn && pc + o.len > a && (pc >> 16) == page) {
      c.code_writes_.push_back({a, size});
      c.request_exit();
      return;
    }
  }
}

void Cpu::apply_code_writes() {
  bool any = false;
  for (const auto& [a, size] : code_writes_) {
    const u32 page = a >> 16;
    if (dcache_state_[page] != 1) continue;
    DecodedOp* ops = dcache_[page].get();
    const u32 first = std::max(a & ~1u, (page << 16) + 6) - 6;
    for (u32 pc = first; pc < a + u32(size) && (pc >> 16) == page; pc += 2) {
      DecodedOp& o = ops[(pc & Bus::kPageMask) >> 1];
      if (!o.fn || pc + o.len <= a) continue;
      // Decoded again in place: compiled blocks may point at it.
      decode(pc, o);
      o.cost8 = u16(8 + (o.len / 2) * fetch_wait16(pc));
      o.hook = pc_hooks_.count(pc) != 0;
      any = true;
    }
  }
  code_writes_.clear();
  if (any && jit_) jit_->flush();
}

DecodedOp* Cpu::decoded(u32 pc) {
  const u32 page = pc >> 16;
  if (dcache_.empty()) flush_decode_cache();
  switch (dcache_state_[page]) {
    case 1: break;
    case 2: return nullptr;
    default:
      // ROM is cached, and RAM in the set_ram_code range (its pages watched for
      // writes). Other code in RAM (the BaseROM's vectors, say) uses the
      // reference path, so it needs no invalidation.
      if (!bus_.is_rom(pc) && !(pc >= ram_code_lo_ && pc < ram_code_hi_ && bus_.watch_code(pc))) {
        dcache_state_[page] = 2;
        return nullptr;
      }
      dcache_[page] = std::make_unique<DecodedOp[]>(Bus::kPageSize / 2);
      dcache_state_[page] = 1;
  }
  DecodedOp& o = dcache_[page][(pc & Bus::kPageMask) >> 1];
  if (!o.fn) {
    decode(pc, o);
    o.cost8 = u16(8 + (o.len / 2) * fetch_wait16(pc));
    o.hook = pc_hooks_.count(pc) != 0;
  }
  return &o;
}

void Cpu::decode(u32 pc, DecodedOp& o) {
  const u16 hw = fetch16(pc);
  const unsigned major = hw >> 11;
  const u32 pcl = pc & ~3u;
  o = DecodedOp{};

  // Instructions naming XY-memory ports (r32-r55) run through the XY handlers.
  if (major >= 0x0c ? xy_fields(hw, true) : xy_fields((u32(hw) << 16) | fetch16(pc + 2), false)) [[unlikely]] {
    if (major >= 0x0c) {
      o.raw = hw;
      o.len = 2;
      o.fn = &Handlers::xy16;
    } else {
      o.raw = (u32(hw) << 16) | fetch16(pc + 2);
      o.len = 4;
      o.fn = &Handlers::xy32;
      if (f_b(o.raw) == kLIMM || f_c(o.raw) == kLIMM) { o.limm = fetch32(pc + 4); o.len = 8; }
    }
    return;
  }

  if (major >= 0x0c) {
    // ---------------- 16-bit ----------------
    const u16 op = hw;
    o.raw = op;
    o.len = 2;
    o.fn = &Handlers::gen16;
    const unsigned b = r16((op >> 8) & 7), c = r16((op >> 5) & 7), a = r16(op & 7);
    const u32 u5 = op & 0x1f;
    auto set = [&](Fn fn, unsigned ra, unsigned rb, unsigned rc = 0, u32 imm = 0) {
      o.fn = fn; o.a = u8(ra); o.b = u8(rb); o.c = u8(rc); o.imm = imm;
    };
    switch (major) {
      case 0x0c: {
        static const Fn f[4] = {&Handlers::ld32_rr, &Handlers::ld8_rr, &Handlers::ld16_rr, &Handlers::add_rrr};
        set(f[(op >> 3) & 3], a, b, c);
        break;
      }
      case 0x0d: {
        static const Fn f[4] = {&Handlers::add_i, &Handlers::sub_i, &Handlers::asl_i, &Handlers::asr_i};
        set(f[(op >> 3) & 3], c, b, 0, op & 7);
        break;
      }
      case 0x0e: {
        const unsigned h = ((op & 7) << 3) | ((op >> 5) & 7);
        const unsigned sub = (op >> 3) & 3;
        if (h == kLIMM && sub != 3) { o.limm = fetch32(pc + 2); o.len = 6; }
        if (h == kPCL) break;  // reads of PCL through h: keep the reference path
        switch (sub) {
          case 0: set(&Handlers::add_rrr, b, b, h); break;
          case 1: set(&Handlers::mov_r, b, h); break;
          case 2: set(&Handlers::cmp_rr, 0, b, h); break;
          case 3: set(&Handlers::mov_r, h, b); break;
        }
        break;
      }
      case 0x0f:
        switch (op & 0x1f) {
          case 0x00:
            switch ((op >> 5) & 7) {
              case 0: set(&Handlers::jmp_r, 0, b); break;
              case 1: set(&Handlers::jmp_r, 0, b); o.delay = 1; break;
              case 2: set(&Handlers::jmp_r, 1, b); break;
              case 3: set(&Handlers::jmp_r, 1, b); o.delay = 1; break;
              case 7:
                switch ((op >> 8) & 7) {
                  case 0: o.fn = &Handlers::nop; break;
                  case 6: set(&Handlers::jmp_r, 0, kBLINK); break;
                  case 7: set(&Handlers::jmp_r, 0, kBLINK); o.delay = 1; break;
                }
                break;
            }
            break;
          case 0x02: set(&Handlers::sub_rrr, b, b, c); break;
          case 0x04: set(&Handlers::and_rrr, b, b, c); break;
          case 0x05: set(&Handlers::or_rrr, b, b, c); break;
          case 0x06: set(&Handlers::bic_rrr, b, b, c); break;
          case 0x07: set(&Handlers::xor_rrr, b, b, c); break;
          case 0x0b: set(&Handlers::tst_rr, 0, b, c); break;
          case 0x0d: set(&Handlers::sexb, b, c); break;
          case 0x0e: set(&Handlers::sexw, b, c); break;
          case 0x0f: set(&Handlers::extb, b, c); break;
          case 0x10: set(&Handlers::extw, b, c); break;
          case 0x11: set(&Handlers::abs_r, b, c); break;
          case 0x12: set(&Handlers::not_r, b, c); break;
          case 0x13: set(&Handlers::neg_r, b, c); break;
          case 0x14: set(&Handlers::addn_rrr<1>, b, b, c); break;
          case 0x15: set(&Handlers::addn_rrr<2>, b, b, c); break;
          case 0x16: set(&Handlers::addn_rrr<3>, b, b, c); break;
          case 0x18: set(&Handlers::asl_rrr, b, b, c); break;
          case 0x19: set(&Handlers::lsr_rrr, b, b, c); break;
          case 0x1a: set(&Handlers::asr_rrr, b, b, c); break;
          case 0x1b: set(&Handlers::asl_i, b, c, 0, 1); break;
          case 0x1c: set(&Handlers::asr_i, b, c, 0, 1); break;
          case 0x1d: set(&Handlers::lsr_i, b, c, 0, 1); break;
        }
        break;
      case 0x10: set(&Handlers::ld32, c, b, 0, u5 << 2); break;
      case 0x11: set(&Handlers::ld8, c, b, 0, u5); break;
      case 0x12: set(&Handlers::ld16, c, b, 0, u5 << 1); break;
      case 0x13: set(&Handlers::ld16x, c, b, 0, u5 << 1); break;
      case 0x14: set(&Handlers::st32, 0, b, c, u5 << 2); break;
      case 0x15: set(&Handlers::st8, 0, b, c, u5); break;
      case 0x16: set(&Handlers::st16, 0, b, c, u5 << 1); break;
      case 0x17:
        switch ((op >> 5) & 7) {
          case 0: set(&Handlers::asl_i, b, b, 0, u5); break;
          case 1: set(&Handlers::lsr_i, b, b, 0, u5); break;
          case 2: set(&Handlers::asr_i, b, b, 0, u5); break;
          case 3: set(&Handlers::sub_i, b, b, 0, u5); break;
          case 4: set(&Handlers::or_i, b, b, 0, 1u << u5); break;
          case 5: set(&Handlers::and_i, b, b, 0, ~(1u << u5)); break;
          case 6: set(&Handlers::and_i, b, b, 0, u5 == 31 ? 0xffffffffu : ((2u << u5) - 1)); break;
          case 7: set(&Handlers::btst_i, 0, b, 0, 1u << u5); break;
        }
        break;
      case 0x18:
        switch ((op >> 5) & 7) {
          case 0: set(&Handlers::ld32, b, kSP, 0, u5 << 2); break;
          case 1: set(&Handlers::ld8, b, kSP, 0, u5 << 2); break;
          case 2: set(&Handlers::st32, 0, kSP, b, u5 << 2); break;
          case 3: set(&Handlers::st8, 0, kSP, b, u5 << 2); break;
          case 4: set(&Handlers::add_i, b, kSP, 0, u5 << 2); break;
          case 5:
            if (((op >> 8) & 7) == 0) set(&Handlers::add_i, kSP, kSP, 0, u5 << 2);
            else if (((op >> 8) & 7) == 1) set(&Handlers::sub_i, kSP, kSP, 0, u5 << 2);
            break;
          case 6:
            if (u5 == 0x01) set(&Handlers::pop, b, 0);
            else if (u5 == 0x11) set(&Handlers::pop, kBLINK, 0);
            break;
          case 7:
            if (u5 == 0x01) set(&Handlers::push, 0, b);
            else if (u5 == 0x11) set(&Handlers::push, 0, kBLINK);
            break;
        }
        break;
      case 0x19: {
        const u32 s9 = sext(op & 0x1ff, 9);
        switch ((op >> 9) & 3) {
          case 0: set(&Handlers::ld32, 0, kGP, 0, s9 << 2); break;
          case 1: set(&Handlers::ld8, 0, kGP, 0, s9); break;
          case 2: set(&Handlers::ld16, 0, kGP, 0, s9 << 1); break;
          case 3: set(&Handlers::add_i, 0, kGP, 0, s9 << 2); break;
        }
        break;
      }
      case 0x1a: set(&Handlers::ld32_abs, b, 0, 0, pcl + ((op & 0xff) << 2)); break;  // [pcl,u10]
      case 0x1b: set(&Handlers::mov_i, b, 0, 0, op & 0xff); break;
      case 0x1c:
        if (op & 0x80) set(&Handlers::cmp_i, 0, b, 0, op & 0x7f);
        else set(&Handlers::add_i, b, b, 0, op & 0x7f);
        break;
      case 0x1d:
        set((op & 0x80) ? &Handlers::brnz : &Handlers::brz, 0, b, 0, pcl + sext(op & 0x7f, 7) * 2);
        break;
      case 0x1e: {
        static constexpr u8 kCc[8] = {0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x06, 0x05, 0x0e};
        o.fn = &Handlers::br;
        switch ((op >> 9) & 3) {
          case 0: o.cc = 0; o.imm = pcl + sext(op & 0x1ff, 9) * 2; break;
          case 1: o.cc = 1; o.imm = pcl + sext(op & 0x1ff, 9) * 2; break;
          case 2: o.cc = 2; o.imm = pcl + sext(op & 0x1ff, 9) * 2; break;
          default: o.cc = kCc[(op >> 6) & 7]; o.imm = pcl + sext(op & 0x3f, 6) * 2;
        }
        break;
      }
      case 0x1f:
        o.fn = &Handlers::br;
        o.a = 1;  // link
        o.imm = pcl + sext(op & 0x7ff, 11) * 4;
        break;
    }
    return;
  }

  // ---------------- 32-bit ----------------
  const u32 op = (u32(hw) << 16) | fetch16(pc + 2);
  o.raw = op;
  o.len = 4;
  o.fn = &Handlers::gen32;
  auto limm_if = [&](unsigned r1, unsigned r2 = 0) {
    if (r1 == kLIMM || r2 == kLIMM) { o.limm = fetch32(pc + 4); o.len = 8; }
  };
  const bool F = op & 0x8000;
  const unsigned b = f_b(op), c = f_c(op), a = f_a(op), p = f_p(op);

  switch (major) {
    case 0x00: {  // Bcc s21 / B s25
      u32 off = ((op >> 17) & 0x3ff) | (((op >> 6) & 0x3ff) << 10);
      o.fn = &Handlers::br;
      o.delay = (op >> 5) & 1;
      if (op & 0x10000) { o.cc = 0; o.imm = pcl + sext(off | ((op & 0xf) << 20), 24) * 2; }
      else { o.cc = u8(op & 0x1f); o.imm = pcl + sext(off, 20) * 2; }
      if (o.cc >= 0x10) o.fn = &Handlers::gen32;
      return;
    }
    case 0x01: {
      if (!(op & 0x10000)) {  // BLcc / BL
        u32 off = (((op >> 18) & 0x1ff) << 1) | (((op >> 6) & 0x3ff) << 10);
        o.fn = &Handlers::br;
        o.a = 1;
        o.delay = (op >> 5) & 1;
        if (op & 0x20000) { o.cc = 0; o.imm = pcl + sext(off | ((op & 0xf) << 20), 24) * 2; }
        else { o.cc = u8(op & 0x1f); o.imm = pcl + sext(off, 20) * 2; }
        if (o.cc >= 0x10) o.fn = &Handlers::gen32;
        return;
      }
      const bool imm = op & 0x10;
      const Fn fn = imm ? brcc_fn<true>(op & 0xf) : brcc_fn<false>(op & 0xf);
      if (!fn || b == kPCL || (!imm && c == kPCL)) return;
      if (imm) limm_if(b); else limm_if(b, c);
      o.fn = fn;
      o.b = u8(b);
      o.c = u8(c);
      o.delay = (op >> 5) & 1;
      o.imm = pcl + sext(((op >> 17) & 0x7f) | (((op >> 15) & 1) << 7), 8) * 2;
      return;
    }
    case 0x02: {  // LD a,[b,s9] without address writeback
      const unsigned aa = (op >> 9) & 3, zz = (op >> 7) & 3;
      const bool x = (op >> 6) & 1;
      if (aa != 0 || zz == 3 || b == kPCL) return;
      limm_if(b);
      o.a = u8(a);
      o.b = u8(b);
      o.imm = sext(((op >> 16) & 0xff) | (((op >> 15) & 1) << 8), 9);
      o.fn = zz == 0 ? &Handlers::ld32 : zz == 1 ? (x ? &Handlers::ld8x : &Handlers::ld8)
                                                 : (x ? &Handlers::ld16x : &Handlers::ld16);
      return;
    }
    case 0x03: {  // ST c,[b,s9] without address writeback
      const unsigned aa = (op >> 3) & 3, zz = (op >> 1) & 3;
      if (aa != 0 || zz == 3 || b == kPCL || c == kPCL) return;
      limm_if(b, c);
      o.b = u8(b);
      o.c = u8(c);
      o.imm = sext(((op >> 16) & 0xff) | (((op >> 15) & 1) << 8), 9);
      o.fn = zz == 0 ? &Handlers::st32 : zz == 1 ? &Handlers::st8 : &Handlers::st16;
      return;
    }
    case 0x04:
    case 0x05: {
      const unsigned sub = (op >> 16) & 63;
      if (p == 3) return;  // conditional forms: reference path
      // PCL as an operand is rare and needs care with LIMM ordering; skip.
      if (b == kPCL || (p == 0 && c == kPCL)) return;
      const bool imm = p != 0;
      auto operands = [&](bool uses_b) {
        if (p == 0) { if (uses_b) limm_if(b, c); else limm_if(c); o.a = u8(a); o.c = u8(c); }
        else if (p == 1) { if (uses_b) limm_if(b); o.a = u8(a); o.imm = c; }
        else { if (uses_b) limm_if(b); o.a = u8(b); o.imm = f_s12(op); }
        o.b = u8(b);
      };
      Fn fn = nullptr;
      if (major == 0x05) {
        switch (sub) {
          case 0x00: fn = alu_fn<kAslM>(F, imm); break;
          case 0x01: fn = alu_fn<kLsrM>(F, imm); break;
          case 0x02: fn = alu_fn<kAsrM>(F, imm); break;
        }
        if (fn) { operands(true); o.fn = fn; }
        return;
      }
      switch (sub) {
        case 0x00: fn = alu_fn<kAdd>(F, imm); break;
        case 0x02: fn = alu_fn<kSub>(F, imm); break;
        case 0x04: fn = alu_fn<kAnd>(F, imm); break;
        case 0x05: fn = alu_fn<kOr>(F, imm); break;
        case 0x06: fn = alu_fn<kBic>(F, imm); break;
        case 0x07: fn = alu_fn<kXor>(F, imm); break;
        case 0x0e: fn = alu_fn<kRsub>(F, imm); break;
        case 0x0f: fn = alu_fn<kBset>(F, imm); break;
        case 0x10: fn = alu_fn<kBclr>(F, imm); break;
        case 0x13: fn = alu_fn<kBmsk>(F, imm); break;
        case 0x14: fn = alu_fn<kAdd1>(F, imm); break;
        case 0x15: fn = alu_fn<kAdd2>(F, imm); break;
        case 0x16: fn = alu_fn<kAdd3>(F, imm); break;
        case 0x17: fn = alu_fn<kSub1>(F, imm); break;
        case 0x18: fn = alu_fn<kSub2>(F, imm); break;
        case 0x19: fn = alu_fn<kSub3>(F, imm); break;
        case 0x0b: fn = test_fn<0>(imm); break;
        case 0x0c: fn = test_fn<1>(imm); break;
        case 0x0d: fn = test_fn<3>(imm); break;
        case 0x11: fn = test_fn<2>(imm); break;
        case 0x0a: {  // MOV b, c/u6/s12
          if (p == 0) { limm_if(c); o.c = u8(c); }
          else o.imm = p == 1 ? c : f_s12(op);
          o.a = u8(b);
          o.fn = F ? (imm ? &Handlers::mov32<true, true> : &Handlers::mov32<true, false>)
                   : (imm ? &Handlers::mov32<false, true> : &Handlers::mov32<false, false>);
          return;
        }
        case 0x20: case 0x21: case 0x22: case 0x23:  // Jcc [c] without .f
          if (p == 0 && !F && c != kPCL) {
            limm_if(c);
            o.fn = &Handlers::jmp_r;
            o.b = u8(c);
            o.a = (sub & 2) ? 1 : 0;
            o.delay = sub & 1;
          }
          return;
      }
      if (fn) { operands(true); o.fn = fn; }
      return;
    }
  }
}

}  // namespace leap::arc
