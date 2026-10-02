#include "core/image.h"

#include <fstream>
#include <vector>

#include "core/rom.h"

namespace leap {

namespace {

void be32(std::vector<u8>& v, u32 x) {
  v.push_back(u8(x >> 24));
  v.push_back(u8(x >> 16));
  v.push_back(u8(x >> 8));
  v.push_back(u8(x));
}

void chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data) {
  be32(out, u32(data.size()));
  std::vector<u8> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  be32(out, crc32(body.data(), body.size()));
}

}  // namespace

bool write_png(const std::string& path, const u32* px, int w, int h) {
  // Raw scanlines with filter byte 0.
  std::vector<u8> raw;
  raw.reserve(size_t(h) * (1 + size_t(w) * 3));
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      const u32 p = px[size_t(y) * w + x];
      raw.push_back(u8(p >> 16));
      raw.push_back(u8(p >> 8));
      raw.push_back(u8(p));
    }
  }
  // zlib stream of stored blocks.
  std::vector<u8> z = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(u8(n));
    z.push_back(u8(n >> 8));
    z.push_back(u8(~n));
    z.push_back(u8(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  } while (pos < raw.size());
  u32 a = 1, b = 0;
  for (u8 c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  be32(z, (b << 16) | a);

  std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<u8> ihdr;
  be32(ihdr, u32(w));
  be32(ihdr, u32(h));
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, RGB
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});

  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  return bool(f);
}

}  // namespace leap
