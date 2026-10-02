#pragma once

// A game's ROM window (game list > right-click, or File > Game Properties):
//   - Info: the file and what its ROM header says;
//   - Contents: the header's tables and the game's assets (Flash movies,
//     sounds, speech, ...), which can be exported;
//   - Hex & Patches: the image in hex, edited through patches (core/patch.h)
//     that are kept apart from it and can be switched on and off.

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/celp.h"
#include "core/patch.h"
#include "core/rom.h"
#include "gui/gamelist.h"

namespace leap {

class SoundtrackDecoder;

struct RomWindowHost {
  std::function<void(const std::string& path, bool allow_unsigned)> play;
  std::function<bool(u32 cart_crc)> running;           // is this game running?
  std::function<void()> restart;                        // reload the running game
  std::function<void(std::function<void(const std::string&)>)> pick_folder;
  // A "save as" dialog, starting at `file_name` in the last folder used.
  std::function<void(const std::string& file_name, std::function<void(const std::string&)>)> pick_save;
  // Listening to an asset (one at a time); `audio_left` is 0 when done.
  std::function<void(const std::vector<s16>& pcm, unsigned rate)> play_audio;
  std::function<void()> stop_audio;
  std::function<double()> audio_left;  // seconds
  // A texture for ImGui::Image (an ImTextureID), from 0xAARRGGBB pixels.
  std::function<u64(const std::vector<u32>& argb, int w, int h)> make_texture;
  std::function<void(u64)> free_texture;
  std::function<const std::vector<u8>*()> bios;         // for speech and soundtracks (may be null)
  // Your compatibility rating ("": leapemu's); updates the entry.
  std::function<void(GameEntry& g, const std::string& tier)> set_rating;
  std::function<std::string(const std::string& rom)> save_path;  // where its .sav is (or would be)
  std::function<void(const std::string& folder)> open_folder;      // in the file manager
  std::filesystem::path patch_dir;
};

// Where a game's patches are kept.
std::filesystem::path patch_file_path(const std::filesystem::path& dir, u32 cart_crc);

class RomWindow {
 public:
  enum class Tab { None, Info, Contents, Hex };
  RomWindow(GameEntry entry, RomWindowHost* host, Tab tab);
  ~RomWindow();
  RomWindow(const RomWindow&) = delete;
  RomWindow& operator=(const RomWindow&) = delete;

  bool draw();  // false once closed
  bool focused() const { return focused_; }  // has the keyboard (last drawn)
  const std::string& path() const { return e_.path; }
  void show(Tab tab) { want_tab_ = tab; focus_ = true; }

 private:
  struct UiPatch {
    std::string name;
    bool enabled = true;
    std::map<u32, u8> bytes;          // offset -> new value (only where it differs)
    std::vector<RomEdit> foreign;     // edits that do not match this image, kept as they are
  };

  bool load_image();
  void load_patches();
  void save_patches();
  void rebuild_overlay();
  u8 shown(u32 off) const;
  const RomAsset* asset_at(u32 off) const;
  void set_byte(u32 off, u8 v);
  void revert_byte(u32 off);
  void goto_offset(u32 off);
  void search(bool from_start);

  void draw_info();
  void draw_contents();
  void draw_hex();
  void draw_patch_bar();
  void export_assets(std::vector<RomAsset> which, const std::string& dir, bool raw);
  void save_asset_as(const RomAsset& a, bool raw);
  void play(size_t asset_index);
  void preview(size_t asset_index);
  void draw_preview();
  CelpDecoder* speech();
  SoundtrackDecoder* soundtracks();  // (nullptr without a BIOS, or none in this game)

  GameEntry e_;
  bool focused_ = false;
  // Where the game's save is (the lookup tests whether the ROM's folder is
  // writable): refreshed at most once a second.
  std::string save_path_;
  double save_path_at_ = -1e9;  // (has the keyboard: hex editing must not press the game's buttons)
  RomWindowHost* host_;
  std::string title_;
  bool open_ = true, focus_ = true;
  Tab want_tab_;
  bool image_tried_ = false;
  std::string image_err_;
  std::vector<u8> img_;
  unsigned blocks_joined_ = 0;  // (join_rom_blocks)
  RomHeader header_;
  RomContents contents_;
  std::vector<RomAsset> by_offset_;  // assets sorted by offset

  // Patches.
  std::vector<UiPatch> patches_;
  int active_ = -1;                  // the patch edits go into
  std::map<u32, std::pair<u8, int>> overlay_;  // offset -> shown value, patch index
  std::string patch_err_;
  bool dirty_since_start_ = false;   // patches changed while this game runs
  int renaming_ = -1;
  char rename_buf_[128] = {};

  // Hex view.
  u32 cursor_ = 0;
  bool low_nibble_ = false, scroll_to_cursor_ = false, show_addresses_ = true;
  char goto_buf_[32] = {}, search_buf_[128] = {};
  std::string search_msg_;

  // Contents.
  u32 selected_asset_ = ~0u;
  size_t playing_ = SIZE_MAX;   // the asset being listened to
  double play_length_ = 0;
  std::string play_err_;
  bool export_raw_ = false;
  size_t previewing_ = SIZE_MAX;  // the bitmap shown in the preview window
  u64 preview_tex_ = 0;
  int preview_w_ = 0, preview_h_ = 0;
  std::string preview_err_;
  std::unique_ptr<CelpDecoder> speech_;
  std::unique_ptr<SoundtrackDecoder> soundtracks_;
  bool soundtracks_tried_ = false;

  // Export (in the background).
  std::thread export_thread_;
  std::atomic<unsigned> export_done_{0}, export_total_{0};
  std::atomic<bool> exporting_{false}, export_stop_{false};
  mutable std::mutex export_mu_;
  std::string export_result_;
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);  // (for folder-picker callbacks)
};

}  // namespace leap
