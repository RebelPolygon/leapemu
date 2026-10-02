#include "core/syn.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace leap {

namespace {

u16 le16(const u8* p) { return u16(p[0] | (p[1] << 8)); }

constexpr double kPi = 3.14159265358979323846;

}  // namespace

bool parse_syn(const u8* d, size_t size, SynSong* out, std::string* err, unsigned forever_plays) {
  *out = SynSong{};
  if (size < 4 || le16(d) != 2) { *err = "not SYN music"; return false; }
  const unsigned ntracks = le16(d + 2);
  if (4 + 4 * size_t(ntracks) > size) { *err = "SYN: truncated header"; return false; }
  for (unsigned t = 0; t < ntracks; t++) {
    size_t o = le16(d + 4 + 4 * t);
    std::vector<SynNote> notes;
    u32 time = 0;
    u8 volume = 127;
    u16 program = 0;
    size_t loop_at = 0;
    unsigned loop_left = 0;
    bool in_loop = false, ended = false;
    auto byte = [&]() -> int { return o < size ? d[o++] : -1; };
    auto duration = [&]() -> int {
      const int a = byte();
      if (a < 0x80) return a;
      const int b = byte();
      return b < 0 ? -1 : ((a - 0x80) << 8) | b;
    };
    for (unsigned steps = 0; !ended && steps < 1'000'000; steps++) {
      const int c = byte();
      if (c < 0) { *err = "SYN: a track runs past the end"; return false; }
      if (c < 0x80) {
        const int len = duration();
        if (len < 0) { *err = "SYN: truncated note"; return false; }
        if (c) notes.push_back({time, u32(len), u8(c), volume, program});
        time += u32(len);
        continue;
      }
      switch (c) {
        case 0x81: case 0x82: case 0x83: case 0x84: break;
        case 0x85: byte(); break;
        case 0x88: volume = u8(std::min(byte(), 127)); break;
        case 0x89: {
          const int p = byte();
          program = p == 0xc0 ? u16(0x100 | byte()) : u16(p & 0x7f);
          break;
        }
        case 0x8a: byte(); duration(); break;  // (pitch slides are not reproduced)
        case 0x8e: {
          const int n = byte();
          loop_at = o;
          loop_left = n == 127 ? forever_plays - 1 : unsigned(std::clamp(n, 1, 16)) - 1;
          if (n == 127) out->loops_forever = true;
          in_loop = true;
          break;
        }
        case 0x8f:
          byte();
          if (in_loop && loop_left) { loop_left--; o = loop_at; }
          else in_loop = false;
          break;
        case 0xff: ended = true; break;
        default: *err = "SYN: unknown command"; return false;
      }
    }
    out->length = std::max(out->length, time);
    out->tracks.push_back(std::move(notes));
  }
  return true;
}

std::vector<u8> syn_to_midi(const SynSong& song, const std::string& title) {
  std::vector<u8> f;
  auto put32 = [](std::vector<u8>& v, u32 x) { for (int k = 3; k >= 0; k--) v.push_back(u8(x >> (8 * k))); };
  auto put16 = [](std::vector<u8>& v, u16 x) { v.push_back(u8(x >> 8)); v.push_back(u8(x)); };
  auto varlen = [](std::vector<u8>& v, u32 x) {
    u8 b[5];
    int n = 0;
    b[n++] = x & 0x7f;
    while (x >>= 7) b[n++] = u8(0x80 | (x & 0x7f));
    while (n) v.push_back(b[--n]);
  };
  auto meta_text = [&](std::vector<u8>& v, u8 type, const std::string& s) {
    v.push_back(0); v.push_back(0xff); v.push_back(type);
    varlen(v, u32(s.size()));
    v.insert(v.end(), s.begin(), s.end());
  };
  f.insert(f.end(), {'M', 'T', 'h', 'd'});
  put32(f, 6);
  put16(f, 1);
  put16(f, u16(song.tracks.size() + 1));
  put16(f, 125);  // ticks per quarter note: at 120 bpm a tick is 4 ms

  auto chunk = [&](const std::vector<u8>& body) {
    f.insert(f.end(), {'M', 'T', 'r', 'k'});
    put32(f, u32(body.size()));
    f.insert(f.end(), body.begin(), body.end());
  };
  {  // Tempo track.
    std::vector<u8> t;
    if (!title.empty()) meta_text(t, 0x03, title);
    t.insert(t.end(), {0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20});  // 500000 us per quarter
    t.insert(t.end(), {0x00, 0xff, 0x2f, 0x00});
    chunk(t);
  }
  unsigned next_channel = 0;
  for (size_t ti = 0; ti < song.tracks.size(); ti++) {
    const std::vector<SynNote>& notes = song.tracks[ti];
    const bool drums = !notes.empty() && notes.front().program >= 126 && notes.front().program <= 127;
    u8 ch = 9;
    if (!drums) {
      if (next_channel == 9) next_channel++;
      ch = u8(next_channel++ % 16);
      if (ch == 9) ch = 10;
    }
    struct Ev { u32 t; int order; u8 a, b, c; };
    std::vector<Ev> ev;
    int prog = -1;
    for (const SynNote& n : notes) {
      const bool kit = n.program == 126 || n.program == 127;
      const u8 c = kit ? 9 : ch;
      if (!kit && int(n.program) != prog) {
        prog = n.program;
        ev.push_back({n.start, 0, u8(0xc0 | c), u8(n.program < 0x100 ? n.program : 0), 0xff});
      }
      ev.push_back({n.start, 2, u8(0x90 | c), n.pitch, u8(std::max<u8>(n.volume, 1))});
      ev.push_back({n.start + n.length, 1, u8(0x80 | c), n.pitch, 0});
    }
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t != b.t ? a.t < b.t : a.order < b.order; });
    std::vector<u8> t;
    meta_text(t, 0x03, "Track " + std::to_string(ti + 1));
    for (const SynNote& n : notes)
      if (n.program >= 0x100) { meta_text(t, 0x01, "cartridge instrument " + std::to_string(n.program & 0xff)); break; }
    u32 now = 0;
    for (const Ev& e : ev) {
      varlen(t, e.t - now);
      now = e.t;
      t.push_back(e.a);
      t.push_back(e.b);
      if (e.c != 0xff) t.push_back(e.c);
    }
    t.insert(t.end(), {0x00, 0xff, 0x2f, 0x00});
    chunk(t);
  }
  return f;
}

// --- A small synthesizer ------------------------------------------------------

namespace {

struct Rng {
  u32 s = 0x12345678;
  float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return float(s) / 2147483648.0f - 1.0f; }
};

// One note (or drum hit) added into `out` from sample `at`.
void render_note(const SynNote& n, unsigned rate, std::vector<float>& out, Rng& rng) {
  const double f = 440.0 * std::pow(2.0, (int(n.pitch) - 69) / 12.0);
  const double held = n.length * kSynTickSeconds;
  const double amp = 0.18 * (n.volume / 127.0);
  const size_t at = size_t(n.start * kSynTickSeconds * rate);
  auto add = [&](size_t i, double v) { if (at + i < out.size()) out[at + i] += float(v); };
  const unsigned p = n.program;

  if (p == 126 || p == 127) {  // drum kit (General MIDI note map)
    const int k = n.pitch;
    double dur = 0.15;
    if (k == 35 || k == 36) dur = 0.3;
    else if (k == 49 || k == 57 || k == 52 || k == 55) dur = 1.2;
    else if (k == 51 || k == 59 || k == 46) dur = 0.5;
    else if (k == 42 || k == 44) dur = 0.06;
    const size_t len = size_t(dur * rate);
    double phase = 0, hp = 0, prev = 0;
    for (size_t i = 0; i < len; i++) {
      const double t = double(i) / rate, env = std::exp(-t * 5.0 / dur);
      double v;
      if (k == 35 || k == 36) {  // kick: a falling sine
        phase += 2 * kPi * (50 + 120 * std::exp(-t * 30)) / rate;
        v = std::sin(phase);
      } else if ((k >= 41 && k <= 50 && k != 42 && k != 44 && k != 46) || k == 47 || k == 48) {  // toms
        phase += 2 * kPi * (80 + (k - 41) * 18 + 60 * std::exp(-t * 20)) / rate;
        v = std::sin(phase);
      } else if (k == 38 || k == 40 || k == 37 || k == 39) {  // snare, stick, clap
        phase += 2 * kPi * 190 / rate;
        v = 0.5 * std::sin(phase) * std::exp(-t * 40) + 0.7 * rng.next();
      } else {  // cymbals and the rest: high-passed noise
        const double x = rng.next();
        hp = 0.6 * (hp + x - prev);
        prev = x;
        v = hp * 1.2;
      }
      add(i, amp * 1.4 * env * v);
    }
    return;
  }

  // Tonal: pick a voice by General MIDI family.
  enum Voice { Piano, Bell, Organ, Pluck, Strings, Brass, Pulse, Flute, Pad, Noise } voice = Pluck;
  double attack = 0.005, decay = 1.0, sustain = 0.0;  // decay: seconds to fall to 1/e
  if (p >= 0x100) { voice = Pluck; }
  else if (p < 8) { voice = Piano; decay = 1.2; }
  else if (p < 16) { voice = Bell; decay = 0.5; }
  else if (p < 24) { voice = Organ; sustain = 1; }
  else if (p < 40) { voice = Pluck; }
  else if (p < 56) { voice = Strings; attack = 0.06; sustain = 1; }
  else if (p < 64) { voice = Brass; attack = 0.02; sustain = 0.8; decay = 0.3; }
  else if (p < 72) { voice = Pulse; attack = 0.01; sustain = 0.9; decay = 0.3; }
  else if (p < 80) { voice = Flute; attack = 0.03; sustain = 1; }
  else if (p < 88) { voice = Pulse; sustain = 1; }
  else if (p < 104) { voice = Pad; attack = 0.1; sustain = 1; }
  else if (p < 112) { voice = Pluck; }
  else if (p < 120) { voice = Bell; decay = 0.25; }
  else { voice = Noise; decay = 0.3; }

  const double release = 0.06;
  const size_t len = size_t((held + release) * rate);
  const size_t held_n = size_t(held * rate);
  if (voice == Pluck) {  // Karplus-Strong string
    const size_t period = std::max<size_t>(2, size_t(rate / f));
    std::vector<float> ring(period);
    for (float& s : ring) s = rng.next();
    size_t pos = 0;
    const double damp = 0.996;
    for (size_t i = 0; i < len; i++) {
      const size_t nx = (pos + 1) % period;
      const float v = ring[pos];
      ring[pos] = float(damp * 0.5 * (ring[pos] + ring[nx]));
      pos = nx;
      const double rel = i < held_n ? 1.0 : std::max(0.0, 1.0 - double(i - held_n) / (release * rate));
      add(i, amp * 1.3 * v * rel);
    }
    return;
  }
  double ph = 0, ph2 = 0;
  const double inc = f / rate;
  for (size_t i = 0; i < len; i++) {
    const double t = double(i) / rate;
    double env = t < attack ? t / attack : sustain + (1 - sustain) * std::exp(-(t - attack) / decay);
    if (i >= held_n) env *= std::max(0.0, 1.0 - double(i - held_n) / (release * rate));
    ph += inc;
    ph -= std::floor(ph);
    ph2 += inc * 1.004;
    ph2 -= std::floor(ph2);
    const double s = std::sin(2 * kPi * ph), saw = 2 * ph - 1, saw2 = 2 * ph2 - 1;
    double v;
    switch (voice) {
      case Piano: v = s + 0.4 * std::sin(4 * kPi * ph) + 0.15 * std::sin(6 * kPi * ph); break;
      case Bell: v = s + 0.5 * std::sin(2 * kPi * ph * 3.5) * std::exp(-t * 6); break;
      case Organ: v = s + 0.5 * std::sin(4 * kPi * ph) + 0.3 * std::sin(6 * kPi * ph); break;
      case Strings: case Pad: v = 0.5 * (saw + saw2); break;
      case Brass: v = saw * 0.8; break;
      case Pulse: v = ph < 0.5 ? 0.6 : -0.6; break;
      case Flute: v = s + 0.05 * rng.next(); break;
      case Noise: v = rng.next(); break;
      default: v = s;
    }
    add(i, amp * env * v);
  }
}

}  // namespace

std::vector<s16> render_syn(const SynSong& song, unsigned rate) {
  const size_t total = size_t((song.length * kSynTickSeconds + 1.5) * rate);
  std::vector<float> mix(std::min<size_t>(total, size_t(rate) * 600), 0.0f);  // (at most 10 minutes)
  Rng rng;
  for (const auto& track : song.tracks)
    for (const SynNote& n : track) render_note(n, rate, mix, rng);
  std::vector<s16> out(mix.size());
  for (size_t i = 0; i < mix.size(); i++) out[i] = s16(std::tanh(mix[i]) * 30000.0f);  // (soft limit)
  return out;
}

}  // namespace leap
