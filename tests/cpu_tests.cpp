// ARCompact CPU unit tests: small hand-encoded programs checked against the
// ISA semantics. Run via `ctest` or directly.

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "core/arc/cpu.h"
#include "core/bus.h"

using namespace leap;
using leap::arc::Cpu;

namespace {

int g_failed = 0, g_checks = 0;

#define CHECK_EQ(a, b)                                                                        \
  do {                                                                                        \
    g_checks++;                                                                               \
    const auto va = (a);                                                                      \
    const auto vb = (b);                                                                      \
    if (va != vb) {                                                                           \
      g_failed++;                                                                             \
      std::printf("  FAIL %s:%d: %s = 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #a,     \
                  static_cast<unsigned long long>(va), static_cast<unsigned long long>(vb)); \
    }                                                                                         \
  } while (0)

// ---- encoders ----
u32 op32(u32 major, u32 b, u32 p, u32 sub, bool f, u32 c, u32 a) {
  return (major << 27) | ((b & 7) << 24) | (p << 22) | (sub << 16) | (u32(f) << 15) |
         (((b >> 3) & 7) << 12) | ((c & 63) << 6) | (a & 63);
}
// a = b OP c
u32 alu_rr(u32 sub, u32 a, u32 b, u32 c, bool f = false) { return op32(4, b, 0, sub, f, c, a); }
// a = b OP u6
u32 alu_ru6(u32 sub, u32 a, u32 b, u32 u6, bool f = false) { return op32(4, b, 1, sub, f, u6, a); }
// b = b OP s12
u32 alu_s12(u32 sub, u32 b, u32 s12, bool f = false) {
  return op32(4, b, 2, sub, f, s12 & 63, (s12 >> 6) & 63);
}
u32 mov_u6(u32 b, u32 u6) { return op32(4, b, 1, 0x0a, false, u6, 0); }
u32 mov_limm(u32 b) { return op32(4, b, 0, 0x0a, false, 62, 0); }

// Backend under test; every test runs once per backend.
Cpu::Backend g_backend = Cpu::Backend::Interpreter;

// Program code lives in the first 64 KiB page, which is remapped as ROM before
// execution so the cached interpreter engages; data and stack are RAM above it.
struct Rig {
  Bus bus;
  std::vector<u8> ram = std::vector<u8>(1 << 20);
  Cpu cpu{bus};
  u32 at = 0;
  bool sealed = false;

  Rig() {
    bus.map_memory(0, u32(ram.size()), ram.data(), true);
    cpu.set_reset_vector(0);
    cpu.reset();
    cpu.set_backend(g_backend);
    cpu.set_reg(Cpu::kSP, 0x80000);
  }
  void h16(u16 v) { bus.write16(at, v); at += 2; }
  void w32(u32 v) { h16(u16(v >> 16)); h16(u16(v)); }  // halfwords, high first
  Rig& i32(u32 op) { w32(op); return *this; }
  Rig& i16(u16 op) { h16(op); return *this; }
  Rig& limm(u32 v) { w32(v); return *this; }
  void seal() {
    if (sealed) return;
    sealed = true;
    bus.map_memory(0, Bus::kPageSize, ram.data(), false);
    cpu.flush_decode_cache();
  }
  // run() rather than step() so the cached backend's burst loop is exercised.
  void steps(int n) { seal(); cpu.run(u64(n)); }
};

void test(const char* name, const std::function<void()>& fn) {
  const int before = g_failed;
  fn();
  std::printf("%s [%s] %s\n", g_failed == before ? "ok  " : "FAIL", Cpu::backend_name(g_backend), name);
}

}  // namespace

void run_all();
void differential();

int main() {
  for (auto b : {Cpu::Backend::Interpreter, Cpu::Backend::CachedInterpreter, Cpu::Backend::Jit}) {
    if (!Cpu::backend_available(b)) continue;  // (the JIT: x86-64 and AArch64 hosts)
    g_backend = b;
    run_all();
  }
  differential();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}

void run_all() {
  test("mov/add basic", [] {
    Rig r;
    r.i32(mov_u6(1, 5)).i32(alu_rr(0x00, 2, 1, 1));
    r.steps(2);
    CHECK_EQ(r.cpu.reg(2), 10u);
    CHECK_EQ(r.cpu.pc(), 8u);
  });

  test("long immediate", [] {
    Rig r;
    r.i32(mov_limm(3)).limm(0x12345678).i32(alu_rr(0x00, 4, 3, 62)).limm(0x11111111);
    r.steps(2);
    CHECK_EQ(r.cpu.reg(3), 0x12345678u);
    CHECK_EQ(r.cpu.reg(4), 0x23456789u);
    CHECK_EQ(r.cpu.pc(), 16u);
  });

  test("sub.f borrow and negative", [] {
    Rig r;
    r.i32(mov_u6(1, 5)).i32(mov_u6(2, 10)).i32(alu_rr(0x02, 3, 1, 2, true));
    r.steps(3);
    CHECK_EQ(r.cpu.reg(3), 0xfffffffbu);
    CHECK_EQ(r.cpu.status32() & (Cpu::kN | Cpu::kC | Cpu::kZ | Cpu::kV), Cpu::kN | Cpu::kC);
  });

  test("add.f overflow", [] {
    Rig r;
    r.i32(mov_limm(1)).limm(0x7fffffff).i32(alu_ru6(0x00, 2, 1, 1, true));
    r.steps(2);
    CHECK_EQ(r.cpu.reg(2), 0x80000000u);
    CHECK_EQ(r.cpu.status32() & (Cpu::kN | Cpu::kC | Cpu::kZ | Cpu::kV), Cpu::kN | Cpu::kV);
  });

  test("adc carry-in with 0xffffffff operand", [] {
    Rig r;
    // Set C via 0xffffffff + 1, then adc 0 + 0xffffffff + C = 0 with carry out.
    r.i32(mov_limm(1)).limm(0xffffffff);
    r.i32(alu_ru6(0x00, 2, 1, 1, true));          // r2 = 0, C=1
    r.i32(mov_u6(3, 0));
    r.i32(alu_rr(0x01, 4, 3, 1, true));           // r4 = 0 + 0xffffffff + 1
    r.steps(4);
    CHECK_EQ(r.cpu.reg(4), 0u);
    CHECK_EQ(bool(r.cpu.status32() & Cpu::kC), true);
    CHECK_EQ(bool(r.cpu.status32() & Cpu::kZ), true);
  });

  test("conditional execution (P=3)", [] {
    Rig r;
    r.i32(mov_u6(1, 1)).i32(alu_ru6(0x0c, 0, 1, 1, true));  // cmp r1,1 -> Z
    // mov.eq r2, 7  (P=3, M=1 u6, cc=1)
    r.i32(op32(4, 2, 3, 0x0a, false, 7, 0x20 | 0x01));
    // mov.ne r3, 9
    r.i32(op32(4, 3, 3, 0x0a, false, 9, 0x20 | 0x02));
    r.steps(4);
    CHECK_EQ(r.cpu.reg(2), 7u);
    CHECK_EQ(r.cpu.reg(3), 0u);
  });

  test("b.d executes delay slot", [] {
    Rig r;
    // b.d +16 (from pc 0): s21 offset in halfwords = 8, N bit (0x20) set.
    const u32 off = 8;
    r.i32((0u << 27) | ((off & 0x3ff) << 17) | (((off >> 10) & 0x3ff) << 6) | 0x20);
    r.i32(mov_u6(4, 1));   // delay slot (pc 4)
    r.i32(mov_u6(6, 1));   // skipped   (pc 8)
    r.i32(mov_u6(6, 2));   // skipped   (pc 12)
    r.i32(mov_u6(5, 2));   // target    (pc 16)
    r.steps(3);
    CHECK_EQ(r.cpu.reg(4), 1u);
    CHECK_EQ(r.cpu.reg(5), 2u);
    CHECK_EQ(r.cpu.reg(6), 0u);
    CHECK_EQ(r.cpu.pc(), 20u);
  });

  test("bl_s links to next instruction", [] {
    Rig r;
    r.i16(0xf800 | 2);     // bl_s +8 (s13 in words: 2)
    r.i16(0x78e0);         // nop_s (pc 2)
    r.i32(mov_u6(1, 1));   // pc 4
    r.i32(mov_u6(2, 3));   // pc 8: target
    r.steps(2);
    CHECK_EQ(r.cpu.reg(Cpu::kBLINK), 2u);
    CHECK_EQ(r.cpu.reg(2), 3u);
    CHECK_EQ(r.cpu.reg(1), 0u);
  });

  test("zero-overhead loop", [] {
    Rig r;
    r.i32(mov_u6(Cpu::kLP_COUNT, 3));
    // lp: P=2, s12 = offset in halfwords from pc&~3 (pc 4) to loop end (pc 12) = 4
    r.i32(alu_s12(0x28, 0, 4));
    r.i32(alu_ru6(0x00, 6, 6, 1));  // pc 8: body r6 += 1
    r.i32(mov_u6(7, 9));            // pc 12: after loop
    r.steps(2 + 3 + 1);
    CHECK_EQ(r.cpu.reg(6), 3u);
    CHECK_EQ(r.cpu.reg(7), 9u);
    CHECK_EQ(r.cpu.reg(Cpu::kLP_COUNT), 0u);
  });

  test("ld/st writeback modes", [] {
    Rig r;
    r.bus.write32(0x20000, 0xdeadbeef);
    r.bus.write32(0x20004, 0xcafef00d);
    r.i32(mov_limm(1)).limm(0x20000);
    // ld.ab r2,[r1,4]: major 2, s9=4, aa=2
    r.i32((2u << 27) | ((1 & 7) << 24) | (4u << 16) | (2u << 9) | 2);
    // ld.a r3,[r1,0] (aa=1, offset 0): r1 unchanged, load 0x1004
    r.i32((2u << 27) | ((1 & 7) << 24) | (0u << 16) | (1u << 9) | 3);
    // st.aw? use st r3,[r1,8] -> 0x100c
    r.i32((3u << 27) | ((1 & 7) << 24) | (8u << 16) | (3u << 6));
    r.steps(4);
    CHECK_EQ(r.cpu.reg(2), 0xdeadbeefu);
    CHECK_EQ(r.cpu.reg(1), 0x20004u);
    CHECK_EQ(r.cpu.reg(3), 0xcafef00du);
    CHECK_EQ(r.bus.read32(0x2000c), 0xcafef00du);
  });

  test("16-bit push/pop and add_s", [] {
    Rig r;
    r.i16(0xd800 | (1 << 8) | 0x42);  // mov_s r1,0x42
    r.i16(0xc0e1 | (1 << 8));         // push_s r1
    r.i16(0xc0c1 | (2 << 8));         // pop_s r2
    r.i16(0xe000 | (2 << 8) | 0x10);  // add_s r2,r2,0x10
    r.steps(4);
    CHECK_EQ(r.cpu.reg(2), 0x52u);
    CHECK_EQ(r.cpu.reg(Cpu::kSP), 0x80000u);
  });

  test("brne reg-u6", [] {
    Rig r;
    r.i32(mov_u6(1, 5));
    // brne r1,3 -> +8 (pc 4 -> 12): major 1, bit16=1, bit4=1 (u6), cc=1
    const u32 off = 4;  // halfwords
    r.i32((1u << 27) | ((1 & 7) << 24) | ((off & 0x7f) << 17) | (1u << 16) | (3u << 6) | 0x10 | 1);
    r.i32(mov_u6(2, 1));
    r.i32(mov_u6(3, 1));
    r.steps(3);
    CHECK_EQ(r.cpu.reg(2), 0u);
    CHECK_EQ(r.cpu.reg(3), 1u);
  });

  test("ror multiple / bmsk 31 / norm", [] {
    Rig r;
    r.i32(mov_limm(1)).limm(0x80000001);
    r.i32(op32(5, 1, 1, 0x03, false, 4, 2));  // ror r2,r1,4
    r.i32(alu_ru6(0x13, 3, 1, 31));           // bmsk r3,r1,31
    r.i32(op32(5, 4, 0, 0x2f, false, 1, 0x01));  // norm r4,r1
    r.steps(4);
    CHECK_EQ(r.cpu.reg(2), 0x18000000u);
    CHECK_EQ(r.cpu.reg(3), 0x80000001u);
    CHECK_EQ(r.cpu.reg(4), 0u);
  });

  test("level-1 interrupt entry and j.f return", [] {
    Rig r;
    r.i32(op32(4, 0, 1, 0x29, false, 2, 0));  // flag 2 (E1)
    r.i32(mov_u6(1, 1));                        // pc 4
    r.i32(mov_u6(2, 2));                        // pc 8
    // Vector 0x19 at 0xc8: j.f [ilink1]
    r.at = 0x19 * 8;
    r.i32(op32(4, 0, 0, 0x20, true, Cpu::kILINK1, 0));
    r.steps(1);
    r.cpu.raise_irq(0x19);
    r.steps(1);  // interrupt taken before pc 4, executes j.f
    CHECK_EQ(r.cpu.reg(Cpu::kILINK1), 4u);
    CHECK_EQ(r.cpu.pc(), 4u);
    CHECK_EQ(bool(r.cpu.status32() & Cpu::kE1), true);
    r.steps(2);
    CHECK_EQ(r.cpu.reg(1), 1u);
    CHECK_EQ(r.cpu.reg(2), 2u);
  });

}

// The backends against each other, on generated programs: each is a loop of
// random instructions (ALU operations with and without flags, carries,
// compares, conditional moves, shifts, 64-bit multiplies, loads and stores)
// run 20 times, then `flag 1`. Long enough that the JIT's compiled blocks run
// (the tests above are too short: the JIT hands a block that might not fit in
// the time slice to the interpreter), so its generated code is checked without
// any ROM. Every register, the flags and the data area must match the
// reference interpreter's.
namespace {
struct Rng {
  u64 s;
  u32 next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return u32(s >> 16); }
  u32 below(u32 n) { return next() % n; }
};

struct Final {
  std::vector<u32> regs;
  std::vector<u8> data;
  bool halted;
};

constexpr u32 kData = 0x20000, kDataSize = 64;

void generate(Rig& r, u64 seed) {
  Rng g{seed * 0x9e3779b97f4a7c15ull + 1};
  auto reg = [&] { return 1 + g.below(9); };  // r1-r9 (r10: the counter, r13: the data base)
  for (u32 i = 1; i <= 9; i++) r.i32(mov_limm(i)).limm(g.next());
  r.i32(mov_limm(13)).limm(kData);
  r.i32(mov_u6(10, 20));
  const u32 loop = r.at;
  static const u32 kAlu[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0e, 0x0f,
                             0x10, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
  for (int k = 0; k < 24; k++) {
    const bool f = g.below(2);
    switch (g.below(8)) {
      case 0: case 1:  // a = b op c
        r.i32(alu_rr(kAlu[g.below(std::size(kAlu))], reg(), reg(), reg(), f));
        break;
      case 2:  // a = b op u6
        r.i32(alu_ru6(kAlu[g.below(std::size(kAlu))], reg(), reg(), g.below(64), f));
        break;
      case 3:  // cmp / tst (flags only)
        r.i32(alu_rr(g.below(2) ? 0x0c : 0x0b, 0, reg(), reg(), true));
        break;
      case 4:  // mov.cc a, c (P=3)
        r.i32(op32(4, reg(), 3, 0x0a, false, reg(), 1 + g.below(15)));
        break;
      case 5:  // asl / lsr / asr / ror a,b,c; mul64 / mulu64 b,c
        if (g.below(3)) r.i32(op32(5, reg(), 0, g.below(4), f, reg(), reg()));
        else r.i32(op32(5, reg(), 0, 4 + g.below(2), false, reg(), 62));  // (a = 62: no destination)
        break;
      case 6: {  // ld a,[r13,off]
        const u32 off = 4 * g.below(kDataSize / 4);
        r.i32((2u << 27) | ((13u & 7) << 24) | (off << 16) | ((13u >> 3) << 12) | reg());
        break;
      }
      default: {  // st c,[r13,off]
        const u32 off = 4 * g.below(kDataSize / 4);
        r.i32((3u << 27) | ((13u & 7) << 24) | (off << 16) | ((13u >> 3) << 12) | (reg() << 6));
        break;
      }
    }
  }
  r.i32(alu_ru6(0x02, 10, 10, 1));  // sub r10,r10,1
  // brne r10,0,loop: s9 (bytes) from the branch's pc & ~3
  const s32 back = s32(loop) - s32(r.at & ~3u);
  const u32 s9 = u32(back) & 0x1ff;
  r.i32((1u << 27) | ((10u & 7) << 24) | (((s9 >> 1) & 0x7f) << 17) | (1u << 16) | (((s9 >> 8) & 1) << 15) |
        ((10u >> 3) << 12) | (0u << 6) | 0x10 | 1);
  r.i32(op32(4, 0, 1, 0x29, false, 1, 0));  // flag 1 (halt)
}

Final run_program(Cpu::Backend b, u64 seed) {
  g_backend = b;
  Rig r;
  generate(r, seed);
  r.seal();
  for (int i = 0; i < 100 && !r.cpu.halted(); i++) r.cpu.run(100'000);
  if (std::getenv("CPU_TESTS_DUMP") && seed == 0 && b == Cpu::Backend::Interpreter) {
    auto peek = [&](u32 a) { return r.bus.peek16(a); };
    for (u32 pc = 0; pc < r.at;) {
      unsigned len;
      std::printf("%04x: %s\n", pc, arc::disassemble(pc, peek, &len).c_str());
      pc += len;
    }
    std::printf("pc %08x r10 %u halted %d\n", r.cpu.pc(), r.cpu.reg(10), r.cpu.halted());
  }
  Final out{std::vector<u32>(64), std::vector<u8>(kDataSize), r.cpu.halted()};
  for (unsigned i = 0; i < 64; i++) out.regs[i] = i == Cpu::kPCL || i == Cpu::kLIMM ? 0 : r.cpu.reg(i);
  out.regs[Cpu::kLIMM] = r.cpu.status32();
  for (u32 i = 0; i < kDataSize; i++) out.data[i] = r.bus.read8(kData + i);
  return out;
}
}  // namespace

void differential() {
  constexpr int kPrograms = 200;
  for (auto b : {Cpu::Backend::CachedInterpreter, Cpu::Backend::Jit}) {
    if (!Cpu::backend_available(b)) continue;
    const int before = g_failed;
    for (int p = 0; p < kPrograms; p++) {
      const Final ref = run_program(Cpu::Backend::Interpreter, u64(p));
      const Final got = run_program(b, u64(p));
      CHECK_EQ(ref.halted, true);
      CHECK_EQ(got.halted, true);
      for (unsigned i = 0; i < 64; i++)
        if (got.regs[i] != ref.regs[i]) {
          std::printf("  program %d: r%u = 0x%08x, the interpreter's 0x%08x%s\n", p, i, got.regs[i], ref.regs[i],
                      i == Cpu::kLIMM ? " (STATUS32)" : "");
          g_failed++;
        }
      if (got.data != ref.data) { std::printf("  program %d: the data area differs\n", p); g_failed++; }
      g_checks++;
    }
    std::printf("%s [%s] %d generated programs match the interpreter\n", g_failed == before ? "ok  " : "FAIL",
                Cpu::backend_name(b), kPrograms);
  }
}
