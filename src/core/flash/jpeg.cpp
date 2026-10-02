#include "core/flash/jpeg.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace leap::swf {

namespace {

constexpr u8 kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                            41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                            30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct Huffman {
  bool defined = false;
  u8 values[256] = {};
  int maxcode[18] = {};   // largest code of each length, -1 if none
  int valptr[17] = {};    // index into values of the first code of each length
  int mincode[17] = {};
  void build(const u8* counts, const u8* vals, int total) {
    std::copy(vals, vals + total, values);
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
      valptr[len] = k;
      mincode[len] = code;
      code += counts[len - 1];
      k += counts[len - 1];
      maxcode[len] = counts[len - 1] ? code - 1 : -1;
      code <<= 1;
    }
    maxcode[17] = 0x7fffffff;
    defined = true;
  }
};

struct Component {
  int id = 0, h = 1, v = 1, tq = 0;
  int td = 0, ta = 0;       // Huffman tables for this scan
  int pred = 0;             // DC predictor
  int bw = 0, bh = 0;       // blocks across / down (whole MCUs)
  std::vector<int> coefs;   // bw*bh blocks of 64 quantized coefficients (natural order)
  std::vector<u8> plane;    // bw*8 x bh*8 samples
  int* at(int bx, int by) { return &coefs[(size_t(by) * size_t(bw) + size_t(bx)) * 64]; }
};

class Decoder {
 public:
  bool run(const std::vector<std::pair<const u8*, size_t>>& parts, int& w, int& h, std::vector<u32>& out);

 private:
  bool segment(const u8* p, size_t n, size_t& pos);
  bool scan(const u8* p, size_t n, size_t& pos);
  // Entropy-coded data.
  int bit();
  int bits(int count);
  int decode(const Huffman& t);
  static int extend(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }
  bool block(Component& c, int* coef);
  // Progressive scans (T.81 G.1.2): DC first/refine, AC first/refine.
  bool dc_first(Component& c, int* coef);
  bool dc_refine(int* coef);
  bool ac_first(Component& c, int* coef);
  bool ac_refine(Component& c, int* coef);
  void idct(const int* coef, const u16* q, u8* dst, int stride);

  u16 qt_[4][64] = {};
  Huffman dc_[4], ac_[4];
  std::vector<Component> comps_;
  int width_ = 0, height_ = 0, hmax_ = 1, vmax_ = 1, restart_ = 0;
  bool frame_ = false, done_ = false, progressive_ = false;
  int ss_ = 0, se_ = 63, ah_ = 0, al_ = 0, eobrun_ = 0;
  // bit reader state
  const u8* d_ = nullptr;
  size_t n_ = 0, i_ = 0;
  u32 acc_ = 0;
  int nbits_ = 0;
  bool marker_ = false;
};

int Decoder::bit() {
  if (nbits_ == 0) {
    u8 byte = 0;
    if (!marker_ && i_ < n_) {
      byte = d_[i_];
      if (byte == 0xff) {
        const u8 next = i_ + 1 < n_ ? d_[i_ + 1] : 0;
        if (next == 0x00) i_ += 2;
        else { marker_ = true; byte = 0; }  // a marker: feed zeros
      } else {
        i_++;
      }
    }
    acc_ = byte;
    nbits_ = 8;
  }
  nbits_--;
  return int((acc_ >> nbits_) & 1);
}

int Decoder::bits(int count) {
  int v = 0;
  for (int k = 0; k < count; k++) v = (v << 1) | bit();
  return v;
}

int Decoder::decode(const Huffman& t) {
  int code = bit(), len = 1;
  while (len <= 16 && code > t.maxcode[len]) { code = (code << 1) | bit(); len++; }
  if (len > 16) return -1;
  return t.values[t.valptr[len] + code - t.mincode[len]];
}

bool Decoder::block(Component& c, int* coef) {
  if (!dc_[c.td].defined || !ac_[c.ta].defined) return false;
  const int t = decode(dc_[c.td]);
  if (t < 0 || t > 16) return false;
  const int diff = t ? extend(bits(t), t) : 0;
  c.pred += diff;
  coef[0] = c.pred;
  for (int k = 1; k < 64;) {
    const int rs = decode(ac_[c.ta]);
    if (rs < 0) return false;
    const int r = rs >> 4, s = rs & 15;
    if (s == 0) {
      if (r != 15) break;  // end of block
      k += 16;
      continue;
    }
    k += r;
    if (k > 63) return false;
    coef[kZigzag[k]] = extend(bits(s), s);
    k++;
  }
  return true;
}

bool Decoder::dc_first(Component& c, int* coef) {
  if (!dc_[c.td].defined) return false;
  const int t = decode(dc_[c.td]);
  if (t < 0 || t > 16) return false;
  c.pred += t ? extend(bits(t), t) : 0;
  coef[0] = c.pred * (1 << al_);
  return true;
}

bool Decoder::dc_refine(int* coef) {
  if (bit()) coef[0] |= 1 << al_;
  return true;
}

bool Decoder::ac_first(Component& c, int* coef) {
  if (eobrun_ > 0) { eobrun_--; return true; }
  if (!ac_[c.ta].defined) return false;
  for (int k = ss_; k <= se_; k++) {
    const int rs = decode(ac_[c.ta]);
    if (rs < 0) return false;
    const int r = rs >> 4, s = rs & 15;
    if (s) {
      k += r;
      if (k > 63) return false;
      coef[kZigzag[k]] = extend(bits(s), s) * (1 << al_);
    } else if (r == 15) {
      k += 15;
    } else {
      eobrun_ = (1 << r) - 1;
      if (r) eobrun_ += bits(r);
      break;
    }
  }
  return true;
}

bool Decoder::ac_refine(Component& c, int* coef) {
  const int p1 = 1 << al_, m1 = -p1;
  auto refine = [&](int& v) {
    if (bit() && (v & p1) == 0) v += v >= 0 ? p1 : m1;
  };
  int k = ss_;
  if (eobrun_ == 0) {
    if (!ac_[c.ta].defined) return false;
    for (; k <= se_; k++) {
      const int rs = decode(ac_[c.ta]);
      if (rs < 0) return false;
      int r = rs >> 4, s = rs & 15;
      if (s) {
        s = bit() ? p1 : m1;  // new coefficients are +-1 at this bit
      } else if (r != 15) {
        eobrun_ = 1 << r;
        if (r) eobrun_ += bits(r);
        break;
      }
      // Skip r zero coefficients (refining the nonzero ones passed over).
      for (; k <= se_; k++) {
        int& v = coef[kZigzag[k]];
        if (v) refine(v);
        else if (--r < 0) break;
      }
      if (s && k <= se_) coef[kZigzag[k]] = s;
    }
  }
  if (eobrun_ > 0) {
    for (; k <= se_; k++) {
      int& v = coef[kZigzag[k]];
      if (v) refine(v);
    }
    eobrun_--;
  }
  return true;
}

void Decoder::idct(const int* coef, const u16* q, u8* dst, int stride) {
  static float cosv[8][8];
  static bool init = false;
  if (!init) {
    for (int x = 0; x < 8; x++)
      for (int u = 0; u < 8; u++)
        cosv[x][u] = float((u == 0 ? std::sqrt(0.5) : 1.0) * std::cos((2 * x + 1) * u * 3.14159265358979 / 16));
    init = true;
  }
  float in[64], tmp[64];
  for (int k = 0; k < 64; k++) in[k] = float(coef[k] * q[k]);  // q is in natural order
  for (int y = 0; y < 8; y++)  // rows: frequency u -> x
    for (int x = 0; x < 8; x++) {
      float s = 0;
      for (int u = 0; u < 8; u++) s += cosv[x][u] * in[y * 8 + u];
      tmp[y * 8 + x] = s / 2;
    }
  for (int x = 0; x < 8; x++)  // columns
    for (int y = 0; y < 8; y++) {
      float s = 0;
      for (int v = 0; v < 8; v++) s += cosv[y][v] * tmp[v * 8 + x];
      const int val = int(std::lround(s / 2 + 128));
      dst[y * stride + x] = u8(std::clamp(val, 0, 255));
    }
}

bool Decoder::scan(const u8* p, size_t n, size_t& pos) {
  if (!frame_ || pos + 2 > n) return false;
  const size_t len = (size_t(p[pos]) << 8) | p[pos + 1];
  if (pos + len > n || len < 6) return false;
  const u8* s = p + pos + 2;
  const int ns = s[0];
  std::vector<Component*> sc;
  for (int k = 0; k < ns; k++) {
    const int id = s[1 + 2 * k], tables = s[2 + 2 * k];
    auto it = std::find_if(comps_.begin(), comps_.end(), [&](const Component& c) { return c.id == id; });
    if (it == comps_.end()) return false;
    it->td = tables >> 4;
    it->ta = tables & 15;
    if (it->td > 3 || it->ta > 3) return false;
    sc.push_back(&*it);
  }
  const int ss = s[1 + 2 * ns], se = s[2 + 2 * ns], ahal = s[3 + 2 * ns];
  if (!progressive_ && (ss != 0 || se != 63 || ahal != 0)) return false;
  ss_ = ss;
  se_ = se;
  ah_ = ahal >> 4;
  al_ = ahal & 15;
  if (progressive_ && (se < ss || se > 63 || (ss == 0) != (se == 0) || (ss > 0 && sc.size() != 1))) return false;
  eobrun_ = 0;
  pos += len;
  // Entropy-coded segment.
  d_ = p;
  n_ = n;
  i_ = pos;
  nbits_ = 0;
  marker_ = false;
  for (auto* c : sc) c->pred = 0;
  const int mcux = (width_ + 8 * hmax_ - 1) / (8 * hmax_), mcuy = (height_ + 8 * vmax_ - 1) / (8 * vmax_);
  int mcus = 0;
  const bool interleaved = sc.size() > 1;
  auto restart = [&]() {
    // Expect RSTn: skip to the marker and reset predictors.
    nbits_ = 0;
    marker_ = false;
    while (i_ + 1 < n_ && !(d_[i_] == 0xff && d_[i_ + 1] >= 0xd0 && d_[i_ + 1] <= 0xd7)) i_++;
    if (i_ + 1 < n_) i_ += 2;
    for (auto* c : sc) c->pred = 0;
    eobrun_ = 0;
  };
  auto decode_block = [&](Component& c, int* coef) {
    if (!progressive_) return block(c, coef);
    if (ss_ == 0) return ah_ ? dc_refine(coef) : dc_first(c, coef);
    return ah_ ? ac_refine(c, coef) : ac_first(c, coef);
  };
  if (interleaved) {
    for (int my = 0; my < mcuy; my++)
      for (int mx = 0; mx < mcux; mx++) {
        if (restart_ && mcus && mcus % restart_ == 0) restart();
        mcus++;
        for (auto* c : sc)
          for (int by = 0; by < c->v; by++)
            for (int bx = 0; bx < c->h; bx++) {
              if (!decode_block(*c, c->at(mx * c->h + bx, my * c->v + by))) return false;
            }
      }
  } else {
    Component* c = sc[0];
    const int bw = (width_ * c->h / hmax_ + 7) / 8, bh = (height_ * c->v / vmax_ + 7) / 8;
    for (int by = 0; by < bh; by++)
      for (int bx = 0; bx < bw; bx++) {
        if (restart_ && mcus && mcus % restart_ == 0) restart();
        mcus++;
        if (!decode_block(*c, c->at(bx, by))) return false;
      }
  }
  // Continue after the entropy-coded data (at the next marker).
  size_t k = i_;
  while (k + 1 < n && !(p[k] == 0xff && p[k + 1] != 0x00 && !(p[k + 1] >= 0xd0 && p[k + 1] <= 0xd7))) k++;
  pos = k;
  done_ = true;
  return true;
}

bool Decoder::segment(const u8* p, size_t n, size_t& pos) {
  // At a marker.
  while (pos < n && p[pos] == 0xff) pos++;  // fill bytes
  if (pos >= n) return false;
  const u8 m = p[pos++];
  if (m == 0xd8 || m == 0xd9 || (m >= 0xd0 && m <= 0xd7) || m == 0x01) return true;  // SOI, EOI, RSTn, TEM
  if (pos + 2 > n) return false;
  const size_t len = (size_t(p[pos]) << 8) | p[pos + 1];
  if (len < 2 || pos + len > n) return false;
  const u8* s = p + pos + 2;
  const size_t sl = len - 2;
  switch (m) {
    case 0xdb:  // DQT
      for (size_t k = 0; k < sl;) {
        const int pq = s[k] >> 4, tq = s[k] & 3;
        k++;
        for (int j = 0; j < 64 && k < sl; j++) {
          const u16 v = pq ? u16((s[k] << 8) | s[k + 1]) : s[k];
          k += pq ? 2 : 1;
          qt_[tq][kZigzag[j]] = v;
        }
      }
      break;
    case 0xc4:  // DHT
      for (size_t k = 0; k + 17 <= sl;) {
        const int tc = s[k] >> 4, th = s[k] & 3;
        const u8* counts = s + k + 1;
        int total = 0;
        for (int j = 0; j < 16; j++) total += counts[j];
        if (total > 256 || k + 17 + size_t(total) > sl) return false;
        (tc ? ac_[th] : dc_[th]).build(counts, s + k + 17, total);
        k += 17 + size_t(total);
      }
      break;
    case 0xc0: case 0xc1: case 0xc2: {  // SOF0/1/2: baseline / extended sequential / progressive
      if (sl < 6 || s[0] != 8) return false;
      progressive_ = m == 0xc2;
      height_ = (s[1] << 8) | s[2];
      width_ = (s[3] << 8) | s[4];
      const int nf = s[5];
      if (!width_ || !height_ || nf < 1 || nf > 4 || sl < size_t(6 + 3 * nf)) return false;
      comps_.assign(size_t(nf), {});
      hmax_ = vmax_ = 1;
      for (int k = 0; k < nf; k++) {
        Component& c = comps_[size_t(k)];
        c.id = s[6 + 3 * k];
        c.h = std::max(1, s[7 + 3 * k] >> 4);
        c.v = std::max(1, s[7 + 3 * k] & 15);
        c.tq = s[8 + 3 * k] & 3;
        hmax_ = std::max(hmax_, c.h);
        vmax_ = std::max(vmax_, c.v);
      }
      const int mcux = (width_ + 8 * hmax_ - 1) / (8 * hmax_), mcuy = (height_ + 8 * vmax_ - 1) / (8 * vmax_);
      for (auto& c : comps_) {
        c.bw = mcux * c.h;
        c.bh = mcuy * c.v;
        c.coefs.assign(size_t(c.bw) * size_t(c.bh) * 64, 0);
        c.plane.assign(size_t(c.bw) * 8 * size_t(c.bh) * 8, 128);
      }
      frame_ = true;
      break;
    }
    case 0xc3: case 0xc5: case 0xc6: case 0xc7: case 0xc9: case 0xca: case 0xcb: case 0xcd: case 0xce: case 0xcf:
      return false;  // lossless, arithmetic: not supported
    case 0xdd:  // DRI
      if (sl >= 2) restart_ = (s[0] << 8) | s[1];
      break;
    case 0xda:  // SOS
      return scan(p, n, pos);
    default:  // APPn, COM, ...
      break;
  }
  pos += len;
  return true;
}

bool Decoder::run(const std::vector<std::pair<const u8*, size_t>>& parts, int& w, int& h, std::vector<u32>& out) {
  for (const auto& [p, n] : parts) {
    size_t pos = 0;
    while (pos < n) {
      if (p[pos] != 0xff) { pos++; continue; }  // resync to a marker
      if (!segment(p, n, pos)) return false;
    }
  }
  if (!done_) return false;
  for (auto& c : comps_)
    for (int by = 0; by < c.bh; by++)
      for (int bx = 0; bx < c.bw; bx++)
        idct(c.at(bx, by), qt_[c.tq], &c.plane[size_t(by * 8) * size_t(c.bw * 8) + size_t(bx * 8)], c.bw * 8);
  w = width_;
  h = height_;
  out.assign(size_t(w) * h, 0);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      auto sample = [&](const Component& c) {
        const int sx = x * c.h / hmax_, sy = y * c.v / vmax_;
        return float(c.plane[size_t(sy) * size_t(c.bw * 8) + size_t(sx)]);
      };
      float r, g, b;
      if (comps_.size() >= 3) {
        const float Y = sample(comps_[0]), cb = sample(comps_[1]) - 128, cr = sample(comps_[2]) - 128;
        r = Y + 1.402f * cr;
        g = Y - 0.344136f * cb - 0.714136f * cr;
        b = Y + 1.772f * cb;
      } else {
        r = g = b = sample(comps_[0]);
      }
      auto c8 = [](float v) { return u32(std::clamp(int(std::lround(v)), 0, 255)); };
      out[size_t(y) * w + x] = 0xff000000u | (c8(r) << 16) | (c8(g) << 8) | c8(b);
    }
  return true;
}

}  // namespace

bool decode_jpeg(const std::vector<std::pair<const u8*, size_t>>& parts, int& w, int& h, std::vector<u32>& argb) {
  Decoder d;
  return d.run(parts, w, h, argb);
}

}  // namespace leap::swf
