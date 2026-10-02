// jit_diff: runs the JIT and the cached interpreter side by side from one
// state and reports the first place they disagree. Both machines advance in
// steps of `step` cycles (with the same buttons held) and are compared after
// each (CPU and whole machine state). On a difference, it bisects for the
// shortest run from the step's start that differs, then prints:
//   - both CPUs after it;
//   - the first compiled block that started at a place or time the
//     interpreter did not (blocks are recompiled to report their starts);
//   - the code there.
// COLD=1 recompiles everything before each bisection probe (to tell stale
// compiled code or links from translation errors).
// Usage: jit_diff BIOS CART STATE CYCLES [step] [buttons-hex]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/arc/cpu.h"
#include "core/machine.h"
using namespace leap;

static bool same(arc::Cpu& a, arc::Cpu& b) {
  if (a.pc() != b.pc() || a.cycles() != b.cycles() || a.status32() != b.status32()) return false;
  for (unsigned i = 0; i < 61; i++)
    if (a.reg(i) != b.reg(i)) return false;
  return a.lp_start() == b.lp_start() && a.lp_end() == b.lp_end() && a.in_delay_slot() == b.in_delay_slot();
}

static void show(const char* name, arc::Cpu& c) {
  std::printf("%s: pc %08x cycles %llu+%u/8 status %08x lp %08x-%08x delay %d\n", name, c.pc(),
              static_cast<unsigned long long>(c.cycles()), c.cycle_eighths(), c.status32(), c.lp_start(), c.lp_end(), c.in_delay_slot());
  for (unsigned i = 0; i < 61; i++) std::printf("%s%08x%s", i % 8 == 0 ? "  " : "", c.reg(i), i % 8 == 7 ? "\n" : " ");
  std::printf("\n");
}

int main(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: jit_diff BIOS CART STATE CYCLES [step] [buttons-hex]\n"); return 2; }
  Machine ref, jit;
  std::string e;
  for (Machine* m : {&ref, &jit}) {
    if (!m->load_bios(argv[1], &e) || !m->load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
    m->reset();
    if (!m->load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 2; }
    m->set_buttons(argc > 6 ? u32(std::strtoul(argv[6], nullptr, 16)) : 0);
  }
  ref.cpu().set_backend(arc::Cpu::Backend::CachedInterpreter);
  jit.cpu().set_backend(arc::Cpu::Backend::Jit);
  const unsigned long long total = std::strtoull(argv[4], nullptr, 10);
  const u64 step = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 10000;
  for (unsigned long long done = 0; done < total; done += step) {
    const std::vector<u8> sr = ref.save_state(), sj = jit.save_state();
    ref.run_cycles(step);
    jit.run_cycles(step);
    if (same(ref.cpu(), jit.cpu()) && ref.save_state() == jit.save_state()) continue;
    std::printf("difference within cycles %llu..%llu; narrowing\n", done, done + step);
    // Bisect: the fewest cycles from the step's start after which they differ.
    const bool cold = std::getenv("COLD") != nullptr;  // recompile for every probe
    auto differs_after = [&](u64 k) {
      ref.load_state(sr, &e);
      jit.load_state(sj, &e);
      if (cold) jit.cpu().flush_decode_cache();
      ref.run_cycles(k);
      jit.run_cycles(k);
      return !same(ref.cpu(), jit.cpu()) || ref.save_state() != jit.save_state();
    };
    u64 lo = 0, hi = step;  // same after lo, different after hi
    while (hi - lo > 1) {
      const u64 mid = lo + (hi - lo) / 2;
      if (differs_after(mid)) hi = mid; else lo = mid;
    }
    differs_after(hi);
    {
      const std::vector<u8> a = ref.save_state(), b = jit.save_state();
      std::printf("after %llu cycles in one run: state sizes %zu / %zu\n", static_cast<unsigned long long>(hi), a.size(), b.size());
      show("interpreter", ref.cpu());
      show("jit", jit.cpu());
      for (size_t i = 0; i < std::min(a.size(), b.size()); i++)
        if (a[i] != b[i]) {
          std::printf("first differing state byte at %zu:", i);
          for (size_t k = i >= 8 ? i - 8 : 0; k < std::min(a.size(), i + 8); k++) std::printf(" %02x/%02x", a[k], b[k]);
          std::printf("\n");
          break;
        }
    }
    // Which block went wrong: every block start the JIT made (pc, time)
    // must be an instruction start of the interpreter's at that time.
    {
      struct Step { u32 pc; u64 t8; };
      std::vector<Step> steps, starts;
      ref.load_state(sr, &e);
      jit.load_state(sj, &e);
      ref.cpu().trace_hook = [&](const arc::Cpu& c) { steps.push_back({c.pc(), c.cycles() * 8 + c.cycle_eighths()}); };
      jit.cpu().jit_block_hook = [&](u32 s, u64 t8) { starts.push_back({s, t8}); };
      jit.cpu().flush_decode_cache();  // (recompile with the report)
      ref.run_cycles(hi);
      jit.run_cycles(hi);
      ref.cpu().trace_hook = nullptr;
      jit.cpu().jit_block_hook = nullptr;
      jit.cpu().flush_decode_cache();
      std::printf("%zu interpreted instructions, %zu block starts\n", steps.size(), starts.size());
      size_t k = 0;
      for (size_t n = 0; n < starts.size(); n++) {
        const Step& b = starts[n];
        while (k < steps.size() && steps[k].t8 < b.t8) k++;
        if (k < steps.size() && steps[k].t8 == b.t8 && steps[k].pc == b.pc) continue;
        std::printf("block %08x starts at time %llu; the interpreter:\n", b.pc, static_cast<unsigned long long>(b.t8));
        for (size_t q = k >= 12 ? k - 12 : 0; q < std::min(steps.size(), k + 3); q++)
          std::printf("  %08x at %llu%s\n", steps[q].pc, static_cast<unsigned long long>(steps[q].t8), q == k ? "  <-" : "");
        if (n) std::printf("previous block start: %08x at %llu\n", starts[n - 1].pc, static_cast<unsigned long long>(starts[n - 1].t8));
        break;
      }
    }
    differs_after(lo);
    const u32 pr = ref.cpu().pc();
    show("before", ref.cpu());
    const std::vector<u8> r1 = ref.save_state();
    ref.run_cycles(hi - lo);
    jit.run_cycles(hi - lo);
    std::printf("after %llu more cycles from pc %08x:\n", static_cast<unsigned long long>(hi - lo), pr);
    show("interpreter", ref.cpu());
    show("jit", jit.cpu());
    if (same(ref.cpu(), jit.cpu())) {
      std::printf("(registers agree: memory or device state differs)\n");
      const std::vector<u8> a = ref.save_state(), b = jit.save_state();
      std::printf("state sizes %zu / %zu\n", a.size(), b.size());
      for (size_t i = 0; i < std::min(a.size(), b.size()); i++)
        if (a[i] != b[i]) {
          std::printf("first differing state byte at %zu:", i);
          for (size_t k = i >= 8 ? i - 8 : 0; k < std::min(a.size(), i + 8); k++) std::printf(" %02x/%02x", a[k], b[k]);
          std::printf("\n");
          break;
        }
    }
    std::printf("code:\n");
    u32 a = pr;
    for (int i = 0; i < 24; i++) {
      unsigned len = 0;
      const std::string s = arc::disassemble(a, [&](u32 x) { return ref.bus().peek16(x); }, &len);
      std::printf("  %08x  %s\n", a, s.c_str());
      a += len ? len : 2;
    }
    (void)r1;
    return 1;
  }
  std::printf("no difference over %llu cycles\n", total);
  return 0;
}
