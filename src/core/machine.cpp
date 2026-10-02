#include "core/machine.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "core/codesig.h"
#include "core/fileio.h"
#include "core/log.h"
#include "core/state.h"

namespace leap {

namespace {

constexpr u32 kBiosSize = 8u << 20;
constexpr u32 kRamBase = 0x3c00'0000, kRamSize = 0x0400'0000;  // matches MAME's map
constexpr u32 kVramBase = 0x0300'0000, kVramSize = 0x1'0000;
constexpr u32 kCartBase = 0x8000'0000;
constexpr u32 kIoBase = 0x0180'0000;
constexpr u32 kStubBase = 0x0200'0000;  // (unused by the hardware) see stub_missing_services
constexpr u32 kStubPort = 0x0201'0000;  // its I/O port
constexpr u32 kStubTable = 0xffff'0000;  // the page holding the pointer to its table
constexpr u32 kCallPage = 0x0204'0000;   // call_guest's return point (a halt)
constexpr u32 kCallStack = 0x3fff'0000;  // and its stack (the top of the RAM window)
constexpr u32 kCyclesPerTick = Machine::kCpuHz / Machine::kTimerHz;  // 6
constexpr u32 kCyclesPerFrame = Machine::kCpuHz / Machine::kFps;
constexpr u32 kCyclesPerSample = Machine::kCpuHz / Machine::kVoiceHz;   // voice mixing
constexpr u32 kCyclesPerPcm = Machine::kCpuHz / Machine::kAudioHz;      // PCM output
constexpr u32 kCyclesPerTouchSample = Machine::kCpuHz / 60;

// Interrupt lines (vector numbers).
constexpr unsigned kIrqAdc = 0x10;
constexpr unsigned kIrqDma = 0x12;
constexpr unsigned kIrqUart = 0x1b;
constexpr unsigned kIrqSound = 0x1d;
constexpr unsigned kIrqPcm = 0x13;         // PCM channels (flags bits 20, 21 in 0x0180'0080)
constexpr unsigned kIrqPower = 0x18;       // power button (flag 0x20 in 0x0180'0080)
constexpr u32 kFlagPowerButton = 0x20;
constexpr unsigned kIrqTimer[3] = {0x19, 0x1a, 0x1e};

// Bits of the GIO data-in register that are buttons (active low).
constexpr u32 kButtonMask = 0xb703'e380;
constexpr u32 kGioPowerSwitch = 1u << 11;  // GIODataIn: low = switched off

s16 alaw_to_linear(u8 v) {
  v ^= 0x55;
  unsigned exp = (v >> 4) & 7;
  int base = ((v & 0x0f) << 1) | 1;
  if (exp != 0) {
    base |= 0x20;
    exp -= 1;
  }
  const int s = base << (exp + 2);
  return s16((v & 0x80) ? -s : s);
}

u32 pal4(u32 v) { return (v & 15) * 17; }
u32 pal3(u32 v) { v &= 7; return (v << 5) | (v << 2) | (v >> 1); }
u32 pal2(u32 v) { return (v & 3) * 0x55; }
u32 argb(u32 r, u32 g, u32 b) { return 0xff000000u | (r << 16) | (g << 8) | b; }

}  // namespace

Machine::Machine()
    : cpu_(bus_),
      bios_(kBiosSize),
      vram_(kVramSize),
      ram_(kRamSize),
      fb_(size_t(kWidth) * kHeight, 0xff000000u) {
  cpu_.set_reset_vector(0x4000'0000);
  cpu_.set_aux_handler({
      [this](u32 reg) -> u32 {
        switch (reg) {
          case 0x11: case 0x1b: case 0x48: return 0;
        }
        LOG_D("aux read %03x", reg);
        return 0;
      },
      [this](u32 reg, u32 v) {
        switch (reg) {
          case 0x1a: palette_[palette_ptr_++ & 0x7ff] = u16(v); return;
          case 0x10: case 0x11: case 0x47: case 0x48: case 0x4b: return;
        }
        LOG_D("aux write %03x = %08x", reg, v);
      },
  });
  map_memory();
  reset();
}

Machine::~Machine() = default;

void Machine::map_memory() {
  cpu_.flush_decode_cache();
  bus_.clear();
  bus_.map_memory(0x0000'0000, kBiosSize, bios_.data(), false);
  bus_.map_memory(0x4000'0000, kBiosSize, bios_.data(), false);
  bus_.map_mmio(kIoBase, 0x1'0000, this);
  bus_.map_memory(kVramBase, kVramSize, vram_.data(), true);
  // The original BIOS clears past the end of VRAM (a bug); ignore it.
  bus_.map_nop(0x0301'0000, 0x2'0000);
  bus_.map_memory(kRamBase, kRamSize, ram_.data(), true);
  if (!cart_.empty()) bus_.map_memory(kCartBase, u32(cart_.size()), cart_.data(), false);
  // A download's code runs from RAM: cached and compiled like ROM, watched for writes.
  if (downloadable()) cpu_.set_ram_code(ram_image_at_, ram_image_at_ + u32(ram_image_.size()));
  else cpu_.set_ram_code(0, 0);
  if (service_stub_) {
    bus_.map_memory(kStubBase, Bus::kPageSize, stub_page_.data(), false);
    bus_.map_mmio(kStubPort, Bus::kPageSize, this);
    bus_.map_memory(kStubTable, Bus::kPageSize, stub_table_page_.data(), false);
  }
  apply_timing();
}

void Machine::apply_timing() {
  auto q = [](float cycles) { return u8(std::clamp(int(cycles * 8.0f + 0.5f), 0, 255)); };
  struct { u8 rom16, rom32, ram16, ram32, sram16, sram32, io16, io32; } t = {
      q(timing.rom16), q(timing.rom32), q(timing.ram16), q(timing.ram32),
      q(timing.sram16), q(timing.sram32), q(timing.io16), q(timing.io32)};
  bus_.set_wait(0x0000'0000, kBiosSize, t.rom16, t.rom32);
  bus_.set_wait(0x4000'0000, kBiosSize, t.rom16, t.rom32);
  if (!cart_.empty())
    bus_.set_wait(kCartBase, u32(cart_.size()), timing.cart16 >= 0 ? q(timing.cart16) : t.rom16,
                  timing.cart32 >= 0 ? q(timing.cart32) : t.rom32);
  bus_.set_wait(kRamBase, kRamSize, t.ram16, t.ram32);
  bus_.set_wait(kVramBase, kVramSize, t.sram16, t.sram32);
  bus_.set_wait(kIoBase, 0x1'0000, t.io16, t.io32);
  const u8 rom_fill = q(timing.rom_fill16), ram_fill = q(timing.ram_fill16);
  bus_.set_cacheable(0x0000'0000, kBiosSize, true, rom_fill);
  bus_.set_cacheable(0x4000'0000, kBiosSize, true, rom_fill);
  if (!cart_.empty()) bus_.set_cacheable(kCartBase, u32(cart_.size()), true, rom_fill);
  bus_.set_cacheable(kRamBase, kRamSize, true, ram_fill);
  auto geometry = [&](u32 kb) { return arc::CacheModel::Config{kb, timing.line_bytes, timing.ways}; };
  cpu_.configure_caches(geometry(timing.icache_kb), geometry(timing.dcache_kb),
                        (timing.line_bytes / 2) * ram_fill);  // also flushes decoded ops (they cache fetch costs)
}

bool Machine::load_bios(const std::string& path, std::string* err) {
  std::vector<u8> img;
  if (!load_rom_file(path, &img, err)) return false;
  return load_bios_image(std::move(img), err);
}

bool Machine::load_bios_image(std::vector<u8> img, std::string* err) {
  if (img.size() > kBiosSize) { *err = "BIOS image larger than 8 MiB"; return false; }
  std::fill(bios_.begin(), bios_.end(), 0);
  std::copy(img.begin(), img.end(), bios_.begin());
  bios_pristine_ = bios_;
  bios_crc_ = crc32(img.data(), img.size());
  bios_loaded_ = true;
  cpu_.flush_decode_cache();
  bios_header_ = parse_rom_header(img);
  bios_problem_.clear();
  if (!bios_header_.valid) {
    // (Every LeapFrog BaseROM has the ROM header cartridges have too.)
    bios_problem_ = "this does not look like a Leapster BaseROM (it has no ROM header); it will probably not boot";
    LOG_W("BaseROM: %s", bios_problem_.c_str());
  } else if (const int bit = stuck_address_line(img, bios_header_); bit >= 0) {
    bios_problem_ = "this BaseROM dump is damaged: it was read with address line A" + std::to_string(bit) +
                    " stuck, so half of it is missing and it cannot boot; a new dump is needed";
    LOG_W("BaseROM: %s", bios_problem_.c_str());
  }
  update_capture();
  return true;
}

bool Machine::load_cart(const std::string& path, std::string* err) {
  std::vector<u8> img;
  if (!load_rom_file(path, &img, err)) return false;
  return load_cart_image(std::move(img), path, err);
}

bool Machine::load_cart_image(std::vector<u8> img, const std::string& name, std::string* err) {
  if (img.size() > 0x1000'0000) { *err = "cartridge image too large"; return false; }
  // Leapster 2 system programs (the SD menu, and homebrew that replaces it)
  // start with "JUMP": they need the Leapster 2's own BaseROM.
  if (img.size() >= 4 && std::memcmp(img.data(), "JUMP", 4) == 0) {
    *err = "this is a Leapster 2 system program (it replaces the Leapster 2's SD card menu), not a game; "
           "it needs the Leapster 2's BaseROM, which leapemu does not have";
    return false;
  }
  // Leapster 2 downloadable games are cartridge images linked to run from RAM.
  const RomHeader h = parse_rom_header(img);
  if (h.valid && h.device_start >= kRamBase && h.device_start < kRamBase + kRamSize) {
    if (h.device_start - kRamBase + img.size() > kRamSize) { *err = "not a Leapster 2 downloadable"; return false; }
    download_name_ = std::filesystem::path(name).stem().string();
    cart_crc_ = crc32(img.data(), img.size());
    patch_cart(img);
    cart_header_ = h;
    ram_image_ = img;
    ram_image_at_ = h.device_start;
    img.resize((img.size() + Bus::kPageMask) & ~size_t(Bus::kPageMask), 0xff);
    cart_ = std::move(img);
    cart_eeprom_.fill(0);
    map_memory();
    update_capture();
    return true;
  }
  cart_crc_ = crc32(img.data(), img.size());
  if (const unsigned n = join_rom_blocks(&img)) LOG_I("cartridge: a development image; removed %u block header%s", n, n == 1 ? "" : "s");
  patch_cart(img);
  cart_header_ = h;
  ram_image_.clear();
  img.resize((img.size() + Bus::kPageMask) & ~size_t(Bus::kPageMask), 0xff);
  cart_ = std::move(img);
  cart_eeprom_.fill(0);  // the save EEPROM is in the cartridge: blank until loaded
  map_memory();
  update_capture();
  return true;
}

void Machine::set_native_capture(bool on) {
  native_capture_ = on;
  update_capture(false);
}

void Machine::update_capture(bool images_changed) {
  if (images_changed) capture_sites_.reset();
  if (native_capture_) {  // the Flash player is in the BaseROM: also without a cartridge
    if (!capture_sites_) capture_sites_ = DrawCapture::locate(cart_, kCartBase, bios_, 0x4000'0000);
    if (!capture_.install(bus_, cpu_, *capture_sites_)) capture_.uninstall(cpu_);
  } else {
    capture_.uninstall(cpu_);
  }
}

void Machine::eject_cart() {
  cart_.clear();
  ram_image_.clear();
  cart_eeprom_.fill(0);
  cart_header_ = {};
  cart_crc_ = 0;
  cart_patch_ = {};
  map_memory();
  update_capture();
}

// Optional BaseROM patches (host conveniences, never needed for commercial
// software). Patch points are located by matching the surrounding bytes so the
// same patch works across BaseROM versions, and are only applied when the
// match is unique.
void Machine::apply_bios_patches() {
  if (bios_pristine_.size() == bios_.size()) bios_ = bios_pristine_;
  unsigned_patched_ = false;
  if (allow_unsigned) {
    // LeapFrog cartridges pass three checks at power-on (see docs/homebrew.md):
    // a 160-bit digest compared against the image, a spot check of the
    // "copyright verse" text, and the sparse checksum (every 0xF94th word).
    // All are forced to "pass". The checksum check lets prototypes (never
    // given checksums) and slightly damaged dumps run; LeapFrog's development
    // units skip it (strap bit 21 clear). Each patch point is located by a
    // signature of the code around it (core/codesig.h), in one of its known
    // compiled forms, and only patched if it occurs exactly once.
    struct Variant {
      CodeSig sig;  // halfword aligned
      std::vector<std::pair<size_t, std::array<u8, 2>>> writes;  // offset -> new halfword
    };
    struct Patch {
      const char* name;
      std::vector<Variant> variants;
    };
    static const Patch kPatches[] = {
        // Where the digest compare's result is chosen: both outcomes become
        // mov_s r0,1.
        {"digest check", {{{10, 0x101f6617f1dc2c36}, {{4, {0x01, 0xd8}}, {10, {0x01, 0xd8}}}}}},
        // The end of the verse comparison: its "fail" result becomes mov_s r0,1.
        {"verse check", {{{12, 0x0127604fecdd4c57}, {{8, {0x01, 0xd8}}}}}},
        // After summing the words: the load of the stored sum becomes a copy
        // of the computed sum, so the two compare equal.
        {"checksum check",
         {{{10, 0x19f9e4932eee8978}, {{4, {0xc9, 0x72}}}},    // v1.5: mov_s r2,r14
          {{12, 0x11cc175ed0188168}, {{4, {0x68, 0x70}}}}}},  // v2.x: mov_s r0,r3
    };
    bool all = true;
    for (const Patch& p : kPatches) {
      bool done = false;
      for (const Variant& v : p.variants) {
        const std::vector<size_t> hits = find_code(bios_, {v.sig}, 2, 2)[0];
        if (hits.size() != 1) continue;
        for (const auto& [off, hw] : v.writes) {
          bios_[hits[0] + off] = hw[0];
          bios_[hits[0] + off + 1] = hw[1];
        }
        done = true;
        break;
      }
      if (!done) {
        LOG_W("allow_unsigned: %s not found in this BaseROM", p.name);
        all = false;
      }
    }
    unsigned_patched_ = all;
  }
  cpu_.flush_decode_cache();
}

// stub_missing_services. The Leapster 2's file functions are table B of
// registry entry 52, past the end of this BaseROM's registry. Every place a
// game reaches that table does it the same way:
//   add rA,rA,0x270; ld_s rA,[rA,4]        (rA: the registry + 0xc)
// In the RAM copy of the download, each such add becomes "mov rA,-2048", so
// the load reads a pointer from 0xffff'f804 (kStubTable's page) instead: to
// leapemu's table at kStubBase + 0x300, whose slot k is code that hands r0-r3
// and k to the port at kStubPort, which answers (service_call):
//   common: st r0..r3,[port+0..0xc]; st r12,[port+0x10]; ld r0,[port+0x14]; j_s [blink]
void Machine::apply_service_stub() {
  const bool was = service_stub_;
  service_stub_ = false;
  if (stub_missing_services && downloadable()) {
    static const u8 kCommon[0x34] = {
        0x00, 0x1e, 0x00, 0x70, 0x01, 0x02, 0x00, 0x00, 0x00, 0x1e, 0x40, 0x70, 0x01, 0x02, 0x04, 0x00,
        0x00, 0x1e, 0x80, 0x70, 0x01, 0x02, 0x08, 0x00, 0x00, 0x1e, 0xc0, 0x70, 0x01, 0x02, 0x0c, 0x00,
        0x00, 0x1e, 0x00, 0x73, 0x01, 0x02, 0x10, 0x00, 0x00, 0x16, 0x00, 0x70, 0x01, 0x02, 0x14, 0x00,
        0xe0, 0x7e, 0xe0, 0x78};
    auto put16 = [](u8* p, u16 v) { p[0] = u8(v); p[1] = u8(v >> 8); };
    auto put32 = [](u8* p, u32 v) { p[0] = u8(v); p[1] = u8(v >> 8); p[2] = u8(v >> 16); p[3] = u8(v >> 24); };
    stub_page_.assign(Bus::kPageSize, 0);
    std::memcpy(stub_page_.data(), kCommon, sizeof(kCommon));
    // Slot k of table t (kTables) is entry n = 32t + k, at kStubBase + 0x40 + 8n:
    //   mov_s r12,0; mov_s r12,n; b common
    // (The first byte of each is 0, which matters: Rabbit River and others use
    // the address of one of these functions as a string buffer, then read it
    // back; here the writes are dropped and the string is empty.)
    for (u32 n = 0; n < 64; n++) {
      u8* e = &stub_page_[0x40 + 8 * n];
      put16(e, 0xdc00);
      put16(e + 2, u16(0xdc00 | n));
      const u32 s = u32(-s32(0x44 + 8 * n)) & 0x1ff'ffff;  // (to common, from the branch)
      const u32 b = 0x0001'0000 | ((s >> 1) & 0x3ff) << 17 | ((s >> 11) & 0x3ff) << 6 | (s >> 21);
      put16(e + 4, u16(b >> 16));
      put16(e + 6, u16(b));
      put32(&stub_page_[0x300 + 4 * n], kStubBase + 0x40 + 8 * n);
    }
    stub_table_page_.assign(Bus::kPageSize, 0);
    // The tables redirected: entry 52 (file functions) and entry 54, table B.
    struct Table { u16 add_s12, mov_s12; u32 slot; };  // (s12 fields as encoded, low halfword)
    static const Table kTables[2] = {{0x0c09, 0x0020, 0xf804},   // add 0x270 -> mov -2048
                                     {0x020a, 0x0024, 0xf904}};  // add 0x288 -> mov -1792
    for (u32 t = 0; t < 2; t++) put32(&stub_table_page_[kTables[t].slot], kStubBase + 0x300 + 0x80 * t);
    // The call sites, in the RAM copy.
    static const unsigned kReg3[8] = {0, 1, 2, 3, 12, 13, 14, 15};
    u8* img = ram_.data() + (ram_image_at_ - kRamBase);
    unsigned sites[2] = {};
    for (size_t o = 0; o + 6 <= ram_image_.size(); o += 2) {
      const u16 h1 = u16(img[o] | img[o + 1] << 8), h2 = u16(img[o + 2] | img[o + 3] << 8), n = u16(img[o + 4] | img[o + 5] << 8);
      if ((h1 & 0xf8ff) != 0x2080) continue;  // add rA,rA,s12
      for (u32 t = 0; t < 2; t++) {
        if ((h2 & 0x8fff) != kTables[t].add_s12) continue;
        const unsigned r = ((h1 >> 8) & 7) | ((h2 >> 12) & 7) << 3;
        if ((n >> 11) != 0x10 || (n & 31) != 1 || kReg3[(n >> 8) & 7] != r || kReg3[(n >> 5) & 7] != r) continue;  // ld_s rA,[rA,4]
        put16(&img[o], u16(0x208a | (r & 7) << 8));  // mov rA,s12
        put16(&img[o + 2], u16(kTables[t].mov_s12 | (r >> 3) << 12));
        sites[t]++;
      }
    }
    LOG_I("missing services: %u calls into the Leapster 2 file functions and %u into entry 54 redirected", sites[0], sites[1]);
    service_stub_ = sites[0] + sites[1] != 0;
  }
  if (service_stub_ != was) map_memory();
}

bool Machine::call_guest(u32 fn, const std::vector<u32>& args, u32* result, u64 max_cycles) {
  if (call_page_.empty()) {
    call_page_.assign(Bus::kPageSize, 0);
    static const u8 kHalt[8] = {0x69, 0x20, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00};  // flag 1; b .
    std::memcpy(call_page_.data(), kHalt, sizeof(kHalt));
    bus_.map_memory(kCallPage, Bus::kPageSize, call_page_.data(), false);
  }
  arc::Cpu& c = cpu_;
  c.begin_call(fn);
  for (unsigned i = 0; i < 8; i++) c.set_reg(i, i < args.size() ? args[i] : 0);
  c.set_reg(arc::Cpu::kSP, kCallStack);
  c.set_reg(arc::Cpu::kBLINK, kCallPage);
  c.set_status32(c.status32() & ~(arc::Cpu::kE1 | arc::Cpu::kE2));
  for (u64 ran = 0; ran < max_cycles;) {
    ran += c.run(1'000'000);
    if (c.halted() && c.pc() == kCallPage + 4) {
      if (result) *result = c.reg(0);
      c.begin_call(kCallPage);
      return true;
    }
    if (c.stop_reason() == arc::Cpu::Stop::Fault || c.halted()) return false;
  }
  return false;
}

// What a redirected call asked for: slot stub_args_[4] of the file functions'
// table, with arguments stub_args_[0..3].
u32 Machine::service_call() {
  const u32 fn = stub_args_[4];
  if ((fn >= 10 && fn <= 14) || fn == 17 || fn == 32 + 10) return stub_file_call(fn);
  static int warned = 0;
  if (warned++ < 8) {
    auto str = [&](u32 a) {
      std::string s;
      for (u8 c; s.size() < 48 && (c = bus_.read8(a + u32(s.size()))) >= 32 && c < 127;) s.push_back(char(c));
      return s;
    };
    LOG_W("missing service: entry %u B[%u](%08x \"%s\", %08x \"%s\", %08x, %08x) not emulated (returns 0)",
          fn < 32 ? 52 : 54, fn % 32, stub_args_[0], str(stub_args_[0]).c_str(), stub_args_[1], str(stub_args_[1]).c_str(),
          stub_args_[2], stub_args_[3]);
  }
  return 0;
}

// The Leapster 2's file functions (C stdio-style), for the one file these games
// use: their save, <directory>\eeprom.ltm (256 bytes in Number Raiders and
// Letterpillar, 2 KiB in Rabbit River, Chicken Coop and Shape Shop). It is the
// cartridge save memory (2 KiB, so the .sav): it exists unless that is all
// zeros, and is always 2 KiB long. (A game creates it and writes it whole, then
// reads and writes records in it; none asks its size. A file created all zeros
// looks absent, and is created again the same.)
u32 Machine::stub_file_call(unsigned fn) {
  constexpr u32 kMax = u32(sizeof(cart_eeprom_));
  const u32* a = stub_args_;
  auto length = [&] {
    return stub_file_made_ || std::any_of(cart_eeprom_.begin(), cart_eeprom_.end(), [](u8 b) { return b != 0; }) ? kMax : 0u;
  };
  auto file = [&](u32 handle) -> StubFile* {
    const u32 i = (handle - (kStubBase + 0x800)) / 0x10;
    return handle >= kStubBase + 0x800 && i < 4 && stub_files_[i].open ? &stub_files_[i] : nullptr;
  };
  auto str = [&](u32 addr) {
    std::string s;
    for (u8 c; s.size() < 128 && (c = bus_.read8(addr + u32(s.size()))) != 0;) s.push_back(char(c));
    return s;
  };
  // The current program's file name, in a file whose name entry 54's B[10] gives.
  static const char kInfoPath[] = "B:\\Leapster\\System\\CurrentProgram";
  const std::string info = "B:\\Leapster\\Apps\\" + download_name_ + "\\" + download_name_ + ".bin";
  switch (fn) {
    case 32 + 10: {  // entry 54 B[10](buffer, size, 1): the name of that file
      for (u32 i = 0; i < a[1]; i++) {
        const u8 c = i + 1 < a[1] && i < sizeof(kInfoPath) ? u8(kInfoPath[i]) : 0;
        bus_.write8(a[0] + i, c);
        if (!c) break;
      }
      return 0;
    }
    case 17: {  // size(path) -> bytes
      const std::string path = str(a[0]);
      if (path == kInfoPath) return u32(info.size());
      LOG_W("missing service: size(\"%s\") not emulated", path.c_str());
      return 0;
    }
    case 10: {  // fopen(path, mode) -> file or 0
      const std::string path = str(a[0]), mode = str(a[1]);
      if (path == kInfoPath && mode[0] == 'r')
        for (u32 i = 0; i < 4; i++)
          if (!stub_files_[i].open) {
            stub_files_[i] = StubFile{1, 0, 0, 1};
            return kStubBase + 0x800 + 0x10 * i;
          }
      const size_t slash = path.find_last_of("\\/");
      std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
      for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (name != "eeprom.ltm" || mode.empty()) { LOG_W("missing service: fopen(\"%s\") not emulated", path.c_str()); return 0; }
      const bool exists = length() != 0;
      if (mode[0] == 'r' && !exists) return 0;
      if (mode[0] == 'w') cart_eeprom_.fill(0);  // (truncated)
      if (mode[0] != 'r') stub_file_made_ = 1;
      for (u32 i = 0; i < 4; i++)
        if (!stub_files_[i].open) {
          StubFile& f = stub_files_[i];
          f.open = 1;
          f.writable = mode[0] != 'r' || mode.find('+') != std::string::npos;
          f.pos = 0;
          return kStubBase + 0x800 + 0x10 * i;
        }
      return 0;
    }
    case 11:  // fclose(file) -> 0
      if (StubFile* f = file(a[0])) { *f = StubFile{}; return 0; }
      return u32(-1);
    case 12:  // fread(ptr, size, count, file) -> items read
    case 13: {  // fwrite(ptr, size, count, file) -> items written
      StubFile* f = file(a[3]);
      if (!f || a[1] == 0 || (fn == 13 && !f->writable)) return 0;
      u32 n = 0;
      if (f->info) {  // (read-only text)
        for (; n < a[2] && f->pos + a[1] <= info.size(); n++)
          for (u32 k = 0; k < a[1]; k++, f->pos++) bus_.write8(a[0] + n * a[1] + k, u8(info[f->pos]));
        return n;
      }
      for (; n < a[2]; n++) {
        if (f->pos + a[1] > (fn == 12 ? length() : kMax)) break;
        for (u32 k = 0; k < a[1]; k++, f->pos++) {
          if (fn == 12) bus_.write8(a[0] + n * a[1] + k, cart_eeprom_[f->pos]);
          else cart_eeprom_[f->pos] = bus_.read8(a[0] + n * a[1] + k);
        }
      }
      return n;
    }
    case 14: {  // fseek(file, offset, whence) -> 0 / -1
      StubFile* f = file(a[0]);
      if (!f) return u32(-1);
      const s64 base = a[2] == 0 ? 0 : a[2] == 1 ? s64(f->pos) : s64(length());
      const s64 to = base + s64(s32(a[1]));
      if (to < 0 || to > kMax) return u32(-1);
      f->pos = u32(to);
      return 0;
    }
  }
  return 0;
}

void Machine::reset() {
  apply_bios_patches();
  std::fill(ram_.begin(), ram_.end(), 0);
  for (StubFile& f : stub_files_) f = StubFile{};
  stub_file_made_ = 0;
  if (!ram_image_.empty()) std::copy(ram_image_.begin(), ram_image_.end(), ram_.begin() + (ram_image_at_ - kRamBase));
  apply_service_stub();  // (after the copy: it edits it)
  std::fill(vram_.begin(), vram_.end(), 0);
  std::fill(fb_.begin(), fb_.end(), 0xff000000u);
  capture_.reset();
  cpu_.reset();

  int_flags_ = 0;
  int_enable_ = 0;
  clock_div_ = 0;
  std::fill(std::begin(lcd_), std::end(lcd_), 0);
  std::fill(std::begin(dma_), std::end(dma_), 0);
  for (auto& t : timers_) {
    t = Timer{};
    t.overflow_at = u64(t.max) * kCyclesPerTick;
  }
  palette_.fill(0);
  palette_ptr_ = 0;

  std::fill(std::begin(adc_ctrl_), std::end(adc_ctrl_), 0);
  adc_fifo_.clear();
  adc_enabled_ = false;
  touch_initted_ = false;
  adc_next_ = 0;

  eeprom_cmd_ = 0;
  for (auto& v : voices_) v = Voice{};
  master_volume_ = 0x4000;
  lcd_brightness_ = 0x30;
  lcd_ctrl_ = 0;
  celp_ = CelpDecoder{};
  celp_cb_[0] = celp_cb_[1] = 0;
  audio_.clear();
  audio_next_ = kCyclesPerSample;
  for (auto& ch : pcm_) ch = PcmChannel{};
  pcm_next_ = 0;
  voice_hold_ = 0;
  frames_ = 0;
  next_frame_ = kCyclesPerFrame;
  autocal_ = AutoCal{};
  powered_off_ = false;
}

// ---------------------------------------------------------------------------
// Non-volatile storage
// ---------------------------------------------------------------------------

namespace {
// Reads up to N bytes (a shorter file leaves the rest zero). Returns whether
// the file exists.
template <size_t N>
bool read_blob(const std::filesystem::path& p, std::array<u8, N>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.fill(0);
  f.read(reinterpret_cast<char*>(out.data()), N);
  return true;
}
// (Whole or not at all: a crash mid-write never leaves a truncated save.)
template <size_t N>
bool write_blob(const std::filesystem::path& p, const std::array<u8, N>& in) {
  return write_file_atomic(p, in.data(), N);
}
std::string cart_nvram_name(u32 crc) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%08x.eep", crc);
  return buf;
}
}  // namespace

void Machine::load_nvram(const std::string& dir) {
  const std::filesystem::path d(dir);
  load_system_nvram((d / "system.eep").string());
  if (has_cart()) load_cart_save((d / cart_nvram_name(cart_crc_)).string());
}

bool Machine::save_nvram(const std::string& dir) const {
  const std::filesystem::path d(dir);
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  bool ok = save_system_nvram((d / "system.eep").string());
  if (has_cart()) ok = save_cart_save((d / cart_nvram_name(cart_crc_)).string()) && ok;
  return ok;
}

bool Machine::load_system_nvram(const std::string& path) {
  sys_eeprom_.fill(0);
  return read_blob(path, sys_eeprom_);
}

bool Machine::save_system_nvram(const std::string& path) const { return write_blob(path, sys_eeprom_); }

bool Machine::load_downloadable(const std::string& path, std::string* err) {
  std::vector<u8> img;
  if (!load_rom_file(path, &img, err)) return false;
  const RomHeader h = parse_rom_header(img);
  if (!h.valid || h.device_start < kRamBase || h.device_start >= kRamBase + kRamSize) {
    *err = "not a Leapster 2 downloadable";
    return false;
  }
  return load_cart_image(std::move(img), path, err);
}

void Machine::patch_cart(std::vector<u8>& img) {
  cart_patch_ = {};
  if (!cart_patches) return;
  cart_patch_ = apply_edits(img, cart_patches(cart_crc_));
  if (cart_patch_.applied || cart_patch_.mismatched)
    LOG_I("ROM patches: %u edits applied, %u not matching this image", cart_patch_.applied, cart_patch_.mismatched);
}

bool Machine::load_cart_save(const std::string& path) {
  cart_eeprom_.fill(0);
  return read_blob(path, cart_eeprom_);
}

bool Machine::save_cart_save(const std::string& path) const { return write_blob(path, cart_eeprom_); }

// ---------------------------------------------------------------------------
// Save states
//
// Layout: "LEAPSTATE" magic, u32 version, u32 BIOS CRC, u32 cart CRC, then
// the serialized machine. Main RAM is stored sparsely (only non-zero 64 KiB
// pages) since games use ~2 MiB of the 64 MiB window.
// ---------------------------------------------------------------------------

namespace {
constexpr char kStateMagic[10] = "LEAPSTATE";
constexpr u32 kStateVersion = 13;  // 13: more of stub_missing_services' file state, 12: open files of stub_missing_services, 11: PCM channels, XY memory, 10: LCD brightness, 9: master volume, 8: interrupt enables, 7: power switch / off, 6: cache model, 5: sub-cycle timing  // 2: speech decoder, 3: no struct padding
}  // namespace

template <class Ar>
void Machine::serialize(Ar& ar) {
  cpu_.serialize(ar);

  // Main RAM, sparse: only 64 KiB pages holding data (games use ~2 MiB of the
  // 64 MiB window). Found in one fast pass, as states are also taken every
  // few frames for rewind.
  constexpr u32 kPage = 0x1'0000;
  static const u8 kZeroPage[kPage] = {};
  auto page_empty = [&](u32 off) { return std::memcmp(&ram_[off], kZeroPage, kPage) == 0; };
  u32 npages = 0;
  std::vector<u32> pages;
  if constexpr (!Ar::kLoading) {
    for (u32 off = 0; off < kRamSize; off += kPage)
      if (!page_empty(off)) pages.push_back(off);
    npages = u32(pages.size());
  }
  ar.io(npages);
  if constexpr (Ar::kLoading) {
    for (u32 off = 0; off < kRamSize; off += kPage)  // (clearing only what holds data)
      if (!page_empty(off)) std::memset(&ram_[off], 0, kPage);
    for (u32 i = 0; i < npages && ar.ok; i++) {
      u32 off = 0;
      ar.io(off);
      if (off > kRamSize - kPage || off % kPage) { ar.ok = false; break; }
      ar.bytes(&ram_[off], kPage);
    }
  } else {
    for (u32 off : pages) {
      ar.io(off);
      ar.bytes(&ram_[off], kPage);
    }
  }
  ar.bytes(vram_.data(), vram_.size());
  ar.bytes(reinterpret_cast<u8*>(fb_.data()), fb_.size() * sizeof(u32));

  ar.io(frames_); ar.io(next_frame_);
  ar.io(int_flags_); ar.io(clock_div_); ar.io(lcd_); ar.io(dma_);
  // Structs are serialized field by field: raw copies would include padding.
  for (auto& t : timers_) {
    ar.io(t.ticks_base); ar.io(t.base_cycle); ar.io(t.control); ar.io(t.max); ar.io(t.overflow_at);
  }
  ar.io(palette_); ar.io(palette_ptr_);
  ar.io(adc_ctrl_); ar.io(adc_fifo_); ar.io(adc_enabled_); ar.io(touch_initted_); ar.io(adc_next_);
  ar.io(eeprom_cmd_); ar.io(sys_eeprom_); ar.io(cart_eeprom_);
  for (auto& v : voices_) {
    ar.io(v.start); ar.io(v.end); ar.io(v.volume); ar.io(v.pitch); ar.io(v.active); ar.io(v.pos); ar.io(v.step);
  }
  ar.io(audio_next_);
  celp_.serialize(ar); ar.io(celp_cb_);
  ar.io(straps);
  if (ar.version >= 7) { ar.io(powered_off_); ar.io(power_switch_); }
  if (ar.version >= 8) ar.io(int_enable_);
  if (ar.version >= 9) ar.io(master_volume_);
  if (ar.version >= 10) { ar.io(lcd_brightness_); ar.io(lcd_ctrl_); }
  if constexpr (Ar::kLoading) {  // (older states: the defaults)
    if (ar.version < 10) lcd_brightness_ = 0x30;
    if (ar.version < 9) master_volume_ = 0x1734;     // the default volume setting
    if (ar.version < 8) int_enable_ = 0x0005'4520;   // what the BaseROM enables at boot
  }
  if (ar.version >= 11) {
    for (auto& ch : pcm_) { ar.io(ch.ctrl); ar.io(ch.base); ar.io(ch.pos); }
    ar.io(pcm_next_); ar.io(voice_hold_);
  } else if constexpr (Ar::kLoading) {
    for (auto& ch : pcm_) ch = PcmChannel{};
    pcm_next_ = 0; voice_hold_ = 0;
  }
  if (ar.version >= 12) {
    for (StubFile& f : stub_files_) {
      ar.io(f.open); ar.io(f.writable); ar.io(f.pos);
      if (ar.version >= 13) ar.io(f.info);
    }
  } else if constexpr (Ar::kLoading) {
    for (StubFile& f : stub_files_) f = StubFile{};
  }
  if (ar.version >= 13) ar.io(stub_file_made_);
  else if constexpr (Ar::kLoading) stub_file_made_ = 0;
}

std::vector<u8> Machine::save_state() {
  StateWriter w;
  w.raw(kStateMagic, sizeof(kStateMagic));
  u32 version = kStateVersion, bcrc = bios_crc_, ccrc = cart_crc_;
  w.io(version); w.io(bcrc); w.io(ccrc);
  w.version = kStateVersion;
  serialize(w);
  return std::move(w.data);
}

bool Machine::load_state(const std::vector<u8>& data, std::string* err) {
  StateReader r(data.data(), data.size());
  char magic[sizeof(kStateMagic)];
  r.raw(magic, sizeof(magic));
  u32 version = 0, bcrc = 0, ccrc = 0;
  r.io(version); r.io(bcrc); r.io(ccrc);
  if (!r.ok || std::memcmp(magic, kStateMagic, sizeof(magic)) != 0) { *err = "not a leapemu save state"; return false; }
  if (version < 4 || version > kStateVersion) { *err = "unsupported save state version " + std::to_string(version); return false; }
  r.version = version;
  if (bcrc != bios_crc_) { *err = "save state was made with a different BIOS"; return false; }
  if (ccrc != cart_crc_) { *err = "save state was made with a different cartridge"; return false; }

  // Deserialize into the live machine; on failure, restore the previous state.
  std::vector<u8> backup = save_state();
  serialize(r);
  if (!r.ok || r.remaining() != 0) {
    StateReader undo(backup.data() + sizeof(kStateMagic) + 12, backup.size() - sizeof(kStateMagic) - 12);
    undo.version = kStateVersion;
    serialize(undo);
    cpu_.ram_changed();
    *err = "save state is truncated or corrupt";
    return false;
  }
  cpu_.ram_changed();  // (RAM was replaced, code a download runs from included)
  audio_.clear();
  capture_.reset();
  return true;
}

bool Machine::save_state_file(const std::string& path, std::string* err) {
  const std::vector<u8> data = save_state();
  return write_file_atomic(path, data.data(), data.size(), err);
}

bool Machine::load_state_file(const std::string& path, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { *err = "cannot open " + path; return false; }
  std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return load_state(data, err);
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

u64 Machine::next_event() const {
  u64 n = audio_next_;
  if (pcm_active()) n = std::min(n, pcm_next_);
  for (const auto& t : timers_) n = std::min(n, t.overflow_at);
  if (adc_enabled_) n = std::min(n, adc_next_);
  return n;
}

void Machine::run_events() {
  const u64 t = now();
  for (int i = 0; i < 3; i++) {
    Timer& tm = timers_[i];
    while (tm.overflow_at <= t) {
      tm.ticks_base = 0;
      tm.base_cycle = tm.overflow_at;
      tm.overflow_at += u64(std::max<u32>(tm.max, 1)) * kCyclesPerTick;
      if (tm.control & 1) cpu_.raise_irq(kIrqTimer[i]);
    }
  }
  if (adc_enabled_ && adc_next_ <= t) {
    adc_sample();
    adc_next_ += kCyclesPerTouchSample;
    if (adc_next_ <= t) adc_next_ = t + kCyclesPerTouchSample;
  }
  // Voices (8 kHz) and PCM (32 kHz), in time order: at a common tick the
  // voices mix first.
  for (;;) {
    const bool pcm = pcm_active() && pcm_next_ <= t;
    if (audio_next_ <= t && (!pcm || audio_next_ <= pcm_next_)) {
      snd_mix_sample();
      audio_next_ += kCyclesPerSample;
    } else if (pcm) {
      pcm_tick();
      pcm_next_ += kCyclesPerPcm;
    } else {
      break;
    }
  }
}

bool Machine::run_cycles(u64 n) {
  const u64 target = now() + n;
  while (now() < target) {
    const u64 until = std::min(target, next_event());
    if (until > now()) cpu_.run(until - now());
    if (cpu_.stop_reason() != arc::Cpu::Stop::None) return false;
    run_events();
  }
  return true;
}

// Turning the power off is seen two ways: the power line interrupts at once
// (line 0x18, flag 0x20, when enabled in 0x0180'0084), which starts the
// shutdown whatever the game is doing, and GIODataIn bit 11 reads low, which a
// BaseROM monitor polls (it can be held off while a game is busy).
void Machine::set_power_switch(bool on) {
  if (power_switch_ && !on) {
    int_flags_ |= kFlagPowerButton;
    if (int_enable_ & kFlagPowerButton) cpu_.raise_irq(kIrqPower);
  }
  power_switch_ = on;
}

bool Machine::run_frame() {
  if (powered_off_) {  // no power: nothing runs, the screen is dark
    std::fill(fb_.begin(), fb_.end(), 0xff000000u);
    frames_++;
    lagged_ = true;
    return true;
  }
  const u64 target = next_frame_;
  const bool ok = target > now() ? run_cycles(target - now()) : true;
  if (!ok) return false;
  next_frame_ += kCyclesPerFrame;
  render_frame();
  frames_++;
  lagged_ = input_reads_ == frame_reads_;
  frame_reads_ = input_reads_;
  auto_calibrate_step();
  return true;
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

void Machine::render_frame() {
  const u32 fmt = lcd_[3];
  if (!(fmt >> 31)) return;  // Display disabled: keep the last image.
  const u32 mode = fmt & 0x3fff'ffff;
  const u32 base = kVramBase + (lcd_[1] & 0xffff);
  const u32 stride = lcd_[2];
  auto px = [&](u32 addr) { return bus_.peek8(addr); };

  // The usual case, the whole frame in video RAM: straight from it, with the
  // colours from a table (the same image as below, a few times faster).
  const u32 row_bytes = mode == 4 ? kWidth * 3 / 2 : kWidth;
  const u64 last = u64(base - kVramBase) + u64(stride) * (kHeight - 1) + row_bytes;
  if ((mode == 4 || mode == 3) && last <= kVramSize) {
    const u8* v = vram_.data() + (base - kVramBase);
    if (mode == 4) {
      static const std::array<u32, 4096> kRgb444 = [] {
        std::array<u32, 4096> t{};
        for (u32 i = 0; i < 4096; i++) t[i] = argb(pal4(i >> 8), pal4(i >> 4), pal4(i));
        return t;
      }();
      for (int y = 0; y < kHeight; y++, v += stride) {
        u32* row = &fb_[size_t(y) * kWidth];
        for (int x = 0; x < kWidth / 2; x++) {
          const u32 b0 = v[x * 3], b1 = v[x * 3 + 1], b2 = v[x * 3 + 2];
          row[x * 2] = kRgb444[(b1 >> 4) << 8 | (b0 >> 4) << 4 | (b0 & 15)];
          row[x * 2 + 1] = kRgb444[(b1 & 15) << 8 | (b2 >> 4) << 4 | (b2 & 15)];
        }
      }
    } else {
      static const std::array<u32, 256> kRgb332 = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; i++) t[i] = argb(pal3(i >> 5), pal3(i >> 2), pal2(i));
        return t;
      }();
      for (int y = 0; y < kHeight; y++, v += stride) {
        u32* row = &fb_[size_t(y) * kWidth];
        for (int x = 0; x < kWidth; x++) row[x] = kRgb332[v[x]];
      }
    }
    return;
  }

  if (mode == 4) {
    // 12 bpp: two pixels packed in three bytes.
    for (int y = 0; y < kHeight; y++) {
      u32* row = &fb_[size_t(y) * kWidth];
      for (int x = 0; x < kWidth / 2; x++) {
        const u32 a = base + y * stride + x * 3;
        const u32 b0 = px(a), b1 = px(a + 1), b2 = px(a + 2);
        row[x * 2] = argb(pal4(b1 >> 4), pal4(b0 >> 4), pal4(b0));
        row[x * 2 + 1] = argb(pal4(b1), pal4(b2 >> 4), pal4(b2));
      }
    }
  } else if (mode == 3) {
    // 8 bpp RGB332 (touch calibration screen).
    for (int y = 0; y < kHeight; y++) {
      u32* row = &fb_[size_t(y) * kWidth];
      for (int x = 0; x < kWidth; x++) {
        const u32 v = px(base + y * stride + x);
        row[x] = argb(pal3(v >> 5), pal3(v >> 2), pal2(v));
      }
    }
  } else {
    static u32 warned = 0;
    if (warned != mode) { warned = mode; LOG_W("unimplemented LCD mode %u", mode); }
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool Machine::calibration_blank() const {
  // The v1.5 BIOS stores calibration at 0x00 and a copy at 0x30.
  return std::all_of(sys_eeprom_.begin(), sys_eeprom_.begin() + 0x60, [](u8 b) { return b == 0; }) ||
         std::all_of(sys_eeprom_.begin(), sys_eeprom_.begin() + 0x60, [](u8 b) { return b == 0xff; });
}

void Machine::request_calibration() {
  std::fill(sys_eeprom_.begin(), sys_eeprom_.begin() + 0x60, 0);
}

void Machine::auto_calibrate_step() {
  AutoCal& a = autocal_;
  if (!auto_calibrate || lcd_mode() != 3) {
    if (a.phase != 0) touch_down_ = false;
    a = AutoCal{};
    return;
  }
  switch (a.phase) {
    case 0: {
      // The calibration target is a small white cross on black.
      long sx = 0, sy = 0, n = 0;
      for (int y = 0; y < kHeight; y++)
        for (int x = 0; x < kWidth; x++)
          if ((fb_[size_t(y) * kWidth + x] & 0xffffff) == 0xffffff) { sx += x; sy += y; n++; }
      if (n == 0 || n > 64) return;  // nothing (or not a crosshair) on screen yet
      const int cx = int(sx / n), cy = int(sy / n);
      // Tap a new target right away; re-tap the same one only if it persists.
      if (cx == a.last_x && cy == a.last_y && ++a.same_target_frames < 90) return;
      a.x = cx;
      a.y = cy;
      a.same_target_frames = 0;
      a.phase = 1;
      a.frames = 0;
      set_touch(true, cx, cy);
      return;
    }
    case 1:
      if (++a.frames >= 15) {
        set_touch(false, a.x, a.y);
        a.phase = 2;
        a.frames = 0;
      }
      return;
    default:
      if (++a.frames >= 30) {
        a.last_x = a.x;
        a.last_y = a.y;
        a.phase = 0;
      }
      return;
  }
}

void Machine::set_touch(bool down, int x, int y) {
  touch_down_ = down;
  x = std::clamp(x, 0, kWidth - 1);
  y = std::clamp(y, 0, kHeight - 1);
  // Linear map onto the raw ADC range; the BIOS calibration absorbs the rest.
  touch_x_ = u32(x) * 0x7f0 / (kWidth - 1);
  touch_y_ = u32(y) * 0x7f0 / (kHeight - 1);
}

// ---------------------------------------------------------------------------
// MMIO: 0x0180'0000 - 0x0180'ffff
// ---------------------------------------------------------------------------

u32 Machine::mmio_read(u32 addr, int size) {
  if ((addr & ~0xffffu) == kStubPort) return (addr & 0xffff) == 0x14 ? service_call() : 0;
  const u32 off = addr & 0xffff;
  const u32 a = off & ~3u;
  u32 v = 0;

  if (a >= 0x4000 && a < 0x5000) {
    v = 0;  // Sound registers are write-only as far as is known.
  } else if (a >= 0xd000 && a < 0xd900) {
    v = timer_read(a - 0xd000);
  } else {
    switch (a) {
      case 0x0030: case 0x0034: case 0x003c: v = 0; break;
      case 0x0038: v = eeprom_cell(eeprom_cmd_); break;           // EEPROM data
      case 0x004c:  // GIODataIn: buttons, and the power switch (bit 11, low = off)
        v = ~(buttons_ & kButtonMask) & ~(power_switch_ ? 0u : kGioPowerSwitch);
        input_reads_++;
        break;
      case 0x0080: v = int_flags_ | 0x80; break;                  // IRQ source flags
      case 0x0084: v = int_enable_; break;                        // their interrupt enables
      case 0x0090: case 0x0094: case 0x0098: case 0x009c:
      case 0x00a0: case 0x00a4: case 0x00a8:
        v = adc_read((a - 0x90) / 4);
        break;
      case 0x1000: case 0x1004: case 0x1008: case 0x100c: case 0x1018: v = 0; break;
      case 0x2078: v = snd_triggered(); break;
      case 0x3000: v = pcm_[0].ctrl; break;
      case 0x3004: v = pcm_[0].base | pcm_[0].pos; break;  // (the play position)
      case 0x3008: v = pcm_[1].ctrl; break;
      case 0x300c: v = pcm_[1].base | pcm_[1].pos; break;
      case 0x8084: case 0x8088: case 0x808c: case 0x8090: case 0x8094: case 0x8098:
        v = lcd_[(a - 0x8084) / 4];
        break;
      case 0x8800: case 0x8804: case 0x8808: case 0x880c: case 0x8810: case 0x8814: case 0x8818:
        v = 0;
        break;
      // System configuration straps. See docs/hardware.md for the bit layout;
      // bit 26 = 1 means "no cartridge inserted".
      // MAME-DIFF: bit 14 is set here. When clear, the BIOS always enters the
      // bare factory touch calibration; when set, it uses the calibration
      // saved in the system EEPROM (or shows the user-facing first-boot
      // calibration screen if there is none).
      case 0x9004:
        v = straps | (cart_.empty() ? 0x0400'0000 : 0);
        if (calibration_blank()) v &= ~(1u << 14);  // no calibration stored: run it
        break;
      case 0x9008:
        // CPU power/clock control. The RTOS idle task polls it every pass of
        // its wait loop, which makes the polling instruction a reliable
        // idle-loop hint (see Cpu::idle_skip).
        cpu_.idle_hint = cpu_.pc();
        v = clock_div_;
        break;
      case 0xb000: v = 0; break;
      case 0xb004: v = 0xffff'ffff; break;
      case 0xb008: v = 1; break;
      default:
        LOG_D("unknown I/O read%d %08x", size * 8, addr);
        v = 0;
    }
  }
  if (size == 4) return v;
  const unsigned shift = (off & 3) * 8;
  return (v >> shift) & (size == 2 ? 0xffff : 0xff);
}

void Machine::mmio_write(u32 addr, u32 v, int size) {
  if ((addr & ~0xffffu) == kStubPort) {
    if ((addr & 0xffff) < 0x14) stub_args_[(addr & 0xffff) / 4] = v;
    return;
  }
  const u32 off = addr & 0xffff;

  if (off >= 0x4000 && off < 0x5000) {
    if (size == 4) {
      snd_write16(off - 0x4000, u16(v));
      snd_write16(off - 0x4000 + 2, u16(v >> 16));
    } else {
      snd_write16(off - 0x4000, u16(v));
    }
    return;
  }
  if (size != 4) LOG_D("sub-word I/O write%d %08x = %x", size * 8, addr, v);
  const u32 a = off & ~3u;

  if (a >= 0xd000 && a < 0xd900) { timer_write(a - 0xd000, v); return; }

  switch (a) {
    case 0x0030: eeprom_cmd_ = v; return;
    case 0x0034: if (v == 2) eeprom_cell(eeprom_cmd_) = u8(eeprom_cmd_); return;
    case 0x0080: int_flags_ &= ~v; return;
    case 0x0084: int_enable_ = v; return;
    case 0x0090: case 0x0094: case 0x0098: case 0x009c:
    case 0x00a0: case 0x00a4: case 0x00a8:
      adc_write((a - 0x90) / 4, v);
      return;
    case 0x2070: snd_command(v); return;
    case 0x3000: pcm_write(0, false, v); return;
    case 0x3004: pcm_write(0, true, v); return;
    case 0x3008: pcm_write(1, false, v); return;
    case 0x300c: pcm_write(1, true, v); return;
    case 0x20e0: celp_cb_[0] = u16(v); celp_load_codebook(); return;  // speech codebook page, low half
    case 0x20e4: celp_cb_[1] = u16(v); celp_load_codebook(); return;  // high half
    case 0x8084: case 0x8088: case 0x808c: case 0x8090: case 0x8094: case 0x8098:
      lcd_[(a - 0x8084) / 4] = v;
      return;
    case 0x8800: {  // DMA control: 0x1b starts a framebuffer copy
      if (v == 0x1b) {
        u32 dst = kVramBase + (lcd_[1] & 0xffff) + dma_[4];
        u32 src = dma_[1];
        for (u32 line = 0; line < dma_[3]; line++) {
          for (u32 w = 0; w < dma_[2]; w++) {
            bus_.write32(dst, bus_.read32(src));
            dst += 4;
            src += 4;
          }
        }
        cpu_.raise_irq(kIrqDma);
        dma_count_++;
        capture_.on_dma(dma_[1], dma_[2] * dma_[3] * 4, frames_);
      }
      dma_[0] = v;
      return;
    }
    case 0x8804: case 0x8808: case 0x880c: case 0x8810:
      dma_[(a - 0x8800) / 4] = v;
      return;
    case 0x9008:  // CPU clock divider (not modelled)
      if (v != clock_div_) LOG_D("clock divider write %08x (cycle %llu)", v, static_cast<unsigned long long>(now()));
      clock_div_ = v;
      return;
    case 0xc020: lcd_ctrl_ = u8(v); return;  // LCD controller (Contrast switches it between 0x35 and 0x95; effect unknown)
    case 0xc024:
      // LCD controller level: the brightness setting, 0x30 by default,
      // 0x24-0x3d over the setting's range (boot also writes 0x80 first).
      if (v < 0x40) lcd_brightness_ = u8(v);
      return;
    case 0xb000:
      // Power off: the BaseROM sets bit 8 at the end of its shutdown sequence
      // (power switch off, or automatic power-off), after its animation.
      if (v & 0x100) {
        powered_off_ = true;
        cpu_.request_exit();
      }
      return;
  }
  LOG_D("unknown I/O write%d %08x = %08x", size * 8, addr, v);
}

// ---------------------------------------------------------------------------
// Timers and UART (0x0180'd000 page)
// ---------------------------------------------------------------------------

u32 Machine::timer_count(int i) const {
  const Timer& t = timers_[i];
  return t.ticks_base + u32((now() - t.base_cycle) / kCyclesPerTick);
}

void Machine::timer_set(int i, u32 count) {
  Timer& t = timers_[i];
  t.ticks_base = count;
  t.base_cycle = now();
  const u32 left = count >= t.max ? 0 : t.max - count;
  t.overflow_at = now() + u64(left) * kCyclesPerTick;
  reschedule();
}

u32 Machine::timer_read(u32 off) {
  if (off == 0x514) return 0xa0;  // UART status: TX ready, RX empty
  if (off == 0x510) return 0;     // UART RX data
  const int i = int((off >> 10) & 3);
  if (i > 2) return 0;
  const u32 base = i == 0 ? 0x084 : u32(i) << 10;
  if (off < base || off >= base + 12) { LOG_D("unknown timer-page read %03x", off); return 0; }
  switch ((off - base) >> 2) {
    case 0: return timer_count(i);
    case 1: return timers_[i].control;
    default: return timers_[i].max;
  }
}

void Machine::timer_write(u32 off, u32 v) {
  if (off == 0x510) {  // UART TX: the MQX debug console
    uart_.push_back(char(v));
    if (echo_uart) { std::fputc(int(v & 0xff), stdout); std::fflush(stdout); }
    return;
  }
  if (off == 0x514) {
    // MQX blocks when its output queue fills, so acknowledge transmit requests.
    if (v == 0x44) cpu_.raise_irq(kIrqUart);
    return;
  }
  const int i = int((off >> 10) & 3);
  if (i > 2) return;
  const u32 base = i == 0 ? 0x084 : u32(i) << 10;
  if (off < base || off >= base + 12) { LOG_D("unknown timer-page write %03x = %08x", off, v); return; }
  Timer& t = timers_[i];
  switch ((off - base) >> 2) {
    case 0: timer_set(i, v); break;
    case 1: t.control = v; break;
    default: {
      const u32 cur = timer_count(i);
      t.max = v;
      timer_set(i, cur);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// EEPROM (0x0180'0030): command word = [31:16] address, [15:8] device,
// [7:0] write data. Device 0x26 is the system EEPROM; others the cartridge's.
// ---------------------------------------------------------------------------

u8& Machine::eeprom_cell(u32 cmd) {
  const u32 addr = cmd >> 16;
  if (((cmd >> 8) & 0xff) == 0x26) return sys_eeprom_[addr % sys_eeprom_.size()];
  return cart_eeprom_[addr % cart_eeprom_.size()];
}

// ---------------------------------------------------------------------------
// ADC / touchscreen (0x0180'0090 - 0x0180'00ab), IRQ 0x10
// ---------------------------------------------------------------------------

u32 Machine::adc_read(unsigned reg) {
  switch (reg) {
    case 0: case 1: case 2: case 3: return adc_ctrl_[reg];
    case 4: {
      input_reads_++;
      if (adc_fifo_.empty()) return 0;
      const u32 v = adc_fifo_.front();
      adc_fifo_.pop_front();
      return v;
    }
    case 6: return adc_fifo_.empty() ? 0x100 : 0;
  }
  return 0;
}

void Machine::adc_write(unsigned reg, u32 v) {
  if (reg > 3) return;
  if (reg == 0) {
    const bool enable = v & 0x8000;
    if (enable && !adc_enabled_) {
      if (!touch_initted_) {
        // The BIOS waits for 17 packets after enabling the channel; their
        // contents are ignored.
        adc_irq();
        for (int i = 0; i < 17; i++) adc_push(0, 0);
        touch_initted_ = true;
      }
      adc_next_ = now() + kCyclesPerTouchSample;
      reschedule();
    }
    adc_enabled_ = enable;
  }
  adc_ctrl_[reg] = v;
}

void Machine::adc_push(unsigned channel, u32 data) {
  if (adc_fifo_.size() >= 65536) return;
  adc_fifo_.push_back((channel << 16) | (data << 5));
}

void Machine::adc_sample() {
  // Two groups of six samples; samples 0,1,4,5 behave like pressure (>= 0x7f0
  // means "not touching").
  const u32 p = touch_down_ ? 0 : 0x7ff;
  const u32 seq[12] = {p, p, touch_x_, touch_y_, p, p, p, p, touch_x_, touch_y_, p, p};
  for (u32 s : seq) adc_push(0, s);
  adc_irq();
}

// ---------------------------------------------------------------------------
// Sound (0x0180'4000 registers, 0x0180'2070 command, 0x0180'2078 status)
//   voices 0-4: pitched A-law with loop; 5-6: raw 8 kHz A-law;
//   voice 7: LFC speech (celp.cpp, docs/speech.md)
// ---------------------------------------------------------------------------

void Machine::snd_write16(u32 off, u16 v) {
  auto set_hi = [](u32& r, u16 v) { r = (u32(v) << 16) | (r & 0xffff); };
  auto set_lo = [](u32& r, u16 v) { r = (r & 0xffff0000u) | v; };
  if (off >= 0x0c4 && off < 0x104) {
    Voice& vc = voices_[(off - 0x0c4) / 8];
    const u32 w = (off - 0x0c4) % 8;
    if (w == 0) set_hi(vc.start, v); else if (w == 4) set_lo(vc.start, v);
  } else if (off >= 0x104 && off < 0x13c) {
    Voice& vc = voices_[(off - 0x104) / 8];
    const u32 w = (off - 0x104) % 8;
    if (w == 0) set_hi(vc.end, v); else if (w == 4) set_lo(vc.end, v);
  } else if (off >= 0x13c && off < 0x15c) {
    if ((off - 0x13c) % 4 == 0) voices_[(off - 0x13c) / 4].volume = v;
  } else if (off >= 0x15c && off < 0x170) {
    if ((off - 0x15c) % 4 == 0) voices_[(off - 0x15c) / 4].pitch = v;
  } else if (off == 0x1c4) {
    master_volume_ = v;  // the volume setting, 0x4000 = unity
  } else {
    LOG_D("sound reg write %03x = %04x", off, v);
  }
}

void Machine::snd_command(u32 data) {
  const u32 cmd = data >> 3;
  const unsigned v = data & 7;
  cpu_.raise_irq(kIrqSound);  // The driver waits for this acknowledge.
  Voice& vc = voices_[v];
  if (cmd == 0x10) {
    if (v < 5) {
      vc.step = vc.pitch / 32768.0;  // 1.15 fixed point, bytes per sample
      vc.pos = double(vc.start & 0xffff);
      vc.active = true;
    } else if (v < 7) {
      vc.step = 1.0;
      vc.pos = 0;
      vc.active = true;
    } else {
      celp_.start(vc.start);
    }
  } else {
    vc.active = false;
    if (v == 7) celp_.stop();
  }
}

void Machine::celp_load_codebook() {
  // The register holds the codebook's 32 KiB page; the chip adds 0x900.
  const u32 page = (u32(celp_cb_[1]) << 16) | celp_cb_[0];
  const u32 base = (page << 15) + CelpDecoder::kCodebookPageOffset;
  std::vector<u8> cb(CelpDecoder::kCodebookSize);
  for (size_t i = 0; i < cb.size(); i++) cb[i] = bus_.peek8(base + u32(i));
  celp_.set_codebook(cb.data(), cb.size());
}

u8 Machine::snd_triggered() const {
  u8 r = voices_[0].active ? 0x80 : 0;
  for (int i = 1; i < 7; i++)
    if (voices_[i].active) r |= u8(1u << (i - 1));
  if (celp_.active()) r |= 1u << 6;  // voice 7 (speech) busy
  return r;
}

void Machine::snd_mix_sample() {
  double mix = 0;
  for (int i = 0; i < 5; i++) {
    Voice& vc = voices_[i];
    if (!vc.active) continue;
    const double loop_point = vc.end >> 16, loop_target = vc.end & 0xffff;
    const u32 addr = (vc.start & 0xffff0000u) | u16(u32(vc.pos));
    mix += alaw_to_linear(bus_.peek8(addr)) * 0.125 * (vc.volume / 16384.0);
    vc.pos += vc.step;
    if (vc.pos >= loop_point) {
      if (loop_point - loop_target < 2) vc.active = false;
      else vc.pos = vc.pos - loop_point + loop_target;
    }
  }
  for (int i = 5; i < 7; i++) {
    Voice& vc = voices_[i];
    if (!vc.active) continue;
    mix += alaw_to_linear(bus_.peek8(vc.start + u32(vc.pos))) * 0.125 * (vc.volume / 16384.0);
    vc.pos += vc.step;
    if (vc.start + u32(vc.pos) >= vc.end) vc.active = false;
  }
  if (celp_.active()) {
    // Level relative to the A-law voices is not yet verified on hardware.
    mix += celp_.next_sample([this](u32 a) { return bus_.peek8(a); }) * 0.0625 * (voices_[7].volume / 16384.0);
  }
  const s16 s = s16(std::clamp(mix * 4.0 * (master_volume_ / 16384.0), -32768.0, 32767.0));
  voice_hold_ = s;
  if (!pcm_active())  // (else the PCM ticks output it, mixed)
    for (u32 i = 0; i < kAudioHz / kVoiceHz; i++) audio_out(s);
}

void Machine::audio_out(s16 s) {
  if (audio_.size() >= kAudioHz) audio_.pop_front();
  audio_.push_back(s);
}

// ---------------------------------------------------------------------------
// PCM channels (0x0180'3000-300c)
// ---------------------------------------------------------------------------

void Machine::pcm_write(unsigned ch, bool addr, u32 v) {
  PcmChannel& c = pcm_[ch];
  if (addr) {
    c.base = v & ~0x1ffu;
    c.pos = v & 0x1ff;
    return;
  }
  const bool was = pcm_active();
  c.ctrl = v;
  if (!was && pcm_active()) {  // the 32 kHz clock starts at the next tick
    pcm_next_ = (now() / kCyclesPerPcm + 1) * kCyclesPerPcm;
    reschedule();
  }
}

// One 32 kHz sample: each playing channel reads its next sample; crossing
// into either half of its buffer raises its interrupt flag (bit 20 + channel).
void Machine::pcm_tick() {
  s32 mix = 0;
  for (unsigned i = 0; i < 2; i++) {
    PcmChannel& c = pcm_[i];
    if (!(c.ctrl & 1)) continue;
    mix += s16(bus_.peek16(c.base + c.pos));
    c.pos = (c.pos + 2) & 0x1ff;
    if (c.pos == 0 || c.pos == 0x100) {
      const u32 flag = 1u << (20 + i);
      int_flags_ |= flag;
      if (int_enable_ & flag) cpu_.raise_irq(kIrqPcm);
    }
  }
  audio_out(s16(std::clamp<s32>(voice_hold_ + mix, -32768, 32767)));
}

size_t Machine::read_audio(s16* out, size_t max) {
  size_t n = std::min(max, audio_.size());
  for (size_t i = 0; i < n; i++) {
    out[i] = audio_.front();
    audio_.pop_front();
  }
  return n;
}

}  // namespace leap
