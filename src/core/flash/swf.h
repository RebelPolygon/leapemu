#pragma once

// SWF (Flash) data structures and parsers, following the public SWF file
// format specification. Used to redraw Flash content that the BaseROM's Flash
// player displays (see core/flash/render.h). Coordinates are twips (1/20
// pixel) unless noted.

#include <memory>
#include <unordered_map>
#include <vector>

#include "core/common.h"

namespace leap::swf {

// 2x3 affine matrix: x' = a*x + c*y + tx, y' = b*x + d*y + ty.
struct Matrix {
  double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
  Matrix operator*(const Matrix& l) const;  // apply `l` first, then this
  Matrix inverse() const;
  void apply(double x, double y, double& ox, double& oy) const {
    ox = a * x + c * y + tx;
    oy = b * x + d * y + ty;
  }
  // Rotation, scale and shear are interpolated as such; the position follows
  // the arc of the change's rotation (`arc`, on by default) or a straight line.
  static Matrix lerp(const Matrix& p, const Matrix& q, double t);
  static inline bool arc = true;
};

// Colour transform: c' = c * mul + add, per channel (add in 0..255 units).
struct CxForm {
  float mul[4] = {1, 1, 1, 1};  // r, g, b, a
  float add[4] = {0, 0, 0, 0};
  CxForm operator*(const CxForm& child) const;  // child first, then this
  bool identity() const;
  static CxForm lerp(const CxForm& p, const CxForm& q, double t);
};

struct GradientStop { u8 ratio; u32 rgba; };  // rgba: 0xRRGGBBAA

struct FillStyle {
  enum Type : u8 { kSolid, kLinear, kRadial, kBitmap } type = kSolid;
  u32 rgba = 0x000000ff;
  Matrix matrix;                       // gradient / bitmap space -> shape space
  std::vector<GradientStop> stops;
  u16 bitmap_id = 0;
  bool repeat = false, smooth = false;
};

struct LineStyle { u16 width = 20; u32 rgba = 0x000000ff; };

// One edge of a shape outline. Style indices are 1-based into Shape::fills /
// Shape::lines (0 = none); styles defined mid-shape are appended, so indices
// are global to the shape.
struct Edge {
  float x0, y0, cx, cy, x1, y1;
  bool curve;
  u16 fill0, fill1, line;
};

struct Shape {
  std::vector<FillStyle> fills;
  std::vector<LineStyle> lines;
  std::vector<Edge> edges;
  // Bounds of the outline (twips); empty (x0 > x1) without edges.
  float x0 = 1, y0 = 1, x1 = 0, y1 = 0;
  // fill_edges[f]: the edges on fill f's boundary (one side only), in order.
  std::vector<std::vector<u32>> fill_edges;
  // Style groups (DefineShape2 and later start a new one with NewStyles): the
  // end of each group's fills and lines. Each group is drawn over the ones
  // before it, its fills and then its lines. Empty: one group.
  struct Group { u32 fills, lines; };
  std::vector<Group> groups;
  // Computes the bounds and fill_edges (after the edges change).
  void compute_bounds();
};

// Parses a SHAPEWITHSTYLE record (fill styles onwards) of a DefineShape
// (version 1), DefineShape2 (2) or DefineShape3 (3) tag.
bool parse_shape_with_style(const u8* p, size_t n, int version, Shape& out);
// Parses a DefineShape tag body (character id and bounds first).
bool parse_define_shape(const u8* body, size_t n, int version, Shape& out);
// Parses a font glyph (a SHAPE: records only, fill style 1 = text colour).
bool parse_glyph(const u8* p, size_t n, Shape& out);

struct TagRef;

// Glyph outlines of a DefineFont / DefineFont2, in a 1024-unit EM square.
struct Font { std::vector<Shape> glyphs; };
bool parse_font(const TagRef& tag, Font& out);

// A DefineText / DefineText2: glyph runs positioned in text space.
struct Text {
  struct Glyph { u16 index; float x, y; };
  struct Run { u16 font = 0; u16 height = 240; u32 rgba = 0x000000ff; std::vector<Glyph> glyphs; };
  Matrix matrix;
  std::vector<Run> runs;
};
bool parse_text(const TagRef& tag, Text& out);
// Bounds (x0, x1, y0, y1 in twips) of a DefineEditText.
bool edit_text_bounds(const TagRef& tag, s32 out[4]);
// Text colour (RGBA, as Text::Run) of a DefineEditText, if it sets one.
bool edit_text_color(const TagRef& tag, u32* rgba);

// A DefineMorphShape: start and end outlines and styles, paired one to one.
struct MorphShape {
  std::vector<FillStyle> fill_start, fill_end;
  std::vector<LineStyle> line_start, line_end;
  Shape start;                  // start edges, with the style indices
  std::vector<Edge> end_edges;  // end positions of the same edges
  // The shape at `ratio` (0 = start, 1 = end).
  void at(double ratio, Shape& out) const;
};
bool parse_morph(const TagRef& tag, MorphShape& out);

// A decoded bitmap, premultiplied ARGB (0xAARRGGBB).
struct Bitmap { int w = 0, h = 0; std::vector<u32> px; };
// DefineBitsLossless (version 1) / DefineBitsLossless2 (2) tag body.
bool decode_lossless(const u8* body, size_t n, int version, Bitmap& out);

// DefineBits (6, with the movie's JPEGTables), DefineBitsJPEG2 (21) and
// DefineBitsJPEG3 (35, with an alpha channel) tag bodies.
struct Movie;
bool decode_jpeg_bits(const u8* body, size_t n, int code, const Movie& movie, Bitmap& out);

// A character definition located in a movie.
struct TagRef { u16 code = 0; const u8* body = nullptr; size_t len = 0; };

// Index of a SWF movie's top-level definition tags, by character id.
struct Movie {
  const u8* data = nullptr;
  size_t size = 0;
  int version = 0;
  u32 background = 0x000000ff;  // SetBackgroundColor, RGBA
  const u8* jpeg_tables = nullptr;  // JPEGTables (for DefineBits)
  size_t jpeg_tables_len = 0;
  std::unordered_map<u16, TagRef> dict;
  // Loads an uncompressed ("FWS") movie. `data` must stay valid.
  bool load(const u8* data, size_t size);
  const TagRef* find(u16 id) const {
    const auto it = dict.find(id);
    return it == dict.end() ? nullptr : &it->second;
  }
};

}  // namespace leap::swf
