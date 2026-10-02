#pragma once

// Soundtracks (asset type 0xF, Schoolhouse Rock's songs): streams the game
// plays through the PCM channels (docs/hardware.md), coded with a codec in the
// cartridge's own code. Each asset is u32 codec (2 in every one known), u32
// length in bytes, then 64-byte blocks of 256 samples at 11025 Hz.
//
// There is no decoder of ours: SoundtrackDecoder runs the cartridge's. It
// boots the BaseROM and the cartridge on a machine of its own until the
// cartridge has registered its codec (registry entry 35, table B: decode,
// initialise, close), then calls those functions for each stream. The result is
// exactly what the game decodes when it plays the song.

#include <memory>
#include <string>
#include <vector>

#include "core/common.h"

namespace leap {

class Machine;

class SoundtrackDecoder {
 public:
  static constexpr unsigned kRate = 11025;

  SoundtrackDecoder();
  ~SoundtrackDecoder();
  // The BaseROM and cartridge images (as load_rom_file gives them).
  bool open(const std::vector<u8>& bios, const std::vector<u8>& cart, std::string* err);
  // The soundtrack at `addr` in the cartridge's address space (asset address).
  bool decode(u32 addr, std::vector<s16>* pcm, std::string* err);

 private:
  std::unique_ptr<Machine> m_;
  u32 decode_fn_ = 0, init_fn_ = 0, close_fn_ = 0;
};

}  // namespace leap
