#include "core/rom.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>

namespace leap {

namespace {

u16 le16(const u8* p) { return u16(p[0] | (p[1] << 8)); }
u32 le32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }

bool read_file(const std::string& path, std::vector<u8>* out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { *err = "cannot open " + path; return false; }
  f.seekg(0, std::ios::end);
  const auto size = f.tellg();
  f.seekg(0);
  out->resize(size_t(size));
  if (size > 0 && !f.read(reinterpret_cast<char*>(out->data()), size)) {
    *err = "cannot read " + path;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Minimal raw-DEFLATE decoder (RFC 1951). Small and dependency-free; ROMs are
// at most a few MiB so speed is unimportant.
// ---------------------------------------------------------------------------
class Inflater {
 public:
  // `limit`: stop once at least that much is out (the start of the data).
  Inflater(const u8* src, size_t len, size_t limit = SIZE_MAX) : src_(src), len_(len), limit_(limit) {}

  bool run(std::vector<u8>* out) {
    out_ = out;
    bool last = false;
    while (!last && out_->size() < limit_) {
      last = bits(1);
      const unsigned type = bits(2);
      if (err_) return false;
      if (type == 0) {
        if (!stored()) return false;
      } else if (type == 1) {
        build_fixed();
        if (!codes()) return false;
      } else if (type == 2) {
        if (!dynamic() || !codes()) return false;
      } else {
        return false;
      }
    }
    return !err_;
  }

 private:
  struct Huff {
    u16 count[16]{};
    u16 symbol[288]{};
  };

  unsigned bits(unsigned n) {
    u32 v = bitbuf_;
    while (bitcnt_ < n) {
      if (pos_ >= len_) { err_ = true; return 0; }
      v |= u32(src_[pos_++]) << bitcnt_;
      bitcnt_ += 8;
    }
    bitbuf_ = v >> n;
    bitcnt_ -= n;
    return v & ((1u << n) - 1);
  }

  static void build(Huff& h, const u8* lengths, unsigned n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (unsigned i = 0; i < n; i++) h.count[lengths[i]]++;
    h.count[0] = 0;
    u16 offs[16];
    offs[1] = 0;
    for (unsigned i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
    for (unsigned i = 0; i < n; i++)
      if (lengths[i]) h.symbol[offs[lengths[i]]++] = u16(i);
  }

  int decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
      code |= int(bits(1));
      if (err_) return -1;
      const int count = h.count[len];
      if (code - count < first) return h.symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    err_ = true;
    return -1;
  }

  bool stored() {
    bitbuf_ = 0;
    bitcnt_ = 0;
    if (pos_ + 4 > len_) return false;
    const unsigned n = le16(src_ + pos_);
    pos_ += 4;
    if (pos_ + n > len_) return false;
    out_->insert(out_->end(), src_ + pos_, src_ + pos_ + n);
    pos_ += n;
    return true;
  }

  void build_fixed() {
    u8 l[288 + 30];
    unsigned i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    build(lit_, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    build(dist_, l, 30);
  }

  bool dynamic() {
    static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const unsigned nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
    if (err_ || nlen > 286 || ndist > 30) return false;
    u8 lengths[320]{};
    for (unsigned i = 0; i < ncode; i++) lengths[order[i]] = u8(bits(3));
    Huff lencode;
    build(lencode, lengths, 19);
    unsigned i = 0;
    while (i < nlen + ndist) {
      int sym = decode(lencode);
      if (sym < 0) return false;
      if (sym < 16) { lengths[i++] = u8(sym); continue; }
      u8 val = 0;
      unsigned rep;
      if (sym == 16) {
        if (i == 0) return false;
        val = lengths[i - 1];
        rep = 3 + bits(2);
      } else if (sym == 17) {
        rep = 3 + bits(3);
      } else {
        rep = 11 + bits(7);
      }
      if (i + rep > nlen + ndist) return false;
      while (rep--) lengths[i++] = val;
    }
    build(lit_, lengths, nlen);
    build(dist_, lengths + nlen, ndist);
    return !err_;
  }

  bool codes() {
    static const u16 lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const u8 lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const u16 dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                  193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                  6145, 8193, 12289, 16385, 24577};
    static const u8 dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int sym = decode(lit_);
      if (sym < 0) return false;
      if (out_->size() >= limit_) return true;
      if (sym < 256) { out_->push_back(u8(sym)); continue; }
      if (sym == 256) return true;
      sym -= 257;
      if (sym >= 29) return false;
      const unsigned len = lbase[sym] + bits(lext[sym]);
      const int ds = decode(dist_);
      if (ds < 0 || ds >= 30) return false;
      const size_t dist = dbase[ds] + bits(dext[ds]);
      if (err_ || dist > out_->size()) return false;
      const size_t from = out_->size() - dist;
      for (unsigned k = 0; k < len; k++) out_->push_back((*out_)[from + k]);
    }
  }

  const u8* src_;
  size_t len_, limit_;
  size_t pos_ = 0;
  u32 bitbuf_ = 0;
  unsigned bitcnt_ = 0;
  bool err_ = false;
  std::vector<u8>* out_ = nullptr;
  Huff lit_, dist_;
};

bool ends_with_ci(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (std::tolower(u8(s[s.size() - n + i])) != suffix[i]) return false;
  return true;
}

// The ROM image's member of a zip: the first .bin, or the only member.
struct ZipMember { u64 data = 0; u32 csize = 0, usize = 0; unsigned method = 0; };

// `read(offset, n)` gives up to n bytes of the zip at `offset`.
bool find_zip_member(u64 zip_size, const std::function<std::vector<u8>(u64, size_t)>& read, ZipMember* m,
                     std::string* err) {
  // Locate the end-of-central-directory record.
  if (zip_size < 22) { *err = "zip too small"; return false; }
  const u64 tail_at = zip_size > 22 + 65535 ? zip_size - (22 + 65535) : 0;
  const std::vector<u8> tail = read(tail_at, size_t(zip_size - tail_at));
  size_t eocd = std::string::npos;
  for (size_t i = tail.size() >= 22 ? tail.size() - 22 + 1 : 0; i-- > 0;)
    if (le32(&tail[i]) == 0x06054b50) { eocd = i; break; }
  if (eocd == std::string::npos) { *err = "zip: no end of central directory"; return false; }
  const unsigned entries = le16(&tail[eocd + 10]);
  const std::vector<u8> dir = read(le32(&tail[eocd + 16]), le32(&tail[eocd + 12]));

  bool found = false;
  u64 local = 0;
  for (size_t p = 0, e = 0; e < entries && !found; e++) {
    if (p + 46 > dir.size() || le32(&dir[p]) != 0x02014b50) break;
    const unsigned nlen = le16(&dir[p + 28]), xlen = le16(&dir[p + 30]), clen = le16(&dir[p + 32]);
    if (p + 46 + nlen > dir.size()) break;
    const std::string name(reinterpret_cast<const char*>(&dir[p + 46]), nlen);
    if (ends_with_ci(name, ".bin") || entries == 1) {
      found = true;
      m->method = le16(&dir[p + 10]);
      m->csize = le32(&dir[p + 20]);  // (the central directory always has the sizes)
      m->usize = le32(&dir[p + 24]);
      local = le32(&dir[p + 42]);
    }
    p += 46 + nlen + xlen + clen;
  }
  if (!found) { *err = "zip: no .bin member"; return false; }

  const std::vector<u8> h = read(local, 30);
  if (h.size() < 30 || le32(&h[0]) != 0x04034b50) { *err = "zip: bad local header"; return false; }
  m->data = local + 30 + le16(&h[26]) + le16(&h[28]);
  if (m->data + m->csize > zip_size) { *err = "zip: truncated"; return false; }
  return true;
}

bool extract_zip(const std::vector<u8>& zip, std::vector<u8>* out, std::string* err) {
  ZipMember zm;
  auto read = [&](u64 at, size_t n) {
    if (at >= zip.size()) return std::vector<u8>();
    return std::vector<u8>(zip.begin() + std::ptrdiff_t(at), zip.begin() + std::ptrdiff_t(std::min<u64>(zip.size(), at + n)));
  };
  if (!find_zip_member(zip.size(), read, &zm, err)) return false;
  const size_t data = size_t(zm.data);
  const u32 csize = zm.csize, usize = zm.usize;
  const unsigned method = zm.method;

  out->clear();
  if (method == 0) {
    out->assign(zip.begin() + data, zip.begin() + data + csize);
  } else if (method == 8) {
    out->reserve(usize);
    Inflater inf(&zip[data], csize);
    if (!inf.run(out)) { *err = "zip: corrupt deflate stream"; return false; }
  } else {
    *err = "zip: unsupported compression method " + std::to_string(method);
    return false;
  }
  if (out->size() != usize) { *err = "zip: size mismatch"; return false; }
  return true;
}

bool unwrap_wav(std::vector<u8>* img) {
  const auto& v = *img;
  if (v.size() < 12 || std::memcmp(v.data(), "RIFF", 4) != 0 || std::memcmp(v.data() + 8, "WAVE", 4) != 0)
    return false;
  size_t p = 12;
  while (p + 8 <= v.size()) {
    const u32 sz = le32(&v[p + 4]);
    if (std::memcmp(&v[p], "data", 4) == 0) {
      const size_t end = std::min(v.size(), p + 8 + size_t(sz));
      std::vector<u8> data(v.begin() + p + 8, v.begin() + end);
      *img = std::move(data);
      return true;
    }
    p += 8 + sz + (sz & 1);
  }
  return false;
}

std::string cstring_at(const std::vector<u8>& img, u32 base, u32 addr) {
  if (addr < base) return {};
  size_t off = addr - base;
  std::string s;
  while (off < img.size() && img[off] && s.size() < 256) s.push_back(char(img[off++]));
  return s;
}

}  // namespace

u32 crc32(const u8* data, size_t len) {
  static u32 table[256];
  static bool init = false;
  if (!init) {
    for (u32 i = 0; i < 256; i++) {
      u32 c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  u32 c = 0xffffffffu;
  for (size_t i = 0; i < len; i++) c = table[(c ^ data[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

bool load_rom_file(const std::string& path, std::vector<u8>* out, std::string* err) {
  std::vector<u8> raw;
  if (!read_file(path, &raw, err)) return false;
  if (raw.size() >= 4 && le32(raw.data()) == 0x04034b50) {
    if (!extract_zip(raw, out, err)) return false;
  } else {
    *out = std::move(raw);
  }
  unwrap_wav(out);
  if (out->empty()) { *err = path + " is empty"; return false; }
  return true;
}

bool peek_rom_file(const std::string& path, size_t n, std::vector<u8>* out, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { *err = "cannot open " + path; return false; }
  f.seekg(0, std::ios::end);
  const u64 size = u64(f.tellg());
  auto read = [&](u64 at, size_t len) {
    std::vector<u8> v(size_t(std::min<u64>(len, at < size ? size - at : 0)));
    f.clear();
    f.seekg(std::streamoff(at));
    if (!v.empty() && !f.read(reinterpret_cast<char*>(v.data()), std::streamsize(v.size()))) v.clear();
    return v;
  };
  std::vector<u8> head = read(0, 4);
  if (head.size() == 4 && le32(head.data()) == 0x04034b50) {
    ZipMember zm;
    if (!find_zip_member(size, read, &zm, err)) return false;
    out->clear();
    if (zm.method == 0) {
      *out = read(zm.data, std::min<size_t>(n, zm.csize));
    } else if (zm.method == 8) {
      // Enough compressed data for n bytes out (a stored block is at most 64 KiB).
      const std::vector<u8> src = read(zm.data, std::min<size_t>(zm.csize, n + 128 * 1024));
      Inflater inf(src.data(), src.size(), n);
      inf.run(out);  // (a partial stream ends in an error; what came out is kept)
    } else {
      *err = "zip: unsupported compression method " + std::to_string(zm.method);
      return false;
    }
  } else if (head.size() == 4 && std::memcmp(head.data(), "RIFF", 4) == 0) {
    if (!load_rom_file(path, out, err)) return false;  // (a wrapped BaseROM: small)
  } else {
    *out = read(0, n);
  }
  if (out->size() > n) out->resize(n);
  if (out->empty()) { *err = path + " is empty"; return false; }
  return true;
}

unsigned join_rom_blocks(std::vector<u8>* img) {
  constexpr size_t kBlock = 0x40'0000, kHeader = 24;
  unsigned n = 0;
  for (size_t at = kBlock; at + kHeader <= img->size(); at += kBlock) {
    const u8* h = img->data() + at;
    if (std::memcmp(h, "LBK", 3) != 0 || !std::all_of(h + 3, h + kHeader, [](u8 c) { return std::isxdigit(c) != 0; })) break;
    img->erase(img->begin() + std::ptrdiff_t(at), img->begin() + std::ptrdiff_t(at + kHeader));
    n++;
  }
  return n;
}

int stuck_address_line(const std::vector<u8>& img, const RomHeader& h) {
  if (!h.valid || h.device_end <= h.device_start) return -1;
  const u64 used = u64(h.device_end) - h.device_start;
  for (int bit = 16; bit < 31; bit++) {
    const size_t half = size_t(1) << bit;
    if (used <= half || img.size() < 2 * half) break;  // (the image doesn't use addresses with this bit)
    bool copy = true;
    for (size_t at = 0; copy && at + 2 * half <= img.size(); at += 2 * half)
      copy = std::memcmp(&img[at], &img[at + half], half) == 0;
    if (copy) return bit;
  }
  return -1;
}

RomHeader parse_rom_header(const std::vector<u8>& img) {
  RomHeader h;
  static const char sig[] = "Copyright LeapFrog     ";
  if (img.size() < 0x144 || std::memcmp(&img[0x100], sig, sizeof(sig) - 1) != 0) return h;
  const u8* p = &img[0x118];
  // p[0..1] = minor/major table version, p[2..3] = RIB count
  h.device_start = le32(p + 4);
  h.device_end = le32(p + 8);
  h.rib_table = le32(p + 0x28);
  h.valid = true;

  // Walk the RIB groups to find the product-info group (0x1003).
  if (h.rib_table < h.device_start) return h;
  size_t r = h.rib_table - h.device_start;
  if (r + 32 > img.size() || std::memcmp(&img[r], "LEAP", 4) != 0) return h;
  const unsigned groups = le16(&img[r + 6]);
  r += 32;
  for (unsigned g = 0; g < groups && r + 8 <= img.size(); g++, r += 8) {
    const unsigned id = le16(&img[r]);
    const unsigned count = le16(&img[r + 2]);
    const u32 addr = le32(&img[r + 4]);
    if (id != 0x1003 || addr < h.device_start) continue;
    size_t q = addr - h.device_start;
    for (unsigned i = 0; i < count && q + 8 <= img.size(); i++, q += 8) {
      const unsigned info = le16(&img[q]);
      const u32 val = le32(&img[q + 4]);
      switch (info) {
        case 0x04: h.copyright = cstring_at(img, h.device_start, val); break;
        case 0x07: h.version = cstring_at(img, h.device_start, val); break;
        case 0x0a: h.part_number = cstring_at(img, h.device_start, val); break;
        case 0x0b: h.title = cstring_at(img, h.device_start, val); break;
        case 0x0c: h.build_tool = cstring_at(img, h.device_start, val); break;
        case 0x0f: h.build_date = cstring_at(img, h.device_start, val); break;
      }
    }
  }
  return h;
}

bool inflate_zlib(const u8* src, size_t len, std::vector<u8>* out) {
  if (len < 2 || (src[0] & 0x0f) != 8 || ((src[0] << 8) | src[1]) % 31 != 0) return false;  // zlib header, deflate
  Inflater inf(src + 2, len - 2);
  return inf.run(out);
}

const char* rom_group_name(u16 id) {
  switch (id) {
    case 0x1000: return "Boot";
    case 0x1001: return "Modules";
    case 0x1003: return "Product info";
    case 0x1006: return "Assets";
    case 0x1009: return "System apps";
    case 0x100c: return "Leapster datasets";
    case 0x100d: return "C-style datasets";
    case 0x2000: return "Apps";
    default: return nullptr;
  }
}

// Asset table ids, as LeapSplit (Leapster-Tools) names them; SWF, speech and
// A-law audio were also confirmed here by their contents.
const char* rom_asset_type_name(u16 type) {
  switch (type) {
    case 0x1: return "Flash fonts";
    case 0x2: return "Instruments";
    case 0x3: return "GAS audio";
    case 0x4: return "Speech (CELP)";
    case 0x5: return "A-law audio";
    case 0x6: return "SYN music";
    case 0x7: return "SWF (Flash)";
    case 0x9: return "PEG bitmaps";
    case 0xd: return "Flash bitmaps";
    case 0xe: return "Video (AVI)";
    case 0xf: return "Soundtracks";
    case 0x10: return "Cursors";
    default: return nullptr;
  }
}

const char* rom_asset_extension(u16 type) {
  switch (type) {
    case 0x6: return ".syn";
    case 0x7: return ".swf";
    default: return ".bin";
  }
}

RomContents list_rom_contents(const std::vector<u8>& img) {
  RomContents c;
  const RomHeader h = parse_rom_header(img);
  if (!h.valid) return c;
  auto rd16 = [&](size_t o) -> u32 { return o + 2 <= img.size() ? le16(&img[o]) : 0; };
  auto rd32 = [&](size_t o) -> u32 { return o + 4 <= img.size() ? le32(&img[o]) : 0; };
  auto off = [&](u32 addr) -> size_t { return addr >= h.device_start ? size_t(addr - h.device_start) : SIZE_MAX; };
  c.full_checksum = rd32(0x124);
  c.sparse_checksum = rd32(0x128);
  const size_t r = off(h.rib_table);
  if (r == SIZE_MAX || r + 32 > img.size() || std::memcmp(&img[r], "LEAP", 4) != 0) return c;
  const unsigned ngroups = rd16(r + 6);
  for (unsigned g = 0; g < ngroups && r + 32 + 8 * (g + 1) <= img.size(); g++) {
    const size_t e = r + 32 + 8 * g;
    c.groups.push_back({u16(rd16(e)), u16(rd16(e + 2)), rd32(e + 4)});
  }
  for (const RomGroup& g : c.groups) {
    const size_t q = off(g.addr);
    if (q == SIZE_MAX) continue;
    for (unsigned i = 0; i < g.count && q + 8 * (i + 1) <= img.size(); i++) {
      const u32 id = rd16(q + 8 * i), val = rd32(q + 8 * i + 4);
      if (g.id == 0x1003) {  // product info
        if (id == 0x1) c.product_id = val;
        if (id == 0x8) c.rom_version = val;
        if (id == 0xc) c.build_tool = cstring_at(img, h.device_start, val);
      }
      if (g.id != 0x1006) continue;
      // An asset table: 8 bytes, the first handle, the count, then pointers.
      const size_t t = off(val);
      if (t == SIZE_MAX || t + 16 > img.size()) continue;
      const u32 first = rd32(t + 8), n = rd32(t + 12);
      for (u32 k = 0; k < n && t + 16 + 4 * (k + 1) <= img.size(); k++) {
        const size_t a = off(rd32(t + 16 + 4 * k));
        if (a >= img.size()) continue;  // (null or outside the image)
        c.assets.push_back({u16(id), first + k, u32(a), 0, false});
      }
    }
  }
  // Sizes: a SWF gives its own; others run to the next asset.
  std::vector<u32> starts;
  for (const RomAsset& a : c.assets) starts.push_back(a.offset);
  std::sort(starts.begin(), starts.end());
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
  for (RomAsset& a : c.assets) {
    if (a.type == 0x7 && a.offset + 8 <= img.size() && std::memcmp(&img[a.offset + 1], "WS", 2) == 0 &&
        le32(&img[a.offset + 4]) <= img.size() - a.offset) {
      a.size = le32(&img[a.offset + 4]);
      a.exact = img[a.offset] == 'F';  // (compressed "CWS": the stored length is the unpacked one)
      if (!a.exact) a.size = 0;
    }
    if (!a.size) {
      auto next = std::upper_bound(starts.begin(), starts.end(), a.offset);
      a.size = u32((next == starts.end() ? img.size() : *next) - a.offset);
      a.exact = false;
    }
  }
  return c;
}

}  // namespace leap
