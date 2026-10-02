#include "core/soundtrack.h"

#include "core/machine.h"
#include "core/movie.h"

namespace leap {

namespace {

constexpr u32 kData = 0x3ffe'0000;  // our data in the machine's RAM: handle, descriptor, output
constexpr u32 kGpRegistry = 0x3c00'0104;  // [gp + 4] at cartridge entry: the registry (services.md)
constexpr u32 kCodecTable = 12 + 12 * 35 + 4;  // registry entry 35, table B

}  // namespace

SoundtrackDecoder::SoundtrackDecoder() = default;
SoundtrackDecoder::~SoundtrackDecoder() = default;

bool SoundtrackDecoder::open(const std::vector<u8>& bios, const std::vector<u8>& cart, std::string* err) {
  m_ = std::make_unique<Machine>();
  Machine& m = *m_;
  if (!m.load_bios_image(bios, err) || !m.load_cart_image(cart, "cartridge", err)) return false;
  m.allow_unsigned = true;  // (a damaged dump decodes just the same)
  m.reset();
  // The cartridge registers its codec early (by frame 30 in Schoolhouse Rock).
  for (int frame = 0; frame < 1200; frame++) {
    InputFrame in;
    apply_input(m, in);
    m.run_frame();
    const u32 registry = m.bus().peek32(kGpRegistry);
    if (!registry) continue;
    const u32 table = m.bus().peek32(registry + kCodecTable);
    if (table < 0x8000'0000u) continue;
    decode_fn_ = m.bus().peek32(table);
    init_fn_ = m.bus().peek32(table + 4);
    close_fn_ = m.bus().peek32(table + 8);
    if (decode_fn_ >= 0x8000'0000u && init_fn_ >= 0x8000'0000u && close_fn_ >= 0x8000'0000u) return true;
  }
  *err = "this cartridge has no soundtrack codec";
  m_.reset();
  return false;
}

bool SoundtrackDecoder::decode(u32 addr, std::vector<s16>* pcm, std::string* err) {
  pcm->clear();
  if (!m_) { *err = "no cartridge"; return false; }
  Machine& m = *m_;
  Bus& b = m.bus();
  const u32 codec = b.peek32(addr), len = b.peek32(addr + 4);
  if (codec != 2 || len > 64u << 20) { *err = "unknown soundtrack codec " + std::to_string(codec); return false; }
  for (u32 i = 0; i < 0x40; i += 4) b.write32(kData + i, 0);
  u32 r = 0;
  if (!m.call_guest(init_fn_, {kData}, &r) || r != 0) { *err = "the codec did not start"; return false; }
  const u32 ctx = b.peek32(kData);
  pcm->reserve(size_t(len / 64) * 256);
  for (u32 off = 0; off + 64 <= len; off += 64) {
    // The descriptor, as the game's player fills it: output, sample count,
    // input, input length.
    for (u32 i = 0; i < 0x20; i += 4) b.write32(kData + 0x20 + i, 0);
    b.write32(kData + 0x20, kData + 0x100);
    b.write16(kData + 0x24, 0x100);
    b.write32(kData + 0x28, addr + 8 + off);
    b.write16(kData + 0x2c, 0x40);
    if (!m.call_guest(decode_fn_, {ctx, kData + 0x20}, &r) || r != 0) {
      *err = "the codec stopped at byte " + std::to_string(off);
      m.call_guest(close_fn_, {kData}, &r);
      return false;
    }
    for (u32 i = 0; i < 256; i++) pcm->push_back(s16(b.peek16(kData + 0x100 + 2 * i)));
  }
  m.call_guest(close_fn_, {kData}, &r);  // (frees the instance)
  return true;
}

}  // namespace leap
