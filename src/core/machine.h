#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/arc/cpu.h"
#include "core/bus.h"
#include "core/celp.h"
#include "core/drawcap.h"
#include "core/patch.h"
#include "core/rom.h"

namespace leap {

// Physical buttons. Values are the bit positions in the GIO data-in register
// (active low on hardware; the machine handles the inversion).
enum Button : u32 {
  kBtnRight = 1u << 7,
  kBtnDown = 1u << 8,
  kBtnLeft = 1u << 9,
  kBtnB = 1u << 13,
  kBtnA = 1u << 14,
  kBtnVolDown = 1u << 15,
  kBtnVolUp = 1u << 16,
  kBtnContrast = 1u << 17,   // (a toggle: see docs/hardware.md, LCD controller)
  kBtnBrightUp = 1u << 24,
  kBtnBrightDown = 1u << 25,
  kBtnPause = 1u << 26,
  kBtnHint = 1u << 28,
  kBtnHome = 1u << 29,
  kBtnUp = 1u << 31,
};

// Leapster family machine (original Leapster / L-MAX / TV hardware; Leapster 2
// differences are not yet known). The hardware model is based on the reverse
// engineering in toadster172/mame@leapster (Alice Shelton) — see
// docs/hardware.md for the register map and open questions.
class Machine : private MmioDevice {
 public:
  static constexpr u32 kCpuHz = 96'000'000;
  static constexpr u32 kTimerHz = 16'000'000;
  static constexpr u32 kFps = 60;
  // Sound output rate. The voices run at 8 kHz (each sample is output four
  // times); the PCM channels, which some games stream through, play at 32 kHz.
  static constexpr u32 kAudioHz = 32000;
  static constexpr u32 kVoiceHz = 8000;
  static constexpr int kWidth = 160, kHeight = 160;

  Machine();
  ~Machine() override;

  bool load_bios(const std::string& path, std::string* err);
  bool load_cart(const std::string& path, std::string* err);
  // The same from images already read (load_rom_file); `name` stands for the
  // file's name (a download's folder name is derived from it).
  bool load_bios_image(std::vector<u8> img, std::string* err);
  bool load_cart_image(std::vector<u8> img, const std::string& name, std::string* err);
  // A Leapster 2 downloadable game (a cartridge-format image linked to run
  // from RAM; load_cart() comes here for one). It is copied into RAM at its
  // address on every reset, and also shown where a cartridge would be, so the
  // BaseROM finds it. It boots only with allow_unsigned (it is not signed as
  // a cartridge). Games that use the Leapster 2 BaseROM's newer interfaces
  // need stub_missing_services.
  bool load_downloadable(const std::string& path, std::string* err);
  bool downloadable() const { return !ram_image_.empty(); }
  void eject_cart();
  bool has_bios() const { return bios_loaded_; }  // (a BaseROM image was loaded)
  bool has_cart() const { return !cart_.empty(); }
  const RomHeader& bios_header() const { return bios_header_; }
  // Why the BaseROM loaded cannot work as dumped ("" if nothing is known).
  const std::string& bios_problem() const { return bios_problem_; }
  const RomHeader& cart_header() const { return cart_header_; }
  u32 bios_crc() const { return bios_crc_; }
  u32 cart_crc() const { return cart_crc_; }  // of the image as loaded, before patches
  // ROM patches (core/patch.h): asked for by cartridge CRC when a cartridge
  // loads, and applied to its image. A patched image fails the BaseROM's
  // digest check, so it boots only with allow_unsigned.
  std::function<std::vector<RomEdit>(u32 cart_crc)> cart_patches;
  const PatchResult& cart_patch() const { return cart_patch_; }

  void reset();

  // Advance emulation by one video frame (1/60 s). Returns false if the CPU
  // stopped (breakpoint, halt or fault); see cpu().stop_reason().
  // Framebuffer DMA transfers started since the machine was created (a
  // statistic, not saved: native games start one per rendered frame).
  u64 dma_count() const { return dma_count_; }
  bool run_frame();
  // Advance by `cycles` CPU cycles (96 MHz).
  bool run_cycles(u64 cycles);

  // ---- video ----
  // 160x160 pixels, 0xAARRGGBB.
  const u32* framebuffer() const { return fb_.data(); }
  u64 frame_count() const { return frames_; }

  // ---- input ----
  void set_buttons(u32 pressed_mask) { buttons_ = pressed_mask; }
  u32 buttons() const { return buttons_; }
  // The power switch. Switching it off makes the BaseROM save its settings,
  // play its power-off animation and cut the power (powered_off()); the
  // BaseROM also powers off by itself after a long time without input. Only
  // reset() (switching on again) restarts a powered-off machine.
  void set_power_switch(bool on);
  bool power_switch() const { return power_switch_; }
  bool powered_off() const { return powered_off_; }
  // The LCD's brightness level as the BaseROM sets it: 0x30 by default,
  // 36-61 over the setting's range. The framebuffer is the image the game
  // drew; frontends show this setting on top of it.
  int lcd_brightness() const { return lcd_brightness_; }
  // Whether the last frame was a lag frame: the software read neither the
  // buttons nor the touch screen during it (host statistic, not saved).
  bool lagged() const { return lagged_; }
  // Touch position in screen pixels (0..159); `down` = stylus touching.
  void set_touch(bool down, int x, int y);

  // Touch calibration. The BIOS keeps calibration in the system EEPROM and
  // only runs its (factory) calibration when told to by strap bit 14. When the
  // calibration area is blank we request it, and with auto_calibrate the
  // machine taps the crosshairs itself, so users never see a broken stylus.
  bool auto_calibrate = true;
  // Erase the stored calibration; takes effect on the next reset().
  void request_calibration();
  bool calibration_blank() const;
  bool calibrating() const { return lcd_mode() == 3; }

  // ---- audio ----
  // Pop up to `max` mono 16-bit samples at kAudioHz. Returns count written.
  size_t read_audio(s16* out, size_t max);

  // ---- non-volatile storage ----
  // System EEPROM (touch calibration, region, settings) and the cartridge's
  // save EEPROM. Frontends persist these between sessions.
  std::array<u8, 512>& system_eeprom() { return sys_eeprom_; }
  std::array<u8, 2048>& cart_eeprom() { return cart_eeprom_; }
  // The cartridge EEPROM belongs to the cartridge: load_cart() and
  // eject_cart() blank it, and reset() leaves both EEPROMs alone.
  // Raw images: the cartridge save (a ".sav" file, 2 KiB) and the system
  // EEPROM (512 bytes). Loading a missing file blanks the EEPROM and returns
  // false. Saves are written atomically (temporary file, then rename).
  bool load_cart_save(const std::string& path);
  bool save_cart_save(const std::string& path) const;
  bool load_system_nvram(const std::string& path);
  bool save_system_nvram(const std::string& path) const;
  // Load/save "<dir>/system.eep" and "<dir>/<cart crc>.eep". Missing files are
  // not an error. Returns false only on I/O errors when saving.
  void load_nvram(const std::string& dir);
  bool save_nvram(const std::string& dir) const;

  // ---- save states ----
  // Serialized machine state. States are tied to the loaded BIOS and
  // cartridge (checked by CRC on load).
  std::vector<u8> save_state();
  bool load_state(const std::vector<u8>& data, std::string* err);
  bool save_state_file(const std::string& path, std::string* err);
  bool load_state_file(const std::string& path, std::string* err);

  // ---- debug ----
  arc::Cpu& cpu() { return cpu_; }

  // Native draw capture for display smoothing (core/drawcap.h). Off by
  // default; when on and the cartridge uses a supported engine, every game
  // frame is also recorded as a draw list. Never affects emulation.
  void set_native_capture(bool on);
  bool native_capture() const { return native_capture_; }
  const DrawCapture& draw_capture() const { return capture_; }
  // ROM images as mapped (BaseROM at 0x4000'0000, cartridge at kCartBase).
  const std::vector<u8>& bios_image() const { return bios_; }               // as mapped (with any patches)
  const std::vector<u8>& bios_original() const { return bios_pristine_; }  // as loaded
  const std::vector<u8>& cart_image() const { return cart_; }
  // The ROMs as seen by the CPU (BaseROM at 0 and 0x4000'0000, cartridge),
  // for redrawing Flash content from its definitions.
  flash::RomView rom_view() const {
    flash::RomView v;
    v.ranges.push_back({0x0000'0000u, bios_.data(), bios_.size()});
    v.ranges.push_back({0x4000'0000u, bios_.data(), bios_.size()});
    v.ranges.push_back({0x8000'0000u, cart_.data(), cart_.size()});
    // A downloadable's movies are at its RAM address (its definitions do not
    // change there: the pristine image serves).
    if (!ram_image_.empty()) v.ranges.push_back({ram_image_at_, ram_image_.data(), ram_image_.size()});
    return v;
  }
  Bus& bus() { return bus_; }
  u64 cycles() const { return cpu_.cycles(); }
  // Text written to the debug UART (MQX console).
  std::string& uart_output() { return uart_; }
  bool echo_uart = false;  // Also write UART output to stdout.
  // Accept cartridges without a valid LeapFrog "Approved Content" signature
  // (homebrew). When enabled, reset() applies a small in-memory patch to the
  // BaseROM's signature check; the loaded image is otherwise untouched. Off by
  // default. Returns whether the patch point was found in the current BIOS.
  bool allow_unsigned = false;
  bool unsigned_patch_active() const { return unsigned_patched_; }

  // A compatibility fix for Leapster 2 downloads that use Leapster 2 services
  // this BaseROM doesn't have (registry entries 52, the file functions, and
  // 54, the current program; table B of each): such a call finds a null or
  // stray function pointer, and the console restarts or faults. With this on
  // (applied at reset, downloads only), the download's calls to those tables
  // are redirected in its RAM copy to leapemu's own functions: for the game's
  // save file, eeprom.ltm, kept in the cartridge save memory (and so in its
  // .sav), and the file naming the current program. Other slots return 0, as
  // a failed call would. Off by default; the game database turns it on for the
  // titles that need it. See docs/downloadables.md.
  bool stub_missing_services = false;

  // For host tools (asset export), not emulation: runs the guest function at
  // `fn` with `args` in r0-r7, interrupts off, on a stack of its own, until it
  // returns (to a halt instruction on a page of the host's); its r0 in
  // *result. False if it faults or runs past `max_cycles`. The machine is left
  // wherever that leaves it: use one dedicated to the purpose.
  bool call_guest(u32 fn, const std::vector<u32>& args, u32* result, u64 max_cycles = 4'000'000'000ull);
  bool missing_service_stub_active() const { return service_stub_; }

  // CPU timing model: extra cycles per bus access, by region. Real hardware
  // runs code from slow 16-bit ROM, so it executes far fewer than one
  // instruction per cycle. Values are calibrated against hardware recordings
  // (docs/timing.md). All zero = MAME-like "one instruction per cycle".
  // Values are in cycles (fractions allowed, resolution 1/8 cycle). The
  // defaults were fitted to real-hardware gameplay recordings (docs/timing.md).
  struct Timing {
    float rom16 = 0, rom32 = 0;           // BaseROM
    float cart16 = 0.75f, cart32 = 1.5f;  // cartridge ROM (< 0: same as BaseROM)
    float ram16 = 1.0f, ram32 = 2.0f;     // main RAM
    float sram16 = 0, sram32 = 0;         // on-chip SRAM / VRAM
    float io16 = 1.5f, io32 = 1.5f;       // peripherals
    // Caches (sizes 0 = none). With a cache, cacheable memory (ROM, RAM)
    // costs nothing extra on a hit and a line fill on a miss, charged at the
    // fill rate per 16-bit transfer; the flat values above then apply only to
    // uncached regions.
    u32 icache_kb = 0, dcache_kb = 0, line_bytes = 32, ways = 2;
    float rom_fill16 = 0, ram_fill16 = 0;
    static Timing none() {  // 1 instruction per cycle
      Timing t;
      t.rom16 = t.rom32 = t.cart16 = t.cart32 = t.ram16 = t.ram32 = t.sram16 = t.sram32 = t.io16 = t.io32 = 0;
      t.icache_kb = t.dcache_kb = 0;
      return t;
    }
  } timing;
  // Re-apply `timing` to the bus (call after changing it).
  void apply_timing();

  // Value read from the 0x0180'9004 configuration strap register, excluding
  // the "no cartridge" bit which the machine sets itself. MAME uses 0x63ffbfff.
  u32 straps = 0x63ff'ffff;

 private:
  // MmioDevice for the 0x0180'0000 peripheral page.
  u32 mmio_read(u32 addr, int size) override;
  void mmio_write(u32 addr, u32 value, int size) override;

  void map_memory();
  u32 lcd_mode() const { return (lcd_[3] >> 31) ? (lcd_[3] & 0x3fff'ffff) : 0; }
  void auto_calibrate_step();
  template <class Ar>
  void serialize(Ar& ar);
  void render_frame();

  // Scheduler.
  u64 now() const { return cpu_.cycles(); }
  u64 next_event() const;
  void run_events();
  void reschedule() { cpu_.request_exit(); }

  // Timers (0x0180'd000 page).
  struct Timer {
    u32 ticks_base = 0;   // Count at `base_cycle`.
    u64 base_cycle = 0;
    u32 control = 0;
    u32 max = 0xffffffff;
    u64 overflow_at = 0;  // CPU cycle of the next overflow.
  };
  u32 timer_count(int i) const;
  void timer_set(int i, u32 count);
  u32 timer_read(u32 off);
  void timer_write(u32 off, u32 v);

  // ADC / touchscreen.
  void adc_write(unsigned reg, u32 v);
  u32 adc_read(unsigned reg);
  void adc_push(unsigned channel, u32 data);
  void adc_sample();
  void adc_irq() { int_flags_ |= 0x100; cpu_.raise_irq(0x10); }

  // Sound.
  void snd_write16(u32 off, u16 v);
  void snd_command(u32 data);
  u8 snd_triggered() const;
  void snd_mix_sample();
  // PCM output: two DMA channels playing 16-bit samples from circular
  // 512-byte buffers at 32 kHz (docs/hardware.md, "PCM output").
  struct PcmChannel { u32 ctrl = 0, base = 0, pos = 0; };
  PcmChannel pcm_[2];
  u64 pcm_next_ = 0;   // next 32 kHz tick while a channel plays
  s16 voice_hold_ = 0; // the latest voice mix (8 kHz), held for the 32 kHz output
  bool pcm_active() const { return (pcm_[0].ctrl | pcm_[1].ctrl) & 1; }
  void pcm_write(unsigned ch, bool addr, u32 v);
  void pcm_tick();
  void audio_out(s16 s);

  // EEPROMs.
  u8& eeprom_cell(u32 cmd);

  Bus bus_;
  arc::Cpu cpu_;

  std::vector<u8> bios_;   // 8 MiB, zero padded (possibly patched)
  std::vector<u8> bios_pristine_;  // as loaded
  std::string bios_problem_;
  bool unsigned_patched_ = false;
  void apply_bios_patches();
  void apply_service_stub();
  bool service_stub_ = false;      // (stub_missing_services, applied)
  std::vector<u8> stub_page_;      // its code, mapped at kStubBase while applied
  std::vector<u8> stub_table_page_;
  std::vector<u8> call_page_;  // (call_guest's return point)
  u32 stub_args_[5] = {};          // r0-r3 and the return address, from the stub
  struct StubFile {
    u32 open = 0, writable = 0, pos = 0, info = 0;  // info: the current-program file (below)
  };
  std::string download_name_;  // a download's file name without extension
  StubFile stub_files_[4];
  u32 stub_file_made_ = 0;  // the save file was created this session (it may still be all zeros)
  u32 service_call();
  u32 stub_file_call(unsigned fn);
  std::vector<u8> cart_;   // padded to a page multiple
  std::vector<u8> vram_;   // 64 KiB at 0x0300'0000
  std::vector<u8> ram_;    // 0x3c00'0000..0x3fff'ffff
  std::vector<u8> ram_image_;  // a downloadable, copied to RAM at ram_image_at_ on reset
  u32 ram_image_at_ = 0;
  RomHeader bios_header_, cart_header_;
  u32 bios_crc_ = 0, cart_crc_ = 0;
  bool bios_loaded_ = false;
  PatchResult cart_patch_;
  void patch_cart(std::vector<u8>& img);

  std::vector<u32> fb_;
  u64 frames_ = 0;
  u64 next_frame_ = 0;

  // Registers.
  u32 int_flags_ = 0;
  u32 clock_div_ = 0;
  u32 lcd_[7]{};
  u32 dma_[5]{};
  u64 dma_count_ = 0;
  bool native_capture_ = false;
  DrawCapture capture_;
  std::optional<DrawCapture::Sites> capture_sites_;  // (found once per BaseROM and cartridge)
  void update_capture(bool images_changed = true);  // (images_changed: the BaseROM or cartridge was replaced)
  u32 buttons_ = 0;
  bool power_switch_ = true, powered_off_ = false;
  u32 int_enable_ = 0;  // 0x0180'0084: which flags of 0x0180'0080 interrupt
  u64 input_reads_ = 0, frame_reads_ = 0;  // button / touch reads (lag detection)
  bool lagged_ = false;
  std::array<Timer, 3> timers_;
  std::array<u16, 0x800> palette_{};  // Aux 0x1a writes (purpose unconfirmed)
  unsigned palette_ptr_ = 0;

  // ADC.
  u32 adc_ctrl_[4]{};
  std::deque<u32> adc_fifo_;
  bool adc_enabled_ = false;
  bool touch_initted_ = false;
  u64 adc_next_ = 0;
  bool touch_down_ = false;
  u32 touch_x_ = 0x3df, touch_y_ = 0x3df;
  // Automatic calibration tapper (host-side helper, not saved in states).
  struct AutoCal {
    int phase = 0;  // 0 = looking, 1 = pressing, 2 = releasing
    int frames = 0;
    int x = -1, y = -1;
    int last_x = -1, last_y = -1;
    int same_target_frames = 0;
  } autocal_;

  // EEPROM (system + cartridge).
  u32 eeprom_cmd_ = 0;
  std::array<u8, 512> sys_eeprom_{};
  std::array<u8, 2048> cart_eeprom_{};

  // UART.
  std::string uart_;

  // Sound.
  struct Voice {
    u32 start = 0, end = 0;
    u16 volume = 0, pitch = 0;
    bool active = false;
    double pos = 0, step = 0;
  };
  std::array<Voice, 8> voices_;
  u16 master_volume_ = 0x4000;  // sound register 0x1c4, set from the volume buttons' setting
  u8 lcd_brightness_ = 0x30, lcd_ctrl_ = 0;  // LCD controller registers 0x0180'c024 / c020
  // Voice 7: LFC speech decoder and its codebook page register (0x0180'20e0/e4).
  CelpDecoder celp_;
  u16 celp_cb_[2]{};
  void celp_load_codebook();
  u64 audio_next_ = 0;
  std::deque<s16> audio_;
};

}  // namespace leap
