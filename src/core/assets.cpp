#include "core/assets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

#include "core/celp.h"
#include "core/soundtrack.h"
#include "core/syn.h"

namespace fs = std::filesystem;

namespace leap {

namespace {

s16 alaw_to_linear(u8 v) {  // G.711
  v ^= 0x55;
  int t = (v & 0x0f) << 4;
  const int seg = (v & 0x70) >> 4;
  t += seg ? 0x108 : 8;
  if (seg > 1) t <<= seg - 1;
  return s16((v & 0x80) ? t : -t);
}

std::vector<u8> wav_bytes(const std::vector<s16>& pcm, unsigned rate) {
  std::vector<u8> f;
  auto w32 = [&](u32 v) { for (int k = 0; k < 4; k++) f.push_back(u8(v >> (8 * k))); };
  auto w16 = [&](u16 v) { f.push_back(u8(v)); f.push_back(u8(v >> 8)); };
  auto tag = [&](const char* t) { f.insert(f.end(), t, t + 4); };
  const u32 bytes = u32(pcm.size() * 2);
  tag("RIFF"); w32(36 + bytes); tag("WAVE"); tag("fmt ");
  w32(16); w16(1); w16(1); w32(rate); w32(rate * 2); w16(2); w16(16);
  tag("data"); w32(bytes);
  for (s16 s : pcm) w16(u16(s));
  return f;
}

std::vector<s16> decode_speech(const std::vector<u8>& img, u32 offset, CelpDecoder& dec) {
  std::vector<s16> pcm;
  auto rd = [&](u32 x) -> u8 { return x < img.size() ? img[x] : 0; };
  dec.start(offset);
  while (dec.active() && pcm.size() < CelpDecoder::kSampleRate * 120) pcm.push_back(dec.next_sample(rd));
  return pcm;
}

}  // namespace

std::string safe_file_name(std::string s) {
  for (char& c : s)
    if (std::strchr("/\\:*?\"<>|", c) || u8(c) < 32) c = '_';
  while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
  return s.empty() ? "unnamed" : s;
}

fs::path asset_folder(const fs::path& root, u16 type) {
  const char* tn = rom_asset_type_name(type);
  return root / safe_file_name(tn ? tn : "Type " + std::to_string(type));
}

// Whether a bitmap's format is one that decodes (by its header alone).
bool image_format_known(const std::vector<u8>& img, const RomAsset& a) {
  if (size_t(a.offset) + 20 > img.size()) return false;
  const u8* d = &img[a.offset];
  if (a.type == 0x9) return d[0] == 1 && (d[1] == 16 || d[1] == 8);
  if (a.type == 0xd) return (d[4] | d[5] << 8 | d[6] << 16 | u32(d[7]) << 24) == 2;
  return false;
}

std::string asset_file_name(const std::vector<u8>& img, const RomAsset& a, bool raw) {
  char name[16];
  std::snprintf(name, sizeof(name), "%04X", a.handle);
  const char* ext = rom_asset_extension(a.type);
  if (!raw) {
    if (a.type == 0x4 || a.type == 0x5 || a.type == 0xf) ext = ".wav";
    else if (a.type == 0x6) ext = ".mid";
    else if (a.type == 0xe) ext = ".avi";
    else if (asset_is_image(a) && image_format_known(img, a)) ext = ".bmp";
    else if (asset_is_font(a) && size_t(a.offset) + 0x92c <= img.size() && img[a.offset] == 0 && img[a.offset + 1] == 1) ext = ".ttf";
  }
  return std::string(name) + ext;
}

bool asset_is_image(const RomAsset& a) { return a.type == 0x9 || a.type == 0xd; }

namespace {

u32 argb4(unsigned a, unsigned r, unsigned g, unsigned b) { return (a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | b * 17; }

bool decode_peg(const u8* d, size_t n, AssetImage* out) {
  if (n < 16 || d[0] != 1 || (d[1] != 16 && d[1] != 8)) return false;
  const int bpp = d[1], w = d[2] | d[3] << 8, h = d[4] | d[5] << 8;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
  const size_t total = size_t(w) * h;
  out->w = w;
  out->h = h;
  out->argb.clear();
  out->argb.reserve(total);
  const size_t ps = bpp / 8;
  auto pixel = [&](size_t o) -> u32 {
    if (bpp == 8) {
      const u8 v = d[o];
      return 0xff00'0000u | u32(((v >> 5) & 7) * 255 / 7) << 16 | u32(((v >> 2) & 7) * 255 / 7) << 8 | u32((v & 3) * 85);
    }
    const u16 v = u16(d[o] | d[o + 1] << 8);
    return v == 0xffff ? 0u : argb4(15, (v >> 8) & 15, (v >> 4) & 15, v & 15);
  };
  size_t o = 16;
  while (out->argb.size() < total) {
    if (o >= n) return false;
    const u8 c = d[o++];
    if (c < 0x80) {
      if (o + ps > n) return false;
      out->argb.insert(out->argb.end(), size_t(c) + 1, pixel(o));
      o += ps;
    } else {
      const size_t k = c - 0x7f;
      if (o + k * ps > n) return false;
      for (size_t i = 0; i < k; i++) out->argb.push_back(pixel(o + i * ps));
      o += k * ps;
    }
  }
  out->argb.resize(total);
  return true;
}

// Flash bitmap format 2; `embedded`: runs may appear inside literals.
bool decode_flash2(const u8* d, size_t n, int w, int h, bool embedded, AssetImage* out) {
  out->w = w;
  out->h = h;
  out->argb.assign(size_t(w) * h, 0);
  int x = 0, y = 0;
  size_t o = 20;
  auto px = [&](size_t at) { const u16 v = u16(d[at] | d[at + 1] << 8); return argb4(15 - (v >> 12), (v >> 8) & 15, (v >> 4) & 15, v & 15); };
  auto put = [&](u32 v, int count) {
    if (y >= h || x + count > w) return false;
    std::fill_n(&out->argb[size_t(y) * w + x], count, v);
    x += count;
    return true;
  };
  while (o < n) {
    const u8 c = d[o++];
    switch (c) {
      case 0:
        if (o >= n || y >= h || x + d[o] > w) return false;
        x += d[o++];
        break;
      case 1:
        if (o + 3 > n || !put(px(o + 1), d[o])) return false;
        o += 3;
        break;
      case 2: case 3: {
        if (o >= n) return false;
        int k = d[o++];
        while (k > 0) {
          if (o + 2 > n) return false;
          if (embedded && d[o] == 0) {  // (a skip inside the literal)
            if (y >= h || x + d[o + 1] > w) return false;
            x += d[o + 1];
            o += 2;
            continue;
          }
          if (embedded && d[o] == 1) {  // (a run inside the literal)
            if (o + 4 > n || !put(px(o + 2), d[o + 1])) return false;
            o += 4;
            continue;
          }
          if (!put(px(o), 1)) return false;
          o += 2;
          k--;
        }
        break;
      }
      case 4: y++; x = 0; break;
      case 5: return y == h;
      default: return false;
    }
  }
  return false;
}

}  // namespace

bool decode_asset_image(const std::vector<u8>& img, const RomAsset& a, AssetImage* out) {
  if (size_t(a.offset) + a.size > img.size() || !image_format_known(img, a)) return false;
  const u8* d = &img[a.offset];
  if (a.type == 0x9) return decode_peg(d, a.size, out);
  const int w = int(d[8] | d[9] << 8), h = int(d[12] | d[13] << 8);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
  return decode_flash2(d, a.size, w, h, false, out) || decode_flash2(d, a.size, w, h, true, out);
}

std::vector<u8> bmp_bytes(const AssetImage& im) {
  std::vector<u8> f;
  auto w32 = [&](u32 v) { for (int k = 0; k < 4; k++) f.push_back(u8(v >> (8 * k))); };
  auto w16 = [&](u16 v) { f.push_back(u8(v)); f.push_back(u8(v >> 8)); };
  const u32 data = u32(im.argb.size() * 4), header = 14 + 108;  // BITMAPV4HEADER: with an alpha mask
  f.push_back('B'); f.push_back('M'); w32(header + data); w32(0); w32(header);
  w32(108); w32(u32(im.w)); w32(u32(-im.h)); w16(1); w16(32); w32(3 /* BI_BITFIELDS */); w32(data);
  w32(2835); w32(2835); w32(0); w32(0);
  w32(0x00ff'0000); w32(0x0000'ff00); w32(0x0000'00ff); w32(0xff00'0000);  // R, G, B, A masks
  w32(0x7352'4742);  // 'sRGB'
  for (int i = 0; i < 12; i++) w32(0);  // (endpoints and gamma: unused with sRGB)
  for (u32 p : im.argb) w32(p);  // (top-down rows: the negative height)
  return f;
}

bool convert_asset(const std::vector<u8>& img, const RomAsset& a, CelpDecoder* speech, bool raw, const std::string& title,
                   std::vector<u8>* out, SoundtrackDecoder* soundtracks) {
  out->clear();
  if (size_t(a.offset) + a.size > img.size()) return false;
  const u8* data = img.data() + a.offset;
  if (!raw && a.type == 0x6) {  // SYN music: MIDI
    SynSong song;
    std::string err;
    if (!parse_syn(data, a.size, &song, &err, 1)) return false;
    *out = syn_to_midi(song, title);
    return true;
  }
  if (!raw && asset_is_font(a)) {  // fonts: TrueType
    AssetFont f;
    if (decode_asset_font(img, a, &f)) {
      *out = ttf_bytes(f);
      return true;
    }
  }
  if (!raw && asset_is_image(a) && image_format_known(img, a)) {  // bitmaps: BMP
    AssetImage im;
    if (!decode_asset_image(img, a, &im)) return false;
    *out = bmp_bytes(im);
    return true;
  }
  if (!raw && a.type == 0xe && a.size >= 16 && std::memcmp(data + 8, "RIFF", 4) == 0) {  // video: the AVI after 8 bytes
    const u32 len = u32(data[12] | data[13] << 8 | data[14] << 16 | u32(data[15]) << 24) + 8;
    if (len > a.size - 8) return false;
    out->assign(data + 8, data + 8 + len);
    return true;
  }
  if (!raw && (a.type == 0x5 || (a.type == 0x4 && speech && speech->has_codebook()) || a.type == 0xf)) {
    std::vector<s16> pcm;
    unsigned rate = 0;
    if (!asset_audio(img, a, speech, 0, &pcm, &rate, soundtracks)) return false;
    *out = wav_bytes(pcm, rate);
    return true;
  }
  out->assign(data, data + a.size);
  return true;
}

bool export_asset(const std::vector<u8>& img, const RomAsset& a, CelpDecoder* speech, bool raw, const std::string& title,
                  const fs::path& path, SoundtrackDecoder* soundtracks) {
  std::vector<u8> bytes;
  if (!convert_asset(img, a, speech, raw, title, &bytes, soundtracks)) return false;
  std::error_code ec;
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary);
  return f && f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

bool asset_is_audio(const RomAsset& a) { return a.type == 0x4 || a.type == 0x5 || a.type == 0x6 || a.type == 0xf; }

bool asset_audio(const std::vector<u8>& img, const RomAsset& a, CelpDecoder* speech, unsigned music_rate, std::vector<s16>* pcm,
                 unsigned* rate, SoundtrackDecoder* soundtracks) {
  pcm->clear();
  if (size_t(a.offset) + a.size > img.size()) return false;
  if (a.type == 0xf) {  // soundtrack: the cartridge's own codec, emulated
    if (!soundtracks) return false;
    const RomHeader h = parse_rom_header(img);
    std::string err;
    if (!h.valid || !soundtracks->decode(h.device_start + a.offset, pcm, &err)) return false;
    *rate = SoundtrackDecoder::kRate;
    return true;
  }
  if (a.type == 0x4) {  // speech
    if (!speech || !speech->has_codebook()) return false;
    *pcm = decode_speech(img, a.offset, *speech);
    *rate = CelpDecoder::kSampleRate;
    return true;
  }
  if (a.type == 0x5) {  // A-law, 8 kHz
    for (u32 k = 0; k < a.size; k++) pcm->push_back(alaw_to_linear(img[a.offset + k]));
    *rate = 8000;
    return true;
  }
  if (a.type == 0x6 && music_rate) {  // SYN music
    SynSong song;
    std::string err;
    if (!parse_syn(img.data() + a.offset, a.size, &song, &err, 2)) return false;
    *pcm = render_syn(song, music_rate);
    *rate = music_rate;
    return true;
  }
  return false;
}

// --- Fonts ---------------------------------------------------------------------

bool asset_is_font(const RomAsset& a) { return a.type == 0x1; }

namespace {

// Characters 0x80-0xFF of Mac Roman.
const u16 kMacRoman[128] = {
    0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1, 0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8,
    0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3, 0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc,
    0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df, 0x00ae, 0x00a9, 0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8,
    0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211, 0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8,
    0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab, 0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca, 0x00ff, 0x0178, 0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02,
    0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1, 0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4,
    0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc, 0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7};

u32 rd32(const std::vector<u8>& v, size_t o) { return o + 4 <= v.size() ? u32(v[o] | v[o + 1] << 8 | v[o + 2] << 16 | u32(v[o + 3]) << 24) : 0; }

std::string cstring(const std::vector<u8>& img, size_t o) {
  std::string s;
  while (o < img.size() && img[o] && s.size() < 64) s.push_back(char(img[o++]));
  return s;
}

}  // namespace

bool decode_asset_font(const std::vector<u8>& img, const RomAsset& a, AssetFont* out) {
  const size_t at = a.offset;
  if (at + 0x92c > img.size() || img[at] != 0 || img[at + 1] != 1) return false;
  const u32 base = parse_rom_header(img).device_start;
  auto off = [&](u32 addr) -> size_t { return addr >= base ? size_t(addr - base) : SIZE_MAX; };
  *out = AssetFont{};
  const size_t fam = off(rd32(img, at + 4)), full = off(rd32(img, at + 8));
  if (fam < img.size()) out->family = cstring(img, fam);
  if (full < img.size()) out->full_name = cstring(img, full);
  out->height = img[at + 0x10];
  const bool mac = img[at + 0x19] == 0x3f;
  int present = 0;
  for (int c = 0; c < 256; c++) {
    const u8* r = &img[at + 0x2c + 5 * c];
    AssetFont::Glyph& g = out->glyphs[size_t(c)];
    g.w = r[0];
    g.h = r[1];
    g.top = s8(r[2]);
    g.left = s8(r[3]);
    g.advance = r[4];
    const size_t bits = off(rd32(img, at + 0x52c + 4 * size_t(c)));
    g.present = g.advance > 0 || (g.w > 0 && bits < img.size());
    if (g.w > 0 && g.h > 0 && bits < img.size()) {
      const size_t row = (size_t(g.w) + 1) / 2;
      if (bits + row * g.h > img.size()) return false;
      g.coverage.resize(size_t(g.w) * g.h);
      for (int y = 0; y < g.h; y++)
        for (int x = 0; x < g.w; x++) {
          const u8 b = img[bits + y * row + x / 2];
          g.coverage[size_t(y) * g.w + x] = x % 2 ? b & 15 : b >> 4;
        }
    } else {
      g.w = g.h = 0;
    }
    present += g.present;
    // Characters: ASCII, then the font's character set.
    u16 u = u16(c);
    if (c < 0x20 || c == 0x7f) u = 0;
    else if (c >= 0x80 && mac) u = kMacRoman[c - 0x80];
    else if (c >= 0x80) {
      switch (c) {  // ISO 8859-15's differences from Latin-1
        case 0xa4: u = 0x20ac; break;
        case 0xa6: u = 0x0160; break;
        case 0xa8: u = 0x0161; break;
        case 0xb4: u = 0x017d; break;
        case 0xb8: u = 0x017e; break;
        case 0xbc: u = 0x0152; break;
        case 0xbd: u = 0x0153; break;
        case 0xbe: u = 0x0178; break;
        default: break;
      }
      if (c < 0xa0) u = 0;  // (control characters in ISO 8859)
    }
    out->unicode[size_t(c)] = u;
  }
  return present > 0 && out->height > 0;
}

AssetImage font_sheet(const AssetFont& f) {
  int asc = 1, desc = 0, cw = 1;
  for (const auto& g : f.glyphs) {
    asc = std::max(asc, g.top);
    desc = std::max(desc, g.h - g.top);
    cw = std::max(cw, std::max(0, g.left) + g.w);
  }
  cw += 2;
  const int ch = asc + desc + 2;
  AssetImage im;
  im.w = 16 * cw;
  im.h = 14 * ch;
  im.argb.assign(size_t(im.w) * im.h, 0);
  for (int c = 0x20; c < 0x100; c++) {
    const AssetFont::Glyph& g = f.glyphs[size_t(c)];
    const int ox = ((c - 0x20) % 16) * cw + 1 + std::max(0, g.left), oy = ((c - 0x20) / 16) * ch + 1 + asc - g.top;
    for (int y = 0; y < g.h; y++)
      for (int x = 0; x < g.w; x++) {
        const int px = ox + x, py = oy + y;
        if (px < 0 || py < 0 || px >= im.w || py >= im.h) continue;
        im.argb[size_t(py) * im.w + px] = u32(g.coverage[size_t(y) * g.w + x] * 17) << 24 | 0xffffff;
      }
  }
  return im;
}

namespace {

struct Be {  // big-endian writer
  std::vector<u8> b;
  void u8_(u32 v) { b.push_back(u8(v)); }
  void u16_(u32 v) { b.push_back(u8(v >> 8)); b.push_back(u8(v)); }
  void s16_(int v) { u16_(u32(u16(s16(v)))); }
  void u32_(u32 v) { u16_(v >> 16); u16_(v & 0xffff); }
  void tag(const char* t) { b.insert(b.end(), t, t + 4); }
  void pad4() { while (b.size() % 4) b.push_back(0); }
};

u32 table_sum(const std::vector<u8>& t) {
  u32 sum = 0;
  for (size_t i = 0; i < t.size(); i += 4) {
    u32 w = 0;
    for (size_t k = 0; k < 4; k++) w = w << 8 | (i + k < t.size() ? t[i + k] : 0);
    sum += w;
  }
  return sum;
}

}  // namespace

std::vector<u8> ttf_bytes(const AssetFont& f) {
  constexpr int P = 64;  // font units per pixel
  const int em = std::max(1, f.height) * P;
  // Glyphs: .notdef, then every present character with a Unicode mapping.
  struct G { int code; std::vector<u8> data; int adv = 0, xmin = 0, ymin = 0, xmax = 0, ymax = 0, points = 0, contours = 0; };
  std::vector<G> glyphs(1);
  glyphs[0].code = -1;
  glyphs[0].adv = em / 2;
  std::vector<std::pair<u16, u16>> cmap;  // unicode -> glyph
  std::vector<bool> used(0x10000, false);
  int asc = 0, desc = 0;
  for (int c = 0; c < 256; c++) {
    const AssetFont::Glyph& g = f.glyphs[size_t(c)];
    const u16 u = f.unicode[size_t(c)];
    if (!g.present || !u || used[u]) continue;
    used[u] = true;
    G out;
    out.code = c;
    out.adv = g.advance * P;
    // Pixels at least half covered, traced into outlines: each filled
    // pixel's edges that border an empty pixel, chained into loops
    // (clockwise around shapes, so holes come out counter-clockwise).
    auto filled = [&](int x, int y) { return x >= 0 && y >= 0 && x < g.w && y < g.h && g.coverage[size_t(y) * g.w + x] >= 8; };
    // Corners are (column, row) of the pixel grid, rows downwards; an edge
    // is kept in `from` -> direction order.
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> edges;  // start corner -> end corners
    for (int y = 0; y < g.h; y++)
      for (int x = 0; x < g.w; x++) {
        if (!filled(x, y)) continue;
        // (Clockwise on screen with y up = with rows downwards: up the left,
        // right along the top, down the right, left along the bottom.)
        if (!filled(x - 1, y)) edges[{x, y + 1}].push_back({x, y});
        if (!filled(x, y - 1)) edges[{x, y}].push_back({x + 1, y});
        if (!filled(x + 1, y)) edges[{x + 1, y}].push_back({x + 1, y + 1});
        if (!filled(x, y + 1)) edges[{x + 1, y + 1}].push_back({x, y + 1});
      }
    std::vector<std::vector<std::pair<int, int>>> loops;
    while (!edges.empty()) {
      auto it = edges.begin();
      const std::pair<int, int> start = it->first;
      std::vector<std::pair<int, int>> loop{start};
      std::pair<int, int> at = start, dir{0, 0};
      for (;;) {
        auto& outs = edges[at];
        // Where two shapes touch at a corner, turn right (stay on this shape).
        size_t pick = 0;
        if (outs.size() > 1) {
          for (size_t k = 0; k < outs.size(); k++) {
            const int dx = outs[k].first - at.first, dy = outs[k].second - at.second;
            if (dx == -dir.second && dy == dir.first) { pick = k; break; }  // (a right turn, rows downwards)
          }
        }
        const std::pair<int, int> next = outs[pick];
        outs.erase(outs.begin() + std::ptrdiff_t(pick));
        if (outs.empty()) edges.erase(at);
        dir = {next.first - at.first, next.second - at.second};
        at = next;
        if (at == start) break;
        loop.push_back(at);
      }
      // Drop points in the middle of straight lines.
      std::vector<std::pair<int, int>> pts;
      const size_t n = loop.size();
      for (size_t k = 0; k < n; k++) {
        const auto& p0 = loop[(k + n - 1) % n];
        const auto& p1 = loop[k];
        const auto& p2 = loop[(k + 1) % n];
        if ((p1.first - p0.first) * (p2.second - p1.second) != (p1.second - p0.second) * (p2.first - p1.first)) pts.push_back(p1);
      }
      if (pts.size() >= 3) loops.push_back(std::move(pts));
    }
    Be d;
    if (!loops.empty()) {
      out.xmin = out.ymin = 1 << 30;
      out.xmax = out.ymax = -(1 << 30);
      std::vector<std::pair<int, int>> seq;  // font units
      std::vector<int> ends;
      for (const auto& loop : loops) {
        for (const auto& [cx, cy] : loop) {
          const int fx = (g.left + cx) * P, fy = (g.top - cy) * P;
          seq.push_back({fx, fy});
          out.xmin = std::min(out.xmin, fx); out.xmax = std::max(out.xmax, fx);
          out.ymin = std::min(out.ymin, fy); out.ymax = std::max(out.ymax, fy);
        }
        ends.push_back(int(seq.size()) - 1);
      }
      out.contours = int(loops.size());
      out.points = int(seq.size());
      d.s16_(out.contours);
      d.s16_(out.xmin); d.s16_(out.ymin); d.s16_(out.xmax); d.s16_(out.ymax);
      for (int e : ends) d.u16_(u32(e));
      d.u16_(0);  // (no instructions)
      for (int i = 0; i < out.points; i++) d.u8_(0x01);  // on-curve; 16-bit deltas
      int px = 0, py = 0;
      for (auto& [x, y] : seq) { d.s16_(x - px); px = x; }
      for (auto& [x, y] : seq) { d.s16_(y - py); py = y; }
      d.pad4();
      asc = std::max(asc, out.ymax);
      desc = std::min(desc, out.ymin);
    }
    out.data = std::move(d.b);
    cmap.push_back({u, u16(glyphs.size())});
    glyphs.push_back(std::move(out));
  }
  std::sort(cmap.begin(), cmap.end());
  asc = std::max(asc, em * 3 / 4);
  const int n = int(glyphs.size());
  int xmin = 0, ymin = 0, xmax = 0, ymax = 0, max_adv = 0, max_pts = 0, max_ctr = 0, min_lsb = 0, min_rsb = 0, max_ext = 0;
  long adv_sum = 0;
  int adv_n = 0;
  for (const G& g : glyphs) {
    max_adv = std::max(max_adv, g.adv);
    if (g.adv) { adv_sum += g.adv; adv_n++; }
    if (!g.contours) continue;
    xmin = std::min(xmin, g.xmin); ymin = std::min(ymin, g.ymin); xmax = std::max(xmax, g.xmax); ymax = std::max(ymax, g.ymax);
    max_pts = std::max(max_pts, g.points); max_ctr = std::max(max_ctr, g.contours);
    min_lsb = std::min(min_lsb, g.xmin); min_rsb = std::min(min_rsb, g.adv - g.xmax); max_ext = std::max(max_ext, g.xmax);
  }
  auto glyph_of = [&](int code) { for (const G& g : glyphs) if (g.code == code) return &g; return static_cast<const G*>(nullptr); };
  std::string ps;
  for (char c : f.full_name.empty() ? std::string("LeapsterFont") : f.full_name)
    if (std::isalnum(u8(c)) || c == '-') ps.push_back(c);
  const std::string family = f.family.empty() ? ps : f.family;

  std::map<std::string, std::vector<u8>> t;
  {  // glyf, loca
    Be glyf, loca;
    for (const G& g : glyphs) { loca.u32_(u32(glyf.b.size())); glyf.b.insert(glyf.b.end(), g.data.begin(), g.data.end()); }
    loca.u32_(u32(glyf.b.size()));
    t["glyf"] = glyf.b;
    t["loca"] = loca.b;
  }
  {  // head
    Be h;
    h.u32_(0x00010000); h.u32_(0x00010000); h.u32_(0); h.u32_(0x5f0f3cf5);
    h.u16_(0x000b); h.u16_(u32(em));
    h.u32_(0); h.u32_(0xbc19'0f80u); h.u32_(0); h.u32_(0xbc19'0f80u);  // created, modified: 2004-01-01 (a fixed date)
    h.s16_(xmin); h.s16_(ymin); h.s16_(xmax); h.s16_(ymax);
    h.u16_(0); h.u16_(8); h.s16_(2); h.s16_(1); h.s16_(0);  // macStyle, lowest ppem, direction, long loca, glyph format
    t["head"] = h.b;
  }
  {  // hhea
    Be h;
    h.u32_(0x00010000); h.s16_(asc); h.s16_(desc); h.s16_(0);
    h.u16_(u32(max_adv)); h.s16_(min_lsb); h.s16_(min_rsb); h.s16_(max_ext);
    h.s16_(1); h.s16_(0); h.s16_(0);
    for (int i = 0; i < 4; i++) h.s16_(0);
    h.s16_(0); h.u16_(u32(n));
    t["hhea"] = h.b;
  }
  {  // hmtx
    Be h;
    for (const G& g : glyphs) { h.u16_(u32(g.adv)); h.s16_(g.contours ? g.xmin : 0); }
    t["hmtx"] = h.b;
  }
  {  // maxp
    Be m;
    m.u32_(0x00010000); m.u16_(u32(n)); m.u16_(u32(max_pts)); m.u16_(u32(max_ctr));
    m.u16_(0); m.u16_(0); m.u16_(2); for (int i = 0; i < 8; i++) m.u16_(0);
    t["maxp"] = m.b;
  }
  {  // cmap: format 4, one segment per character
    Be s;
    const int seg = int(cmap.size()) + 1;
    int sr = 1, es = 0;
    while (sr * 2 <= seg) { sr *= 2; es++; }
    s.u16_(4); s.u16_(u32(16 + 8 * seg)); s.u16_(0);
    s.u16_(u32(2 * seg)); s.u16_(u32(2 * sr)); s.u16_(u32(es)); s.u16_(u32(2 * seg - 2 * sr));
    for (auto& [u, g] : cmap) s.u16_(u);
    s.u16_(0xffff); s.u16_(0);
    for (auto& [u, g] : cmap) s.u16_(u);
    s.u16_(0xffff);
    for (auto& [u, g] : cmap) s.u16_(u32(u16(g - u)));
    s.u16_(1);
    for (int i = 0; i < seg; i++) s.u16_(0);
    Be c;
    c.u16_(0); c.u16_(2);
    c.u16_(0); c.u16_(3); c.u32_(20);  // Unicode BMP
    c.u16_(3); c.u16_(1); c.u32_(20);  // Windows Unicode BMP
    c.b.insert(c.b.end(), s.b.begin(), s.b.end());
    t["cmap"] = c.b;
  }
  {  // name
    const std::vector<std::pair<int, std::string>> names = {
        {1, family}, {2, "Regular"}, {3, "leapemu: " + ps}, {4, f.full_name.empty() ? family : f.full_name}, {5, "Version 1.0"}, {6, ps}};
    Be strings, nm;
    nm.u16_(0); nm.u16_(u32(names.size())); nm.u16_(u32(6 + 12 * names.size()));
    for (auto& [id, str] : names) {
      const size_t at = strings.b.size();
      for (char ch : str) strings.u16_(u8(ch));
      nm.u16_(3); nm.u16_(1); nm.u16_(0x409); nm.u16_(u32(id)); nm.u16_(u32(strings.b.size() - at)); nm.u16_(u32(at));
    }
    nm.b.insert(nm.b.end(), strings.b.begin(), strings.b.end());
    t["name"] = nm.b;
  }
  {  // OS/2 (version 4)
    const G* x = glyph_of('x');
    const G* H = glyph_of('H');
    Be o;
    o.u16_(4); o.s16_(int(adv_n ? adv_sum / adv_n : em / 2)); o.u16_(400); o.u16_(5); o.u16_(0);
    o.s16_(em * 65 / 100); o.s16_(em * 70 / 100); o.s16_(0); o.s16_(em * 14 / 100);
    o.s16_(em * 65 / 100); o.s16_(em * 70 / 100); o.s16_(0); o.s16_(em * 48 / 100);
    o.s16_(P); o.s16_(em * 30 / 100); o.s16_(0);
    for (int i = 0; i < 10; i++) o.u8_(0);  // panose
    o.u32_(0x00000003); o.u32_(0); o.u32_(0); o.u32_(0);  // Basic Latin, Latin-1
    o.tag("NONE");
    o.u16_(0x0040);
    o.u16_(cmap.empty() ? 0x20 : cmap.front().first); o.u16_(cmap.empty() ? 0x20 : cmap.back().first);
    o.s16_(asc); o.s16_(desc); o.s16_(0);
    o.u16_(u32(std::max(asc, ymax))); o.u16_(u32(-std::min(desc, ymin)));
    o.u32_(1); o.u32_(0);
    o.s16_(x && x->contours ? x->ymax : em / 2); o.s16_(H && H->contours ? H->ymax : em * 7 / 10);
    o.u16_(0); o.u16_(0x20); o.u16_(1);
    t["OS/2"] = o.b;
  }
  {  // post (version 3: no glyph names)
    Be p;
    p.u32_(0x00030000); p.u32_(0); p.s16_(-P); p.s16_(P); p.u32_(0); p.u32_(0); p.u32_(0); p.u32_(0); p.u32_(0);
    t["post"] = p.b;
  }
  // The file: table directory (tags in order), then the tables.
  Be file;
  const u16 nt = u16(t.size());
  int sr = 1, es = 0;
  while (sr * 2 <= nt) { sr *= 2; es++; }
  file.u32_(0x00010000); file.u16_(nt); file.u16_(u32(16 * sr)); file.u16_(u32(es)); file.u16_(u32(16 * nt - 16 * sr));
  size_t at = 12 + 16 * size_t(nt), head_at = 0;
  for (auto& [tag, data] : t) {  // (std::map: sorted by tag, as required)
    file.tag(tag.c_str());
    file.u32_(table_sum(data)); file.u32_(u32(at)); file.u32_(u32(data.size()));
    if (tag == "head") head_at = at;
    at += (data.size() + 3) & ~size_t(3);
  }
  for (auto& [tag, data] : t) { file.b.insert(file.b.end(), data.begin(), data.end()); file.pad4(); }
  const u32 adjust = 0xb1b0afbau - table_sum(file.b);
  for (int k = 0; k < 4; k++) file.b[head_at + 8 + size_t(k)] = u8(adjust >> (24 - 8 * k));
  return file.b;
}

}  // namespace leap
