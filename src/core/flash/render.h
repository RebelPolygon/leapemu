#pragma once

// Redraws a frame of the BaseROM Flash player's display list from the SWF
// definitions, at any resolution, optionally with object transforms
// interpolated between two frames.

#include <array>
#include <deque>
#include <map>
#include <thread>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/common.h"
#include "core/flash/raster.h"
#include "core/flash/swf.h"

namespace leap { class Bus; }

namespace leap::flash {

// One object of the player's display list.
struct Object {
  u32 id = 0;             // player display object (stable while it exists)
  s32 parent = -1;        // index into Frame::objects
  u32 depth = 0;
  u16 clip_depth = 0;     // non-zero: masks later siblings up to this depth
  u8 type = 0;            // player character type (see kType*)
  u8 tag = 0;             // SWF tag code of the definition
  u16 char_id = 0;
  u32 def = 0;            // player's pointer to the definition data (cartridge address)
  u32 movie = 0;          // SWF file start (cartridge address)
  swf::Matrix local;      // relative to the parent
  swf::CxForm local_cx;
  u16 ratio = 0;          // morph shapes: 0..65535 between start and end
  bool visible = true;
};

struct Frame {
  // Player character types.
  // (9: a dynamic text field, 97: a movie loaded into a movie clip, 98: a
  // movie level.)
  static constexpr u8 kTypeShape = 0, kTypeText = 4, kTypeSprite = 6, kTypeMorph = 7, kTypeEditText = 9, kTypeLoaded = 97,
                      kTypeLevel = 98, kTypeRoot = 128;
  std::vector<Object> objects;  // pre-order (parents before children, siblings by depth)
  u32 background = 0xffffffff;  // RGBA
  bool unsupported = false;     // contains content the renderer cannot draw
  // Stage twips -> screen pixels.
  swf::Matrix view{1.0 / 20, 0, 0, 1.0 / 20, 0, 0};

  // Dynamic text fields, as the BaseROM's text engine drew them: glyphs of
  // its device fonts (4-bit anti-aliased bitmaps) at screen pixel positions.
  struct Glyph {
    u32 bits = 0;            // glyph rows in the BaseROM, (w + 1) / 2 bytes each, high nibble first
    u16 w = 0, h = 0;
    s16 x = 0, y = 0;        // screen pixels
    u32 rgb = 0;             // byte 0 red, 1 green, 2 blue
    s16 clip[4] = {};        // x0, y0, x1, y1 (inclusive), screen pixels
  };
  // Text in a movie's embedded outline font (UseOutlines) is drawn by the
  // player as glyph shapes instead: each one's DefineFont2 shape record and
  // its placement in the field's own space (twips).
  struct Outline {
    u32 shape = 0;           // cartridge address of the glyph's shape
    swf::Matrix m;
  };
  struct Field {
    swf::Matrix placed;      // the field's stage -> screen pixels transform when drawn
    std::vector<Glyph> glyphs;
    std::vector<Outline> outlines;
  };
  std::unordered_map<u32, Field> fields;  // by display object
};

// Resolves emulated ROM addresses (cartridge, BaseROM) to host memory.
struct RomView {
  struct Range { u32 addr; const u8* base; size_t size; };
  std::vector<Range> ranges;
  const u8* ptr(u32 a) const {
    for (const Range& r : ranges) if (a >= r.addr && a - r.addr < r.size) return r.base + (a - r.addr);
    return nullptr;
  }
  size_t avail(u32 a) const {
    for (const Range& r : ranges) if (a >= r.addr && a - r.addr < r.size) return r.size - (a - r.addr);
    return 0;
  }
};

// Reads the Flash player's display list, rooted in the player context `ctx`
// (the value at offset 8 of any display object), into `out`. Objects are
// listed parents first, siblings in depth order. Uses peek* only.
bool snapshot(const Bus& bus, u32 ctx, Frame& out);

class Renderer {
 public:
  void set_rom(const RomView& rom) { rom_ = rom; carriers_key_ = 0; shapes_.clear(); movies_.clear(); bitmaps_.clear(); texts_.clear(); fonts_.clear(); morphs_.clear(); glyphs_.clear(); field_rgba_.clear(); }

  // Draws frame `b` (or its objects interpolated from `a` at t in [0,1]) at
  // `scale` output pixels per screen pixel into `out` (160*scale square,
  // ARGB). Returns false if `b` has content that could not be drawn.
  // `before`: the frame before `a`, if known (how parts were already moving).
  bool render(const Frame& b, const Frame* a, double t, int scale, std::vector<u32>& out, const Frame* before = nullptr);
  // How objects are matched between frames for interpolation: by timeline
  // slot (depth path) or by display object instance.
  enum class Match { Slot, Instance };
  Match match = Match::Slot;
  // Carry flat sibling parts by the joints between them (on by default).
  bool skeleton = true;
  // A part that jumps far for its size with nothing carrying it is a drawing
  // reused elsewhere, not motion: it changes at the midpoint (on by default).
  bool reuse_check = true;
  // A group of touching parts that mostly changes drawings (a new pose)
  // changes whole at the midpoint, rather than mixing two poses (on by default).
  bool pose_switch = true;
  // Why the last render() returned false (diagnostics).
  const std::string& issue() const { return issue_; }
  // Text fields of the last rendered frame whose text was never captured
  // (drawn as empty): their screen rectangles (x0, y0, x1, y1), to check
  // against the LCD.
  const std::vector<std::array<int, 4>>& uncaptured_fields() const { return uncaptured_; }
  // The other text fields' screen rectangles (drawn from captured glyphs).
  const std::vector<std::array<int, 4>>& captured_fields() const { return field_rects_; }
  // The drawn frame's objects' own (parent-relative) transforms at the last
  // render()'s moment (diagnostics).
  const std::vector<swf::Matrix>& interpolated_locals() const { return locals_; }

 private:
  struct Ctx;
  const swf::Shape* shape(const Object& o);
  const swf::Movie* movie(u32 addr);
  const swf::Bitmap* bitmap(u32 movie_addr, u16 id);
  const swf::Text* text(const Object& o);
  const swf::Font* font(u32 movie_addr, u16 id);
  bool draw_text(Ctx& c, const Object& o, const swf::Matrix& m, const swf::CxForm& cx, const float* mask);
  bool draw_field(Ctx& c, const Object& o, const swf::Matrix& m, const swf::CxForm& cx, const float* mask);
  void draw_node(Ctx& c, int index, const float* mask);
  void draw_shape(Ctx& c, const swf::Shape& s, const swf::Matrix& m, const swf::CxForm& cx, u32 movie, const float* mask,
                  std::vector<float>* mask_out, const u32* fill_rgba = nullptr);

  RomView rom_;
  std::vector<swf::Matrix> locals_;
  // Interpolation skeleton (see render()): each part's carrier, if any, and
  // its transform relative to the carrier in the drawn and the other frame.
  struct Carrier {
    int by = -1;
    swf::Matrix rb, ra;
    // A joint: the point kept in place (qx, qy in the carrier's space, px, py
    // in the part's). The part is placed so it stays there at every moment.
    bool joint = false;
    double qx = 0, qy = 0, px = 0, py = 0;
  };
  void find_carriers(const Frame& b, const Frame& a, const std::unordered_map<u64, int>& in_a, const std::vector<u64>& kb,
                     const std::vector<char>& glides);
  std::vector<Carrier> carriers_;
  std::vector<size_t> order_;   // carriers before the parts they carry
  u64 carriers_key_ = 0;        // the frame pair they were found for
  std::unordered_map<u32, std::unique_ptr<swf::Shape>> shapes_;    // by definition address
  std::unordered_map<u32, std::unique_ptr<swf::Movie>> movies_;    // by SWF address
  std::map<std::pair<u32, u16>, std::unique_ptr<swf::Bitmap>> bitmaps_;
  std::map<std::pair<u32, u16>, std::unique_ptr<swf::Text>> texts_;
  std::map<std::pair<u32, u16>, std::unique_ptr<swf::Font>> fonts_;
  std::map<std::pair<u32, u16>, std::unique_ptr<swf::MorphShape>> morphs_;
  std::unordered_map<u32, std::unique_ptr<swf::Shape>> glyphs_;    // outline glyphs, by shape address
  std::map<std::pair<u32, u16>, u32> field_rgba_;                  // text fields' colours
  std::vector<std::unique_ptr<swf::Shape>> morph_now_;  // this render's morph shapes, by object index
  const swf::MorphShape* morph(const Object& o);
  // Scratch buffers of each horizontal band (drawn in parallel at larger
  // scales): coverage, and masks (one per nesting level), for its rows only.
  struct Band { Coverage cov; std::deque<std::vector<float>> masks; };
  std::deque<Band> bands_;
  std::string issue_;
  std::vector<std::array<int, 4>> uncaptured_, field_rects_;
  void prepare(const Frame& b);
  void fail(Ctx& c, const char* what, const Object* o, u32 detail = 0);
};

}  // namespace leap::flash
