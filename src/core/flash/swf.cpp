#include "core/flash/swf.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

#include "core/flash/jpeg.h"
#include "core/rom.h"

namespace leap::swf {

// ---------------------------------------------------------------------------
// Matrices and colour transforms
// ---------------------------------------------------------------------------

Matrix Matrix::operator*(const Matrix& l) const {
  Matrix r;
  r.a = a * l.a + c * l.b;
  r.b = b * l.a + d * l.b;
  r.c = a * l.c + c * l.d;
  r.d = b * l.c + d * l.d;
  r.tx = a * l.tx + c * l.ty + tx;
  r.ty = b * l.tx + d * l.ty + ty;
  return r;
}

Matrix Matrix::inverse() const {
  const double det = a * d - b * c;
  if (std::abs(det) < 1e-12) return Matrix{0, 0, 0, 0, 0, 0};
  Matrix r;
  r.a = d / det;
  r.b = -b / det;
  r.c = -c / det;
  r.d = a / det;
  r.tx = -(r.a * tx + r.c * ty);
  r.ty = -(r.b * tx + r.d * ty);
  return r;
}

Matrix Matrix::lerp(const Matrix& p, const Matrix& q, double t) {
  auto m = [&](double x, double y) { return x + (y - x) * t; };
  // Interpolate rotation, scale and skew rather than the raw entries, so a
  // rotating part keeps its size: M = R(angle) * [sx shear; 0 sy].
  struct Parts { double angle, sx, sy, shear; };
  auto split = [](const Matrix& x, Parts& o) {
    o.sx = std::hypot(x.a, x.b);
    if (o.sx < 1e-9) return false;
    o.angle = std::atan2(x.b, x.a);
    const double cs = std::cos(o.angle), sn = std::sin(o.angle);
    o.shear = x.c * cs + x.d * sn;
    o.sy = -x.c * sn + x.d * cs;
    return true;
  };
  Parts a, b;
  if (!split(p, a) || !split(q, b))
    return Matrix{m(p.a, q.a), m(p.b, q.b), m(p.c, q.c), m(p.d, q.d), m(p.tx, q.tx), m(p.ty, q.ty)};
  double da = b.angle - a.angle;  // shortest way round
  while (da > 3.14159265358979) da -= 6.28318530717959;
  while (da < -3.14159265358979) da += 6.28318530717959;
  const double ang = a.angle + da * t, sx = m(a.sx, b.sx), sy = m(a.sy, b.sy), sh = m(a.shear, b.shear);
  const double cs = std::cos(ang), sn = std::sin(ang);
  Matrix r{sx * cs, sx * sn, sh * cs - sy * sn, sh * sn + sy * cs, m(p.tx, q.tx), m(p.ty, q.ty)};
  // The position moves on the arc of the change's own rotation, not on a
  // straight line. As complex numbers, p -> q is (close to) the similarity
  // z -> u z + v, u = scale * e^(i angle): a rotation about its fixed point.
  // Following it with u(t) = scale(t) * e^(i angle t) keeps parts that turn
  // about a shared joint (a flat set of sibling parts making up a limb)
  // together, where straight lines would pull them apart mid-way.
  // Mirroring changes are left on straight lines.
  // Only for a change that is a turn with uniform scale (a similarity): with
  // other scaling or shear, that model is not the change, and a point the two
  // endpoints share would drift in between. Straight lines keep such points.
  const double dp = p.a * p.d - p.b * p.c, dq = q.a * q.d - q.b * q.c, dr = r.a * r.d - r.b * r.c;
  bool similar = false;
  if (std::abs(dp) > 1e-12) {
    // A = linear(q) * linear(p)^-1
    const double ia = p.d / dp, ib = -p.b / dp, ic = -p.c / dp, id = p.a / dp;
    const double Aa = q.a * ia + q.c * ib, Ab = q.b * ia + q.d * ib, Ac = q.a * ic + q.c * id, Ad = q.b * ic + q.d * id;
    const double norm = std::abs(Aa) + std::abs(Ab) + std::abs(Ac) + std::abs(Ad);
    similar = std::abs(Aa - Ad) + std::abs(Ab + Ac) <= 0.02 * norm;
  }
  if (arc && similar && dp * dq > 0 && dr * dp > 0) {
    using C = std::complex<double>;
    const C u = std::polar(std::sqrt(dq / dp), da);
    const C ut = std::polar(std::sqrt(dr / dp), da * t);
    const C z0(p.tx, p.ty), v = C(q.tx, q.ty) - u * z0;
    // (u(t) - 1) / (u - 1): -> t as the change becomes a pure translation.
    const C k = std::abs(u - 1.0) < 1e-6 ? C(t) : (ut - 1.0) / (u - 1.0);
    const C z = ut * z0 + v * k;
    r.tx = z.real();
    r.ty = z.imag();
  }
  return r;
}

CxForm CxForm::operator*(const CxForm& child) const {
  CxForm r;
  for (int i = 0; i < 4; i++) {
    r.mul[i] = child.mul[i] * mul[i];
    r.add[i] = child.add[i] * mul[i] + add[i];
  }
  return r;
}

bool CxForm::identity() const {
  for (int i = 0; i < 4; i++)
    if (mul[i] != 1 || add[i] != 0) return false;
  return true;
}

CxForm CxForm::lerp(const CxForm& p, const CxForm& q, double t) {
  CxForm r;
  for (int i = 0; i < 4; i++) {
    r.mul[i] = float(p.mul[i] + (q.mul[i] - p.mul[i]) * t);
    r.add[i] = float(p.add[i] + (q.add[i] - p.add[i]) * t);
  }
  return r;
}

// ---------------------------------------------------------------------------
// Bit-level reader
// ---------------------------------------------------------------------------

namespace {

class Reader {
 public:
  Reader(const u8* p, size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  size_t pos() const { return (bit_ + 7) / 8; }

  u32 ub(unsigned bits) {
    u32 v = 0;
    for (unsigned i = 0; i < bits; i++) {
      if (bit_ / 8 >= n_) { ok_ = false; return 0; }
      v = (v << 1) | ((p_[bit_ / 8] >> (7 - bit_ % 8)) & 1);
      bit_++;
    }
    return v;
  }
  s32 sb(unsigned bits) {
    const u32 v = ub(bits);
    return bits && (v >> (bits - 1)) ? s32(v | (~0u << bits)) : s32(v);
  }
  void align() { bit_ = (bit_ + 7) & ~size_t(7); }
  u8 u8v() { align(); if (bit_ / 8 >= n_) { ok_ = false; return 0; } const u8 v = p_[bit_ / 8]; bit_ += 8; return v; }
  u16 u16v() { const u16 lo = u8v(); return u16(lo | (u8v() << 8)); }
  u32 u32v() { const u32 lo = u16v(); return lo | (u32(u16v()) << 16); }
  u32 rgb() { const u32 r = u8v(), g = u8v(), b = u8v(); return (r << 24) | (g << 16) | (b << 8) | 0xff; }
  u32 rgba() { const u32 r = u8v(), g = u8v(), b = u8v(), a = u8v(); return (r << 24) | (g << 16) | (b << 8) | a; }
  void skip(size_t bytes) { align(); bit_ += bytes * 8; if (bit_ / 8 > n_) ok_ = false; }

  void rect() {
    const unsigned nb = ub(5);
    for (int i = 0; i < 4; i++) sb(nb);
    align();
  }
  Matrix matrix() {
    align();
    Matrix m;
    if (ub(1)) { const unsigned nb = ub(5); m.a = sb(nb) / 65536.0; m.d = sb(nb) / 65536.0; }
    if (ub(1)) { const unsigned nb = ub(5); m.b = sb(nb) / 65536.0; m.c = sb(nb) / 65536.0; }
    const unsigned nb = ub(5);
    m.tx = sb(nb);
    m.ty = sb(nb);
    align();
    return m;
  }

 private:
  const u8* p_;
  size_t n_;
  size_t bit_ = 0;
  bool ok_ = true;
};

bool read_fill_styles(Reader& r, int version, std::vector<FillStyle>& out) {
  unsigned n = r.u8v();
  if (n == 0xff && version >= 2) n = r.u16v();
  for (unsigned i = 0; i < n && r.ok(); i++) {
    FillStyle f;
    const u8 type = r.u8v();
    switch (type) {
      case 0x00:
        f.type = FillStyle::kSolid;
        f.rgba = version >= 3 ? r.rgba() : r.rgb();
        break;
      case 0x10: case 0x12: case 0x13: {
        f.type = type == 0x10 ? FillStyle::kLinear : FillStyle::kRadial;
        f.matrix = r.matrix();
        const unsigned count = r.u8v() & 15;
        for (unsigned k = 0; k < count; k++) {
          const u8 ratio = r.u8v();
          f.stops.push_back({ratio, version >= 3 ? r.rgba() : r.rgb()});
        }
        if (type == 0x13) r.u16v();  // focal point (Flash 8)
        if (f.stops.empty()) f.stops.push_back({0, 0x000000ff});
        break;
      }
      case 0x40: case 0x41: case 0x42: case 0x43:
        f.type = FillStyle::kBitmap;
        f.bitmap_id = r.u16v();
        f.matrix = r.matrix();
        f.repeat = type == 0x40 || type == 0x42;
        f.smooth = type == 0x40 || type == 0x41;
        break;
      default:
        return false;
    }
    out.push_back(std::move(f));
  }
  return r.ok();
}

bool read_line_styles(Reader& r, int version, std::vector<LineStyle>& out) {
  unsigned n = r.u8v();
  if (n == 0xff && version >= 2) n = r.u16v();
  for (unsigned i = 0; i < n && r.ok(); i++) {
    LineStyle l;
    l.width = r.u16v();
    l.rgba = version >= 3 ? r.rgba() : r.rgb();
    out.push_back(l);
  }
  return r.ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------

namespace {

// SHAPERECORDs up to the end record. Style indices are offset by the styles
// already in `out` when the records start.
bool parse_records(Reader& r, int version, Shape& out) {
  r.align();
  unsigned fill_bits = r.ub(4), line_bits = r.ub(4);
  u32 fill_base = 0, line_base = 0;  // global index offsets of the current style arrays
  u16 fill0 = 0, fill1 = 0, line = 0;
  float x = 0, y = 0;
  for (int guard = 0; guard < 1 << 20 && r.ok(); guard++) {
    if (r.ub(1) == 0) {
      const u32 flags = r.ub(5);
      if (flags == 0) {  // EndShapeRecord
        if (!out.groups.empty()) out.groups.push_back({u32(out.fills.size()), u32(out.lines.size())});
        return r.ok();
      }
      if (flags & 1) {  // MoveTo
        const unsigned nb = r.ub(5);
        x = float(r.sb(nb));
        y = float(r.sb(nb));
      }
      auto style = [&](unsigned bits, u32 base, size_t count) -> u16 {
        const u32 v = r.ub(bits);
        return v == 0 || v > count - base ? u16(0) : u16(base + v);
      };
      if (flags & 2) fill0 = style(fill_bits, fill_base, out.fills.size());
      if (flags & 4) fill1 = style(fill_bits, fill_base, out.fills.size());
      if (flags & 8) line = style(line_bits, line_base, out.lines.size());
      if (flags & 16) {  // NewStyles (DefineShape2 and later)
        out.groups.push_back({u32(out.fills.size()), u32(out.lines.size())});
        fill_base = u32(out.fills.size());
        line_base = u32(out.lines.size());
        if (!read_fill_styles(r, version, out.fills) || !read_line_styles(r, version, out.lines)) return false;
        r.align();
        fill_bits = r.ub(4);
        line_bits = r.ub(4);
        fill0 = fill1 = line = 0;
      }
      continue;
    }
    Edge e{};
    e.x0 = x;
    e.y0 = y;
    e.fill0 = fill0;
    e.fill1 = fill1;
    e.line = line;
    const bool straight = r.ub(1);
    const unsigned nb = r.ub(4) + 2;
    if (straight) {
      float dx = 0, dy = 0;
      if (r.ub(1)) { dx = float(r.sb(nb)); dy = float(r.sb(nb)); }
      else if (r.ub(1)) dy = float(r.sb(nb));
      else dx = float(r.sb(nb));
      e.curve = false;
      e.x1 = x + dx;
      e.y1 = y + dy;
      e.cx = (e.x0 + e.x1) / 2;
      e.cy = (e.y0 + e.y1) / 2;
    } else {
      const float cdx = float(r.sb(nb)), cdy = float(r.sb(nb));
      const float adx = float(r.sb(nb)), ady = float(r.sb(nb));
      e.curve = true;
      e.cx = x + cdx;
      e.cy = y + cdy;
      e.x1 = e.cx + adx;
      e.y1 = e.cy + ady;
    }
    x = e.x1;
    y = e.y1;
    out.edges.push_back(e);
  }
  return false;
}

}  // namespace

void Shape::compute_bounds() {
  x0 = y0 = 1e30f;
  x1 = y1 = -1e30f;
  for (const Edge& e : edges)
    for (const float* p : {&e.x0, &e.cx, &e.x1}) {
      const float x = p[0], y = p[1];
      x0 = std::min(x0, x); x1 = std::max(x1, x);
      y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
  if (edges.empty()) { x0 = y0 = 1; x1 = y1 = 0; }
  fill_edges.assign(fills.size() + 1, {});
  for (size_t i = 0; i < edges.size(); i++) {
    const Edge& e = edges[i];
    if (e.fill0 == e.fill1) continue;
    if (e.fill0 && e.fill0 <= fills.size()) fill_edges[e.fill0].push_back(u32(i));
    if (e.fill1 && e.fill1 <= fills.size()) fill_edges[e.fill1].push_back(u32(i));
  }
}

bool parse_shape_with_style(const u8* p, size_t n, int version, Shape& out) {
  out = {};
  Reader r(p, n);
  if (!read_fill_styles(r, version, out.fills) || !read_line_styles(r, version, out.lines)) return false;
  const bool ok = parse_records(r, version, out);
  out.compute_bounds();
  return ok;
}

bool parse_glyph(const u8* p, size_t n, Shape& out) {
  out = {};
  out.fills.push_back(FillStyle{});  // fill style 1: the text colour, supplied when drawing
  Reader r(p, n);
  const bool ok = parse_records(r, 1, out);
  out.compute_bounds();
  return ok;
}

bool parse_font(const TagRef& tag, Font& out) {
  out = {};
  const u8* b = tag.body;
  const size_t n = tag.len;
  if (n < 4) return false;
  std::vector<u32> offsets;
  size_t table = 2;  // offsets are relative to the start of the offset table
  if (tag.code == 10) {  // DefineFont
    const u32 first = b[2] | (b[3] << 8);
    const size_t count = first / 2;
    if (4 + count * 2 > n) return false;
    for (size_t i = 0; i < count; i++) offsets.push_back(b[2 + 2 * i] | (b[3 + 2 * i] << 8));
  } else if (tag.code == 48) {  // DefineFont2
    if (n < 5) return false;
    const u8 flags = b[2];
    const size_t name_len = b[4];
    size_t o = 5 + name_len;
    if (o + 2 > n) return false;
    const size_t count = b[o] | (b[o + 1] << 8);
    o += 2;
    table = o;
    const bool wide = flags & 0x08;
    const size_t esz = wide ? 4 : 2;
    if (o + count * esz > n) return false;
    for (size_t i = 0; i < count; i++) {
      const u8* e = b + o + i * esz;
      offsets.push_back(wide ? u32(e[0] | (e[1] << 8) | (e[2] << 16) | (u32(e[3]) << 24)) : u32(e[0] | (e[1] << 8)));
    }
  } else {
    return false;
  }
  out.glyphs.resize(offsets.size());
  for (size_t i = 0; i < offsets.size(); i++) {
    const size_t o = table + offsets[i];
    if (o < n) parse_glyph(b + o, n - o, out.glyphs[i]);
  }
  return true;
}

bool parse_text(const TagRef& tag, Text& out) {
  out = {};
  if (tag.code != 11 && tag.code != 33) return false;
  const bool rgba = tag.code == 33;
  Reader r(tag.body, tag.len);
  r.u16v();  // character id
  r.rect();
  out.matrix = r.matrix();
  const unsigned glyph_bits = r.u8v(), advance_bits = r.u8v();
  Text::Run run;
  float x = 0, y = 0;
  for (int guard = 0; guard < 4096 && r.ok(); guard++) {
    const u8 flags = r.u8v();
    if (flags == 0) return r.ok();  // end of records
    if (flags & 0x08) run.font = r.u16v();
    if (flags & 0x04) run.rgba = rgba ? r.rgba() : r.rgb();
    if (flags & 0x01) x = float(s16(r.u16v()));
    if (flags & 0x02) y = float(s16(r.u16v()));
    if (flags & 0x08) run.height = r.u16v();
    const unsigned count = r.u8v();
    run.glyphs.clear();
    for (unsigned i = 0; i < count && r.ok(); i++) {
      Text::Glyph g;
      g.index = u16(r.ub(glyph_bits));
      g.x = x;
      g.y = y;
      x += float(r.sb(advance_bits));
      run.glyphs.push_back(g);
    }
    r.align();
    out.runs.push_back(run);
  }
  return false;
}

namespace {

bool read_morph_fills(Reader& r, MorphShape& m) {
  unsigned n = r.u8v();
  if (n == 0xff) n = r.u16v();
  for (unsigned i = 0; i < n && r.ok(); i++) {
    FillStyle a, b;
    const u8 type = r.u8v();
    switch (type) {
      case 0x00:
        a.rgba = r.rgba();
        b.rgba = r.rgba();
        break;
      case 0x10: case 0x12: {
        a.type = b.type = type == 0x10 ? FillStyle::kLinear : FillStyle::kRadial;
        a.matrix = r.matrix();
        b.matrix = r.matrix();
        const unsigned count = r.u8v() & 15;
        for (unsigned k = 0; k < count; k++) {
          const u8 ra = r.u8v();
          const u32 ca = r.rgba();
          const u8 rb = r.u8v();
          const u32 cb = r.rgba();
          a.stops.push_back({ra, ca});
          b.stops.push_back({rb, cb});
        }
        if (a.stops.empty()) { a.stops.push_back({0, 0x000000ff}); b.stops.push_back({0, 0x000000ff}); }
        break;
      }
      case 0x40: case 0x41: case 0x42: case 0x43:
        a.type = b.type = FillStyle::kBitmap;
        a.bitmap_id = b.bitmap_id = r.u16v();
        a.matrix = r.matrix();
        b.matrix = r.matrix();
        a.repeat = b.repeat = type == 0x40 || type == 0x42;
        a.smooth = b.smooth = type == 0x40 || type == 0x41;
        break;
      default:
        return false;
    }
    m.fill_start.push_back(a);
    m.fill_end.push_back(b);
  }
  return r.ok();
}

u32 lerp_rgba(u32 a, u32 b, double t) {
  u32 out = 0;
  for (int s = 0; s < 32; s += 8) out |= u32(std::lround(((a >> s) & 255) + (double((b >> s) & 255) - ((a >> s) & 255)) * t)) << s;
  return out;
}

}  // namespace

bool parse_morph(const TagRef& tag, MorphShape& out) {
  out = {};
  if (tag.code != 46) return false;
  Reader r(tag.body, tag.len);
  r.u16v();  // character id
  r.rect();
  r.rect();
  const u32 offset = r.u32v();
  const size_t end_at = r.pos() + offset;
  if (!read_morph_fills(r, out)) return false;
  unsigned n = r.u8v();
  if (n == 0xff) n = r.u16v();
  for (unsigned i = 0; i < n && r.ok(); i++) {
    LineStyle a, b;
    a.width = r.u16v();
    b.width = r.u16v();
    a.rgba = r.rgba();
    b.rgba = r.rgba();
    out.line_start.push_back(a);
    out.line_end.push_back(b);
  }
  if (!r.ok()) return false;
  out.start.fills = out.fill_start;
  out.start.lines = out.line_start;
  if (!parse_records(r, 3, out.start)) return false;
  if (end_at >= tag.len) return false;
  Reader e(tag.body + end_at, tag.len - end_at);
  Shape end;
  if (!parse_records(e, 3, end)) return false;
  out.end_edges = std::move(end.edges);
  return out.end_edges.size() == out.start.edges.size();
}

void MorphShape::at(double t, Shape& out) const {
  out = {};
  auto m = [&](double a, double b) { return a + (b - a) * t; };
  for (size_t i = 0; i < fill_start.size(); i++) {
    FillStyle f = fill_start[i];
    const FillStyle& e = fill_end[i];
    f.rgba = lerp_rgba(f.rgba, e.rgba, t);
    f.matrix = Matrix{m(f.matrix.a, e.matrix.a), m(f.matrix.b, e.matrix.b), m(f.matrix.c, e.matrix.c), m(f.matrix.d, e.matrix.d),
                      m(f.matrix.tx, e.matrix.tx), m(f.matrix.ty, e.matrix.ty)};
    for (size_t k = 0; k < f.stops.size() && k < e.stops.size(); k++) {
      f.stops[k].ratio = u8(std::lround(m(f.stops[k].ratio, e.stops[k].ratio)));
      f.stops[k].rgba = lerp_rgba(f.stops[k].rgba, e.stops[k].rgba, t);
    }
    out.fills.push_back(f);
  }
  for (size_t i = 0; i < line_start.size(); i++)
    out.lines.push_back(LineStyle{u16(std::lround(m(line_start[i].width, line_end[i].width))), lerp_rgba(line_start[i].rgba, line_end[i].rgba, t)});
  out.edges = start.edges;
  for (size_t i = 0; i < out.edges.size(); i++) {
    Edge& a = out.edges[i];
    const Edge& b = end_edges[i];
    auto f = [&](float& x, float y) { x = float(m(x, y)); };
    f(a.x0, b.x0); f(a.y0, b.y0); f(a.x1, b.x1); f(a.y1, b.y1); f(a.cx, b.cx); f(a.cy, b.cy);
    a.curve = a.curve || b.curve;  // a straight edge paired with a curve becomes a curve
  }
  out.compute_bounds();
}

bool edit_text_bounds(const TagRef& tag, s32 out[4]) {
  if (tag.code != 37) return false;
  Reader r(tag.body, tag.len);
  r.u16v();  // character id
  const unsigned nb = r.ub(5);
  for (int i = 0; i < 4; i++) out[i] = r.sb(nb);
  return r.ok();
}

bool edit_text_color(const TagRef& tag, u32* rgba) {
  if (tag.code != 37) return false;
  Reader r(tag.body, tag.len);
  r.u16v();  // character id
  r.rect();
  const u8 f1 = r.u8v(), f2 = r.u8v();
  if (!(f1 & 0x04)) return false;  // HasTextColor
  if (f1 & 0x01) r.skip(2);        // HasFont: font id
  if (f2 & 0x80)                   // HasFontClass (SWF 9 and later): a string
    while (r.ok() && r.u8v()) {}
  if (f1 & 0x01) r.skip(2);        // font height
  *rgba = r.rgba();
  return r.ok();
}

bool parse_define_shape(const u8* body, size_t n, int version, Shape& out) {
  Reader r(body, n);
  r.u16v();  // character id
  r.rect();
  if (!r.ok()) return false;
  const size_t off = r.pos();
  return parse_shape_with_style(body + off, n - off, version, out);
}

// ---------------------------------------------------------------------------
// Bitmaps
// ---------------------------------------------------------------------------

bool decode_lossless(const u8* body, size_t n, int version, Bitmap& out) {
  if (n < 7) return false;
  const u8 format = body[2];
  const int w = body[3] | (body[4] << 8), h = body[5] | (body[6] << 8);
  size_t off = 7;
  unsigned table = 0;
  if (format == 3) { if (n < 8) return false; table = body[7] + 1u; off = 8; }
  std::vector<u8> raw;
  if (!inflate_zlib(body + off, n - off, &raw)) return false;
  out.w = w;
  out.h = h;
  out.px.assign(size_t(w) * h, 0);
  const bool alpha = version >= 2;
  auto premul = [](u32 a, u32 r, u32 g, u32 b) { return (a << 24) | (r << 16) | (g << 8) | b; };
  if (format == 3) {
    const size_t entry = alpha ? 4 : 3;
    const size_t stride = (size_t(w) + 3) & ~size_t(3);
    if (raw.size() < table * entry + stride * h) return false;
    const u8* pal = raw.data();
    const u8* pix = raw.data() + table * entry;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        const unsigned i = pix[size_t(y) * stride + x];
        if (i >= table) continue;
        const u8* c = pal + i * entry;
        out.px[size_t(y) * w + x] = premul(alpha ? c[3] : 255, c[0], c[1], c[2]);  // Lossless2 is premultiplied
      }
    return true;
  }
  if (format == 4) {  // 15-bit RGB, rows padded to 32 bits
    const size_t stride = ((size_t(w) * 2) + 3) & ~size_t(3);
    if (raw.size() < stride * h) return false;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        const u8* p = &raw[size_t(y) * stride + size_t(x) * 2];
        const u32 v = (u32(p[0]) << 8) | p[1];
        const u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        out.px[size_t(y) * w + x] = premul(255, r * 255 / 31, g * 255 / 31, b * 255 / 31);
      }
    return true;
  }
  if (format == 5) {  // 32-bit (A)RGB
    if (raw.size() < size_t(w) * h * 4) return false;
    for (size_t i = 0; i < out.px.size(); i++) {
      const u8* p = &raw[i * 4];
      out.px[i] = premul(alpha ? p[0] : 255, p[1], p[2], p[3]);
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Movies
// ---------------------------------------------------------------------------

bool Movie::load(const u8* d, size_t n) {
  data = d;
  size = n;
  dict.clear();
  if (n < 8 || d[0] != 'F' || d[1] != 'W' || d[2] != 'S') return false;
  version = d[3];
  const size_t len = std::min<size_t>(n, d[4] | (d[5] << 8) | (d[6] << 16) | (size_t(d[7]) << 24));
  Reader r(d + 8, len - 8);
  r.rect();
  r.u16v();  // frame rate
  r.u16v();  // frame count
  if (!r.ok()) return false;
  size_t o = 8 + r.pos();
  while (o + 2 <= len) {
    const u16 h = u16(d[o] | (d[o + 1] << 8));
    o += 2;
    const u16 code = h >> 6;
    size_t tl = h & 0x3f;
    if (tl == 0x3f) {
      if (o + 4 > len) break;
      tl = d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (size_t(d[o + 3]) << 24);
      o += 4;
    }
    if (o + tl > len) break;
    const u8* body = d + o;
    if (code == 0) break;
    if (code == 8) { jpeg_tables = body; jpeg_tables_len = tl; }
    if (code == 9 && tl >= 3) background = (u32(body[0]) << 24) | (u32(body[1]) << 16) | (u32(body[2]) << 8) | 0xff;
    // Character definitions: every Define* tag starts with its id.
    switch (code) {
      case 2: case 6: case 7: case 10: case 11: case 14: case 20: case 21: case 22: case 32: case 33: case 34:
      case 35: case 36: case 37: case 39: case 46: case 48:
        if (tl >= 2) dict[u16(body[0] | (body[1] << 8))] = TagRef{code, body, tl};
        break;
      default:
        break;
    }
    o += tl;
  }
  return true;
}

bool decode_jpeg_bits(const u8* body, size_t n, int code, const Movie& movie, Bitmap& out) {
  if (n < 2) return false;
  std::vector<std::pair<const u8*, size_t>> parts;
  size_t jpeg_len = n - 2;
  const u8* alpha = nullptr;
  size_t alpha_len = 0;
  if (code == 6) {
    if (movie.jpeg_tables) parts.push_back({movie.jpeg_tables, movie.jpeg_tables_len});
  } else if (code == 35) {
    if (n < 6) return false;
    jpeg_len = body[2] | (body[3] << 8) | (body[4] << 16) | (size_t(body[5]) << 24);
    if (6 + jpeg_len > n) return false;
    alpha = body + 6 + jpeg_len;
    alpha_len = n - 6 - jpeg_len;
  } else if (code != 21) {
    return false;
  }
  parts.push_back({body + (code == 35 ? 6 : 2), jpeg_len});
  int w = 0, h = 0;
  if (!decode_jpeg(parts, w, h, out.px)) return false;
  out.w = w;
  out.h = h;
  if (alpha && alpha_len) {  // zlib-compressed alpha, one byte per pixel; premultiply
    std::vector<u8> a;
    if (inflate_zlib(alpha, alpha_len, &a) && a.size() >= out.px.size())
      for (size_t i = 0; i < out.px.size(); i++) {
        const u32 p = out.px[i], k = a[i];
        out.px[i] = (k << 24) | ((((p >> 16) & 255) * k / 255) << 16) | ((((p >> 8) & 255) * k / 255) << 8) | ((p & 255) * k / 255);
      }
  }
  return true;
}

}  // namespace leap::swf
