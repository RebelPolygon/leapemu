#pragma once

// ROM patches: changes to a cartridge image kept apart from it, for cheats and
// modifications. A game's patches live in one text file, each patch a named
// list of edits that can be switched on and off:
//
//   leapemu-patches 1
//   cart_crc 1a2b3c4d
//   cart_title Pet Pals
//
//   patch on Infinite lives
//   0001a2c4 0400 0000
//   patch off Skip the intro
//   00200010 e0010a00 4a264a26
//
// An edit line is: file offset in the image, the original bytes, the new
// bytes (hex). An edit is applied only where the image still holds its
// original bytes, so a patch made for one version of a game cannot damage
// another.

#include <string>
#include <vector>

#include "core/common.h"

namespace leap {

struct RomEdit {
  u32 offset = 0;
  std::vector<u8> from, to;  // the same length
};

struct RomPatch {
  std::string name;
  bool enabled = true;
  std::vector<RomEdit> edits;
};

struct PatchFile {
  u32 cart_crc = 0;
  std::string cart_title;
  std::vector<RomPatch> patches;

  bool load(const std::string& path, std::string* err);
  bool save(const std::string& path, std::string* err) const;
  // The enabled patches' edits, in order.
  std::vector<RomEdit> enabled_edits() const;
};

struct PatchResult {
  unsigned applied = 0, mismatched = 0;  // edits
  u32 crc = 0;                            // identifies the applied edits (0: none)
};

// Applies the edits whose original bytes match; later edits win.
PatchResult apply_edits(std::vector<u8>& image, const std::vector<RomEdit>& edits);

}  // namespace leap
