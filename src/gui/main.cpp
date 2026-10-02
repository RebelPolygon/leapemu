// leapemu: SDL3 + Dear ImGui frontend.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gui/gamelist.h"
#include "gui/romwindow.h"
#include "gui/icon.h"
#include "core/arc/cpu.h"
#include "core/image.h"
#include "core/flash/timeline.h"
#include "core/interp.h"
#include "core/log.h"
#include "core/machine.h"
#include "core/movie.h"
#include "core/rewind.h"
#include "core/scale.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

using namespace leap;
namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Settings persisted as key=value lines in the user's preference directory.
// ---------------------------------------------------------------------------
struct Settings {
  std::map<std::string, std::string> kv;
  fs::path file;

  void load(const fs::path& dir) {
    file = dir / "leapemu.ini";
    std::ifstream f(file);
    std::string line;
    while (std::getline(f, line)) {
      const auto eq = line.find('=');
      if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    saved = kv;
  }
  // Written to a temporary file, then renamed over the old one: a crash or a
  // full disk can't leave half a settings file. Only when something changed.
  std::map<std::string, std::string> saved;
  void save() {
    if (kv == saved && fs::exists(file)) return;
    const fs::path tmp = fs::path(file).concat(".tmp");
    {
      std::ofstream f(tmp);
      for (const auto& [k, v] : kv) f << k << '=' << v << '\n';
      if (!f.good()) return;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (!ec) saved = kv;
  }
  std::string get(const std::string& k, const std::string& def = {}) const {
    auto it = kv.find(k);
    return it == kv.end() ? def : it->second;
  }
};

// File dialog results arrive on another thread.
struct PendingFile {
  std::mutex mu;
  std::string path;
  enum Kind { None, Bios, Cart, CartUnsigned, MoviePlay, MovieRecord, MovieRecordBlank, GameFolder, SavesFolder, ExportFolder, ExportFile } kind = None;
};

// Emulation speeds (Emulation > Speed, - and =).
constexpr double kSpeeds[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};

// Controls: the console's buttons, and the emulator's own actions
// (shortcuts). Each has two key slots and a gamepad button, set in Settings >
// Controls and saved as key_<id> / pad_<id>. An emulator shortcut can need
// modifiers (Ctrl, Shift, Alt); a console button works with Shift held, not
// with Ctrl or Alt.
enum class Act {
  Button, Touch, FastForward, FastForwardToggle, Slower, Faster, Rewind, FrameBack, FrameAdvance, QuickSave, QuickLoad,
  SaveSlot, LoadSlot, Pause, Reset, Stop, Power, Mute, Screenshot, Fullscreen, LoadRom, Settings, Quit
};
enum : u8 { kModCtrl = 1, kModShift = 2, kModAlt = 4 };
struct KeyBind {
  SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
  u8 mods = 0;
  bool operator==(const KeyBind&) const = default;
};
constexpr SDL_Scancode kNoKey = SDL_SCANCODE_UNKNOWN;
constexpr KeyBind kNone{};
constexpr KeyBind K(SDL_Scancode k, u8 mods = 0) { return {k, mods}; }
u8 mods_of(SDL_Keymod m) {
  return u8((m & SDL_KMOD_CTRL ? kModCtrl : 0) | (m & SDL_KMOD_SHIFT ? kModShift : 0) | (m & SDL_KMOD_ALT ? kModAlt : 0));
}
struct Control {
  const char* id;
  const char* label;
  Act act;
  u32 button;                  // Act::Button: the console button; SaveSlot / LoadSlot: the slot
  KeyBind defaults[2];
  SDL_GamepadButton pad = SDL_GAMEPAD_BUTTON_INVALID;  // default gamepad button
};
const Control kControls[] = {
    {"up", "Up", Act::Button, kBtnUp, {K(SDL_SCANCODE_UP), K(SDL_SCANCODE_W)}, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"down", "Down", Act::Button, kBtnDown, {K(SDL_SCANCODE_DOWN), K(SDL_SCANCODE_S)}, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"left", "Left", Act::Button, kBtnLeft, {K(SDL_SCANCODE_LEFT), K(SDL_SCANCODE_A)}, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"right", "Right", Act::Button, kBtnRight, {K(SDL_SCANCODE_RIGHT), K(SDL_SCANCODE_D)}, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"a", "A", Act::Button, kBtnA, {K(SDL_SCANCODE_X), K(SDL_SCANCODE_SPACE)}, SDL_GAMEPAD_BUTTON_SOUTH},
    {"b", "B", Act::Button, kBtnB, {K(SDL_SCANCODE_Z), kNone}, SDL_GAMEPAD_BUTTON_EAST},
    {"hint", "Hint", Act::Button, kBtnHint, {K(SDL_SCANCODE_C), kNone}, SDL_GAMEPAD_BUTTON_WEST},
    {"home", "Home", Act::Button, kBtnHome, {K(SDL_SCANCODE_BACKSPACE), kNone}, SDL_GAMEPAD_BUTTON_BACK},
    {"pause", "Pause", Act::Button, kBtnPause, {K(SDL_SCANCODE_RETURN), kNone}, SDL_GAMEPAD_BUTTON_START},
    // (The gamepad's stylus: presses at the right stick's cursor. The right trigger does too.)
    {"touch", "Stylus (at the cursor)", Act::Touch, 0, {kNone, kNone}, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    // (Rarely used: in Emulation > Console too.)
    {"bright_down", "Brightness down", Act::Button, kBtnBrightDown, {kNone, kNone}},
    {"bright_up", "Brightness up", Act::Button, kBtnBrightUp, {kNone, kNone}},
    {"contrast", "Contrast", Act::Button, kBtnContrast, {kNone, kNone}},
    {"vol_down", "Volume down", Act::Button, kBtnVolDown, {kNone, kNone}},
    {"vol_up", "Volume up", Act::Button, kBtnVolUp, {kNone, kNone}},
    {"pause_emu", "Pause / resume", Act::Pause, 0, {K(SDL_SCANCODE_P, kModCtrl), kNone}},
    {"reset", "Reset", Act::Reset, 0, {K(SDL_SCANCODE_R, kModCtrl), kNone}},
    {"stop", "Stop (close the game)", Act::Stop, 0, {K(SDL_SCANCODE_W, kModCtrl), kNone}},
    {"power", "Power switch", Act::Power, 0, {kNone, kNone}},
    {"fast_forward", "Fast forward (hold)", Act::FastForward, 0, {K(SDL_SCANCODE_TAB), kNone}, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"ff_toggle", "Fast forward (toggle)", Act::FastForwardToggle, 0, {K(SDL_SCANCODE_TAB, kModShift), kNone}},
    {"slower", "Slower", Act::Slower, 0, {K(SDL_SCANCODE_MINUS), kNone}},
    {"faster", "Faster", Act::Faster, 0, {K(SDL_SCANCODE_EQUALS), kNone}},
    {"rewind", "Rewind (hold)", Act::Rewind, 0, {K(SDL_SCANCODE_GRAVE), kNone}, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"frame_back", "Frame back", Act::FrameBack, 0, {K(SDL_SCANCODE_COMMA), kNone}},
    {"frame_advance", "Frame advance", Act::FrameAdvance, 0, {K(SDL_SCANCODE_PERIOD), kNone}},
    {"quick_save", "Quick save", Act::QuickSave, 0, {K(SDL_SCANCODE_F1, kModShift), kNone}},
    {"quick_load", "Quick load", Act::QuickLoad, 0, {K(SDL_SCANCODE_F1), kNone}},
    {"save1", "Save slot 1", Act::SaveSlot, 1, {K(SDL_SCANCODE_F2, kModShift), kNone}},
    {"save2", "Save slot 2", Act::SaveSlot, 2, {K(SDL_SCANCODE_F3, kModShift), kNone}},
    {"save3", "Save slot 3", Act::SaveSlot, 3, {K(SDL_SCANCODE_F4, kModShift), kNone}},
    {"save4", "Save slot 4", Act::SaveSlot, 4, {K(SDL_SCANCODE_F5, kModShift), kNone}},
    {"load1", "Load slot 1", Act::LoadSlot, 1, {K(SDL_SCANCODE_F2), kNone}},
    {"load2", "Load slot 2", Act::LoadSlot, 2, {K(SDL_SCANCODE_F3), kNone}},
    {"load3", "Load slot 3", Act::LoadSlot, 3, {K(SDL_SCANCODE_F4), kNone}},
    {"load4", "Load slot 4", Act::LoadSlot, 4, {K(SDL_SCANCODE_F5), kNone}},
    {"mute", "Mute", Act::Mute, 0, {K(SDL_SCANCODE_M, kModCtrl), kNone}},
    {"screenshot", "Screenshot", Act::Screenshot, 0, {K(SDL_SCANCODE_F12), kNone}},
    {"fullscreen", "Fullscreen", Act::Fullscreen, 0, {K(SDL_SCANCODE_F11), K(SDL_SCANCODE_RETURN, kModAlt)}},
    {"load_rom", "Load ROM", Act::LoadRom, 0, {K(SDL_SCANCODE_O, kModCtrl), kNone}},
    {"settings", "Settings", Act::Settings, 0, {K(SDL_SCANCODE_COMMA, kModCtrl), kNone}},
    {"quit", "Exit", Act::Quit, 0, {K(SDL_SCANCODE_Q, kModCtrl), kNone}},
};
constexpr size_t kControlCount = sizeof(kControls) / sizeof(kControls[0]);

class App {
 public:
  int run(int argc, char** argv);

 private:
  bool init_sdl();
  void shutdown();
  void handle_event(const SDL_Event& e);
  void update_input();
  void update_cursor();
  void toggle_power();  // the power switch: off (the console shuts down), or on again
  void emulate(double dt);
  void draw_ui();
  void draw_menu();
  void render_screen();
  void render_native_layers();
  void render_lcd_tone();
  void render_pad_cursor();
  void update_display_mode();
  void set_touch_from_window(float wx, float wy);
  void toggle_fullscreen();
  void draw_cpu();
  void draw_disasm();
  void draw_memory();
  void draw_uart();
  void draw_status();
  void draw_idle();
  void draw_bios_prompt();
  void draw_welcome();
  std::string waiting_cart_;     // a game asked for before a BaseROM was chosen
  bool waiting_unsigned_ = false;

  bool open_bios(const std::string& path);
  bool open_cart(const std::string& path, bool allow_unsigned = false);
  std::string cart_title_;  // shown in the title and status bars
  void boot_bios();
  void unload_cart();
  void power_cycle();
  void draw_about();
  std::array<std::array<KeyBind, 2>, kControlCount> keys_{};       // the key slots of kControls
  std::array<SDL_GamepadButton, kControlCount> pad_btn_{};          // and their gamepad buttons
  // A slot waiting for a key or gamepad button (Settings > Controls): slots 0-1
  // are keys, 2 the gamepad button.
  int capture_control_ = -1, capture_slot_ = 0;
  void assign_pad(size_t control, SDL_GamepadButton b);
  void do_action(Act act, u32 arg, bool repeat);  // a non-button control pressed (arg: its slot)
  void rewind_released();
  void load_keys();
  void save_keys();
  void assign_key(size_t control, int slot, KeyBind key);
  // The control a key (with exactly these modifiers) belongs to (-1: none).
  int control_of(KeyBind key) const;
  bool held(Act act, const bool* keys) const;
  // Whether a key slot is down now (keyboard state, current modifiers).
  static bool key_down(const KeyBind& k, const bool* keys, u8 mods, bool button);
  // An action's first key, for menus ("" if none).
  std::string shortcut(Act act, u32 arg = 0) const;
  bool ff_toggle_ = false;      // fast forward latched on (its toggle key)
  // Emulation > Console: a console button pressed from the menu, for a moment.
  u32 console_press_ = 0;
  u64 console_press_until_ = 0;
  // Sound: the host volume (0-1) and mute (Settings > Audio & Video, Ctrl+M).
  float volume_ = 1.0f;
  bool muted_ = false;
  void set_muted(bool m);
  void apply_volume();
  // Settings > Emulation: pause and/or mute while the window is in the background.
  bool inactive_pause_ = false, inactive_mute_ = false;
  bool auto_paused_ = false;    // paused by losing focus (resumes on regaining it)
  // Fast forward's speed (0: unbounded).
  double ff_speed_ = 0;
  // Emulation > Stop: the game closes at once (back to the game list).
  void stop_game();
  void store_settings();
  void clear_held_input();
  void screenshot();
  bool show_settings_ = false;
  // A tool window (Settings, a ROM window, the debugger) has the keyboard:
  // its keys don't reach the game. Found while drawing, used next frame.
  bool ui_keys_ = false, ui_keys_next_ = false;
  void claim_keys() { if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) ui_keys_next_ = true; }
  enum class SettingsPage { Paths, Emulation, AudioVideo, Controls, Shortcuts } settings_page_ = SettingsPage::Paths;
  bool vsync_ = true;
  void draw_settings();
  // Recently used files (settings keys <key>0, <key>1, ...), most recent first.
  std::vector<std::string> recent(const char* key) const;
  void add_recent(const char* key, const std::string& value, size_t max);
  static constexpr int kListMax = 64;  // (game folders, recent files)
  void set_list(const char* key, const std::vector<std::string>& list);
  // Saves: the cartridge's EEPROM as a .sav file (next to the ROM, or in the
  // data folder) and the system EEPROM, written within a second of changing.
  fs::path cart_save_path(const std::string& rom) const;
  fs::path saves_dir() const;
  void move_save();
  fs::path system_save_path() const { return nvram_dir_ / "system.eep"; }
  void load_cart_save(const std::string& rom);
  void reload_saves();
  void flush_saves();
  // Tool-assisted play.
  bool step_frame();
  void start_movie(const std::string& path, PendingFile::Kind kind);
  void stop_movie();
  void set_read_only(bool read_only);
  void cycle_speed(int dir);
  fs::path state_path(int slot) const;
  void save_slot(int slot);
  void load_slot(int slot);
  void load_state(const fs::path& path, const std::string& name);
  int pending_scale_ = 0;        // --scale: the window size to set once the bars are measured
  bool fs_menu_ = false;         // fullscreen: the menu bar is showing (the mouse at the top)
  void open_dialog(PendingFile::Kind kind);
  void save_dialog(PendingFile::Kind kind);
  // An error shows for kErrorMs while the game runs (and until something
  // changes while it is stopped), then the status bar returns.
  void set_error(const std::string& msg) { error_ = msg; error_time_ = SDL_GetTicks(); LOG_E("%s", msg.c_str()); }
  static constexpr u64 kErrorMs = 10000;
  u64 error_time_ = 0;
  bool error_shown() const { return !error_.empty() && (!running_ || SDL_GetTicks() - error_time_ < kErrorMs); }
  void draw_toast();  // fullscreen: the latest message or error, over the picture

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* screen_tex_ = nullptr;
  SDL_AudioStream* audio_ = nullptr;
  SDL_AudioStream* preview_audio_ = nullptr;  // ROM window: listening to an asset
  unsigned preview_rate_ = 0;
  SDL_Gamepad* pad_ = nullptr;

  Machine m_;
  Settings settings_;
  fs::path pref_dir_, nvram_dir_;
  PendingFile pending_;

  bool quit_ = false;
  bool running_ = false;  // emulation running (vs paused)
  bool powered_ = false;  // a BIOS is loaded and the machine has been reset
  double speed_ = 1.0;   // 0 = unthrottled
  double audio_ratio_ = 1.0;  // playback rate of the audio stream
  double frame_accum_ = 0;
  u64 last_counter_ = 0;
  double emu_fps_ = 0;
  u64 fps_frames_ = 0;
  double fps_time_ = 0;

  // Input state.
  bool touch_down_ = false;
  int touch_x_ = 0, touch_y_ = 0;
  // The gamepad's stylus: the right stick moves a cursor over the screen (in
  // LCD pixels, from the centre), and the stylus control presses there. The
  // cursor shows while it is used, and fades out after kCursorIdleMs.
  static constexpr u64 kCursorIdleMs = 3000;
  float cursor_x_ = Machine::kWidth / 2.0f, cursor_y_ = Machine::kHeight / 2.0f;
  bool cursor_down_ = false;
  u64 cursor_used_ms_ = 0;  // when it last moved or pressed (0: not yet)
  u64 cursor_ns_ = 0;       // (the last update)
  bool turbo_ = false;  // Tab held: run unthrottled
  u64 rewind_ff_ticks_ = 0;  // (the last rewind snapshot during fast forward)
  InputFrame live_input_;  // this frame's input from the keyboard, gamepad and mouse
  bool switched_off_ = false;  // Emulation > Power Switch Off: the console's power switch
  // Once the console has turned itself off, its cartridge is taken out (the
  // game list shows); Power On puts the same one back and starts it.
  bool console_off_ = false;
  u64 power_off_frame_ = 0, power_off_ticks_ = 0;  // when the switch was slid off
  std::string off_cart_;
  bool off_cart_unsigned_ = false;
  void console_switched_off();
  void power_on_again();
  // Rewind (hold `): snapshots every few frames, stepped back through in real time.
  Rewind rewind_;
  bool rewind_on_ = true;        // Emulation > Rewind
  bool rewinding_ = false;       // the key is held
  bool test_rewind_ = false;     // (automated tests: as if held)
  u32 test_buttons_ = 0;         // (automated tests: buttons held)
  bool test_turbo_ = false;      // (automated tests: fast forward held)
  float test_stick_x_ = 0, test_stick_y_ = 0;  // (the right stick, -1 to 1)
  bool test_stylus_ = false;     // (the stylus control held)
  // LEAPEMU_FRAME_STATS: time spent drawing the screen each frame, printed at exit.
  std::unique_ptr<std::vector<double>> frame_stats_;
  bool rewind_session_ = false;  // rewound since the key went down (one rerecord)
  // Holding the rewind key rewinds; the frame back key steps back one frame.
  u64 rewind_down_ms_ = 0;       // when the key went down (0: not down)
  bool rewind_resume_ = false;   // running when it went down (resumed after a hold)
  bool rewind_held_ = false;     // the press became a hold
  // The input of recent frames (index: frame - input_log_base_), to replay
  // from a snapshot when stepping back a single frame.
  std::deque<InputFrame> input_log_;
  u64 input_log_base_ = 0;
  bool frame_reverse();
  double rewind_accum_ = 0;
  void rewind_step(double dt);
  InputFrame last_input_;  // the input of the last emulated frame (input display)

  // Saves.
  std::string cart_rom_;           // the loaded cartridge's file
  fs::path cart_save_path_;        // where its save is written
  std::array<u8, 2048> cart_saved_{};
  std::array<u8, 512> sys_saved_{};
  bool cart_save_force_ = false;   // write even if unchanged (new location)
  bool sys_loaded_ = false;
  bool saves_beside_rom_ = true;   // Settings > Paths
  bool saves_suspended_ = false;   // a movie runs on its own EEPROM contents
  u64 last_flush_ = 0;

  // Tool-assisted play: input movies, frame advance, counters.
  enum class MovieMode { None, Recording, Playing, Finished };
  Movie movie_;
  MovieMode movie_mode_ = MovieMode::None;
  std::string movie_path_;
  Movie user_settings_;  // the emulation settings before the movie applied its own
  u64 lag_count_ = 0;
  int advance_ = 0;              // frames to advance while paused
  bool show_counters_ = false;   // Tools > Movies > Show Counters
  bool hw_timing_ = true;
  FrameInterpolator interp_;
  bool smoothing_ = false;     // View > Interpolation
  bool vector_scale_ = false;  // View > Vector Scale to Window

  // Screen presentation.
  enum class Filter { Nearest, Bilinear, Sharp, Bicubic, Lanczos, Mmpx, Lcd };
  // Software filters (bicubic, Lanczos, MMPX, LCD): the 160x160 image
  // filtered to an integer size, then drawn smoothly to the screen size.
  void render_filtered(const u32* image, float scale);
  SDL_Texture* filtered_tex_ = nullptr;
  int filtered_w_ = 0;
  std::vector<u32> filter_src_, filter_out_, filter_tmp_;
  int filter_k_ = 0;
  Filter filter_done_ = Filter::Nearest;
  std::vector<float> lcd_ghost_;  // LCD: the slowly responding panel
  u64 lcd_ticks_ = 0;
  Filter filter_ = Filter::Nearest;
  bool integer_scale_ = true;
  bool fullscreen_ = false;
  SDL_Texture* sharp_tex_ = nullptr;
  int sharp_k_ = 0;
  // Native frame interpolation: one texture per layer, reused across frames.
  std::vector<NativeLayer> layers_;
  struct LayerTex {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    u64 version = 0;  // NativeLayer::version of the pixels in tex
    // The layer through a software filter (redone only when its pixels change).
    SDL_Texture* ftex = nullptr;
    int fw = 0, fh = 0, fk = 0;
    u64 fhash = 0;
    Filter ffilter = Filter::Nearest;
  };
  SDL_Texture* grid_tex_ = nullptr;  // LCD gaps over the redrawn layers
  int grid_k_ = 0;
  std::vector<LayerTex> layer_tex_;
  bool native_shown_ = false;  // the last frame was drawn from native layers
  // The interpolator redraws from the ROM images: after loading another ROM
  // or BIOS it must be given the new ones before it draws anything again.
  void sync_rom_view();
  const u8* rom_bios_ = nullptr;  // ROM images the interpolator was given
  const u8* rom_cart_ = nullptr;
  size_t rom_cart_size_ = 0;
  SDL_FRect screen_rect_{};  // in render-output pixels
  float menu_h_ = 0, status_h_ = 0;  // in window coordinates
  u64 total_frames_ = 0;
  u64 start_ticks_ = 0;

  // UI state.
  bool show_cpu_ = false, show_disasm_ = false, show_memory_ = false, show_uart_ = false;
  bool show_about_ = false;
  // The game list (Settings > Paths): shown instead of the dark screen while
  // nothing runs, when there are game folders.
  std::unique_ptr<GameList> games_;
  std::vector<GameEntry> game_rows_;   // as shown (sorted)
  u64 game_rows_version_ = ~0ull;
  int game_sel_ = -1;
  bool show_game_list_ = true, game_dirs_recursive_ = false;
  // ROM windows (Properties, Contents, Hex & Patches), one per game.
  RomWindowHost rom_host_;
  std::vector<std::unique_ptr<RomWindow>> rom_windows_;
  std::function<void(const std::string&)> folder_target_;  // who asked for a folder (ExportFolder)
  bool cart_load_unsigned_ = false;  // how the running game was loaded (to restart it the same way)
  void open_rom_window(const GameEntry& g, RomWindow::Tab tab);
  // Your compatibility ratings (settings: compat_<crc>).
  void apply_own_rating(GameEntry& g) const {
    g.database_compatibility = g.compatibility;
    char key[32];
    std::snprintf(key, sizeof(key), "compat_%08x", g.crc);
    auto it = settings_.kv.find(key);
    // (A rating that agrees with leapemu's is not shown as yours.)
    g.own_compatibility = it != settings_.kv.end() && !it->second.empty() && it->second != g.database_compatibility;
    if (g.own_compatibility) g.compatibility = it->second;
  }
  void set_own_rating(size_t row, const std::string& tier) { set_rating(game_rows_[row].crc, game_rows_[row].database_compatibility, tier); }
  void set_rating(u32 crc, const std::string& database, const std::string& tier) {
    char key[32];
    std::snprintf(key, sizeof(key), "compat_%08x", crc);
    if (tier.empty() || tier == database) settings_.kv.erase(key);
    else settings_.kv[key] = tier;
    settings_.save();
    for (GameEntry& r : game_rows_)  // (every copy of this game)
      if (r.crc == crc) { r.compatibility = r.database_compatibility; apply_own_rating(r); }
  }
  void init_rom_tools();
  std::vector<std::string> game_dirs() const { return recent("game_dir"); }
  void refresh_games() { if (games_) games_->refresh(game_dirs(), game_dirs_recursive_); }
  void open_folder_dialog(PendingFile::Kind kind = PendingFile::GameFolder);
  void draw_game_list();
  void draw_rom_windows();
  u32 mem_addr_ = 0x3c00'0000;
  char mem_input_[16] = "3c000000";
  bool follow_pc_ = true;
  u32 disasm_addr_ = 0;
  std::string error_;
  std::string status_msg_;
  u64 status_time_ = 0;
};

// ---------------------------------------------------------------------------

int App::run(int argc, char** argv) {
  std::string bios_arg, cart_arg, state_arg, movie_arg;
  bool fullscreen_arg = false;
  auto usage = [&](FILE* to) {
    std::fprintf(to,
                 "usage: leapemu [options] [ROM]\n\n"
                 "  ROM                  a cartridge image (.bin or .zip) to start\n"
                 "  -b, --bios FILE      the BaseROM to use (otherwise the last one chosen)\n"
                 "  -f, --fullscreen     start in fullscreen\n"
                 "  -s, --scale N        window size: N times the 160x160 screen (1-8)\n"
                 "  -t, --state FILE     load a save state once the game starts\n"
                 "  -m, --movie FILE     play an input movie (.lmv) from power-on\n"
                 "  -v                   more logging\n"
                 "  -h, --help           this help\n"
                 "  --version            the version\n\n"
                 "leapemu-cli runs the emulator without a window (tests, exports, traces).\n");
  };
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) { std::fprintf(stderr, "leapemu: %s needs a value\n", a.c_str()); return false; }
      out = argv[++i];
      return true;
    };
    std::string scale;
    if (a == "-h" || a == "--help") { usage(stdout); return 0; }
    else if (a == "--version") { std::printf("leapemu %s\n", LEAPEMU_VERSION); return 0; }
    else if (a == "-b" || a == "--bios") { if (!value(bios_arg)) return 2; }
    else if (a == "-f" || a == "--fullscreen") fullscreen_arg = true;
    else if (a == "-s" || a == "--scale") {
      if (!value(scale)) return 2;
      pending_scale_ = std::clamp(std::atoi(scale.c_str()), 1, 8);
    } else if (a == "-t" || a == "--state") { if (!value(state_arg)) return 2; }
    else if (a == "-m" || a == "--movie") { if (!value(movie_arg)) return 2; }
    else if (a == "-v") Log::level = LogLevel::Debug;
    else if (!a.empty() && a[0] != '-' && cart_arg.empty()) cart_arg = a;
    else { std::fprintf(stderr, "leapemu: unknown option %s\n\n", a.c_str()); usage(stderr); return 2; }
  }

  char* pref = SDL_GetPrefPath("leapemu", "leapemu");
  pref_dir_ = pref ? fs::path(pref) : fs::current_path();
  SDL_free(pref);
  nvram_dir_ = pref_dir_ / "nvram";
  settings_.load(pref_dir_);  // (first: the window's render driver and VSync are settings)
  if (!init_sdl()) return 1;
  saves_beside_rom_ = settings_.get("saves_beside_rom", "1") == "1";
  settings_.kv.erase("skip_intro");  // (a removed option)
  rewind_on_ = settings_.get("rewind", "1") == "1";
  show_counters_ = settings_.get("show_counters", "0") == "1";
  volume_ = float(std::clamp(std::atof(settings_.get("volume", "1").c_str()), 0.0, 1.0));
  muted_ = settings_.get("mute", "0") == "1";
  inactive_pause_ = settings_.get("inactive_pause", "0") == "1";
  inactive_mute_ = settings_.get("inactive_mute", "0") == "1";
  ff_speed_ = std::clamp(std::atof(settings_.get("ff_speed", "0").c_str()), 0.0, 16.0);
  apply_volume();

  settings_.kv.erase("boot_on_startup");  // (a removed option)
  load_keys();
  show_game_list_ = settings_.get("show_game_list", "1") == "1";
  game_dirs_recursive_ = settings_.get("game_dirs_recursive", "0") == "1";
  games_ = std::make_unique<GameList>(pref_dir_ / "gamelist.cache");
  init_rom_tools();
  refresh_games();
  if (recent("recent_rom").empty() && !settings_.get("last_cart").empty())  // (older settings)
    add_recent("recent_rom", settings_.get("last_cart"), 10);
  const std::string bios = !bios_arg.empty() ? bios_arg : settings_.get("bios");
  if (!bios.empty()) open_bios(bios);

  filter_ = Filter(std::clamp(std::atoi(settings_.get("filter", "0").c_str()), 0, int(Filter::Lcd)));
  {
    // (A new key: the JIT is the default where available, whatever the old
    // "cpu_backend" key saved.)
    const bool jit = arc::Cpu::backend_available(arc::Cpu::Backend::Jit);
    const int b = std::atoi(settings_.get("cpu_core", jit ? "2" : "1").c_str());
    const auto backend = arc::Cpu::Backend(std::clamp(b, 0, 2));
    m_.cpu().set_backend(arc::Cpu::backend_available(backend) ? backend : arc::Cpu::Backend::CachedInterpreter);
    m_.cpu().idle_skip = settings_.get("idle_skip", "1") == "1";
    hw_timing_ = settings_.get("hw_timing", "1") == "1";
    smoothing_ = settings_.get("interp", "0") != "0";
    vector_scale_ = settings_.get("vector_scale", "0") == "1";
    update_display_mode();
    m_.timing = hw_timing_ ? Machine::Timing{} : Machine::Timing::none();
    m_.apply_timing();
  }
  integer_scale_ = settings_.get("integer_scale", "1") == "1";
  // A cartridge on the command line starts at once, and so does a BIOS given
  // there without one; otherwise the screen stays dark until something loads.
  if (!cart_arg.empty()) open_cart(cart_arg);
  else if (!bios_arg.empty() && m_.has_bios()) boot_bios();
  if (!movie_arg.empty()) {
    if (powered_) start_movie(movie_arg, PendingFile::MoviePlay);
    else set_error("--movie needs a game (or a BIOS) to play it on");
  }
  if (!state_arg.empty()) {
    if (powered_) load_state(state_arg, fs::path(state_arg).filename().string());
    else set_error("--state needs a game (or a BIOS) to load it into");
  }
  if (fullscreen_arg) toggle_fullscreen();
  start_ticks_ = SDL_GetTicks();
  last_counter_ = SDL_GetPerformanceCounter();
  const char* quit_after = std::getenv("LEAPEMU_QUIT_AFTER_MS");  // automated testing
  if (std::getenv("LEAPEMU_FRAME_STATS")) frame_stats_ = std::make_unique<std::vector<double>>();
  // Automated GUI testing: LEAPEMU_TEST_SCRIPT="ms:load0;ms:tap:x:y;..." with
  // window coordinates. Taps are injected as real SDL mouse events. Also
  // move:x:y (pointer), record, play, stop, ro, rw (movies), saveN, loadN,
  // hash, poweroff, rwon / rwoff (hold rewind), btn:N (hold button bit N
  // for 200 ms) and key:N (press the key with SDL scancode N).
  struct TestStep { u64 ms; std::string op; float x = 0, y = 0; bool done = false; };
  std::vector<TestStep> script;
  if (const char* ts = std::getenv("LEAPEMU_TEST_SCRIPT")) {
    std::string all = ts;
    size_t pos = 0;
    while (pos < all.size()) {
      size_t end = all.find(';', pos);
      if (end == std::string::npos) end = all.size();
      const std::string item = all.substr(pos, end - pos);
      TestStep st;
      char op[16] = {};
      if (std::sscanf(item.c_str(), "%llu:%15[^:]:%f:%f", reinterpret_cast<unsigned long long*>(&st.ms), op, &st.x, &st.y) >= 2) {
        st.op = op;
        script.push_back(st);
        if (st.op == "tap") { TestStep up = st; up.ms += 200; up.op = "release"; script.push_back(up); }
        if (st.op == "btn") { TestStep up = st; up.ms += 200; up.op = "btnup"; script.push_back(up); }
      }
      pos = end + 1;
    }
  }
  while (!quit_) {
    if (quit_after && SDL_GetTicks() - start_ticks_ > u64(std::atoll(quit_after))) quit_ = true;
    for (auto& st : script) {
      if (st.done || SDL_GetTicks() - start_ticks_ < st.ms) continue;
      st.done = true;
      if (st.op == "load0") { load_slot(0); continue; }
      // Movie operations, with the movie file in LEAPEMU_TEST_MOVIE.
      const char* tm = std::getenv("LEAPEMU_TEST_MOVIE");
      if (st.op == "record" && tm) { start_movie(tm, PendingFile::MovieRecord); continue; }
      if (st.op == "play" && tm) { start_movie(tm, PendingFile::MoviePlay); continue; }
      if (st.op == "stop") { stop_movie(); continue; }
      if (st.op == "poweroff") { if (!switched_off_) toggle_power(); continue; }
      if (st.op == "rwon" || st.op == "rwoff") { test_rewind_ = st.op == "rwon"; continue; }
      if (st.op == "btn") { test_buttons_ |= 1u << int(st.x); continue; }
      if (st.op == "ff" || st.op == "ffoff") { test_turbo_ = st.op == "ff"; continue; }
      if (st.op == "rstick") { test_stick_x_ = st.x; test_stick_y_ = st.y; continue; }  // rstick:x:y
      if (st.op == "stylus" || st.op == "stylusup") { test_stylus_ = st.op == "stylus"; continue; }
      if (st.op == "controls") { show_settings_ = true; settings_page_ = SettingsPage::Controls; continue; }
      if (st.op == "shortcuts") { show_settings_ = true; settings_page_ = SettingsPage::Shortcuts; continue; }
      if (st.op == "settings") { show_settings_ = true; settings_page_ = SettingsPage(int(st.x)); continue; }
      if (st.op == "key") {  // key:SCANCODE[:MOD]: a key press (shortcuts; buttons read the keyboard state)
        SDL_Event kev{};
        kev.type = SDL_EVENT_KEY_DOWN;
        kev.key.windowID = SDL_GetWindowID(window_);
        kev.key.scancode = SDL_Scancode(int(st.x));
        kev.key.mod = SDL_Keymod(int(st.y));  // (key:SCANCODE:MOD, e.g. 64 = left Ctrl, 1 = left Shift)
        kev.key.down = true;
        SDL_PushEvent(&kev);
        continue;
      }
      if (st.op == "btnup") { test_buttons_ &= ~(1u << int(st.x)); continue; }
      if (st.op.rfind("cart", 0) == 0) {  // cartN: load the ROM in LEAPEMU_TEST_CARTN
        if (const char* c = std::getenv(("LEAPEMU_TEST_CART" + st.op.substr(4)).c_str())) {
          const std::string v = c;  // ("unsigned:path": as File > Load Unsigned ROM)
          if (v.rfind("unsigned:", 0) == 0) open_cart(v.substr(9), true);
          else open_cart(v);
        }
        continue;
      }
      if (st.op == "ro" || st.op == "rw") { set_read_only(st.op == "ro"); continue; }
      if (st.op.size() == 5 && st.op.rfind("save", 0) == 0) { save_slot(st.op[4] - '0'); continue; }
      if (st.op.size() == 5 && st.op.rfind("load", 0) == 0) { load_slot(st.op[4] - '0'); continue; }
      if (st.op == "hash") {
        const std::vector<u8> state = m_.save_state();
        std::printf("frame %llu state crc32 %08x rerecords %llu movie frames %zu\n", static_cast<unsigned long long>(m_.frame_count()),
                    crc32(state.data(), state.size()), static_cast<unsigned long long>(movie_.rerecords), movie_.frames.size());
        std::fflush(stdout);
        continue;
      }
      SDL_Event ev{};
      if (st.op == "move") {
        ev.type = SDL_EVENT_MOUSE_MOTION;
        ev.motion.windowID = SDL_GetWindowID(window_);
        ev.motion.x = st.x;
        ev.motion.y = st.y;
        SDL_PushEvent(&ev);
        continue;
      }
      ev.type = st.op == "tap" ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
      ev.button.windowID = SDL_GetWindowID(window_);
      ev.button.button = SDL_BUTTON_LEFT;
      ev.button.down = st.op == "tap";
      ev.button.x = st.x;
      ev.button.y = st.y;
      SDL_PushEvent(&ev);
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(e);

    PendingFile::Kind kind;
    std::string path;
    {
      std::lock_guard<std::mutex> lock(pending_.mu);
      kind = pending_.kind;
      path = pending_.path;
      pending_.kind = PendingFile::None;
    }
    if (kind == PendingFile::Bios) open_bios(path);
    else if (kind == PendingFile::Cart) open_cart(path);
    else if (kind == PendingFile::CartUnsigned) open_cart(path, true);
    else if (kind == PendingFile::ExportFolder || kind == PendingFile::ExportFile) {
      // (the next dialog starts where this one ended)
      settings_.kv["export_dir"] = kind == PendingFile::ExportFolder ? path : fs::path(path).parent_path().string();
      if (auto target = std::move(folder_target_)) target(path);
      folder_target_ = nullptr;
    } else if (kind == PendingFile::SavesFolder) {
      settings_.kv["saves_dir"] = path;
      settings_.save();
      move_save();
    } else if (kind == PendingFile::GameFolder) {
      std::vector<std::string> dirs = game_dirs();
      if (dirs.size() >= size_t(kListMax)) set_error("At most " + std::to_string(kListMax) + " game folders");
      else if (std::find(dirs.begin(), dirs.end(), path) == dirs.end()) {
        dirs.push_back(path);
        set_list("game_dir", dirs);
      }
      refresh_games();
    }
    else if (kind != PendingFile::None) start_movie(path, kind);

    const u64 now = SDL_GetPerformanceCounter();
    const double dt = double(now - last_counter_) / double(SDL_GetPerformanceFrequency());
    last_counter_ = now;

    update_input();
    emulate(dt);
    if (powered_ && m_.powered_off() && movie_mode_ == MovieMode::None) console_switched_off();
    // A game that does not let the console turn off (a broken or unsupported
    // one) is switched off anyway after 10 emulated seconds (or 20 real ones).
    if (powered_ && switched_off_ && !m_.powered_off() && movie_mode_ == MovieMode::None && running_ &&
        (m_.frame_count() - power_off_frame_ > 600 || SDL_GetTicks() - power_off_ticks_ > 20000)) {
      console_switched_off();
      status_msg_ = "The game did not turn the console off: switched off";
      status_time_ = SDL_GetTicks();
    }
    if (SDL_GetTicks() - last_flush_ >= 1000) {
      flush_saves();
      store_settings();  // (settings too, within a second of a change)
      last_flush_ = SDL_GetTicks();
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    const u64 screen_t0 = SDL_GetPerformanceCounter();
    render_screen();
    render_lcd_tone();
    render_pad_cursor();
    if (frame_stats_) frame_stats_->push_back(double(SDL_GetPerformanceCounter() - screen_t0) * 1000.0 / double(SDL_GetPerformanceFrequency()));
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    // Debug aid: LEAPEMU_DUMP_WINDOW=file.png saves the last rendered frame on exit.
    if (quit_ && std::getenv("LEAPEMU_DUMP_WINDOW")) {
      if (SDL_Surface* raw = SDL_RenderReadPixels(renderer_, nullptr)) {
        if (SDL_Surface* conv = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888)) {
          std::vector<u32> px(size_t(conv->w) * conv->h);
          for (int y = 0; y < conv->h; y++)
            std::memcpy(&px[size_t(y) * conv->w], static_cast<u8*>(conv->pixels) + y * conv->pitch, conv->w * 4);
          write_png(std::getenv("LEAPEMU_DUMP_WINDOW"), px.data(), conv->w, conv->h);
          SDL_DestroySurface(conv);
        }
        SDL_DestroySurface(raw);
      }
    }
    SDL_RenderPresent(renderer_);
  }

  stop_movie();
  flush_saves();
  store_settings();
  const double wall = double(SDL_GetTicks() - start_ticks_) / 1000.0;
  if (wall > 0) LOG_I("ran %llu frames in %.1f s wall time (%.2f fps)", static_cast<unsigned long long>(total_frames_), wall, total_frames_ / wall);
  shutdown();
  return 0;
}

bool App::init_sdl() {
  // (The app id lets Wayland desktops match the window to leapemu.)
  SDL_SetAppMetadata("leapemu", LEAPEMU_VERSION, "leapemu");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return false;
  }
  int win_w = 640, win_h = 700;
  if (const char* ws = std::getenv("LEAPEMU_WINDOW_SIZE")) std::sscanf(ws, "%dx%d", &win_w, &win_h);  // automated testing
  window_ = SDL_CreateWindow("leapemu", win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window_) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
  if (SDL_Surface* icon = SDL_CreateSurfaceFrom(kIconSize, kIconSize, SDL_PIXELFORMAT_RGBA32,
                                                const_cast<unsigned char*>(kIconRgba), kIconSize * 4)) {
    SDL_SetWindowIcon(window_, icon);  // (res/leapemu.svg)
    SDL_DestroySurface(icon);
  }
  // Settings > Audio & Video: SDL's render driver (its first working one by
  // default), and VSync.
  const std::string driver = settings_.get("render_driver");
  if (!driver.empty()) renderer_ = SDL_CreateRenderer(window_, driver.c_str());
  if (!renderer_) renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!renderer_) { std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }
  vsync_ = settings_.get("vsync", "1") == "1";
  SDL_SetRenderVSync(renderer_, vsync_ ? 1 : 0);

  screen_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                  Machine::kWidth, Machine::kHeight);
  SDL_SetTextureScaleMode(screen_tex_, SDL_SCALEMODE_NEAREST);
  SDL_SetWindowMinimumSize(window_, Machine::kWidth, Machine::kHeight);  // (View > Window Size > 1x)

  SDL_AudioSpec spec{SDL_AUDIO_S16, 1, int(Machine::kAudioHz)};
  audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (audio_) SDL_ResumeAudioStreamDevice(audio_);
  else LOG_W("no audio: %s", SDL_GetError());

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.IniFilename = nullptr;  // layout is rebuilt each launch
  ImGui::StyleColorsDark();
  const float scale = SDL_GetWindowDisplayScale(window_);
  if (scale > 1.0f) {
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleDpi = scale;
  }
  ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
  ImGui_ImplSDLRenderer3_Init(renderer_);
  return true;
}

void App::shutdown() {
  if (frame_stats_ && !frame_stats_->empty()) {
    auto& v = *frame_stats_;
    std::sort(v.begin(), v.end());
    std::printf("screen drawing over %zu frames: median %.2f ms, 90%% %.2f ms, 99%% %.2f ms, max %.2f ms\n", v.size(), v[v.size() / 2],
                v[v.size() * 9 / 10], v[v.size() * 99 / 100], v.back());
  }
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  if (pad_) SDL_CloseGamepad(pad_);
  rom_windows_.clear();  // (before the audio they may be using goes)
  if (preview_audio_) SDL_DestroyAudioStream(preview_audio_);
  preview_audio_ = nullptr;
  if (audio_) SDL_DestroyAudioStream(audio_);
  if (sharp_tex_) SDL_DestroyTexture(sharp_tex_);
  if (filtered_tex_) SDL_DestroyTexture(filtered_tex_);
  for (auto& lt : layer_tex_) {
    if (lt.tex) SDL_DestroyTexture(lt.tex);
    if (lt.ftex) SDL_DestroyTexture(lt.ftex);
  }
  if (grid_tex_) SDL_DestroyTexture(grid_tex_);
  SDL_DestroyTexture(screen_tex_);
  SDL_DestroyRenderer(renderer_);
  SDL_DestroyWindow(window_);
  SDL_Quit();
}

// ---------------------------------------------------------------------------
// Machine control
// ---------------------------------------------------------------------------

// Sets the BaseROM; a running machine restarts with it.
bool App::open_bios(const std::string& path) {
  std::string err;
  if (!m_.load_bios(path, &err)) { set_error("BIOS: " + err); return false; }
  settings_.kv["bios"] = path;
  add_recent("recent_bios", path, 5);
  if (!sys_loaded_) {  // the console's own EEPROM (touch calibration, settings)
    m_.load_system_nvram(system_save_path().string());
    sys_saved_ = m_.system_eeprom();
    sys_loaded_ = true;
  }
  if (powered_) power_cycle();
  status_msg_ = "BIOS: " + fs::path(path).filename().string();
  status_time_ = SDL_GetTicks();
  error_.clear();  // (an earlier BIOS error is resolved)
  if (!m_.bios_problem().empty()) set_error("BIOS: " + m_.bios_problem());
  else if (!waiting_cart_.empty()) {  // the game asked for before there was a BaseROM
    const std::string cart = std::exchange(waiting_cart_, {});
    open_cart(cart, waiting_unsigned_);
  }
  return true;
}

// `allow_unsigned`: File > Load Unsigned ROM, for homebrew. The BaseROM only
// boots cartridges signed by LeapFrog; for this cartridge a small in-memory
// patch skips that check.
bool App::open_cart(const std::string& path, bool allow_unsigned) {
  if (!m_.has_bios()) {
    set_error("Choose a BaseROM (BIOS) first: nothing runs without one");
    waiting_cart_ = path;  // (it starts once one is chosen)
    waiting_unsigned_ = allow_unsigned;
    return false;
  }
  unload_cart();
  std::string err;
  if (!m_.load_cart(path, &err)) {
    set_error("Cartridge: " + err);
    powered_ = false;  // (no cartridge now: idle)
    return false;
  }
  settings_.kv["last_cart"] = path;
  add_recent("recent_rom", (allow_unsigned ? "unsigned:" : "") + path, 10);
  cart_title_ = m_.cart_header().valid && !m_.cart_header().title.empty() ? m_.cart_header().title : fs::path(path).stem().string();
  if (m_.downloadable()) {  // a Leapster 2 download: its package's meta.inf names it
    std::ifstream meta(fs::path(path).parent_path() / "meta.inf");
    for (std::string line; std::getline(meta, line);)
      if (line.rfind("Name=\"", 0) == 0) {
        const size_t end = line.find('"', 6);
        if (end != std::string::npos && end > 6) cart_title_ = line.substr(6, end - 6);
      }
  }
  const std::string& title = cart_title_;
  SDL_SetWindowTitle(window_, ("leapemu - " + title).c_str());
  load_cart_save(path);
  // (downloadables are not signed as cartridges; patched images fail the digest;
  // a few known dumps have damaged bytes)
  m_.allow_unsigned = allow_unsigned || m_.downloadable() || m_.cart_patch().applied || database_has_flag(m_.cart_crc(), "unsigned");
  m_.stub_missing_services = database_has_flag(m_.cart_crc(), "stub-services");
  cart_load_unsigned_ = allow_unsigned;
  if (m_.cart_patch().applied || m_.cart_patch().mismatched) {
    status_msg_ = "ROM patches: " + std::to_string(m_.cart_patch().applied) + " edits applied";
    if (m_.cart_patch().mismatched) status_msg_ += ", " + std::to_string(m_.cart_patch().mismatched) + " do not match this ROM";
    status_time_ = SDL_GetTicks();
  }
  power_cycle();
  return true;
}

// Emulation > Console > Start Without Cartridge: the console with no cartridge.
void App::boot_bios() {
  if (!m_.has_bios()) return;
  unload_cart();
  power_cycle();
}

// Takes the cartridge out: its movie stops, its save is written, and nothing
// keeps pointing into its image (the redraw's ROM view, captured frames).
void App::unload_cart() {
  stop_movie();
  flush_saves();
  if (m_.has_cart()) m_.eject_cart();
  cart_rom_.clear();
  cart_save_path_.clear();
  m_.allow_unsigned = false;
  m_.stub_missing_services = false;
  interp_.reset();
  sync_rom_view();
  SDL_SetWindowTitle(window_, "leapemu");
}

std::vector<std::string> App::recent(const char* key) const {
  std::vector<std::string> out;
  for (int i = 0; i < kListMax; i++) {
    const std::string v = settings_.get(key + std::to_string(i));
    if (!v.empty()) out.push_back(v);
  }
  return out;
}

void App::add_recent(const char* key, const std::string& value, size_t max) {
  std::vector<std::string> list = recent(key);
  // The same file loaded either way is one entry.
  const std::string plain = value.rfind("unsigned:", 0) == 0 ? value.substr(9) : value;
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const std::string& v) { return v == plain || v == "unsigned:" + plain; }),
             list.end());
  list.insert(list.begin(), value);
  if (list.size() > max) list.resize(max);
  set_list(key, list);
}

// A list setting: <key>0, <key>1, ... (at most kListMax).
void App::set_list(const char* key, const std::vector<std::string>& list) {
  for (int i = 0; i < kListMax; i++) settings_.kv.erase(key + std::to_string(i));
  for (size_t i = 0; i < list.size() && i < size_t(kListMax); i++) settings_.kv[key + std::to_string(i)] = list[i];
  settings_.save();
}

// Like other emulators, the save sits next to the ROM with a .sav extension
// ("Pet Pals (USA).zip" -> "Pet Pals (USA).sav"), unless that folder is not
// writable or the user prefers the data folder.
fs::path App::cart_save_path(const std::string& rom) const {
  fs::path p(rom);
  p.replace_extension(".sav");
  if (saves_beside_rom_) {
    const fs::path probe = p.parent_path() / ".leapemu-write-test";
    std::error_code ec;
    const bool writable = bool(std::ofstream(probe));
    fs::remove(probe, ec);
    if (writable) return p;
  }
  return saves_dir() / p.filename();
}

// Settings > Paths: where saves go when not next to the ROM (the data folder's
// "saves" folder unless one is chosen).
fs::path App::saves_dir() const {
  const std::string d = settings_.get("saves_dir");
  return d.empty() ? pref_dir_ / "saves" : fs::path(d);
}

// The running game's save follows a change of where saves go.
void App::move_save() {
  if (cart_rom_.empty() || saves_suspended_) return;
  cart_save_path_ = cart_save_path(cart_rom_);
  cart_save_force_ = true;
  flush_saves();
}

void App::load_cart_save(const std::string& rom) {
  cart_rom_ = rom;
  cart_save_path_ = cart_save_path(rom);
  bool found = m_.load_cart_save(cart_save_path_.string());
  cart_save_force_ = false;
  if (!found) {
    // Wherever else it may be: next to the ROM, the chosen saves folder, the
    // data folder's (after the setting changed), and nvram/<crc>.eep (before
    // .sav files). A save found elsewhere is written to the current place;
    // the old file is left as it is.
    const fs::path name = cart_save_path_.filename();
    char legacy[32];
    std::snprintf(legacy, sizeof(legacy), "%08x.eep", m_.cart_crc());
    for (const fs::path& old : {fs::path(rom).replace_extension(".sav"), saves_dir() / name, pref_dir_ / "saves" / name,
                                nvram_dir_ / legacy})
      if (old != cart_save_path_ && m_.load_cart_save(old.string())) {
        LOG_I("imported save from %s", old.string().c_str());
        cart_save_force_ = true;
        break;
      }
  }
  cart_saved_ = m_.cart_eeprom();
}

// Back to the files on disk (after a movie ran on its own EEPROM contents).
void App::reload_saves() {
  m_.load_system_nvram(system_save_path().string());
  sys_saved_ = m_.system_eeprom();
  if (m_.has_cart() && !cart_rom_.empty()) load_cart_save(cart_rom_);
  saves_suspended_ = false;
}

void App::flush_saves() {
  if (!sys_loaded_ || saves_suspended_) return;
  std::error_code ec;
  if (m_.system_eeprom() != sys_saved_) {
    fs::create_directories(system_save_path().parent_path(), ec);
    if (m_.save_system_nvram(system_save_path().string())) sys_saved_ = m_.system_eeprom();
    else LOG_W("could not write %s", system_save_path().string().c_str());
  }
  if (m_.has_cart() && !cart_save_path_.empty() && (cart_save_force_ || m_.cart_eeprom() != cart_saved_)) {
    fs::create_directories(cart_save_path_.parent_path(), ec);
    if (!m_.save_cart_save(cart_save_path_.string())) {
      // Not writable: the chosen saves folder, then the data folder's.
      bool saved = false;
      for (const fs::path& dir : {saves_dir(), pref_dir_ / "saves"}) {
        if (dir == cart_save_path_.parent_path()) continue;
        LOG_W("could not write %s; trying %s", cart_save_path_.string().c_str(), dir.string().c_str());
        cart_save_path_ = dir / cart_save_path_.filename();
        fs::create_directories(dir, ec);
        if ((saved = m_.save_cart_save(cart_save_path_.string()))) break;
      }
      if (!saved) { set_error("Cannot write the game's save: " + cart_save_path_.string()); return; }
    }
    cart_saved_ = m_.cart_eeprom();
    cart_save_force_ = false;
  }
}

void App::power_cycle() {
  if (!m_.has_bios() || m_.bios_crc() == 0) return;
  stop_movie();
  // The EEPROMs keep their contents across a reset, as on hardware.
  if (saves_suspended_) reload_saves();
  else flush_saves();
  switched_off_ = false;  // (switching on)
  console_off_ = false;
  clear_held_input();
  rewind_.clear();
  m_.reset();
  m_.cpu().clear_stop();
  interp_.reset();
  sync_rom_view();
  powered_ = true;
  running_ = true;
  frame_accum_ = 0;
  lag_count_ = 0;
  error_.clear();
}

fs::path App::state_path(int slot) const {
  char name[64];
  std::snprintf(name, sizeof(name), "%08x-%d.state", m_.has_cart() ? m_.cart_crc() : 0u, slot);
  return pref_dir_ / "states" / name;
}

void App::save_slot(int slot) {
  if (!powered_) return;
  std::error_code ec;
  fs::create_directories(pref_dir_ / "states", ec);
  std::string err;
  if (!m_.save_state_file(state_path(slot).string(), &err)) { set_error(err); return; }
  // With a movie, the state carries the movie's input up to this frame, so
  // loading it while recording continues that timeline (rerecording).
  const fs::path companion = fs::path(state_path(slot)).replace_extension(".lmv");
  if (movie_mode_ != MovieMode::None) {
    Movie branch = movie_;
    branch.frames.resize(std::min<size_t>(branch.frames.size(), m_.frame_count()));
    if (!branch.save(companion.string(), &err)) { set_error("Movie: " + err); return; }
    if (movie_mode_ == MovieMode::Recording && !movie_.save(movie_path_, &err)) set_error("Movie: " + err);
  } else {
    std::error_code ec;
    fs::remove(companion, ec);
  }
  error_.clear();
  status_msg_ = slot ? "Saved state " + std::to_string(slot) : std::string("Quick saved");
  status_time_ = SDL_GetTicks();
}

void App::load_slot(int slot) { load_state(state_path(slot), slot ? "state " + std::to_string(slot) : "quick save"); }

void App::load_state(const fs::path& path, const std::string& name) {
  if (!powered_) return;
  std::string err;
  if (!m_.load_state_file(path.string(), &err)) { set_error("Load state: " + err); return; }
  interp_.reset();
  // A new timeline: its first snapshot, and no input from the old one.
  input_log_.clear();
  input_log_base_ = m_.frame_count();
  if (rewind_on_) rewind_.mark(m_);
  error_.clear();
  status_msg_ = "Loaded " + name;
  if (movie_mode_ != MovieMode::None) {
    const u64 f = m_.frame_count();
    Movie branch;
    std::string e2;
    const fs::path companion = fs::path(path).replace_extension(".lmv");
    const bool have = fs::exists(companion) && branch.load(companion.string(), &e2) && branch.bios_crc == movie_.bios_crc &&
                      branch.cart_crc == movie_.cart_crc && branch.frames.size() >= f;
    if (movie_mode_ == MovieMode::Recording) {
      if (have) movie_.frames.assign(branch.frames.begin(), branch.frames.begin() + std::ptrdiff_t(f));
      else if (f <= movie_.frames.size()) movie_.frames.resize(f);
      else { set_error("This state is past the end of the movie"); return; }
      movie_.rerecords++;
      status_msg_ += " (rerecord " + std::to_string(movie_.rerecords) + ")";
    } else {
      bool same = !have || f <= movie_.frames.size();
      for (u64 i = 0; same && have && i < f; i++) same = branch.frames[i] == movie_.frames[i];
      if (!same) set_error("This state is from a different timeline than the movie");
      movie_mode_ = f < movie_.frames.size() ? MovieMode::Playing : MovieMode::Finished;
    }
  }
  status_time_ = SDL_GetTicks();
}

void App::open_dialog(PendingFile::Kind kind) {
  static const SDL_DialogFileFilter rom_filters[] = {
      {"Leapster images", "bin;zip;wav"},
      {"All files", "*"},
  };
  static const SDL_DialogFileFilter movie_filters[] = {
      {"leapemu movies", "lmv"},
      {"All files", "*"},
  };
  const SDL_DialogFileFilter* filters = kind == PendingFile::MoviePlay ? movie_filters : rom_filters;
  // The callback may run on another thread; it only records the choice and
  // the main loop acts on it.
  struct Ctx { PendingFile* p; PendingFile::Kind k; };
  static Ctx ctx;
  ctx = {&pending_, kind};
  SDL_ShowOpenFileDialog(
      [](void* user, const char* const* files, int) {
        auto* c = static_cast<Ctx*>(user);
        if (!files || !files[0]) return;
        std::lock_guard<std::mutex> lock(c->p->mu);
        c->p->path = files[0];
        c->p->kind = c->k;
      },
      &ctx, window_, filters, 2, nullptr, false);
}

void App::save_dialog(PendingFile::Kind kind) {
  static const SDL_DialogFileFilter filters[] = {{"leapemu movies", "lmv"}};
  struct Ctx { PendingFile* p; PendingFile::Kind k; };
  static Ctx ctx;
  ctx = {&pending_, kind};
  SDL_ShowSaveFileDialog(
      [](void* user, const char* const* files, int) {
        auto* c = static_cast<Ctx*>(user);
        if (!files || !files[0]) return;
        std::lock_guard<std::mutex> lock(c->p->mu);
        c->p->path = files[0];
        if (fs::path(c->p->path).extension().empty()) c->p->path += ".lmv";
        c->p->kind = c->k;
      },
      &ctx, window_, filters, 1, nullptr);
}

// ---------------------------------------------------------------------------
// Input and timing
// ---------------------------------------------------------------------------

void App::handle_event(const SDL_Event& e) {
  ImGui_ImplSDL3_ProcessEvent(&e);
  switch (e.type) {
    case SDL_EVENT_QUIT: quit_ = true; break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (e.window.windowID == SDL_GetWindowID(window_)) quit_ = true;
      break;
    case SDL_EVENT_DROP_FILE: {
      const std::string path = e.drop.data ? e.drop.data : "";
      // Treat anything carrying a BaseROM header as a BIOS, else a cartridge.
      std::vector<u8> img;
      std::string err;
      if (load_rom_file(path, &img, &err)) {
        const RomHeader h = parse_rom_header(img);
        if (h.valid && h.title.find("BaseROM") != std::string::npos) open_bios(path);
        else open_cart(path);
      } else {
        set_error(err);
      }
      break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (inactive_pause_ && running_) { running_ = false; auto_paused_ = true; }
      apply_volume();
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      if (auto_paused_ && powered_) running_ = true;
      auto_paused_ = false;
      apply_volume();
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      if (!pad_) pad_ = SDL_OpenGamepad(e.gdevice.which);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      if (pad_ && SDL_GetGamepadID(pad_) == e.gdevice.which) { SDL_CloseGamepad(pad_); pad_ = nullptr; }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (e.button.button == SDL_BUTTON_LEFT && !ImGui::GetIO().WantCaptureMouse) {
        float px, py;
        int ww, wh, ow, oh;
        SDL_GetWindowSize(window_, &ww, &wh);
        SDL_GetCurrentRenderOutputSize(renderer_, &ow, &oh);
        px = e.button.x * ow / float(ww);
        py = e.button.y * oh / float(wh);
        const SDL_FPoint pt{px, py};
        if (SDL_PointInRectFloat(&pt, &screen_rect_)) {
          touch_down_ = true;
          set_touch_from_window(e.button.x, e.button.y);
        }
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (touch_down_) set_touch_from_window(e.motion.x, e.motion.y);
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (e.button.button == SDL_BUTTON_LEFT) touch_down_ = false;
      break;
    case SDL_EVENT_KEY_UP:  // (whatever the modifiers now)
      for (size_t i = 0; i < kControlCount; i++)
        if (kControls[i].act == Act::Rewind && (keys_[i][0].key == e.key.scancode || keys_[i][1].key == e.key.scancode)) rewind_released();
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
      const auto b = SDL_GamepadButton(e.gbutton.button);
      if (capture_control_ >= 0) {  // Settings > Controls
        if (capture_slot_ == 2) { assign_pad(size_t(capture_control_), b); capture_control_ = -1; }
        break;
      }
      for (size_t i = 0; i < kControlCount; i++)
        if (pad_btn_[i] == b && kControls[i].act != Act::Button) do_action(kControls[i].act, kControls[i].button, false);
      break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      for (size_t i = 0; i < kControlCount; i++)
        if (pad_btn_[i] == SDL_GamepadButton(e.gbutton.button) && kControls[i].act == Act::Rewind) rewind_released();
      break;
    case SDL_EVENT_KEY_DOWN: {
      const SDL_Scancode sc = e.key.scancode;
      if (capture_control_ >= 0) {  // Settings > Controls: this key (with its modifiers) goes to the waiting slot
        const bool modifier = sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_LSHIFT ||
                              sc == SDL_SCANCODE_RSHIFT || sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT ||
                              sc == SDL_SCANCODE_LGUI || sc == SDL_SCANCODE_RGUI;
        if (!e.key.repeat && !modifier && (capture_slot_ < 2 || sc == SDL_SCANCODE_ESCAPE)) {
          if (sc != SDL_SCANCODE_ESCAPE) assign_key(size_t(capture_control_), capture_slot_, KeyBind{sc, mods_of(e.key.mod)});
          capture_control_ = -1;
        }
        break;
      }
      // Fixed shortcuts (not rebindable): Escape leaves fullscreen, Ctrl+F toggles it.
      if ((sc == SDL_SCANCODE_ESCAPE && fullscreen_) || (sc == SDL_SCANCODE_F && mods_of(e.key.mod) == kModCtrl)) {
        if (!e.key.repeat) toggle_fullscreen();
        break;
      }
      if (ImGui::GetIO().WantTextInput) break;
      const int c = control_of(KeyBind{sc, mods_of(e.key.mod)});
      if (c < 0) break;
      // (While a tool window has the keyboard, only the window-level shortcuts work.)
      const Act act = kControls[c].act;
      if (ui_keys_ && act != Act::Settings && act != Act::Fullscreen && act != Act::Quit && act != Act::LoadRom) break;
      do_action(act, kControls[c].button, e.key.repeat);
      break;
    }
  }
}

void App::rewind_released() {
  if (!rewind_down_ms_) return;
  if (rewind_held_) running_ = rewind_resume_;  // after a hold, carry on as before
  rewind_down_ms_ = 0;
  rewind_held_ = false;
}

void App::do_action(Act act, u32 arg, bool repeat) {
  // (Held: frame advance and frame back repeat.)
  if (act == Act::FrameAdvance) {
    if (powered_) { running_ = false; advance_++; }
    return;
  }
  if (act == Act::FrameBack) {
    frame_reverse();
    return;
  }
  if (repeat) return;
  auto say = [&](const std::string& msg) { status_msg_ = msg; status_time_ = SDL_GetTicks(); };
  switch (act) {
    case Act::Pause:
      if (powered_) running_ = !running_;
      break;
    case Act::Rewind:  // (rewinds while held: see update_input)
      if (!powered_) break;
      rewind_down_ms_ = SDL_GetTicks();
      rewind_resume_ = running_;
      rewind_held_ = false;
      if (!rewind_on_) say("Rewinding needs Emulation > Rewind");
      break;
    case Act::FastForwardToggle:
      ff_toggle_ = !ff_toggle_;
      say(ff_toggle_ ? "Fast forward on" : "Fast forward off");
      break;
    case Act::Reset: if (powered_) power_cycle(); break;
    case Act::Stop: stop_game(); break;
    case Act::Slower: cycle_speed(-1); break;
    case Act::Faster: cycle_speed(+1); break;
    case Act::QuickSave: if (powered_) save_slot(0); break;
    case Act::QuickLoad: if (powered_ && fs::exists(state_path(0))) load_slot(0); break;
    case Act::SaveSlot: if (powered_) save_slot(int(arg)); break;
    case Act::LoadSlot: if (powered_ && fs::exists(state_path(int(arg)))) load_slot(int(arg)); break;
    case Act::Power: toggle_power(); break;
    case Act::Mute: set_muted(!muted_); say(muted_ ? "Muted" : "Sound on"); break;
    case Act::Fullscreen: toggle_fullscreen(); break;
    case Act::Screenshot: screenshot(); break;
    case Act::LoadRom: open_dialog(PendingFile::Cart); break;
    case Act::Settings: show_settings_ = true; break;
    case Act::Quit: quit_ = true; break;
    default: break;  // (buttons and fast forward are read as held: update_input)
  }
}

// ---- key bindings ----

// A key as shown: "Ctrl+Shift+F1", with friendlier names for a few keys
// (`words`: spelled out where the symbol alone is easy to miss, as in the
// Settings tables; menus show the symbol).
std::string key_label(const KeyBind& k, bool words = true) {
  if (k.key == kNoKey) return "";
  std::string n = SDL_GetScancodeName(k.key);
  if (n == "Return") n = "Enter";
  else if (n == "`" && words) n = "` (backquote)";
  else if (n == "-" && words) n = "Minus";
  else if (n == "Up" || n == "Down" || n == "Left" || n == "Right") n += words ? " arrow" : "";
  return std::string(k.mods & kModCtrl ? "Ctrl+" : "") + (k.mods & kModShift ? "Shift+" : "") + (k.mods & kModAlt ? "Alt+" : "") + n;
}
// As saved: SDL's key name after its modifiers.
std::string key_setting(const KeyBind& k) {
  if (k.key == kNoKey) return "";
  return std::string(k.mods & kModCtrl ? "Ctrl+" : "") + (k.mods & kModShift ? "Shift+" : "") + (k.mods & kModAlt ? "Alt+" : "") +
         SDL_GetScancodeName(k.key);
}
KeyBind parse_key(std::string v) {
  KeyBind k;
  for (bool more = true; more;) {
    more = false;
    for (auto [prefix, bit] : {std::pair{"Ctrl+", kModCtrl}, std::pair{"Shift+", kModShift}, std::pair{"Alt+", kModAlt}})
      if (v.size() > std::strlen(prefix) && v.rfind(prefix, 0) == 0) { k.mods |= bit; v = v.substr(std::strlen(prefix)); more = true; }
  }
  k.key = v.empty() ? kNoKey : SDL_GetScancodeFromName(v.c_str());
  if (k.key == kNoKey) k.mods = 0;
  return k;
}

void App::load_keys() {
  for (size_t i = 0; i < kControlCount; i++) {
    pad_btn_[i] = kControls[i].pad;
    const std::string p = settings_.get(std::string("pad_") + kControls[i].id);
    if (p == "none") pad_btn_[i] = SDL_GAMEPAD_BUTTON_INVALID;
    else if (!p.empty()) pad_btn_[i] = SDL_GetGamepadButtonFromString(p.c_str());
    keys_[i] = {kControls[i].defaults[0], kControls[i].defaults[1]};
    const std::string v = settings_.get(std::string("key_") + kControls[i].id);
    if (v.empty()) continue;
    // "name,name": SDL key names, each after any "Ctrl+", "Shift+", "Alt+".
    // (The comma key is "," itself: split at the comma between two names.)
    size_t comma = v.find(',', 1);  // (a name starting with "," is the comma key)
    while (comma != std::string::npos && v[comma - 1] == '+') comma = v.find(',', comma + 1);  // ("Ctrl+,": the key)
    const std::string names[2] = {v.substr(0, comma), comma == std::string::npos ? "" : v.substr(comma + 1)};
    for (int s = 0; s < 2; s++) keys_[i][size_t(s)] = parse_key(names[s]);
  }
}

void App::save_keys() {
  for (size_t i = 0; i < kControlCount; i++) {
    const std::string pk = std::string("pad_") + kControls[i].id;
    if (pad_btn_[i] == kControls[i].pad) settings_.kv.erase(pk);
    else if (pad_btn_[i] == SDL_GAMEPAD_BUTTON_INVALID) settings_.kv[pk] = "none";
    else settings_.kv[pk] = SDL_GetGamepadStringForButton(pad_btn_[i]);
    const std::string key = std::string("key_") + kControls[i].id;
    if (keys_[i][0] == kControls[i].defaults[0] && keys_[i][1] == kControls[i].defaults[1]) { settings_.kv.erase(key); continue; }
    settings_.kv[key] = key_setting(keys_[i][0]) + "," + key_setting(keys_[i][1]);
  }
  settings_.save();
}

// A key belongs to one control: taking it here frees it elsewhere.
void App::assign_key(size_t control, int slot, KeyBind key) {
  if (key.key == SDL_SCANCODE_F && key.mods == kModCtrl) {  // (fixed: see the key handler)
    status_msg_ = "Ctrl+F is fixed: it toggles fullscreen";
    status_time_ = SDL_GetTicks();
    return;
  }
  if (key.key != kNoKey)
    for (size_t i = 0; i < kControlCount; i++)
      for (auto& s : keys_[i])
        if (s == key && !(i == control && &s == &keys_[i][size_t(slot)])) {
          s = kNone;
          status_msg_ = key_label(key) + " moved here from \"" + kControls[i].label + "\"";
          status_time_ = SDL_GetTicks();
        }
  keys_[control][size_t(slot)] = key;
  save_keys();
}

// A gamepad button belongs to one control too.
void App::assign_pad(size_t control, SDL_GamepadButton b) {
  if (b != SDL_GAMEPAD_BUTTON_INVALID)
    for (auto& p : pad_btn_)
      if (p == b) p = SDL_GAMEPAD_BUTTON_INVALID;
  pad_btn_[control] = b;
  save_keys();
}

int App::control_of(KeyBind key) const {
  if (key.key == kNoKey) return -1;
  for (size_t i = 0; i < kControlCount; i++)
    if (keys_[i][0] == key || keys_[i][1] == key) return int(i);
  return -1;
}

bool App::key_down(const KeyBind& k, const bool* keys, u8 mods, bool button) {
  if (k.key == kNoKey || !keys || !keys[k.key]) return false;
  if (k.mods) return mods == k.mods;
  return button ? !(mods & (kModCtrl | kModAlt)) : mods == 0;  // (a console button works with Shift held)
}

std::string App::shortcut(Act act, u32 arg) const {
  for (size_t i = 0; i < kControlCount; i++)
    if (kControls[i].act == act && kControls[i].button == arg)
      for (const KeyBind& k : keys_[i])
        if (k.key != kNoKey) return key_label(k, false);
  return "";
}

bool App::held(Act act, const bool* keys) const {
  const u8 mods = mods_of(SDL_GetModState());
  for (size_t i = 0; i < kControlCount; i++) {
    if (kControls[i].act != act) continue;
    for (const KeyBind& k : keys_[i])
      if (key_down(k, keys, mods, act == Act::Button || act == Act::Touch)) return true;
    if (pad_ && pad_btn_[i] != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad_, pad_btn_[i])) return true;
  }
  return false;
}

// The console has finished turning off (its settings and save written): the
// cartridge comes out, and the game list shows. (During a movie the machine
// stays as it is: switching off is part of the movie's input.)
// Emulation > Stop: the console is switched off at once, without waiting for
// the game (for one that hangs, or just to leave it). Its save is written as
// it is; settings the console would save while turning off are not.
void App::stop_game() {
  if (!powered_) return;
  console_switched_off();
  status_msg_ = "Stopped";
  status_time_ = SDL_GetTicks();
}

// The original 160x160 image, in the data folder's "screenshots" folder, named
// after the game and the time (never over an earlier one).
// Settings kept in members (not written where they change) go to the
// settings file with the rest. A movie's own idle-loop setting is not the
// user's.
void App::store_settings() {
  settings_.kv["filter"] = std::to_string(int(filter_));
  settings_.kv["integer_scale"] = integer_scale_ ? "1" : "0";
  settings_.kv["cpu_core"] = std::to_string(int(m_.cpu().backend()));
  settings_.kv.erase("cpu_backend");
  const bool idle = movie_mode_ != MovieMode::None ? user_settings_.idle_skip : m_.cpu().idle_skip;
  settings_.kv["idle_skip"] = idle ? "1" : "0";
  settings_.save();
}

void App::screenshot() {
  if (!powered_) return;
  std::string title = cart_title_.empty() ? "leapemu" : cart_title_;
  for (char& c : title)
    if (std::strchr("\\/:*?\"<>|", c)) c = '_';
  const std::time_t now = std::time(nullptr);
  char when[32];
  std::strftime(when, sizeof(when), "%Y-%m-%d %H-%M-%S", std::localtime(&now));
  const fs::path dir = pref_dir_ / "screenshots";
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path shot = dir / (title + " " + when + ".png");
  for (int n = 2; fs::exists(shot, ec); n++) shot = dir / (title + " " + when + " (" + std::to_string(n) + ").png");
  if (!write_png(shot.string(), m_.framebuffer(), Machine::kWidth, Machine::kHeight)) {
    set_error("Cannot write " + shot.string());
    return;
  }
  status_msg_ = "Saved " + shot.filename().string();
  status_time_ = SDL_GetTicks();
}

void App::set_muted(bool m) {
  muted_ = m;
  settings_.kv["mute"] = muted_ ? "1" : "0";
  apply_volume();
}

void App::apply_volume() {
  const bool background = inactive_mute_ && !(SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS);
  if (audio_) SDL_SetAudioStreamGain(audio_, muted_ || background ? 0.0f : volume_);
}

void App::console_switched_off() {
  off_cart_ = cart_rom_;  // (empty: the BIOS was running alone)
  off_cart_unsigned_ = cart_load_unsigned_;
  unload_cart();
  powered_ = false;
  running_ = false;
  console_off_ = true;
  clear_held_input();
}

// What the keys were doing belongs to the game that is ending: a latched fast
// forward or a rewind key held across the change doesn't carry over.
void App::clear_held_input() {
  ff_toggle_ = false;
  rewind_down_ms_ = 0;
  rewind_held_ = false;
  rewind_resume_ = false;
  advance_ = 0;
}

// Power On after that: the same cartridge, if its file is still there.
void App::power_on_again() {
  console_off_ = false;
  if (off_cart_.empty()) { boot_bios(); return; }
  std::error_code ec;
  if (!fs::exists(off_cart_, ec)) {
    set_error("Cannot power on: " + fs::path(off_cart_).filename().string() + " is no longer there");
    return;
  }
  open_cart(std::string(off_cart_), off_cart_unsigned_);
}

void App::toggle_power() {
  if (!powered_) {
    if (console_off_) power_on_again();
    return;
  }
  if (m_.powered_off()) power_cycle();
  else if (!switched_off_) {
    switched_off_ = true;
    running_ = true;
    power_off_frame_ = m_.frame_count();
    power_off_ticks_ = SDL_GetTicks();
  }
}

void App::update_input() {
  u32 mask = 0;
  turbo_ = false;
  rewinding_ = false;
  if (!ImGui::GetIO().WantTextInput && !ui_keys_) {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const u8 mods = mods_of(SDL_GetModState());
    turbo_ = held(Act::FastForward, keys);
    const bool rewind_held = held(Act::Rewind, keys) && rewind_down_ms_;
    rewinding_ = (rewind_held || test_rewind_) && rewind_on_ && powered_;
    for (size_t i = 0; i < kControlCount && capture_control_ < 0; i++)
      if (kControls[i].act == Act::Button)
        for (const KeyBind& k : keys_[i])
          if (key_down(k, keys, mods, true)) mask |= kControls[i].button;
  }
  if (pad_ && capture_control_ < 0) {
    for (size_t i = 0; i < kControlCount; i++)
      if (kControls[i].act == Act::Button && pad_btn_[i] != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad_, pad_btn_[i]))
        mask |= kControls[i].button;
    // (Fast forward and rewind held on the gamepad; the left stick is also the d-pad.)
    turbo_ = turbo_ || held(Act::FastForward, nullptr);
    const bool rewind_pad = held(Act::Rewind, nullptr) && rewind_down_ms_;
    rewinding_ = rewinding_ || (rewind_pad && rewind_on_ && powered_);
    const int lx = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX);
    const int ly = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY);
    if (lx < -16000) mask |= kBtnLeft;
    if (lx > 16000) mask |= kBtnRight;
    if (ly < -16000) mask |= kBtnUp;
    if (ly > 16000) mask |= kBtnDown;
  }
  turbo_ = turbo_ || test_turbo_ || ff_toggle_;
  update_cursor();
  if (console_press_ && SDL_GetTicks() >= console_press_until_) console_press_ = 0;
  live_input_.buttons = mask | test_buttons_ | console_press_;
  live_input_.power_off = switched_off_;
  live_input_.touch = touch_down_ || cursor_down_;
  // (The mouse first, if both press.)
  live_input_.x = u8(std::clamp(touch_down_ ? touch_x_ : int(cursor_x_), 0, Machine::kWidth - 1));
  live_input_.y = u8(std::clamp(touch_down_ ? touch_y_ : int(cursor_y_), 0, Machine::kHeight - 1));
}

// The gamepad's stylus. The stick's speed grows with the square of its tilt
// past a dead zone, for fine control near the centre; full tilt crosses the
// screen in about 0.8 s of real time (at any emulation speed).
void App::update_cursor() {
  const u64 now = SDL_GetTicksNS();
  const float dt = cursor_ns_ ? std::min(0.1f, float(now - cursor_ns_) * 1e-9f) : 0.0f;
  cursor_ns_ = now;
  float sx = test_stick_x_, sy = test_stick_y_;
  bool down = test_stylus_;
  if (capture_control_ < 0) {
    const bool* keys = ImGui::GetIO().WantTextInput ? nullptr : SDL_GetKeyboardState(nullptr);
    down = down || held(Act::Touch, keys);
    if (pad_) {
      sx += SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
      sy += SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
      down = down || SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    }
  }
  constexpr float kDead = 0.2f, kSpeed = 200.0f;  // (LCD pixels per second)
  const float mag = std::hypot(sx, sy);
  bool used = down;
  if (mag > kDead && powered_) {
    const float t = std::min(1.0f, (mag - kDead) / (1.0f - kDead));
    const float v = kSpeed * t * t * dt / mag;
    cursor_x_ = std::clamp(cursor_x_ + sx * v, 0.0f, Machine::kWidth - 0.01f);
    cursor_y_ = std::clamp(cursor_y_ + sy * v, 0.0f, Machine::kHeight - 0.01f);
    used = true;
  }
  cursor_down_ = down && powered_;
  if (used && powered_) cursor_used_ms_ = SDL_GetTicks();
}

// One emulated frame, with the input of the movie being played or the live
// input (recorded when recording a movie).
void App::sync_rom_view() {
  if (m_.bios_image().data() == rom_bios_ && m_.cart_image().data() == rom_cart_ && m_.cart_image().size() == rom_cart_size_) return;
  rom_bios_ = m_.bios_image().data();
  rom_cart_ = m_.cart_image().data();
  rom_cart_size_ = m_.cart_image().size();
  interp_.set_rom(m_.rom_view());
}

bool App::step_frame() {
  sync_rom_view();
  const u64 f = m_.frame_count();
  InputFrame in = live_input_;
  if (movie_mode_ == MovieMode::Playing) {
    if (f >= movie_.frames.size()) {
      movie_mode_ = MovieMode::Finished;
      running_ = false;
      advance_ = 0;
      status_msg_ = "Movie finished";
      status_time_ = SDL_GetTicks();
      return false;
    }
    in = movie_.frames[f];
  } else if (movie_mode_ == MovieMode::Recording) {
    movie_.frames.resize(std::min<size_t>(movie_.frames.size(), f));
    movie_.frames.resize(f);  // (blank input for frames never recorded)
    movie_.frames.push_back(in);
  }
  // (The log follows the machine: going back truncates it.)
  if (f < input_log_base_ || f > input_log_base_ + input_log_.size()) { input_log_.clear(); input_log_base_ = f; }
  input_log_.resize(size_t(f - input_log_base_));
  input_log_.push_back(in);
  while (input_log_.size() > 60u * 60 * 60) { input_log_.pop_front(); input_log_base_++; }  // (an hour)
  apply_input(m_, in);
  last_input_ = in;
  if (!m_.run_frame()) {
    running_ = false;
    const auto& c = m_.cpu();
    set_error(c.stop_reason() == arc::Cpu::Stop::Breakpoint ? "Breakpoint hit" : c.stop_message());
    disasm_addr_ = c.pc();
    return false;
  }
  lag_count_ += m_.lagged();
  if (rewind_on_) {
    // Fast forward: snapshots by wall time (as often as at normal speed), not
    // every few frames; each costs a few milliseconds, which at 40x speed
    // would take most of the time. Going back to any frame in between still
    // works (the nearest snapshot, then the input log).
    if (!turbo_) rewind_.frame(m_);
    else if (const u64 t = SDL_GetTicks(); t - rewind_ff_ticks_ >= 1000 * u64(rewind_.interval) / 60) {
      rewind_ff_ticks_ = t;
      rewind_.mark(m_);
    }
  }
  fps_frames_++;
  total_frames_++;
  interp_.push(m_.framebuffer(), m_.frame_count(), m_.draw_capture().latest());
  return true;
}

// Holding the rewind key steps back through the snapshots at the speed they
// were taken (faster with fast forward held).
void App::rewind_step(double dt) {
  rewind_accum_ += dt * Machine::kFps / rewind_.interval * (turbo_ ? 4 : 1);
  int steps = std::min(int(rewind_accum_), 8);
  rewind_accum_ -= std::floor(rewind_accum_);
  bool stepped = false;
  while (steps-- > 0 && rewind_.step_back(m_)) stepped = true;
  if (!stepped) return;
  interp_.reset();
  m_.cpu().clear_stop();
  // A movie follows the machine back: recording, the input after this point
  // is dropped (one rerecord per rewind); playing, it plays on from here.
  const u64 f = m_.frame_count();
  if (movie_mode_ == MovieMode::Recording) {
    movie_.frames.resize(std::min<size_t>(movie_.frames.size(), f));
    if (!rewind_session_) movie_.rerecords++;
  } else if (movie_mode_ == MovieMode::Playing || movie_mode_ == MovieMode::Finished) {
    movie_mode_ = f < movie_.frames.size() ? MovieMode::Playing : MovieMode::Finished;
  }
  rewind_session_ = true;
}

// Frame back: exactly one frame, paused. The latest snapshot
// at or before that frame is restored and the frames after it run again
// with the input they had (the movie's, or the logged live input).
bool App::frame_reverse() {
  if (!powered_) return false;
  running_ = false;
  advance_ = 0;
  auto say = [&](const char* msg) { status_msg_ = msg; status_time_ = SDL_GetTicks(); };
  if (!rewind_on_) { say("Frame back needs Emulation > Rewind"); return false; }
  const u64 f = m_.frame_count();
  if (f == 0) return false;
  const u64 target = f - 1;
  const bool movie = movie_mode_ != MovieMode::None;
  auto input_at = [&](u64 k, InputFrame& in) {
    if (movie) {
      if (k >= movie_.frames.size()) return false;
      in = movie_.frames[k];
      return true;
    }
    if (k < input_log_base_ || k >= input_log_base_ + input_log_.size()) return false;
    in = input_log_[size_t(k - input_log_base_)];
    return true;
  };
  u64 at;
  InputFrame in;
  bool ok = rewind_.find(target, &at);
  for (u64 k = at; ok && k < target; k++) ok = input_at(k, in);
  if (!ok) { say("No earlier frame to go back to"); return false; }
  if (!rewind_.restore(target, m_)) { say("No earlier frame to go back to"); return false; }
  m_.cpu().clear_stop();
  sync_rom_view();
  for (u64 k = at; k < target; k++) {
    input_at(k, in);
    apply_input(m_, in);
    last_input_ = in;
    if (!m_.run_frame()) break;
    rewind_.frame(m_);
  }
  static std::vector<s16> silence(Machine::kAudioHz);
  m_.read_audio(silence.data(), silence.size());
  interp_.reset();
  interp_.push(m_.framebuffer(), m_.frame_count(), m_.draw_capture().latest());
  // A movie follows, as with rewinding: recording, the input from here on
  // is dropped (a rerecord); playing, it plays on from here.
  if (movie_mode_ == MovieMode::Recording) {
    movie_.frames.resize(std::min<size_t>(movie_.frames.size(), m_.frame_count()));
    movie_.rerecords++;
  } else if (movie_mode_ == MovieMode::Playing || movie_mode_ == MovieMode::Finished) {
    movie_mode_ = m_.frame_count() < movie_.frames.size() ? MovieMode::Playing : MovieMode::Finished;
  }
  return true;
}

void App::emulate(double dt) {
  static std::vector<s16> buf(Machine::kAudioHz);
  if (rewinding_) {
    rewind_held_ = true;
    rewind_step(dt);
    m_.read_audio(buf.data(), buf.size());  // (silent)
    frame_accum_ = 0;
    return;
  }
  rewind_session_ = false;
  rewind_accum_ = 0;
  if (powered_ && !running_ && advance_ > 0) {  // frame advance
    advance_ = 0;
    m_.cpu().clear_stop();
    step_frame();
    m_.read_audio(buf.data(), buf.size());  // (no sound while stepping)
  }
  if (!powered_ || !running_) { frame_accum_ = 0; return; }
  const double speed = turbo_ ? ff_speed_ : speed_;
  int frames;
  if (speed == 0) {
    frames = 1000;  // unthrottled: bounded by the time budget below
    frame_accum_ = 0;
  } else {
    frame_accum_ += dt * Machine::kFps * speed;
    frames = int(frame_accum_);
    frame_accum_ -= frames;
    const int cap = std::max(2, int(std::ceil(4 * speed)));
    if (frames > cap) { frames = cap; frame_accum_ = 0; }  // don't spiral after a stall
  }
  const u64 budget_end = SDL_GetPerformanceCounter() + SDL_GetPerformanceFrequency() * 12 / 1000;

  for (int i = 0; i < frames; i++) {
    if (!step_frame()) break;
    if (speed == 0 && SDL_GetPerformanceCounter() >= budget_end) break;
  }

  // Audio plays at every speed, faster and higher (or slower and lower) with
  // the game, like tape. Fast forward plays it at the speed achieved, up to
  // kMaxAudioSpeed; beyond that it is silent.
  constexpr double kMaxAudioSpeed = 4.0;
  const size_t n = m_.read_audio(buf.data(), buf.size());
  const double rate = speed > 0 ? speed : emu_fps_ / Machine::kFps;
  if (audio_ && n && rate > 0 && rate <= kMaxAudioSpeed) {
    const double ratio = std::max(rate, 0.1);
    if (std::abs(ratio - audio_ratio_) > 0.02) {
      SDL_SetAudioStreamFrequencyRatio(audio_, float(ratio));
      audio_ratio_ = ratio;
    }
    // Keep latency bounded (~0.2 s): drop audio if the device queue is deep.
    if (SDL_GetAudioStreamQueued(audio_) < int(Machine::kAudioHz / 5 * sizeof(s16) * std::max(1.0, ratio)))
      SDL_PutAudioStreamData(audio_, buf.data(), int(n * sizeof(s16)));
  }

  fps_time_ += dt;
  if (fps_time_ >= 0.5) {
    emu_fps_ = fps_frames_ / fps_time_;
    fps_frames_ = 0;
    fps_time_ = 0;
  }
}

// ---------------------------------------------------------------------------
// Tool-assisted play
// ---------------------------------------------------------------------------

void App::start_movie(const std::string& path, PendingFile::Kind kind) {
  if (!powered_) return;
  stop_movie();
  std::string err;
  const bool record = kind != PendingFile::MoviePlay;
  flush_saves();
  Movie mv;
  if (record) {
    // From power-on with the current save data (or none).
    if (saves_suspended_) reload_saves();
    mv = Movie::from_machine(m_);
    if (kind == PendingFile::MovieRecordBlank) mv.cart_eeprom.fill(0);
  } else if (!mv.load(path, &err)) {
    set_error("Movie: " + err);
    return;
  }
  user_settings_ = Movie::from_machine(m_);
  if (!mv.start(m_, &err)) { set_error("Movie: " + err); return; }
  if (record && !mv.save(path, &err)) { set_error("Movie: " + err); return; }
  movie_ = std::move(mv);
  movie_path_ = path;
  movie_mode_ = record ? MovieMode::Recording : MovieMode::Playing;
  // The movie runs on its own EEPROM contents: nothing is written to the
  // save files until the next reset or cartridge change.
  saves_suspended_ = true;
  rewind_.clear();
  interp_.reset();
  lag_count_ = 0;
  frame_accum_ = 0;
  running_ = true;
  error_.clear();
  status_msg_ = std::string(record ? "Recording " : "Playing ") + fs::path(path).filename().string();
  status_time_ = SDL_GetTicks();
}

void App::stop_movie() {
  if (movie_mode_ == MovieMode::None) return;
  std::string err;
  if (movie_mode_ == MovieMode::Recording) {
    movie_.frames.resize(std::min<size_t>(movie_.frames.size(), m_.frame_count()));
    if (!movie_.save(movie_path_, &err)) set_error("Movie: " + err);
  }
  movie_mode_ = MovieMode::None;
  m_.timing = user_settings_.timing;  // the user's settings again
  m_.apply_timing();
  m_.cpu().idle_skip = user_settings_.idle_skip;
  m_.allow_unsigned = user_settings_.allow_unsigned || m_.downloadable() || m_.cart_patch().applied;
  m_.stub_missing_services = user_settings_.stub_missing_services;
  m_.auto_calibrate = user_settings_.auto_calibrate;
}

// Read-only plays the movie; read-write records from the current frame on
// (discarding the movie's later input), counting a rerecord.
void App::set_read_only(bool read_only) {
  std::string err;
  if (read_only && movie_mode_ == MovieMode::Recording) {
    if (!movie_.save(movie_path_, &err)) set_error("Movie: " + err);
    movie_mode_ = m_.frame_count() < movie_.frames.size() ? MovieMode::Playing : MovieMode::Finished;
  } else if (!read_only && (movie_mode_ == MovieMode::Playing || movie_mode_ == MovieMode::Finished)) {
    if (m_.frame_count() > movie_.frames.size()) {
      set_error("Past the end of the movie: load a state from within it to record");
      return;
    }
    movie_.frames.resize(m_.frame_count());
    movie_.rerecords++;
    movie_mode_ = MovieMode::Recording;
  }
}

void App::cycle_speed(int dir) {
  const double* begin = std::begin(kSpeeds);
  const double* end = std::end(kSpeeds);
  const double cur = speed_ == 0 ? 1e9 : speed_;
  if (dir > 0) {
    const double* it = std::upper_bound(begin, end, cur);
    speed_ = it == end ? 0 : *it;  // past the fastest: unthrottled
  } else {
    const double* it = std::lower_bound(begin, end, cur);
    speed_ = it == begin ? *begin : *(it - 1);
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), speed_ == 0 ? "Speed: unthrottled" : "Speed: %gx", speed_);
  status_msg_ = buf;
  status_time_ = SDL_GetTicks();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

// Fullscreen has no status bar: its latest message or error shows briefly at
// the bottom left, over the picture.
void App::draw_toast() {
  const bool msg = !status_msg_.empty() && SDL_GetTicks() - status_time_ < 2000;
  if (!msg && !error_shown()) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 12, vp->Pos.y + vp->Size.y - 12), ImGuiCond_Always, ImVec2(0, 1));
  ImGui::SetNextWindowBgAlpha(0.8f);
  ImGui::Begin("##toast", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  if (msg) ImGui::TextUnformatted(status_msg_.c_str());
  else ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", error_.c_str());
  ImGui::End();
}

// While nothing runs: the BIOS prompt until a BaseROM is chosen, then the game list.
void App::draw_idle() {
  if (powered_) return;
  if (!m_.has_bios()) draw_bios_prompt();
  else if (show_game_list_ && !game_dirs().empty()) draw_game_list();
  else if (show_game_list_ && !console_off_) draw_welcome();
}

// A BaseROM but no game folders yet: where to go next.
void App::draw_welcome() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + (menu_h_ + vp->Size.y - status_h_) / 2), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::min(520.0f, vp->Size.x - 32), vp->Size.y));
  ImGui::Begin("##welcome", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::TextUnformatted("Your games");
  ImGui::Separator();
  ImGui::TextWrapped("Add the folder where your cartridge dumps are, and they are listed here while "
                     "nothing is running. Or load one file at a time.");
  ImGui::Spacing();
  if (ImGui::Button("Add Game Folder...")) open_folder_dialog();
  ImGui::SameLine();
  if (ImGui::Button("Load ROM...")) open_dialog(PendingFile::Cart);
  ImGui::TextDisabled("Later: folders in Tools > Settings > Paths, single files in File > Load ROM.");
  ImGui::End();
}

// No BaseROM yet (or the chosen one is missing): nothing can run without
// one, so ask for it.
void App::draw_bios_prompt() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + (menu_h_ + vp->Size.y - status_h_) / 2), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::min(520.0f, vp->Size.x - 32), vp->Size.y));
  ImGui::Begin("##bios_prompt", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::TextUnformatted("Choose a BaseROM");
  ImGui::Separator();
  ImGui::TextWrapped("leapemu runs the Leapster's own system ROM (the BaseROM, often called the BIOS): "
                     "every game, homebrew included, is started by it. Use a dump of your own console's; "
                     ".bin, .zip and .wav-wrapped dumps all work.");
  ImGui::Spacing();
  if (ImGui::Button("Browse...")) open_dialog(PendingFile::Bios);
  ImGui::SameLine();
  ImGui::TextDisabled("or drag the file onto this window");
  if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", error_.c_str());
  const std::vector<std::string> bioses = recent("recent_bios");
  if (!bioses.empty()) {
    ImGui::Spacing();
    ImGui::TextDisabled("Used before:");
    for (size_t i = 0; i < bioses.size(); i++) {
      std::error_code ec;
      const bool there = fs::exists(bioses[i], ec);
      ImGui::BeginDisabled(!there);
      if (ImGui::Selectable((bioses[i] + (there ? "" : "  (missing)") + "##b" + std::to_string(i)).c_str())) open_bios(bioses[i]);
      ImGui::EndDisabled();
    }
  }
  ImGui::End();
}

void App::draw_ui() {
  ui_keys_ = ui_keys_next_;
  ui_keys_next_ = false;
  if (!fullscreen_) {
    fs_menu_ = false;
    draw_menu();
    draw_status();
    draw_idle();
    if (!rom_windows_.empty()) draw_rom_windows();
    if (pending_scale_ && menu_h_ > 0 && status_h_ > 0) {  // (--scale: once the bars' heights are known)
      SDL_SetWindowSize(window_, Machine::kWidth * pending_scale_, int(Machine::kHeight * pending_scale_ + menu_h_ + status_h_));
      pending_scale_ = 0;
    }
  } else {
    menu_h_ = status_h_ = 0;
    draw_idle();  // (the game list and BIOS prompt fill the screen)
    // The menu bar shows over the picture while the mouse is at the top edge
    // (or one of its menus is open).
    const ImVec2 mouse = ImGui::GetIO().MousePos;  // (off the window: hidden)
    const bool in = ImGui::IsMousePosValid(&mouse);
    const float edge = ImGui::GetFrameHeight();
    if ((in && mouse.y <= 2) || (fs_menu_ && ((in && mouse.y <= edge * 1.5f) || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)))) {
      draw_menu();
      fs_menu_ = true;
    } else {
      fs_menu_ = false;
    }
    if (!rom_windows_.empty()) draw_rom_windows();
    draw_toast();
    menu_h_ = status_h_ = 0;  // (the picture keeps the whole screen)
  }
  if (show_cpu_) draw_cpu();
  if (show_disasm_) draw_disasm();
  if (show_memory_) draw_memory();
  if (show_uart_) draw_uart();
  if (show_about_) draw_about();
  if (show_settings_) draw_settings();
}

void App::draw_menu() {
  if (!ImGui::BeginMainMenuBar()) return;
  menu_h_ = ImGui::GetWindowSize().y;
  auto key = [&](Act a, u32 arg = 0) {  // (the shortcut column follows the bindings)
    static std::string k;
    k = shortcut(a, arg);
    return k.empty() ? nullptr : k.c_str();
  };
  const bool game = m_.has_cart() && !cart_rom_.empty();
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Load ROM...", key(Act::LoadRom))) open_dialog(PendingFile::Cart);
    if (ImGui::MenuItem("Load Unsigned ROM...")) open_dialog(PendingFile::CartUnsigned);
    ImGui::SetItemTooltip("For homebrew, prototypes and slightly damaged dumps: the BaseROM only boots\n"
                          "cartridges signed by LeapFrog with a correct checksum. This skips those checks\n"
                          "for this cartridge (in memory only).");
    const std::vector<std::string> roms = recent("recent_rom");
    if (ImGui::BeginMenu("Recent", !roms.empty())) {
      for (size_t i = 0; i < roms.size(); i++) {
        const bool unsig = roms[i].rfind("unsigned:", 0) == 0;
        const std::string path = unsig ? roms[i].substr(9) : roms[i];
        const std::string label = fs::path(path).filename().string() + (unsig ? "  (unsigned)" : "") + "##" + std::to_string(i);
        if (ImGui::MenuItem(label.c_str())) open_cart(path, unsig);
        ImGui::SetItemTooltip("%s", path.c_str());
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Clear List")) {
        set_list("recent_rom", {});
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Game Properties...", nullptr, false, game)) {
      GameEntry g;
      if (read_game_entry(cart_rom_, &g)) { apply_own_rating(g); open_rom_window(g, RomWindow::Tab::Info); }
      else set_error("Cannot read " + cart_rom_);
    }
    ImGui::SetItemTooltip("The running game's file, contents (assets), and hex view with patches.");
    ImGui::Separator();
    if (ImGui::MenuItem("Open Data Folder")) SDL_OpenURL(("file://" + pref_dir_.string()).c_str());
    ImGui::SetItemTooltip("Settings, save states, screenshots and the console's own memory.");
    if (ImGui::MenuItem("Exit", key(Act::Quit))) quit_ = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Emulation")) {
    if (ImGui::MenuItem(running_ || !powered_ ? "Pause" : "Resume", key(Act::Pause), false, powered_)) running_ = !running_;
    if (ImGui::MenuItem("Reset", key(Act::Reset), false, powered_)) power_cycle();
    if (ImGui::MenuItem("Stop", key(Act::Stop), false, powered_)) stop_game();
    ImGui::SetItemTooltip("Closes the game at once, back to the game list. Its save is written as it is;\n"
                          "settings the console would save while turning off are not.");
    if ((powered_ && m_.powered_off()) || console_off_) {
      if (ImGui::MenuItem("Power On", key(Act::Power))) toggle_power();
      ImGui::SetItemTooltip("Switches the console on again, with the same game.");
    } else {
      if (ImGui::MenuItem("Power Switch Off", key(Act::Power), false, powered_ && !switched_off_)) toggle_power();
      ImGui::SetItemTooltip("Slides the console's power switch: it saves its settings, plays its\n"
                            "power-off animation and turns off. It also turns itself off after\n"
                            "a long time without input, like the real one.");
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Speed")) {
      for (double sp : kSpeeds) {
        char label[32];
        std::snprintf(label, sizeof(label), "%gx", sp);
        if (ImGui::MenuItem(label, nullptr, speed_ == sp)) speed_ = sp;
      }
      if (ImGui::MenuItem("Unthrottled", nullptr, speed_ == 0)) speed_ = 0;
      ImGui::Separator();
      ImGui::MenuItem("Fast Forward", key(Act::FastForwardToggle), &ff_toggle_);
      if (const std::string k = shortcut(Act::FastForward); !k.empty()) ImGui::SetItemTooltip("Also while %s is held.", k.c_str());
      if (const std::string a = shortcut(Act::Slower), b = shortcut(Act::Faster); !a.empty() && !b.empty())
        ImGui::TextDisabled("%s / %s : slower / faster", a.c_str(), b.c_str());
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Rewind", key(Act::Rewind), &rewind_on_)) {
      settings_.kv["rewind"] = rewind_on_ ? "1" : "0";
      if (!rewind_on_) rewind_.clear();
    }
    ImGui::SetItemTooltip("Hold %s to go back in time (with fast forward: faster).\n"
                          "Keeps the last %.0f s (%.1f MB).", shortcut(Act::Rewind).c_str(), rewind_.seconds(), rewind_.bytes() / 1e6);
    if (ImGui::MenuItem("Frame Back", key(Act::FrameBack), false, powered_ && rewind_on_)) frame_reverse();
    ImGui::SetItemTooltip("Pauses, then goes back exactly one frame each time (held: repeats).\n"
                          "Needs Rewind (the history it steps back through).");
    if (ImGui::MenuItem("Frame Advance", key(Act::FrameAdvance), false, powered_)) { running_ = false; advance_++; }
    ImGui::SetItemTooltip("Pauses, then runs one frame each time (held: repeats).");
    ImGui::Separator();
    if (ImGui::BeginMenu("Save State", powered_)) {
      if (ImGui::MenuItem("Quick Save", key(Act::QuickSave))) save_slot(0);
      for (u32 i = 1; i <= 4; i++)
        if (ImGui::MenuItem(("Slot " + std::to_string(i)).c_str(), key(Act::SaveSlot, i))) save_slot(int(i));
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Load State", powered_)) {
      if (ImGui::MenuItem("Quick Load", key(Act::QuickLoad), false, fs::exists(state_path(0)))) load_slot(0);
      for (u32 i = 1; i <= 4; i++)
        if (ImGui::MenuItem(("Slot " + std::to_string(i)).c_str(), key(Act::LoadSlot, i), false, fs::exists(state_path(int(i)))))
          load_slot(int(i));
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Console")) {
      if (ImGui::MenuItem("Start Without Cartridge", nullptr, false, m_.has_bios())) boot_bios();
      ImGui::SetItemTooltip("Starts the console with no cartridge: its own menu and settings.");
      if (ImGui::MenuItem("Recalibrate Touch", nullptr, false, powered_)) {
        stop_movie();
        if (saves_suspended_) reload_saves();
        m_.request_calibration();
        power_cycle();  // (the erased calibration is saved and the BaseROM asks for a new one)
      }
      ImGui::Separator();
      ImGui::TextDisabled("The console's own buttons");
      for (size_t i = 0; i < kControlCount; i++) {
        const u32 b = kControls[i].button;
        if (kControls[i].act != Act::Button ||
            !(b == kBtnBrightDown || b == kBtnBrightUp || b == kBtnContrast || b == kBtnVolDown || b == kBtnVolUp))
          continue;
        const std::string k = shortcut(Act::Button, b);
        std::string label = kControls[i].label;  // (Title Case, as menus are)
        for (size_t c = 0; c < label.size(); c++)
          if (c == 0 || label[c - 1] == ' ') label[c] = char(std::toupper(u8(label[c])));
        if (ImGui::MenuItem(label.c_str(), k.empty() ? nullptr : k.c_str(), false, powered_)) {
          console_press_ = b;  // (pressed briefly, as with a finger)
          console_press_until_ = SDL_GetTicks() + 150;
        }
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  // (Each part: its submenus, then its toggles.)
  if (ImGui::BeginMenu("View")) {
    if (ImGui::BeginMenu("Window Size")) {
      int ww = 0, wh = 0;
      SDL_GetWindowSize(window_, &ww, &wh);
      for (int k = 1; k <= 8; k++) {
        char label[8];
        std::snprintf(label, sizeof(label), "%dx", k);
        const int w = Machine::kWidth * k, h = int(Machine::kHeight * k + menu_h_ + status_h_);
        if (ImGui::MenuItem(label, nullptr, !fullscreen_ && ww == w && wh == h)) {
          if (fullscreen_) toggle_fullscreen();
          SDL_SetWindowSize(window_, w, h);
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Filter")) {
      struct { Filter f; const char* name; const char* tip; } kFilters[] = {
          {Filter::Nearest, "Nearest (pixel perfect)", "Each pixel a sharp square."},
          {Filter::Sharp, "Sharp Bilinear", "Sharp squares, smoothed only where the size is not a whole multiple."},
          {Filter::Bilinear, "Bilinear", "Smooth but soft."},
          {Filter::Bicubic, "Bicubic", "Smooth and sharper than bilinear: suits the Flash games' cartoon art and photos."},
          {Filter::Lanczos, "Lanczos", "Sharper still, with faint halos along hard edges."},
          {Filter::Mmpx, "MMPX (pixel art)", "Rebuilds smooth diagonals and curves from pixel art (Sonic X, Go Diego Go).\n"
                                            "Not for anti-aliased art, photos or small text."},
          {Filter::Lcd, "LCD", "The handheld's screen: a faint grid between pixels and its slow response."},
      };
      for (const auto& f : kFilters) {
        if (ImGui::MenuItem(f.name, nullptr, filter_ == f.f)) filter_ = f.f;
        ImGui::SetItemTooltip("%s", f.tip);
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Fullscreen", key(Act::Fullscreen), fullscreen_)) toggle_fullscreen();
    ImGui::MenuItem("Integer Scaling", nullptr, &integer_scale_);
    ImGui::SetItemTooltip("Only whole multiples of 160x160 (2x, 3x...), so every pixel is the same size;\n"
                          "leaves a border when the window is not an exact multiple.");
    if (ImGui::MenuItem("Vector Scale to Window", nullptr, &vector_scale_)) {
      settings_.kv["vector_scale"] = vector_scale_ ? "1" : "0";
      update_display_mode();
    }
    ImGui::SetItemTooltip("Redraws Flash content (most menus, cutscenes and the Flash games) as vectors\n"
                          "at the window's resolution, instead of enlarging the 160x160 image.\n"
                          "Anything a game draws itself is shown as the 160x160 image.\n"
                          "Paused (the 160x160 image) while fast forwarding or above 1x speed.");
    if (ImGui::MenuItem("Interpolation (hack, buggy)", nullptr, &smoothing_)) {
      settings_.kv["interp"] = smoothing_ ? "1" : "0";
      update_display_mode();
    }
    ImGui::SetItemTooltip("Smooths motion between game frames by redrawing the game's own\n"
                          "objects at in-between positions (Flash content, Sonic X, Go Diego Go),\n"
                          "or by estimating motion from the pixels otherwise.\n"
                          "Display only; adds about one game frame of lag. Paused while fast\n"
                          "forwarding or above 1x speed.\n"
                          "Buggy: parts of characters can still slide, stretch or pop.");
    ImGui::Separator();
    if (ImGui::BeginMenu("Volume")) {
      for (int v : {100, 75, 50, 25}) {
        char label[16];
        std::snprintf(label, sizeof(label), "%d%%", v);
        if (ImGui::MenuItem(label, nullptr, std::abs(volume_ * 100 - float(v)) < 0.5f)) {
          volume_ = float(v) / 100.0f;
          settings_.kv["volume"] = std::to_string(volume_);
          set_muted(false);
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Mute", key(Act::Mute), muted_)) set_muted(!muted_);
    ImGui::Separator();
    if (ImGui::MenuItem("Screenshot", key(Act::Screenshot), false, powered_)) screenshot();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Tools")) {
    if (ImGui::MenuItem("Settings...", key(Act::Settings))) show_settings_ = true;
    ImGui::Separator();
    if (ImGui::BeginMenu("Movies")) {
      const bool active = movie_mode_ != MovieMode::None;
      if (ImGui::MenuItem("Record...", nullptr, false, powered_)) save_dialog(PendingFile::MovieRecord);
      ImGui::SetItemTooltip("Restarts the game and records every frame's input from power-on,\n"
                            "starting from the current save data.");
      if (ImGui::MenuItem("Record from Blank Save...", nullptr, false, powered_)) save_dialog(PendingFile::MovieRecordBlank);
      if (ImGui::MenuItem("Play...", nullptr, false, powered_)) open_dialog(PendingFile::MoviePlay);
      if (ImGui::MenuItem("Stop", nullptr, false, active)) {
        stop_movie();
        // (The game goes on with the movie's save data, which must not reach
        // the real save: saving resumes after a reset, which reloads it.)
        status_msg_ = saves_suspended_ ? "Movie stopped. Saving resumes after a reset" : "Movie stopped";
        status_time_ = SDL_GetTicks();
      }
      bool read_only = movie_mode_ == MovieMode::Playing || movie_mode_ == MovieMode::Finished;
      if (ImGui::MenuItem("Read-Only", nullptr, &read_only, active)) set_read_only(read_only);
      ImGui::SetItemTooltip("Read-only: the movie plays back, and loading a state keeps its input.\n"
                            "Off: input is recorded, and loading a state rewinds the movie to it\n"
                            "(rerecording). States saved during a movie keep its input.");
      if (ImGui::MenuItem("Show Counters", nullptr, &show_counters_))
        settings_.kv["show_counters"] = show_counters_ ? "1" : "0";
      if (active) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", fs::path(movie_path_).filename().string().c_str());
        ImGui::TextDisabled("%zu frames, %llu rerecords", movie_.frames.size(), static_cast<unsigned long long>(movie_.rerecords));
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Debugger")) {
      ImGui::MenuItem("CPU", nullptr, &show_cpu_);
      ImGui::MenuItem("Disassembly", nullptr, &show_disasm_);
      ImGui::MenuItem("Memory", nullptr, &show_memory_);
      ImGui::MenuItem("UART Console", nullptr, &show_uart_);
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("Controls...")) { show_settings_ = true; settings_page_ = SettingsPage::Controls; }
    if (ImGui::MenuItem("Shortcuts...")) { show_settings_ = true; settings_page_ = SettingsPage::Shortcuts; }
    ImGui::Separator();
    if (ImGui::MenuItem("About leapemu")) show_about_ = true;
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();
}

void App::toggle_fullscreen() {
  fullscreen_ = !fullscreen_;
  SDL_SetWindowFullscreen(window_, fullscreen_);
}

void App::set_touch_from_window(float wx, float wy) {
  int ww, wh, ow, oh;
  SDL_GetWindowSize(window_, &ww, &wh);
  SDL_GetCurrentRenderOutputSize(renderer_, &ow, &oh);
  const float px = wx * ow / float(ww), py = wy * oh / float(wh);
  const float scale = screen_rect_.w / Machine::kWidth;
  if (scale <= 0) return;
  touch_x_ = std::clamp(int((px - screen_rect_.x) / scale), 0, Machine::kWidth - 1);
  touch_y_ = std::clamp(int((py - screen_rect_.y) / scale), 0, Machine::kHeight - 1);
}

// Draw the emulated LCD as the window's content, between the menu and status
// bars. Drawn directly with SDL (not through ImGui, whose backend forces
// bilinear sampling) so "Nearest" is truly pixel perfect.
void App::render_screen() {
  int ow, oh, ww, wh;
  SDL_GetCurrentRenderOutputSize(renderer_, &ow, &oh);
  SDL_GetWindowSize(window_, &ww, &wh);
  const float px_per_unit = ww > 0 ? ow / float(ww) : 1.0f;
  const float top = menu_h_ * px_per_unit, bottom = status_h_ * px_per_unit;
  const float aw = float(ow), ah = std::max(0.0f, oh - top - bottom);
  const float fit = std::min(aw / Machine::kWidth, ah / Machine::kHeight);
  const float scale = (integer_scale_ && fit >= 1.0f) ? std::floor(fit) : fit;
  const float w = Machine::kWidth * scale, h = Machine::kHeight * scale;
  screen_rect_ = SDL_FRect{std::floor((aw - w) / 2), std::floor(top + (ah - h) / 2), w, h};
  if (!powered_) return;

  SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
  SDL_SetRenderViewport(renderer_, nullptr);
  SDL_SetRenderClipRect(renderer_, nullptr);
  const u32* image = m_.framebuffer();
  native_shown_ = false;
  if (interp_.mode() != FrameInterpolator::Mode::Off && !turbo_ && speed_ > 0 && speed_ <= 1.0) {
    const double t = double(m_.frame_count()) + frame_accum_;
    // Vector (Flash) content is redrawn at the window's resolution.
    const int k = vector_scale_ ? std::clamp(int(std::lround(screen_rect_.w / Machine::kWidth)), 1, 8) : 1;
    sync_rom_view();
    native_shown_ = interp_.layers(t, k, layers_);
    if (native_shown_) { render_native_layers(); return; }
    if (const u32* img = interp_.render(t)) image = img;
  }
  SDL_UpdateTexture(screen_tex_, nullptr, image, Machine::kWidth * 4);
  switch (filter_) {
    case Filter::Nearest:
      SDL_SetTextureScaleMode(screen_tex_, SDL_SCALEMODE_NEAREST);
      SDL_RenderTexture(renderer_, screen_tex_, nullptr, &screen_rect_);
      break;
    case Filter::Bilinear:
      SDL_SetTextureScaleMode(screen_tex_, SDL_SCALEMODE_LINEAR);
      SDL_RenderTexture(renderer_, screen_tex_, nullptr, &screen_rect_);
      break;
    case Filter::Bicubic: case Filter::Lanczos: case Filter::Mmpx: case Filter::Lcd:
      render_filtered(image, scale);
      break;
    case Filter::Sharp: {
      // Integer nearest-neighbour prescale, then bilinear for the remainder:
      // crisp pixels without uneven pixel widths at non-integer sizes.
      const int k = std::max(1, int(std::floor(scale)));
      if (!sharp_tex_ || sharp_k_ != k) {
        if (sharp_tex_) SDL_DestroyTexture(sharp_tex_);
        sharp_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                       Machine::kWidth * k, Machine::kHeight * k);
        SDL_SetTextureScaleMode(sharp_tex_, SDL_SCALEMODE_LINEAR);
        sharp_k_ = k;
      }
      SDL_SetTextureScaleMode(screen_tex_, SDL_SCALEMODE_NEAREST);
      SDL_SetRenderTarget(renderer_, sharp_tex_);
      SDL_RenderTexture(renderer_, screen_tex_, nullptr, nullptr);
      SDL_SetRenderTarget(renderer_, nullptr);
      SDL_RenderTexture(renderer_, sharp_tex_, nullptr, &screen_rect_);
      break;
    }
  }
}

// The gamepad's stylus cursor, over the screen: a crosshair, white with a dark
// outline (yellow while pressing). It fades out once unused for a while.
void App::render_pad_cursor() {
  if (!powered_ || m_.powered_off() || !cursor_used_ms_) return;
  const u64 idle = SDL_GetTicks() - cursor_used_ms_;
  if (idle >= kCursorIdleMs && !cursor_down_) return;
  const float alpha = cursor_down_ ? 1.0f : std::min(1.0f, float(kCursorIdleMs - idle) / 500.0f);
  const float scale = screen_rect_.w / Machine::kWidth;
  const float cx = screen_rect_.x + (std::floor(cursor_x_) + 0.5f) * scale;
  const float cy = screen_rect_.y + (std::floor(cursor_y_) + 0.5f) * scale;
  const float arm = std::clamp(scale * 5.0f, 10.0f, 48.0f);  // arm length
  const float th = std::clamp(std::round(scale * 0.75f), 2.0f, 6.0f);  // thickness
  const float gap = std::max(th, scale * 0.75f);  // (the centre is left open, so the pixel under it shows)
  auto cross = [&](float grow, u8 r, u8 g, u8 b) {
    const float t = th + 2 * grow, a = arm + grow;
    const SDL_FRect rs[4] = {
        {cx - a, cy - t / 2, a - gap + grow, t}, {cx + gap - grow, cy - t / 2, a - gap + grow, t},
        {cx - t / 2, cy - a, t, a - gap + grow}, {cx - t / 2, cy + gap - grow, t, a - gap + grow}};
    SDL_SetRenderDrawColor(renderer_, r, g, b, u8(std::lround(alpha * 255)));
    SDL_RenderFillRects(renderer_, rs, 4);
  };
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  SDL_SetRenderClipRect(renderer_, nullptr);
  cross(std::max(1.0f, std::round(th / 3)), 0, 0, 0);
  if (cursor_down_) cross(0, 255, 210, 40);
  else cross(0, 255, 255, 255);
}

// The LCD's brightness setting (the level the BaseROM writes to the LCD
// controller, 0x30 by default): drawn as a shade over the screen, so it applies
// to every filter and to redrawn vector content alike. Above the default the
// panel is lighter (dark areas fade towards white); below it, darker (light
// areas fade towards black). The strength per step is an estimate.
void App::render_lcd_tone() {
  if (!powered_ || m_.powered_off()) return;
  const int d = m_.lcd_brightness() - 0x30;
  if (d == 0) return;
  const float a = std::min(0.6f, std::abs(d) * 0.035f);
  const u8 v = d > 0 ? 255 : 0;
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer_, v, v, v, u8(std::lround(a * 255)));
  SDL_RenderFillRect(renderer_, &screen_rect_);
}

// Native capture serves both frame interpolation and vector scaling.
void App::update_display_mode() {
  const bool native = smoothing_ || vector_scale_;
  interp_.set_mode(native ? FrameInterpolator::Mode::Native : FrameInterpolator::Mode::Off);
  interp_.smoothing = smoothing_;
  m_.set_native_capture(native);
}

void App::render_filtered(const u32* image, float scale) {
  const int W = Machine::kWidth, H = Machine::kHeight;
  const u32* src = image;
  bool changed = filter_ != filter_done_ || filter_src_.size() != size_t(W) * H ||
                 std::memcmp(filter_src_.data(), image, size_t(W) * H * 4) != 0;
  if (changed) filter_src_.assign(image, image + size_t(W) * H);
  if (filter_ == Filter::Lcd) {
    // The panel responds slowly (it smears motion): each pixel moves towards
    // the new image with a time constant of ~40 ms.
    const u64 now = SDL_GetTicks();
    const float a = lcd_ghost_.empty() ? 1.0f : 1.0f - std::exp(-float(now - lcd_ticks_) / 40.0f);
    lcd_ticks_ = now;
    if (lcd_ghost_.size() != size_t(W) * H * 3) lcd_ghost_.assign(size_t(W) * H * 3, 0.0f);
    filter_tmp_.resize(size_t(W) * H);
    bool moving = false;
    for (size_t i = 0; i < size_t(W) * H; i++) {
      u32 o = 0xff000000u;
      for (int c = 0; c < 3; c++) {
        float& g = lcd_ghost_[i * 3 + size_t(c)];
        const float t = float((image[i] >> (16 - 8 * c)) & 255);
        g += (t - g) * a;
        if (std::abs(t - g) > 0.5f) moving = true; else g = t;
        o |= u32(std::lround(g)) << (16 - 8 * c);
      }
      filter_tmp_[i] = o;
    }
    changed = changed || moving;
    src = filter_tmp_.data();
  }
  // The filtered size: a whole multiple near the screen size (MMPX doubles).
  int k;
  if (filter_ == Filter::Mmpx) k = scale >= 3.5f ? 4 : 2;
  else if (filter_ == Filter::Lcd) k = std::clamp(int(std::floor(scale)), 1, 8);
  else k = std::clamp(int(std::ceil(scale)), 1, 8);
  if (k != filter_k_) changed = true;
  if (changed || !filtered_tex_) {
    switch (filter_) {
      case Filter::Bicubic: scale::bicubic(src, W, H, k, filter_out_); break;
      case Filter::Lanczos: scale::lanczos(src, W, H, k, filter_out_); break;
      case Filter::Lcd: scale::lcd(src, W, H, k, filter_out_); break;
      case Filter::Mmpx:
        scale::mmpx2x(src, W, H, filter_out_);
        if (k == 4) {
          filter_tmp_.swap(filter_out_);
          scale::mmpx2x(filter_tmp_.data(), W * 2, H * 2, filter_out_);
        }
        break;
      default: break;
    }
    filter_k_ = k;
    filter_done_ = filter_;
    if (!filtered_tex_ || filtered_w_ != W * k) {
      if (filtered_tex_) SDL_DestroyTexture(filtered_tex_);
      filtered_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W * k, H * k);
      SDL_SetTextureScaleMode(filtered_tex_, SDL_SCALEMODE_LINEAR);
      filtered_w_ = W * k;
    }
    SDL_UpdateTexture(filtered_tex_, nullptr, filter_out_.data(), W * k * 4);
  }
  SDL_RenderTexture(renderer_, filtered_tex_, nullptr, &screen_rect_);
}

// Native interpolation: each layer is drawn at its fractional position at the
// window's resolution, so motion stays smooth however large the screen is.
void App::render_native_layers() {
  const float s = screen_rect_.w / Machine::kWidth;
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderFillRect(renderer_, &screen_rect_);
  const bool software = filter_ == Filter::Bicubic || filter_ == Filter::Lanczos || filter_ == Filter::Mmpx;
  const bool smooth = filter_ != Filter::Nearest && filter_ != Filter::Sharp && filter_ != Filter::Lcd;
  if (layer_tex_.size() < layers_.size()) layer_tex_.resize(layers_.size());
  for (size_t i = 0; i < layers_.size(); i++) {
    const NativeLayer& L = layers_[i];
    if (L.w <= 0 || L.h <= 0) continue;
    LayerTex& lt = layer_tex_[i];
    SDL_Texture* tex = nullptr;
    if (software && L.texel >= 0.999f) {
      // A layer at the original resolution (planes, sprites, 160x160 Flash):
      // through the chosen filter, then smoothly to its size on screen.
      const int k = filter_ == Filter::Mmpx ? (s >= 3.5f ? 4 : 2) : std::clamp(int(std::ceil(s)), 1, 8);
      u64 h = 0xcbf29ce484222325ull;
      for (int n = 0; n < L.w * L.h; n++) h = (h ^ L.px[n]) * 0x100000001b3ull;
      if (!lt.ftex || lt.fhash != h || lt.fk != k || lt.ffilter != filter_ || lt.fw != L.w || lt.fh != L.h) {
        switch (filter_) {
          case Filter::Bicubic: scale::bicubic(L.px, L.w, L.h, k, filter_out_); break;
          case Filter::Lanczos: scale::lanczos(L.px, L.w, L.h, k, filter_out_); break;
          default:
            scale::mmpx2x(L.px, L.w, L.h, filter_out_);
            if (k == 4) {
              filter_tmp_.swap(filter_out_);
              scale::mmpx2x(filter_tmp_.data(), L.w * 2, L.h * 2, filter_out_);
            }
            break;
        }
        if (!lt.ftex || lt.fw != L.w || lt.fh != L.h || lt.fk != k) {
          if (lt.ftex) SDL_DestroyTexture(lt.ftex);
          lt.ftex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, L.w * k, L.h * k);
          if (lt.ftex) {
            SDL_SetTextureBlendMode(lt.ftex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(lt.ftex, SDL_SCALEMODE_LINEAR);
          }
        }
        if (lt.ftex) SDL_UpdateTexture(lt.ftex, nullptr, filter_out_.data(), L.w * k * 4);
        lt.fw = L.w;
        lt.fh = L.h;
        lt.fk = k;
        lt.fhash = h;
        lt.ffilter = filter_;
      }
      tex = lt.ftex;
    } else {
      if (!lt.tex || lt.w != L.w || lt.h != L.h) {
        if (lt.tex) SDL_DestroyTexture(lt.tex);
        lt.tex = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, L.w, L.h);
        if (!lt.tex) continue;
        SDL_SetTextureBlendMode(lt.tex, SDL_BLENDMODE_BLEND);
        lt.w = L.w; lt.h = L.h;
        lt.version = 0;
      }
      if (!L.version || L.version != lt.version) SDL_UpdateTexture(lt.tex, nullptr, L.px, L.w * 4);
      lt.version = L.version;
      // Layers redrawn at the screen's resolution (vector graphics) only need
      // smoothing for what is left of the scale; pixel layers follow the filter.
      const bool linear = filter_ == Filter::Bilinear || (smooth && L.texel < 0.999f);
      SDL_SetTextureScaleMode(lt.tex, linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
      tex = lt.tex;
    }
    if (!tex) continue;
    SDL_SetTextureAlphaMod(tex, L.opacity);
    const int cx0 = int(std::lround(screen_rect_.x + std::max(0, L.clip[0]) * s));
    const int cy0 = int(std::lround(screen_rect_.y + std::max(0, L.clip[1]) * s));
    const int cx1 = int(std::lround(screen_rect_.x + std::min(Machine::kWidth, L.clip[2]) * s));
    const int cy1 = int(std::lround(screen_rect_.y + std::min(Machine::kHeight, L.clip[3]) * s));
    if (cx1 <= cx0 || cy1 <= cy0) continue;
    const SDL_Rect clip{cx0, cy0, cx1 - cx0, cy1 - cy0};
    SDL_SetRenderClipRect(renderer_, &clip);
    const SDL_FRect dst{screen_rect_.x + L.x * s, screen_rect_.y + L.y * s, L.w * L.texel * s, L.h * L.texel * s};
    SDL_RenderTexture(renderer_, tex, nullptr, &dst);
  }
  SDL_SetRenderClipRect(renderer_, nullptr);
  if (filter_ == Filter::Lcd) {  // the panel's grid, fixed to the screen
    const int k = std::clamp(int(std::floor(s)), 1, 8);
    if (!grid_tex_ || grid_k_ != k) {
      if (grid_tex_) SDL_DestroyTexture(grid_tex_);
      std::vector<u32> grid;
      scale::lcd_grid(Machine::kWidth, Machine::kHeight, k, grid);
      grid_tex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, Machine::kWidth * k,
                                    Machine::kHeight * k);
      if (grid_tex_) {
        SDL_UpdateTexture(grid_tex_, nullptr, grid.data(), Machine::kWidth * k * 4);
        SDL_SetTextureBlendMode(grid_tex_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(grid_tex_, SDL_SCALEMODE_LINEAR);
      }
      grid_k_ = k;
    }
    if (grid_tex_) SDL_RenderTexture(renderer_, grid_tex_, nullptr, &screen_rect_);
  }
}

// Settings > Paths > Add Folder.
void App::open_folder_dialog(PendingFile::Kind kind) {
  struct Ctx { PendingFile* p; PendingFile::Kind kind; };
  static Ctx ctx;
  ctx = {&pending_, kind};
  static std::string start;  // (read by the dialog until it closes)
  start = kind == PendingFile::ExportFolder ? settings_.get("export_dir", "") : "";
  SDL_ShowOpenFolderDialog(
      [](void* user, const char* const* files, int) {
        auto* c = static_cast<Ctx*>(user);
        if (!files || !files[0]) return;
        std::lock_guard<std::mutex> lock(c->p->mu);
        c->p->path = files[0];
        c->p->kind = c->kind;
      },
      &ctx, window_, start.empty() ? nullptr : start.c_str(), false);
}

namespace {

// The game list's order of types: cartridges, downloads, prototypes, then
// anything else.
int type_rank(const std::string& t) {
  static const char* kOrder[] = {"Cartridge", "Download", "Prototype"};
  for (int i = 0; i < 3; i++)
    if (t == kOrder[i]) return i;
  return 3;
}

// Compatibility ratings, worst to best; a rating's squares (of five) are its
// index. (The fifth square is not given: nothing claims perfection.)
constexpr const char* kTiers[] = {"Untested", "Broken", "Intro", "Menus", "In-game"};
constexpr const char* kTierMeaning[] = {"Not tried yet", "Does not start", "Starts, then stops at the LeapFrog screen or intro",
                                        "The game's screens and menus work, not gameplay", "Gameplay works"};

int tier_score(const std::string& t) {
  for (int i = 0; i < 5; i++)
    if (t == kTiers[i]) return i;
  return 0;
}

ImVec4 tier_color(const std::string& t) {
  switch (tier_score(t)) {
    case 4: return ImVec4(0.45f, 0.85f, 0.40f, 1);  // In-game
    case 3: return ImVec4(0.90f, 0.80f, 0.35f, 1);  // Menus
    case 2: return ImVec4(0.95f, 0.55f, 0.30f, 1);  // Intro
    case 1: return ImVec4(0.95f, 0.35f, 0.35f, 1);  // Broken
    default: return ImVec4(0.60f, 0.60f, 0.60f, 1);  // Untested
  }
}

// A rating as five squares (filled: its score) and its name.
void draw_tier(const std::string& t, bool own) {
  const int score = tier_score(t);
  const ImVec4 col = tier_color(t);
  const float h = ImGui::GetTextLineHeight(), side = std::floor(h * 0.62f), gap = std::max(1.0f, std::floor(side * 0.3f));
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float y = at.y + std::floor((h - side) / 2);
  for (int i = 0; i < 5; i++) {
    const ImVec2 a(at.x + i * (side + gap), y), b(a.x + side, y + side);
    if (i < score) dl->AddRectFilled(a, b, ImGui::GetColorU32(col));
    else dl->AddRect(a, b, ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.55f)));
  }
  ImGui::Dummy(ImVec2(5 * (side + gap) + gap, h));
  ImGui::SameLine(0, 0);
  ImGui::TextColored(col, "%s%s", t.c_str(), own ? "*" : "");
}

std::string file_size(u64 n) {
  char b[32];
  if (n >= (1u << 20)) std::snprintf(b, sizeof(b), "%.1f MiB", n / 1048576.0);
  else std::snprintf(b, sizeof(b), "%.0f KiB", n / 1024.0);
  return b;
}

}  // namespace

// The game list, filling the window between the menu and the status bar.
void App::draw_game_list() {
  bool resort = false;
  if (games_->version() != game_rows_version_) {
    game_rows_ = games_->entries();
    for (GameEntry& g : game_rows_) apply_own_rating(g);
    game_rows_version_ = games_->version();
    game_sel_ = -1;
    resort = true;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + menu_h_));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - menu_h_ - status_h_));
  ImGui::Begin("##gamelist", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking);
  if (game_rows_.empty()) {
    ImGui::TextDisabled("%s", games_->scanning() ? "Looking for games..."
                                                 : "No Leapster games in the game folders (Tools > Settings > Paths).");
    ImGui::End();
    return;
  }
  const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
  // (By default: by type, cartridges first, then by title.)
  if (ImGui::BeginTable("##games_by_type", 5, flags)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_NoHide, 3.2f);
    ImGui::TableSetupColumn("Region", 0, 0.8f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_DefaultSort, 1.0f);
    ImGui::TableSetupColumn("Compatibility", 0, 1.9f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_PreferSortDescending, 0.9f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs(); sort && (sort->SpecsDirty || resort) && sort->SpecsCount > 0) {
      const int col = sort->Specs[0].ColumnIndex;
      const bool asc = sort->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
      // The sorted column in its direction; ties by title (A-Z), then region.
      auto cmp = [](const auto& x, const auto& y) { return x < y ? -1 : y < x ? 1 : 0; };
      auto primary = [&](const GameEntry& a, const GameEntry& b) {
        switch (col) {
          case 1: return cmp(a.region, b.region);
          case 2: { const int r = cmp(type_rank(a.type), type_rank(b.type)); return r ? r : cmp(a.type, b.type); }
          case 3: return cmp(tier_score(b.compatibility), tier_score(a.compatibility));  // (best first)
          case 4: return cmp(a.size, b.size);
          default: return cmp(a.name, b.name);
        }
      };
      std::stable_sort(game_rows_.begin(), game_rows_.end(), [&](const GameEntry& a, const GameEntry& b) {
        const int p = primary(a, b);
        if (p) return asc ? p < 0 : p > 0;
        return std::tie(a.name, a.region) < std::tie(b.name, b.region);
      });
      sort->SpecsDirty = false;
      game_sel_ = -1;
    }
    int launch = -1;
    for (int i = 0; i < int(game_rows_.size()); i++) {
      const GameEntry& g = game_rows_[size_t(i)];
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      const std::string label = g.name + "##game" + std::to_string(i);
      if (ImGui::Selectable(label.c_str(), game_sel_ == i,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
        game_sel_ = i;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) launch = i;
      }
      if (ImGui::BeginPopupContextItem()) {
        game_sel_ = i;
        if (ImGui::MenuItem("Play")) launch = i;
        if (ImGui::MenuItem("Play as Unsigned")) { open_cart(g.path, true); }
        ImGui::SetItemTooltip("Skips the BaseROM's signature and checksum checks (homebrew, prototypes, damaged dumps).");
        ImGui::Separator();
        if (ImGui::BeginMenu("Compatibility")) {
          for (int t = 4; t >= 0; t--)
            if (ImGui::MenuItem(kTiers[t], nullptr, g.own_compatibility && g.compatibility == kTiers[t])) set_own_rating(size_t(i), kTiers[t]);
          ImGui::Separator();
          if (ImGui::MenuItem(("Use leapemu's (" + g.database_compatibility + ")").c_str(), nullptr, !g.own_compatibility, g.own_compatibility))
            set_own_rating(size_t(i), "");
          ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Game Properties...")) open_rom_window(g, RomWindow::Tab::Info);
        if (ImGui::MenuItem("Contents...")) open_rom_window(g, RomWindow::Tab::Contents);
        ImGui::SetItemTooltip("Its Flash movies, sounds, speech and other assets, which can be exported.");
        if (ImGui::MenuItem("Hex & Patches...")) open_rom_window(g, RomWindow::Tab::Hex);
        ImGui::SetItemTooltip("Edit its bytes as patches: kept apart from the ROM, switchable on and off.");
        if (ImGui::MenuItem("Open Containing Folder"))
          SDL_OpenURL(("file://" + fs::path(g.path).parent_path().string()).c_str());
        ImGui::EndPopup();
      }
      // (The row takes the hover: the tooltip depends on the column.)
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        if (ImGui::TableGetHoveredColumn() == 3) {
          std::string tip = kTierMeaning[tier_score(g.compatibility)];
          if (g.own_compatibility) tip += "\nYour rating (leapemu's: " + g.database_compatibility + ")";
          if (!g.notes.empty()) tip += "\n\n" + g.notes;
          ImGui::SetTooltip("%s", tip.c_str());
        } else {
          ImGui::SetTooltip("%s", g.path.c_str());
        }
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(g.region.c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(g.type.c_str());
      ImGui::TableSetColumnIndex(3);
      draw_tier(g.compatibility, g.own_compatibility);
      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(file_size(g.size).c_str());
    }
    if (game_sel_ >= 0 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
      launch = game_sel_;
    ImGui::EndTable();
    if (launch >= 0) {
      const GameEntry g = game_rows_[size_t(launch)];
      open_cart(g.path, g.needs_unsigned);
    }
  }
  ImGui::End();
}

// The ROM windows' link to the rest of the app.
void App::init_rom_tools() {
  const fs::path dir = pref_dir_ / "patches";
  rom_host_.patch_dir = dir;
  rom_host_.play = [this](const std::string& path, bool allow_unsigned) { open_cart(path, allow_unsigned); };
  rom_host_.running = [this](u32 crc) { return m_.has_cart() && m_.cart_crc() == crc; };
  rom_host_.restart = [this] { if (!cart_rom_.empty()) open_cart(std::string(cart_rom_), cart_load_unsigned_); };
  rom_host_.pick_folder = [this](std::function<void(const std::string&)> f) {
    folder_target_ = std::move(f);
    open_folder_dialog(PendingFile::ExportFolder);
  };
  rom_host_.pick_save = [this](const std::string& name, std::function<void(const std::string&)> f) {
    folder_target_ = std::move(f);
    static std::string ext, label, start;  // (the dialog reads these until it closes)
    static SDL_DialogFileFilter filter;
    ext = fs::path(name).extension().string();
    if (!ext.empty()) ext.erase(0, 1);
    label = ext + " files";
    filter = {label.c_str(), ext.c_str()};
    const std::string dir = settings_.get("export_dir", "");
    start = (dir.empty() ? fs::path(name) : fs::path(dir) / name).string();
    struct Ctx { PendingFile* p; std::string ext; };
    static Ctx ctx;
    ctx = {&pending_, ext};
    SDL_ShowSaveFileDialog(
        [](void* user, const char* const* files, int) {
          auto* c = static_cast<Ctx*>(user);
          if (!files || !files[0]) return;
          std::lock_guard<std::mutex> lock(c->p->mu);
          c->p->path = files[0];
          if (fs::path(c->p->path).extension().empty() && !c->ext.empty()) c->p->path += "." + c->ext;
          c->p->kind = PendingFile::ExportFile;
        },
        &ctx, window_, ext.empty() ? nullptr : &filter, ext.empty() ? 0 : 1, start.c_str());
  };
  rom_host_.play_audio = [this](const std::vector<s16>& pcm, unsigned rate) {
    if (preview_audio_ && preview_rate_ != rate) { SDL_DestroyAudioStream(preview_audio_); preview_audio_ = nullptr; }
    if (!preview_audio_) {
      const SDL_AudioSpec spec{SDL_AUDIO_S16, 1, int(rate)};
      preview_audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
      if (!preview_audio_) { LOG_W("no audio: %s", SDL_GetError()); return; }
      preview_rate_ = rate;
      SDL_ResumeAudioStreamDevice(preview_audio_);
    }
    SDL_ClearAudioStream(preview_audio_);
    SDL_PutAudioStreamData(preview_audio_, pcm.data(), int(pcm.size() * sizeof(s16)));
  };
  rom_host_.stop_audio = [this] { if (preview_audio_) SDL_ClearAudioStream(preview_audio_); };
  rom_host_.audio_left = [this] {
    return preview_audio_ ? SDL_GetAudioStreamQueued(preview_audio_) / (2.0 * preview_rate_) : 0.0;
  };
  rom_host_.make_texture = [this](const std::vector<u32>& argb, int w, int h) -> u64 {
    SDL_Texture* t = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return 0;
    SDL_UpdateTexture(t, nullptr, argb.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    return u64(reinterpret_cast<uintptr_t>(t));
  };
  rom_host_.free_texture = [](u64 t) { SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(uintptr_t(t))); };
  rom_host_.bios = [this]() -> const std::vector<u8>* { return m_.has_bios() ? &m_.bios_original() : nullptr; };
  rom_host_.set_rating = [this](GameEntry& g, const std::string& tier) {
    set_rating(g.crc, g.database_compatibility, tier);
    g.compatibility = g.database_compatibility;
    apply_own_rating(g);
  };
  rom_host_.save_path = [this](const std::string& rom) { return cart_save_path(rom).string(); };
  rom_host_.open_folder = [](const std::string& dir) { SDL_OpenURL(("file://" + dir).c_str()); };
  // A cartridge's enabled patches, applied as it loads.
  m_.cart_patches = [dir](u32 crc) {
    const fs::path f = patch_file_path(dir, crc);
    std::error_code ec;
    PatchFile pf;
    std::string err;
    if (!fs::exists(f, ec) || !pf.load(f.string(), &err)) return std::vector<RomEdit>();
    return pf.enabled_edits();
  };
}

void App::open_rom_window(const GameEntry& g, RomWindow::Tab tab) {
  for (auto& w : rom_windows_)
    if (w->path() == g.path) { w->show(tab); return; }
  rom_windows_.push_back(std::make_unique<RomWindow>(g, &rom_host_, tab));
}

void App::draw_rom_windows() {
  for (size_t i = 0; i < rom_windows_.size();) {
    const bool open = rom_windows_[i]->draw();
    if (rom_windows_[i]->focused()) ui_keys_next_ = true;
    if (open) i++;
    else rom_windows_.erase(rom_windows_.begin() + std::ptrdiff_t(i));
  }
}

// Tools > Settings: one window, a page list on the left. Changes apply at
// once. Controls and Shortcuts: every key, changeable; two key slots and a
// gamepad button each: click one and press a key (Escape cancels),
// right-click to clear it.
void App::draw_settings() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, vp->WorkSize.x - 16), std::min(580.0f, vp->WorkSize.y - 16)), ImGuiCond_Appearing);
  ImGui::SetNextWindowBgAlpha(1.0f);
  if (!ImGui::Begin("Settings", &show_settings_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    capture_control_ = -1;
    return;
  }
  claim_keys();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float line = ImGui::GetTextLineHeightWithSpacing();
  const float bottom = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
  // Pages.
  static const std::pair<SettingsPage, const char*> kPages[] = {{SettingsPage::Paths, "Paths"},
                                                                {SettingsPage::Emulation, "Emulation"},
                                                                {SettingsPage::AudioVideo, "Audio & Video"},
                                                                {SettingsPage::Controls, "Controls"},
                                                                {SettingsPage::Shortcuts, "Shortcuts"}};
  ImGui::BeginChild("##pages", ImVec2(ImGui::CalcTextSize("Audio & Video").x + style.WindowPadding.x * 2 + style.FramePadding.x * 2, -bottom),
                    ImGuiChildFlags_Borders);
  for (const auto& [page, name] : kPages)
    if (ImGui::Selectable(name, settings_page_ == page)) { settings_page_ = page; capture_control_ = -1; }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##page", ImVec2(0, -bottom), ImGuiChildFlags_Borders);
  auto heading = [](const char* text) { ImGui::SeparatorText(text); };
  auto note = [](const char* text) {  // (dimmed, wrapped to the page)
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
  };
  auto save = [&](const char* k, bool v) { settings_.kv[k] = v ? "1" : "0"; };
  auto path_row = [&](const char* id, const std::string& path, const char* none) {  // a path, shortened to fit
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    if (path.empty()) ImGui::TextDisabled("%s", none);
    else {
      ImGui::TextUnformatted(path.c_str());
      ImGui::SetItemTooltip("%s", path.c_str());
    }
    ImGui::PopID();
  };

  // The key tables (Controls, Shortcuts): the action's name, then columns
  // that share the rest of the width, each a button filling its cell.
  auto pad_name = [](SDL_GamepadButton b) -> std::string {
    switch (b) {
      case SDL_GAMEPAD_BUTTON_INVALID: return "";
      case SDL_GAMEPAD_BUTTON_SOUTH: return "South (A)";
      case SDL_GAMEPAD_BUTTON_EAST: return "East (B)";
      case SDL_GAMEPAD_BUTTON_WEST: return "West (X)";
      case SDL_GAMEPAD_BUTTON_NORTH: return "North (Y)";
      case SDL_GAMEPAD_BUTTON_BACK: return "Back";
      case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
      case SDL_GAMEPAD_BUTTON_START: return "Start";
      case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left stick";
      case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right stick";
      case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "Left bumper";
      case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "Right bumper";
      case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
      case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
      case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
      default: {
        const char* n = SDL_GetGamepadStringForButton(b);
        return n ? n : "?";
      }
    }
  };
  auto table = [&](const char* id, const char* first, bool buttons, float height) {
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchSame;
    if (!ImGui::BeginTable(id, 4, flags, ImVec2(0, height))) return;
    float name_w = 0;
    for (size_t i = 0; i < kControlCount; i++)
      if ((kControls[i].act == Act::Button || kControls[i].act == Act::Touch) == buttons)
        name_w = std::max(name_w, ImGui::CalcTextSize(kControls[i].label).x);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(first, ImGuiTableColumnFlags_WidthFixed, name_w);
    ImGui::TableSetupColumn("Key");
    ImGui::TableSetupColumn("Other key");
    ImGui::TableSetupColumn("Gamepad");
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < kControlCount; i++) {
      if ((kControls[i].act == Act::Button || kControls[i].act == Act::Touch) != buttons) continue;
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(kControls[i].label);
      for (int slot = 0; slot < 3; slot++) {
        ImGui::TableSetColumnIndex(1 + slot);
        const bool waiting = capture_control_ == int(i) && capture_slot_ == slot;
        const std::string text = waiting ? std::string(slot == 2 ? "press a button..." : "press a key...")
                                 : slot == 2 ? pad_name(pad_btn_[i]) : key_label(keys_[i][size_t(slot)]);
        const std::string label = text + "##" + std::to_string(i) + "_" + std::to_string(slot);
        if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0))) { capture_control_ = int(i); capture_slot_ = slot; }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
          if (slot == 2) assign_pad(i, SDL_GAMEPAD_BUTTON_INVALID);
          else assign_key(i, slot, kNone);
          capture_control_ = -1;
        }
        if (slot == 2) ImGui::SetItemTooltip("Click, then press a gamepad button (Escape cancels). Right-click to clear.");
        else if (buttons) ImGui::SetItemTooltip("Click, then press a key (Escape cancels). Right-click to clear.");
        else ImGui::SetItemTooltip("Click, then press a key, with Ctrl, Shift or Alt if wanted\n(Escape cancels). Right-click to clear.");
      }
    }
    ImGui::EndTable();
  };
  auto reset_keys = [&](bool buttons) {
    if (!ImGui::Button("Reset to Defaults")) return;
    auto mine = [&](size_t i) { return (kControls[i].act == Act::Button || kControls[i].act == Act::Touch) == buttons; };
    for (size_t i = 0; i < kControlCount; i++) {
      if (!mine(i)) continue;
      keys_[i] = {kControls[i].defaults[0], kControls[i].defaults[1]};
      pad_btn_[i] = kControls[i].pad;
    }
    // A default the other table took over in the meantime is freed there
    // (a key or button belongs to one control).
    for (size_t i = 0; i < kControlCount; i++) {
      if (mine(i)) continue;
      for (size_t j = 0; j < kControlCount; j++) {
        if (!mine(j)) continue;
        for (auto& k : keys_[i])
          if (k.key != kNoKey && (k == keys_[j][0] || k == keys_[j][1])) k = kNone;
        if (pad_btn_[i] != SDL_GAMEPAD_BUTTON_INVALID && pad_btn_[i] == pad_btn_[j]) pad_btn_[i] = SDL_GAMEPAD_BUTTON_INVALID;
      }
    }
    save_keys();
    capture_control_ = -1;
  };

  switch (settings_page_) {
    case SettingsPage::Paths: {
      heading("BaseROM (BIOS)");
      const std::string current = settings_.get("bios");
      path_row("bios", current, "None chosen.");
      if (ImGui::Button("Browse...##bios")) open_dialog(PendingFile::Bios);
      const std::vector<std::string> bioses = recent("recent_bios");
      if (bioses.size() > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(line * 12);
        if (ImGui::BeginCombo("##recent_bios", "Recent", ImGuiComboFlags_HeightLarge)) {
          for (size_t i = 0; i < bioses.size(); i++) {
            const std::string label = fs::path(bioses[i]).filename().string() + "##" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), bioses[i] == current)) open_bios(bioses[i]);
            ImGui::SetItemTooltip("%s", bioses[i].c_str());
          }
          ImGui::EndCombo();
        }
      }
      heading("Game saves");
      if (ImGui::Checkbox("Next to the ROM", &saves_beside_rom_)) {
        save("saves_beside_rom", saves_beside_rom_);
        move_save();
      }
      ImGui::SetItemTooltip("Pet Pals (USA).zip -> Pet Pals (USA).sav, as in other emulators.");
      note(saves_beside_rom_ ? "Otherwise (a ROM's folder that cannot be written), in:" : "In:");
      path_row("saves", saves_dir().string(), "");
      if (ImGui::Button("Browse...##saves")) open_folder_dialog(PendingFile::SavesFolder);
      ImGui::SameLine();
      ImGui::BeginDisabled(settings_.get("saves_dir").empty());
      if (ImGui::Button("Default##saves")) {
        settings_.kv.erase("saves_dir");
        settings_.save();
        move_save();
      }
      ImGui::EndDisabled();
      ImGui::SetItemTooltip("The data folder's \"saves\" folder.");
      if (!cart_save_path_.empty()) ImGui::TextDisabled("The running game's: %s", cart_save_path_.string().c_str());
      heading("Game list");
      const std::vector<std::string> dirs = game_dirs();
      if (dirs.empty()) ImGui::TextDisabled("No game folders yet.");
      for (size_t i = 0; i < dirs.size(); i++) {
        ImGui::PushID(int(i));
        if (ImGui::SmallButton("Open")) SDL_OpenURL(("file://" + dirs[i]).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
          std::vector<std::string> rest = dirs;
          rest.erase(rest.begin() + std::ptrdiff_t(i));
          set_list("game_dir", rest);
          refresh_games();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(dirs[i].c_str());
        ImGui::PopID();
      }
      if (ImGui::Button("Add Folder...")) open_folder_dialog();
      ImGui::SameLine();
      ImGui::BeginDisabled(dirs.empty());
      if (ImGui::Button("Refresh")) refresh_games();
      ImGui::EndDisabled();
      if (ImGui::Checkbox("Search subfolders", &game_dirs_recursive_)) {
        save("game_dirs_recursive", game_dirs_recursive_);
        refresh_games();
      }
      ImGui::SameLine(0, line * 2);
      if (ImGui::Checkbox("Show when no game is running", &show_game_list_)) save("show_game_list", show_game_list_);
      heading("Data folder");
      path_row("data", pref_dir_.string(), "");
      ImGui::SetItemTooltip("Settings, save states, screenshots and the console's own memory.");
      if (ImGui::Button("Open Data Folder")) SDL_OpenURL(("file://" + pref_dir_.string()).c_str());
      break;
    }
    case SettingsPage::Emulation: {
      heading("CPU core");
      using Backend = arc::Cpu::Backend;
      for (Backend b : {Backend::Jit, Backend::CachedInterpreter, Backend::Interpreter}) {
        const bool avail = arc::Cpu::backend_available(b);
        std::string label = arc::Cpu::backend_name(b);
        if (!avail) label += " (not on this computer)";
        ImGui::BeginDisabled(!avail);
        if (ImGui::RadioButton(label.c_str(), m_.cpu().backend() == b)) m_.cpu().set_backend(b);
        ImGui::EndDisabled();
      }
      note("All give the same results; the JIT is the fastest.");
      // (A movie runs with its own settings: these are locked while one is active.)
      const bool movie = movie_mode_ != MovieMode::None;
      ImGui::BeginDisabled(movie);
      if (ImGui::Checkbox("Idle loop skip", &m_.cpu().idle_skip)) save("idle_skip", m_.cpu().idle_skip);
      ImGui::SetItemTooltip("Jumps ahead while the console's OS is provably idle (no effect on results).");
      bool hw_timing = hw_timing_;
      if (ImGui::Checkbox("Accurate timing", &hw_timing)) {
        hw_timing_ = hw_timing;
        m_.timing = hw_timing_ ? Machine::Timing{} : Machine::Timing::none();
        m_.apply_timing();
        save("hw_timing", hw_timing_);
      }
      ImGui::SetItemTooltip("The console's memory wait states. Off: one instruction per cycle (games run faster\n"
                            "than on the real console).");
      ImGui::EndDisabled();
      if (movie) note("A movie is active: it uses the settings it was recorded with.");
      heading("Fast forward and rewind");
      static const std::pair<double, const char*> kFf[] = {{0, "Unthrottled"}, {2, "2x"}, {3, "3x"}, {4, "4x"}, {8, "8x"}};
      ImGui::SetNextItemWidth(line * 8);
      const char* cur = "Unthrottled";
      for (const auto& [sp, name] : kFf) if (ff_speed_ == sp) cur = name;
      if (ImGui::BeginCombo("Fast forward speed", cur)) {
        for (const auto& [sp, name] : kFf)
          if (ImGui::Selectable(name, ff_speed_ == sp)) { ff_speed_ = sp; settings_.kv["ff_speed"] = std::to_string(sp); }
        ImGui::EndCombo();
      }
      if (ImGui::Checkbox("Rewind", &rewind_on_)) {
        save("rewind", rewind_on_);
        if (!rewind_on_) rewind_.clear();
      }
      ImGui::SetItemTooltip("Keeps snapshots every 0.1 s, about 11 KB each, up to 256 MB (roughly half an hour).\n"
                            "Needed for rewinding and Frame Back.");
      heading("When the window is in the background");
      if (ImGui::Checkbox("Pause", &inactive_pause_)) save("inactive_pause", inactive_pause_);
      ImGui::SameLine(0, line * 2);
      if (ImGui::Checkbox("Mute", &inactive_mute_)) { save("inactive_mute", inactive_mute_); apply_volume(); }
      break;
    }
    case SettingsPage::AudioVideo: {
      heading("Audio");
      int v = int(std::lround(volume_ * 100));
      ImGui::SetNextItemWidth(line * 12);
      if (ImGui::SliderInt("Volume", &v, 0, 100, "%d%%")) {
        volume_ = float(v) / 100.0f;
        settings_.kv["volume"] = std::to_string(volume_);
        set_muted(false);  // (as choosing a volume in the View menu does)
      }
      ImGui::SameLine(0, line);
      bool m = muted_;
      if (ImGui::Checkbox("Mute", &m)) set_muted(m);
      note("Fast forward is silent above 4x. The console's own volume buttons are in Emulation > Console.");
      heading("Video");
      static const char* kFilterNames[] = {"Nearest (pixel perfect)", "Sharp Bilinear", "Bilinear", "Bicubic", "Lanczos",
                                           "MMPX (pixel art)", "LCD"};
      static const Filter kFilterOrder[] = {Filter::Nearest, Filter::Sharp, Filter::Bilinear, Filter::Bicubic,
                                            Filter::Lanczos, Filter::Mmpx, Filter::Lcd};
      const char* fname = kFilterNames[0];
      for (size_t i = 0; i < std::size(kFilterOrder); i++) if (filter_ == kFilterOrder[i]) fname = kFilterNames[i];
      ImGui::SetNextItemWidth(line * 12);
      if (ImGui::BeginCombo("Filter", fname)) {
        for (size_t i = 0; i < std::size(kFilterOrder); i++)
          if (ImGui::Selectable(kFilterNames[i], filter_ == kFilterOrder[i])) filter_ = kFilterOrder[i];
        ImGui::EndCombo();
      }
      ImGui::Checkbox("Integer scaling", &integer_scale_);
      if (ImGui::Checkbox("Vector scale to window", &vector_scale_)) {
        save("vector_scale", vector_scale_);
        update_display_mode();
      }
      if (ImGui::Checkbox("Interpolation (hack, buggy)", &smoothing_)) {
        save("interp", smoothing_);
        update_display_mode();
      }
      note("(Also in the View menu, with descriptions.) Vector scaling and interpolation pause while fast "
           "forwarding or above 1x speed.");
      if (ImGui::Checkbox("VSync", &vsync_)) {
        SDL_SetRenderVSync(renderer_, vsync_ ? 1 : 0);
        save("vsync", vsync_);
      }
      ImGui::SetItemTooltip("Waits for the display's refresh: no tearing. Off: frames are shown as soon as\n"
                            "they are ready (tearing possible).");
      // SDL's render driver: it shows the picture (scaling, Nearest and
      // Bilinear) on the GPU. The other filters, vector scaling and
      // interpolation are computed on the CPU, whatever the driver.
      const std::string want = settings_.get("render_driver");
      ImGui::SetNextItemWidth(line * 12);
      if (ImGui::BeginCombo("Render driver", want.empty() ? "Automatic" : want.c_str())) {
        if (ImGui::Selectable("Automatic", want.empty())) { settings_.kv.erase("render_driver"); settings_.save(); }
        for (int i = 0; i < SDL_GetNumRenderDrivers(); i++) {
          const char* name = SDL_GetRenderDriver(i);
          if (name && ImGui::Selectable(name, want == name)) { settings_.kv["render_driver"] = name; settings_.save(); }
        }
        ImGui::EndCombo();
      }
      const char* in_use = SDL_GetRendererName(renderer_);
      ImGui::TextDisabled("In use: %s%s", in_use ? in_use : "?",
                          !want.empty() && in_use && want != in_use ? "  (the choice applies when leapemu restarts)" : "");
      note("The GPU shows the picture (scaling, Nearest, Bilinear); the other filters, vector scaling and "
           "interpolation are drawn on the CPU.");
      break;
    }
    case SettingsPage::Controls:
      table("##console", "Button", true, ImGui::GetContentRegionAvail().y - line * 6);
      note("Stylus: left mouse button on the screen. On a gamepad, the right stick moves a cursor and the right "
           "trigger (or the Stylus button) presses. The left stick also works as the d-pad.");
      ImGui::TextDisabled(pad_ ? "Gamepad: %s" : "%s", pad_ ? SDL_GetGamepadName(pad_) : "No gamepad connected.");
      reset_keys(true);
      break;
    case SettingsPage::Shortcuts:
      table("##emulator", "Action", false, ImGui::GetContentRegionAvail().y - line * 3);
      note("Fixed: Ctrl+F also toggles fullscreen, Escape leaves it.");
      reset_keys(false);
      break;
  }
  ImGui::EndChild();
  if (ImGui::Button("Close")) { show_settings_ = false; capture_control_ = -1; }
  ImGui::End();
  if (!show_settings_) { capture_control_ = -1; settings_.save(); }
}

// Help > About: what leapemu is, and the work it builds on.
void App::draw_about() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::min(560.0f, vp->WorkSize.x - 20), vp->WorkSize.y - 20));
  ImGui::SetNextWindowBgAlpha(1.0f);
  if (!ImGui::Begin("About leapemu", &show_about_,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }
  if (ImGui::IsWindowAppearing()) ImGui::SetWindowFocus();  // (on opening only)
  claim_keys();
  const float wrap = std::min(520.0f, vp->WorkSize.x - 60);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
  ImGui::SetWindowFontScale(1.8f);
  ImGui::TextUnformatted("leapemu");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Text("Version %s", LEAPEMU_VERSION);
  ImGui::Spacing();
  ImGui::TextWrapped("An emulator for the LeapFrog Leapster family.");
  ImGui::Separator();
  ImGui::TextWrapped("Built on the research and tools of others (hover for details):");
  struct Credit { const char* who; const char* what; const char* detail; const char* url; };
  static const Credit kCredits[] = {
      {"Alice Shelton (toadster172)", "Leapster hardware research (MAME)",
       "Timers, LCD and DMA, touch screen, EEPROM, UART and sound, in MAME's leapster branch.\n"
       "Our hardware model follows it, and a build of it is our test oracle.",
       "https://github.com/toadster172/mame/tree/leapster"},
      {"David Haywood and MAME", "Original driver, ARCompact core",
       "The original MAME Leapster driver, ARCompact CPU core and software list:\n"
       "BaseROM identification, and a reference for our CPU core.",
       "https://github.com/mamedev/mame"},
      {"BLiNXthetimesweeperGOD", "LeapFrog-Tools, format notes",
       "ROM headers, resource tables and the Leapster system overview.",
       "https://github.com/BLiNXthetimesweeperGOD/LeapFrog-Tools"},
      {"lfhacks", "Leapster-Tools (LeapSplit)", "The successor to LeapFrog-Tools: the ROM asset tables.",
       "https://github.com/lfhacks/Leapster-Tools"},
      {"Nathan Farlow", "LFC speech decoder", "leapfrog-voice-decoder: the speech codec behind our speech playback.",
       "https://github.com/nathanfarlow/leapfrog-voice-decoder"},
      {"ResHax, LeapFrog Wiki", "Early speech-format notes", "Community notes on LeapFrog's LPC/LFC speech format.",
       "https://reshax.com/topic/985-leapfrog-lpclfc-format/"},
      {"ARC / Synopsys", "ISA reference, GNU toolchain", "The ARCompact ISA Programmer's Reference and the ARC GNU toolchain.",
       "https://github.com/foss-for-synopsys-dwc-arc-processors/toolchain"},
      {"SDL", "Window, input, audio", "Sam Lantinga and contributors.", "https://www.libsdl.org"},
      {"Dear ImGui", "User interface", "Omar Cornut and contributors.", "https://github.com/ocornut/imgui"},
  };
  if (ImGui::BeginTable("credits", 2, ImGuiTableFlags_SizingFixedFit)) {
    for (const Credit& c : kCredits) {
      ImGui::PushID(c.who);
      ImGui::TableNextColumn();
      if (ImGui::TextLink(c.who)) SDL_OpenURL(c.url);
      ImGui::SetItemTooltip("%s\n%s", c.detail, c.url);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(c.what);
      ImGui::SetItemTooltip("%s", c.detail);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::Separator();
  ImGui::TextDisabled("LeapFrog and Leapster are trademarks of LeapFrog Enterprises, Inc. Not affiliated with "
                      "LeapFrog; contains no LeapFrog software.");
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  if (ImGui::Button("Close")) show_about_ = false;
  ImGui::End();
}

void App::draw_cpu() {
  ImGui::Begin("CPU", &show_cpu_);
  claim_keys();
  const auto& c = m_.cpu();
  ImGui::Text("PC  %08x   cycles %llu", c.pc(), static_cast<unsigned long long>(c.cycles()));
  const u32 st = c.status32();
  ImGui::Text("STATUS32 %08x  [%c%c%c%c] E1=%d E2=%d%s%s", st, (st & arc::Cpu::kZ) ? 'Z' : '-',
              (st & arc::Cpu::kN) ? 'N' : '-', (st & arc::Cpu::kC) ? 'C' : '-',
              (st & arc::Cpu::kV) ? 'V' : '-', bool(st & arc::Cpu::kE1), bool(st & arc::Cpu::kE2),
              c.sleeping() ? "  SLEEP" : "", c.in_delay_slot() ? "  DELAY" : "");
  ImGui::Text("LP  %08x-%08x count %08x   IRQ pending %08x", c.lp_start(), c.lp_end(),
              c.reg(arc::Cpu::kLP_COUNT), c.pending_irqs());
  ImGui::Separator();
  if (ImGui::BeginTable("regs", 4, ImGuiTableFlags_SizingFixedFit)) {
    for (unsigned r = 0; r < 32; r++) {
      ImGui::TableNextColumn();
      ImGui::Text("%-6s %08x", arc::reg_name(r), c.reg(r));
    }
    ImGui::EndTable();
  }
  ImGui::Separator();
  if (ImGui::Button(running_ ? "Pause" : "Run")) {
    m_.cpu().clear_stop();
    running_ = !running_;
  }
  ImGui::SameLine();
  if (ImGui::Button("Step") && !running_) {
    m_.cpu().clear_stop();
    m_.run_cycles(1);
    disasm_addr_ = m_.cpu().pc();
  }
  ImGui::SameLine();
  if (ImGui::Button("Step 1000") && !running_) {
    m_.cpu().clear_stop();
    m_.run_cycles(1000);
    disasm_addr_ = m_.cpu().pc();
  }
  ImGui::End();
}

void App::draw_disasm() {
  ImGui::SetNextWindowSize(ImVec2(460, 520), ImGuiCond_FirstUseEver);
  ImGui::Begin("Disassembly", &show_disasm_);
  claim_keys();
  ImGui::Checkbox("Follow PC", &follow_pc_);
  auto& cpu = m_.cpu();
  if (follow_pc_ || disasm_addr_ == 0) disasm_addr_ = cpu.pc();
  ImGui::SameLine();
  ImGui::TextDisabled("(click a line to toggle a breakpoint)");
  ImGui::Separator();
  auto fetch = [this](u32 a) { return m_.bus().peek16(a); };
  u32 pc = disasm_addr_;
  auto& bps = cpu.breakpoints();
  for (int i = 0; i < 48; i++) {
    unsigned len = 2;
    const std::string text = arc::disassemble(pc, fetch, &len);
    const bool is_pc = pc == cpu.pc();
    const bool bp = bps.count(pc);
    char line[160];
    std::snprintf(line, sizeof(line), "%s %08x  %s", bp ? "*" : " ", pc, text.c_str());
    if (is_pc) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 90, 255));
    ImGui::PushID(int(pc));
    if (ImGui::Selectable(line, is_pc)) {
      if (bp) bps.erase(pc);
      else bps.insert(pc);
    }
    ImGui::PopID();
    if (is_pc) ImGui::PopStyleColor();
    pc += len;
  }
  ImGui::End();
}

void App::draw_memory() {
  ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
  ImGui::Begin("Memory", &show_memory_);
  claim_keys();
  ImGui::SetNextItemWidth(120);
  if (ImGui::InputText("Address", mem_input_, sizeof(mem_input_),
                       ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue))
    mem_addr_ = u32(std::strtoul(mem_input_, nullptr, 16)) & ~15u;
  ImGui::SameLine();
  ImGui::TextDisabled("BIOS 40000000  I/O 01800000  VRAM 03000000  RAM 3c000000  Cart 80000000");
  ImGui::Separator();
  ImGui::BeginChild("hex");
  for (int row = 0; row < 32; row++) {
    const u32 a = mem_addr_ + row * 16;
    char hex[64], asc[17];
    int p = 0;
    for (int i = 0; i < 16; i++) {
      const u8 b = m_.bus().peek8(a + i);
      p += std::snprintf(hex + p, sizeof(hex) - p, "%02x%s", b, i == 7 ? "  " : " ");
      asc[i] = (b >= 32 && b < 127) ? char(b) : '.';
    }
    asc[16] = 0;
    ImGui::Text("%08x  %s %s", a, hex, asc);
  }
  ImGui::EndChild();
  ImGui::End();
}

void App::draw_uart() {
  ImGui::SetNextWindowSize(ImVec2(520, 300), ImGuiCond_FirstUseEver);
  ImGui::Begin("UART Console", &show_uart_);
  claim_keys();
  if (ImGui::Button("Clear")) m_.uart_output().clear();
  ImGui::Separator();
  ImGui::BeginChild("uart");
  ImGui::TextUnformatted(m_.uart_output().c_str());
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  ImGui::End();
}

void App::draw_status() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = ImGui::GetFrameHeight();
  status_h_ = h;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 2));
  ImGui::Begin("##status", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::PopStyleVar();
  if (!status_msg_.empty() && SDL_GetTicks() - status_time_ < 2000) {
    ImGui::TextUnformatted(status_msg_.c_str());
  } else if (error_shown()) {
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
  } else if (!powered_) {
    const char* off = console_off_ ? "Powered off  |  " : "";
    if (m_.has_bios() && show_game_list_ && !game_dirs().empty())
      ImGui::Text("%s%s", off, games_->scanning() ? "Looking for games..." : (std::to_string(game_rows_.size()) + (game_rows_.size() == 1 ? " game" : " games")).c_str());
    else if (console_off_)
      ImGui::Text("Powered off  |  Emulation > Power On starts %s again", off_cart_.empty() ? "the console" : fs::path(off_cart_).stem().string().c_str());
    else
      ImGui::TextUnformatted(m_.has_bios() ? ("Ready: File > Load ROM" + (shortcut(Act::LoadRom).empty() ? std::string() : " (" + shortcut(Act::LoadRom) + ")") +
                                              ", or drag a ROM here").c_str()
                                           : "No BaseROM (BIOS) yet: choose one to start");
  } else {
    const std::string cart = m_.has_cart() ? (cart_title_.empty() ? "unknown cartridge" : cart_title_)
                                           : "no cartridge";
    std::string tas;
    if (movie_mode_ != MovieMode::None || show_counters_) {
      char buf[160];
      const char* mode = movie_mode_ == MovieMode::Recording ? "Recording " : movie_mode_ == MovieMode::Playing ? "Playing "
                         : movie_mode_ == MovieMode::Finished                ? "Finished " : "";
      if (movie_mode_ == MovieMode::Playing || movie_mode_ == MovieMode::Finished)
        std::snprintf(buf, sizeof(buf), "  |  %sframe %llu/%zu  lag %llu", mode, static_cast<unsigned long long>(m_.frame_count()),
                      movie_.frames.size(), static_cast<unsigned long long>(lag_count_));
      else
        std::snprintf(buf, sizeof(buf), "  |  %sframe %llu  lag %llu", mode, static_cast<unsigned long long>(m_.frame_count()),
                      static_cast<unsigned long long>(lag_count_));
      tas = buf;
      if (show_counters_) tas += "  " + Movie::encode(last_input_);
    }
    // Left: what the console is doing (with the speed, if not 1x), the
    // title and the TAS counters. Right: the frame rate.
    std::string state = m_.powered_off() ? "Powered off" : switched_off_ ? "Powering off..."
                        : rewinding_ ? "Rewinding" : !running_ ? "Paused"
                        : m_.calibrating() ? "Calibrating touch screen..." : turbo_ ? "Fast forward" : "Running";
    if (state == "Running" && speed_ != 1.0) {
      char sp[32];
      std::snprintf(sp, sizeof(sp), speed_ == 0 ? "Running unthrottled" : "Running at %gx", speed_);
      state = sp;
    }
    if (saves_suspended_ && movie_mode_ == MovieMode::None) tas += "  |  saving paused until reset";
    ImGui::Text("%s  |  %s%s", state.c_str(), cart.c_str(), tas.c_str());
    char fps[48];
    std::snprintf(fps, sizeof(fps), "%.1f fps (%.0f%%)", emu_fps_, emu_fps_ / Machine::kFps * 100.0);
    const float w = ImGui::CalcTextSize(fps).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - w - 8.0f));
    ImGui::TextUnformatted(fps);
  }
  ImGui::End();
}

}  // namespace

int main(int argc, char** argv) {
  App app;
  return app.run(argc, argv);
}
