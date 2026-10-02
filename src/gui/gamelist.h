#pragma once

// The game list: the Leapster games found in the user's game folders, with
// what their ROM headers say and what leapemu's game database (res/games.tsv)
// knows about them. Folders are scanned on a background thread; what each
// file holds is cached (by path, size and modification time) so that large
// zipped images are read only once.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/common.h"

namespace leap {

struct GameEntry {
  std::string path;
  u64 size = 0;         // file size, bytes
  std::string format;   // "ZIP" / "BIN"
  u32 crc = 0;          // CRC-32 of the ROM data (as the emulator identifies it)
  // From the ROM header.
  std::string internal_title, part_number, version, build_date;
  u32 device_start = 0, device_end = 0;
  bool download = false;  // a Leapster 2 downloadable (linked to run from RAM)
  // What the image itself suggests it is when the database does not know it:
  // "Prototype", "Homebrew" or empty, and why.
  std::string kind, kind_reason;
  // Shown in the list: from the database, else the package's meta.inf (for a
  // download), else the file name.
  std::string name, region, type, compatibility, notes;
  bool needs_unsigned = false;  // boots only with the unsigned patches
  bool stub_services = false;   // needs Machine::stub_missing_services
  bool in_database = false;
  // Your own rating (game list > right-click > Compatibility) replaces the
  // database's, which is kept here.
  std::string database_compatibility;
  bool own_compatibility = false;
};

// The compatibility fixes the database gives the image with this CRC (its
// flags): "unsigned", a dump with damaged bytes that boots only with the
// unsigned patches; "stub-services", a Leapster 2 download that uses
// Leapster 2 services leapemu supplies (Machine::stub_missing_services).
bool database_has_flag(u32 crc, const char* flag);

// The entry for one file, read now (not from the cache).
bool read_game_entry(const std::string& path, GameEntry* e);

class GameList {
 public:
  explicit GameList(std::filesystem::path cache_file);
  ~GameList();
  GameList(const GameList&) = delete;
  GameList& operator=(const GameList&) = delete;

  // Scans `folders` (and their subfolders if `recursive`) in the background;
  // a scan in progress is abandoned.
  void refresh(const std::vector<std::string>& folders, bool recursive);
  bool scanning() const { return scanning_; }
  // The entries found by the latest completed scan; `version` changes with
  // each new result.
  std::vector<GameEntry> entries() const;
  u64 version() const { return version_; }

 private:
  void scan(std::vector<std::string> folders, bool recursive);
  void load_cache();
  void save_cache() const;

  std::filesystem::path cache_file_;
  mutable std::mutex mu_;
  std::vector<GameEntry> entries_;
  struct Cached { u64 size = 0; s64 mtime = 0; bool game = false; GameEntry e; };
  std::map<std::string, Cached> cache_;  // by path (non-games too, to skip them quickly)
  std::thread thread_;
  std::atomic<bool> stop_{false}, scanning_{false};
  std::atomic<u64> version_{0};
};

}  // namespace leap
