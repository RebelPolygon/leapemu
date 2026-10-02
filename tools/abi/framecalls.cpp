// framecalls: for one emulated frame, attributes every RAM store (excluding
// the stack) to the innermost function executing it, via a shadow call stack
// built from call/return instructions. Prints functions by store count with
// the address range they write and their callers.
// Usage: framecalls BIOS CART STATE [ticks-to-record]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "core/arc/cpu.h"
#include "core/machine.h"
using namespace leap;
struct Fn { u64 calls = 0, stores = 0; u32 lo = ~0u, hi = 0; std::map<u32, u64> callers; std::map<u32, u64> pages; };
int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: framecalls BIOS CART STATE [ticks]\n"); return 1; }
  Machine m; std::string e;
  if (!m.load_bios(argv[1], &e) || !m.load_cart(argv[2], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  m.reset();
  if (!m.load_state_file(argv[3], &e)) { std::fprintf(stderr, "%s\n", e.c_str()); return 1; }
  auto peek = [&](u32 a) { return m.bus().peek16(a); };
  std::map<u32, Fn> fns;
  std::vector<std::pair<u32, u32>> stack;  // (function, return address)
  u32 pending_call_ret = 0; bool pending = false; int delay = 0;
  u32 cur_fn = 0;
  const u32 sp0 = m.cpu().reg(28);
  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    const u32 pc = c.pc();
    // Returns: arriving at the return address of the innermost frame.
    while (!stack.empty() && pc == stack.back().second) { stack.pop_back(); }
    if (pending && delay == 0) {  // first instruction of a callee
      pending = false;
      stack.push_back({pc, pending_call_ret});
      Fn& f = fns[pc]; f.calls++;
      f.callers[stack.size() > 1 ? stack[stack.size() - 2].first : 0]++;
      if (stack.size() > 256) stack.erase(stack.begin());
    } else if (pending) delay--;
    cur_fn = stack.empty() ? 0 : stack.back().first;
    unsigned len;
    const std::string d = arc::disassemble(pc, peek, &len);
    if (d.rfind("bl", 0) == 0 || d.rfind("jl", 0) == 0) {
      const bool ds = d.find(".d ") != std::string::npos || d.rfind("bl_s.d", 0) == 0 || d.rfind("jl_s.d", 0) == 0;
      pending = true; delay = ds ? 1 : 0;
      unsigned l2 = 0;
      pending_call_ret = pc + len + (ds ? (arc::disassemble(pc + len, peek, &l2), l2) : 0);
    }
  };
  m.bus().watch = [&](u32 a, u32, int n, bool w) {
    if (!w || (a >> 24) != 0x3c) return;
    const u32 sp = m.cpu().reg(28);
    if (a >= sp - 0x400 && a < sp0 + 0x4000) return;  // stack
    Fn& f = fns[cur_fn]; f.stores++; f.lo = std::min(f.lo, a); f.hi = std::max(f.hi, a + n - 1); f.pages[a >> 12]++;
  };
  for (int i = 0, n = argc > 4 ? std::atoi(argv[4]) : 1; i < n; i++) m.run_frame();
  if (std::getenv("ALLFNS")) {  // every function called, with callers
    for (auto& [pc, f] : fns) {
      if (!f.calls) continue;
      std::printf("%08x calls %6llu stores %7llu callers:", pc, (unsigned long long)f.calls, (unsigned long long)f.stores);
      for (auto& [c, n] : f.callers) std::printf(" %08x(%llu)", c, (unsigned long long)n);
      std::printf("\n");
    }
    return 0;
  }
  if (const char* want = std::getenv("FNS")) {  // callers of specific functions
    for (const char* p = want; *p;) {
      const u32 fpc = u32(std::strtoul(p, const_cast<char**>(&p), 16));
      if (*p == ',') p++;
      auto it = fns.find(fpc);
      if (it == fns.end()) { std::printf("%08x: never called\n", fpc); continue; }
      std::printf("%08x calls %llu stores %llu callers:", fpc, (unsigned long long)it->second.calls, (unsigned long long)it->second.stores);
      for (auto& [c, n] : it->second.callers) std::printf(" %08x(%llu)", c, (unsigned long long)n);
      std::printf("\n");
    }
    return 0;
  }
  std::vector<std::pair<u64, u32>> v;
  for (auto& [pc, f] : fns) if (f.stores) v.push_back({f.stores, pc});
  std::sort(v.rbegin(), v.rend());
  for (size_t i = 0; i < v.size() && i < 30; i++) {
    const Fn& f = fns[v[i].second];
    std::printf("%08x calls %5llu stores %7llu  range %08x-%08x  callers:", v[i].second, (unsigned long long)f.calls,
                (unsigned long long)f.stores, f.lo, f.hi);
    std::vector<std::pair<u64, u32>> cs; for (auto& [c, n] : f.callers) cs.push_back({n, c});
    std::sort(cs.rbegin(), cs.rend());
    for (size_t j = 0; j < cs.size() && j < 3; j++) std::printf(" %08x(%llu)", cs[j].second, (unsigned long long)cs[j].first);
    std::printf("\n");
  }
}
