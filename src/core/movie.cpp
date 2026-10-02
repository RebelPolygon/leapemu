#include "core/movie.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "core/fileio.h"

namespace leap {

namespace {

struct Column { char letter; u32 button; };
constexpr Column kColumns[] = {
    {'U', kBtnUp},       {'D', kBtnDown},     {'L', kBtnLeft},       {'R', kBtnRight},       {'A', kBtnA},
    {'B', kBtnB},        {'H', kBtnHint},     {'P', kBtnPause},      {'E', kBtnHome},        {'v', kBtnVolDown},
    {'V', kBtnVolUp},    {'k', kBtnBrightDown}, {'K', kBtnBrightUp}, {'c', kBtnContrast},
};
constexpr char kPowerColumn = 'O';  // after the buttons: power switch off
constexpr int kNumColumns = int(sizeof(kColumns) / sizeof(kColumns[0]));

template <size_t N>
std::string to_hex(const std::array<u8, N>& a) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(N * 2);
  for (u8 b : a) { s += d[b >> 4]; s += d[b & 15]; }
  return s;
}

template <size_t N>
bool from_hex(const std::string& s, std::array<u8, N>& a) {
  if (s.size() != N * 2) return false;
  auto nib = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
  for (size_t i = 0; i < N; i++) {
    const int h = nib(s[2 * i]), l = nib(s[2 * i + 1]);
    if (h < 0 || l < 0) return false;
    a[i] = u8(h << 4 | l);
  }
  return true;
}

}  // namespace

void apply_input(Machine& m, const InputFrame& in) {
  m.set_buttons(in.buttons);
  m.set_power_switch(!in.power_off);
  if (!m.calibrating() || in.touch) m.set_touch(in.touch, in.x, in.y);
}

std::string Movie::encode(const InputFrame& in) {
  std::string s;
  for (const Column& c : kColumns) s += (in.buttons & c.button) ? c.letter : '.';
  s += in.power_off ? kPowerColumn : '.';
  s += '|';
  if (in.touch) s += std::to_string(in.x) + "," + std::to_string(in.y);
  return s;
}

bool Movie::decode(const std::string& line, InputFrame& out) {
  out = InputFrame{};
  const size_t bar = line.find('|');
  if (bar != size_t(kNumColumns) && bar != size_t(kNumColumns) + 1) return false;
  for (int i = 0; i < kNumColumns; i++) {
    const char ch = line[size_t(i)];
    if (ch == kColumns[i].letter) out.buttons |= kColumns[i].button;
    else if (ch != '.' && ch != ' ') return false;
  }
  if (bar == size_t(kNumColumns) + 1) {
    const char ch = line[size_t(kNumColumns)];
    if (ch == kPowerColumn) out.power_off = true;
    else if (ch != '.' && ch != ' ') return false;
  }
  const std::string touch = line.substr(bar + 1);
  if (touch.empty() || touch.find_first_not_of(" \r") == std::string::npos) return true;
  int x = -1, y = -1;
  if (std::sscanf(touch.c_str(), "%d,%d", &x, &y) != 2 || x < 0 || x >= Machine::kWidth || y < 0 || y >= Machine::kHeight) return false;
  out.touch = true;
  out.x = u8(x);
  out.y = u8(y);
  return true;
}

Movie Movie::from_machine(Machine& m) {
  Movie mv;
  mv.bios_crc = m.bios_crc();
  mv.cart_crc = m.has_cart() ? m.cart_crc() : 0;
  mv.cart_patches = m.has_cart() ? m.cart_patch().crc : 0;
  mv.cart_title = m.cart_header().title;
  mv.timing = m.timing;
  mv.idle_skip = m.cpu().idle_skip;
  mv.allow_unsigned = m.allow_unsigned;
  mv.stub_missing_services = m.stub_missing_services;
  mv.auto_calibrate = m.auto_calibrate;
  mv.system_eeprom = m.system_eeprom();
  mv.cart_eeprom = m.cart_eeprom();
  return mv;
}

bool Movie::start(Machine& m, std::string* err) const {
  char buf[128];
  if (m.bios_crc() != bios_crc) {
    std::snprintf(buf, sizeof(buf), "the movie needs BaseROM %08x (loaded: %08x)", bios_crc, m.bios_crc());
    *err = buf;
    return false;
  }
  const u32 cart = m.has_cart() ? m.cart_crc() : 0;
  if (cart != cart_crc) {
    std::snprintf(buf, sizeof(buf), "the movie needs cartridge %08x%s%s (loaded: %08x)", cart_crc, cart_title.empty() ? "" : ", ",
                  cart_title.c_str(), cart);
    *err = buf;
    return false;
  }
  const u32 patches = m.has_cart() ? m.cart_patch().crc : 0;
  if (patches != cart_patches) {
    if (!cart_patches) std::snprintf(buf, sizeof(buf), "the movie was made without ROM patches: turn them off");
    else std::snprintf(buf, sizeof(buf), "the movie needs the ROM patches it was made with (%08x; active: %08x)", cart_patches, patches);
    *err = buf;
    return false;
  }
  m.timing = timing;
  m.apply_timing();
  m.cpu().idle_skip = idle_skip;
  m.allow_unsigned = allow_unsigned;
  m.stub_missing_services = stub_missing_services;
  m.auto_calibrate = auto_calibrate;
  m.system_eeprom() = system_eeprom;
  m.cart_eeprom() = cart_eeprom;
  m.cpu().clear_stop();
  m.reset();
  return true;
}

bool Movie::save(const std::string& path, std::string* err) const {
  std::ostringstream o;
  char buf[64];
  o << "leapemu-movie 1\n";
  std::snprintf(buf, sizeof(buf), "%08x", bios_crc);
  o << "bios_crc " << buf << "\n";
  std::snprintf(buf, sizeof(buf), "%08x", cart_crc);
  o << "cart_crc " << buf << "\n";
  if (!cart_title.empty()) o << "cart_title " << cart_title << "\n";
  if (cart_patches) {
    std::snprintf(buf, sizeof(buf), "%08x", cart_patches);
    o << "cart_patches " << buf << "\n";
  }
  const Machine::Timing& t = timing;
  o << std::setprecision(9);
  o << "timing " << t.rom16 << ' ' << t.rom32 << ' ' << t.cart16 << ' ' << t.cart32 << ' ' << t.ram16 << ' ' << t.ram32 << ' ' << t.sram16
    << ' ' << t.sram32 << ' ' << t.io16 << ' ' << t.io32 << ' ' << t.icache_kb << ' ' << t.dcache_kb << ' ' << t.line_bytes << ' '
    << t.ways << ' ' << t.rom_fill16 << ' ' << t.ram_fill16 << "\n";
  o << "idle_skip " << int(idle_skip) << "\n";
  o << "allow_unsigned " << int(allow_unsigned) << "\n";
  if (stub_missing_services) o << "stub_missing_services 1\n";
  o << "auto_calibrate " << int(auto_calibrate) << "\n";
  o << "system_eeprom " << to_hex(system_eeprom) << "\n";
  o << "cart_eeprom " << to_hex(cart_eeprom) << "\n";
  o << "rerecords " << rerecords << "\n";
  if (!author.empty()) o << "author " << author << "\n";
  if (!comment.empty()) o << "comment " << comment << "\n";
  o << "input\n";
  for (const InputFrame& f : frames) o << encode(f) << "\n";
  const std::string text = o.str();

  return write_file_atomic(path, text, err);
}

bool Movie::load(const std::string& path, std::string* err) {
  std::ifstream f(path);
  if (!f) { *err = "cannot open " + path; return false; }
  *this = Movie{};
  std::string line;
  if (!std::getline(f, line) || line.rfind("leapemu-movie ", 0) != 0) { *err = path + " is not a leapemu movie"; return false; }
  if (std::atoi(line.c_str() + 14) != 1) { *err = path + ": unsupported movie version"; return false; }
  bool in_input = false;
  int lineno = 1;
  while (std::getline(f, line)) {
    lineno++;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (in_input) {
      if (line.empty()) continue;
      InputFrame in;
      if (!decode(line, in)) { *err = path + ":" + std::to_string(lineno) + ": bad input line"; return false; }
      frames.push_back(in);
      continue;
    }
    if (line == "input") { in_input = true; continue; }
    const size_t sp = line.find(' ');
    const std::string key = line.substr(0, sp), val = sp == std::string::npos ? "" : line.substr(sp + 1);
    bool ok = true;
    if (key == "bios_crc") bios_crc = u32(std::strtoul(val.c_str(), nullptr, 16));
    else if (key == "cart_crc") cart_crc = u32(std::strtoul(val.c_str(), nullptr, 16));
    else if (key == "cart_title") cart_title = val;
    else if (key == "cart_patches") cart_patches = u32(std::strtoul(val.c_str(), nullptr, 16));
    else if (key == "timing") {
      Machine::Timing& t = timing;
      std::istringstream in(val);
      ok = bool(in >> t.rom16 >> t.rom32 >> t.cart16 >> t.cart32 >> t.ram16 >> t.ram32 >> t.sram16 >> t.sram32 >> t.io16 >> t.io32 >>
                t.icache_kb >> t.dcache_kb >> t.line_bytes >> t.ways >> t.rom_fill16 >> t.ram_fill16);
    } else if (key == "idle_skip") idle_skip = val == "1";
    else if (key == "allow_unsigned") allow_unsigned = val == "1";
    else if (key == "stub_missing_services") stub_missing_services = val == "1";
    else if (key == "auto_calibrate") auto_calibrate = val == "1";
    else if (key == "system_eeprom") ok = from_hex(val, system_eeprom);
    else if (key == "cart_eeprom") ok = from_hex(val, cart_eeprom);
    else if (key == "rerecords") rerecords = std::strtoull(val.c_str(), nullptr, 10);
    else if (key == "author") author = val;
    else if (key == "comment") comment = val;
    // Unknown keys are ignored (newer versions may add some).
    if (!ok) { *err = path + ":" + std::to_string(lineno) + ": bad " + key; return false; }
  }
  if (!in_input) { *err = path + ": no input section"; return false; }
  return true;
}

}  // namespace leap
