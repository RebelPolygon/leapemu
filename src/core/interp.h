#pragma once

#include <array>
#include <deque>
#include <memory>
#include <vector>

#include "core/common.h"
#include "core/drawcap.h"

namespace leap {

// Display-side frame interpolation for smoother motion. Leapster games update
// their logic once per rendered frame (~15 fps in gameplay), so the emulator
// must present frames at that rate to keep gameplay speed correct. This class
// synthesises in-between images for the display only; it never affects
// emulation.
//
// The displayed image lags the emulation by one game frame: while frame B is
// the newest, the display animates from the previous frame A towards B.
//
// Modes: Motion estimates motion from the pixels; Native re-renders the
// game's own draw list (planes and sprites at in-between positions, Flash
// content as vectors, see core/drawcap.h) when the machine captured one, and
// uses Motion otherwise.
class FrameInterpolator {
 public:
  enum class Mode { Off, Motion, Native };

  // Animate between game frames. Off: show the newest frame as it is (Native
  // mode still redraws vector content at the requested scale), without lag.
  bool smoothing = true;
  static constexpr int kW = 160, kH = 160;

  void set_mode(Mode m) { mode_ = m; reset(); }
  Mode mode() const { return mode_; }
  void reset();

  // Call after every emulated 60 Hz frame. `frame_index` counts emulated
  // frames; identical consecutive images are ignored. `native` is the
  // machine's latest captured draw list (Native mode), if any.
  void push(const u32* fb, u64 frame_index, std::shared_ptr<const NativeFrame> native = nullptr);

  // Native mode: the layers to show at time `t` (see native_layers()), or
  // false when the current frames have no usable draw lists (then use
  // render()). Vector (Flash) content is drawn at `scale` output pixels per
  // screen pixel. Layer pixels stay valid until the next call.
  bool layers(double t, int scale, std::vector<NativeLayer>& out);

  // ROM images, needed to redraw Flash content (Native mode).
  void set_rom(const flash::RomView& rom) { flash_.set_rom(rom); reset(); }

  // Image to display at emulated time `t` (in frames, fractional). Returns the
  // latest pushed frame unchanged when interpolation is off or not possible.
  const u32* render(double t);

  // Longest gap (in 60 Hz frames) that is still interpolated; longer pauses
  // (menus, static screens) switch instantly.
  int max_gap = 8;
  // Captured frames further apart than this (in 60 Hz ticks) switch at once
  // instead of animating: a pause (a menu waiting for input), not motion.
  // Flash content runs at 8-12 fps, and the player sometimes skips a frame,
  // so gaps of 5-15 ticks are ordinary motion.
  int max_capture_gap = 20;

 private:
  struct Vec { int dx = 0, dy = 0; };
  // A foreground object: connected pixels of the newest frame that the camera
  // motion does not explain, and the displacement that best explains them.
  struct Object {
    std::vector<int> pixels;  // indices into the frame
    Vec d;                    // newest(p) ~ previous(p - d)
    bool explained = false;
  };

  void analyse();
  void compose(double alpha);

  double alpha_at(double t) const;
  void enqueue(std::shared_ptr<const NativeFrame> f, double t);
  void push_flash(const u32* fb, u64 frame_index, std::shared_ptr<const NativeFrame> native);
  void push_drawn(const u32* fb, u64 frame_index, std::shared_ptr<const NativeFrame> native);
  bool captured_ready() const { return fb_ && !fq_.empty() && fq_.back().ok; }
  void advance_clock(double t);
  double capture_period() const;

  Mode mode_ = Mode::Off;
  std::vector<std::vector<u32>> layer_storage_;
  std::vector<NativeLayer> layer_tmp_;
  std::vector<u32> composite_;
  // Flash content is redrawn from its display lists (the player updates the
  // LCD in several partial DMAs per frame).
  flash::Renderer flash_;
  // Recent captured frames (Flash, or native draw lists), oldest first, with
  // the time each was captured and whether its redraw matched the LCD. The
  // display plays them back on its own clock, which runs a little behind
  // (about one frame): frames arrive unevenly (Flash 2 to 17 ticks apart, the
  // Cars race 4 or 5), and animating each over the previous gap made the
  // image jump when a frame came early and stall when one came late.
  // Following the captures' own times, the motion is continuous; the clock
  // runs somewhat faster or slower to keep its lag near one typical interval.
  struct Capture { std::shared_ptr<const NativeFrame> f; double t; bool ok; };
  std::deque<Capture> fq_;
  std::shared_ptr<const NativeFrame> fb_;  // newest captured frame (fq_.back())
  std::shared_ptr<const NativeFrame> fq_before_;  // the frame before fq_.front() (how parts were moving)
  double show_t_ = 0;            // the display clock: the capture time being shown
  double clock_t_ = 0;           // time of the last clock update
  bool flash_ok_ = false;        // the newest Flash frame redraws faithfully
  int flash_mismatch_ = 0;       // consecutive LCD images the redraw did not match
  std::vector<u32> flash_img_, flash_check_;
  // What flash_img_ shows (a settled or held frame is not redrawn).
  struct Drawn {
    std::shared_ptr<const NativeFrame> show, prev, before;
    double alpha = -1;
    int scale = 0;
  } drawn_;
  u64 flash_version_ = 0;
  // The capture flash_check_ was made from, and what that redraw found.
  std::shared_ptr<const NativeFrame> checked_;
  bool check_complete_ = false;
  std::vector<std::array<int, 4>> check_uncaptured_, check_captured_;
  std::vector<u32> a_, b_, out_;       // previous / newest distinct frame, output
  std::vector<int> la_, lb_;           // luma of a_ / b_
  double ta_ = 0, tb_ = 0;             // presentation times of a_ / b_
  bool have_a_ = false, have_b_ = false;
  bool cut_ = false;                   // frames too different: no interpolation
  Vec global_;                         // camera motion: b(p) ~ a(p - global_)
  std::vector<u8> fg_a_, fg_b_;        // foreground masks (not explained by camera motion)
  std::vector<Object> objects_;
};

}  // namespace leap
