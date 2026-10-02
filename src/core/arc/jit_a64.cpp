// The JIT's code generator for AArch64 hosts (Linux, macOS, Windows, Android:
// any ARMv8-A; x18, reserved on some of them, is never used). It keeps the
// same rules as the x86-64 one (jit.h); see jit_x64.cpp for the reasoning
// behind each piece.
//
// Registers: x19 = Cpu*, x20 = the bus's page table (both callee-saved, set by
// the trampoline); w0-w15 and x16-x17 are scratch.

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

#include "core/arc/jit_internal.h"
#include "core/bus.h"

#ifdef LEAP_JIT_A64

#include "core/arc/a64.h"

namespace leap::arc {

using namespace a64;
using namespace jit_detail;

namespace {

constexpr Reg kCpu = 19, kPages = 20;

// Bit n of the mask: whether ARC condition `cc` holds when (STATUS32 >> 8) &
// 15 == n (V, C, N, Z), as in jit_x64.cpp.
u32 cond_mask(unsigned cc) {
  u32 m = 0;
  for (unsigned n = 0; n < 16; n++) {
    const bool v = n & 1, c = n & 2, neg = n & 4, z = n & 8;
    bool t = false;
    switch (cc) {
      case 0x00: t = true; break;
      case 0x01: t = z; break;
      case 0x02: t = !z; break;
      case 0x03: t = !neg; break;
      case 0x04: t = neg; break;
      case 0x05: t = c; break;
      case 0x06: t = !c; break;
      case 0x07: t = v; break;
      case 0x08: t = !v; break;
      case 0x09: t = !z && (neg == v); break;
      case 0x0a: t = neg == v; break;
      case 0x0b: t = neg != v; break;
      case 0x0c: t = z || (neg != v); break;
      case 0x0d: t = !c && !z; break;
      case 0x0e: t = c || z; break;
      case 0x0f: t = !neg && !z; break;
    }
    if (t) m |= 1u << n;
  }
  return m;
}

}  // namespace

// enter(cpu, code): saves x19-x30, sets up x19 and x20 and jumps to `code`.
// Blocks end by jumping to leave, which restores them and returns w0.
void Jit::emit_trampoline() {
  if (!arena_) return;
  Emitter e;
  e.stp_pre(29, 30, SP, -96);
  e.stp(19, 20, SP, 16);
  e.stp(21, 22, SP, 32);
  e.stp(23, 24, SP, 48);
  e.stp(25, 26, SP, 64);
  e.stp(27, 28, SP, 80);
  e.movx(kCpu, 0);
  e.mov64(kPages, u64(reinterpret_cast<uintptr_t>(bus_.pages_.data())));
  e.br(1);
  const size_t leave = e.size();
  e.ldp(27, 28, SP, 80);
  e.ldp(25, 26, SP, 64);
  e.ldp(23, 24, SP, 48);
  e.ldp(21, 22, SP, 32);
  e.ldp(19, 20, SP, 16);
  e.ldp_post(29, 30, SP, 96);
  e.ret();
  u8* at = reserve(e.size());
  write(at, e.code.data(), e.size());
  enter_ = reinterpret_cast<Enter>(at);
  leave_ = at + leave;
}

bool Jit::emit(u32 start, const std::vector<Insn>& insns, u32 guard, Emitted* out) {
  Cpu& c = cpu_;
  const s32 o_r = off(c, c.r_[0]), o_pc = off(c, c.pc_), o_status = off(c, c.status32_);
  const s32 o_stall = off(c, c.stall_), o_frac8 = off(c, c.frac8_), o_cycles = off(c, c.cycles_);
  const s32 o_next = off(c, c.next_pc_), o_redir = off(c, c.redirected_);
  const s32 o_dpend = off(c, c.delay_pending_), o_dtarget = off(c, c.delay_target_), o_dlink = off(c, c.delay_link_);
  const s32 o_attn = off(c, c.attention_), o_lpend = off(c, c.lp_end_), o_end8 = off(c, c.jit_end8_);
  const s32 o_idle = off(c, c.idle_hint);
  using Page = Bus::Page;
  static_assert(sizeof(Page) == 32, "the page-table walk below assumes 32-byte pages");
  const u32 o_rd = u32(offsetof(Page, rd)), o_wr = u32(offsetof(Page, wr));
  const u32 o_w16 = u32(offsetof(Page, wait16)), o_w32 = u32(offsetof(Page, wait32));
  const u32 last = insns.back().pc;

  Emitter e;
  Label ret1, bail0, bail2;
  std::vector<std::function<void()>> deferred;  // out-of-line code, emitted after the block
  struct Chain { Label slot, stub, slot_addr; };
  std::vector<std::unique_ptr<Chain>> chains;
  std::vector<Emitted::Reloc>& relocs = out->relocs;

  // A field of the Cpu: [x19 + off]. Far fields (the Cpu is large) go through
  // x17 = x19 + the offset's upper part.
  auto field = [&](s32 o, unsigned size, Reg* base) -> u32 {
    if (u32(o) / size < 4096) { *base = kCpu; return u32(o); }
    e.addsub_imm(0, 17, kCpu, u32(o) >> 12, true, true);
    *base = 17;
    return u32(o) & 0xfff;
  };
  auto ld = [&](Reg t, s32 o, unsigned size) { Reg b; const u32 x = field(o, size, &b); e.ldr(t, b, x, size); };
  auto st = [&](Reg t, s32 o, unsigned size) { Reg b; const u32 x = field(o, size, &b); e.str(t, b, x, size); };
  auto call = [&](const void* fn) {
    e.mov64(16, u64(reinterpret_cast<uintptr_t>(fn)));
    e.blr(16);
  };
  auto add_imm = [&](Reg d, Reg n, u32 v, bool x) {  // d = n + v (any v)
    if (v < 4096) e.addsub_imm(0, d, n, v, x);
    else { e.mov32(17, v); e.addsub(0, d, n, 17, 0, x); }
  };

  // Entry: run only if every instruction starts before the end of the time
  // slice, and no zero-overhead loop ends inside the block.
  ld(0, o_cycles, 8);
  e.lsl(0, 0, 3, true);
  ld(1, o_frac8, 4);
  e.addsub(0, 0, 0, 1, 0, true);
  if (guard) add_imm(0, 0, guard, true);
  ld(1, o_end8, 8);
  e.addsub(3, ZR, 0, 1, 0, true);  // cmp x0, x1
  e.bcond(HS, bail0);
  if (last != start) {  // start < lp_end <= last
    ld(0, o_lpend, 4);
    e.mov32(1, start + 1);
    e.addsub(2, 0, 0, 1);
    e.mov32(1, last - start);
    e.addsub(3, ZR, 0, 1);
    e.bcond(LO, bail2);
  }
  if (c.jit_block_hook) {
    e.movx(0, kCpu);
    e.mov32(1, start);
    call(reinterpret_cast<const void*>(&Cpu::jit_trace));
  }

  u32 prefix = 0;  // static cost of the instructions before the current one
  const Insn* cur = nullptr;
  u32 limm = 0;
  auto load_reg = [&](Reg h, int r) {
    if (r == Cpu::kLIMM) e.mov32(h, limm);
    else if (r == Cpu::kPCL) e.mov32(h, cur->pc & ~3u);
    else ld(h, o_r + 4 * r, 4);
  };
  auto store_reg = [&](int r, Reg h) { if (r >= 0 && r < int(Cpu::kLIMM)) st(h, o_r + 4 * r, 4); };
  // Charge the cost so far (the instructions before this one, and the data
  // waits until now); later waits are counted from -prefix.
  auto commit = [&](u32 pre) {
    ld(1, o_stall, 4);
    if (pre) add_imm(1, 1, pre, false);
    ld(2, o_frac8, 4);
    e.addsub(0, 1, 1, 2);
    e.lsr(2, 1, 3);
    ld(3, o_cycles, 8);
    e.addsub(0, 3, 3, 2, 0, true);
    st(3, o_cycles, 8);
    e.mov32(2, 7);
    e.logic(0, 1, 1, 2);
    st(1, o_frac8, 4);
    e.mov32(1, u32(0) - pre);
    st(1, o_stall, 4);
  };
  // Charge the whole block (through `cost`) and its waits.
  auto commit_all = [&](u32 cost) {
    ld(1, o_stall, 4);
    add_imm(1, 1, cost, false);
    ld(2, o_frac8, 4);
    e.addsub(0, 1, 1, 2);
    e.lsr(2, 1, 3);
    ld(3, o_cycles, 8);
    e.addsub(0, 3, 3, 2, 0, true);
    st(3, o_cycles, 8);
    e.mov32(2, 7);
    e.logic(0, 1, 1, 2);
    st(1, o_frac8, 4);
    st(ZR, o_stall, 4);
  };
  // Leave the block after instruction `in` (Cpu::jit_exit; len 0: set by a fallback).
  auto exit_after = [&](const Insn& in, u32 cost, u32 len) {
    e.movx(0, kCpu);
    e.mov32(1, in.pc);
    e.mov32(2, cost);
    e.mov32(3, len);
    call(reinterpret_cast<const void*>(&Cpu::jit_exit));
    e.b(ret1);
  };
  // Go on to `target` through a link slot (after charging).
  auto chain_to = [&](u32 target, u32 cost) {
    commit_all(cost);
    e.mov32(0, target);
    st(0, o_pc, 4);
    chains.push_back(std::make_unique<Chain>());
    e.ldr_lit(16, chains.back()->slot);
    e.br(16);
  };
  auto chainable = [&](u32 target) { return chain_ && !idle_pcs_.count(target) && (target >> 16) == (start >> 16); };
  // Go on to the address in w0 if its block is in the front cache (and it is
  // not the idle-loop head); else `miss`, with w0 kept.
  static_assert(sizeof(Recent) == 16, "the lookup below assumes 16-byte entries");
  auto chain_dynamic = [&](u32 cost, Label& miss) {
    if (!chain_) { e.b(miss); return; }
    ld(1, o_idle, 4);
    e.addsub(3, ZR, 0, 1);
    e.bcond(EQ, miss);
    e.ubfx(2, 0, 1, 14);  // (pc >> 1) & (kRecent - 1)
    static_assert(kRecent == 1u << 14, "the mask above assumes 2^14 entries");
    e.mov64(3, u64(reinterpret_cast<uintptr_t>(recent_.data())));
    e.addsub(0, 2, 3, 2, 4, true);  // x2 = x3 + (x2 << 4)
    e.ldr(4, 2, u32(offsetof(Recent, pc)), 4);
    e.addsub(3, ZR, 4, 0);
    e.bcond(NE, miss);
    e.ldr(7, 2, u32(offsetof(Recent, block)), 8);
    e.cbz(7, miss, true);
    e.mov(6, 0);
    commit_all(cost);
    st(6, o_pc, 4);
    e.ldr(16, 7, u32(offsetof(Block, code)), 8);
    e.br(16);
  };
  // STATUS32's flags: the ARC nibble (Z N C V, as bits 11-8 of STATUS32) in
  // w10, from the host's NZCV (C inverted after a subtraction: ARM's C is
  // "no borrow", ARC's "borrow").
  enum : u32 { FN = 4, FZ = 8, FC = 2, FV = 1 };  // (bits of the nibble)
  auto nzcv = [&](bool invert_c) {
    e.mrs_nzcv(10);
    e.lsr(10, 10, 28);    // N Z C V
    e.ubfx(11, 10, 3, 1);  // N
    e.ubfx(12, 10, 2, 1);  // Z
    e.bfi(10, 12, 3, 1);
    e.bfi(10, 11, 2, 1);   // Z N C V
    if (invert_c) { e.mov32(11, FC); e.logic(2, 10, 10, 11); }
  };
  // Merge `which` of the nibble in w10 into STATUS32.
  auto merge = [&](u32 which) {
    ld(9, o_status, 4);
    e.mov32(11, ~(which << 8));
    e.logic(0, 9, 9, 11);
    e.mov32(11, which);
    e.logic(0, 10, 10, 11);
    e.addsub(0, 9, 9, 10, 8);  // w9 |= w10 << 8 (those bits are clear: add = or)
    st(9, o_status, 4);
  };
  // N and Z of w0 (and C = bit 0 of w13 if `with_c`).
  auto nz_of = [&](Reg r, bool with_c) {
    e.logic(3, ZR, r, r);  // tst
    nzcv(false);
    if (with_c) e.bfi(10, 13, 1, 1);
    merge(with_c ? FN | FZ | FC : FN | FZ);
  };
  // Whether the ARC condition `cc` holds: falls through if so, else to `no`.
  auto cond_check = [&](unsigned cc, Label& no) {
    ld(0, o_status, 4);
    e.ubfx(0, 0, 8, 4);
    e.mov32(1, cond_mask(cc));
    e.shiftv(1, 1, 1, 0);  // lsr
    e.tbz(1, 0, no);
  };
  // The page of the address in w0: x2 = &pages[w0 >> 16].
  auto page_of = [&]() {
    e.lsr(2, 0, 16);
    e.addsub(0, 2, kPages, 2, 5, true);
  };
  const u32 align_mask[5] = {0, 0xffff, 0xfffe, 0, 0xfffc};  // in-page offset of an aligned access
  // Data read of `size` bytes at w0 into w0, with the wait states. I/O goes
  // out of line, after charging the time so far.
  auto read = [&](u8 size, bool sext) {
    Label slow, done;
    page_of();
    e.ldr(8, 2, size == 4 ? o_w32 : o_w16, 1);
    e.ldr(9, 2, o_rd, 8);
    e.cbz(9, slow, true);
    ld(3, o_stall, 4);
    e.addsub(0, 3, 3, 8);
    st(3, o_stall, 4);
    e.mov32(3, align_mask[size]);
    e.logic(0, 0, 0, 3);
    e.ldr_reg(0, 9, 0, size, sext);
    e.bind(done);
    const u32 pre = prefix, pc = cur->pc;
    Bus* bus = &bus_;
    deferred.push_back([&e, &commit, &st, &ld, &call, slow, done, pre, pc, size, sext, bus, o_pc, o_stall]() mutable {
      e.bind(slow);
      e.mov(21, 0);
      e.mov(22, 8);
      commit(pre);
      e.mov32(0, pc);
      st(0, o_pc, 4);
      e.mov64(0, u64(reinterpret_cast<uintptr_t>(bus)));
      e.mov32(2, size == 4 ? ~3u : size == 2 ? ~1u : ~0u);
      e.logic(0, 1, 21, 2);
      e.mov32(2, u32(size));
      call(reinterpret_cast<const void*>(&Jit::slow_read));
      if (size == 2) { if (sext) e.sxth(0, 0); else e.uxth(0, 0); }
      else if (size == 1) { if (sext) e.sxtb(0, 0); else e.uxtb(0, 0); }
      ld(3, o_stall, 4);
      e.addsub(0, 3, 3, 22);
      st(3, o_stall, 4);
      e.b(done);
    });
  };
  // Data write of the low `size` bytes of w1 to w0.
  auto write = [&](u8 size) {
    Label slow, done;
    page_of();
    e.ldr(8, 2, size == 4 ? o_w32 : o_w16, 1);
    e.ldr(9, 2, o_wr, 8);
    e.cbz(9, slow, true);
    ld(3, o_stall, 4);
    e.addsub(0, 3, 3, 8);
    st(3, o_stall, 4);
    e.mov32(3, align_mask[size]);
    e.logic(0, 0, 0, 3);
    e.str_reg(1, 9, 0, size);
    e.bind(done);
    const u32 pre = prefix, pc = cur->pc;
    Bus* bus = &bus_;
    deferred.push_back([&e, &commit, &st, &ld, &call, slow, done, pre, pc, size, bus, o_pc, o_stall]() mutable {
      e.bind(slow);
      e.mov(21, 0);
      e.mov(22, 8);
      e.mov(23, 1);
      commit(pre);
      e.mov32(0, pc);
      st(0, o_pc, 4);
      e.mov64(0, u64(reinterpret_cast<uintptr_t>(bus)));
      e.mov32(2, size == 4 ? ~3u : size == 2 ? ~1u : ~0u);
      e.logic(0, 1, 21, 2);
      if (size == 4) e.mov(2, 23);
      else if (size == 2) e.uxth(2, 23);
      else e.uxtb(2, 23);
      e.mov32(3, u32(size));
      call(reinterpret_cast<const void*>(&Jit::slow_write));
      ld(3, o_stall, 4);
      e.addsub(0, 3, 3, 22);
      st(3, o_stall, 4);
      e.b(done);
    });
  };
  // An I/O access can raise an interrupt or ask the loop to stop: leave after
  // this instruction if so.
  u32 exit_flag = 0;  // kSlotExit while compiling a delay slot
  auto check_attention = [&](const Insn& in, u32 cost) {
    Label out;
    ld(0, o_attn, 1);
    e.cbnz(0, out);
    const u32 len = in.len | exit_flag;
    deferred.push_back([&e, &exit_after, out, &in, cost, len]() mutable {
      e.bind(out);
      exit_after(in, cost, len);
    });
  };
  std::function<void(const Insn&)> emit_one;  // one instruction (below)
  // Taking a branch: the interpreter's Cpu::jump.
  auto take = [&](const Insn& in, bool delay, bool link, bool target_in_w0, u32 target, u32 cost) {
    if (delay && in.slot) {
      // The slot runs now, then the target.
      const Insn& s = *in.slot;
      if (target_in_w0) st(0, o_dtarget, 4);
      else { e.mov32(0, target & ~1u); st(0, o_dtarget, 4); }
      e.mov32(0, u32(link));
      st(0, o_dlink, 1);
      const u32 saved_prefix = prefix, saved_limm = limm;
      const Insn* saved_cur = cur;
      prefix = cost;
      exit_flag = kSlotExit;
      emit_one(s);
      exit_flag = 0;
      prefix = saved_prefix;
      cur = saved_cur;
      limm = saved_limm;
      const u32 cost2 = cost + s.cost8;
      auto set_blink = [&] { e.mov32(1, s.pc + s.len); st(1, o_r + 4 * Cpu::kBLINK, 4); };
      if (!target_in_w0 && chainable(target & ~1u)) {
        if (link) set_blink();
        chain_to(target & ~1u, cost2);
      } else {
        Label miss;
        if (target_in_w0) {
          ld(0, o_dtarget, 4);
          if (link) set_blink();
          chain_dynamic(cost2, miss);
        } else {
          e.b(miss);
        }
        e.bind(miss);
        exit_after(s, cost2, s.len | kSlotExit);
      }
      return;
    }
    if (delay) {
      e.mov32(1, 1);
      st(1, o_dpend, 1);
      if (target_in_w0) st(0, o_dtarget, 4);
      else { e.mov32(1, target & ~1u); st(1, o_dtarget, 4); }
      e.mov32(1, u32(link));
      st(1, o_dlink, 1);
    } else {
      if (link) { e.mov32(1, in.pc + in.len); st(1, o_r + 4 * Cpu::kBLINK, 4); }
      if (!target_in_w0 && chainable(target & ~1u)) {  // (a taken branch skips the loop check)
        chain_to(target & ~1u, cost);
        return;
      }
      if (target_in_w0) {
        Label miss;
        chain_dynamic(cost, miss);
        e.bind(miss);
        st(0, o_next, 4);
      } else {
        e.mov32(1, target & ~1u);
        st(1, o_next, 4);
      }
      e.mov32(1, 1);
      st(1, o_redir, 1);
    }
    exit_after(in, cost, in.len);
  };

  bool ended = false;  // the last instruction left the block on every path
  emit_one = [&](const Insn& in) {
    const JitOp& j = in.j;
    cur = &in;
    limm = in.o->limm;
    const u32 cost = prefix + in.cost8;  // through this instruction, before its data waits
    if (in.hook) {
      e.mov32(0, in.pc);
      st(0, o_pc, 4);
      const auto& h = c.pc_hooks_.at(in.pc);
      e.mov64(0, u64(reinterpret_cast<uintptr_t>(h.second)));
      e.movx(1, kCpu);
      call(reinterpret_cast<const void*>(h.first));
    }
    auto operand2 = [&](Reg h) { if (j.imm) e.mov32(h, j.k); else load_reg(h, j.s2); };
    // A conditional instruction does nothing when its condition fails.
    Label not_executed;
    if (j.xcc) cond_check(j.xcc, not_executed);

    switch (j.kind) {
      case JitOp::Nop:
        break;

      case JitOp::Mov:
        operand2(0);
        store_reg(j.dst, 0);
        if (j.f) nz_of(0, false);
        break;

      case JitOp::Mul:
        load_reg(0, j.s1);
        operand2(1);
        if (j.op == 0) e.smull(0, 0, 1); else e.umull(0, 0, 1);
        store_reg(Cpu::kMLO, 0);
        store_reg(Cpu::kMMID, 0);
        e.lsr(0, 0, 32, true);
        store_reg(Cpu::kMHI, 0);
        break;

      case JitOp::Unary: {
        operand2(0);
        switch (j.op) {
          case JitOp::kSexb: e.sxtb(0, 0); store_reg(j.dst, 0); if (j.f) nz_of(0, false); break;
          case JitOp::kSexw: e.sxth(0, 0); store_reg(j.dst, 0); if (j.f) nz_of(0, false); break;
          case JitOp::kExtb: e.uxtb(0, 0); store_reg(j.dst, 0); if (j.f) nz_of(0, false); break;
          case JitOp::kExtw: e.uxth(0, 0); store_reg(j.dst, 0); if (j.f) nz_of(0, false); break;
          case JitOp::kNot: e.logic(5, 0, ZR, 0); store_reg(j.dst, 0); if (j.f) nz_of(0, false); break;  // (orn)
          case JitOp::kNeg: e.addsub(2, 0, ZR, 0); store_reg(j.dst, 0); break;
          case JitOp::kAbs:
            e.addsub(2, 1, ZR, 0);           // w1 = -w0
            e.logic(3, ZR, 0, 0);            // tst w0
            e.csel(0, 1, 0, MI);             // negative: -w0
            store_reg(j.dst, 0);
            break;
          case JitOp::kAsl1:  // N, Z; C = bit 31; V = bit 31 changed: ADDS w0, w0, w0
            e.addsub(1, 0, 0, 0);
            store_reg(j.dst, 0);
            if (j.f) { nzcv(false); merge(FN | FZ | FC | FV); }
            break;
          case JitOp::kAsr1: case JitOp::kLsr1:
            e.ubfx(13, 0, 0, 1);  // C: the bit shifted out
            if (j.op == JitOp::kAsr1) e.asr(0, 0, 1); else e.lsr(0, 0, 1);
            store_reg(j.dst, 0);
            if (j.f) nz_of(0, true);
            break;
          case JitOp::kRor1:  // C: bit 0, which becomes bit 31
            e.ubfx(13, 0, 0, 1);
            e.mov32(1, 1);
            e.shiftv(3, 0, 0, 1);  // ror
            store_reg(j.dst, 0);
            if (j.f) nz_of(0, true);
            break;
          case JitOp::kRrc:  // through C: C into bit 31, bit 0 into C
            ld(1, o_status, 4);
            e.ubfx(1, 1, 9, 1);
            e.ubfx(13, 0, 0, 1);
            e.lsr(0, 0, 1);
            e.bfi(0, 1, 31, 1);
            store_reg(j.dst, 0);
            if (j.f) nz_of(0, true);
            break;
          case JitOp::kRlc:  // through C: C into bit 0, bit 31 into C
            ld(1, o_status, 4);
            e.ubfx(1, 1, 9, 1);
            e.ubfx(13, 0, 31, 1);
            e.lsl(0, 0, 1);
            e.logic(1, 0, 0, 1);
            store_reg(j.dst, 0);
            if (j.f) nz_of(0, true);
            break;
        }
        break;
      }

      case JitOp::Alu: {
        load_reg(0, j.s1);
        operand2(1);  // (the immediate in w1 too)
        auto store = [&] { store_reg(j.dst, 0); };
        // add / sub of s2 << sh (flags N, Z, C, V)
        auto arith = [&](bool sub, unsigned sh) {
          e.addsub(sub ? (j.f ? 3 : 2) : (j.f ? 1 : 0), 0, 0, 1, sh);
          store();
          if (j.f) { nzcv(sub); merge(FN | FZ | FC | FV); }
        };
        auto logic = [&](unsigned op) {  // and / or / eor / bic with w1 (flags N, Z)
          e.logic(op, 0, 0, 1);
          store();
          if (j.f) nz_of(0, false);
        };
        auto bit = [&](Reg d) { e.mov32(2, 1); e.shiftv(0, d, 2, 1); };  // d = 1 << (w1 & 31)
        switch (j.op) {
          case JitOp::kAdd: arith(false, 0); break;
          case JitOp::kSub: arith(true, 0); break;
          case JitOp::kAdd1: arith(false, 1); break;
          case JitOp::kAdd2: arith(false, 2); break;
          case JitOp::kAdd3: arith(false, 3); break;
          case JitOp::kSub1: arith(true, 1); break;
          case JitOp::kSub2: arith(true, 2); break;
          case JitOp::kSub3: arith(true, 3); break;
          case JitOp::kRsub:
            e.addsub(j.f ? 3 : 2, 0, 1, 0);
            store();
            if (j.f) { nzcv(true); merge(FN | FZ | FC | FV); }
            break;
          case JitOp::kAdc: case JitOp::kSbc: {  // with STATUS32.C as the carry / borrow in
            const bool sbc = j.op == JitOp::kSbc;
            ld(2, o_status, 4);
            e.ubfx(2, 2, 9, 1);
            if (sbc) { e.mov32(3, 1); e.logic(2, 2, 2, 3); }  // (ARM's C: no borrow)
            e.lsl(2, 2, 29);
            e.msr_nzcv(2);
            if (j.f) e.adcs(0, 0, 1, sbc); else e.adc(0, 0, 1, sbc);
            store();
            if (j.f) { nzcv(sbc); merge(FN | FZ | FC | FV); }
            break;
          }
          case JitOp::kBxor:
            if (j.imm) e.mov32(1, 1u << (j.k & 31)); else bit(1);
            logic(2);
            break;
          case JitOp::kAnd: logic(0); break;
          case JitOp::kOr: logic(1); break;
          case JitOp::kXor: logic(2); break;
          case JitOp::kBic: logic(4); break;
          case JitOp::kBset:
            if (j.imm) e.mov32(1, 1u << (j.k & 31)); else bit(1);
            logic(1);
            break;
          case JitOp::kBclr:
            if (j.imm) e.mov32(1, 1u << (j.k & 31)); else bit(1);
            logic(4);
            break;
          case JitOp::kBmsk: {
            const unsigned n = j.k & 31;
            if (j.imm) e.mov32(1, n == 31 ? 0xffffffffu : ((2u << n) - 1));
            else { e.mov32(2, 2); e.shiftv(0, 2, 2, 1); e.addsub_imm(2, 1, 2, 1); }  // (2 << (w1 & 31)) - 1
            logic(0);
            break;
          }
          case JitOp::kAsl: case JitOp::kLsr: case JitOp::kAsr: {
            const unsigned op = j.op == JitOp::kAsl ? 0 : j.op == JitOp::kLsr ? 1 : 2;
            if (!j.imm) { e.shiftv(op, 0, 0, 1); store(); break; }  // (no flags: jit.cpp)
            const unsigned n = j.k & 31;
            if (n == 0) { store(); if (j.f) nz_of(0, false); break; }
            if (j.f) e.ubfx(13, 0, op == 0 ? 32 - n : n - 1, 1);  // C: the last bit shifted out
            if (op == 0) e.lsl(0, 0, n); else if (op == 1) e.lsr(0, 0, n); else e.asr(0, 0, n);
            store();
            if (j.f) nz_of(0, true);
            break;
          }
        }
        break;
      }

      case JitOp::Test:
        load_reg(0, j.s1);
        operand2(1);
        switch (j.op) {
          case 0:  // tst
            e.logic(3, ZR, 0, 1);
            nzcv(false);
            merge(FN | FZ);
            break;
          case 1:  // cmp
            e.addsub(3, ZR, 0, 1);
            nzcv(true);
            merge(FN | FZ | FC | FV);
            break;
          case 2:  // btst
            if (j.imm) e.mov32(1, 1u << (j.k & 31));
            else { e.mov32(2, 1); e.shiftv(0, 1, 2, 1); }
            e.logic(3, ZR, 0, 1);
            nzcv(false);
            merge(FN | FZ);
            break;
          default:  // rcmp: s2 - s1
            e.addsub(3, ZR, 1, 0);
            nzcv(true);
            merge(FN | FZ | FC | FV);
            break;
        }
        break;

      case JitOp::Load:
        // (Cpu::do_load: mode 1 writes the base back before the load, mode 2
        // after the destination, from the base's original value.)
        if (j.s1 < 0) e.mov32(0, j.k);
        else {
          load_reg(0, j.s1);
          if (!j.imm) { load_reg(1, j.s2); if (j.scale) e.lsl(1, 1, j.scale); }
          else e.mov32(1, j.k);
          if (j.aa == 2) {
            e.addsub(0, 24, 0, 1);  // the new base (w24 survives the read's slow path)
          } else {
            e.addsub(0, 0, 0, 1);
            if (j.aa == 1) store_reg(j.s1, 0);
          }
        }
        read(j.size, j.sext);
        store_reg(j.dst, 0);
        if (j.aa == 2) store_reg(j.s1, 24);
        check_attention(in, cost);
        break;

      case JitOp::Store:
        // (Cpu::do_store: the value is read after a mode-1 writeback; mode 2
        // writes base + offset back after the store.)
        load_reg(0, j.s1);
        if (j.aa != 2 && j.k) add_imm(0, 0, j.k, false);
        if (j.aa == 1) store_reg(j.s1, 0);
        load_reg(1, j.val);
        write(j.size);
        if (j.aa == 2) {
          load_reg(0, j.s1);
          if (j.k) add_imm(0, 0, j.k, false);
          store_reg(j.s1, 0);
        }
        check_attention(in, cost);
        break;

      case JitOp::Push:
        load_reg(0, Cpu::kSP);
        e.addsub_imm(2, 0, 0, 4);
        store_reg(Cpu::kSP, 0);
        load_reg(1, j.val);
        write(4);
        check_attention(in, cost);
        break;

      case JitOp::Pop:
        load_reg(0, Cpu::kSP);
        read(4, false);
        store_reg(j.dst, 0);
        load_reg(0, Cpu::kSP);
        e.addsub_imm(0, 0, 0, 4);
        store_reg(Cpu::kSP, 0);
        check_attention(in, cost);
        break;

      case JitOp::Branch: {
        Label skip;
        if (j.cc != 0) cond_check(j.cc, skip);
        take(in, j.delay, j.link, false, j.k, cost);
        if (j.cc != 0) e.bind(skip);
        else ended = true;
        break;
      }

      case JitOp::BranchZ: {
        Label skip;
        load_reg(0, j.s1);
        if (j.op == 0) e.cbnz(0, skip); else e.cbz(0, skip);
        take(in, false, false, false, j.k, cost);
        e.bind(skip);
        break;
      }

      case JitOp::Jump:
        load_reg(0, j.s1);
        e.mov32(1, ~1u);
        e.logic(0, 0, 0, 1);
        take(in, j.delay, j.link, true, 0, cost);
        if (!j.xcc) ended = true;
        break;

      case JitOp::BranchCmp: {
        Label skip;
        load_reg(0, j.s1);
        if (j.op == 0xe || j.op == 0xf) {  // bbit0 / bbit1: skip if the bit is set / clear
          if (j.imm) {
            if (j.op == 0xe) e.tbnz(0, j.k2 & 31, skip); else e.tbz(0, j.k2 & 31, skip);
          } else {
            load_reg(1, j.s2);
            e.shiftv(1, 0, 0, 1);  // lsr by (w1 & 31)
            if (j.op == 0xe) e.tbnz(0, 0, skip); else e.tbz(0, 0, skip);
          }
        } else {
          if (j.imm) e.mov32(1, j.k2); else load_reg(1, j.s2);
          e.addsub(3, ZR, 0, 1);
          static constexpr Cond kTaken[6] = {EQ, NE, LT, GE, LO, HS};
          e.bcond(Cond(kTaken[j.op] ^ 1), skip);  // (conditions come in complementary pairs)
        }
        take(in, j.delay, false, false, j.k, cost);
        e.bind(skip);
        break;
      }

      case JitOp::Generic:
      case JitOp::Other: {
        Label out;
        e.movx(0, kCpu);
        e.mov64(1, u64(reinterpret_cast<uintptr_t>(in.o)));
        e.mov32(2, in.pc);
        e.mov32(3, prefix | (in.len << 24));
        call(reinterpret_cast<const void*>(&Cpu::jit_generic));
        e.cbnz(0, out);
        deferred.push_back([&e, &exit_after, out, &in, cost]() mutable {
          e.bind(out);
          exit_after(in, cost, 0);
        });
        break;
      }
    }
    if (j.xcc) e.bind(not_executed);
  };
  for (const Insn& in : insns) {
    emit_one(in);
    prefix += in.cost8;
  }
  // Falling off the end: on to the next instruction, unless a zero-overhead
  // loop ends there (Cpu::jit_exit runs the interpreter's loop check).
  if (!ended) {
    const Insn& in = insns.back();
    const u32 seq = in.pc + in.len;
    if (chainable(seq)) {
      Label looped;
      ld(0, o_lpend, 4);
      e.mov32(1, seq);
      e.addsub(3, ZR, 0, 1);
      e.bcond(EQ, looped);
      chain_to(seq, prefix);
      e.bind(looped);
    }
    exit_after(in, prefix, in.len);
  }

  const u64 leave = u64(reinterpret_cast<uintptr_t>(leave_));
  auto leave_with = [&](u32 v) {
    e.mov32(0, v);
    e.mov64(16, leave);
    e.br(16);
  };
  e.bind(ret1);
  leave_with(1);
  e.bind(bail0);
  leave_with(0);
  e.bind(bail2);
  leave_with(2);
  for (auto& d : deferred) d();
  // Link stubs: note which slot to fill (link()), and return to the dispatcher.
  for (auto& ch : chains) {
    e.bind(ch->stub);
    e.ldr_lit(0, ch->slot_addr);
    e.mov64(1, u64(reinterpret_cast<uintptr_t>(&link_slot_)));
    e.str(0, 1, 0, 8);
    leave_with(1);
  }
  // Literals: each slot (the address an exit jumps to: its stub until linked),
  // and each slot's own address (for its stub).
  e.align(8);
  for (auto& ch : chains) {
    e.bind(ch->slot);
    relocs.push_back({int(e.size()), ch->stub.pos});
    e.u64le(0);
    e.bind(ch->slot_addr);
    relocs.push_back({int(e.size()), ch->slot.pos});
    e.u64le(0);
  }
  if (e.bad) return false;
  out->code = std::move(e.code);
  return true;
}

}  // namespace leap::arc

#endif  // LEAP_JIT_A64
