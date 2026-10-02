#include "core/celp.h"

#include <algorithm>

// LFC bitstream (all fields little-endian 16-bit words; bit fields are taken
// LSB first within one word):
//
//   u16 mode                       0/1/2: 4/6/8 pulses per pulsed sub-frame
//   u16 mask[0]; filter[0]         (filter present only if mask has a present bit)
//   for each frame i:
//     u16 mask[i+1]; filter[i+1]   read-ahead of the next frame's header
//     sub-frame records of frame i (6 x 32 samples, in time order)
//   ...until a mask with no present bits and all six pulsed bits (0x0fc0).
//
// mask: bit 5-j = sub-frame j present, bit 11-j = sub-frame j pulsed (else noise).
// filter: word 0 = k0:5 k1:4 k2:4 k3:3, word 1 = k4:3 k5:3 k6:3 k7:3 k8:2 k9:2.
// noise record (1 word): row:8 gain:5 sign:1
// pulse record: gain:5 shape:6 then positions/signs (see decode_record).
//
// Arithmetic is Q15 with round-half-up multiplies and 16-bit saturation. The
// format was worked out by Nathan Farlow (github.com/nathanfarlow/
// leapfrog-voice-decoder); the tables here are read from the BaseROM codebook.

namespace leap {

namespace {

// Codebook layout (byte offsets from the codebook start, BaseROM 0x900).
constexpr size_t kShape0 = 0x000;    // 64 x 3
constexpr size_t kShape1 = 0x180;    // 64 x 5
constexpr size_t kShape2 = 0x400;    // 64 x 7
constexpr size_t kGain = 0x780;      // 3 x 32
constexpr size_t kGradient = 0x840;  // 32
constexpr size_t kK = 0x880;         // 32+16+16+8+8+8+8+8+4+4 = 112
constexpr size_t kNoise = 0xf00;     // 256 x 32
// 0x960..0xeff (720 halfwords) is not used by this decoder; see docs/speech.md.

constexpr unsigned kKBits[10] = {5, 4, 4, 3, 3, 3, 3, 3, 2, 2};
constexpr unsigned kKOffset[10] = {0, 32, 48, 64, 72, 80, 88, 96, 104, 108};

constexpr u16 kEndMask = 0x0fc0;

inline s32 q15_mul(s32 a, s32 b) { return (a * b + 0x4000) >> 15; }  // round half up
inline s32 sat16(s32 x) { return std::clamp(x, -32768, 32767); }

inline bool mask_has_present(u16 m) { return (m & 0x3f) != 0; }
inline bool mask_is_end(u16 m) { return (m & 0x3f) == 0 && (m & 0xfc0) == 0xfc0; }

template <size_t N>
void load(std::array<s16, N>& dst, const u8* src) {
  for (size_t i = 0; i < N; i++) dst[i] = s16(u16(src[2 * i] | (src[2 * i + 1] << 8)));
}

// Pulls bit fields LSB-first out of one 16-bit word.
struct Bits {
  u16 v;
  unsigned take(unsigned n) {
    const unsigned r = v & ((1u << n) - 1);
    v = u16(v >> n);
    return r;
  }
};

}  // namespace

void CelpDecoder::set_codebook(const u8* cb, size_t len) {
  if (!cb || len < kCodebookSize) {
    has_codebook_ = false;
    return;
  }
  load(shape0_, cb + kShape0);
  load(shape1_, cb + kShape1);
  load(shape2_, cb + kShape2);
  load(gain_, cb + kGain);
  load(gradient_, cb + kGradient);
  load(k_table_, cb + kK);
  load(noise_, cb + kNoise);
  has_codebook_ = true;
}

void CelpDecoder::start(u32 addr) {
  start_ = pos_ = addr;
  k_.fill(0);
  b_.fill(0);
  buf_.fill(0);
  sample_ = 32;
  record_ = 0;
  state_ = has_codebook_ ? State::Start : State::Idle;
}

u16 CelpDecoder::read16(const std::function<u8(u32)>& read8) {
  const u16 v = u16(read8(pos_) | (read8(pos_ + 1) << 8));
  pos_ += 2;
  return v;
}

void CelpDecoder::read_mask_and_filter(const std::function<u8(u32)>& read8) {
  next_mask_ = read16(read8);
  next_has_filter_ = mask_has_present(next_mask_);
  if (next_has_filter_) {
    Bits w0{read16(read8)};
    for (int i = 0; i < 4; i++) next_filter_[i] = u8(w0.take(kKBits[i]));
    Bits w1{read16(read8)};
    for (int i = 4; i < 10; i++) next_filter_[i] = u8(w1.take(kKBits[i]));
  }
}

// Make the read-ahead header current and read the following one. Returns
// false (and enters the ring-down) at the end marker.
bool CelpDecoder::begin_frame(const std::function<u8(u32)>& read8) {
  cur_mask_ = next_mask_;
  if (mask_is_end(cur_mask_) || pos_ - start_ > kMaxStreamBytes) {
    state_ = State::RingDown;
    ringdown_left_ = kRingDown;
    return false;
  }
  if (next_has_filter_)
    for (int i = 0; i < 10; i++) k_[i] = k_table_[kKOffset[i] + next_filter_[i]];
  read_mask_and_filter(read8);
  record_ = 0;
  return true;
}

void CelpDecoder::decode_record(const std::function<u8(u32)>& read8) {
  buf_.fill(0);
  const unsigned j = record_;
  const bool present = bit(cur_mask_, 5 - j);
  const bool pulsed = bit(cur_mask_, 11 - j);
  if (!present) return;

  if (!pulsed) {
    Bits w{read16(read8)};
    const unsigned row = w.take(8), g = w.take(5), sign = w.take(1);
    const s32 loud = sign ? -gradient_[g] : gradient_[g];
    const s16* n = &noise_[row * 32];
    for (int i = 0; i < 32; i++) buf_[i] = s16(q15_mul(loud, n[i]));
    return;
  }

  Bits w{read16(read8)};
  const unsigned g = w.take(5), shape = w.take(6);
  unsigned pos[8], sgn[8], count;
  if (mode_ == 0) {
    w.take(1);
    pos[0] = w.take(4);
    Bits w1{read16(read8)};
    for (int i = 0; i < 4; i++) sgn[i] = w1.take(1);
    for (int i = 1; i < 4; i++) pos[i] = w1.take(4);
    for (int i = 0; i < 4; i++) pos[i] *= 2;
    count = 4;
  } else {
    pos[0] = w.take(5);
    Bits w1{read16(read8)};
    for (int i = 0; i < 6; i++) sgn[i] = w1.take(1);
    pos[1] = w1.take(5);
    pos[2] = w1.take(5);
    Bits w2{read16(read8)};
    pos[3] = w2.take(5);
    pos[4] = w2.take(5);
    pos[5] = w2.take(5);
    count = 6;
    if (mode_ == 2) {
      sgn[6] = w2.take(1);
      Bits w3{read16(read8)};
      pos[6] = w3.take(5);
      pos[7] = w3.take(5);
      w3.take(5);
      sgn[7] = w3.take(1);
      count = 8;
    }
  }

  const s16* ratios = mode_ == 0 ? &shape0_[shape * 3]
                    : mode_ == 1 ? &shape1_[shape * 5]
                                 : &shape2_[shape * 7];
  const s32 loudest = gain_[mode_ * 32 + g];
  for (unsigned i = 0; i < count; i++) {
    const s32 amp = i == 0 ? loudest : q15_mul(loudest, ratios[i - 1]);
    buf_[pos[i]] = s16(sgn[i] ? -amp : amp);  // later pulses overwrite earlier ones
  }
}

// 10-stage lattice synthesis filter.
s16 CelpDecoder::filter(s32 x) {
  s32 f = x;
  for (int j = 9; j >= 0; j--) {
    f = sat16(f + q15_mul(k_[j], b_[j]));
    b_[j + 1] = s16(sat16(b_[j] + q15_mul(-k_[j], f)));
  }
  b_[0] = s16(f);
  return s16(f);
}

s16 CelpDecoder::next_sample(const std::function<u8(u32)>& read8) {
  switch (state_) {
    case State::Idle:
      return 0;
    case State::Start:
      mode_ = read16(read8);
      if (mode_ > 2) {
        state_ = State::Idle;
        return 0;
      }
      read_mask_and_filter(read8);
      state_ = State::Frame;
      if (!begin_frame(read8)) return next_sample(read8);
      break;
    case State::RingDown:
      if (ringdown_left_ == 0) {
        state_ = State::Idle;
        return 0;
      }
      ringdown_left_--;
      return filter(0);
    case State::Frame:
      break;
  }

  if (sample_ >= 32) {
    if (record_ >= 6 && !begin_frame(read8)) return next_sample(read8);
    decode_record(read8);
    record_++;
    sample_ = 0;
  }
  return filter(buf_[sample_++]);
}

}  // namespace leap
