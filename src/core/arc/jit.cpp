#include "core/arc/jit.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>

#include "core/arc/cpu.h"
#include "core/arc/jit_internal.h"
#include "core/bus.h"
#include "core/log.h"

#ifdef LEAP_JIT
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__APPLE__) && defined(LEAP_JIT_A64)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif
#endif

namespace leap::arc {

#ifndef LEAP_JIT

bool Jit::supported() { return false; }
Jit::Jit(Cpu& cpu, Bus& bus) : cpu_(cpu), bus_(bus) {}
Jit::~Jit() = default;
Jit::Block* Jit::block(u32, u32) { return nullptr; }
void Jit::link(const Block*) {}
void Jit::flush() {}
Jit::Block* Jit::compile(u32, u32) { return nullptr; }
void Jit::emit_trampoline() {}
u8* Jit::reserve(size_t) { return nullptr; }
void Jit::write(u8*, const u8*, size_t) {}

#else

using namespace jit_detail;

namespace {

constexpr size_t kArenaSize = 64u << 20;

}  // namespace

bool Jit::supported() { return true; }

Jit::Jit(Cpu& cpu, Bus& bus) : cpu_(cpu), bus_(bus), recent_(kRecent) {
#ifdef _WIN32
  arena_ = static_cast<u8*>(VirtualAlloc(nullptr, kArenaSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
#elif defined(__APPLE__) && defined(LEAP_JIT_A64)
  // Apple silicon: executable memory is MAP_JIT, writable by one thread at a
  // time (pthread_jit_write_protect_np, in write()).
  void* p = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
  arena_ = p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
#else
  void* p = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {  // W^X systems: write, then make executable
    rwx_ = false;
    p = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  arena_ = p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
#endif
  if (arena_) arena_size_ = kArenaSize;
  else LOG_E("JIT: could not allocate executable memory");
  if (const char* v = std::getenv("LEAPEMU_JIT_NOCHAIN")) chain_ = std::atoi(v) == 0;
  flush();
}

Jit::~Jit() {
  if (std::getenv("LEAPEMU_JIT_STATS"))
    std::fprintf(stderr, "jit: %llu block runs, %llu interpreted, guards %llu slice / %llu loop, %llu compiles, %llu flushes, %zu blocks, %zu KiB\n",
                 static_cast<unsigned long long>(stats.runs), static_cast<unsigned long long>(stats.interpreted),
                 static_cast<unsigned long long>(stats.guard_slice), static_cast<unsigned long long>(stats.guard_loop),
                 static_cast<unsigned long long>(stats.compiles), static_cast<unsigned long long>(stats.flushes), blocks_.size(), used_ / 1024);
  if (std::getenv("LEAPEMU_JIT_STATS")) {
    std::fprintf(stderr, "jit: interpreted: %llu delay slots, %llu outside compiled code\n", static_cast<unsigned long long>(stats.delay_slots),
                 static_cast<unsigned long long>(stats.no_block));
    std::vector<std::pair<u64, std::string>> g;
    for (const auto& [k, v] : stats.generic) g.push_back({v, k});
    std::sort(g.rbegin(), g.rend());
    for (size_t i = 0; i < g.size() && i < 25; i++) std::fprintf(stderr, "  %10llu  %s\n", static_cast<unsigned long long>(g[i].first), g[i].second.c_str());
  }
  if (!arena_) return;
#ifdef _WIN32
  VirtualFree(arena_, 0, MEM_RELEASE);
#else
  munmap(arena_, arena_size_);
#endif
}

void Jit::flush() {
  stats.flushes++;
  blocks_.clear();
  variants_.clear();
  std::fill(recent_.begin(), recent_.end(), Recent{});
  link_slot_ = nullptr;
  used_ = 0;
  pages_ = bus_.pages_.data();
  max_wait8_ = 0;
  for (const auto& p : bus_.pages_) max_wait8_ = std::max<u32>(max_wait8_, std::max(p.wait16, p.wait32));
  emit_trampoline();  // (always at the start of the arena, so its address never changes)
}

u8* Jit::reserve(size_t n) {
  if (!arena_ || n + 4096 > arena_size_) return nullptr;
  if (used_ + n > arena_size_) flush();
  u8* at = arena_ + used_;
  used_ += (n + 15) & ~size_t(15);
  return at;
}

void Jit::write(u8* at, const u8* data, size_t n) {
#if defined(__APPLE__) && defined(LEAP_JIT_A64)
  pthread_jit_write_protect_np(0);
  std::memcpy(at, data, n);
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(at, n);
  return;
#endif
#ifndef _WIN32
  if (!rwx_) {
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    u8* lo = reinterpret_cast<u8*>(reinterpret_cast<uintptr_t>(at) & ~(page - 1));
    const size_t len = size_t(at + n - lo);
    mprotect(lo, len, PROT_READ | PROT_WRITE);
    std::memcpy(at, data, n);
    mprotect(lo, len, PROT_READ | PROT_EXEC);
#ifdef LEAP_JIT_A64
    __builtin___clear_cache(reinterpret_cast<char*>(at), reinterpret_cast<char*>(at + n));
#endif
    return;
  }
#endif
  std::memcpy(at, data, n);
#ifdef LEAP_JIT_A64  // (instruction caches don't see data writes on ARM)
#ifdef _WIN32
  FlushInstructionCache(GetCurrentProcess(), at, n);
#else
  __builtin___clear_cache(reinterpret_cast<char*>(at), reinterpret_cast<char*>(at + n));
#endif
#endif
}

void Jit::link(const Block* to) {
  u8* slot = link_slot_;
  link_slot_ = nullptr;
  if (!slot || !to || to->variant || idle_pcs_.count(to->start)) return;
  const u64 target = u64(reinterpret_cast<uintptr_t>(to->code));
  write(slot, reinterpret_cast<const u8*>(&target), 8);
}

Jit::Block* Jit::block(u32 pc, u32 stop) {
  // The idle-loop hint moves between the places that poll the power control
  // register. Blocks end before every place it has been, so only a new one
  // needs the code recompiled.
  if (cpu_.idle_hint != last_idle_hint_ || bus_.pages_.data() != pages_) {
    last_idle_hint_ = cpu_.idle_hint;
    if (!idle_pcs_.count(cpu_.idle_hint) || bus_.pages_.data() != pages_) {
      idle_pcs_.insert(cpu_.idle_hint);
      flush();
    }
  }
  if (stop != ~0u) {
    const auto it = variants_.find(u64(stop) << 32 | pc);
    return it != variants_.end() ? it->second.get() : compile(pc, stop);
  }
  Recent& r = recent_[(pc >> 1) & (kRecent - 1)];
  if (r.pc == pc) return r.block;
  Block* b;
  const auto it = blocks_.find(pc);
  if (it != blocks_.end()) b = it->second.get();
  else b = compile(pc, ~0u);  // (stores the result, nullptr included)
  r = {pc, b};
  return b;
}

// ---------------------------------------------------------------------------
// Block formation (the code generators: jit_x64.cpp, jit_a64.cpp)
// ---------------------------------------------------------------------------

Jit::Block* Jit::compile(u32 start, u32 stop) {
  Cpu& c = cpu_;
  stats.compiles++;
  const u64 vkey = u64(stop) << 32 | start;
  if (stop == ~0u) blocks_[start];  // (a failure is remembered as nullptr)
  else variants_[vkey];
  if (!arena_ || c.ic_.enabled() || c.dc_.enabled()) return nullptr;

  // ---- the instructions ----
  std::vector<Insn> insns;
  std::deque<Insn> slots;  // (stable addresses)
  for (u32 pc = start;;) {
    if (!insns.empty() && (idle_pcs_.count(pc) || pc == c.lp_end_ || pc == stop || (pc >> 16) != (start >> 16))) break;
    const DecodedOp* o = c.decoded(pc);
    if (!o) break;
    Insn in;
    in.pc = pc;
    in.o = o;
    in.j = jit_classify(*o);
    in.len = o->len;
    if (in.j.kind == JitOp::Generic || in.j.kind == JitOp::Other) {
      unsigned len = 0;
      disassemble(pc, [&](u32 a) { return bus_.peek16(a); }, &len);
      if (len) in.len = len;
    }
    in.cost8 = o->cost8;
    in.hook = c.pc_hooks_.count(pc) != 0;
    // Shifts by a register that set flags leave C alone for a zero count,
    // which x86 cannot express cheaply: use the handler.
    if (in.j.kind == JitOp::Alu && in.j.f && !in.j.imm && (in.j.op == JitOp::kAsl || in.j.op == JitOp::kLsr || in.j.op == JitOp::kAsr))
      in.j.kind = JitOp::Other;
    insns.push_back(in);
    if (is_branch(in.j.kind) && in.j.delay) {  // the delay slot, if it can run inline
      const u32 spc = pc + in.len;
      const DecodedOp* so = (spc >> 16) == (start >> 16) && !idle_pcs_.count(spc) ? c.decoded(spc) : nullptr;
      if (so) {
        Insn s;
        s.pc = spc;
        s.o = so;
        s.j = jit_classify(*so);
        s.len = so->len;
        s.cost8 = so->cost8;
        s.hook = c.pc_hooks_.count(spc) != 0;
        const bool flag_shift = s.j.kind == JitOp::Alu && s.j.f && !s.j.imm &&
                                (s.j.op == JitOp::kAsl || s.j.op == JitOp::kLsr || s.j.op == JitOp::kAsr);
        if (slot_ok(s.j.kind) && !flag_shift) {
          slots.push_back(s);
          insns.back().slot = &slots.back();
        }
      }
    }
    if (is_branch(in.j.kind) || int(insns.size()) >= kMaxInsns) break;
    pc += in.len;
  }
  if (insns.empty()) return nullptr;

  // The most the block can take before its last instruction starts.
  const u32 max8 = max_wait8_;
  u32 guard = 0;
  for (size_t i = 0; i + 1 < insns.size(); i++) {
    const JitOp::Kind k = insns[i].j.kind;
    guard += insns[i].cost8;
    if (k == JitOp::Load || k == JitOp::Store || k == JitOp::Push || k == JitOp::Pop) guard += max8;
    else if (k == JitOp::Generic || k == JitOp::Other) guard += 4 * max8 + 8;
  }
  if (insns.back().slot) guard += insns.back().cost8;  // (the slot must start in the slice too)
  const u32 last = insns.back().pc;

  Emitted em;
  if (!emit(start, insns, guard, &em)) return nullptr;
  u8* code = reserve(em.code.size());
  if (!code) return nullptr;
  for (const Emitted::Reloc& r : em.relocs) {
    const u64 addr = u64(reinterpret_cast<uintptr_t>(code + r.target));
    std::memcpy(&em.code[size_t(r.at)], &addr, 8);
  }
  this->write(code, em.code.data(), em.code.size());
  if (const char* dir = std::getenv("LEAPEMU_JIT_DUMP")) {  // debugging: objdump -D -b binary -m i386:x86-64
    char name[512];
    std::snprintf(name, sizeof(name), "%s/%08x.bin", dir, start);
    if (FILE* f = std::fopen(name, "wb")) { std::fwrite(em.code.data(), 1, em.code.size(), f); std::fclose(f); }
  }
  // Profiling: LEAPEMU_JIT_PERFMAP=1 names each block for perf (/tmp/perf-<pid>.map).
  static FILE* const perf_map = [] {
    if (!std::getenv("LEAPEMU_JIT_PERFMAP")) return static_cast<FILE*>(nullptr);
    char name[64];
    std::snprintf(name, sizeof(name), "/tmp/perf-%d.map", int(getpid()));
    return std::fopen(name, "w");
  }();
  if (perf_map) {
    std::fprintf(perf_map, "%llx %zx arc_%08x%s\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(code)), em.code.size(), start,
                 stop != ~0u ? "_v" : "");
    std::fflush(perf_map);
  }
  // (reserve() may have flushed everything)
  auto b = std::make_unique<Block>();
  b->code = code;
  b->start = start;
  b->last = last;
  b->guard8 = guard;
  b->count = u32(insns.size());
  b->variant = stop != ~0u;
  Block* raw = b.get();
  if (stop == ~0u) blocks_[start] = std::move(b);
  else variants_[vkey] = std::move(b);
  return raw;
}

u32 Jit::slow_read(Bus* bus, u32 a, u32 size) { return bus->slow_read(a, int(size)); }
void Jit::slow_write(Bus* bus, u32 a, u32 v, u32 size) { bus->slow_write(a, v, int(size)); }

#endif  // LEAP_JIT

}  // namespace leap::arc
