#pragma once

// Dynamic recompiler for the ARCompact core (Cpu::Backend::Jit).
//
// Straight-line runs of ROM code ("blocks", up to a branch) are translated to
// host code once and run as one call. The translation keeps the cached
// interpreter's results exactly, including the cycle count and when it is
// observed:
//   - the guest registers stay in Cpu::r_ (host registers are scratch), so
//     any instruction can fall back to the interpreter's own handler;
//   - a block's instruction costs are added up when it is compiled, and its
//     data-access wait states as they happen; the total is charged when the
//     block exits. Before anything that can see the time (an I/O access, an
//     interpreter fallback) the cost so far is charged first, exactly as the
//     interpreter would have by then;
//   - a block runs only when the whole of it fits in the time slice, so the
//     slice ends on the same instruction as in the interpreter (otherwise the
//     interpreter runs those instructions);
//   - blocks end where the interpreter's loop would look at the state between
//     instructions: the idle-loop head, and the end of a zero-overhead loop.
// The JIT is used only with the flat timing model (no cache model), and only
// for ROM, whose code never changes.
//
// Hosts: x86-64 (jit_x64.cpp; System V and Windows calling conventions) and
// AArch64 (jit_a64.cpp; Linux, macOS, Windows).

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/common.h"

namespace leap {
class Bus;
}

namespace leap::arc {

class Cpu;
struct DecodedOp;

// What a decoded instruction does, as far as the JIT is concerned (see
// jit_classify() in decode.cpp, which knows the interpreter's handlers).
struct JitOp {
  enum Kind : u8 {
    Other,   // no native translation: call the handler
    Nop,
    Alu,     // dst = s1 <op> s2 (flags if `f`)
    Unary,   // dst = <op>(s2)
    Mov,     // dst = s2 (flags if `f`)
    Test,    // flags of s1 <op> s2: 0 tst, 1 cmp, 2 btst, 3 rcmp
    Load,    // dst = [s1 + s2]; s1 < 0: absolute
    Store,   // [s1 + s2] = val
    Push,    // sp -= 4; [sp] = val
    Pop,     // dst = [sp]; sp += 4
    Branch,  // if cond: jump to target (link, delay)
    BranchZ, // if (s1 == 0) == (op == 0): jump to target
    Jump,    // jump to register s1 (link, delay)
    BranchCmp,  // if s1 <cc> s2: jump to target (delay)
    Mul,        // MLO = MMID = low, MHI = high word of s1 * s2 (op 0 signed, 1 unsigned)
    Generic,    // the interpreter's reference path (anything)
  };
  // ALU operations, the same order as decode.cpp's.
  enum AluOp : u8 {
    kAdd, kSub, kAnd, kOr, kBic, kXor, kRsub, kBset, kBclr, kBmsk,
    kAdd1, kAdd2, kAdd3, kSub1, kSub2, kSub3, kAsl, kLsr, kAsr,
    kAdc, kSbc, kBxor,  // (only from jit_classify's own decoding)
  };
  // Unary: the 16-bit forms (no flags), and the 32-bit single-operand group.
  enum UnaryOp : u8 { kSexb, kSexw, kExtb, kExtw, kNot, kNeg, kAbs, kAsl1, kAsr1, kLsr1, kRor1, kRrc, kRlc };

  Kind kind = Other;
  u8 op = 0;          // AluOp / UnaryOp / Test kind / BranchCmp condition
  bool f = false;     // sets flags
  bool imm = false;   // s2 is the immediate `k`
  u8 size = 4;        // Load / Store: bytes
  bool sext = false;  // Load: sign-extend
  int dst = -1, s1 = -1, s2 = -1, val = -1;  // registers (-1: none)
  u32 k = 0;          // immediate / address offset / branch target
  u32 k2 = 0;         // BranchCmp: the immediate compared with (if `imm`)
  u8 cc = 0;          // Branch: condition code (0 = always)
  bool link = false, delay = false;
  u8 xcc = 0;         // Alu / Test / Mov / Jump: runs only if this condition holds (0 = always)
  u8 aa = 0;          // Load / Store address mode: 0 [s1+s2], 1 same with s1 written back
                      // first, 2 [s1] then s1 += s2 (s2 already scaled for mode 3)
  u8 scale = 0;       // Load with a register offset: s2 << scale (mode 3)
};
JitOp jit_classify(const DecodedOp& o);

namespace jit_detail {
struct Insn;
}

class Jit {
 public:
  // Whether this host can run the JIT.
  static bool supported();

  Jit(Cpu& cpu, Bus& bus);
  ~Jit();
  Jit(const Jit&) = delete;
  Jit& operator=(const Jit&) = delete;

  struct Block {
    const u8* code = nullptr;  // entered through run(), or from a linked block
    u32 start = 0;
    u32 last = 0;    // address of the last instruction
    u32 guard8 = 0;  // most 1/8 cycles the block can take before its last instruction starts
    u32 count = 0;   // instructions
    bool variant = false;  // stops early at a zero-overhead loop's end
  };
  // The compiled block starting at `pc`, compiling it if needed; nullptr if
  // the code there cannot be compiled (not ROM). With `stop`, a variant that
  // ends before that address (a loop end the normal block runs through).
  Block* block(u32 pc, u32 stop = ~0u);
  // Runs `b` and the blocks linked from it, until one leaves for the
  // dispatcher. 0: `b` did not start, as the time slice ends within it; 2:
  // `b` did not start, as a zero-overhead loop ends inside it; 1: ran.
  // Each block checks this on entry, so blocks can jump straight to the next.
  u32 run(const Block* b) { return enter_(&cpu_, b->code); }
  // A block's exit whose destination was not compiled yet returns to the
  // dispatcher; link() then points that exit at `to`, the block there.
  void link(const Block* to);
  void forget_link() { link_slot_ = nullptr; }
  // Forget all compiled code (the decoded instructions, hooks or timing changed).
  void flush();

  // Statistics.
  size_t blocks() const { return blocks_.size(); }
  size_t code_bytes() const { return used_; }
  struct Stats {
    u64 runs = 0, interpreted = 0, guard_slice = 0, guard_loop = 0, flushes = 0, compiles = 0, delay_slots = 0, no_block = 0;
    std::unordered_map<std::string, u64> generic;  // (LEAPEMU_JIT_STATS=2) by mnemonic
  };
  Stats stats;

 private:
  Block* compile(u32 pc, u32 stop);
  // The code generator (jit_x64.cpp, jit_a64.cpp): the trampoline (enter_, leave_), and a
  // block's host code, with the places to write absolute addresses into once
  // the code's place is known.
  struct Emitted {
    std::vector<u8> code;
    struct Reloc { int at, target; };  // write the address of code offset `target` at `at`
    std::vector<Reloc> relocs;
  };
  bool emit(u32 start, const std::vector<jit_detail::Insn>& insns, u32 guard, Emitted* out);
  void emit_trampoline();
  u8* reserve(size_t n);                        // arena space for n bytes (may flush)
  void write(u8* at, const u8* data, size_t n);  // into the arena (W^X aware)
  // I/O accesses from compiled code (the bus's slow path).
  static u32 slow_read(Bus* bus, u32 a, u32 size);
  static void slow_write(Bus* bus, u32 a, u32 v, u32 size);

  Cpu& cpu_;
  Bus& bus_;
  u8* arena_ = nullptr;
  size_t arena_size_ = 0, used_ = 0;
  bool rwx_ = true;  // the arena is writable and executable at once
  std::unordered_map<u32, std::unique_ptr<Block>> blocks_;
  std::unordered_map<u64, std::unique_ptr<Block>> variants_;  // by stop << 32 | start
  using Enter = u32 (*)(Cpu*, const u8*);
  Enter enter_ = nullptr;      // saves host registers, jumps into a block
  const u8* leave_ = nullptr;  // restores them and returns (eax: run()'s result)
  u8* link_slot_ = nullptr;    // set by the exit that last returned unlinked
  bool chain_ = true;          // link blocks (LEAPEMU_JIT_NOCHAIN=1: never, for debugging)
  struct Recent { u32 pc = ~0u; Block* block = nullptr; };
  std::vector<Recent> recent_;  // direct-mapped front cache of blocks_
  // Conditions the compiled code assumed.
  std::unordered_set<u32> idle_pcs_;  // every idle-loop hint seen (blocks end before them)
  u32 last_idle_hint_ = ~0u;          // (the hint idle_pcs_ was last checked for)
  const void* pages_ = nullptr;
  u32 max_wait8_ = 0;
};

}  // namespace leap::arc
