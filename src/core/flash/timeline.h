#pragma once

// Plays a SWF movie's timelines (the main timeline and every movie clip's)
// from their control tags alone: PlaceObject(2), RemoveObject(2), ShowFrame.
// ActionScript is not run. Used as ground truth for Flash rendering and
// interpolation tests, and to replay frames the Leapster's player skipped.

#include <map>
#include <memory>
#include <vector>

#include "core/common.h"
#include "core/flash/render.h"
#include "core/flash/swf.h"

namespace leap::flash {

class Timeline {
 public:
  // `movie` is loaded from `movie_addr` (its address in emulated memory, used
  // for definition pointers).
  Timeline(const swf::Movie& movie, u32 movie_addr);

  // Advance by one frame (the main timeline and all clips, looping at their
  // ends). The first call shows frame 1.
  void step();
  int frame() const { return root_.frame; }
  int frame_count() const { return int(root_.frames.size()); }

  // The current display list, in the form of a player snapshot. Instance ids
  // are stable while a timeline moves an object and new when it places one.
  Frame snapshot() const;

 private:
  struct Placed;
  struct Clip {
    std::vector<std::vector<std::pair<const u8*, size_t>>> frames;  // control tags per frame
    int frame = 0;                                                    // current frame (1-based, 0 = before start)
    std::map<u16, std::unique_ptr<Placed>> list;                      // by depth
  };
  struct Placed {
    u32 instance = 0;
    u16 id = 0;
    const swf::TagRef* def = nullptr;
    swf::Matrix m;
    swf::CxForm cx;
    u16 ratio = 0, clip_depth = 0;
    std::unique_ptr<Clip> clip;  // movie clips
  };

  void load_clip(Clip& c, const u8* tags, size_t n);
  void advance(Clip& c);
  void apply(Clip& c, u16 code, const u8* body, size_t len);
  void emit(const Clip& c, s32 parent, Frame& out) const;

  const swf::Movie& movie_;
  u32 movie_addr_;
  Clip root_;
  u32 next_instance_ = 1;
};

}  // namespace leap::flash
