// ROM-free regressions for capture, vector rendering and display interpolation.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <vector>

#include "core/arc/cpu.h"
#include "core/bus.h"
#include "core/interp.h"

using namespace leap;

namespace {
int failed = 0, checks = 0;
#define CHECK(expr) do { checks++; if (!(expr)) { failed++; \
  std::printf("FAIL line %d: %s\n", __LINE__, #expr); } } while (0)

void test(const char* name, const std::function<void()>& fn) {
  const int before = failed;
  fn();
  std::printf("%s %s\n", failed == before ? "ok  " : "FAIL", name);
}

// Exercise the public hook -> snapshot -> DMA publication path. Only the
// player's data layout is needed, no BIOS image.
struct CaptureRig {
  Bus bus;
  std::vector<u8> mem = std::vector<u8>(Bus::kPageSize);
  arc::Cpu cpu{bus};
  DrawCapture capture;
  static constexpr u32 ctx = 0x1000, root = ctx + 0x2c, entry = 0x100;
  CaptureRig() {
    bus.map_memory(0, u32(mem.size()), mem.data(), true);
    bus.write16(entry, 0x214a); bus.write16(entry + 2, 0);  // mov r1,0
    cpu.set_reset_vector(entry); cpu.reset();
    bus.write32(root + 8, ctx);
    bus.write32(root + 0x2c, 65536); bus.write32(root + 0x38, 65536);
    bus.write8(root + 0x6c, 1);
    bus.write32(ctx, 65536); bus.write32(ctx + 12, 65536);
    bus.write32(ctx + 0xf4, 3200); bus.write32(ctx + 0xfc, 3200);
    DrawCapture::Sites sites;
    sites.flash_object = entry;
    CHECK(capture.install(bus, cpu, sites));
  }
  std::shared_ptr<const NativeFrame> publish(u64 t) {
    cpu.set_reg(0, root); cpu.set_pc(entry); cpu.step();
    capture.on_dma(0, 160 * 160 * 3 / 2, t);
    return capture.latest();
  }
};

// A minimal DefineShape3 SHAPEWITHSTYLE: a solid square, [0,1000]^2.
std::vector<u8> square() {
  std::vector<u8> v{1, 0, 255, 0, 0, 255, 0};  // one RGBA fill, no lines
  size_t bit = v.size() * 8;
  auto put = [&](u32 value, unsigned n) {
    for (unsigned i = n; i > 0; i--, bit++) {
      if (bit / 8 == v.size()) v.push_back(0);
      v[bit / 8] |= u8(((value >> (i - 1)) & 1) << (7 - bit % 8));
    }
  };
  put(1, 4); put(0, 4);  // fill/line index bits
  put(0, 1); put(4, 5); put(1, 1);  // select fill1
  for (auto [dx, dy] : {std::pair{1000, 0}, {0, 1000}, {-1000, 0}, {0, -1000}}) {
    put(1, 1); put(1, 1); put(9, 4); put(1, 1);  // straight, 11-bit general line
    put(u32(dx), 11); put(u32(dy), 11);
  }
  put(0, 6);
  return v;
}

flash::Frame parts() {
  flash::Frame f;
  flash::Object root;
  root.type = flash::Frame::kTypeRoot;
  f.objects.push_back(root);
  flash::Object big;
  big.id = 1; big.parent = 0; big.depth = 1; big.def = 0x1000; big.tag = 32;
  big.local.tx = big.local.ty = 200;
  f.objects.push_back(big);
  auto small = big;
  small.id = 2; small.depth = 2;
  small.local = {0.25, 0, 0, 0.25, 300, 300};
  small.local_cx.mul[0] = 0; small.local_cx.add[2] = 255;
  f.objects.push_back(small);
  return f;
}

bool near(const swf::Matrix& a, const swf::Matrix& b) {
  return std::abs(a.a-b.a) < 1e-9 && std::abs(a.b-b.b) < 1e-9 &&
         std::abs(a.c-b.c) < 1e-9 && std::abs(a.d-b.d) < 1e-9 &&
         std::abs(a.tx-b.tx) < 1e-9 && std::abs(a.ty-b.ty) < 1e-9;
}

std::shared_ptr<NativeFrame> plain(u32 rgba, bool unsupported = false) {
  auto n = std::make_shared<NativeFrame>();
  auto f = std::make_shared<flash::Frame>();
  f->background = rgba; f->unsupported = unsupported;
  n->flash = f;
  return n;
}
}  // namespace

int main() {
  test("capture publishes morph-only and view-only changes", [] {
    CaptureRig r;
    auto a = r.publish(1);
    CHECK(a && a->flash);
    CHECK(r.publish(2) == a);  // duplicate dirty-region DMA
    r.bus.write16(CaptureRig::root + 0x20, 32768);
    auto b = r.publish(3);
    CHECK(b != a);
    CHECK(b->flash->objects[0].ratio == 32768);
    r.bus.write32(CaptureRig::ctx, 131072);
    auto c = r.publish(4);
    CHECK(c != b);
    CHECK(std::abs(c->flash->view.a - 0.1) < 1e-9);
    CHECK(r.publish(5) == c);
    r.capture.reset();
    CHECK(!r.capture.latest());
  });
  test("skeleton cache matches a fresh renderer after c/d changes", [] {
    auto data = square();
    flash::RomView rom{{{0x1000, data.data(), data.size()}}};
    for (bool shear : {false, true}) {
      auto a = parts(), b = a;
      b.objects[1].local.tx += 100; b.objects[2].local.tx += 100;
      flash::Renderer cached, fresh;
      cached.set_rom(rom); fresh.set_rom(rom);
      std::vector<u32> x, y;
      CHECK(cached.render(b, &a, 0.75, 1, x));
      if (shear) b.objects[2].local.c = 0.2;
      else b.objects[2].local.d = 0.5;
      CHECK(cached.render(b, &a, 0.75, 1, x));
      CHECK(fresh.render(b, &a, 0.75, 1, y));
      CHECK(near(cached.interpolated_locals()[2], fresh.interpolated_locals()[2]));
      CHECK(x == y);
    }
  });
  test("Flash long pauses switch immediately", [] {
    FrameInterpolator i;
    i.set_mode(FrameInterpolator::Mode::Native);
    std::vector<u32> black(160*160, 0xff000000), white(160*160, 0xffffffff);
    i.push(black.data(), 0, plain(0x000000ff));
    i.push(white.data(), 100, plain(0xffffffff));
    std::vector<NativeLayer> layers;
    CHECK(i.layers(100, 2, layers));
    CHECK(layers[0].px[0] == 0xffffffff);
  });
  test("overlapping clip-depth masks intersect and expire independently", [] {
    auto data = square();
    flash::RomView rom{{{0x1000, data.data(), data.size()}}};
    auto f = parts(); f.objects.resize(1);
    flash::Object mask;
    mask.parent = 0; mask.depth = 1; mask.clip_depth = 4; mask.def = 0x1000; mask.tag = 32;
    f.objects.push_back(mask);  // mask A: x=0..50, active through depth 4
    mask.depth = 2; mask.clip_depth = 3; mask.local.tx = 400;
    f.objects.push_back(mask);  // mask B: x=20..70, active only through depth 3
    auto fill = mask;
    fill.depth = 3; fill.clip_depth = 0; fill.local = {};
    fill.local_cx.mul[0] = 0; fill.local_cx.add[2] = 255;
    f.objects.push_back(fill);  // blue, masked by A and B
    fill.depth = 4; fill.local = {1.6, 0, 0, 0.1, 0, 400};
    fill.local_cx.add[2] = 0; fill.local_cx.add[1] = 255;
    f.objects.push_back(fill);  // green stripe, masked by A alone
    fill.depth = 5; fill.local = {0.1, 0, 0, 0.1, 1200, 1200}; fill.local_cx = {};
    f.objects.push_back(fill);  // red square outside both expired masks
    flash::Renderer r; r.set_rom(rom);
    std::vector<u32> image;
    for (int k : {1, 4}) {
      CHECK(r.render(f, nullptr, 1, k, image));
      auto pixel = [&](int x, int y) { return image[size_t(y*k) * (160*k) + x*k]; };
      for (int y : {10, 30, 45}) {
        CHECK(pixel(10, y) == 0xffffffff);
        CHECK(pixel(30, y) == 0xff0000ff);
      }
      CHECK(pixel(10, 22) == 0xff00ff00);
      CHECK(pixel(60, 22) == 0xffffffff);
      CHECK(pixel(62, 62) == 0xffff0000);
      // Crossing intervals: B outlives A, so only B clips depth 4.
      f.objects[1].clip_depth = 3; f.objects[2].clip_depth = 4;
      CHECK(r.render(f, nullptr, 1, k, image));
      CHECK(pixel(10, 22) == 0xffffffff);
      CHECK(pixel(60, 22) == 0xff00ff00);
      CHECK(pixel(62, 62) == 0xffff0000);
      f.objects[1].clip_depth = 4; f.objects[2].clip_depth = 3;
    }
  });
  test("failed previous redraw is not presented as a complete layer", [] {
    FrameInterpolator i;
    i.set_mode(FrameInterpolator::Mode::Native);
    // Close colours keep the pixel-motion path from classifying this as a cut.
    std::vector<u32> black(160*160, 0xff000000), gray(160*160, 0xff010101);
    i.push(black.data(), 0, plain(0x000000ff, true));
    i.push(gray.data(), 4, plain(0x010101ff));
    std::vector<NativeLayer> layers;
    CHECK(!i.layers(4, 2, layers));
    CHECK(i.render(4)[0] == 0xff010101);
    CHECK(i.layers(8, 2, layers));
    CHECK(layers[0].px[0] == 0xff010101);
  });
  test("stale Flash validation expires even with an unchanged LCD", [] {
    FrameInterpolator i;
    i.set_mode(FrameInterpolator::Mode::Native);
    std::vector<u32> black(160*160, 0xff000000), white(160*160, 0xffffffff);
    i.push(black.data(), 0, plain(0x000000ff));
    for (u64 t = 1; t <= 4; t++) i.push(white.data(), t);
    std::vector<NativeLayer> layers;
    CHECK(!i.layers(4, 1, layers));
    i.reset();
    i.push(white.data(), 5, plain(0x000000ff));
    CHECK(i.layers(5, 1, layers));  // new history gets its own grace period
    auto stale = plain(0x000000ff);
    for (u64 t = 6; t <= 10; t++) i.push(white.data(), t, stale);
    CHECK(!i.layers(10, 1, layers));
    i.reset();
    i.push(white.data(), 11, plain(0x000000ff));
    CHECK(i.layers(11, 1, layers));
  });
  test("Flash frames arriving unevenly still move continuously", [] {
    auto data = square();
    flash::RomView rom{{{0x1000, data.data(), data.size()}}};
    flash::Renderer r; r.set_rom(rom);
    FrameInterpolator i;
    i.set_mode(FrameInterpolator::Mode::Native);
    i.set_rom(rom);
    // The square moves 10 px per frame; the frames arrive 8, 17, 5, 8, 8 ticks apart.
    const u64 times[] = {0, 8, 25, 30, 38, 46};
    auto centre = [](const std::vector<u32>& img) {
      double sx = 0, n = 0;
      for (int y = 0; y < 160; y++)
        for (int x = 0; x < 160; x++) {
          const double red = double((img[size_t(y) * 160 + size_t(x)] >> 16) & 255) - double(img[size_t(y) * 160 + size_t(x)] & 255);
          if (red > 0) { sx += red * x; n += red; }
        }
      return n > 0 ? sx / n : -1.0;
    };
    std::vector<double> xs;
    std::vector<NativeLayer> layers;
    std::vector<u32> lcd, img;
    size_t next = 0;
    for (double t = 0; t <= 80; t += 0.25) {
      if (next < std::size(times) && t >= double(times[next])) {
        auto n = std::make_shared<NativeFrame>();
        auto f = std::make_shared<flash::Frame>(parts());
        f->objects.resize(2);
        f->objects[1].local.tx = 200 + 200.0 * double(next);
        f->background = 0xffffffff;
        n->flash = f;
        r.render(*f, nullptr, 1, 1, lcd);
        i.push(lcd.data(), times[next], n);
        next++;
      }
      if (!i.layers(t, 1, layers)) continue;
      composite_layers(layers, 1, img);
      xs.push_back(centre(img));
    }
    CHECK(xs.size() > 200);
    double worst = 0;
    for (size_t k = 1; k < xs.size(); k++) worst = std::max(worst, std::abs(xs[k] - xs[k - 1]));
    CHECK(worst < 1.5);  // at most ~1 px per quarter tick; a jump would be several px
    CHECK(std::abs(xs.back() - xs.front() - 50.0) < 0.5);  // ends on the newest frame
  });
  test("native draw lists arriving unevenly still move continuously", [] {
    // A red 8x8 sprite moves 10 px per game frame; frames arrive 4 or 5 ticks
    // apart (as in the Cars race).
    const u64 times[] = {0, 4, 9, 13, 17, 22, 26, 30};
    FrameInterpolator i;
    i.set_mode(FrameInterpolator::Mode::Native);
    std::vector<double> xs;
    std::vector<NativeLayer> layers;
    std::vector<u32> lcd(160 * 160), img;
    size_t next = 0;
    for (double t = 0; t <= 50; t += 0.25) {
      if (next < std::size(times) && t >= double(times[next])) {
        auto n = std::make_shared<NativeFrame>();
        NativeFrame::Sprite sp;
        sp.id = 1;
        sp.x = sp.bx = 20 + 10 * int(next);
        sp.y = sp.by = 70;
        sp.bw = sp.bh = 8;
        sp.px.assign(64, u16(NativeFrame::kDrawn | 0xf00));
        n->sprites.push_back(sp);
        n->ops.push_back({NativeFrame::OpKind::kSprite, 0});
        n->screen.resize(160 * 160);
        n->compose(n->screen.data());
        for (size_t k = 0; k < lcd.size(); k++) lcd[k] = rgb12_to_argb(n->screen[k]);
        i.push(lcd.data(), times[next], n);
        next++;
      }
      if (!i.layers(t, 1, layers)) continue;
      composite_layers(layers, 1, img);
      double sx = 0, cnt = 0;
      for (int y = 0; y < 160; y++)
        for (int x = 0; x < 160; x++)
          if (img[size_t(y) * 160 + size_t(x)] == 0xffff0000u) { sx += x; cnt++; }
      if (cnt) xs.push_back(sx / cnt);
    }
    CHECK(xs.size() > 150);
    double worst = 0;
    for (size_t k = 1; k < xs.size(); k++) worst = std::max(worst, std::abs(xs[k] - xs[k - 1]));
    CHECK(worst < 1.5);  // a jump back or ahead would be several px
    CHECK(std::abs(xs.back() - xs.front() - 70.0) < 0.5);  // ends on the newest frame
  });
  test("matrix turn keeps a rigid pivot and is reversible", [] {
    const double theta = 0.8, cs = std::cos(theta), sn = std::sin(theta);
    const swf::Matrix a, b{cs, sn, -sn, cs, 200 - cs*200 + sn*100, 100 - sn*200 - cs*100};
    for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
      const auto m = swf::Matrix::lerp(a, b, t);
      double x, y; m.apply(200, 100, x, y);
      CHECK(std::hypot(x - 200, y - 100) < 1e-9);
      CHECK(near(m, swf::Matrix::lerp(b, a, 1-t)));
    }
  });
  test("a point both endpoints share stays fixed under non-uniform scaling", [] {
    const swf::Matrix p, q{2, 0, 0, 1, -2000, 0};  // stretch X about x = 2000
    for (double t : {0.25, 0.5, 0.75}) {
      double x, y;
      swf::Matrix::lerp(p, q, t).apply(2000, 0, x, y);
      CHECK(std::hypot(x - 2000, y) < 1e-6);
    }
  });
  test("a carried part keeps its joint while turning and stretching", [] {
    auto data = square();
    flash::RomView rom{{{0x1000, data.data(), data.size()}}};
    auto a = parts(), b = a;
    // The small part turns 30 degrees and stretches 2x along X about the joint
    // J = (400, 400) (twips), which lies inside both parts.
    const double th = 0.5235987755982988, cs = std::cos(th), sn = std::sin(th);
    const swf::Matrix to_j{1, 0, 0, 1, 400, 400}, from_j{1, 0, 0, 1, -400, -400};
    const swf::Matrix turn{2 * cs, 2 * sn, -sn, cs, 0, 0};
    b.objects[2].local = to_j * turn * from_j * a.objects[2].local;
    double px, py;  // J in the small part's space
    a.objects[2].local.inverse().apply(400, 400, px, py);
    flash::Renderer r;
    r.set_rom(rom);
    std::vector<u32> image;
    for (double t : {0.25, 0.5, 0.75}) {
      CHECK(r.render(b, &a, t, 1, image));
      double x, y;
      r.interpolated_locals()[2].apply(px, py, x, y);
      CHECK(std::hypot(x - 400, y - 400) < 1e-3);
    }
  });
  test("children of a replaced clip do not glide", [] {
    auto data = square();
    flash::RomView rom{{{0x1000, data.data(), data.size()}}};
    flash::Frame a;
    flash::Object root;
    root.type = flash::Frame::kTypeRoot;
    a.objects.push_back(root);
    flash::Object clip;
    clip.id = 1; clip.parent = 0; clip.depth = 1; clip.type = flash::Frame::kTypeSprite; clip.def = 0x2000;
    a.objects.push_back(clip);
    flash::Object part;
    part.id = 2; part.parent = 1; part.depth = 1; part.def = 0x1000; part.tag = 32; part.local.tx = 200;
    a.objects.push_back(part);
    auto b = a;
    b.objects[2].local.tx = 1000;
    flash::Renderer r;
    r.set_rom(rom);
    std::vector<u32> image;
    CHECK(r.render(b, &a, 0.75, 1, image));
    CHECK(std::abs(r.interpolated_locals()[2].tx - 800) < 1e-6);   // same clip: glides
    b.objects[1].def = 0x3000;                                      // the timeline replaced the clip
    CHECK(r.render(b, &a, 0.75, 1, image));
    CHECK(std::abs(r.interpolated_locals()[2].tx - 1000) < 1e-6);  // new content: shown as is
  });
  test("a view-only pan moves smoothly across the midpoint", [] {
    auto data = square();
    flash::RomView rom{{{0x1000, data.data(), data.size()}}};
    auto a = parts(), b = a;
    b.view.tx += 20;
    flash::Renderer r;
    r.set_rom(rom);
    auto first_x = [&](double t) {
      std::vector<u32> image;
      r.render(b, &a, t, 1, image);
      for (int x = 0; x < 160; x++)
        if (image[size_t(15) * 160 + size_t(x)] != 0xffffffffu) return x;
      return -1;
    };
    CHECK(std::abs(first_x(0.49999) - first_x(0.50001)) <= 1);
    CHECK(first_x(0.0) + 20 == first_x(1.0));
  });
  std::printf("%d checks, %d failures\n", checks, failed);
  return failed ? 1 : 0;
}
