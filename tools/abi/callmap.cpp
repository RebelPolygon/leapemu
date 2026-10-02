// callmap: black-box map of control transfers between cartridge code and the
// BaseROM, for the clean-room specification (docs/open-bios/CLEANROOM.md).
//
// It observes register state at region crossings only; it never records or
// prints BaseROM code. Output: one row per distinct BaseROM entry point that
// cartridge code calls (with call-site style, argument samples and return
// values), and one row per cartridge entry point the BaseROM calls.
//
// Usage: callmap BIOS CART NVRAM_DIR SECONDS [--touch X,Y@F ...]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/arc/cpu.h"
#include "core/machine.h"

using namespace leap;

namespace {

enum Region { kBios, kCart, kRam, kOther };
Region region(u32 a) {
  if (a < 0x0080'0000 || (a >= 0x4000'0000 && a < 0x4080'0000)) return kBios;
  if (a >= 0x8000'0000 && a < 0xc000'0000) return kCart;
  if (a >= 0x3c00'0000 && a < 0x4000'0000) return kRam;
  return kOther;
}

struct Sample {
  u32 args[4];
};

struct Entry {
  u64 calls = 0;
  std::set<u32> callers;               // call-site pcs on the other side
  std::map<std::string, u64> styles;   // call instruction mnemonic class
  std::vector<Sample> samples;         // first few argument sets
  std::map<u32, u64> returns;          // r0 at return (bounded)
  u64 unreturned = 0;
};

struct Pending {
  u32 target;
  u32 ret_addr;
  bool from_cart;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: callmap BIOS CART NVRAM_DIR SECONDS\n");
    return 2;
  }
  Machine m;
  std::string err;
  if (!m.load_bios(argv[1], &err) || !m.load_cart(argv[2], &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
  m.reset();
  m.load_nvram(argv[3]);
  const int frames = int(std::atof(argv[4]) * 60);

  std::map<u32, Entry> to_bios, to_cart;
  std::vector<Pending> stack;
  u32 prev_pc = m.cpu().pc();
  u64 irqs = 0, irq_returns = 0, jumps = 0;
  u32 first_cart_pc = 0;
  u32 first_cart_regs[8] = {};
  auto fetch = [&](u32 a) { return m.bus().peek16(a); };

  // Function-pointer provenance: cartridge reads of values that point into
  // BaseROM code, and who stored such values in RAM.
  struct Slot { u32 value = 0; u64 reads = 0; std::set<u32> readers; };
  struct Store { u32 value = 0; Region writer = kOther; u64 cycle = 0; u32 pc = 0; };
  std::map<u32, Slot> ptr_reads;
  std::map<u32, Store> ptr_stores;
  u32 cur_pc = 0;
  auto is_bios_code = [](u32 v) { return v >= 0x4000'0000 && v < 0x4080'0000; };
  m.bus().watch = [&](u32 a, u32 v, int size, bool write) {
    if (size != 4 || !is_bios_code(v)) return;
    if (write) {
      if (region(a) == kRam && !ptr_stores.count(a)) ptr_stores[a] = {v, region(cur_pc), m.cpu().cycles(), cur_pc};
    } else if (region(cur_pc) == kCart) {
      Slot& sl = ptr_reads[a];
      sl.value = v;
      sl.reads++;
      if (sl.readers.size() < 8) sl.readers.insert(cur_pc);
    }
  };

  m.cpu().trace_hook = [&](const arc::Cpu& c) {
    const u32 pc = c.pc();
    cur_pc = pc;
    const Region from = region(prev_pc), to = region(pc);
    if (from != to && (from == kCart || to == kCart)) {
      const u32 vb = c.int_vector_base();
      if (pc >= vb && pc < vb + 0x100 && (pc - vb) % 8 == 0) {
        irqs++;
      } else if (pc == c.reg(arc::Cpu::kILINK1) || pc == c.reg(arc::Cpu::kILINK2)) {
        irq_returns++;
      } else if (!stack.empty() && pc == stack.back().ret_addr) {
        // Return from a crossing call.
        Pending p = stack.back();
        stack.pop_back();
        auto& e = (p.from_cart ? to_bios : to_cart)[p.target];
        if (e.returns.size() < 16 || e.returns.count(c.reg(0))) e.returns[c.reg(0)]++;
      } else {
        const u32 blink = c.reg(arc::Cpu::kBLINK);
        const bool linked = blink > prev_pc && blink <= prev_pc + 12;
        auto& e = (to == kBios ? to_bios : to_cart)[pc];
        e.calls++;
        e.callers.insert(prev_pc);
        unsigned len;
        std::string dis = arc::disassemble(prev_pc, fetch, &len);
        std::string mn = dis.substr(0, dis.find(' '));
        std::string style = mn + (dis.find('[') != std::string::npos ? " [reg]" : "");
        e.styles[linked ? style : style + " (no link)"]++;
        if (e.samples.size() < 3) e.samples.push_back({{c.reg(0), c.reg(1), c.reg(2), c.reg(3)}});
        if (linked) stack.push_back({pc, blink, to == kBios});
        else jumps++;
        if (to == kCart && !first_cart_pc) {
          first_cart_pc = pc;
          for (int i = 0; i < 4; i++) first_cart_regs[i] = c.reg(i);
          first_cart_regs[4] = c.reg(arc::Cpu::kSP);
          first_cart_regs[5] = c.reg(arc::Cpu::kGP);
          first_cart_regs[6] = c.reg(arc::Cpu::kBLINK);
          first_cart_regs[7] = c.status32();
        }
        if (stack.size() > 256) stack.erase(stack.begin());
      }
    }
    prev_pc = pc;
  };

  for (int f = 0; f < frames; f++) {
    if (!m.run_frame()) { std::fprintf(stderr, "stopped: %s\n", m.cpu().stop_message().c_str()); break; }
  }

  const RomHeader& h = m.cart_header();
  std::printf("# Call map: %s (%s)\n\n", h.title.c_str(), h.part_number.c_str());
  std::printf("Observed %.0f s of emulation. Interrupt entries from cart code: %llu, returns: %llu. "
              "Unlinked cross-region jumps: %llu.\n\n",
              std::atof(argv[4]), (unsigned long long)irqs, (unsigned long long)irq_returns, (unsigned long long)jumps);
  std::printf("First entry into cartridge code: %08x with r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x gp=%08x "
              "blink=%08x status32=%08x\n\n",
              first_cart_pc, first_cart_regs[0], first_cart_regs[1], first_cart_regs[2], first_cart_regs[3],
              first_cart_regs[4], first_cart_regs[5], first_cart_regs[6], first_cart_regs[7]);

  auto dump = [&](const char* title, std::map<u32, Entry>& map) {
    std::printf("## %s (%zu distinct targets)\n\n", title, map.size());
    std::printf("| target | calls | call sites | call style | sample args r0..r3 | return r0 (count) |\n|---|---|---|---|---|---|\n");
    for (auto& [t, e] : map) {
      std::string styles, samples, rets;
      for (auto& [s, n] : e.styles) styles += s + "×" + std::to_string(n) + " ";
      for (auto& s : e.samples) {
        char b[64];
        std::snprintf(b, sizeof(b), "(%x,%x,%x,%x) ", s.args[0], s.args[1], s.args[2], s.args[3]);
        samples += b;
      }
      int k = 0;
      for (auto& [r, n] : e.returns) {
        if (k++ == 6) { rets += "…"; break; }
        char b[32];
        std::snprintf(b, sizeof(b), "%x(%llu) ", r, (unsigned long long)n);
        rets += b;
      }
      std::printf("| %08x | %llu | %zu | %s| %s| %s|\n", t, (unsigned long long)e.calls, e.callers.size(),
                  styles.c_str(), samples.c_str(), rets.c_str());
    }
    std::printf("\n");
  };
  dump("Cartridge → BaseROM", to_bios);
  dump("BaseROM → cartridge", to_cart);

  // Registry annotation: the structure at [gp+4] (gp = 0x3c000100) holds
  // u16 version, u16 count, then `count` entries of {u32, table A, table B};
  // each table is an array of function pointers.
  struct Loc { unsigned entry; char table; unsigned slot; };
  std::map<u32, std::vector<Loc>> where;
  auto& bus = m.bus();
  const u32 sys = bus.peek32(0x3c000104);
  const unsigned count = bus.peek16(sys + 2);
  std::printf("## Interface registry\n\nSystem structure at [gp+4] = %08x: version %04x, %u entries.\n\n",
              sys, bus.peek16(sys), count);
  std::printf("| entry | table A | table B | owner |\n|---|---|---|---|\n");
  auto is_code = [](u32 v) { return (v >= 0x4000'0000 && v < 0x4080'0000) || (v >= 0x8000'0000 && v < 0x8100'0000); };
  for (unsigned i = 0; i < count; i++) {
    const u32 e = sys + 8 + i * 12;
    const u32 ta = bus.peek32(e + 4), tb = bus.peek32(e + 8);
    const char* owner = (ta >> 28) == 4 ? "system" : (ta >> 28) == 8 ? "cartridge" : "-";
    std::printf("| %u | %08x | %08x | %s |\n", i, ta, tb, owner);
    for (int t = 0; t < 2; t++) {
      const u32 base = t ? tb : ta;
      if (!is_code(base)) continue;
      for (unsigned k = 0; k < 256; k++) {
        const u32 fn = bus.peek32(base + k * 4);
        if (!is_code(fn)) break;
        where[fn].push_back({i, char('A' + t), k});
      }
    }
  }
  std::printf("\n## Calls by registry slot\n\nEach observed cross-region call target located in the registry "
              "(entry.table[slot]).\n\n| slot | direction | target | calls | return r0 samples |\n|---|---|---|---|---|\n");
  auto annotate = [&](const char* dir, std::map<u32, Entry>& mp) {
    for (auto& [t, e] : mp) {
      auto it = where.find(t);
      if (it == where.end()) continue;
      std::string locs;
      for (auto& l : it->second) locs += std::to_string(l.entry) + "." + l.table + "[" + std::to_string(l.slot) + "] ";
      std::string rets;
      int k = 0;
      for (auto& [r, n] : e.returns) { if (k++ == 4) { rets += "…"; break; } char b[24]; std::snprintf(b, sizeof(b), "%x ", r); rets += b; }
      std::printf("| %s| %s | %08x | %llu | %s|\n", locs.c_str(), dir, t, (unsigned long long)e.calls, rets.c_str());
    }
  };
  annotate("cart→system", to_bios);
  annotate("system→cart", to_cart);
  std::printf("\n");

  // Where did the call targets come from?
  std::printf("## Function-pointer sources read by cartridge code\n\n");
  std::printf("Slots whose value is a BaseROM code address, read by cartridge code. "
              "`called` = the value is a cartridge→BaseROM call target above. `stored by` = who wrote the "
              "slot (RAM only) and when.\n\n");
  std::printf("| slot | region | value | called | reads | stored by | store cycle |\n|---|---|---|---|---|---|---|\n");
  const char* rn[] = {"bios", "cart", "ram", "other"};
  for (auto& [a, sl] : ptr_reads) {
    const bool called = to_bios.count(sl.value) != 0;
    std::string by = "-", when = "-";
    if (auto it = ptr_stores.find(a); it != ptr_stores.end()) {
      char b[48];
      std::snprintf(b, sizeof(b), "%s @%08x", rn[it->second.writer], it->second.pc);
      by = b;
      when = std::to_string(it->second.cycle);
    }
    std::printf("| %08x | %s | %08x | %s | %llu | %s | %s |\n", a, rn[region(a)], sl.value, called ? "yes" : "",
                (unsigned long long)sl.reads, by.c_str(), when.c_str());
  }
  return 0;
}
