#pragma once

#include <cstdint>
#include <cstddef>

namespace leap {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Sign-extend the low `bits` bits of `v`.
constexpr u32 sext(u32 v, unsigned bits) {
  const u32 m = 1u << (bits - 1);
  v &= (bits == 32) ? 0xffffffffu : ((1u << bits) - 1);
  return (v ^ m) - m;
}

// Extract `len` bits of `v` starting at bit `lo`.
constexpr u32 bits(u32 v, unsigned lo, unsigned len) {
  return (v >> lo) & ((len >= 32) ? 0xffffffffu : ((1u << len) - 1));
}

constexpr bool bit(u32 v, unsigned n) { return (v >> n) & 1; }

}  // namespace leap
