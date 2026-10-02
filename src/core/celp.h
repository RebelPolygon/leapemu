#pragma once

// LeapFrog "LFC" speech decoder: the Leapster / LeapPad sound chip's voice 7.
//
// LFC is a small CELP-style codec: a 10th-order lattice (reflection
// coefficient) synthesis filter excited either by a few signed pulses or by a
// row of a fixed noise table, 32 samples per sub-frame, 6 sub-frames per
// frame, 8 kHz output. All quantisation tables live in a 20224-byte (0x4f00)
// "codebook" that is identical in every Leapster and LeapPad BaseROM (it sits
// at BaseROM offset 0x900). See docs/speech.md for the bitstream format and for
// what is confirmed and what is not.
//
// The decoder is self-contained: it knows nothing about Machine. The caller
// provides the codebook once, starts a stream at an address, and pulls one
// sample at a time, supplying a byte reader for the stream memory.

#include <array>
#include <cstddef>
#include <functional>

#include "core/common.h"

namespace leap {

class CelpDecoder {
 public:
  static constexpr size_t kCodebookSize = 0x4f00;
  // The BaseROM stores the codebook at this offset inside a 32 KiB-aligned
  // page; the chip's codebook register only holds the page number (see
  // docs/speech.md), so the hardware apparently adds this offset itself.
  static constexpr u32 kCodebookPageOffset = 0x900;
  static constexpr unsigned kSampleRate = 8000;

  // Parse the codebook (little-endian 16-bit tables). `len` must be at least
  // kCodebookSize; otherwise the decoder stays silent.
  void set_codebook(const u8* cb, size_t len);
  bool has_codebook() const { return has_codebook_; }

  // Begin decoding the LFC stream at `addr` (the voice 7 start address, i.e.
  // the stream's leading pulse-mode halfword). Resets the filter state.
  void start(u32 addr);
  // Stop immediately (voice stop command).
  void stop() { state_ = State::Idle; }
  // True while samples remain, including the filter ring-down at the end.
  bool active() const { return state_ != State::Idle; }
  // Current stream read address (for debugging).
  u32 position() const { return pos_; }

  // Produce the next 8 kHz sample. Returns 0 when inactive. `read8` reads one
  // byte of the stream memory (cartridge ROM, normally).
  s16 next_sample(const std::function<u8(u32)>& read8);

  // Number of samples of zero-input ring-down played after the end marker.
  static constexpr unsigned kRingDown = 160;
  // Safety limit: stop a stream that runs this far without an end marker.
  static constexpr u32 kMaxStreamBytes = 1u << 20;

  template <class Ar>
  void serialize(Ar& ar) {
    ar.io(shape0_); ar.io(shape1_); ar.io(shape2_); ar.io(gain_); ar.io(gradient_);
    ar.io(k_table_); ar.io(noise_); ar.io(has_codebook_);
    ar.io(state_); ar.io(pos_); ar.io(start_); ar.io(mode_); ar.io(cur_mask_); ar.io(next_mask_);
    ar.io(next_filter_); ar.io(next_has_filter_); ar.io(record_); ar.io(sample_);
    ar.io(ringdown_left_); ar.io(k_); ar.io(b_); ar.io(buf_);
  }

 private:
  enum class State : u8 { Idle, Start, Frame, RingDown };

  u16 read16(const std::function<u8(u32)>& read8);
  void read_mask_and_filter(const std::function<u8(u32)>& read8);
  bool begin_frame(const std::function<u8(u32)>& read8);
  void decode_record(const std::function<u8(u32)>& read8);
  s16 filter(s32 x);

  // Codebook tables (Q15).
  std::array<s16, 64 * 3> shape0_{};  // 4-pulse mode: ratios of pulses 2..4
  std::array<s16, 64 * 5> shape1_{};  // 6-pulse mode
  std::array<s16, 64 * 7> shape2_{};  // 8-pulse mode
  std::array<s16, 3 * 32> gain_{};    // pulse gain per mode
  std::array<s16, 32> gradient_{};    // noise gain
  std::array<s16, 112> k_table_{};    // reflection coefficient quantisers
  std::array<s16, 256 * 32> noise_{}; // noise excitation rows
  bool has_codebook_ = false;

  // Stream state.
  State state_ = State::Idle;
  u32 pos_ = 0;
  u32 start_ = 0;
  u16 mode_ = 0;          // 0/1/2 = 4/6/8 pulses per pulsed sub-frame
  u16 cur_mask_ = 0;      // sub-frame type mask of the frame being played
  u16 next_mask_ = 0;     // mask of the following frame (read ahead)
  std::array<u8, 10> next_filter_{};
  bool next_has_filter_ = false;
  u8 record_ = 0;         // sub-frame index within the frame (0..5)
  u8 sample_ = 32;        // index into buf_
  u16 ringdown_left_ = 0;
  std::array<s16, 10> k_{};
  std::array<s16, 11> b_{};
  std::array<s16, 32> buf_{};
};

}  // namespace leap
