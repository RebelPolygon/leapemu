// leapemu-cli: headless Leapster emulator for testing, tracing and automation.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "core/arc/cpu.h"
#include "core/image.h"
#include "core/interp.h"
#include "core/log.h"
#include "core/assets.h"
#include "core/celp.h"
#include "core/machine.h"
#include "core/movie.h"
#include "core/soundtrack.h"
#include "gui/game_db.h"

using namespace leap;
namespace fs = std::filesystem;

namespace {

void usage() {
  std::puts(
      "usage: leapemu-cli --bios FILE [options]\n"
      "\n"
      "  --bios FILE          BaseROM image (.bin, .zip, or WAV-wrapped dump)\n"
      "  --cart FILE          cartridge image (.bin or .zip)\n"
      "  --frames N           run N video frames (default 60)\n"
      "  --seconds S          run S emulated seconds (overrides --frames)\n"
      "  --screenshot FILE    write the final frame as PNG\n"
      "  --shot-every N       also write FILE-<frame>.png every N frames\n"
      "  --trace FILE         write a per-instruction trace\n"
      "  --trace-format F     'native' (default) or 'mame' (matches tools/oracle traces)\n"
      "  --mame-compat        reproduce MAME quirks and straps for oracle trace diffs\n"
      "  --straps HEX         value of the 0x01809004 strap register (MAME: 63ffbfff)\n"
      "  --trace-limit N      stop tracing after N instructions (default 1000000)\n"
      "  --allow-unsigned     boot cartridges without a LeapFrog signature (homebrew)\n"
      "  --stub-services      give a Leapster 2 download leapemu's own Leapster 2 services\n"
      "                       (the game database does for titles that need them)\n"
      "  --patches FILE       apply a ROM patch file's enabled patches (implies --allow-unsigned)\n"
      "  --nvram DIR          load/save EEPROM contents (calibration, saves) in DIR\n"
      "  --save FILE          load/save the cartridge's save EEPROM as a .sav file\n"
      "  --movie FILE         play an input movie (.lmv) from power-on; runs its length\n"
      "                       unless --frames/--seconds is given\n"
      "  --record-movie FILE  record this run's input (--press/--touch) as a movie\n"
      "  --state-hash         print a CRC-32 of the final machine state (sync checks)\n"
      "  --load-state FILE    load a save state before running\n"
      "  --save-state FILE    write a save state after running\n"
      "  --profile            print a histogram of where the CPU spends its time\n"
      "  --audio-out FILE     write emulated audio (32 kHz mono, 16-bit) to a WAV file\n"
      "  --timing R16,R32,M16,M32,S16,S32,I16,I32  wait states in cycles (ROM, RAM, SRAM, I/O)\n"
      "                       (ROM covers the cartridge too unless --cart-timing is given)\n"
      "  --cache IKB,DKB[,LINE[,WAYS]]  instruction/data cache sizes in KiB (0 = none), line bytes, ways\n"
      "  --cart-timing C16,C32  cartridge ROM wait states if different from the BaseROM\n"
      "  --fill ROM16,RAM16   cache-line fill cost per 16-bit transfer, in cycles\n"
      "  --no-waits           disable the calibrated timing (1 instruction per cycle)\n"
      "  --cpu BACKEND        interpreter | cached | jit (the default on x86-64 and AArch64)\n"
      "  --no-idle-skip       execute the RTOS idle loop instead of skipping it\n"
"  --interp MODE        display interpolation for --interp-dump: off, motion, native\n"
      "  --interp-steps N     --interp-dump: images per emulated frame (default 1)\n"
      "  --interp-scale K     --interp-dump: output scale (native mode renders at K x resolution)\n"
      "  --interp-dump DIR    write the displayed (interpolated) image of every frame as PNG\n"
      "  --uart               echo the debug UART (MQX console) to stdout\n"
      "  --press BTN@F[-F2]   hold a button during frames F..F2 (a,b,up,down,left,right,\n"
      "                       hint,home,pause,volup,voldown,brightup,brightdown,contrast);\n"
      "                       repeatable\n"
      "  --touch X,Y@F[-F2]   touch the screen at X,Y during frames F..F2; repeatable\n"
      "  --power-off F        turn the power switch off at frame F (the BaseROM saves its\n"
      "                       settings, plays its power-off animation and cuts the power)\n"
      "  --break ADDR         stop when execution reaches ADDR (hex); repeatable\n"
      "  --disasm ADDR N      disassemble N instructions at ADDR (hex) and exit\n"
      "  --info               print ROM header information and exit\n"
      "  --export-assets DIR  write the cartridge's assets into DIR (a folder per type) and exit:\n"
      "                       .swf, .wav (speech, A-law) and .mid (SYN music); the rest as stored\n"
      "  --export-raw         with --export-assets: every asset as stored in the ROM\n"
      "  -v / -vv             more logging\n"
      "  --version            the version");
}

u32 parse_hex(const char* s) { return u32(std::strtoul(s, nullptr, 16)); }

struct Press {
  u32 mask = 0;
  bool touch = false;
  int x = 0, y = 0;
  u64 from = 0, to = 0;
};

bool parse_range(const char* s, u64* from, u64* to) {
  const char* at = std::strchr(s, '@');
  if (!at) return false;
  *from = std::strtoull(at + 1, nullptr, 10);
  const char* dash = std::strchr(at, '-');
  *to = dash ? std::strtoull(dash + 1, nullptr, 10) : *from + 5;
  return true;
}

u32 button_from_name(const std::string& n) {
  if (n == "a") return kBtnA;
  if (n == "b") return kBtnB;
  if (n == "up") return kBtnUp;
  if (n == "down") return kBtnDown;
  if (n == "left") return kBtnLeft;
  if (n == "right") return kBtnRight;
  if (n == "hint") return kBtnHint;
  if (n == "home") return kBtnHome;
  if (n == "pause") return kBtnPause;
  if (n == "volup") return kBtnVolUp;
  if (n == "voldown") return kBtnVolDown;
  if (n == "brightup") return kBtnBrightUp;
  if (n == "brightdown") return kBtnBrightDown;
  if (n == "contrast") return kBtnContrast;
  return 0;
}

void print_header(const char* what, const RomHeader& h, u32 crc) {
  std::printf("%s: crc32=%08x", what, crc);
  if (!h.valid) { std::puts(" (no LeapFrog header)"); return; }
  std::printf("  mapped %08x-%08x  RIB @%08x\n", h.device_start, h.device_end, h.rib_table);
  if (!h.title.empty()) std::printf("  title:     %s\n", h.title.c_str());
  if (!h.part_number.empty()) std::printf("  part:      %s\n", h.part_number.c_str());
  if (!h.version.empty()) std::printf("  version:   %s\n", h.version.c_str());
  if (!h.build_date.empty()) std::printf("  built:     %s\n", h.build_date.c_str());
  if (!h.copyright.empty()) std::printf("  copyright: %s\n", h.copyright.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::string bios, cart, shot, trace_path, nvram, load_state, save_state, audio_out, interp_dump;
  bool outputs_ok = true;  // every file asked for was written (the exit status)
  std::string save_file, movie_path, record_path;
  bool state_hash = false, frames_given = false;
  FrameInterpolator interp;
  int interp_steps = 1, interp_scale = 1;
  std::vector<NativeLayer> dump_layers;
  u64 frames = 60, trace_limit = 1'000'000, shot_every = 0;
  double seconds = 0;
  std::string patches_path, export_dir;
  bool export_raw = false;
  bool uart = false, info = false, mame_trace = false, profile = false, no_idle_skip = false;
  auto backend = arc::Cpu::backend_available(arc::Cpu::Backend::Jit) ? arc::Cpu::Backend::Jit : arc::Cpu::Backend::CachedInterpreter;
  bool timing_set = false, no_waits = false;
  float timing_v[8] = {};
  int cache_v[4] = {-1, -1, -1, -1};
  float fill_v[2] = {-1, -1};
  float cart_v[2] = {-1, -1};
  u32 straps = 0;
  bool straps_set = false, mame_compat = false, allow_unsigned = false, stub_services = false;
  u32 dis_addr = 0, dis_count = 0;
  std::vector<Press> presses;
  s64 power_off_at = -1;  // frame at which the power switch is turned off
  std::vector<u32> breaks;

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) { std::fprintf(stderr, "%s needs an argument\n", a.c_str()); std::exit(2); }
      return argv[++i];
    };
    // A number, or an error (not a silent 0).
    auto number = [&]() -> double {
      const char* v = next();
      char* end = nullptr;
      const double d = std::strtod(v, &end);
      if (end == v || *end) { std::fprintf(stderr, "%s needs a number, not \"%s\"\n", a.c_str(), v); std::exit(2); }
      return d;
    };
    if (a == "--bios") bios = next();
    else if (a == "--cart") cart = next();
    else if (a == "--frames") { frames = u64(std::max(0.0, number())); frames_given = true; }
    else if (a == "--seconds") { seconds = std::max(0.0, number()); frames_given = true; }
    else if (a == "--save") save_file = next();
    else if (a == "--movie") movie_path = next();
    else if (a == "--record-movie") record_path = next();
    else if (a == "--state-hash") state_hash = true;
    else if (a == "--power-off") power_off_at = s64(number());
    else if (a == "--screenshot") shot = next();
    else if (a == "--shot-every") shot_every = u64(std::max(0.0, number()));
    else if (a == "--trace") trace_path = next();
    else if (a == "--trace-format") mame_trace = std::string(next()) == "mame";
    else if (a == "--mame-compat") mame_compat = true;
    else if (a == "--no-waits") no_waits = true;
    else if (a == "--straps") { straps = parse_hex(next()); straps_set = true; }
    else if (a == "--trace-limit") trace_limit = u64(std::max(0.0, number()));
    else if (a == "--uart") uart = true;
    else if (a == "--nvram") nvram = next();
    else if (a == "--allow-unsigned") allow_unsigned = true;
    else if (a == "--stub-services") stub_services = true;
    else if (a == "--audio-out") audio_out = next();
    else if (a == "--interp-dump") interp_dump = next();
    else if (a == "--interp") {
      const std::string mode = next();
      interp.set_mode(mode == "native"   ? FrameInterpolator::Mode::Native
                      : mode == "motion" ? FrameInterpolator::Mode::Motion
                                         : FrameInterpolator::Mode::Off);
    }
    else if (a == "--interp-steps") interp_steps = std::max(1, int(number()));
    else if (a == "--interp-scale") interp_scale = std::clamp(int(number()), 1, 8);
    else if (a == "--load-state") load_state = next();
    else if (a == "--save-state") save_state = next();
    else if (a == "--info") info = true;
    else if (a == "--patches" && i + 1 < argc) patches_path = argv[++i];
    else if (a == "--export-assets" && i + 1 < argc) export_dir = argv[++i];
    else if (a == "--export-raw") export_raw = true;
    else if (a == "--profile") profile = true;
    else if (a == "--no-idle-skip") no_idle_skip = true;
    else if (a == "--timing") {
      float v[8] = {};
      if (std::sscanf(next(), "%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 8) {
        std::fprintf(stderr, "--timing needs 8 comma-separated values\n");
        return 2;
      }
      timing_set = true;
      for (int k = 0; k < 8; k++) timing_v[k] = v[k];
    } else if (a == "--cache") {
      const int n = std::sscanf(next(), "%d,%d,%d,%d", &cache_v[0], &cache_v[1], &cache_v[2], &cache_v[3]);
      auto pow2 = [](int x) { return x > 0 && (x & (x - 1)) == 0; };
      if (n < 2 || cache_v[0] < 0 || cache_v[1] < 0 || (n >= 3 && (!pow2(cache_v[2]) || cache_v[2] < 4)) ||
          (n >= 4 && (cache_v[3] < 1 || cache_v[3] > 8))) {
        std::fprintf(stderr, "--cache needs IKB,DKB[,LINE[,WAYS]] (line a power of two >= 4, 1-8 ways)\n");
        return 2;
      }
    } else if (a == "--cart-timing") {
      if (std::sscanf(next(), "%f,%f", &cart_v[0], &cart_v[1]) != 2) {
        std::fprintf(stderr, "--cart-timing needs C16,C32\n");
        return 2;
      }
    } else if (a == "--fill") {
      if (std::sscanf(next(), "%f,%f", &fill_v[0], &fill_v[1]) != 2) {
        std::fprintf(stderr, "--fill needs ROM16,RAM16\n");
        return 2;
      }
    } else if (a == "--cpu") {
      const std::string b = next();
      if (b == "interpreter" || b == "interp") backend = arc::Cpu::Backend::Interpreter;
      else if (b == "cached") backend = arc::Cpu::Backend::CachedInterpreter;
      else if (b == "jit") {
        if (arc::Cpu::backend_available(arc::Cpu::Backend::Jit)) backend = arc::Cpu::Backend::Jit;
        else std::fprintf(stderr, "the JIT recompiler is not available on this host; using cached\n");
      }
      else { std::fprintf(stderr, "unknown --cpu %s\n", b.c_str()); return 2; }
    }
    else if (a == "--break") breaks.push_back(parse_hex(next()));
    else if (a == "--disasm") { dis_addr = parse_hex(next()); dis_count = u32(std::strtoul(next(), nullptr, 10)); }
    else if (a == "--press") {
      const std::string s = next();
      Press p;
      p.mask = button_from_name(s.substr(0, s.find('@')));
      if (!p.mask || !parse_range(s.c_str(), &p.from, &p.to)) { std::fprintf(stderr, "bad --press %s\n", s.c_str()); return 2; }
      presses.push_back(p);
    } else if (a == "--touch") {
      const std::string s = next();
      Press p;
      p.touch = true;
      if (std::sscanf(s.c_str(), "%d,%d", &p.x, &p.y) != 2 || !parse_range(s.c_str(), &p.from, &p.to)) {
        std::fprintf(stderr, "bad --touch %s\n", s.c_str());
        return 2;
      }
      presses.push_back(p);
    }
    else if (a == "-v") Log::level = LogLevel::Debug;
    else if (a == "-vv") Log::level = LogLevel::Trace;
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (a == "--version") { std::printf("leapemu-cli %s\n", LEAPEMU_VERSION); return 0; }
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
  }
  if (bios.empty()) { usage(); return 2; }

  Machine m;
  std::string err;
  if (!m.load_bios(bios, &err)) { std::fprintf(stderr, "bios: %s\n", err.c_str()); return 1; }
  if (!patches_path.empty()) {
    PatchFile pf;
    if (!pf.load(patches_path, &err)) { std::fprintf(stderr, "patches: %s\n", err.c_str()); return 1; }
    m.cart_patches = [pf](u32 crc) { return crc == pf.cart_crc ? pf.enabled_edits() : std::vector<RomEdit>(); };
  }
  if (!cart.empty() && !m.load_cart(cart, &err)) { std::fprintf(stderr, "cart: %s\n", err.c_str()); return 1; }
  if (mame_compat) {
    m.cpu().mame_compat = true;
    m.straps = 0x63ff'bfff;
  }
  if (straps_set) m.straps = straps;
  // (downloadables are not signed as cartridges; patched images fail the digest)
  // (and the database's compatibility fixes for known images, as in the GUI)
  m.allow_unsigned = allow_unsigned || m.downloadable() || m.cart_patch().applied ||
                     (m.has_cart() && game_db_has_flag(m.cart_crc(), "unsigned"));
  m.stub_missing_services = stub_services || (m.has_cart() && game_db_has_flag(m.cart_crc(), "stub-services"));
  if (no_idle_skip) m.cpu().idle_skip = false;
  m.cpu().set_backend(backend);
  if (timing_set) {
    auto& t = m.timing;
    t.rom16 = timing_v[0]; t.rom32 = timing_v[1]; t.ram16 = timing_v[2]; t.ram32 = timing_v[3];
    t.sram16 = timing_v[4]; t.sram32 = timing_v[5]; t.io16 = timing_v[6]; t.io32 = timing_v[7];
    t.cart16 = t.cart32 = -1;  // cartridge ROM as BaseROM unless --cart-timing
  }
  {
    auto& t = m.timing;
    if (cache_v[0] >= 0) { t.icache_kb = u32(cache_v[0]); t.dcache_kb = u32(cache_v[1]); }
    if (cache_v[2] > 0) t.line_bytes = u32(cache_v[2]);
    if (cache_v[3] > 0) t.ways = u32(cache_v[3]);
    if (fill_v[0] >= 0) { t.rom_fill16 = fill_v[0]; t.ram_fill16 = fill_v[1]; }
    if (cart_v[0] >= 0) { t.cart16 = cart_v[0]; t.cart32 = cart_v[1]; }
    // The cache geometry must divide into whole sets.
    for (u32 kb : {t.icache_kb, t.dcache_kb})
      if (kb && (kb * 1024) % (t.line_bytes * t.ways)) {
        std::fprintf(stderr, "cache of %u KiB does not divide into %u-byte lines x %u ways\n", kb, t.line_bytes, t.ways);
        return 2;
      }
  }
  if (mame_compat || no_waits) m.timing = Machine::Timing::none();
  m.apply_timing();
  m.reset();
  m.set_native_capture(interp.mode() == FrameInterpolator::Mode::Native);
  {
    const flash::RomView rom = m.rom_view();
    interp.set_rom(rom);
  }
  if (!nvram.empty()) m.load_nvram(nvram);
  if (!save_file.empty()) m.load_cart_save(save_file);
  m.echo_uart = uart;
  if (!load_state.empty() && !m.load_state_file(load_state, &err)) {
    std::fprintf(stderr, "load state: %s\n", err.c_str());
    return 1;
  }
  // Movies start at power-on with the settings and EEPROM contents they record.
  Movie movie, recording;
  const bool playing = !movie_path.empty(), recording_on = !record_path.empty();
  if ((playing || recording_on) && !load_state.empty()) {
    std::fprintf(stderr, "movies start at power-on: --load-state cannot be combined with --movie/--record-movie\n");
    return 2;
  }
  if (playing) {
    if (!movie.load(movie_path, &err) || !movie.start(m, &err)) { std::fprintf(stderr, "movie: %s\n", err.c_str()); return 1; }
    if (!frames_given) frames = movie.frames.size();
  }
  if (recording_on) {
    recording = playing ? movie : Movie::from_machine(m);
    recording.frames.clear();
    if (!playing && !recording.start(m, &err)) { std::fprintf(stderr, "movie: %s\n", err.c_str()); return 1; }
  }

  if (!export_dir.empty()) {
    std::vector<u8> img;
    if (cart.empty() || !load_rom_file(cart, &img, &err)) { std::fprintf(stderr, "export: needs --cart\n"); return 1; }
    join_rom_blocks(&img);
    CelpDecoder speech;  // (the codebook is in the BaseROM)
    const std::vector<u8>& b = m.bios_original();
    if (b.size() >= CelpDecoder::kCodebookPageOffset + CelpDecoder::kCodebookSize)
      speech.set_codebook(&b[CelpDecoder::kCodebookPageOffset], CelpDecoder::kCodebookSize);
    unsigned written = 0, failed = 0;
    const std::vector<RomAsset> assets = list_rom_contents(img).assets;
    // Soundtracks: the cartridge's own codec, on a machine of their own.
    SoundtrackDecoder soundtracks;
    bool have_soundtracks = false;
    if (!export_raw && std::any_of(assets.begin(), assets.end(), [](const RomAsset& a) { return a.type == 0xf; })) {
      std::string serr;
      have_soundtracks = soundtracks.open(m.bios_original(), img, &serr);
      if (!have_soundtracks) std::fprintf(stderr, "soundtracks: %s\n", serr.c_str());
    }
    for (const RomAsset& a : assets)
      (export_asset(img, a, &speech, export_raw, fs::path(cart).stem().string(),
                    asset_folder(export_dir, a.type) / asset_file_name(img, a, export_raw), have_soundtracks ? &soundtracks : nullptr)
           ? written
           : failed)++;
    std::printf("%u assets written to %s%s\n", written, export_dir.c_str(), failed ? (" (" + std::to_string(failed) + " failed)").c_str() : "");
    return failed ? 1 : 0;
  }
  if (info) {
    print_header("bios", m.bios_header(), m.bios_crc());
    if (!m.bios_problem().empty()) std::printf("bios problem: %s\n", m.bios_problem().c_str());
    if (m.has_cart()) {
      print_header("cart", m.cart_header(), m.cart_crc());
      if (m.cart_patch().applied || m.cart_patch().mismatched)
        std::printf("cart patches: %u edits applied, %u not matching (%08x)\n", m.cart_patch().applied, m.cart_patch().mismatched,
                    m.cart_patch().crc);
      std::vector<u8> img;
      if (load_rom_file(cart, &img, &err)) {
        if (const unsigned n = join_rom_blocks(&img)) std::printf("cart image: development upload, %u block header%s removed\n", n, n == 1 ? "" : "s");
        const RomContents c = list_rom_contents(img);
        for (const RomGroup& g : c.groups) {
          const char* n = rom_group_name(g.id);
          std::printf("cart group %04x %-18s %4u entries at %08x\n", g.id, n ? n : "?", g.count, g.addr);
        }
        std::map<u16, unsigned> per_type;
        for (const RomAsset& a : c.assets) per_type[a.type]++;
        for (const auto& [t, n] : per_type) {
          const char* name = rom_asset_type_name(t);
          std::printf("cart assets %2x %-14s %5u\n", t, name ? name : "?", n);
        }
      }
    }
    return 0;
  }

  if (dis_count) {
    auto fetch = [&](u32 a) { return m.bus().peek16(a); };
    u32 pc = dis_addr;
    for (u32 n = 0; n < dis_count; n++) {
      unsigned len = 2;
      const std::string text = arc::disassemble(pc, fetch, &len);
      std::printf("%08x: %s\n", pc, text.c_str());
      pc += len;
    }
    return 0;
  }

  FILE* trace = nullptr;
  u64 traced = 0;
  if (!trace_path.empty()) {
    trace = std::fopen(trace_path.c_str(), "w");
    if (!trace) { std::fprintf(stderr, "cannot open %s\n", trace_path.c_str()); return 1; }
    auto fetch = [&](u32 a) { return m.bus().peek16(a); };
    m.cpu().trace_hook = [&, fetch](const arc::Cpu& c) {
      if (traced >= trace_limit) return;
      traced++;
      unsigned len = 2;
      const std::string text = arc::disassemble(c.pc(), fetch, &len);
      if (mame_trace) {
        // PC STATUS32 LP_COUNT LP_START LP_END r0..r31 | disasm
        std::fprintf(trace, "%08X %08X %08X %08X %08X", c.pc(), c.status32(), c.reg(arc::Cpu::kLP_COUNT),
                     c.lp_start(), c.lp_end());
        for (unsigned r = 0; r < 32; r++) std::fprintf(trace, " %08X", c.reg(r));
        std::fprintf(trace, " | %08X: %s\n", c.pc(), text.c_str());
        return;
      }
      std::fprintf(trace, "%08x %-32s", c.pc(), text.c_str());
      for (unsigned r = 0; r < 32; r++) std::fprintf(trace, " %08x", c.reg(r));
      std::fprintf(trace, " st=%08x lp=%08x\n", c.status32(), c.reg(arc::Cpu::kLP_COUNT));
    };
  }

  std::map<u32, u64> hist;
  u64 samples = 0;
  if (profile && !trace) {
    m.cpu().trace_hook = [&](const arc::Cpu& c) {
      if (++samples % 97 == 0) hist[c.pc()]++;
    };
  }

  for (u32 b : breaks) m.cpu().breakpoints().insert(b);

  if (seconds > 0) frames = u64(seconds * Machine::kFps + 0.5);
  bool ok = true;
  std::vector<s16> audio;
  u64 lag_frames = 0;
  for (u64 f = 0; f < frames; f++) {
    InputFrame in;
    if (playing && f < movie.frames.size()) {
      in = movie.frames[f];
    } else {
      for (const Press& p : presses) {
        if (f < p.from || f > p.to) continue;
        if (p.touch) { in.touch = true; in.x = u8(std::clamp(p.x, 0, Machine::kWidth - 1)); in.y = u8(std::clamp(p.y, 0, Machine::kHeight - 1)); }
        else in.buttons |= p.mask;
      }
    }
    if (!(playing && f < movie.frames.size())) in.power_off = power_off_at >= 0 && s64(f) >= power_off_at;
    apply_input(m, in);
    if (recording_on) recording.frames.push_back(in);

    if (!m.run_frame()) { ok = false; break; }
    lag_frames += m.lagged();
    if (!interp_dump.empty()) {
      interp.push(m.framebuffer(), m.frame_count(), m.draw_capture().latest());
      for (int k = 0; k < interp_steps; k++) {
        const double t = double(m.frame_count()) + double(k) / interp_steps;
        std::vector<u32> big;
        const int W = Machine::kWidth * interp_scale, H = Machine::kHeight * interp_scale;
        if (interp.layers(t, interp_scale, dump_layers)) {
          composite_layers(dump_layers, interp_scale, big);
        } else if (const u32* img = interp.render(t)) {
          big.resize(size_t(W) * H);
          for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) big[size_t(y) * W + x] = img[size_t(y / interp_scale) * Machine::kWidth + x / interp_scale];
        } else {
          continue;
        }
        char name[600];
        std::snprintf(name, sizeof(name), "%s/f-%05llu-%02d.png", interp_dump.c_str(), static_cast<unsigned long long>(f), k);
        write_png(name, big.data(), W, H);
      }
    }
    {
      s16 buf[512];
      size_t n;
      while ((n = m.read_audio(buf, 512)) > 0)
        if (!audio_out.empty()) audio.insert(audio.end(), buf, buf + n);
    }
    if (shot_every && !shot.empty() && f % shot_every == 0) {
      char name[512];
      std::snprintf(name, sizeof(name), "%s-%05llu.png", shot.c_str(), static_cast<unsigned long long>(f));
      if (!write_png(name, m.framebuffer(), Machine::kWidth, Machine::kHeight) && outputs_ok) {
        std::fprintf(stderr, "cannot write %s\n", name);
        outputs_ok = false;
      }
    }
  }
  if (trace) std::fclose(trace);
  if (!audio_out.empty()) {
    std::ofstream w(audio_out, std::ios::binary);
    auto u32le = [&](u32 v) { w.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16le = [&](u16 v) { w.write(reinterpret_cast<const char*>(&v), 2); };
    const u32 bytes = u32(audio.size() * 2);
    w.write("RIFF", 4); u32le(36 + bytes); w.write("WAVEfmt ", 8); u32le(16); u16le(1); u16le(1);
    u32le(Machine::kAudioHz); u32le(Machine::kAudioHz * 2); u16le(2); u16le(16);
    w.write("data", 4); u32le(bytes);
    w.write(reinterpret_cast<const char*>(audio.data()), bytes);
    w.close();
    if (!w) { std::fprintf(stderr, "cannot write %s\n", audio_out.c_str()); outputs_ok = false; }
  }
  if (!save_state.empty() && !m.save_state_file(save_state, &err)) { std::fprintf(stderr, "save state: %s\n", err.c_str()); outputs_ok = false; }
  if (!nvram.empty() && !m.save_nvram(nvram)) { std::fprintf(stderr, "cannot save nvram to %s\n", nvram.c_str()); outputs_ok = false; }
  if (!save_file.empty() && !m.save_cart_save(save_file)) { std::fprintf(stderr, "cannot write %s\n", save_file.c_str()); outputs_ok = false; }
  if (recording_on && !recording.save(record_path, &err)) { std::fprintf(stderr, "movie: %s\n", err.c_str()); outputs_ok = false; }
  if (playing || recording_on)
    std::printf("movie: %llu frames, %llu lag frames\n", static_cast<unsigned long long>(m.frame_count()),
                static_cast<unsigned long long>(lag_frames));
  if (state_hash) {
    const std::vector<u8> st = m.save_state();
    std::printf("state crc32 %08x at frame %llu\n", crc32(st.data(), st.size()), static_cast<unsigned long long>(m.frame_count()));
  }

  if (profile) {
    std::vector<std::pair<u64, u32>> top;
    u64 total = 0;
    for (auto& [pc, n] : hist) { top.push_back({n, pc}); total += n; }
    std::sort(top.rbegin(), top.rend());
    auto fetch = [&](u32 a) { return m.bus().peek16(a); };
    std::printf("top instructions by sampled execution count:\n");
    for (size_t i = 0; i < std::min<size_t>(top.size(), 30); i++) {
      unsigned len;
      std::printf("  %5.2f%%  %08x  %s\n", 100.0 * top[i].first / total, top[i].second,
                  arc::disassemble(top[i].second, fetch, &len).c_str());
    }
  }
  const auto& cpu = m.cpu();
  std::fprintf(stderr, "cpu busy %.1f%% of emulated time\n",
               100.0 * double(cpu.cycles() - cpu.idle_cycles()) / double(std::max<u64>(cpu.cycles(), 1)));
  std::fprintf(stderr, "stopped after %llu frames, %llu cycles, pc=%08x%s\n",
               static_cast<unsigned long long>(m.frame_count()),
               static_cast<unsigned long long>(m.cycles()), cpu.pc(), cpu.sleeping() ? " (sleeping)" : "");
  if (m.dma_count())
    std::fprintf(stderr, "framebuffer DMAs: %llu (%.2f per second of emulated time)\n",
                 static_cast<unsigned long long>(m.dma_count()), double(m.dma_count()) * Machine::kFps / double(std::max<u64>(frames, 1)));
  if (!ok) {
    const char* why = cpu.stop_reason() == arc::Cpu::Stop::Breakpoint ? "breakpoint"
                      : cpu.stop_reason() == arc::Cpu::Stop::Halted  ? "halted"
                                                                      : "fault";
    std::fprintf(stderr, "cpu %s: %s\n", why, cpu.stop_message().c_str());
  }
  if (!uart && !m.uart_output().empty())
    std::fprintf(stderr, "uart output (%zu bytes); rerun with --uart to see it\n", m.uart_output().size());
  if (!shot.empty() && !write_png(shot, m.framebuffer(), Machine::kWidth, Machine::kHeight)) {
    std::fprintf(stderr, "cannot write %s\n", shot.c_str());
    outputs_ok = false;
  }
  // Exit status: 1 if a file asked for could not be written, 3 if the CPU
  // stopped (fault, halt, breakpoint), else 0.
  if (!outputs_ok) return 1;
  return ok ? 0 : 3;
}
