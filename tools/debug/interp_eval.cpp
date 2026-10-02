// interp_eval: measures Flash frame interpolation against ground truth. Plays
// a SWF movie's timeline from the cartridge (flash::Timeline), then for each
// frame N renders the interpolation between frames N-1 and N+1 at t = 0.5 and
// compares it with the real frame N, with straight-line and arc motion
// (swf::Matrix::arc). Holding frame N-1 is the baseline.
// Usage: interp_eval BIOS CART SWF_ADDR [frames] [scale] [dump_prefix]
#include <cmath>
#include <cstdio>
#include <map>
#include <cstdlib>
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

// Joint drift: sibling shapes that meet at a joint in frames a and b (a point
// of one that keeps its place relative to the other, inside both shapes)
// should stay together in between. Returns the mean separation in screen
// pixels at t = 0.5 over such pairs, and counts them.
static double joint_drift(const flash::Frame& fa, const flash::Frame& fb, const flash::RomView& rom, flash::Renderer& r, int& pairs) {
  std::vector<u32> img;
  r.render(fb, &fa, 0.5, 1, img);
  const auto& lt = r.interpolated_locals();
  auto paths = [](const flash::Frame& f) {
    std::vector<std::string> p(f.objects.size());
    std::map<std::string, size_t> by;
    for (size_t i = 0; i < f.objects.size(); i++) {
      const auto& o = f.objects[i];
      p[i] = (o.parent >= 0 ? p[size_t(o.parent)] + "/" : "") + std::to_string(o.depth);
      by[p[i]] = i;
    }
    return std::make_pair(p, by);
  };
  const auto [pb, byb] = paths(fb);
  const auto [pa, bya] = paths(fa);
  static std::map<u32, swf::Shape> shapes;
  auto bounds = [&](const flash::Object& o) -> const swf::Shape* {
    auto it = shapes.find(o.def);
    if (it == shapes.end()) {
      swf::Shape s;
      const int version = o.tag == 2 ? 1 : o.tag == 22 ? 2 : o.tag == 32 ? 3 : 0;
      if (!version || !rom.ptr(o.def) || !swf::parse_shape_with_style(rom.ptr(o.def), rom.avail(o.def), version, s)) s.x1 = s.x0 - 1;
      else s.compute_bounds();
      it = shapes.emplace(o.def, std::move(s)).first;
    }
    return it->second.x1 > it->second.x0 ? &it->second : nullptr;
  };
  // Gliding shapes (same definition in the same slot), grouped by parent.
  struct G { const flash::Object* a; const flash::Object* b; const swf::Shape* s; size_t i; };
  std::map<s32, std::vector<G>> groups;
  for (size_t i = 0; i < fb.objects.size(); i++) {
    const auto& o = fb.objects[i];
    if (o.type != flash::Frame::kTypeShape || !o.visible) continue;
    const auto it = bya.find(pb[i]);
    if (it == bya.end() || fa.objects[it->second].def != o.def) continue;
    if (const swf::Shape* s = bounds(o)) groups[o.parent].push_back({&fa.objects[it->second], &o, s, i});
  }
  double sum = 0;
  pairs = 0;
  for (const auto& [parent, g] : groups) {
    // Screen pixels per parent unit (frame b's world scale).
    swf::Matrix w = fb.view;
    std::vector<s32> chain;
    for (s32 p = parent; p >= 0; p = fb.objects[size_t(p)].parent) chain.push_back(p);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) w = w * fb.objects[size_t(*it)].local;
    const double px = std::sqrt(std::abs(w.a * w.d - w.b * w.c));
    for (size_t i = 0; i < g.size(); i++)
      for (size_t j = 0; j < g.size(); j++) {
        if (i == j) continue;
        // j relative to i in each frame; the joint is the fixed point of
        // the change of that relation (in i's space).
        const swf::Matrix r1 = g[i].a->local.inverse() * g[j].a->local, r0 = g[i].b->local.inverse() * g[j].b->local;
        const swf::Matrix A = r1 * r0.inverse();
        const double l00 = 1 - A.a, l01 = -A.c, l10 = -A.b, l11 = 1 - A.d, det = l00 * l11 - l01 * l10;
        if (std::abs(det) < 1e-4) continue;  // rigid or sliding: no joint
        const double qx = (l11 * A.tx - l01 * A.ty) / det, qy = (-l10 * A.tx + l00 * A.ty) / det;  // in i's space
        const swf::Shape& si = *g[i].s;
        if (qx < si.x0 || qx > si.x1 || qy < si.y0 || qy > si.y1) continue;
        double jx, jy;  // the same point in j's space (frame a)
        r0.inverse().apply(qx, qy, jx, jy);
        const swf::Shape& sj = *g[j].s;
        if (jx < sj.x0 || jx > sj.x1 || jy < sj.y0 || jy > sj.y1) continue;
        const swf::Matrix& mi = lt[g[i].i];
        const swf::Matrix& mj = lt[g[j].i];
        double x1, y1, x2, y2;
        mi.apply(qx, qy, x1, y1);
        mj.apply(jx, jy, x2, y2);
        sum += std::hypot(x1 - x2, y1 - y2) * px;
        pairs++;
      }
  }
  return pairs ? sum / pairs : 0;
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: interp_eval BIOS CART SWF_ADDR [frames] [scale] [dump_prefix]\n"); return 2; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
  const u32 addr = u32(std::strtoul(argv[3], nullptr, 16));
  const int frames = argc > 4 ? std::atoi(argv[4]) : 60, scale = argc > 5 ? std::atoi(argv[5]) : 2;
  const flash::RomView rom = m.rom_view();
  swf::Movie movie;
  if (!movie.load(rom.ptr(addr), rom.avail(addr))) { std::fprintf(stderr, "no SWF at %08x\n", addr); return 2; }
  flash::Timeline tl(movie, addr);
  std::vector<flash::Frame> f;
  for (int i = 0; i < frames && i < tl.frame_count(); i++) { tl.step(); f.push_back(tl.snapshot()); }
  flash::Renderer r;
  r.set_rom(rom);
  std::vector<u32> truth, held, interp[2];
  double sum_hold = 0, sum_int[2] = {0, 0};
  int n = 0, dumped = 0;
  for (size_t i = 1; i + 1 < f.size(); i++) {
    r.render(f[i], nullptr, 1.0, scale, truth);
    r.render(f[i - 1], nullptr, 1.0, scale, held);
    for (int k = 0; k < 2; k++) {  // straight-line motion, arc motion
      swf::Matrix::arc = k == 1;
      r.render(f[i + 1], &f[i - 1], 0.5, scale, interp[k]);
    }
    const double eh = error(truth, held), e0 = error(truth, interp[0]), e1 = error(truth, interp[1]);
    sum_hold += eh; sum_int[0] += e0; sum_int[1] += e1; n++;
    std::printf("frame %3zu: hold %5.2f  line %5.2f  arc %5.2f\n", i + 1, eh, e0, e1);
    if (argc > 6 && dumped < 12 && (e0 > eh * 0.8 || e1 > eh * 0.8) && eh > 0.5) {
      const int W = 160 * scale;
      std::vector<u32> out(size_t(W) * 3 * W);
      for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
          out[size_t(y) * 3 * W + x] = truth[size_t(y) * W + x];
          out[size_t(y) * 3 * W + W + x] = interp[0][size_t(y) * W + x];
          out[size_t(y) * 3 * W + 2 * W + x] = interp[1][size_t(y) * W + x];
        }
      char name[512];
      std::snprintf(name, sizeof(name), "%s-%03zu.png", argv[6], i + 1);
      write_png(name, out.data(), 3 * W, W);
      dumped++;
    }
  }
  // Joint drift between consecutive frames.
  // (line: straight-line motion; arc: arc motion; skeleton: arc motion with
  // parts carried by their joints, the default.)
  double drift[3] = {0, 0, 0};
  int joints = 0;
  for (size_t i = 1; i < f.size(); i++)
    for (int k = 0; k < 3; k++) {
      swf::Matrix::arc = k >= 1;
      r.skeleton = k == 2;
      int pairs = 0;
      const double d = joint_drift(f[i - 1], f[i], rom, r, pairs);
      drift[k] += d * pairs;
      if (!k) joints += pairs;
    }
  swf::Matrix::arc = true;
  r.skeleton = true;
  if (joints)
    std::printf("joint drift at t=0.5 (screen px) over %d joints: line %.3f, arc %.3f, skeleton %.3f\n", joints, drift[0] / joints,
                drift[1] / joints, drift[2] / joints);
  if (n) std::printf("mean error (0..255): hold %.2f, line %.2f, arc %.2f over %d frames\n", sum_hold / n, sum_int[0] / n, sum_int[1] / n, n);
}
