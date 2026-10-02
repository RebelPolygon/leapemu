// cutscene_eval: compares Flash frame interpolation in a running game with the
// movie's own frames. Runs the emulator from a state, captures each frame the
// BaseROM's player draws, and finds the SWF frame it shows (by rendering the
// movie's timeline with flash::Timeline). Between two captures that are k SWF
// frames apart, the interpolation at t = j/k is compared with SWF frame +j:
// the frames the player skipped or the emulator would show in between.
// Usage: cutscene_eval BIOS CART STATE SWF_ADDR|- TICKS [scale] [dump_prefix]
// Environment: PAIR=n lists how capture n's objects match capture n-1;
// STRIPS=prefix [STEPS=k] renders consecutive captures at k moments (only
// pair n with PAIR).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <string>
#include "core/flash/render.h"
#include "core/flash/timeline.h"
#include "core/image.h"
#include "core/machine.h"
using namespace leap;

static double error(const std::vector<u32>& a, const std::vector<u32>& b) {
  double s = 0;
  for (size_t i = 0; i < a.size(); i++)
    for (int sh = 0; sh < 24; sh += 8) s += std::abs(int((a[i] >> sh) & 255) - int((b[i] >> sh) & 255));
  return s / (a.size() * 3.0);
}

int main(int argc, char** argv) {
  if (argc < 6) { std::fprintf(stderr, "usage: cutscene_eval BIOS CART STATE SWF_ADDR TICKS [scale] [dump_prefix]\n"); return 2; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  m.set_native_capture(true);
  const u32 addr = u32(std::strtoul(argv[4], nullptr, 16));
  const int ticks = std::atoi(argv[5]), scale = argc > 6 ? std::atoi(argv[6]) : 2;
  const char* dump = argc > 7 ? argv[7] : nullptr;
  const flash::RomView rom = m.rom_view();
  swf::Movie movie;  // "-": no reference movie (PAIR / STRIPS only)
  const bool have_ref = std::string(argv[4]) != "-";
  if (have_ref && !movie.load(rom.ptr(addr), rom.avail(addr))) { std::fprintf(stderr, "no SWF at %08x\n", addr); return 2; }
  flash::Renderer r;
  r.set_rom(rom);
  if (const char* v = std::getenv("SKELETON")) r.skeleton = std::atoi(v) != 0;  // A/B comparisons
  if (const char* v = std::getenv("REUSE")) r.reuse_check = std::atoi(v) != 0;
  if (const char* v = std::getenv("POSE")) r.pose_switch = std::atoi(v) != 0;

  // The movie's frames, rendered at 1x (for matching) and at `scale`.
  flash::Timeline tl(movie, addr);
  std::vector<std::vector<u32>> ref1, refs;
  for (int i = 0; have_ref && i < tl.frame_count(); i++) {
    tl.step();
    const flash::Frame f = tl.snapshot();
    ref1.emplace_back();
    refs.emplace_back();
    r.render(f, nullptr, 1.0, 1, ref1.back());
    r.render(f, nullptr, 1.0, scale, refs.back());
  }
  if (have_ref) std::printf("SWF %08x: %d frames\n", addr, tl.frame_count());

  // Captured frames and the SWF frame each shows.
  std::vector<std::shared_ptr<const NativeFrame>> caps;
  std::vector<int> at;
  std::shared_ptr<const NativeFrame> last;  // (held: a freed frame's address can be reused)
  int from = 0;
  std::vector<u32> img;
  for (int t = 0; t < ticks; t++) {
    m.run_frame();
    const auto nf = m.draw_capture().latest();
    if (!nf || !nf->flash || nf == last) continue;
    last = nf;
    if (!have_ref) {
      if (std::getenv("TICKS_LOG")) std::printf("tick %4d: capture %zu\n", t, caps.size());
      caps.push_back(nf); at.push_back(int(caps.size())); continue;
    }
    r.render(*nf->flash, nullptr, 1.0, 1, img);
    int best = -1;
    double be = 1e9;
    for (int k = from; k < int(ref1.size()) && k < from + 40; k++) {
      const double d = error(img, ref1[size_t(k)]);
      if (d < be) { be = d; best = k; }
    }
    if (best < 0) break;
    std::printf("tick %4d: SWF frame %3d (match error %.2f)\n", t, best + 1, be);
    caps.push_back(nf);
    at.push_back(best);
    from = best;
  }

  // PAIR=n: the objects of capture n and how they match capture n-1.
  if (const char* pair = std::getenv("PAIR")) {
    const size_t c = size_t(std::atoi(pair));
    if (c >= 1 && c < caps.size()) {
      auto list = [&](const flash::Frame& f) {
        std::map<std::string, size_t> slots;
        std::vector<std::string> path(f.objects.size());
        std::vector<swf::Matrix> world(f.objects.size());
        for (size_t i = 0; i < f.objects.size(); i++) {
          const auto& o = f.objects[i];
          path[i] = (o.parent >= 0 ? path[size_t(o.parent)] + "/" : "") + std::to_string(o.depth);
          world[i] = o.parent >= 0 ? world[size_t(o.parent)] * o.local : f.view * o.local;
          slots[path[i]] = i;
        }
        return std::make_tuple(slots, path, world);
      };
      const auto& fa = *caps[c - 1]->flash;
      const auto& fb = *caps[c]->flash;
      auto [sa, pa, wa] = list(fa);
      auto [sb, pb, wb] = list(fb);
      for (size_t i = 0; i < fb.objects.size(); i++) {
        const auto& o = fb.objects[i];
        const auto it = sa.find(pb[i]);
        std::printf("%-24s type %3u id %4u inst %08x", pb[i].c_str(), o.type, o.char_id, o.id);
        if (it == sa.end()) { std::printf("  NEW slot\n"); continue; }
        const auto& p = fa.objects[it->second];
        const auto& A = wa[it->second];
        const auto& B = wb[i];
        std::printf("  %s %s  pos %6.1f,%6.1f -> %6.1f,%6.1f  (d %5.1f)  lin %.2f %.2f %.2f %.2f -> %.2f %.2f %.2f %.2f\n",
                    p.id == o.id ? "same-inst" : "new-inst ", p.def == o.def ? "same-def" : "new-def ", A.tx, A.ty, B.tx, B.ty,
                    std::hypot(B.tx - A.tx, B.ty - A.ty), A.a, A.b, A.c, A.d, B.a, B.b, B.c, B.d);
      }
    }
  }

  // STRIPS=prefix: every consecutive pair of captures at t = 0, 1/4, ... 1.
  if (const char* strips = std::getenv("STRIPS")) {
    const int W = 160 * scale, steps = std::getenv("STEPS") ? std::atoi(std::getenv("STEPS")) : 5;
    std::vector<u32> out(size_t(W) * steps * W), im;
    for (size_t c = 1; c < caps.size(); c++) {
      for (int j = 0; j < steps; j++) {
        r.render(*caps[c]->flash, caps[c - 1]->flash.get(), double(j) / (steps - 1), scale, im, c >= 2 ? caps[c - 2]->flash.get() : nullptr);
        for (int y = 0; y < W; y++)
          for (int x = 0; x < W; x++) out[size_t(y) * steps * W + size_t(j) * W + x] = im[size_t(y) * W + x];
      }
      if (std::getenv("PAIR") && c != size_t(std::atoi(std::getenv("PAIR")))) continue;
      char name[512];
      std::snprintf(name, sizeof(name), "%s-%03zu.png", strips, c);
      write_png(name, out.data(), steps * W, W);
    }
  }

  // Interpolation between consecutive captures vs the skipped frames.
  double sum_hold = 0, sum_int = 0;
  int n = 0, dumped = 0;
  std::vector<u32> held, interp;
  for (size_t c = 1; c < caps.size(); c++) {
    const int k = at[c] - at[c - 1];
    if (k < 2) continue;
    for (int j = 1; j < k; j++) {
      const double t = double(j) / k;
      r.render(*caps[c - 1]->flash, nullptr, 1.0, scale, held);
      r.render(*caps[c]->flash, caps[c - 1]->flash.get(), t, scale, interp, c >= 2 ? caps[c - 2]->flash.get() : nullptr);
      const auto& truth = refs[size_t(at[c - 1] + j)];
      const double eh = error(truth, held), ei = error(truth, interp);
      sum_hold += eh; sum_int += ei; n++;
      std::printf("SWF %3d -> %3d, frame %3d (t %.2f): hold %5.2f  interp %5.2f%s\n", at[c - 1] + 1, at[c] + 1, at[c - 1] + j + 1, t, eh, ei,
                  ei > eh ? "  WORSE" : "");
      if (dump && dumped < 40 && ei > 0.6 * eh && eh > 0.5) {
        const int W = 160 * scale;
        std::vector<u32> out(size_t(W) * 3 * W);
        for (int y = 0; y < W; y++)
          for (int x = 0; x < W; x++) {
            out[size_t(y) * 3 * W + x] = held[size_t(y) * W + x];
            out[size_t(y) * 3 * W + W + x] = interp[size_t(y) * W + x];
            out[size_t(y) * 3 * W + 2 * W + x] = truth[size_t(y) * W + x];
          }
        char name[512];
        std::snprintf(name, sizeof(name), "%s-%03d.png", dump, at[c - 1] + j + 1);
        write_png(name, out.data(), 3 * W, W);
        dumped++;
      }
    }
  }
  if (n) std::printf("mean error (0..255) over %d skipped frames: hold %.2f, interp %.2f\n", n, sum_hold / n, sum_int / n);
}
