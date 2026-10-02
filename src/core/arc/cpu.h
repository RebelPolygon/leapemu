#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/arc/cache.h"
#include "core/arc/xy.h"
#include "core/bus.h"
#include "core/common.h"

namespace leap::arc {

class Cpu;
class Jit;

// A pre-decoded instruction (see decode.cpp). Instructions in ROM are decoded
// once and dispatched through `fn` afterwards.
struct DecodedOp {
  void (*fn)(Cpu&, const DecodedOp&) = nullptr;
  u32 raw = 0;   // instruction word (16-bit ops in the low half)
  u32 imm = 0;   // immediate, precomputed mask or absolute branch target
  u32 limm = 0;  // long immediate, if the instruction has one
  u8 len = 0;    // length in bytes including the long immediate
  u8 a = 0, b = 0, c = 0;  // register numbers (16-bit fields already expanded)
  u8 cc = 0;     // condition code
  u8 delay = 0;  // .d branch
  u16 cost8 = 8; // base cost in 1/8 cycles: 1 cycle + instruction-fetch wait states
  u8 hook = 0;   // an observation hook runs before this instruction (see Cpu::set_pc_hook)
};

// ARCtangent-A5 (ARCompact ISA, basecase + barrel shifter, norm/swap and
// mul64 extensions) as found in the Leapster "DART" SoC.
//
// Semantics follow the ARCompact Programmer's Reference and were cross-checked
// against MAME's arcompact core (BSD-3-Clause, David Haywood; Leapster fixes by
// Alice Shelton in toadster172/mame@leapster). Places where this core
// deliberately differs from MAME are marked "MAME-DIFF".
class Cpu {
 public:
  // Core register numbers with special meaning.
  enum : unsigned {
    kGP = 26, kFP = 27, kSP = 28, kILINK1 = 29, kILINK2 = 30, kBLINK = 31,
    kMLO = 57, kMMID = 58, kMHI = 59, kLP_COUNT = 60, kLIMM = 62, kPCL = 63,
  };

  // STATUS32 bits.
  enum : u32 {
    kH = 1u << 0, kE1 = 1u << 1, kE2 = 1u << 2, kL = 1u << 12,
    kV = 1u << 8, kC = 1u << 9, kN = 1u << 10, kZ = 1u << 11,
  };

  // Execution backend.
  //   Interpreter:        fetch + decode every instruction (reference path)
  //   CachedInterpreter:  decode ROM instructions once, dispatch pre-decoded
  //                       handlers in a tight loop (default)
  //   Jit:                dynamic recompiler (see jit.h): ROM code compiled to
  //                       host code in blocks; CachedInterpreter where it
  //                       cannot run (other hosts, the cache timing model)
  enum class Backend { Interpreter, CachedInterpreter, Jit };

  // Why run() returned early.
  enum class Stop { None, Breakpoint, Halted, Fault };

  // Auxiliary registers not implemented by the core are forwarded here.
  struct AuxHandler {
    std::function<u32(u32 reg)> read;
    std::function<void(u32 reg, u32 value)> write;
  };

  explicit Cpu(Bus& bus);
  ~Cpu();

  void set_aux_handler(AuxHandler h) { aux_ = std::move(h); }
  // Reset vector / initial interrupt vector base (Leapster: 0x40000000).
  void set_reset_vector(u32 v) { reset_vector_ = v; }
  void reset();

  // Latch interrupt `line` (3..31). Edge-style: cleared when taken.
  void raise_irq(unsigned line) { pending_irqs_ |= 1u << line; attention_ = true; }
  void clear_irq(unsigned line) { pending_irqs_ &= ~(1u << line); }

  // Execute up to `cycles` instructions (1 cycle each for now). Returns the
  // number of cycles consumed; check stop_reason() if it returned early.
  u64 run(u64 cycles);
  // Ask run() to return after the current instruction (used by the machine
  // when an I/O write changes the next scheduled event).
  void request_exit() { exit_request_ = true; attention_ = true; }
  // Execute exactly one instruction (or one idle cycle while sleeping).
  void step();

  Stop stop_reason() const { return stop_; }
  const std::string& stop_message() const { return stop_msg_; }
  void clear_stop() { stop_ = Stop::None; stop_msg_.clear(); }
  bool sleeping() const { return sleeping_; }
  bool halted() const { return halted_; }
  // For host tools that call guest functions (Machine::call_guest): start
  // fresh at `pc`, out of any halt, sleep, delay slot or zero-overhead loop.
  void begin_call(u32 pc) {
    halted_ = sleeping_ = in_delay_slot_ = delay_pending_ = false;
    stop_ = Stop::None;
    lp_end_ = lp_start_ = 0;
    r_[kLP_COUNT] = 0;
    pc_ = pc & ~1u;
  }

  // Breakpoints on instruction fetch address.
  std::unordered_set<u32>& breakpoints() { return breakpoints_; }

  // Reproduce known MAME arcompact quirks so traces can be compared in
  // lockstep with the oracle (tools/oracle). Never needed for correctness.
  bool mame_compat = false;

  // Idle-loop skipping. When execution reaches `idle_hint` twice in a row with
  // identical register state, no interrupt taken in between, interrupts
  // enabled and nothing pending, the loop provably spins until the next
  // interrupt, so the rest of the time slice is skipped (like SLEEP).
  // The machine sets the hint to the idle task's loop head.
  bool idle_skip = true;
  void set_backend(Backend b) { backend_ = b; cur_page_ = ~0u; }
  Backend backend() const { return backend_; }
  static const char* backend_name(Backend b) {
    switch (b) {
      case Backend::Interpreter: return "Interpreter";
      case Backend::CachedInterpreter: return "Cached Interpreter";
      case Backend::Jit: return "JIT Recompiler";
    }
    return "?";
  }
  static bool backend_available(Backend b);
  u32 idle_hint = ~0u;

  // Cache timing model (see cache.h). With a cache configured, accesses to
  // cacheable memory cost nothing extra on a hit; a miss costs one line fill
  // at the page's fill16 rate, plus `writeback8` (1/8 cycles) if a dirty line
  // is evicted. Without a cache, every access pays the page's flat wait states.
  void configure_caches(const CacheModel::Config& icache, const CacheModel::Config& dcache, u32 writeback8);
  const CacheModel& icache() const { return ic_; }
  const CacheModel& dcache() const { return dc_; }

  // Called before each instruction executes (after interrupt dispatch).
  std::function<void(const Cpu&)> trace_hook;
  // Debugging the JIT (tools/debug/jit_diff): blocks compiled while this is
  // set call it as they start (linked ones too), with the time in 1/8 cycles.
  std::function<void(u32 start, u64 time8)> jit_block_hook;

  // Observation hooks: `fn(ctx, cpu)` runs just before the instruction at `pc`
  // executes, in every backend, without disabling the fast paths. Hooks must
  // not change emulated state (display features that follow a game's own
  // drawing use them; see core/drawcap.h).
  using PcHookFn = void (*)(void* ctx, const Cpu& cpu);
  void set_pc_hook(u32 pc, PcHookFn fn, void* ctx);
  void clear_pc_hooks();

  // ---- architectural state (public for debuggers / save states) ----
  u32 pc() const { return pc_; }
  void set_pc(u32 pc) { pc_ = pc & ~1u; }
  u32 reg(unsigned i) const { return r_[i & 63]; }
  const XyUnit& xy() const { return xy_; }
  void set_reg(unsigned i, u32 v) { r_[i & 63] = v; }
  u32 status32() const { return status32_; }
  void set_status32(u32 v) { status32_ = v; }
  u32 lp_start() const { return lp_start_; }
  u32 lp_end() const { return lp_end_; }
  u32 pending_irqs() const { return pending_irqs_; }
  u32 int_vector_base() const { return int_vector_base_; }
  u64 cycles() const { return cycles_; }
  u32 cycle_eighths() const { return frac8_; }  // sub-cycle part of the count
  // Cycles spent in SLEEP waiting for an interrupt (for load measurement).
  u64 idle_cycles() const { return idle_cycles_; }
  bool in_delay_slot() const { return in_delay_slot_; }

  // Save-state support (see core/state.h). Host-side settings such as
  // breakpoints, hooks and mame_compat are not part of the state.
  template <class Ar>
  void serialize(Ar& ar) {
    // r62 (LIMM) and r63 (PCL) are derived per instruction: not state.
    for (unsigned i = 0; i < kLIMM; i++) ar.io(r_[i]);
    ar.io(pc_); ar.io(status32_); ar.io(status32_l1_); ar.io(status32_l2_);
    ar.io(lp_start_); ar.io(lp_end_); ar.io(int_vector_base_);
    ar.io(aux_irq_lv12_); ar.io(aux_irq_lev_); ar.io(debug_);
    for (auto& t : timer_) for (auto& v : t) ar.io(v);
    ar.io(pending_irqs_);
    ar.io(in_delay_slot_); ar.io(delay_link_); ar.io(delay_target_);
    ar.io(sleeping_); ar.io(halted_); ar.io(cycles_);
    if (ar.version >= 5) ar.io(frac8_); else frac8_ = 0;
    if (ar.version >= 6) { ic_.serialize(ar); dc_.serialize(ar); ar.io(last_iline_); }
    else { ic_.invalidate(); dc_.invalidate(); last_iline_ = ~0u; }
    if (ar.version >= 11) xy_.serialize(ar);
    else if constexpr (Ar::kLoading) xy_ = XyUnit{};
    if constexpr (Ar::kLoading) { stop_ = Stop::None; stop_msg_.clear(); exit_request_ = false; }
  }

 // Drop all cached decoded instructions (call when the memory map changes).
  void flush_decode_cache();
  // Code in RAM that may be cached (decoded, compiled) like ROM: [lo, hi), for
  // a program loaded into RAM (a Leapster 2 download). Its pages are watched
  // for writes (Bus::watch_code), and instructions written over are decoded
  // again before the next one runs. Empty by default: RAM code is interpreted.
  void set_ram_code(u32 lo, u32 hi);
  // RAM changed behind the bus's back (a loaded state): forget RAM code.
  void ram_changed() { if (bus_.code_watched()) flush_decode_cache(); }

 private:
  friend struct Handlers;
  friend class Jit;

  // ---- decode cache ----
  bool use_cache() const { return backend_ != Backend::Interpreter; }
  DecodedOp* decoded(u32 pc);
  void run_burst(u64 end);
  bool select_page(u32 pc);
  u32 cur_page_ = ~0u;          // page whose decoded ops are in cur_ops_
  DecodedOp* cur_ops_ = nullptr;
  Backend backend_ = Backend::CachedInterpreter;
  // Set by anything that needs the general run loop's attention (interrupts,
  // faults, halts, SLEEP, exit requests); the burst loop checks only this.
  bool attention_ = false;
  void decode(u32 pc, DecodedOp& o);
  bool burst_step(u32 pc);  // one run_burst iteration; false: return from it
  u32 ram_code_lo_ = 0, ram_code_hi_ = 0;
  std::vector<std::pair<u32, int>> code_writes_;  // to decode again (set_ram_code)
  static void code_written(void* cpu, u32 addr, int size);
  void apply_code_writes();

  // ---- JIT (jit.h) ----
  std::unique_ptr<Jit> jit_;
  u64 jit_end8_ = 0;        // end of the time slice, 1/8 cycles (checked by blocks)
  bool jit_ready();         // the JIT can run now (creating it if needed)
  void run_jit(u64 end);    // run_burst through compiled blocks
  // Called by compiled code: leave a block after the instruction at `pc`
  // (charging `cost8` + waits, then advancing like the interpreter), and run
  // an instruction through its handler (true: leave the block after it).
  static void jit_exit(Cpu* c, u32 pc, u32 cost8, u32 len);
  static u32 jit_generic(Cpu* c, const DecodedOp* o, u32 pc, u32 prefix_len);
  static void jit_trace(Cpu* c, u32 start) { if (c->jit_block_hook) c->jit_block_hook(start, c->cycles_ * 8 + c->frac8_); }
  std::vector<std::unique_ptr<DecodedOp[]>> dcache_;
  std::vector<u8> dcache_state_;  // per 64 KiB page: 0 unknown, 1 cached, 2 not cacheable

  // ---- execution ----
  void execute();              // one instruction at pc_
  void finish(u32 pc, bool was_delay_slot);  // advance pc after an instruction
  void exec32(u32 op);
  void exec16(u16 op);
  void exec_op04(u32 op);
  void exec_op05(u32 op);
  void check_interrupts();

  // Instruction fetch: 32-bit words are stored as two halfwords, high first.
  u16 fetch16(u32 a) { return bus_.read16(a & ~1u); }
  u32 fetch32(u32 a) { return (u32(fetch16(a)) << 16) | fetch16(a + 2); }

  // Data access. The Leapster ignores misaligned low address bits.
  u32 rd32(u32 a) { stall_ += data_wait(a, false, bus_.wait32(a)); return bus_.read32(a & ~3u); }
  u16 rd16(u32 a) { stall_ += data_wait(a, false, bus_.wait16(a)); return bus_.read16(a & ~1u); }
  u8 rd8(u32 a) { stall_ += data_wait(a, false, bus_.wait16(a)); return bus_.read8(a); }
  void wr32(u32 a, u32 v) { stall_ += data_wait(a, true, bus_.wait32(a)); bus_.write32(a & ~3u, v); }
  void wr16(u32 a, u16 v) { stall_ += data_wait(a, true, bus_.wait16(a)); bus_.write16(a & ~1u, v); }
  void wr8(u32 a, u8 v) { stall_ += data_wait(a, true, bus_.wait16(a)); bus_.write8(a, v); }
  // Wait states of one data access (1/8 cycles): through the data cache for
  // cacheable memory, otherwise the page's flat cost.
  u32 data_wait(u32 a, bool write, u32 flat) {
    if (!dc_.enabled() || !bus_.cacheable(a)) return flat;
    bool wrote_back = false;
    if (dc_.access(a, write, &wrote_back)) return 0;
    return (dc_.line_bytes() / 2) * bus_.fill16(a) + (wrote_back ? writeback8_ : 0);
  }
  // Flat instruction-fetch wait per halfword at `a` (0 where the instruction
  // cache handles fetches instead).
  u32 fetch_wait16(u32 a) const { return ic_.enabled() && bus_.cacheable(a) ? 0 : bus_.wait16(a); }
  // Instruction-cache cost of fetching [pc, pc+len) (1/8 cycles). Only fetches
  // allocate instruction-cache lines, so a line equal to the last one looked
  // up is still resident and needs no lookup.
  u32 icache_fetch(u32 pc, u32 len) {
    u32 cost = 0;
    const u32 first = pc >> ic_.line_shift(), last = (pc + len - 1) >> ic_.line_shift();
    for (u32 line = first; line <= last; line++) {
      if (line == last_iline_) continue;
      last_iline_ = line;
      bool unused;
      if (!ic_.access(line << ic_.line_shift(), false, &unused))
        cost += (ic_.line_bytes() / 2) * bus_.fill16(line << ic_.line_shift());
    }
    return cost;
  }
  u32 icache_fetch_if_cached(u32 pc, u32 len) {
    return ic_.enabled() && bus_.cacheable(pc) ? icache_fetch(pc, len) : 0;
  }
  // Instruction-fetch wait states for the instruction just executed on the
  // reference path (one 16-bit transfer per halfword, including any LIMM).
  u32 fetch_stall(u32 pc) const { return (len_ / 2) * fetch_wait16(pc); }  // 1/8 cycles
  // Advance the cycle counter by `cost8` eighths of a cycle.
  void charge(u32 cost8) {
    frac8_ += cost8;
    cycles_ += frac8_ >> 3;
    frac8_ &= 7;
  }

  // If either register field is 62, fetch the long immediate that follows the
  // current instruction and extend the instruction length.
  void use_limm(unsigned ra, unsigned rb = 0) {
    if ((ra == kLIMM || rb == kLIMM) && !limm_loaded_) {
      r_[kLIMM] = fetch32(pc_ + len_);
      len_ += 4;
      limm_loaded_ = true;
    }
  }
  void setr(unsigned i, u32 v) { if (i < kLIMM) r_[i] = v; }  // r62/r63 are not writable

  // Control flow.
  void jump(u32 target, bool delay, bool link);
  void branch_rel(u32 offset_bytes, bool delay, bool link);

  // Condition codes.
  bool cond(unsigned cc);
  bool fz() const { return status32_ & kZ; }
  bool fn() const { return status32_ & kN; }
  bool fc() const { return status32_ & kC; }
  bool fv() const { return status32_ & kV; }
  void set_flag(u32 f, bool on) { status32_ = on ? (status32_ | f) : (status32_ & ~f); }
  void flags_nz(u32 r) { set_flag(kN, r >> 31); set_flag(kZ, r == 0); }

  // ALU with flag computation.
  u32 alu_add(u32 a, u32 b, u32 carry_in, bool f);
  u32 alu_sub(u32 a, u32 b, u32 borrow_in, bool f);

  // Load/store with addressing modes.
  void do_load(unsigned dst, unsigned base, u32 offset, unsigned zz, bool x, unsigned aa);
  void do_store(unsigned base, u32 offset, u32 value, unsigned zz, unsigned aa);

  // Aux registers.
  u32 aux_read(u32 reg);
  void aux_write(u32 reg, u32 value);

  // XY memory (xy.h). An instruction with a register field in r32-r55 runs
  // through xy_before / xy_after: source ports are read from XY memory into
  // their registers first, and a destination port is stored after.
  struct XyUse { u8 src[3]; u8 nsrc = 0; u8 dst = 0; bool writes = false; };
  static bool xy_fields(u32 raw, bool is16);
  XyUse xy_before(u32 raw, bool is16);
  void xy_after(const XyUse& u);
  XyUnit xy_;

  void fault(const char* fmt, ...);

  Bus& bus_;
  AuxHandler aux_;

  u32 r_[64]{};
  u32 pc_ = 0;
  u32 status32_ = 0;
  u32 status32_l1_ = 0;
  u32 status32_l2_ = 0;
  u32 lp_start_ = 0;
  u32 lp_end_ = 0;
  u32 int_vector_base_ = 0;
  u32 aux_irq_lv12_ = 0;
  u32 aux_irq_lev_ = 0;
  u32 debug_ = 0;
  u32 timer_[2][3]{};  // Core timers (not ticking; the Leapster uses its own).
  u32 pending_irqs_ = 0;
  u32 reset_vector_ = 0;

  // Per-instruction decode state.
  u32 len_ = 0;           // Instruction length including any LIMM.
  u32 next_pc_ = 0;       // Where execution continues after this instruction.
  bool redirected_ = false;  // Control flow left the sequential path.
  bool limm_loaded_ = false;

  // Delay-slot state.
  bool in_delay_slot_ = false;  // The instruction about to run is a delay slot.
  bool delay_pending_ = false;  // Set by a .D branch during this instruction.
  bool delay_link_ = false;
  u32 delay_target_ = 0;

  bool exit_request_ = false;
  u32 stall_ = 0;       // data-access wait states of the current instruction (1/8 cycles)
  u32 fetch_cost_ = 0;  // fetch wait states of the instruction just executed (1/8 cycles)
  u32 frac8_ = 0;       // sub-cycle remainder of the cycle counter (1/8 cycles)
  CacheModel ic_, dc_;
  u32 last_iline_ = ~0u;   // instruction-cache line looked up most recently
  u32 writeback8_ = 0;     // cost of writing back one dirty data-cache line
  bool cur_icached_ = false;  // cur_page_ is fetched through the instruction cache
  bool idle_armed_ = false;
  u64 idle_hash_ = 0;
  u64 idle_state_hash() const;
  bool sleeping_ = false;
  bool halted_ = false;
  Stop stop_ = Stop::None;
  std::string stop_msg_;
  u64 cycles_ = 0;
  u64 idle_cycles_ = 0;

  std::unordered_set<u32> breakpoints_;
  u32 warned_cc_ = 0;

  std::unordered_map<u32, std::pair<PcHookFn, void*>> pc_hooks_;
  void run_pc_hook(u32 pc) {
    const auto it = pc_hooks_.find(pc);
    if (it != pc_hooks_.end()) it->second.first(it->second.second, *this);
  }
};

// Disassemble the instruction at `pc`. `fetch16` returns the halfword at an
// address. Writes the instruction length in bytes (2, 4, 6 or 8) to *len.
std::string disassemble(u32 pc, const std::function<u16(u32)>& fetch16, unsigned* len);

// Canonical register name ("r0", "gp", "sp", "blink", "lp_count", ...).
const char* reg_name(unsigned r);

}  // namespace leap::arc
