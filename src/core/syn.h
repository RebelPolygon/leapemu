#pragma once

// SYN music (asset type 6): the BaseROM's sequencer format. Observed layout,
// checked against every SYN asset of 54 cartridges (all parse exactly to
// their end marker), with command meanings from the SYN format notes
// (docs/cart-bios-abi.md):
//
//   u16 0x0002, u16 track count, then per track: u16 offset, u16 id (BE).
//   A track is a list of commands:
//     00-7F d      a note of that pitch lasting d ticks (pitch 0: a rest)
//     81-84        (sequencer use; no operand)
//     85 x         (unknown; x is 0-7)
//     88 v         volume (used per note, as accents)
//     89 p         program p of the BaseROM's bank (General MIDI order;
//                  126 and 127 are drum kits)
//     89 C0 n      the cartridge's own instrument n
//     8A b d       pitch slide (b centred on 0x80) over d ticks
//     8E n         loop start: n repeats, 127 = forever
//     8F 00        loop end
//     FF 00        end of track
//   A duration d is one byte, or 0x80|high, low for longer ones.
//
// A tick is 4 ms (250 Hz): a title theme measured in leapemu's audio loops
// exactly when its SYN data says it should.

#include <string>
#include <vector>

#include "core/common.h"

namespace leap {

constexpr double kSynTickSeconds = 0.004;

struct SynNote {
  u32 start = 0, length = 0;  // ticks
  u8 pitch = 60, volume = 127;
  u16 program = 0;            // 0-127: the BaseROM's bank; 0x100 | n: cartridge instrument n
};

struct SynSong {
  std::vector<std::vector<SynNote>> tracks;
  u32 length = 0;             // ticks
  bool loops_forever = false;
};

// `forever_plays`: how many times a loop marked "forever" is played.
bool parse_syn(const u8* data, size_t size, SynSong* out, std::string* err, unsigned forever_plays = 2);

// A standard MIDI file (type 1, one track per SYN track): 125 ticks per
// quarter note at 120 bpm, so a MIDI tick is a SYN tick. Drum kits go to
// channel 10; cartridge instruments become piano, named in a text event.
std::vector<u8> syn_to_midi(const SynSong& song, const std::string& title);

// Renders the song with simple General MIDI-style instruments (an
// approximation: not the game's own instrument samples).
std::vector<s16> render_syn(const SynSong& song, unsigned rate);

}  // namespace leap
