#pragma once

#include <algorithm>
#include <vector>

#include "core/common.h"

namespace leap::arc {

// Timing-only model of an ARC cache: tags, LRU and dirty bits, no data (the
// emulator always reads and writes memory directly, so coherence never
// matters). Set-associative with LRU replacement; write-back, write-allocate.
class CacheModel {
 public:
  struct Config {
    u32 size_kb = 0;     // 0 = no cache
    u32 line_bytes = 32;
    u32 ways = 2;
    bool operator==(const Config&) const = default;
  };

  void configure(const Config& c) {
    cfg_ = c;
    if (!enabled()) { tags_.clear(); dirty_.clear(); age_.clear(); return; }
    shift_ = 0;
    while ((1u << shift_) < c.line_bytes) shift_++;
    sets_ = c.size_kb * 1024 / c.line_bytes / c.ways;
    tags_.assign(size_t(sets_) * c.ways, 0);
    dirty_.assign(tags_.size(), 0);
    age_.assign(tags_.size(), 0);
  }
  const Config& config() const { return cfg_; }
  bool enabled() const { return cfg_.size_kb != 0; }
  u32 line_shift() const { return shift_; }
  u32 line_bytes() const { return cfg_.line_bytes; }

  void invalidate() {
    std::fill(tags_.begin(), tags_.end(), 0u);
    std::fill(dirty_.begin(), dirty_.end(), u8(0));
  }
  // Number of dirty lines (written back by a flush), and clear them.
  u32 flush() {
    u32 n = 0;
    for (auto& d : dirty_) { n += d; d = 0; }
    return n;
  }

  // Access the line holding `addr`. Returns true on a hit. On a miss the line
  // is allocated; *wrote_back is set if a dirty line was evicted.
  bool access(u32 addr, bool write, bool* wrote_back) {
    const u32 line = addr >> shift_;
    const u32 tag = line + 1;  // 0 marks an invalid way
    const size_t base = size_t(line % sets_) * cfg_.ways;
    ++clock_;
    for (u32 w = 0; w < cfg_.ways; w++) {
      if (tags_[base + w] == tag) {
        age_[base + w] = clock_;
        if (write) dirty_[base + w] = 1;
        return true;
      }
    }
    // Miss: replace the least recently used way (an invalid way first).
    size_t victim = base;
    for (u32 w = 1; w < cfg_.ways; w++) {
      const size_t i = base + w;
      if (tags_[victim] == 0) break;
      if (tags_[i] == 0 || clock_ - age_[i] > clock_ - age_[victim]) victim = i;
    }
    *wrote_back = tags_[victim] != 0 && dirty_[victim];
    tags_[victim] = tag;
    dirty_[victim] = write;
    age_[victim] = clock_;
    return false;
  }

  template <class Ar>
  void serialize(Ar& ar) {
    ar.io(tags_); ar.io(dirty_); ar.io(age_); ar.io(clock_);
    if constexpr (Ar::kLoading) {
      const size_t n = enabled() ? size_t(sets_) * cfg_.ways : 0;
      if (tags_.size() != n || dirty_.size() != n || age_.size() != n) configure(cfg_);  // other geometry: start cold
    }
  }

 private:
  Config cfg_;
  u32 shift_ = 5, sets_ = 1;
  u32 clock_ = 0;
  std::vector<u32> tags_;
  std::vector<u8> dirty_;
  std::vector<u32> age_;
};

}  // namespace leap::arc
