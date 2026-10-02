#include "gui/gamelist.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/rom.h"
#include "gui/game_db.h"

namespace fs = std::filesystem;

namespace leap {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

// "Sonic X (USA).zip" -> name "Sonic X", region "USA".
void from_file_name(const fs::path& p, std::string* name, std::string* region) {
  std::string stem = p.stem().string();
  std::string reg;
  while (!stem.empty() && stem.back() == ')') {
    const size_t open = stem.rfind('(');
    if (open == std::string::npos) break;
    if (reg.empty()) reg = stem.substr(open + 1, stem.size() - open - 2);
    stem = stem.substr(0, open);
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
  }
  *name = stem.empty() ? p.stem().string() : stem;
  *region = reg;
}

// The Name="..." of a Leapster 2 package's meta.inf beside the file.
std::string package_name(const fs::path& p) {
  std::ifstream meta(p.parent_path() / "meta.inf");
  for (std::string line; std::getline(meta, line);)
    if (line.rfind("Name=\"", 0) == 0) {
      const size_t end = line.find('"', 6);
      if (end != std::string::npos && end > 6) return line.substr(6, end - 6);
    }
  return {};
}

// Name, region, type and compatibility: the database's, else our own guess.
void describe(GameEntry& e) {
  for (const GameDbEntry& d : kGameDb)
    if (d.crc == e.crc) {
      e.name = d.name;
      e.region = d.region;
      e.type = d.type;
      e.compatibility = d.tier;
      e.notes = d.notes;
      e.needs_unsigned = game_db_flag(d.flags, "unsigned");
      e.stub_services = game_db_flag(d.flags, "stub-services");
      e.in_database = true;
      return;
    }
  from_file_name(e.path, &e.name, &e.region);
  if (e.download) {
    const std::string n = package_name(e.path);
    if (!n.empty()) e.name = n;
  }
  e.type = e.download ? "Download" : e.kind.empty() ? "Cartridge" : e.kind;
  if (!e.kind_reason.empty()) e.notes = "Not in leapemu's database; it looks like " + e.kind_reason + ".";
  e.compatibility = "Untested";
}

// Whether `p` is a file the scan of `dir` would list.
bool listed_by(const fs::path& p, const fs::path& dir, bool recursive) {
  if (!recursive) return p.parent_path() == dir;
  auto d = dir.begin(), q = p.begin();
  for (; d != dir.end() && q != p.end(); ++d, ++q)
    if (*d != *q) return false;
  return d == dir.end() && q != p.end();
}

// What a cartridge image not in the database is likely to be, from marks in the
// image itself (each seen in known prototypes, and in none of the retail dumps):
//  - a build tool other than LeapFrog's ToolPad: homebrew;
//  - a development upload (a block header at 4 MiB, see join_rom_blocks);
//  - an image not padded to a ROM chip's size (a build output, not a chip dump);
//  - a placeholder part number, or none of the 152- cartridge part numbers
//    every retail cartridge carries;
//  - a version string saying not to ship it.
void guess_kind(const RomHeader& h, std::vector<u8>* img, GameEntry* e) {
  if (h.build_tool.rfind("ToolPad", 0) != 0) {
    e->kind = "Homebrew";
    e->kind_reason = "homebrew (not built with LeapFrog's tools)";
    return;
  }
  std::vector<std::string> why;
  const size_t size = img->size();  // (as dumped: a development upload can be padded)
  if (join_rom_blocks(img)) why.push_back("a development upload (a block header at 4 MiB)");
  if (size & (size - 1)) why.push_back("not the size of a ROM chip");
  if (h.part_number == "123-4567") why.push_back("a placeholder part number");
  else if (h.part_number.find("152-") == std::string::npos) why.push_back("no cartridge part number");
  if (lower(h.version).find("not ship") != std::string::npos) why.push_back("marked not to ship");
  if (why.empty()) return;
  e->kind = "Prototype";
  e->kind_reason = "a prototype: ";
  for (size_t i = 0; i < why.size(); i++) e->kind_reason += (i ? ", " : "") + why[i];
}

// What a file holds, if it is a Leapster game. A Leapster image has
// LeapFrog's header at 0x100: that alone is looked at first, so that other
// systems' images (a CD image's .bin, say) are never read whole.
bool read_rom(const std::string& path, GameEntry* e) {
  std::vector<u8> img;
  std::string err;
  const bool leapster = peek_rom_file(path, 0x200, &img, &err) && img.size() >= 4 &&
                        std::memcmp(img.data(), "JUMP", 4) != 0 &&  // (a Leapster 2 system program)
                        parse_rom_header(img).valid;
  if (!leapster || !load_rom_file(path, &img, &err)) return false;
  const RomHeader h = parse_rom_header(img);
  if (!h.valid || h.title.find("BaseROM") != std::string::npos) return false;  // (not a BIOS)
  e->crc = crc32(img.data(), img.size());
  e->internal_title = h.title;
  e->part_number = h.part_number;
  e->version = h.version;
  e->build_date = h.build_date;
  e->device_start = h.device_start;
  e->device_end = h.device_end;
  e->download = h.device_start >= 0x3c00'0000u && h.device_start < 0x4000'0000u;
  if (!e->download) guess_kind(h, &img, e);
  return true;
}

// Separator-free fields for the cache file.
std::string clean(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

}  // namespace

bool database_has_flag(u32 crc, const char* flag) { return game_db_has_flag(crc, flag); }

bool read_game_entry(const std::string& path, GameEntry* e) {
  *e = GameEntry{};
  if (!read_rom(path, e)) return false;
  std::error_code ec;
  e->path = path;
  e->size = fs::file_size(path, ec);
  e->format = lower(fs::path(path).extension().string()) == ".zip" ? "ZIP" : "BIN";
  describe(*e);
  return true;
}

GameList::GameList(fs::path cache_file) : cache_file_(std::move(cache_file)) { load_cache(); }

GameList::~GameList() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void GameList::refresh(const std::vector<std::string>& folders, bool recursive) {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  stop_ = false;
  scanning_ = true;
  thread_ = std::thread([this, folders, recursive] { scan(folders, recursive); });
}

std::vector<GameEntry> GameList::entries() const {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_;
}

void GameList::scan(std::vector<std::string> folders, bool recursive) {
  std::vector<GameEntry> found;
  std::vector<fs::path> files;
  std::vector<fs::path> listed;  // folders read completely (not missing or unplugged)
  for (const std::string& folder : folders) {
    std::error_code ec;
    auto consider = [&](const fs::directory_entry& de) {
      std::error_code e2;
      if (!de.is_regular_file(e2)) return;
      const std::string ext = lower(de.path().extension().string());
      if (ext == ".zip" || ext == ".bin") files.push_back(de.path());
    };
    if (recursive) {
      for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
           !ec && it != fs::recursive_directory_iterator() && !stop_; it.increment(ec))
        consider(*it);
    } else {
      for (auto it = fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
           !ec && it != fs::directory_iterator() && !stop_; it.increment(ec))
        consider(*it);
    }
    if (!ec) {
      fs::path d = fs::path(folder).lexically_normal();
      if (!d.has_filename()) d = d.parent_path();  // (no trailing separator)
      listed.push_back(d);
    }
  }
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());

  std::map<std::string, Cached> cache;
  {
    std::lock_guard<std::mutex> lock(mu_);
    cache = cache_;
  }
  for (const fs::path& p : files) {
    if (stop_) break;
    std::error_code ec;
    const u64 size = fs::file_size(p, ec);
    const s64 mtime = s64(fs::last_write_time(p, ec).time_since_epoch().count());
    const std::string key = p.string();
    auto it = cache.find(key);
    if (it == cache.end() || it->second.size != size || it->second.mtime != mtime) {
      Cached c;
      c.size = size;
      c.mtime = mtime;
      c.game = read_rom(key, &c.e);
      it = cache.insert_or_assign(key, c).first;
    }
    if (!it->second.game) continue;
    GameEntry e = it->second.e;
    e.path = key;
    e.size = size;
    e.format = lower(p.extension().string()) == ".zip" ? "ZIP" : "BIN";
    describe(e);
    found.push_back(std::move(e));
  }
  // Forget files that are gone from the folders just read. Other entries stay
  // (a folder removed from the list for now, a drive not plugged in).
  if (!stop_)
    for (auto it = cache.begin(); it != cache.end();) {
      const fs::path p = fs::path(it->first).lexically_normal();
      const bool gone = !std::binary_search(files.begin(), files.end(), fs::path(it->first)) &&
                        std::any_of(listed.begin(), listed.end(),
                                    [&](const fs::path& d) { return listed_by(p, d, recursive); });
      it = gone ? cache.erase(it) : std::next(it);
    }
  if (!stop_) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      entries_ = std::move(found);
      cache_ = std::move(cache);
    }
    save_cache();
    version_++;
  }
  scanning_ = false;
}

// One line per file: path, size, mtime, game (0/1), crc, internal title, part
// number, version, build date, device start, device end, download (0/1), kind,
// kind reason.
void GameList::load_cache() {
  std::ifstream in(cache_file_);
  for (std::string line; std::getline(in, line);) {
    std::vector<std::string> f;
    std::stringstream ss(line);
    for (std::string x; std::getline(ss, x, '\t');) f.push_back(x);
    if (!line.empty() && line.back() == '\t') f.push_back("");  // (getline drops a last empty field)
    if (f.size() < 14) continue;  // (also an older cache's line: that file is read again)
    Cached c;
    try {
      c.size = std::stoull(f[1]);
      c.mtime = std::stoll(f[2]);
      c.game = f[3] == "1";
      c.e.crc = u32(std::stoul(f[4], nullptr, 16));
      c.e.internal_title = f[5];
      c.e.part_number = f[6];
      c.e.version = f[7];
      c.e.build_date = f[8];
      c.e.device_start = u32(std::stoul(f[9], nullptr, 16));
      c.e.device_end = u32(std::stoul(f[10], nullptr, 16));
      c.e.download = f[11] == "1";
      c.e.kind = f[12];
      c.e.kind_reason = f[13];
    } catch (const std::exception&) {
      continue;  // (a damaged line: that file is simply read again)
    }
    cache_[f[0]] = c;
  }
}

void GameList::save_cache() const {
  std::map<std::string, Cached> cache;
  {
    std::lock_guard<std::mutex> lock(mu_);
    cache = cache_;
  }
  std::error_code ec;
  fs::create_directories(cache_file_.parent_path(), ec);
  const fs::path tmp = cache_file_.string() + ".tmp";
  {
    std::ofstream out(tmp);
    char hex[3][16];
    for (const auto& [path, c] : cache) {
      std::snprintf(hex[0], sizeof(hex[0]), "%08x", c.e.crc);
      std::snprintf(hex[1], sizeof(hex[1]), "%08x", c.e.device_start);
      std::snprintf(hex[2], sizeof(hex[2]), "%08x", c.e.device_end);
      out << clean(path) << '\t' << c.size << '\t' << c.mtime << '\t' << (c.game ? 1 : 0) << '\t' << hex[0] << '\t'
          << clean(c.e.internal_title) << '\t' << clean(c.e.part_number) << '\t' << clean(c.e.version) << '\t'
          << clean(c.e.build_date) << '\t' << hex[1] << '\t' << hex[2] << '\t' << (c.e.download ? 1 : 0) << '\t'
          << clean(c.e.kind) << '\t' << clean(c.e.kind_reason) << '\n';
    }
  }
  fs::rename(tmp, cache_file_, ec);
}

}  // namespace leap
