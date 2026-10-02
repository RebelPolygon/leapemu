#pragma once

// Shared by the JIT's front end (jit.cpp) and its code generators
// (jit_x64.cpp, jit_a64.cpp).

#include <cstddef>

#include "core/arc/cpu.h"
#include "core/arc/jit.h"

#if !defined(LEAPEMU_ANALYSIS) && (defined(__x86_64__) || defined(_M_X64))
#define LEAP_JIT_X64 1
#elif !defined(LEAPEMU_ANALYSIS) && (defined(__aarch64__) || defined(_M_ARM64))
#define LEAP_JIT_A64 1
#endif
#if defined(LEAP_JIT_X64) || defined(LEAP_JIT_A64)
#define LEAP_JIT 1
#endif

namespace leap::arc::jit_detail {

constexpr size_t kRecent = 1u << 14;  // entries of Jit::recent_
constexpr int kMaxInsns = 64;         // instructions per block, at most

// Byte offset of a member of `base`.
template <class T, class M>
inline s32 off(const T& base, const M& member) {
  return s32(reinterpret_cast<const char*>(&member) - reinterpret_cast<const char*>(&base));
}

struct Insn {
  u32 pc = 0;
  const DecodedOp* o = nullptr;
  JitOp j;
  u32 len = 0;     // true length (a fallback can find a long immediate at run time)
  u32 cost8 = 0;   // the interpreter's charge before data waits
  bool hook = false;
  const Insn* slot = nullptr;  // a .d branch's delay slot, compiled into its taken path
};

// A delay-slot instruction that can run inline after a taken branch.
inline bool slot_ok(JitOp::Kind k) {
  return k == JitOp::Nop || k == JitOp::Alu || k == JitOp::Unary || k == JitOp::Mov || k == JitOp::Test ||
         k == JitOp::Load || k == JitOp::Store || k == JitOp::Push || k == JitOp::Pop;
}

constexpr u32 kSlotExit = 0x80000000u;  // Cpu::jit_exit: the instruction was a delay slot

inline bool is_branch(JitOp::Kind k) {
  return k == JitOp::Branch || k == JitOp::BranchZ || k == JitOp::Jump || k == JitOp::BranchCmp;
}

}  // namespace leap::arc::jit_detail
