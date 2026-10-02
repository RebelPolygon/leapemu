#include "core/codesig.h"

#include <algorithm>

namespace leap {

namespace {

constexpr u64 kP = (u64(1) << 61) - 1;
constexpr u64 kBase = 0x1c3f'5e2b'97a1'0d65 % kP, kCheckBase = 0x0b6d'92c4'7e31'a5f3 % kP;

u64 reduce(u64 x) {  // x < 2^64 -> x mod P
  x = (x & kP) + (x >> 61);
  return x >= kP ? x - kP : x;
}

u64 mul(u64 a, u64 b) {  // a, b < P
#if defined(__SIZEOF_INT128__)
  const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
  return reduce((u64(p) & kP) + u64(p >> 61));
#else
  // a = a1*2^31 + a0, b likewise; 2^62 = 2 and 2^61 = 1 (mod P).
  const u64 a1 = a >> 31, a0 = a & 0x7fff'ffff, b1 = b >> 31, b0 = b & 0x7fff'ffff;
  const u64 mid = a1 * b0 + a0 * b1;
  return reduce(2 * (a1 * b1) + (mid >> 30) + ((mid & 0x3fff'ffff) << 31) + a0 * b0);
#endif
}

u64 hash_with(u64 base, u64 h, const u8* p, size_t n) {
  for (size_t i = 0; i < n; i++) h = reduce(mul(h, base) + p[i] + 1);
  return h;
}

}  // namespace

u64 code_hash(const u8* p, size_t n) { return hash_with(kBase, 0, p, n); }

std::vector<std::vector<size_t>> find_code(const std::vector<u8>& img, std::initializer_list<CodeSig> sigs,
                                           size_t align, size_t max) {
  std::vector<std::vector<size_t>> out(sigs.size());
  // One rolling hash per distinct length: h(i+1) = h(i)*B + (new+1) - (old+1)*B^n.
  struct Window {
    size_t n;
    u64 h;
    std::vector<u64> drop;     // (b+1)*B^n for each byte value b
    std::vector<size_t> sigs;  // indices into `sigs` with this length
  };
  std::vector<Window> windows;
  size_t open = 0;  // signatures still short of `max` matches
  for (size_t k = 0; k < sigs.size(); k++) {
    const CodeSig& s = sigs.begin()[k];
    if (s.len == 0 || s.len > img.size() || max == 0) continue;
    open++;
    auto w = std::find_if(windows.begin(), windows.end(), [&](const Window& x) { return x.n == s.len; });
    if (w == windows.end()) {
      Window nw{s.len, code_hash(img.data(), s.len), std::vector<u64>(256), {}};
      u64 bn = 1;
      for (size_t i = 0; i < nw.n; i++) bn = mul(bn, kBase);
      for (unsigned b = 0; b < 256; b++) nw.drop[b] = mul(b + 1, bn);
      windows.push_back(std::move(nw));
      w = windows.end() - 1;
    }
    w->sigs.push_back(k);
  }
  for (size_t i = 0; open > 0; i++) {
    bool any = false;
    for (Window& w : windows) {
      if (i + w.n > img.size()) continue;
      any = true;
      if (i % align == 0)
        for (size_t k : w.sigs)
          if (w.h == sigs.begin()[k].hash && out[k].size() < max) {
            out[k].push_back(i);
            if (out[k].size() == max) open--;
          }
      if (i + w.n < img.size()) w.h = reduce(mul(w.h, kBase) + img[i + w.n] + 1 + kP - w.drop[img[i]]);
    }
    if (!any) break;
  }
  return out;
}

u64 code_check(const std::vector<u8>& img, size_t at, const std::vector<CodeRun>& runs) {
  u64 h = 0;
  for (const CodeRun& r : runs) {
    if (at + r.off + r.len > img.size()) return 0;
    h = hash_with(kCheckBase, h, img.data() + at + r.off, r.len);
  }
  return h;
}

}  // namespace leap
