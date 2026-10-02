#include "core/rewind.h"

#include <cstring>
#include <string>

#include "core/machine.h"

namespace leap {

namespace {

// The difference from `to` to `from`: the target length, then runs of
// (u32 zero-byte count, u32 literal count, literal bytes) of to XOR from, with
// both padded with zeros to the longer length.
std::vector<u8> encode(const std::vector<u8>& from, const std::vector<u8>& to) {
  std::vector<u8> out;
  auto put32 = [&](u32 v) { for (int i = 0; i < 4; i++) out.push_back(u8(v >> (8 * i))); };
  const size_t n = std::max(from.size(), to.size());
  put32(u32(to.size()));
  auto x = [&](size_t i) { return u8((i < from.size() ? from[i] : 0) ^ (i < to.size() ? to[i] : 0)); };
  size_t i = 0;
  while (i < n) {
    size_t z = i;
    while (z < n && x(z) == 0) z++;
    size_t l = z;
    // A literal run ends at 8 zero bytes in a row (cheaper as a zero run).
    size_t zeros = 0;
    while (l < n && zeros < 8) {
      zeros = x(l) == 0 ? zeros + 1 : 0;
      l++;
    }
    if (zeros == 8) l -= 8;
    put32(u32(z - i));
    put32(u32(l - z));
    for (size_t k = z; k < l; k++) out.push_back(x(k));
    i = l;
  }
  return out;
}

// Applies a difference to `state` (turning it into the state it was made to).
bool apply_diff(std::vector<u8>& state, const std::vector<u8>& diff) {
  size_t p = 0;
  auto get32 = [&](u32& v) {
    if (p + 4 > diff.size()) return false;
    v = u32(diff[p]) | u32(diff[p + 1]) << 8 | u32(diff[p + 2]) << 16 | u32(diff[p + 3]) << 24;
    p += 4;
    return true;
  };
  u32 len = 0;
  if (!get32(len)) return false;
  const size_t n = std::max<size_t>(state.size(), len);
  state.resize(n, 0);
  size_t i = 0;
  while (p < diff.size()) {
    u32 z = 0, l = 0;
    if (!get32(z) || !get32(l) || i + z + l > n || p + l > diff.size()) return false;
    i += z;
    for (u32 k = 0; k < l; k++) state[i++] ^= diff[p++];
  }
  state.resize(len);
  return true;
}

}  // namespace

void Rewind::clear() {
  newest_.clear();
  back_.clear();
  back_frames_.clear();
  bytes_ = 0;
  count_ = 0;
}

void Rewind::frame(Machine& m) {
  if (++count_ < interval) return;
  count_ = 0;
  std::vector<u8> state = m.save_state();
  if (!newest_.empty()) {
    back_.push_back(encode(state, newest_));  // from the new state back to the previous one
    back_frames_.push_back(newest_frame_);
    bytes_ += back_.back().size();
    while (bytes() > budget && !back_.empty()) {  // forget the oldest
      bytes_ -= back_.front().size();
      back_.pop_front();
      back_frames_.pop_front();
    }
  }
  newest_ = std::move(state);
  newest_frame_ = m.frame_count();
}

bool Rewind::find(u64 frame, u64* at) const {
  if (newest_.empty()) return false;
  if (newest_frame_ <= frame) { *at = newest_frame_; return true; }
  for (size_t i = back_frames_.size(); i-- > 0;)
    if (back_frames_[i] <= frame) { *at = back_frames_[i]; return true; }
  return false;
}

bool Rewind::restore(u64 frame, Machine& m) {
  u64 at;
  if (!find(frame, &at)) return false;
  while (newest_frame_ > frame) {  // (find() guarantees an older state qualifies)
    if (!apply_diff(newest_, back_.back())) { clear(); return false; }
    bytes_ -= back_.back().size();
    back_.pop_back();
    newest_frame_ = back_frames_.back();
    back_frames_.pop_back();
  }
  std::string err;
  if (!m.load_state(newest_, &err)) { clear(); return false; }
  count_ = 0;
  return true;
}

bool Rewind::step_back(Machine& m) {
  if (newest_.empty()) return false;
  std::string err;
  if (!m.load_state(newest_, &err)) { clear(); return false; }
  count_ = 0;
  if (back_.empty()) {
    newest_.clear();
  } else {
    if (!apply_diff(newest_, back_.back())) { clear(); return true; }
    bytes_ -= back_.back().size();
    back_.pop_back();
    newest_frame_ = back_frames_.back();
    back_frames_.pop_back();
  }
  return true;
}

}  // namespace leap
