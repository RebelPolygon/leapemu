// Recognising a run of a ROM's compiled code by its length and a hash of its
// bytes, rather than by the bytes themselves, so that no LeapFrog code is
// copied into leapemu. tools/debug/code_sig prints the values for a run.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <vector>

#include "core/common.h"

namespace leap {

struct CodeSig {
  u32 len;
  u64 hash;  // code_hash() of the run
};

// Polynomial hash (modulo 2^61 - 1) of n bytes.
u64 code_hash(const u8* p, size_t n);

// For each of `sigs`, the offsets in `img` where it matches, at multiples of
// `align`, in order and at most `max` of them; in one pass over `img`, which
// ends once every signature has `max` matches.
std::vector<std::vector<size_t>> find_code(const std::vector<u8>& img, std::initializer_list<CodeSig> sigs,
                                           size_t align = 1, size_t max = 1);

// A second, independent hash of several runs of `img` (offset from `at`,
// length) taken together, to confirm that a routine found by a CodeSig is the
// known version of it. 0 if a run is out of range.
struct CodeRun {
  u32 off, len;
};
u64 code_check(const std::vector<u8>& img, size_t at, const std::vector<CodeRun>& runs);

}  // namespace leap
