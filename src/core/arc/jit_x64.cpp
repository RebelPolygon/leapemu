// The JIT's code generator for x86-64 hosts (System V and Windows calling
// conventions). See jit.h for the rules the code keeps.

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

#include "core/arc/jit_internal.h"
#include "core/bus.h"

#ifdef LEAP_JIT_X64

#include "core/arc/x64.h"

namespace leap::arc {

using namespace x64;
using namespace jit_detail;

namespace {

#ifdef _WIN32
constexpr Reg kArg[4] = {RCX, RDX, R8, R9};
#else
constexpr Reg kArg[4] = {RDI, RSI, RDX, RCX};
#endif

// x86 condition-code test of STATUS32's flags: bit n of the mask is whether
// ARC condition `cc` holds when (STATUS32 >> 8) & 15 == n (V, C, N, Z).
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

// enter(cpu, code): saves the callee-saved registers, sets up rbx = Cpu* and
// r12 = the bus's page table, and jumps to `code`. Blocks end by jumping to
// leave, which restores them and returns eax. r13-r15 are scratch that
// survives calls; the stack stays 16-byte aligned with 32 bytes of shadow
// space (Windows) for calls from blocks.
void Jit::emit_trampoline() {
  if (!arena_) return;
  Emitter e;
  e.push(RBX); e.push(R12); e.push(R13); e.push(R14); e.push(R15);
  e.alu64(Emitter::SUB, RSP, 32);
  e.mov64(RBX, kArg[0]);
  e.mov64(R12, u64(reinterpret_cast<uintptr_t>(bus_.pages_.data())));
  e.jmp(kArg[1]);
  const size_t leave = e.size();
  e.alu64(Emitter::ADD, RSP, 32);
  e.pop(R15); e.pop(R14); e.pop(R13); e.pop(R12); e.pop(RBX);
  e.ret();
  u8* at = reserve(e.size());
  write(at, e.code.data(), e.size());
  enter_ = reinterpret_cast<Enter>(at);
  leave_ = at + leave;
}

bool Jit::emit(u32 start, const std::vector<Insn>& insns, u32 guard, Emitted* out) {
  Cpu& c = cpu_;
  // ---- offsets into the CPU and the bus's page table ----
  const s32 o_r = off(c, c.r_[0]), o_pc = off(c, c.pc_), o_status = off(c, c.status32_);
  const s32 o_stall = off(c, c.stall_), o_frac8 = off(c, c.frac8_), o_cycles = off(c, c.cycles_);
  const s32 o_next = off(c, c.next_pc_), o_redir = off(c, c.redirected_);
  const s32 o_dpend = off(c, c.delay_pending_), o_dtarget = off(c, c.delay_target_), o_dlink = off(c, c.delay_link_);
  const s32 o_attn = off(c, c.attention_), o_lpend = off(c, c.lp_end_), o_end8 = off(c, c.jit_end8_);
  using Page = Bus::Page;
  static_assert(sizeof(Page) == 32, "the page-table walk below assumes 32-byte pages");
  const s32 o_rd = s32(offsetof(Page, rd)), o_wr = s32(offsetof(Page, wr));
  const s32 o_w16 = s32(offsetof(Page, wait16)), o_w32 = s32(offsetof(Page, wait32));
  auto reg_mem = [&](int r) { return mem(RBX, o_r + 4 * r); };

  Emitter e;
  Label ret1, bail0, bail2;
  std::vector<std::function<void()>> deferred;  // out-of-line code, emitted after the block
  // Links to other blocks: each exit jumps through an 8-byte slot, which
  // first holds the address of a stub that returns to the dispatcher (see
  // link()). Absolute addresses are filled in once the code's place is known.
  struct Chain { Label slot, stub; int stub_imm = 0; };
  std::vector<std::unique_ptr<Chain>> chains;
  std::vector<Emitted::Reloc>& relocs = out->relocs;

  const u32 last = insns.back().pc;

  // Entry: run only if every instruction starts before the end of the time
  // slice, and no zero-overhead loop ends inside the block (the interpreter
  // looks for one after every instruction).
  e.mov64(RAX, mem(RBX, o_cycles));
  e.shift64(Emitter::SHL, RAX, 3);
  e.mov(RCX, mem(RBX, o_frac8));
  e.alu64(Emitter::ADD, RAX, RCX);
  if (guard) e.alu64(Emitter::ADD, RAX, guard);
  e.alu64(Emitter::CMP, RAX, mem(RBX, o_end8));
  e.jcc(AE, bail0);
  if (last != start) {  // start < lp_end <= last
    e.mov(RAX, mem(RBX, o_lpend));
    e.alu(Emitter::SUB, RAX, start + 1);
    e.alu(Emitter::CMP, RAX, last - start);
    e.jcc(B, bail2);
  }

  if (c.jit_block_hook) {  // debugging: report each start
    e.mov64(kArg[0], RBX);
    e.mov(kArg[1], start);
    e.call(reinterpret_cast<const void*>(&Cpu::jit_trace));
  }

  u32 prefix = 0;   // static cost of the instructions before the current one
  const Insn* cur = nullptr;
  u32 limm = 0;

  // The register the code just stored, and from where: reloading it right after
  // (nothing emitted since, no label bound there) needs no memory access.
  size_t stored_end = ~size_t(0);
  int stored_reg = -1;
  Reg stored_from = RAX;
  auto load_reg = [&](Reg h, int r) {
    if (r == Cpu::kLIMM) e.mov(h, limm);
    else if (r == Cpu::kPCL) e.mov(h, cur->pc & ~3u);
    else if (r == stored_reg && e.size() == stored_end && e.last_bind != stored_end) {
      if (h != stored_from) e.mov(h, stored_from);
    } else e.mov(h, reg_mem(r));
  };
  auto store_reg = [&](int r, Reg h) {
    if (r >= 0 && r < int(Cpu::kLIMM)) {
      e.mov(reg_mem(r), h);
      stored_end = e.size();
      stored_reg = r;
      stored_from = h;
    }
  };
  // Charge the cost so far (the instructions before this one, and the data
  // waits until now); later waits are counted from -prefix, so the block's
  // exit charges the right total.
  auto commit = [&](u32 pre) {
    e.mov(RCX, mem(RBX, o_stall));
    if (pre) e.alu(Emitter::ADD, RCX, pre);
    e.alu(Emitter::ADD, RCX, mem(RBX, o_frac8));
    e.mov(RDX, RCX);
    e.shift(Emitter::SHR, RDX, 3);
    e.alu64(Emitter::ADD, mem(RBX, o_cycles), RDX);
    e.alu(Emitter::AND, RCX, 7);
    e.mov(mem(RBX, o_frac8), RCX);
    e.mov(mem(RBX, o_stall), u32(0) - pre);
  };
  // Leave the block after instruction `in`: Cpu::jit_exit charges and moves
  // on the way the interpreter's loop does (len 0: set by a fallback).
  auto exit_after = [&](const Insn& in, u32 cost, u32 len) {
    e.mov64(kArg[0], RBX);
    e.mov(kArg[1], in.pc);
    e.mov(kArg[2], cost);
    e.mov(kArg[3], len);
    e.call(reinterpret_cast<const void*>(&Cpu::jit_exit));
    e.jmp(ret1);
  };
  // Charge the whole block (through `cost`) and its waits.
  auto commit_all = [&](u32 cost) {
    e.mov(RCX, mem(RBX, o_stall));
    e.alu(Emitter::ADD, RCX, cost);
    e.alu(Emitter::ADD, RCX, mem(RBX, o_frac8));
    e.mov(RDX, RCX);
    e.shift(Emitter::SHR, RDX, 3);
    e.alu64(Emitter::ADD, mem(RBX, o_cycles), RDX);
    e.alu(Emitter::AND, RCX, 7);
    e.mov(mem(RBX, o_frac8), RCX);
    e.mov(mem(RBX, o_stall), 0u);
  };
  // Go on to `target` through a link slot (after charging).
  auto chain_to = [&](u32 target, u32 cost) {
    commit_all(cost);
    e.mov(mem(RBX, o_pc), target);
    chains.push_back(std::make_unique<Chain>());
    e.jmp_indirect(chains.back()->slot);
  };
  auto chainable = [&](u32 target) { return chain_ && !idle_pcs_.count(target) && (target >> 16) == (start >> 16); };
  // Go on to the address in eax if its block is in the front cache (and it
  // is not the idle-loop head, which the dispatcher must see); else `miss`,
  // with eax kept.
  const s32 o_idle = off(c, c.idle_hint);
  static_assert(sizeof(Recent) == 16, "the lookup below assumes 16-byte entries");
  auto chain_dynamic = [&](u32 cost, Label& miss) {
    if (!chain_) { e.jmp(miss); return; }
    e.alu(Emitter::CMP, RAX, mem(RBX, o_idle));
    e.jcc(E, miss);
    e.mov(RDX, RAX);
    e.shift(Emitter::SHR, RDX, 1);
    e.alu(Emitter::AND, RDX, u32(kRecent - 1));
    e.shift64(Emitter::SHL, RDX, 4);
    e.mov64(RCX, u64(reinterpret_cast<uintptr_t>(recent_.data())));
    e.alu64(Emitter::ADD, RDX, RCX);
    e.alu(Emitter::CMP, RAX, mem(RDX, s32(offsetof(Recent, pc))));
    e.jcc(NE, miss);
    e.mov64(R14, mem(RDX, s32(offsetof(Recent, block))));
    e.test64(R14, R14);
    e.jcc(E, miss);
    e.mov(R13, RAX);
    commit_all(cost);
    e.mov(mem(RBX, o_pc), R13);
    e.mov64(RAX, mem(R14, s32(offsetof(Block, code))));
    e.jmp(RAX);
  };
  // STATUS32 flags from the x86 flags of the last operation. The SETcc must
  // follow it directly; `between` runs after them (e.g. storing the result).
  enum : u32 { FN = 1, FZ = 2, FC = 4, FV = 8 };
  auto flags = [&](u32 which, const std::function<void()>& between) {
    if (which & FN) e.setcc(S, R8);
    if (which & FZ) e.setcc(E, R9);
    if (which & FC) e.setcc(B, R10);
    if (which & FV) e.setcc(O, R11);
    if (between) between();
    u32 clear = 0;
    if (which & FN) clear |= Cpu::kN;
    if (which & FZ) clear |= Cpu::kZ;
    if (which & FC) clear |= Cpu::kC;
    if (which & FV) clear |= Cpu::kV;
    e.mov(RDX, mem(RBX, o_status));
    e.alu(Emitter::AND, RDX, ~clear);
    auto merge = [&](Reg r, u8 bit) {
      e.movzx8(r, r);
      e.shift(Emitter::SHL, r, bit);
      e.alu(Emitter::OR, RDX, r);
    };
    if (which & FN) merge(R8, 10);
    if (which & FZ) merge(R9, 11);
    if (which & FC) merge(R10, 9);
    if (which & FV) merge(R11, 8);
    e.mov(mem(RBX, o_status), RDX);
  };
  // The page of the address in eax: rdx = &pages[eax >> 16].
  auto page_of = [&]() {
    e.mov(RDX, RAX);
    e.shift(Emitter::SHR, RDX, 16);
    e.shift64(Emitter::SHL, RDX, 5);
    e.alu64(Emitter::ADD, RDX, R12);
  };
  const u32 align_mask[5] = {0, 0xffff, 0xfffe, 0, 0xfffc};  // in-page offset of an aligned access
  // Data read of `size` bytes at eax into eax, with the wait states. I/O goes
  // out of line, after charging the time so far.
  auto read = [&](u8 size, bool sext) {
    Label slow, done;
    page_of();
    e.movzx8(R8, mem(RDX, size == 4 ? o_w32 : o_w16));
    e.mov64(R9, mem(RDX, o_rd));
    e.test64(R9, R9);
    e.jcc(E, slow);
    e.alu(Emitter::ADD, mem(RBX, o_stall), R8);
    e.alu(Emitter::AND, RAX, align_mask[size]);
    const Mem m = mem(R9, RAX);
    if (size == 4) e.mov(RAX, m);
    else if (size == 2) { if (sext) e.movsx16(RAX, m); else e.movzx16(RAX, m); }
    else { if (sext) e.movsx8(RAX, m); else e.movzx8(RAX, m); }
    e.bind(done);
    const u32 pre = prefix, pc = cur->pc;
    Bus* bus = &bus_;
    deferred.push_back([&e, &commit, slow, done, pre, pc, size, sext, bus, o_pc, o_stall]() mutable {
      e.bind(slow);
      e.mov(R13, RAX);
      e.mov(R14, R8);
      commit(pre);
      e.mov(mem(RBX, o_pc), pc);
      e.mov64(kArg[0], u64(reinterpret_cast<uintptr_t>(bus)));
      e.mov(kArg[1], R13);
      e.alu(Emitter::AND, kArg[1], size == 4 ? ~3u : size == 2 ? ~1u : ~0u);
      e.mov(kArg[2], u32(size));
      e.call(reinterpret_cast<const void*>(&Jit::slow_read));
      if (size == 2) { if (sext) e.movsx16(RAX, RAX); else e.movzx16(RAX, RAX); }
      else if (size == 1) { if (sext) e.movsx8(RAX, RAX); else e.movzx8(RAX, RAX); }
      e.alu(Emitter::ADD, mem(RBX, o_stall), R14);
      e.jmp(done);
    });
  };
  // Data write of the low `size` bytes of ecx to eax.
  auto write = [&](u8 size) {
    Label slow, done;
    page_of();
    e.movzx8(R8, mem(RDX, size == 4 ? o_w32 : o_w16));
    e.mov64(R9, mem(RDX, o_wr));
    e.test64(R9, R9);
    e.jcc(E, slow);
    e.alu(Emitter::ADD, mem(RBX, o_stall), R8);
    e.alu(Emitter::AND, RAX, align_mask[size]);
    const Mem m = mem(R9, RAX);
    if (size == 4) e.mov(m, RCX);
    else if (size == 2) e.mov16(m, RCX);
    else e.mov8(m, RCX);
    e.bind(done);
    const u32 pre = prefix, pc = cur->pc;
    Bus* bus = &bus_;
    deferred.push_back([&e, &commit, slow, done, pre, pc, size, bus, o_pc, o_stall]() mutable {
      e.bind(slow);
      e.mov(R13, RAX);
      e.mov(R14, R8);
      e.mov(R15, RCX);
      commit(pre);
      e.mov(mem(RBX, o_pc), pc);
      e.mov64(kArg[0], u64(reinterpret_cast<uintptr_t>(bus)));
      e.mov(kArg[1], R13);
      e.alu(Emitter::AND, kArg[1], size == 4 ? ~3u : size == 2 ? ~1u : ~0u);
      if (size == 4) e.mov(kArg[2], R15);
      else if (size == 2) e.movzx16(kArg[2], R15);
      else e.movzx8(kArg[2], R15);
      e.mov(kArg[3], u32(size));
      e.call(reinterpret_cast<const void*>(&Jit::slow_write));
      e.alu(Emitter::ADD, mem(RBX, o_stall), R14);
      e.jmp(done);
    });
  };
  // An I/O access can raise an interrupt or ask the loop to stop: leave after
  // this instruction if so.
  u32 exit_flag = 0;  // kSlotExit while compiling a delay slot
  auto check_attention = [&](const Insn& in, u32 cost) {
    Label out;
    e.cmp8(mem(RBX, o_attn), 0);
    e.jcc(NE, out);
    const u32 len = in.len | exit_flag;
    deferred.push_back([&e, &exit_after, out, &in, cost, len]() mutable {
      e.bind(out);
      exit_after(in, cost, len);
    });
  };
  std::function<void(const Insn&)> emit_one;  // one instruction (below)
  // Taking a branch: the interpreter's Cpu::jump.
  auto take = [&](const Insn& in, bool delay, bool link, bool target_in_eax, u32 target, u32 cost) {
    if (delay && in.slot) {
      // The slot runs now, then the target (Cpu::finish for a delay slot: the
      // link is the address after the slot, no loop check).
      const Insn& s = *in.slot;
      if (target_in_eax) e.mov(mem(RBX, o_dtarget), RAX);
      else e.mov(mem(RBX, o_dtarget), target & ~1u);
      e.mov8(mem(RBX, o_dlink), u8(link));
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
      if (!target_in_eax && chainable(target & ~1u)) {
        if (link) e.mov(reg_mem(Cpu::kBLINK), s.pc + s.len);
        chain_to(target & ~1u, cost2);
      } else {
        Label miss;
        if (target_in_eax) {
          e.mov(RAX, mem(RBX, o_dtarget));
          if (link) e.mov(reg_mem(Cpu::kBLINK), s.pc + s.len);
          chain_dynamic(cost2, miss);
        } else {
          e.jmp(miss);
        }
        e.bind(miss);
        exit_after(s, cost2, s.len | kSlotExit);
      }
      return;
    }
    if (delay) {
      e.mov8(mem(RBX, o_dpend), u8(1));
      if (target_in_eax) e.mov(mem(RBX, o_dtarget), RAX);
      else e.mov(mem(RBX, o_dtarget), target & ~1u);
      e.mov8(mem(RBX, o_dlink), u8(link));
    } else {
      if (link) e.mov(reg_mem(Cpu::kBLINK), in.pc + in.len);
      if (!target_in_eax && chainable(target & ~1u)) {  // (a taken branch skips the loop check)
        chain_to(target & ~1u, cost);
        return;
      }
      if (target_in_eax) {
        Label miss;
        chain_dynamic(cost, miss);
        e.bind(miss);
        e.mov(mem(RBX, o_next), RAX);
      } else {
        e.mov(mem(RBX, o_next), target & ~1u);
      }
      e.mov8(mem(RBX, o_redir), u8(1));
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
      e.mov(mem(RBX, o_pc), in.pc);
      const auto& h = c.pc_hooks_.at(in.pc);
      e.mov64(kArg[0], u64(reinterpret_cast<uintptr_t>(h.second)));
      e.mov64(kArg[1], RBX);
      e.call(reinterpret_cast<const void*>(h.first));
    }
    auto operand2 = [&](Reg h) { if (j.imm) e.mov(h, j.k); else load_reg(h, j.s2); };
    // A conditional instruction does nothing when its condition fails.
    Label not_executed;
    if (j.xcc) {
      e.mov(RAX, mem(RBX, o_status));
      e.shift(Emitter::SHR, RAX, 8);
      e.alu(Emitter::AND, RAX, 15u);
      e.mov(RCX, cond_mask(j.xcc));
      e.bt(RCX, RAX);
      e.jcc(AE, not_executed);
    }

    switch (j.kind) {
      case JitOp::Nop:
        break;

      case JitOp::Mov:
        operand2(RAX);
        store_reg(j.dst, RAX);
        if (j.f) { e.test(RAX, RAX); flags(FN | FZ, nullptr); }
        break;

      case JitOp::Mul:
        load_reg(RAX, j.s1);
        operand2(RCX);
        if (j.op == 0) { e.movsxd(RAX, RAX); e.movsxd(RCX, RCX); }  // (the 32-bit loads zero-extend)
        e.imul64(RAX, RCX);
        store_reg(Cpu::kMLO, RAX);
        store_reg(Cpu::kMMID, RAX);
        e.shift64(Emitter::SHR, RAX, 32);
        store_reg(Cpu::kMHI, RAX);
        break;

      case JitOp::Unary: {
        operand2(RAX);
        const auto store = [&] { store_reg(j.dst, RAX); };
        // Flags: N and Z from the result; C from the bit shifted out (x86 CF,
        // kept in r10b across the TEST).
        auto nz_c = [&] {
          if (!j.f) { store(); return; }
          e.setcc(B, R10);
          e.test(RAX, RAX);
          flags(FN | FZ, store);
          e.mov(RDX, mem(RBX, o_status));
          e.alu(Emitter::AND, RDX, ~Cpu::kC);
          e.movzx8(R10, R10);
          e.shift(Emitter::SHL, R10, 9);
          e.alu(Emitter::OR, RDX, R10);
          e.mov(mem(RBX, o_status), RDX);
        };
        auto nz = [&] { if (j.f) { e.test(RAX, RAX); flags(FN | FZ, store); } else store(); };
        switch (j.op) {
          case JitOp::kSexb: e.movsx8(RAX, RAX); nz(); break;
          case JitOp::kSexw: e.movsx16(RAX, RAX); nz(); break;
          case JitOp::kExtb: e.movzx8(RAX, RAX); nz(); break;
          case JitOp::kExtw: e.movzx16(RAX, RAX); nz(); break;
          case JitOp::kNot: e.not_(RAX); nz(); break;
          case JitOp::kNeg: e.neg(RAX); store(); break;
          case JitOp::kAbs: e.mov(RCX, RAX); e.neg(RCX); e.test(RAX, RAX); e.cmov(S, RAX, RCX); store(); break;
          case JitOp::kAsl1:  // C = bit 31, V = bit 31 changed: exactly x86 ADD's
            e.alu(Emitter::ADD, RAX, RAX);
            if (j.f) flags(FN | FZ | FC | FV, store); else store();
            break;
          case JitOp::kAsr1: e.shift(Emitter::SAR, RAX, 1); if (j.f) flags(FN | FZ | FC, store); else store(); break;
          case JitOp::kLsr1: e.shift(Emitter::SHR, RAX, 1); if (j.f) flags(FN | FZ | FC, store); else store(); break;
          case JitOp::kRor1: e.rot1(Emitter::ROR1, RAX); nz_c(); break;
          case JitOp::kRrc: e.bt(mem(RBX, o_status), 9); e.rot1(Emitter::RCR1, RAX); nz_c(); break;
          case JitOp::kRlc: e.bt(mem(RBX, o_status), 9); e.rot1(Emitter::RCL1, RAX); nz_c(); break;
        }
        break;
      }

      case JitOp::Alu: {
        load_reg(RAX, j.s1);
        if (!j.imm) load_reg(RCX, j.s2);
        const auto store = [&] { store_reg(j.dst, RAX); };
        auto arith = [&](Emitter::Alu op, unsigned sh) {  // add / sub of s2 << sh
          if (j.imm) e.alu(op, RAX, j.k << sh);
          else { if (sh) e.shift(Emitter::SHL, RCX, u8(sh)); e.alu(op, RAX, RCX); }
          if (j.f) flags(FN | FZ | FC | FV, store); else store();
        };
        auto logic = [&](Emitter::Alu op, u32 imm) {  // and / or / xor with imm or ecx
          if (j.imm) e.alu(op, RAX, imm); else e.alu(op, RAX, RCX);
          if (j.f) flags(FN | FZ, store); else store();
        };
        auto bit_reg = [&]() { e.mov(RDX, 1u); e.shift_cl(Emitter::SHL, RDX); };  // edx = 1 << (ecx & 31)
        switch (j.op) {
          case JitOp::kAdd: arith(Emitter::ADD, 0); break;
          case JitOp::kSub: arith(Emitter::SUB, 0); break;
          case JitOp::kAdd1: arith(Emitter::ADD, 1); break;
          case JitOp::kAdd2: arith(Emitter::ADD, 2); break;
          case JitOp::kAdd3: arith(Emitter::ADD, 3); break;
          case JitOp::kSub1: arith(Emitter::SUB, 1); break;
          case JitOp::kSub2: arith(Emitter::SUB, 2); break;
          case JitOp::kSub3: arith(Emitter::SUB, 3); break;
          case JitOp::kRsub:
            if (j.imm) { e.mov(RCX, RAX); e.mov(RAX, j.k); }
            else { e.mov(RDX, RAX); e.mov(RAX, RCX); e.mov(RCX, RDX); }
            e.alu(Emitter::SUB, RAX, RCX);
            if (j.f) flags(FN | FZ | FC | FV, store); else store();
            break;
          case JitOp::kAdc: case JitOp::kSbc:  // with STATUS32.C as the carry / borrow in
            e.bt(mem(RBX, o_status), 9);
            if (j.imm) e.alu(j.op == JitOp::kAdc ? Emitter::ADC : Emitter::SBB, RAX, j.k);
            else e.alu(j.op == JitOp::kAdc ? Emitter::ADC : Emitter::SBB, RAX, RCX);
            if (j.f) flags(FN | FZ | FC | FV, store); else store();
            break;
          case JitOp::kBxor:
            if (!j.imm) { bit_reg(); e.mov(RCX, RDX); }
            logic(Emitter::XOR, 1u << (j.k & 31));
            break;
          case JitOp::kAnd: logic(Emitter::AND, j.k); break;
          case JitOp::kOr: logic(Emitter::OR, j.k); break;
          case JitOp::kXor: logic(Emitter::XOR, j.k); break;
          case JitOp::kBic:
            if (!j.imm) e.not_(RCX);
            logic(Emitter::AND, ~j.k);
            break;
          case JitOp::kBset:
            if (!j.imm) { bit_reg(); e.mov(RCX, RDX); }
            logic(Emitter::OR, 1u << (j.k & 31));
            break;
          case JitOp::kBclr:
            if (!j.imm) { bit_reg(); e.not_(RDX); e.mov(RCX, RDX); }
            logic(Emitter::AND, ~(1u << (j.k & 31)));
            break;
          case JitOp::kBmsk: {
            const unsigned n = j.k & 31;
            if (!j.imm) { e.mov(RDX, 2u); e.shift_cl(Emitter::SHL, RDX); e.alu(Emitter::SUB, RDX, 1u); e.mov(RCX, RDX); }
            logic(Emitter::AND, n == 31 ? 0xffffffffu : ((2u << n) - 1));
            break;
          }
          case JitOp::kAsl: case JitOp::kLsr: case JitOp::kAsr: {
            const Emitter::Shift s = j.op == JitOp::kAsl ? Emitter::SHL : j.op == JitOp::kLsr ? Emitter::SHR : Emitter::SAR;
            if (!j.imm) { e.shift_cl(s, RAX); store(); break; }  // (no flags: see above)
            const unsigned n = j.k & 31;
            if (n == 0) {
              if (j.f) { e.test(RAX, RAX); flags(FN | FZ, store); } else store();
            } else {
              e.shift(s, RAX, u8(n));
              if (j.f) flags(FN | FZ | FC, store); else store();
            }
            break;
          }
        }
        break;
      }

      case JitOp::Test:
        load_reg(RAX, j.s1);
        if (!j.imm) load_reg(RCX, j.s2);
        switch (j.op) {
          case 0:  // tst
            if (j.imm) e.test(RAX, j.k); else e.test(RAX, RCX);
            flags(FN | FZ, nullptr);
            break;
          case 1:  // cmp
            if (j.imm) e.alu(Emitter::CMP, RAX, j.k); else e.alu(Emitter::CMP, RAX, RCX);
            flags(FN | FZ | FC | FV, nullptr);
            break;
          case 2:  // btst
            if (j.imm) e.test(RAX, 1u << (j.k & 31));
            else { e.mov(RDX, 1u); e.shift_cl(Emitter::SHL, RDX); e.test(RAX, RDX); }
            flags(FN | FZ, nullptr);
            break;
          default:  // rcmp: s2 - s1
            if (j.imm) e.mov(RCX, j.k);
            e.alu(Emitter::CMP, RCX, RAX);
            flags(FN | FZ | FC | FV, nullptr);
            break;
        }
        break;

      case JitOp::Load:
        // (Cpu::do_load: mode 1 writes the base back before the load, mode 2
        // after the destination, from the base's original value.)
        if (j.s1 < 0) e.mov(RAX, j.k);
        else {
          load_reg(RAX, j.s1);
          if (!j.imm) { load_reg(RCX, j.s2); if (j.scale) e.shift(Emitter::SHL, RCX, j.scale); }
          if (j.aa == 2) {  // r15 = the new base (the read's slow path keeps r15)
            e.mov(R15, RAX);
            if (j.imm) e.alu(Emitter::ADD, R15, j.k); else e.alu(Emitter::ADD, R15, RCX);
          } else {
            if (j.imm) { if (j.k) e.alu(Emitter::ADD, RAX, j.k); }
            else e.alu(Emitter::ADD, RAX, RCX);
            if (j.aa == 1) store_reg(j.s1, RAX);
          }
        }
        read(j.size, j.sext);
        store_reg(j.dst, RAX);
        if (j.aa == 2) store_reg(j.s1, R15);
        check_attention(in, cost);
        break;

      case JitOp::Store:
        // (Cpu::do_store: the value is read after a mode-1 writeback; mode 2
        // writes base + offset back after the store.)
        load_reg(RAX, j.s1);
        if (j.aa != 2 && j.k) e.alu(Emitter::ADD, RAX, j.k);
        if (j.aa == 1) store_reg(j.s1, RAX);
        load_reg(RCX, j.val);
        write(j.size);
        if (j.aa == 2) {  // (a store leaves the registers alone)
          load_reg(RAX, j.s1);
          if (j.k) e.alu(Emitter::ADD, RAX, j.k);
          store_reg(j.s1, RAX);
        }
        check_attention(in, cost);
        break;

      case JitOp::Push:
        e.mov(RAX, reg_mem(Cpu::kSP));
        e.alu(Emitter::SUB, RAX, 4u);
        e.mov(reg_mem(Cpu::kSP), RAX);
        load_reg(RCX, j.val);
        write(4);
        check_attention(in, cost);
        break;

      case JitOp::Pop:
        e.mov(RAX, reg_mem(Cpu::kSP));
        read(4, false);
        store_reg(j.dst, RAX);
        e.mov(RAX, reg_mem(Cpu::kSP));
        e.alu(Emitter::ADD, RAX, 4u);
        e.mov(reg_mem(Cpu::kSP), RAX);
        check_attention(in, cost);
        break;

      case JitOp::Branch: {
        Label skip;
        if (j.cc != 0) {
          e.mov(RAX, mem(RBX, o_status));
          e.shift(Emitter::SHR, RAX, 8);
          e.alu(Emitter::AND, RAX, 15u);
          e.mov(RCX, cond_mask(j.cc));
          e.bt(RCX, RAX);
          e.jcc(AE, skip);
        }
        take(in, j.delay, j.link, false, j.k, cost);
        if (j.cc != 0) e.bind(skip);
        else ended = true;
        break;
      }

      case JitOp::BranchZ: {
        Label skip;
        load_reg(RAX, j.s1);
        e.test(RAX, RAX);
        e.jcc(j.op == 0 ? NE : E, skip);
        take(in, false, false, false, j.k, cost);
        e.bind(skip);
        break;
      }

      case JitOp::Jump:
        load_reg(RAX, j.s1);
        e.alu(Emitter::AND, RAX, ~1u);
        take(in, j.delay, j.link, true, 0, cost);
        if (!j.xcc) ended = true;
        break;

      case JitOp::BranchCmp: {
        Label skip;
        load_reg(RAX, j.s1);
        Cond taken;
        if (j.op == 0xe || j.op == 0xf) {  // bbit0 / bbit1
          if (j.imm) e.test(RAX, 1u << (j.k2 & 31));
          else { load_reg(RCX, j.s2); e.bt(RAX, RCX); }
          taken = j.imm ? (j.op == 0xe ? E : NE) : (j.op == 0xe ? AE : B);
        } else {
          if (j.imm) e.alu(Emitter::CMP, RAX, j.k2);
          else { load_reg(RCX, j.s2); e.alu(Emitter::CMP, RAX, RCX); }
          static constexpr Cond kCmp[6] = {E, NE, L, GE, B, AE};
          taken = kCmp[j.op];
        }
        e.jcc(Cond(taken ^ 1), skip);  // (x86 conditions come in complementary pairs)
        take(in, j.delay, false, false, j.k, cost);
        e.bind(skip);
        break;
      }

      case JitOp::Generic:
      case JitOp::Other: {
        Label out;
        e.mov64(kArg[0], RBX);
        e.mov64(kArg[1], u64(reinterpret_cast<uintptr_t>(in.o)));
        e.mov(kArg[2], in.pc);
        e.mov(kArg[3], prefix | (in.len << 24));
        e.call(reinterpret_cast<const void*>(&Cpu::jit_generic));
        e.test(RAX, RAX);
        e.jcc(NE, out);
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
      e.mov(RAX, mem(RBX, o_lpend));
      e.alu(Emitter::CMP, RAX, seq);
      e.jcc(E, looped);
      chain_to(seq, prefix);
      e.bind(looped);
    }
    exit_after(in, prefix, in.len);
  }

  const auto leave = u64(reinterpret_cast<uintptr_t>(leave_));
  e.bind(ret1);
  e.mov(RAX, 1u);
  e.mov64(RCX, leave);
  e.jmp(RCX);
  e.bind(bail0);
  e.mov(RAX, 0u);
  e.mov64(RCX, leave);
  e.jmp(RCX);
  e.bind(bail2);
  e.mov(RAX, 2u);
  e.mov64(RCX, leave);
  e.jmp(RCX);
  for (auto& d : deferred) d();
  // Link stubs: note which slot to fill, and return to the dispatcher.
  for (auto& ch : chains) {
    e.bind(ch->stub);
    ch->stub_imm = int(e.size()) + 2;  // (the imm64 of mov rax, imm64)
    e.mov64(RAX, u64(0));
    e.mov64(RCX, u64(reinterpret_cast<uintptr_t>(&link_slot_)));
    e.mov64(mem(RCX), RAX);
    e.mov(RAX, 1u);
    e.mov64(RCX, leave);
    e.jmp(RCX);
  }
  e.align(8);
  for (auto& ch : chains) {
    e.bind(ch->slot);
    relocs.push_back({int(e.size()), ch->stub.pos});
    relocs.push_back({ch->stub_imm, int(e.size())});
    e.u64le(0);
  }

  out->code = std::move(e.code);
  return true;
}

}  // namespace leap::arc

#endif  // LEAP_JIT_X64
