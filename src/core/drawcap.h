#pragma once

// Native draw capture: follows a game engine's own draw calls so the display
// can re-render frames with in-between positions (smooth motion) instead of
// estimating motion from pixels. Display-only: capture never changes
// emulation (it uses observation hooks, see arc::Cpu::set_pc_hook).
//
// Supported engines:
// - the tile-plane engine used by Sonic X and Go Diego Go! Animal Rescuer. It
//   composes each frame in a 160x160 16-bit (0x0RGB) surface from tile planes
//   (DrawPlane: 32x32 ring tilemap of 8x8 4bpp tiles, scroll in 1/4096 pixel,
//   clip rectangle, opaque/transparent/alpha modes) and sprite layers
//   (DrawSpriteLayer: one draw per sprite object at an integer position), then
//   copies it to the LCD with one framebuffer DMA;
// - the race engine of Cars: an earlier version of the same planes (scroll in
//   whole pixels) for the sky, and a perspective floor drawn from a 128x128
//   ring tilemap seen from a camera (position and heading); see Floor;
// - the BaseROM's Flash player.

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/common.h"
#include "core/flash/render.h"

namespace leap {

class Bus;
namespace arc { class Cpu; }

// One game frame as the engine drew it.
struct NativeFrame {
  static constexpr int kW = 160, kH = 160;
  static constexpr u16 kDrawn = 0x8000;  // flag on captured 0x0RGB pixels

  struct Plane {
    u32 id = 0;                        // engine plane object (stable across frames)
    s32 scroll_x = 0, scroll_y = 0;    // 1/4096 pixel
    s32 clip[4] = {0, 0, kW, kH};      // x0, y0, x1, y1 (exclusive), screen pixels
    u8 mode = 0;                       // 0 opaque, 1 transparent, 2 alpha blended
    u16 alpha = 256;                   // mode 2: weight of the plane, 0..256
    std::array<u16, 32 * 32> map{};    // ring tilemap: index 0-9, hflip 0x400, vflip 0x800, palette 12-15
    std::array<u16, 256> palette{};    // 16 palettes x 16 colours (0x0RGB)
    std::vector<u8> tiles;             // 32 bytes per tile, up to the highest index used
    // Pixel of the plane at world coordinates, kDrawn | colour, or 0 where
    // the plane is transparent.
    u16 sample(int wx, int wy) const;
  };
  struct Sprite {
    u32 id = 0;                   // engine sprite object (stable across frames)
    int x = 0, y = 0;             // engine position (screen pixels)
    s32 scale = 0x10000;          // the engine's drawing scale (16.16), around (x, y)
    int bx = 0, by = 0, bw = 0, bh = 0;  // bounding box of the pixels it drew
    std::vector<u16> px;          // bw x bh, kDrawn | colour, 0 = untouched
  };
  // A perspective ground plane (Cars): each screen row from horizon - 1 down
  // shows the ground at a depth from a table, along the camera's heading.
  struct Floor {
    s32 cam_x = 0, cam_y = 0;  // camera position on the ground, 1/4096 texel
    u32 heading = 0;           // 1/4096 of a step, 256 steps a turn
    int horizon = 0;
    std::vector<u16> map;      // 128x128 ring of tile entries (tile 0-11, palette 12-15)
    std::vector<u8> tiles;     // 32 bytes per 8x8 4bpp tile
    std::array<u16, 256> palette{};
    std::array<s32, 256> sine{}, cosine{};  // x4096
    std::array<s32, 171> depth{};           // per table row
    int depth_row0 = 10;                    // table row of screen row horizon - 1 (the first drawn)
    // The view, as the engine computes it: camera, sine and cosine of the
    // heading (x4096), and the horizon row (the camera tilts when the car jumps).
    struct View {
      double x = 0, y = 0;
      double sine = 0, cosine = 0;
      double horizon = 0;
    };
    View view() const;
    // The view from `a` to `b` at t; false if the motion between them is a cut.
    static bool between(const Floor& a, const Floor& b, double t, View& out);
    // Draws rows first_row() .. kH - 1 seen from `v` into out (0x0RGB, kW
    // wide, first_row() first), rounding the view to the engine's integers
    // (its horizon is the floor's own): exact for the views it drew.
    void draw(const View& v, u16* out) const;
    // At `scale` x resolution, screen rows `top` .. kH - 1 (ARGB, kW*scale
    // wide; transparent above the view's horizon).
    void draw_scaled(const View& v, int scale, int top, u32* out) const;
    int first_row() const { return horizon - 1; }
    u16 texel(s32 u, s32 v) const;
  };
  enum class OpKind : u8 { kPlane, kSprite, kFloor };
  struct Op { OpKind kind; u32 index; };

  std::vector<Plane> planes;
  std::vector<Sprite> sprites;
  std::vector<Floor> floors;
  std::vector<Op> ops;               // draw order
  // Flash frames (BaseROM Flash player) carry the player's display list
  // instead of planes and sprites; they are redrawn by flash::Renderer.
  std::shared_ptr<const flash::Frame> flash;

  std::vector<u16> screen;           // image sent to the LCD (0x0RGB)
  std::vector<u16> residual;         // screen pixels the ops don't reproduce (kDrawn | colour)
  u32 residual_count = 0;
  u64 frame_index = 0;               // emulated frame in which it was displayed

  // Replays the ops at their captured positions (0x0RGB, kW x kH). Matches
  // the engine's output exactly for planes and sprites.
  void compose(u16* out) const;
  // Few enough unexplained pixels to trust re-rendering this frame.
  bool trusted() const { return flash ? !flash->unsupported : residual_count <= u32(kW * kH / 4); }
};

// A layer of an interpolated frame: an image placed at a fractional screen
// position, clipped to a screen rectangle, drawn with an opacity.
struct NativeLayer {
  const u32* px = nullptr;  // ARGB, alpha 0 = transparent
  int w = 0, h = 0;
  float texel = 1;          // size of one layer pixel in screen pixels
  float x = 0, y = 0;       // screen position of px[0]
  s32 clip[4] = {0, 0, NativeFrame::kW, NativeFrame::kH};
  u8 opacity = 255;
  // Nonzero: the same number means the same pixels as before (the caller
  // can keep its copy); 0: unknown.
  u64 version = 0;
};

// Builds the layers showing the motion from frame `a` to frame `b` at
// t in [0, 1] (t = 1 is `b` exactly). Planes and sprites found in both frames
// glide between their positions, a floor's camera moves between its views;
// everything else is shown as in `b`. A floor is drawn at `scale` x
// resolution. The returned layers point into `storage`, which the caller
// keeps alive.
void native_layers(const NativeFrame& b, const NativeFrame* a, double t, std::vector<std::vector<u32>>& storage,
                   std::vector<NativeLayer>& layers, int scale = 1);
// Software composite of `layers` at `scale` x resolution (ARGB, 160*scale square).
void composite_layers(const std::vector<NativeLayer>& layers, int scale, std::vector<u32>& out);

u32 rgb12_to_argb(u16 c);

class DrawCapture {
 public:
  // Where the supported engines' routines are (guest addresses; 0: absent).
  struct Sites {
    u32 plane = 0, sprite_layer = 0;  // cartridge tile-plane engine: DrawPlane, DrawSpriteLayer
    u32 flash_object = 0;             // BaseROM Flash player: draw one display object
    u32 text_field = 0, glyph = 0;    // its text engine: draw a text field, draw a device-font glyph
    u32 outline_glyph = 0;            // its outline-font glyph loop (the hook point)
    u32 race_plane = 0;               // Cars race engine: its DrawPlane
    u32 race_sprite = 0, race_sprite_end = 0;  // its sprite-layer loop: an object's draw, the step to the next
    u32 floor = 0;                    // its floor routine
    u32 floor_ctx = 0, floor_sine = 0, floor_cosine = 0, floor_depth = 0;  // the floor's data (RAM)
    u32 floor_row0 = 0;               // depth table row of the floor's first row
  };
  // Looks for them in the cartridge image (mapped at `cart_base`) and the
  // BaseROM (at `bios_base`, for its Flash player).
  static Sites locate(const std::vector<u8>& cart, u32 cart_base, const std::vector<u8>& bios, u32 bios_base);
  // Hooks those found. Returns false if there were none.
  bool install(const Bus& bus, arc::Cpu& cpu, const Sites& sites);
  void uninstall(arc::Cpu& cpu);
  bool installed() const { return installed_; }
  const std::string& engine() const { return engine_; }

  // Called by the machine for every framebuffer DMA (source address, bytes).
  void on_dma(u32 src, u32 bytes, u64 frame_index);
  // Drop both in-progress and published display history (reset, state load).
  void reset();

  // The most recently completed frame, or null.
  std::shared_ptr<const NativeFrame> latest() const { return done_; }

 private:
  static void hook_plane(void* self, const arc::Cpu& cpu);
  static void hook_sprite_begin(void* self, const arc::Cpu& cpu);
  static void hook_sprite_end(void* self, const arc::Cpu& cpu);
  static void hook_flash_object(void* self, const arc::Cpu& cpu);
  static void hook_text_field(void* self, const arc::Cpu& cpu);
  static void hook_glyph(void* self, const arc::Cpu& cpu);
  static void hook_outline_glyph(void* self, const arc::Cpu& cpu);
  static void hook_race_plane(void* self, const arc::Cpu& cpu);
  static void hook_floor(void* self, const arc::Cpu& cpu);
  static void hook_race_sprite(void* self, const arc::Cpu& cpu);
  bool begin_op(u32 surface, bool other_surface_ok);
  void publish_flash(u64 frame_index);
  void flush_sprite();
  bool read_surface(std::vector<u16>& out) const;
  bool read_u16s(u32 addr, u16* out, size_t n) const;

  const Bus* bus_ = nullptr;
  Sites sites_;
  bool installed_ = false;
  std::string engine_;
  u32 surface_px_ = 0;       // composed surface pixels (from the planes' target)
  u32 race_surface_ = 0;     // Cars: the surface the floor is drawn to (the screen's)
  bool unsupported_ = false; // something in this frame can't be captured
  std::shared_ptr<NativeFrame> cur_;
  std::shared_ptr<const NativeFrame> done_;
  bool pending_ = false;
  NativeFrame::Sprite pend_;
  std::vector<u16> before_, after_;
  // Flash player.
  bool flash_hooked_ = false;
  u32 flash_ctx_ = 0;        // player context (from its display objects)
  bool flash_drawn_ = false; // the player drew since the last DMA
  std::shared_ptr<const flash::Frame> flash_last_;
  // Dynamic text fields: the glyphs of each field's latest drawing.
  struct FieldCapture {
    u64 gen = ~0ull;         // Flash frame in which it was drawn
    bool placed_set = false;
    swf::Matrix placed;
    u32 def = 0;             // the field's definition (display objects' memory is reused)
    std::vector<flash::Frame::Glyph> glyphs;
    std::vector<char> stale;  // per glyph: in a region being redrawn, and not drawn again (yet)
    std::vector<u64> drawn;   // per glyph: Flash frame it was last drawn in
    std::vector<flash::Frame::Outline> outlines;  // embedded-font glyphs of its latest drawing
  };
  std::unordered_map<u32, FieldCapture> fields_;
  u32 text_obj_ = 0;         // field being drawn
  u32 glyph_surface_ = 0;    // the text engine's drawing surface (its clip is the region being redrawn)
  bool text_drawn_ = false;  // a field was drawn since the last Flash frame
  bool stray_glyphs_ = false;  // glyphs drawn outside any field (text we can't place)
  u64 flash_gen_ = 0;
};

}  // namespace leap
