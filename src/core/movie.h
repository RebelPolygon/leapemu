#pragma once

// Input movies for tool-assisted play (TAS): the input of every frame from
// power-on, replayed to reproduce a run exactly. Emulation is deterministic:
// the same BaseROM, cartridge, settings, EEPROM contents and per-frame input
// always give the same machine state.
//
// File format (".lmv", text, one frame per line so it can be edited and
// diffed):
//
//   leapemu-movie 1
//   bios_crc af05e5a0
//   cart_crc 1a2b3c4d
//   cart_title Pet Pals
//   cart_patches 5e6f7a8b      (only with ROM patches: identifies the edits)
//   timing 0 0 0.75 1.5 1 2 0 0 1.5 1.5 0 0 32 2 0 0
//   idle_skip 1
//   allow_unsigned 0
//   stub_missing_services 1    (only when on: see Machine::stub_missing_services)
//   auto_calibrate 1
//   system_eeprom <1024 hex digits>
//   cart_eeprom <4096 hex digits>
//   rerecords 12
//   author ...
//   comment ...
//   input
//   ...............|
//   ....A..........|
//   ...............|80,79
//
// Each input line has one column per button, a letter when pressed and '.'
// otherwise (Up Down Left Right A B Hint Pause homE, volume v/V, brightness
// k/K, contrast c, power switch off O), then '|' and the stylus position when
// it is touching. (Lines without the power column read as switched on.)

#include <array>
#include <string>
#include <vector>

#include "core/common.h"
#include "core/machine.h"

namespace leap {

struct InputFrame {
  u32 buttons = 0;  // Button mask
  bool touch = false;
  u8 x = 0, y = 0;  // stylus position in screen pixels, when touching
  bool power_off = false;  // power switch in the off position
  bool operator==(const InputFrame&) const = default;
};

// Applies one frame's input to the machine, as the frontends do: the stylus is
// left to the automatic calibration tapper while it runs, unless touching.
void apply_input(Machine& m, const InputFrame& in);

class Movie {
 public:
  // Emulation settings and power-on contents.
  u32 bios_crc = 0, cart_crc = 0, cart_patches = 0;
  std::string cart_title;
  Machine::Timing timing;
  bool idle_skip = true, allow_unsigned = false, auto_calibrate = true, stub_missing_services = false;
  std::array<u8, 512> system_eeprom{};
  std::array<u8, 2048> cart_eeprom{};
  u64 rerecords = 0;
  std::string author, comment;
  std::vector<InputFrame> frames;  // frames[n]: input during frame n (0 = first after power-on)

  // A movie of `m`'s current setup and EEPROM contents, with no input yet.
  static Movie from_machine(Machine& m);
  // Checks that `m` has the movie's BaseROM and cartridge, applies the
  // movie's settings and EEPROM contents, and resets (power-on). Frame n of
  // the movie is then the input for the machine's frame_count() == n.
  bool start(Machine& m, std::string* err) const;

  bool save(const std::string& path, std::string* err) const;
  bool load(const std::string& path, std::string* err);

  static std::string encode(const InputFrame& in);
  static bool decode(const std::string& line, InputFrame& out);
};

}  // namespace leap
