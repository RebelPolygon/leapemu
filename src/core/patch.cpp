#include "core/patch.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/fileio.h"
#include "core/rom.h"

namespace leap {

namespace {

std::string to_hex(const std::vector<u8>& v) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (u8 b : v) { s.push_back(d[b >> 4]); s.push_back(d[b & 15]); }
  return s;
}

bool from_hex(const std::string& s, std::vector<u8>* out) {
  if (s.size() % 2) return false;
  out->clear();
  for (size_t i = 0; i < s.size(); i += 2) {
    unsigned v = 0;
    if (std::sscanf(s.c_str() + i, "%2x", &v) != 1 || !std::isxdigit(u8(s[i])) || !std::isxdigit(u8(s[i + 1]))) return false;
    out->push_back(u8(v));
  }
  return true;
}

}  // namespace

bool PatchFile::load(const std::string& path, std::string* err) {
  std::ifstream f(path);
  if (!f) { *err = "cannot open " + path; return false; }
  *this = PatchFile{};
  std::string line;
  if (!std::getline(f, line) || line.rfind("leapemu-patches ", 0) != 0) { *err = path + " is not a leapemu patch file"; return false; }
  int lineno = 1;
  while (std::getline(f, line)) {
    lineno++;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t sp = line.find(' ');
    const std::string key = line.substr(0, sp), val = sp == std::string::npos ? "" : line.substr(sp + 1);
    if (key == "cart_crc") { cart_crc = u32(std::strtoul(val.c_str(), nullptr, 16)); continue; }
    if (key == "cart_title") { cart_title = val; continue; }
    if (key == "patch") {
      RomPatch p;
      const size_t sp2 = val.find(' ');
      const std::string state = val.substr(0, sp2);
      p.enabled = state == "on";
      p.name = sp2 == std::string::npos ? "" : val.substr(sp2 + 1);
      patches.push_back(std::move(p));
      continue;
    }
    // An edit: offset from to.
    std::istringstream in(line);
    std::string off, from, to;
    RomEdit e;
    if (patches.empty() || !(in >> off >> from >> to) || !from_hex(from, &e.from) || !from_hex(to, &e.to) ||
        e.from.size() != e.to.size() || e.from.empty()) {
      *err = path + ":" + std::to_string(lineno) + ": bad line";
      return false;
    }
    e.offset = u32(std::strtoul(off.c_str(), nullptr, 16));
    patches.back().edits.push_back(std::move(e));
  }
  return true;
}

bool PatchFile::save(const std::string& path, std::string* err) const {
  std::ostringstream o;
  char buf[16];
  o << "leapemu-patches 1\n";
  std::snprintf(buf, sizeof(buf), "%08x", cart_crc);
  o << "cart_crc " << buf << "\n";
  if (!cart_title.empty()) o << "cart_title " << cart_title << "\n";
  for (const RomPatch& p : patches) {
    o << "\npatch " << (p.enabled ? "on" : "off") << ' ' << p.name << "\n";
    for (const RomEdit& e : p.edits) {
      std::snprintf(buf, sizeof(buf), "%08x", e.offset);
      o << buf << ' ' << to_hex(e.from) << ' ' << to_hex(e.to) << "\n";
    }
  }
  const std::string text = o.str();
  return write_file_atomic(path, text, err);
}

std::vector<RomEdit> PatchFile::enabled_edits() const {
  std::vector<RomEdit> out;
  for (const RomPatch& p : patches)
    if (p.enabled) out.insert(out.end(), p.edits.begin(), p.edits.end());
  return out;
}

PatchResult apply_edits(std::vector<u8>& image, const std::vector<RomEdit>& edits) {
  PatchResult r;
  std::vector<u8> applied;  // what the CRC covers: each applied edit's offset and bytes
  const std::vector<u8> original = edits.empty() ? std::vector<u8>() : image;
  for (const RomEdit& e : edits) {
    if (e.from.size() != e.to.size() || size_t(e.offset) + e.from.size() > image.size() ||
        std::memcmp(&original[e.offset], e.from.data(), e.from.size()) != 0) {
      r.mismatched++;
      continue;
    }
    std::memcpy(&image[e.offset], e.to.data(), e.to.size());
    r.applied++;
    for (int k = 0; k < 4; k++) applied.push_back(u8(e.offset >> (8 * k)));
    applied.insert(applied.end(), e.to.begin(), e.to.end());
  }
  if (r.applied) r.crc = crc32(applied.data(), applied.size());
  return r;
}

}  // namespace leap
