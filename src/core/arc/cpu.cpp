#include "core/arc/cpu.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <string_view>

#include "core/arc/jit.h"
#include "core/log.h"

namespace leap::arc {

namespace {

// 16-bit instructions address r0-r3 and r12-r15 with 3-bit fields.
constexpr unsigned r16(unsigned f) { return f > 3 ? f + 8 : f; }

// 32-bit field accessors.
constexpr unsigned f_a(u32 op) { return op & 63; }
constexpr unsigned f_b(u32 op) { return ((op >> 24) & 7) | (((op >> 12) & 7) << 3); }
constexpr unsigned f_c(u32 op) { return (op >> 6) & 63; }
constexpr unsigned f_p(u32 op) { return (op >> 22) & 3; }
constexpr bool f_f(u32 op) { return op & 0x8000; }
constexpr u32 f_s12(u32 op) { return sext(((op >> 6) & 63) | ((op & 63) << 6), 12); }

// 16-bit field accessors.
constexpr unsigned h_b(u16 op) { return r16((op >> 8) & 7); }
constexpr unsigned h_c(u16 op) { return r16((op >> 5) & 7); }
constexpr unsigned h_a(u16 op) { return r16(op & 7); }

// Interrupt vectors in descending priority (vectors 0-2 are exceptions and
// are not dispatched through this path). Level-2 vs level-1 is decided by
// AUX_IRQ_LEV before priority is considered.
constexpr unsigned kIrqPriority[] = {7, 6, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19,
                                     18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 5, 4, 3};

s32 sat32(s64 v, bool* sat) {
  if (v > INT32_MAX) { *sat = true; return INT32_MAX; }
  if (v < INT32_MIN) { *sat = true; return INT32_MIN; }
  return s32(v);
}
s16 sat16(s32 v, bool* sat) {
  if (v > INT16_MAX) { *sat = true; return INT16_MAX; }
  if (v < INT16_MIN) { *sat = true; return INT16_MIN; }
  return s16(v);
}

// Warn once per mnemonic for instructions whose semantics are implemented from
// the ISA description but have not yet been validated against hardware.
void unverified(const char* name) {
  static std::set<std::string_view> seen;
  if (seen.insert(name).second) LOG_W("executing unverified instruction %s", name);
}

}  // namespace

Cpu::Cpu(Bus& bus) : bus_(bus) {
  bus_.set_code_hook(&Cpu::code_written, this);
  flush_decode_cache();
  reset();
}

Cpu::~Cpu() = default;

bool Cpu::backend_available(Backend b) { return b != Backend::Jit || Jit::supported(); }

void Cpu::reset() {
  for (auto& r : r_) r = 0;
  pc_ = int_vector_base_ = reset_vector_ & ~1u;
  int_vector_base_ &= 0xfffffc00;
  status32_ = status32_l1_ = status32_l2_ = 0;
  lp_start_ = lp_end_ = 0;
  aux_irq_lv12_ = 0;
  aux_irq_lev_ = 0x000000c0;  // Vectors 6 and 7 default to level 2.
  debug_ = 0;
  for (auto& t : timer_) t[0] = t[1] = t[2] = 0;
  pending_irqs_ = 0;
  in_delay_slot_ = delay_pending_ = delay_link_ = false;
  delay_target_ = 0;
  sleeping_ = halted_ = false;
  frac8_ = 0;
  ic_.invalidate();
  dc_.invalidate();
  last_iline_ = ~0u;
  stop_ = Stop::None;
  stop_msg_.clear();
  cycles_ = 0;
}

void Cpu::fault(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  char full[320];
  std::snprintf(full, sizeof(full), "%s at pc=%08x", buf, pc_);
  stop_ = Stop::Fault;
  stop_msg_ = full;
  attention_ = true;
  LOG_E("cpu fault: %s", full);
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

void Cpu::step() {
  if (!code_writes_.empty()) [[unlikely]] apply_code_writes();
  if (halted_) return;
  if (!in_delay_slot_) check_interrupts();
  if (sleeping_) { cycles_++; idle_cycles_++; return; }
  if (trace_hook) trace_hook(*this);
  execute();
  charge(8 + fetch_cost_ + stall_);
  stall_ = 0;
}

void Cpu::set_pc_hook(u32 pc, PcHookFn fn, void* ctx) {
  pc_hooks_[pc & ~1u] = {fn, ctx};
  flush_decode_cache();  // decoded ops carry the hook flag
}

void Cpu::clear_pc_hooks() {
  if (pc_hooks_.empty()) return;
  pc_hooks_.clear();
  flush_decode_cache();
}

void Cpu::configure_caches(const CacheModel::Config& icache, const CacheModel::Config& dcache, u32 writeback8) {
  if (!(ic_.config() == icache)) ic_.configure(icache);
  if (!(dc_.config() == dcache)) dc_.configure(dcache);
  writeback8_ = writeback8;
  last_iline_ = ~0u;
  flush_decode_cache();  // decoded ops cache their flat fetch cost
}

u64 Cpu::run(u64 n) {
  const u64 start = cycles_;
  const u64 end = cycles_ + n;
  stop_ = Stop::None;
  const bool bps = !breakpoints_.empty();
  bool first = true;  // Never re-trigger the breakpoint we just stopped on.

  while (cycles_ < end) {
    if (!code_writes_.empty()) [[unlikely]] apply_code_writes();
    if (halted_) { stop_ = Stop::Halted; break; }
    if (pending_irqs_ && !in_delay_slot_) check_interrupts();
    if (sleeping_) {
      // Nothing can wake the core until the machine raises an interrupt,
      // which only happens between run() calls: skip to the end of the slice.
      idle_cycles_ += end - cycles_;
      cycles_ = end;
      break;
    }
    if (pc_ == idle_hint && idle_skip && !mame_compat && !trace_hook) {
      const u64 h = idle_state_hash();
      if (idle_armed_ && h == idle_hash_ && pending_irqs_ == 0 && (status32_ & (kE1 | kE2))) {
        idle_cycles_ += end - cycles_;
        cycles_ = end;
        break;
      }
      idle_hash_ = h;
      idle_armed_ = true;
    }
    if (bps && !first && breakpoints_.count(pc_)) { stop_ = Stop::Breakpoint; break; }
    first = false;
    if (trace_hook) trace_hook(*this);
    execute();
    charge(8 + fetch_cost_ + stall_);
    stall_ = 0;
    if (stop_ != Stop::None) break;
    if (exit_request_) { exit_request_ = false; break; }
    if (use_cache() && !trace_hook && !bps && !pending_irqs_ && !sleeping_) {
      if (backend_ == Backend::Jit && jit_ready()) run_jit(end);
      else run_burst(end);
      if (stop_ != Stop::None) break;
      if (exit_request_) { exit_request_ = false; break; }
    }
  }
  return cycles_ - start;
}

void Cpu::check_interrupts() {
  const u32 pend = pending_irqs_ & ~7u;
  if (!pend) return;

  u32 masked;
  int level;
  if ((status32_ & kE2) && (pend & aux_irq_lev_)) {
    masked = pend & aux_irq_lev_;
    level = 2;
  } else if ((status32_ & kE1) && (pend & ~aux_irq_lev_)) {
    masked = pend & ~aux_irq_lev_;
    level = 1;
  } else {
    return;
  }

  unsigned vec = 0;
  for (unsigned v : kIrqPriority)
    if (masked & (1u << v)) { vec = v; break; }

  if (level == 2) {
    r_[kILINK2] = pc_;
    status32_l2_ = status32_;
    status32_ &= ~(kE1 | kE2);
    aux_irq_lv12_ |= 2;
  } else {
    r_[kILINK1] = pc_;
    status32_l1_ = status32_;
    status32_ &= ~kE1;
    aux_irq_lv12_ |= 1;
  }
  pc_ = int_vector_base_ + vec * 8;
  pending_irqs_ &= ~(1u << vec);
  idle_armed_ = false;
  sleeping_ = false;
  debug_ &= ~(1u << 23);  // DEBUG.ZZ (sleeping)
}

void Cpu::execute() {
  const u32 pc = pc_;
  if (!pc_hooks_.empty()) [[unlikely]] run_pc_hook(pc);
  r_[kPCL] = pc & ~3u;
  redirected_ = false;
  delay_pending_ = false;
  const bool was_delay_slot = in_delay_slot_;

  if (const DecodedOp* o = use_cache() ? decoded(pc) : nullptr) {
    len_ = o->len;
    r_[kLIMM] = o->limm;
    limm_loaded_ = true;
    o->fn(*this, *o);
    fetch_cost_ = o->cost8 - 8u + icache_fetch_if_cached(pc, len_);  // run-time LIMMs of fallbacks are charged to stall_
  } else {
    limm_loaded_ = false;
    const u16 hw = fetch16(pc);
    if ((hw >> 11) < 0x0c) {
      len_ = 4;
      const u32 op = (u32(hw) << 16) | fetch16(pc + 2);
      if (xy_fields(op, false)) [[unlikely]] {
        const XyUse u = xy_before(op, false);
        exec32(op);
        xy_after(u);
      } else {
        exec32(op);
      }
    } else {
      len_ = 2;
      if (xy_fields(hw, true)) [[unlikely]] {
        const XyUse u = xy_before(hw, true);
        exec16(hw);
        xy_after(u);
      } else {
        exec16(hw);
      }
    }
    fetch_cost_ = fetch_stall(pc) + icache_fetch_if_cached(pc, len_);
  }
  if (stop_ == Stop::Fault || (halted_ && stop_ == Stop::Halted)) return;  // pc stays put
  finish(pc, was_delay_slot);
}

inline void Cpu::finish(u32 pc, bool was_delay_slot) {
  const u32 seq = pc + len_;
  if (was_delay_slot) {
    // This instruction was the delay slot of an earlier .D branch.
    if (delay_link_) r_[kBLINK] = seq;
    pc_ = delay_target_;
    in_delay_slot_ = false;
    return;
  }
  if (delay_pending_) {
    in_delay_slot_ = true;
    pc_ = seq;
    return;
  }
  u32 npc = redirected_ ? next_pc_ : seq;
  // Zero-overhead loop: triggers when sequential flow reaches LP_END.
  // MAME-DIFF: MAME also triggers when a taken 32-bit branch lands on LP_END.
  if (npc == lp_end_ && !redirected_ && !(status32_ & kL)) {
    if (r_[kLP_COUNT] != 1) npc = lp_start_;
    r_[kLP_COUNT]--;
  }
  pc_ = npc;
}

// Cached-interpreter inner loop: runs pre-decoded ROM instructions back to back
// until something needs the general loop (interrupt raised, fault/halt,
// SLEEP, exit request, idle-loop head, code outside ROM, or end of slice).
// Handlers never read PCL/LIMM through the register file except via the
// generic fallbacks, which set them up themselves.
inline bool Cpu::burst_step(u32 pc) {
  if ((pc >> 16) != cur_page_ && !select_page(pc)) return false;
  DecodedOp& o = cur_ops_[(pc & Bus::kPageMask) >> 1];
  if (!o.fn) {
    decode(pc, o);
    o.cost8 = u16(8 + (o.len / 2) * fetch_wait16(pc));
    o.hook = pc_hooks_.count(pc) != 0;
  }
  if (o.hook) [[unlikely]] run_pc_hook(pc);
  const bool was_delay_slot = in_delay_slot_;
  len_ = o.len;
  r_[kLIMM] = o.limm;
  limm_loaded_ = true;
  redirected_ = false;
  delay_pending_ = false;
  o.fn(*this, o);
  charge(o.cost8 + stall_ + (cur_icached_ ? icache_fetch(pc, len_) : 0));
  stall_ = 0;
  if (attention_) {
    if (stop_ == Stop::Fault || (halted_ && stop_ == Stop::Halted)) return false;
    finish(pc, was_delay_slot);
    return false;
  }
  finish(pc, was_delay_slot);
  return true;
}

void Cpu::run_burst(u64 end) {
  attention_ = false;
  while (cycles_ < end) {
    const u32 pc = pc_;
    if (pc == idle_hint) return;
    if (!burst_step(pc)) return;
  }
}

// ---------------------------------------------------------------------------
// JIT (see jit.h)
// ---------------------------------------------------------------------------

// LEAPEMU_JIT_STATS: 1 = counts at exit, 2 = also which instructions the
// interpreter still runs (slow).
static int jit_stats_level() {
  static const int level = std::getenv("LEAPEMU_JIT_STATS") ? std::atoi(std::getenv("LEAPEMU_JIT_STATS")) : 0;
  return level;
}

bool Cpu::jit_ready() {
  if (ic_.enabled() || dc_.enabled() || !Jit::supported()) return false;
  if (!jit_) jit_ = std::make_unique<Jit>(*this, bus_);
  return true;
}

// run_burst, running compiled blocks where it can. A block runs only if all
// of its instructions start before the end of the slice, and no
// zero-overhead loop ends inside it (the interpreter checks for one after
// every instruction); otherwise the next instruction is interpreted.
void Cpu::run_jit(u64 end) {
  attention_ = false;
  jit_end8_ = end * 8;
  jit_->forget_link();
  while (cycles_ < end) {
    if (pc_ == idle_hint) break;
    if (!in_delay_slot_) {
      const Jit::Block* b = jit_->block(pc_);
      jit_->link(b);  // the exit that just returned here now leads to b
      // (An interpreted branch leaves these set; blocks expect them clear.)
      redirected_ = delay_pending_ = false;
      u32 r = b ? jit_->run(b) : 0;
      // Linked blocks may have run before one declined to start, so what
      // follows is about pc_, wherever that is now.
      if (r == 2) {  // a loop ends inside the block at pc_: the variant that stops there
        jit_->stats.guard_loop++;
        jit_->forget_link();
        b = jit_->block(pc_, lp_end_);
        r = b ? jit_->run(b) : 0;
      }
      if (r == 1) {
        jit_->stats.runs++;
        if (attention_) break;
        continue;
      }
      if (b) jit_->stats.guard_slice++;
      if (pc_ == idle_hint || cycles_ >= end) break;  // (linked blocks may have got here)
      if (b && !in_delay_slot_ && jit_stats_level() < 2) {
        // A block that would run past the end of the slice: the rest of the
        // slice is interpreted (as it would be one instruction at a time).
        jit_->forget_link();
        while (cycles_ < end && pc_ != idle_hint) {
          jit_->stats.interpreted++;
          if (!burst_step(pc_)) break;
        }
        break;
      }
    }
    jit_->forget_link();
    jit_->stats.interpreted++;
    if (in_delay_slot_) jit_->stats.delay_slots++;
    if (jit_stats_level() >= 2) [[unlikely]] {
      unsigned len;
      std::string d = disassemble(pc_, [&](u32 a) { return bus_.peek16(a); }, &len);
      d = d.substr(0, d.find(' '));
      jit_->stats.generic["(interpreted) " + d]++;
    }
    if (!burst_step(pc_)) break;
  }
  jit_->forget_link();
}

void Cpu::jit_exit(Cpu* c, u32 pc, u32 cost8, u32 len) {
  c->charge(cost8 + c->stall_);
  c->stall_ = 0;
  if (c->stop_ == Stop::Fault || (c->halted_ && c->stop_ == Stop::Halted)) { c->pc_ = pc; return; }
  const bool slot = len & 0x80000000u;  // a delay slot compiled into its branch
  len &= 0x7fffffffu;
  if (len) c->len_ = len;
  c->finish(pc, slot);
  c->redirected_ = c->delay_pending_ = false;
}

u32 Cpu::jit_generic(Cpu* c, const DecodedOp* o, u32 pc, u32 prefix_len) {
  const u32 prefix = prefix_len & 0xffffff, len = prefix_len >> 24;
  if (c->jit_ && jit_stats_level() >= 2) [[unlikely]] {
    unsigned l;
    std::string d = disassemble(pc, [&](u32 a) { return c->bus_.peek16(a); }, &l);
    c->jit_->stats.generic[d.substr(0, d.find(' '))]++;
  }
  // The time as the interpreter would show it now (the handler may do I/O).
  c->charge(prefix + c->stall_);
  c->stall_ = u32(0) - prefix;
  c->pc_ = pc;
  const u32 lp_end = c->lp_end_, status = c->status32_;
  c->len_ = o->len;
  c->r_[kLIMM] = o->limm;
  c->limm_loaded_ = true;
  o->fn(*c, *o);
  // Leave the block if the handler changed the flow of control, needs the
  // loop's attention, or changed what the block assumed (the loop end).
  return c->redirected_ || c->delay_pending_ || c->attention_ || c->len_ != len || c->lp_end_ != lp_end ||
         ((c->status32_ ^ status) & kL);
}

bool Cpu::select_page(u32 pc) {
  if (!decoded(pc)) return false;  // not ROM: general path
  cur_page_ = pc >> 16;
  cur_ops_ = dcache_[cur_page_].get();
  cur_icached_ = ic_.enabled() && bus_.cacheable(pc);
  return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

u64 Cpu::idle_state_hash() const {
  u64 h = 1469598103934665603ull;
  auto mix = [&](u32 v) { h = (h ^ v) * 1099511628211ull; };
  for (unsigned i = 0; i < 32; i++) mix(r_[i]);
  mix(status32_);
  mix(r_[kLP_COUNT]);
  mix(lp_start_);
  mix(lp_end_);
  return h;
}

bool Cpu::cond(unsigned cc) {
  switch (cc & 0x1f) {
    case 0x00: return true;                              // al
    case 0x01: return fz();                              // eq
    case 0x02: return !fz();                             // ne
    case 0x03: return !fn();                             // pl
    case 0x04: return fn();                              // mi
    case 0x05: return fc();                              // cs / lo
    case 0x06: return !fc();                             // cc / hs
    case 0x07: return fv();                              // vs
    case 0x08: return !fv();                             // vc
    case 0x09: return !fz() && (fn() == fv());           // gt
    case 0x0a: return fn() == fv();                      // ge
    case 0x0b: return fn() != fv();                      // lt
    case 0x0c: return fz() || (fn() != fv());            // le
    case 0x0d: return !fc() && !fz();                    // hi
    case 0x0e: return fc() || fz();                      // ls
    case 0x0f: return !fn() && !fz();                    // pnz
    default:
      if (!(warned_cc_ & (1u << (cc & 0x1f)))) {
        warned_cc_ |= 1u << (cc & 0x1f);
        LOG_W("extension condition code %02x at %08x treated as false", cc & 0x1f, pc_);
      }
      return false;
  }
}

u32 Cpu::alu_add(u32 a, u32 b, u32 cin, bool f) {
  const u64 s = u64(a) + b + cin;
  const u32 r = u32(s);
  if (f) {
    flags_nz(r);
    set_flag(kC, s >> 32);
    set_flag(kV, ((a ^ r) & (b ^ r)) >> 31);
  }
  return r;
}

u32 Cpu::alu_sub(u32 a, u32 b, u32 bin, bool f) {
  const u32 r = a - b - bin;
  if (f) {
    flags_nz(r);
    set_flag(kC, u64(b) + bin > a);
    set_flag(kV, ((a ^ b) & (a ^ r)) >> 31);
  }
  return r;
}

void Cpu::jump(u32 target, bool delay, bool link) {
  target &= ~1u;
  if (delay) {
    delay_pending_ = true;
    delay_target_ = target;
    delay_link_ = link;
  } else {
    if (link) r_[kBLINK] = pc_ + len_;
    next_pc_ = target;
    redirected_ = true;
  }
}

void Cpu::branch_rel(u32 offset, bool delay, bool link) {
  jump((pc_ & ~3u) + offset, delay, link);
}

void Cpu::do_load(unsigned dst, unsigned base, u32 off, unsigned zz, bool x, unsigned aa) {
  const u32 b = r_[base];
  u32 addr = b;
  switch (aa) {
    case 0: addr = b + off; break;
    case 1: addr = b + off; setr(base, addr); break;  // .a: pre-modify with writeback
    case 2: addr = b; break;                          // .ab: post-modify
    case 3: addr = b + (off << (zz == 0 ? 2 : zz == 2 ? 1 : 0)); break;  // .as: scaled
  }
  u32 v;
  switch (zz) {
    case 0: v = rd32(addr); break;
    case 1: v = rd8(addr); if (x) v = sext(v, 8); break;
    case 2: v = rd16(addr); if (x) v = sext(v, 16); break;
    default: fault("illegal load size"); return;
  }
  setr(dst, v);
  if (aa == 2) setr(base, b + off);
}

void Cpu::do_store(unsigned base, u32 off, u32 src, unsigned zz, unsigned aa) {
  const u32 b = r_[base];
  u32 addr = b;
  switch (aa) {
    case 0: addr = b + off; break;
    case 1: addr = b + off; setr(base, addr); break;
    case 2: addr = b; break;
    case 3: addr = b + (off << (zz == 0 ? 2 : zz == 2 ? 1 : 0)); break;
  }
  // Read the data register after any pre-modify writeback (matches MAME when
  // the base and data registers are the same).
  const u32 v = r_[src];
  switch (zz) {
    case 0: wr32(addr, v); break;
    case 1: wr8(addr, u8(v)); break;
    case 2: wr16(addr, u16(v)); break;
    default: fault("illegal store size"); return;
  }
  if (aa == 2) setr(base, b + off);
}

// ---------------------------------------------------------------------------
// Auxiliary registers
// ---------------------------------------------------------------------------

u32 Cpu::aux_read(u32 reg) {
  switch (reg) {
    case 0x002: return lp_start_;
    case 0x003: return lp_end_;
    case 0x005: return debug_;
    case 0x006: return pc_;
    case 0x00a: return status32_;
    case 0x00b: return status32_l1_;
    case 0x00c: return status32_l2_;
    case 0x021: case 0x022: case 0x023: return timer_[0][reg - 0x021];
    case 0x025: return int_vector_base_;
    case 0x043: return aux_irq_lv12_;
    case 0x100: case 0x101: case 0x102: return timer_[1][reg - 0x100];
    // MAME-DIFF: MAME reads AUX_IRQ_LEV as 0; we return the register.
    case 0x200: return mame_compat ? 0 : aux_irq_lev_;
  }
  if (XyUnit::is_aux(reg)) return xy_.aux_read(reg);
  if (aux_.read) return aux_.read(reg);
  LOG_W("unhandled aux read %03x at %08x", reg, pc_);
  return 0;
}

void Cpu::aux_write(u32 reg, u32 v) {
  switch (reg) {
    case 0x002: lp_start_ = v & ~1u; return;
    case 0x003: lp_end_ = v & ~1u; return;
    case 0x00a: return;  // STATUS32 is read-only via SR; use FLAG.
    case 0x00b: status32_l1_ = v; return;
    case 0x00c: status32_l2_ = v; return;
    case 0x012: r_[kMHI] = v; return;  // MULHI
    case 0x021: case 0x022: case 0x023: timer_[0][reg - 0x021] = v; return;
    case 0x025: int_vector_base_ = v & 0xfffffc00; return;
    case 0x043: aux_irq_lv12_ &= ~(v & 3); return;  // write-1-to-clear
    case 0x100: case 0x101: case 0x102: timer_[1][reg - 0x100] = v; return;
    case 0x200: aux_irq_lev_ = v; return;
    // Cache maintenance (timing model only; control registers are ignored).
    case 0x010: ic_.invalidate(); last_iline_ = ~0u; break;   // IC_IVIC
    case 0x047: dc_.invalidate(); break;                      // DC_IVDC
    case 0x04b: stall_ += dc_.flush() * writeback8_; break;   // DC_FLSH
  }
  if (XyUnit::is_aux(reg)) { xy_.aux_write(reg, v, bus_); return; }
  attention_ = true;
  if (aux_.write) { aux_.write(reg, v); return; }
  LOG_W("unhandled aux write %03x = %08x at %08x", reg, v, pc_);
}

// ---------------------------------------------------------------------------
// XY memory ports (r32-r55)
// ---------------------------------------------------------------------------

namespace {

// Which registers an instruction reads and which it writes (for the formats
// that can name r32-r55: 32-bit ALU, loads and stores, and the 16-bit
// forms with a 6-bit register).
struct Fields { unsigned src[3]; unsigned nsrc = 0; int dst = -1; bool conditional = false; };

Fields fields32(u32 op) {
  Fields f;
  const unsigned major = op >> 27, b = f_b(op), c = f_c(op), a = f_a(op), p = f_p(op);
  auto src = [&](unsigned r) { f.src[f.nsrc++] = r; };
  if (major == 2) { src(b); f.dst = int(a); return f; }  // LD a,[b,s9]
  if (major == 3) { src(b); src(c); return f; }         // ST c,[b,s9]
  if (major != 4 && major != 5) return f;
  const unsigned sub = (op >> 16) & 63;
  const bool c_reg = p == 0 || (p == 3 && !(op & 0x20));
  if (major == 4 && sub == 0x2f) {  // single operand: op b,c
    if (c_reg || p == 1) { if (c_reg) src(c); f.dst = int(b); }
    return f;
  }
  if (major == 4 && sub == 0x0a) {  // MOV b,c
    if (c_reg) src(c);
    f.dst = int(b);
    f.conditional = p == 3;
    return f;
  }
  const bool no_dst = major == 4 && (sub == 0x0b || sub == 0x0c || sub == 0x0d || sub == 0x11 || sub == 0x2b ||
                                     (sub >= 0x20 && sub <= 0x23));
  if (!(major == 4 && sub >= 0x20 && sub <= 0x23)) src(b);
  if (c_reg) src(c);
  if (!no_dst) {
    f.dst = int(p <= 1 ? a : b);
    f.conditional = p == 3;
  }
  return f;
}

Fields fields16(u16 op) {
  Fields f;
  if ((op >> 11) != 0x0e) return f;
  const unsigned b = r16((op >> 8) & 7), h = ((op & 7) << 3) | ((op >> 5) & 7);
  switch ((op >> 3) & 3) {
    case 0: f.src[0] = b; f.src[1] = h; f.nsrc = 2; f.dst = int(b); break;  // ADD_S b,b,h
    case 1: f.src[0] = h; f.nsrc = 1; f.dst = int(b); break;                 // MOV_S b,h
    case 2: f.src[0] = b; f.src[1] = h; f.nsrc = 2; break;                   // CMP_S b,h
    case 3: f.src[0] = b; f.nsrc = 1; f.dst = int(h); break;                 // MOV_S h,b
  }
  return f;
}

}  // namespace

bool Cpu::xy_fields(u32 raw, bool is16) {
  const Fields f = is16 ? fields16(u16(raw)) : fields32(raw);
  for (unsigned i = 0; i < f.nsrc; i++)
    if (XyUnit::is_port(f.src[i])) return true;
  return f.dst >= 0 && XyUnit::is_port(unsigned(f.dst));
}

Cpu::XyUse Cpu::xy_before(u32 raw, bool is16) {
  const Fields f = is16 ? fields16(u16(raw)) : fields32(raw);
  XyUse u;
  for (unsigned i = 0; i < f.nsrc; i++) {
    const unsigned r = f.src[i];
    if (!XyUnit::is_port(r)) continue;
    bool seen = false;  // (a port named twice is read once)
    for (unsigned k = 0; k < u.nsrc; k++) seen |= u.src[k] == r;
    if (seen) continue;
    u.src[u.nsrc++] = u8(r);
    r_[r] = xy_.read_port(r);
  }
  if (f.dst >= 0 && XyUnit::is_port(unsigned(f.dst))) {
    u.dst = u8(f.dst);
    u.writes = !f.conditional || cond(raw & 0x1f);
  }
  return u;
}

void Cpu::xy_after(const XyUse& u) {
  if (u.writes) xy_.write_port(u.dst, r_[u.dst]);
}

// ---------------------------------------------------------------------------
// 32-bit instructions
// ---------------------------------------------------------------------------

void Cpu::exec32(u32 op) {
  switch (op >> 27) {
    case 0x00: {  // Bcc s21 / B s25
      u32 off = ((op >> 17) & 0x3ff) | (((op >> 6) & 0x3ff) << 10);
      const bool d = op & 0x20;
      if (op & 0x10000) {
        off = sext(off | ((op & 0xf) << 20), 24) * 2;
        branch_rel(off, d, false);
      } else if (cond(op & 0x1f)) {
        branch_rel(sext(off, 20) * 2, d, false);
      }
      return;
    }

    case 0x01: {
      const bool d = op & 0x20;
      if (!(op & 0x10000)) {  // BLcc s21 / BL s25 (offset bit 1 is the lsb)
        u32 off = (((op >> 18) & 0x1ff) << 1) | (((op >> 6) & 0x3ff) << 10);
        if (op & 0x20000) {
          off = sext(off | ((op & 0xf) << 20), 24) * 2;
          branch_rel(off, d, true);
        } else if (cond(op & 0x1f)) {
          branch_rel(sext(off, 20) * 2, d, true);
        }
        return;
      }
      // BRcc / BBITn: compare and branch.
      const unsigned b = f_b(op);
      u32 vb, vc;
      if (op & 0x10) {
        use_limm(b);
        vb = r_[b];
        vc = f_c(op);
      } else {
        const unsigned c = f_c(op);
        use_limm(b, c);
        vb = r_[b];
        vc = r_[c];
      }
      bool take;
      switch (op & 0xf) {
        case 0x0: take = vb == vc; break;
        case 0x1: take = vb != vc; break;
        case 0x2: take = s32(vb) < s32(vc); break;
        case 0x3: take = s32(vb) >= s32(vc); break;
        case 0x4: take = vb < vc; break;
        case 0x5: take = vb >= vc; break;
        case 0xe: take = !((vb >> (vc & 31)) & 1); break;
        case 0xf: take = (vb >> (vc & 31)) & 1; break;
        default: fault("illegal BRcc %08x", op); return;
      }
      if (take) {
        const u32 off = sext(((op >> 17) & 0x7f) | (((op >> 15) & 1) << 7), 8) * 2;
        branch_rel(off, d, false);
      }
      return;
    }

    case 0x02: {  // LD a,[b,s9]
      const u32 s9 = sext(((op >> 16) & 0xff) | (((op >> 15) & 1) << 8), 9);
      const unsigned b = f_b(op);
      use_limm(b);
      do_load(f_a(op), b, s9, (op >> 7) & 3, (op >> 6) & 1, (op >> 9) & 3);
      return;
    }

    case 0x03: {  // ST c,[b,s9]
      const u32 s9 = sext(((op >> 16) & 0xff) | (((op >> 15) & 1) << 8), 9);
      const unsigned b = f_b(op), c = f_c(op);
      use_limm(b, c);
      do_store(b, s9, c, (op >> 1) & 3, (op >> 3) & 3);
      return;
    }

    case 0x04: exec_op04(op); return;
    case 0x05: exec_op05(op); return;

    default:
      fault("unimplemented extension instruction %08x (major %02x)", op, op >> 27);
      return;
  }
}

void Cpu::exec_op04(u32 op) {
  const unsigned sub = (op >> 16) & 63;
  const bool F = f_f(op);
  const unsigned b = f_b(op);

  // Decode the three-operand general format. Returns false if the
  // instruction's condition fails.
  unsigned dst = 0;
  u32 s1 = 0, s2 = 0;
  auto general = [&]() -> bool {
    switch (f_p(op)) {
      case 0: {
        const unsigned c = f_c(op);
        use_limm(b, c);
        dst = f_a(op); s1 = r_[b]; s2 = r_[c];
        return true;
      }
      case 1:
        use_limm(b);
        dst = f_a(op); s1 = r_[b]; s2 = f_c(op);
        return true;
      case 2:
        use_limm(b);
        dst = b; s1 = r_[b]; s2 = f_s12(op);
        return true;
      default:
        if (op & 0x20) {
          use_limm(b);
          s2 = f_c(op);
        } else {
          const unsigned c = f_c(op);
          use_limm(b, c);
          s2 = r_[c];
        }
        dst = b; s1 = r_[b];
        return cond(op & 0x1f);
    }
  };

  // Single-source operand (c / u6 / s12 / cc) used by MOV, FLAG, Jcc, LR.
  // `creg` receives the register used, or 64 for an immediate.
  unsigned creg = 64;
  auto source = [&](u32& v) -> bool {
    switch (f_p(op)) {
      case 0: creg = f_c(op); use_limm(creg); v = r_[creg]; return true;
      case 1: v = f_c(op); return true;
      case 2: v = f_s12(op); return true;
      default:
        if (op & 0x20) { v = f_c(op); }
        else { creg = f_c(op); use_limm(creg); v = r_[creg]; }
        return cond(op & 0x1f);
    }
  };

  auto shl = [](u32 v, u32 n) { return v << (n & 31); };

  switch (sub) {
    case 0x00: if (general()) setr(dst, alu_add(s1, s2, 0, F)); return;               // ADD
    case 0x01: if (general()) setr(dst, alu_add(s1, s2, fc(), F)); return;            // ADC
    case 0x02: if (general()) setr(dst, alu_sub(s1, s2, 0, F)); return;               // SUB
    case 0x03: if (general()) setr(dst, alu_sub(s1, s2, fc(), F)); return;            // SBC
    case 0x04: if (general()) { u32 r = s1 & s2; if (F) flags_nz(r); setr(dst, r); } return;   // AND
    case 0x05: if (general()) { u32 r = s1 | s2; if (F) flags_nz(r); setr(dst, r); } return;   // OR
    case 0x06: if (general()) { u32 r = s1 & ~s2; if (F) flags_nz(r); setr(dst, r); } return;  // BIC
    case 0x07: if (general()) { u32 r = s1 ^ s2; if (F) flags_nz(r); setr(dst, r); } return;   // XOR
    case 0x08:    // MAX
    case 0x09: {  // MIN
      if (!general()) return;
      const bool pick2 = (sub == 0x08) ? s32(s2) >= s32(s1) : s32(s2) <= s32(s1);
      if (F) {
        const u32 alu = s1 - s2;
        flags_nz(alu);
        set_flag(kV, ((s1 ^ s2) & (s1 ^ alu)) >> 31);
        set_flag(kC, pick2);
      }
      setr(dst, pick2 ? s2 : s1);
      return;
    }
    case 0x0a: {  // MOV b, src
      u32 v;
      if (!source(v)) return;
      setr(b, v);
      if (F) flags_nz(v);
      return;
    }
    case 0x0b: if (general()) flags_nz(s1 & s2); return;                              // TST
    case 0x0c: if (general()) alu_sub(s1, s2, 0, true); return;                       // CMP
    case 0x0d: if (general()) alu_sub(s2, s1, 0, true); return;                       // RCMP
    case 0x0e: if (general()) setr(dst, alu_sub(s2, s1, 0, F)); return;               // RSUB
    case 0x0f: if (general()) { u32 r = s1 | (1u << (s2 & 31)); if (F) flags_nz(r); setr(dst, r); } return;   // BSET
    case 0x10: if (general()) { u32 r = s1 & ~(1u << (s2 & 31)); if (F) flags_nz(r); setr(dst, r); } return;  // BCLR
    case 0x11: if (general()) flags_nz(s1 & (1u << (s2 & 31))); return;               // BTST
    case 0x12: if (general()) { u32 r = s1 ^ (1u << (s2 & 31)); if (F) flags_nz(r); setr(dst, r); } return;   // BXOR
    case 0x13: if (general()) {                                                         // BMSK
      const unsigned n = s2 & 31;
      u32 r = s1 & (n == 31 ? 0xffffffffu : ((2u << n) - 1));
      if (F) flags_nz(r);
      setr(dst, r);
    } return;
    case 0x14: if (general()) setr(dst, alu_add(s1, shl(s2, 1), 0, F)); return;        // ADD1
    case 0x15: if (general()) setr(dst, alu_add(s1, shl(s2, 2), 0, F)); return;        // ADD2
    case 0x16: if (general()) setr(dst, alu_add(s1, shl(s2, 3), 0, F)); return;        // ADD3
    case 0x17: if (general()) setr(dst, alu_sub(s1, shl(s2, 1), 0, F)); return;        // SUB1
    case 0x18: if (general()) setr(dst, alu_sub(s1, shl(s2, 2), 0, F)); return;        // SUB2
    case 0x19: if (general()) setr(dst, alu_sub(s1, shl(s2, 3), 0, F)); return;        // SUB3
    case 0x1a: case 0x1b: case 0x1c: case 0x1d: {  // MPY, MPYH, MPYHU, MPYU
      static const char* names[] = {"mpy", "mpyh", "mpyhu", "mpyu"};
      unverified(names[sub - 0x1a]);
      if (!general()) return;
      const bool sgn = sub == 0x1a || sub == 0x1b;
      const s64 p = sgn ? s64(s32(s1)) * s64(s32(s2)) : s64(u64(s1) * u64(s2));
      const u64 up = u64(p);
      const bool high = sub == 0x1b || sub == 0x1c;
      const u32 r = high ? u32(up >> 32) : u32(up);
      if (F) {
        flags_nz(r);
        if (!high) set_flag(kV, sgn ? (p != s64(s32(r))) : ((up >> 32) != 0));
      }
      setr(dst, r);
      return;
    }

    case 0x20: case 0x21: case 0x22: case 0x23: {  // Jcc, Jcc.D, JLcc, JLcc.D
      const bool d = sub & 1, link = sub & 2;
      u32 target;
      if (!source(target)) return;
      if (F) {
        if (creg == kILINK1) status32_ = status32_l1_;
        else if (creg == kILINK2) status32_ = status32_l2_;
        else LOG_W("j.f through non-ilink register r%u at %08x", creg, pc_);
      }
      jump(target, d, link);
      return;
    }

    case 0x28: {  // LPcc
      const unsigned p = f_p(op);
      if (p == 2) {
        lp_start_ = pc_ + len_;
        lp_end_ = (pc_ & ~3u) + f_s12(op) * 2;
      } else if (p == 3 && (op & 0x20)) {
        const u32 end = (pc_ & ~3u) + f_c(op) * 2;
        if (cond(op & 0x1f)) {
          lp_start_ = pc_ + len_;
          lp_end_ = end;
        } else {
          next_pc_ = end;  // Skip the loop body.
          redirected_ = true;
        }
      } else {
        fault("illegal LP form %08x", op);
      }
      return;
    }

    case 0x29: {  // FLAG
      u32 v;
      if (!source(v)) return;
      if (v & kH) {
        halted_ = true;
        attention_ = true;
        stop_ = Stop::Halted;
        stop_msg_ = "FLAG with H bit set (processor halt)";
        pc_ += len_;
        return;
      }
      const u32 mask = kE1 | kE2 | kV | kC | kN | kZ;
      status32_ = (status32_ & ~mask) | (v & mask);
      attention_ = true;  // interrupt enables may have changed
      return;
    }

    case 0x2a: {  // LR b,[c]
      u32 reg;
      if (!source(reg)) return;
      setr(b, aux_read(reg));
      return;
    }

    case 0x2b: {  // SR b,[c]
      u32 reg, val;
      switch (f_p(op)) {
        case 0: { const unsigned c = f_c(op); use_limm(b, c); val = r_[b]; reg = r_[c]; break; }
        case 1: use_limm(b); val = r_[b]; reg = f_c(op); break;
        case 2: use_limm(b); val = r_[b]; reg = f_s12(op); break;
        default:
          if (op & 0x20) { use_limm(b); reg = f_c(op); }
          else { const unsigned c = f_c(op); use_limm(b, c); reg = r_[c]; }
          val = r_[b];
          if (!cond(op & 0x1f)) return;
      }
      aux_write(reg, val);
      return;
    }

    case 0x2f: {  // Single-operand group
      const unsigned sop = op & 63;
      if (sop == 0x3f) {  // Zero-operand group
        switch ((op >> 24) & 7) {
          case 1:  // SLEEP
            sleeping_ = true;
            attention_ = true;
            debug_ |= 1u << 23;
            return;
          case 2: fault("SWI/TRAP0 not implemented"); return;
          case 3: return;  // SYNC: no outstanding memory transactions to wait for.
          case 4: fault("RTIE is not an ARCtangent-A5 instruction"); return;
          case 5:  // BRK
            halted_ = true;
            attention_ = true;
            stop_ = Stop::Halted;
            stop_msg_ = "BRK instruction";
            return;
          default: fault("illegal zero-operand instruction %08x", op); return;
        }
      }
      u32 src;
      switch (f_p(op)) {
        case 0: { const unsigned c = f_c(op); use_limm(c); src = r_[c]; break; }
        case 1: src = f_c(op); break;
        default: fault("illegal single-operand form %08x", op); return;
      }
      u32 r;
      switch (sop) {
        case 0x00:  // ASL
          r = src << 1;
          if (F) { flags_nz(r); set_flag(kC, src >> 31); set_flag(kV, (src ^ r) >> 31); }
          break;
        case 0x01:  // ASR
          r = u32(s32(src) >> 1);
          if (F) { flags_nz(r); set_flag(kC, src & 1); }
          break;
        case 0x02:  // LSR
          r = src >> 1;
          if (F) { flags_nz(r); set_flag(kC, src & 1); }
          break;
        case 0x03:  // ROR
          r = std::rotr(src, 1);
          if (F) { flags_nz(r); set_flag(kC, src & 1); }
          break;
        case 0x04:  // RRC
          r = (src >> 1) | (fc() ? 0x80000000u : 0);
          if (F) { flags_nz(r); set_flag(kC, src & 1); }
          break;
        case 0x05: r = sext(src, 8); if (F) flags_nz(r); break;   // SEXB
        case 0x06: r = sext(src, 16); if (F) flags_nz(r); break;  // SEXW
        case 0x07: r = src & 0xff; if (F) flags_nz(r); break;     // EXTB
        case 0x08: r = src & 0xffff; if (F) flags_nz(r); break;   // EXTW
        case 0x09:  // ABS
          r = (src >> 31) ? 0u - src : src;
          if (F) {
            set_flag(kZ, r == 0);
            set_flag(kN, src == 0x80000000u);
            set_flag(kC, src >> 31);
            set_flag(kV, src == 0x80000000u);
          }
          break;
        case 0x0a: r = ~src; if (F) flags_nz(r); break;  // NOT
        case 0x0b:  // RLC
          r = (src << 1) | (fc() ? 1 : 0);
          if (F) { flags_nz(r); set_flag(kC, src >> 31); }
          break;
        case 0x0c: {  // EX b,[c]: swap register with memory word
          const u32 addr = src;
          const u32 old = rd32(addr);
          wr32(addr, r_[b]);
          setr(b, old);
          return;
        }
        default: fault("illegal single-operand instruction %08x", op); return;
      }
      setr(b, r);
      return;
    }

    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37: {  // LD a,[b,c]
      const unsigned c = f_c(op);
      use_limm(b, c);
      do_load(f_a(op), b, r_[c], (op >> 17) & 3, (op >> 16) & 1, f_p(op));
      return;
    }

    default:
      fault("illegal instruction %08x (04 sub %02x)", op, sub);
      return;
  }
}

void Cpu::exec_op05(u32 op) {
  const unsigned sub = (op >> 16) & 63;
  const bool F = f_f(op);
  const unsigned b = f_b(op);

  unsigned dst = 0;
  u32 s1 = 0, s2 = 0;
  auto general = [&]() -> bool {
    switch (f_p(op)) {
      case 0: { const unsigned c = f_c(op); use_limm(b, c); dst = f_a(op); s1 = r_[b]; s2 = r_[c]; return true; }
      case 1: use_limm(b); dst = f_a(op); s1 = r_[b]; s2 = f_c(op); return true;
      case 2: use_limm(b); dst = b; s1 = r_[b]; s2 = f_s12(op); return true;
      default:
        if (op & 0x20) { use_limm(b); s2 = f_c(op); }
        else { const unsigned c = f_c(op); use_limm(b, c); s2 = r_[c]; }
        dst = b; s1 = r_[b];
        return cond(op & 0x1f);
    }
  };

  switch (sub) {
    case 0x00: {  // ASL multiple
      if (!general()) return;
      const unsigned n = s2 & 31;
      const u32 r = s1 << n;
      if (F) { flags_nz(r); if (n) set_flag(kC, (s1 >> (32 - n)) & 1); }
      setr(dst, r);
      return;
    }
    case 0x01: {  // LSR multiple
      if (!general()) return;
      const unsigned n = s2 & 31;
      const u32 r = s1 >> n;
      if (F) { flags_nz(r); if (n) set_flag(kC, (s1 >> (n - 1)) & 1); }
      setr(dst, r);
      return;
    }
    case 0x02: {  // ASR multiple
      if (!general()) return;
      const unsigned n = s2 & 31;
      const u32 r = u32(s32(s1) >> n);
      if (F) { flags_nz(r); if (n) set_flag(kC, (s1 >> (n - 1)) & 1); }
      setr(dst, r);
      return;
    }
    case 0x03: {  // ROR multiple (MAME-DIFF: MAME rotates by the wrong amount)
      if (!general()) return;
      const unsigned n = s2 & 31;
      const u32 r = std::rotr(s1, n);
      if (F) { flags_nz(r); if (n) set_flag(kC, (s1 >> (n - 1)) & 1); }
      setr(dst, r);
      return;
    }
    case 0x04: case 0x05: {  // MUL64 / MULU64 (result to MLO/MMID/MHI only)
      if (!general()) return;
      const u64 p = (sub == 0x04) ? u64(s64(s32(s1)) * s64(s32(s2))) : u64(s1) * u64(s2);
      r_[kMLO] = u32(p);
      // Per toadster172's hardware findings, MMID receives the low word too.
      r_[kMMID] = u32(p);
      r_[kMHI] = u32(p >> 32);
      return;
    }
    case 0x06: case 0x07: {  // ADDS / SUBS (saturating)
      unverified(sub == 0x06 ? "adds" : "subs");
      if (!general()) return;
      bool sat = false;
      const s64 wide = (sub == 0x06) ? s64(s32(s1)) + s32(s2) : s64(s32(s1)) - s32(s2);
      const u32 r = u32(sat32(wide, &sat));
      if (F) { flags_nz(r); set_flag(kV, sat); set_flag(kC, false); }
      setr(dst, r);
      return;
    }
    case 0x08: {  // DIVAW: division assist step
      unverified("divaw");
      if (!general()) return;
      u32 r;
      if (s1 == 0) {
        r = 0;
      } else {
        const u32 t = s1 << 1;
        const u32 d = s2 << 16;
        r = (t >= d) ? ((t - d) | 1) : t;
      }
      setr(dst, r);
      return;
    }
    case 0x0a: case 0x0b: {  // ASLS / ASRS (saturating shifts, signed amount)
      unverified(sub == 0x0a ? "asls" : "asrs");
      if (!general()) return;
      s32 n = s32(s2);
      if (sub == 0x0b) n = -n;
      bool sat = false;
      s32 r;
      if (n >= 0) {
        const s64 wide = s64(s32(s1)) << std::min(n, 32);
        r = sat32(wide, &sat);
      } else {
        r = s32(s1) >> std::min(-n, 31);
      }
      if (F) { flags_nz(u32(r)); set_flag(kV, sat); set_flag(kC, false); }
      setr(dst, u32(r));
      return;
    }
    case 0x0c: case 0x10: case 0x14: {  // MULDW / MACDW / MSUBDW (dual 16x16, XY DSP option)
      if (!general()) return;
      setr(dst, xy_.dual_mac(sub, s1, s2, r_[56], r_[57]));
      return;
    }
    case 0x28: case 0x29: {  // ADDSDW / SUBSDW (dual 16-bit saturating)
      unverified(sub == 0x28 ? "addsdw" : "subsdw");
      if (!general()) return;
      bool sat = false;
      auto lane = [&](unsigned sh) {
        const s32 a = s16(s1 >> sh), c = s16(s2 >> sh);
        return u32(u16(sat16(sub == 0x28 ? a + c : a - c, &sat))) << sh;
      };
      const u32 r = lane(0) | lane(16);
      if (F) { flags_nz(r); set_flag(kV, sat); set_flag(kC, false); }
      setr(dst, r);
      return;
    }
    case 0x2f: {  // Single-operand extension group
      const unsigned sop = op & 63;
      u32 src;
      switch (f_p(op)) {
        case 0: { const unsigned c = f_c(op); use_limm(c); src = r_[c]; break; }
        case 1: src = f_c(op); break;
        default: fault("illegal extension single-operand form %08x", op); return;
      }
      u32 r;
      bool sat = false;
      switch (sop) {
        case 0x00: r = std::rotl(src, 16); if (F) flags_nz(r); break;  // SWAP
        case 0x01:  // NORM
          if (src == 0 || src == 0xffffffffu) r = 31;
          else r = ((src >> 31) ? std::countl_one(src) : std::countl_zero(src)) - 1;
          if (F) flags_nz(src);
          break;
        case 0x02: unverified("sat16"); r = u32(s32(sat16(s32(src), &sat))); if (F) { flags_nz(r); set_flag(kV, sat); } break;
        case 0x03: {  // RND16
          unverified("rnd16");
          const s64 v = (s64(s32(src)) + 0x8000) >> 16;
          r = u32(s32(sat16(s32(std::clamp<s64>(v, INT32_MIN, INT32_MAX)), &sat)));
          if (F) { flags_nz(r); set_flag(kV, sat); }
          break;
        }
        case 0x04: {  // ABSSW
          unverified("abssw");
          const s32 v = s16(src);
          r = u32(u16(sat16(v < 0 ? -v : v, &sat)));
          if (F) { flags_nz(r); set_flag(kV, sat); }
          break;
        }
        case 0x05: {  // ABSS
          unverified("abss");
          const s64 v = s32(src);
          r = u32(sat32(v < 0 ? -v : v, &sat));
          if (F) { flags_nz(r); set_flag(kV, sat); }
          break;
        }
        case 0x06: unverified("negsw"); r = u32(u16(sat16(-s32(s16(src)), &sat))); if (F) { flags_nz(r); set_flag(kV, sat); } break;
        case 0x07: unverified("negs"); r = u32(sat32(-s64(s32(src)), &sat)); if (F) { flags_nz(r); set_flag(kV, sat); } break;
        case 0x08: {  // NORMW
          const u32 v = sext(src, 16);
          if ((v & 0xffff) == 0 || (v & 0xffff) == 0xffff) r = 15;
          else r = ((v >> 31) ? std::countl_one(v) : std::countl_zero(v)) - 17;
          if (F) flags_nz(v);
          break;
        }
        default: fault("illegal extension single-operand instruction %08x", op); return;
      }
      setr(b, r);
      return;
    }
    default:
      fault("illegal instruction %08x (05 sub %02x)", op, sub);
      return;
  }
}

// ---------------------------------------------------------------------------
// 16-bit instructions
// ---------------------------------------------------------------------------

void Cpu::exec16(u16 op) {
  const unsigned b = h_b(op), c = h_c(op);
  const u32 u5 = op & 0x1f;

  switch (op >> 11) {
    case 0x0c: {  // LD_S / LDB_S / LDW_S a,[b,c]; ADD_S a,b,c
      const u32 addr = r_[b] + r_[c];
      switch ((op >> 3) & 3) {
        case 0: r_[h_a(op)] = rd32(addr); break;
        case 1: r_[h_a(op)] = rd8(addr); break;
        case 2: r_[h_a(op)] = rd16(addr); break;
        case 3: r_[h_a(op)] = addr; break;
      }
      return;
    }

    case 0x0d: {  // ADD_S / SUB_S / ASL_S / ASR_S c,b,u3
      const u32 u3 = op & 7;
      switch ((op >> 3) & 3) {
        case 0: r_[c] = r_[b] + u3; break;
        case 1: r_[c] = r_[b] - u3; break;
        case 2: r_[c] = r_[b] << u3; break;
        case 3: r_[c] = u32(s32(r_[b]) >> u3); break;
      }
      return;
    }

    case 0x0e: {  // ADD_S b,b,h / MOV_S b,h / CMP_S b,h / MOV_S h,b
      const unsigned h = ((op & 7) << 3) | ((op >> 5) & 7);
      switch ((op >> 3) & 3) {
        case 0: use_limm(h); r_[b] = r_[b] + r_[h]; break;
        case 1: use_limm(h); r_[b] = r_[h]; break;
        case 2: use_limm(h); alu_sub(r_[b], r_[h], 0, true); break;
        case 3: setr(h, r_[b]); break;
      }
      return;
    }

    case 0x0f: {
      switch (op & 0x1f) {
        case 0x00:
          switch ((op >> 5) & 7) {
            case 0: jump(r_[b], false, false); return;  // J_S [b]
            case 1: jump(r_[b], true, false); return;   // J_S.D [b]
            case 2: jump(r_[b], false, true); return;   // JL_S [b]
            case 3: jump(r_[b], true, true); return;    // JL_S.D [b]
            case 6: if (!fz()) r_[b] = 0; return;       // SUB_S.NE b,b,b
            case 7:
              switch ((op >> 8) & 7) {
                case 0: return;  // NOP_S
                case 1: fault("UNIMP_S"); return;
                case 4: if (fz()) jump(r_[kBLINK], false, false); return;   // JEQ_S [blink]
                case 5: if (!fz()) jump(r_[kBLINK], false, false); return;  // JNE_S [blink]
                case 6: jump(r_[kBLINK], false, false); return;             // J_S [blink]
                case 7: jump(r_[kBLINK], true, false); return;              // J_S.D [blink]
                default: fault("illegal 16-bit instruction %04x", op); return;
              }
            default: fault("illegal 16-bit instruction %04x", op); return;
          }
        case 0x02: r_[b] = r_[b] - r_[c]; return;                   // SUB_S
        case 0x04: r_[b] = r_[b] & r_[c]; return;                   // AND_S
        case 0x05: r_[b] = r_[b] | r_[c]; return;                   // OR_S
        case 0x06: r_[b] = r_[b] & ~r_[c]; return;                  // BIC_S
        case 0x07: r_[b] = r_[b] ^ r_[c]; return;                   // XOR_S
        case 0x0b: flags_nz(r_[b] & r_[c]); return;                 // TST_S
        case 0x0c: {                                                // MUL64_S
          const u64 p = u64(s64(s32(r_[b])) * s64(s32(r_[c])));
          r_[kMLO] = u32(p);
          r_[kMMID] = u32(p);
          r_[kMHI] = u32(p >> 32);
          return;
        }
        case 0x0d: r_[b] = sext(r_[c], 8); return;                  // SEXB_S
        case 0x0e: r_[b] = sext(r_[c], 16); return;                 // SEXW_S
        case 0x0f: r_[b] = r_[c] & 0xff; return;                    // EXTB_S
        case 0x10: r_[b] = r_[c] & 0xffff; return;                  // EXTW_S
        case 0x11: r_[b] = (r_[c] >> 31) ? 0u - r_[c] : r_[c]; return;  // ABS_S
        case 0x12: r_[b] = ~r_[c]; return;                          // NOT_S
        case 0x13: r_[b] = 0u - r_[c]; return;                      // NEG_S
        case 0x14: r_[b] = r_[b] + (r_[c] << 1); return;            // ADD1_S
        case 0x15: r_[b] = r_[b] + (r_[c] << 2); return;            // ADD2_S
        case 0x16: r_[b] = r_[b] + (r_[c] << 3); return;            // ADD3_S
        case 0x18: r_[b] = r_[b] << (r_[c] & 31); return;           // ASL_S b,b,c
        case 0x19: r_[b] = r_[b] >> (r_[c] & 31); return;           // LSR_S b,b,c
        case 0x1a: r_[b] = u32(s32(r_[b]) >> (r_[c] & 31)); return; // ASR_S b,b,c
        case 0x1b: r_[b] = r_[c] << 1; return;                      // ASL_S b,c
        case 0x1c: r_[b] = u32(s32(r_[c]) >> 1); return;            // ASR_S b,c
        case 0x1d: r_[b] = r_[c] >> 1; return;                      // LSR_S b,c
        case 0x1e: fault("TRAP_S %u not implemented", (op >> 5) & 63); return;
        case 0x1f:  // BRK_S
          halted_ = true;
          attention_ = true;
          stop_ = Stop::Halted;
          stop_msg_ = "BRK_S instruction";
          return;
        default: fault("illegal 16-bit instruction %04x", op); return;
      }
    }

    case 0x10: r_[c] = rd32(r_[b] + (u5 << 2)); return;             // LD_S c,[b,u7]
    case 0x11: r_[c] = rd8(r_[b] + u5); return;                     // LDB_S c,[b,u5]
    case 0x12: r_[c] = rd16(r_[b] + (u5 << 1)); return;             // LDW_S c,[b,u6]
    case 0x13: r_[c] = sext(rd16(r_[b] + (u5 << 1)), 16); return;   // LDW_S.X c,[b,u6]
    case 0x14: wr32(r_[b] + (u5 << 2), r_[c]); return;              // ST_S c,[b,u7]
    case 0x15: wr8(r_[b] + u5, u8(r_[c])); return;                  // STB_S c,[b,u5]
    case 0x16: wr16(r_[b] + (u5 << 1), u16(r_[c])); return;         // STW_S c,[b,u6]

    case 0x17:  // Shift/sub/bit with u5
      switch ((op >> 5) & 7) {
        case 0: r_[b] <<= u5; return;                               // ASL_S
        case 1: r_[b] >>= u5; return;                               // LSR_S
        case 2: r_[b] = u32(s32(r_[b]) >> u5); return;              // ASR_S
        case 3: r_[b] -= u5; return;                                // SUB_S
        case 4: r_[b] |= 1u << u5; return;                          // BSET_S
        case 5: r_[b] &= ~(1u << u5); return;                       // BCLR_S
        case 6: r_[b] &= (u5 == 31) ? 0xffffffffu : ((2u << u5) - 1); return;  // BMSK_S
        case 7: flags_nz(r_[b] & (1u << u5)); return;               // BTST_S
      }
      return;

    case 0x18:  // Stack-pointer based
      switch ((op >> 5) & 7) {
        case 0: r_[b] = rd32(r_[kSP] + (u5 << 2)); return;          // LD_S b,[sp,u7]
        case 1: r_[b] = rd8(r_[kSP] + (u5 << 2)); return;           // LDB_S b,[sp,u7]
        case 2: wr32(r_[kSP] + (u5 << 2), r_[b]); return;           // ST_S b,[sp,u7]
        case 3: wr8(r_[kSP] + (u5 << 2), u8(r_[b])); return;        // STB_S b,[sp,u7]
        case 4: r_[b] = r_[kSP] + (u5 << 2); return;                // ADD_S b,sp,u7
        case 5:
          switch ((op >> 8) & 7) {
            case 0: r_[kSP] += u5 << 2; return;                     // ADD_S sp,sp,u7
            case 1: r_[kSP] -= u5 << 2; return;                     // SUB_S sp,sp,u7
            default: fault("illegal 16-bit instruction %04x", op); return;
          }
        case 6:
          switch (u5) {
            case 0x01: r_[b] = rd32(r_[kSP]); r_[kSP] += 4; return;           // POP_S b
            case 0x11: r_[kBLINK] = rd32(r_[kSP]); r_[kSP] += 4; return;      // POP_S blink
            default: fault("illegal 16-bit instruction %04x", op); return;
          }
        case 7:
          switch (u5) {
            case 0x01: r_[kSP] -= 4; wr32(r_[kSP], r_[b]); return;            // PUSH_S b
            case 0x11: r_[kSP] -= 4; wr32(r_[kSP], r_[kBLINK]); return;       // PUSH_S blink
            default: fault("illegal 16-bit instruction %04x", op); return;
          }
      }
      return;

    case 0x19: {  // GP-relative
      const u32 s9 = sext(op & 0x1ff, 9);
      switch ((op >> 9) & 3) {
        case 0: r_[0] = rd32(r_[kGP] + (s9 << 2)); return;          // LD_S r0,[gp,s11]
        case 1: r_[0] = rd8(r_[kGP] + s9); return;                  // LDB_S r0,[gp,s9]
        case 2: r_[0] = rd16(r_[kGP] + (s9 << 1)); return;          // LDW_S r0,[gp,s10]
        case 3: r_[0] = r_[kGP] + (s9 << 2); return;                // ADD_S r0,gp,s11
      }
      return;
    }

    case 0x1a: r_[b] = rd32(r_[kPCL] + ((op & 0xff) << 2)); return; // LD_S b,[pcl,u10]
    case 0x1b: r_[b] = op & 0xff; return;                           // MOV_S b,u8
    case 0x1c:
      if (op & 0x80) alu_sub(r_[b], op & 0x7f, 0, true);            // CMP_S b,u7
      else r_[b] += op & 0x7f;                                      // ADD_S b,b,u7
      return;

    case 0x1d: {  // BREQ_S / BRNE_S b,0,s8
      const bool take = (op & 0x80) ? r_[b] != 0 : r_[b] == 0;
      if (take) branch_rel(sext(op & 0x7f, 7) * 2, false, false);
      return;
    }

    case 0x1e: {  // B_S / Bcc_S
      bool take;
      u32 off;
      switch ((op >> 9) & 3) {
        case 0: take = true; off = sext(op & 0x1ff, 9) * 2; break;
        case 1: take = fz(); off = sext(op & 0x1ff, 9) * 2; break;
        case 2: take = !fz(); off = sext(op & 0x1ff, 9) * 2; break;
        default: {
          static constexpr unsigned kCc[8] = {0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x06, 0x05, 0x0e};
          take = cond(kCc[(op >> 6) & 7]);  // gt ge lt le hi hs lo ls
          off = sext(op & 0x3f, 6) * 2;
        }
      }
      if (take) branch_rel(off, false, false);
      return;
    }

    case 0x1f:  // BL_S s13
      branch_rel(sext(op & 0x7ff, 11) * 4, false, true);
      return;
  }
}

}  // namespace leap::arc
