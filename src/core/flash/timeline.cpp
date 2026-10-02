#include "core/flash/timeline.h"

namespace leap::flash {

namespace {

class Bits {
 public:
  Bits(const u8* p, size_t n) : p_(p), n_(n) {}
  u32 ub(unsigned bits) {
    u32 v = 0;
    for (unsigned i = 0; i < bits; i++) {
      const size_t byte = bit_ / 8;
      v = (v << 1) | (byte < n_ ? (p_[byte] >> (7 - bit_ % 8)) & 1 : 0);
      bit_++;
    }
    return v;
  }
  s32 sb(unsigned bits) {
    const u32 v = ub(bits);
    return bits && (v >> (bits - 1)) ? s32(v | (~0u << bits)) : s32(v);
  }
  void align() { bit_ = (bit_ + 7) & ~size_t(7); }
  size_t pos() const { return (bit_ + 7) / 8; }
  void seek(size_t byte) { bit_ = byte * 8; }
  u8 u8v() { align(); const u8 v = bit_ / 8 < n_ ? p_[bit_ / 8] : 0; bit_ += 8; return v; }
  u16 u16v() { const u16 lo = u8v(); return u16(lo | (u8v() << 8)); }
  swf::Matrix matrix() {
    align();
    swf::Matrix m;
    if (ub(1)) { const unsigned nb = ub(5); m.a = sb(nb) / 65536.0; m.d = sb(nb) / 65536.0; }
    if (ub(1)) { const unsigned nb = ub(5); m.b = sb(nb) / 65536.0; m.c = sb(nb) / 65536.0; }
    const unsigned nb = ub(5);
    m.tx = sb(nb);
    m.ty = sb(nb);
    align();
    return m;
  }
  swf::CxForm cxform(bool alpha) {
    align();
    swf::CxForm c;
    const bool add = ub(1), mul = ub(1);
    const unsigned nb = ub(4);
    const int channels = alpha ? 4 : 3;
    if (mul) for (int i = 0; i < channels; i++) c.mul[i] = sb(nb) / 256.0f;
    if (add) for (int i = 0; i < channels; i++) c.add[i] = float(sb(nb));
    align();
    return c;
  }

 private:
  const u8* p_;
  size_t n_;
  size_t bit_ = 0;
};

// Control tags between ShowFrames, from a tag stream.
void split_frames(const u8* d, size_t n, std::vector<std::vector<std::pair<const u8*, size_t>>>& frames) {
  frames.clear();
  std::vector<std::pair<const u8*, size_t>> cur;
  size_t o = 0;
  while (o + 2 <= n) {
    const size_t header = o;
    const u16 h = u16(d[o] | (d[o + 1] << 8));
    o += 2;
    const u16 code = h >> 6;
    size_t len = h & 0x3f;
    if (len == 0x3f) {
      if (o + 4 > n) break;
      len = d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (size_t(d[o + 3]) << 24);
      o += 4;
    }
    if (o + len > n) break;
    if (code == 0) break;
    if (code == 1) { frames.push_back(std::move(cur)); cur.clear(); }
    else if (code == 4 || code == 5 || code == 26 || code == 28) cur.push_back({d + header, len});
    o += len;
  }
}

// The tag header before a body (short or long form) was kept; decode it.
u16 tag_code(const u8* p) { return u16(p[0] | (p[1] << 8)) >> 6; }
const u8* tag_body(const u8* p) { return ((p[0] | (p[1] << 8)) & 0x3f) == 0x3f ? p + 6 : p + 2; }

u8 type_of(u16 code) {
  switch (code) {
    case 2: case 22: case 32: return Frame::kTypeShape;
    case 11: case 33: return Frame::kTypeText;
    case 39: return Frame::kTypeSprite;
    case 37: return Frame::kTypeEditText;
    case 7: case 34: return 2;   // button
    case 46: return Frame::kTypeMorph;
    default: return 255;
  }
}

}  // namespace

Timeline::Timeline(const swf::Movie& movie, u32 movie_addr) : movie_(movie), movie_addr_(movie_addr) {
  const u8* d = movie.data;
  if (!d || movie.size < 12) return;
  const unsigned nb = d[8] >> 3;
  const size_t start = 8 + (5 + 4 * nb + 7) / 8 + 4;
  const size_t len = std::min<size_t>(movie.size, d[4] | (d[5] << 8) | (d[6] << 16) | (size_t(d[7]) << 24));
  if (start < len) load_clip(root_, d + start, len - start);
}

void Timeline::load_clip(Clip& c, const u8* tags, size_t n) { split_frames(tags, n, c.frames); }

void Timeline::step() { advance(root_); }

void Timeline::advance(Clip& c) {
  if (c.frames.empty()) return;
  // Clips already on stage advance first; clips placed this frame start at 1.
  for (auto& [depth, p] : c.list)
    if (p->clip) advance(*p->clip);
  c.frame++;
  if (c.frame > int(c.frames.size())) {  // loop: rebuild from frame 1
    c.frame = 1;
    c.list.clear();
  }
  for (const auto& [tag, len] : c.frames[size_t(c.frame - 1)]) apply(c, tag_code(tag), tag_body(tag), len);
}

void Timeline::apply(Clip& c, u16 code, const u8* body, size_t len) {
  Bits r(body, len);
  auto place = [&](u16 depth, u16 id) -> Placed& {
    auto p = std::make_unique<Placed>();
    p->instance = next_instance_++;
    p->id = id;
    p->def = movie_.find(id);
    if (p->def && p->def->code == 39 && p->def->len >= 4) {  // movie clip: its own timeline, at frame 1
      p->clip = std::make_unique<Clip>();
      load_clip(*p->clip, p->def->body + 4, p->def->len - 4);
    }
    Placed& ref = *p;
    c.list[depth] = std::move(p);
    return ref;
  };
  switch (code) {
    case 4: {  // PlaceObject
      const u16 id = r.u16v(), depth = r.u16v();
      Placed& p = place(depth, id);
      p.m = r.matrix();
      if (r.pos() < len) p.cx = r.cxform(false);
      if (p.clip) advance(*p.clip);
      break;
    }
    case 26: {  // PlaceObject2
      const u8 flags = r.u8v();
      const u16 depth = r.u16v();
      Placed* p = nullptr;
      const auto it = c.list.find(depth);
      if (flags & 0x02) {  // character: a new instance (replacing, it keeps the old transform)
        const u16 id = r.u16v();
        swf::Matrix m;
        swf::CxForm cx;
        u16 ratio = 0, clip = 0;
        if (it != c.list.end() && (flags & 0x01)) { m = it->second->m; cx = it->second->cx; ratio = it->second->ratio; clip = it->second->clip_depth; }
        p = &place(depth, id);
        p->m = m;
        p->cx = cx;
        p->ratio = ratio;
        p->clip_depth = clip;
      } else if (it != c.list.end()) {
        p = it->second.get();
      }
      if (!p) break;
      if (flags & 0x04) p->m = r.matrix();
      if (flags & 0x08) p->cx = r.cxform(true);
      if (flags & 0x10) p->ratio = r.u16v();
      if (flags & 0x20) while (r.pos() < len && r.u8v()) {}  // name
      if (flags & 0x40) p->clip_depth = r.u16v();
      if ((flags & 0x02) && p->clip) advance(*p->clip);
      break;
    }
    case 5:  // RemoveObject
      r.u16v();
      c.list.erase(r.u16v());
      break;
    case 28:  // RemoveObject2
      c.list.erase(r.u16v());
      break;
    default:
      break;
  }
}

Frame Timeline::snapshot() const {
  Frame f;
  Object root;
  root.type = Frame::kTypeRoot;
  root.id = 0x80000000u;
  f.objects.push_back(root);
  emit(root_, 0, f);
  return f;
}

void Timeline::emit(const Clip& c, s32 parent, Frame& out) const {
  for (const auto& [depth, p] : c.list) {
    if (!p->def) continue;
    Object o;
    o.id = p->instance;
    o.parent = parent;
    o.depth = depth;
    o.clip_depth = p->clip_depth;
    o.type = type_of(p->def->code);
    o.tag = u8(p->def->code);
    o.char_id = p->id;
    o.movie = movie_addr_;
    o.local = p->m;
    o.local_cx = p->cx;
    o.ratio = p->ratio;
    const u8* body = p->def->body;
    size_t skip = 0;
    if (o.type == Frame::kTypeShape && p->def->len > 3) skip = 2 + (5 + 4 * (body[2] >> 3) + 7) / 8;  // id, bounds
    o.def = movie_addr_ + u32(body + skip - movie_.data);
    if (o.type == 255) continue;
    const s32 index = s32(out.objects.size());
    out.objects.push_back(o);
    if (p->clip) emit(*p->clip, index, out);
  }
}

}  // namespace leap::flash
