#pragma once

// Rewind history: machine states taken every few frames, most recent first
// back. Only the newest state is kept whole; each older one is stored as its
// difference from the next (XOR, run-length coded), which is small because
// most of memory does not change between snapshots. The oldest history is
// dropped beyond a memory budget.

#include <deque>
#include <vector>

#include "core/common.h"

namespace leap {

class Machine;

class Rewind {
 public:
  int interval = 6;                  // frames between snapshots
  size_t budget = 256u << 20;        // bytes kept, at most

  void clear();
  // After each emulated frame: takes a snapshot every `interval` frames.
  void frame(Machine& m);
  // Takes a snapshot now (after loading a state: the new timeline's first).
  void mark(Machine& m) { count_ = interval - 1; frame(m); }
  // Restores the most recent snapshot and removes it from the history (so
  // repeated calls step further back). False when there is none.
  bool step_back(Machine& m);
  // The latest snapshot at or before emulated frame `frame`, if any (*at: its
  // frame number).
  bool find(u64 frame, u64* at) const;
  // Restores that snapshot, dropping the newer ones; it stays in the history
  // (as the newest), so emulation can go on from it. False if there is none.
  bool restore(u64 frame, Machine& m);
  size_t snapshots() const { return newest_.empty() ? 0 : back_.size() + 1; }
  size_t bytes() const { return bytes_ + newest_.size(); }
  // Seconds of history (at 60 frames per second). (Snapshots need not be
  // evenly spaced: the GUI takes them by wall time during fast forward.)
  double seconds() const {
    if (newest_.empty()) return 0;
    const u64 oldest = back_frames_.empty() ? newest_frame_ : back_frames_.front();
    return double(newest_frame_ - oldest + u64(interval)) / 60.0;
  }

 private:
  std::vector<u8> newest_;              // the most recent state, whole
  std::deque<std::vector<u8>> back_;    // back_[i]: from state i+1 (newer) to state i
  u64 newest_frame_ = 0;                // frame numbers of newest_ and of each older state
  std::deque<u64> back_frames_;
  size_t bytes_ = 0;                    // sum of back_ sizes
  int count_ = 0;                       // frames since the last snapshot
};

}  // namespace leap
