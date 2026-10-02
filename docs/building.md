# Building and development

## Building

Requires CMake ≥ 3.24 and a C++20 compiler. The GUI also needs SDL3: the system package
is used if present, otherwise it is fetched automatically. Dear ImGui is always fetched.

```sh
cmake -S . -B build -G Ninja   # (-G Ninja is optional: any CMake generator works)
cmake --build build
ctest --test-dir build         # CPU and display unit tests
```

The first configure needs git and network access to fetch Dear ImGui (and SDL3 when
the system has none).

**Packages.** With the system's SDL3, nothing else is needed:

```sh
sudo pacman -S cmake ninja git sdl3                          # Arch Linux
sudo dnf install cmake ninja-build gcc-c++ git SDL3-devel   # Fedora
```

Where the distribution has no SDL3 (Debian 12, Ubuntu 24.04 and older), it is built from
source, which needs the development files of what it supports (the list the release
builds use):

```sh
sudo apt install cmake ninja-build g++ git pkg-config libx11-dev libxext-dev \
  libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
  libwayland-dev libxkbcommon-dev wayland-protocols libegl1-mesa-dev libgl1-mesa-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libdecor-0-dev libdbus-1-dev libudev-dev
```

**Multi-configuration generators** (Visual Studio, Xcode, Ninja Multi-Config) choose
the configuration when building, and put the programs in a folder per configuration:
`cmake --build build --config Release`, `ctest --test-dir build -C Release`, and the
programs in `build/Release/`.

Builds: `build/leapemu` (GUI), `build/leapemu-cli` (headless), and the tests.
Options: `-DLEAPEMU_BUILD_GUI=OFF` builds only the core and CLI (the core has no
third-party dependencies); `-DLEAPEMU_BUILD_TOOLS=ON` adds the development tools
([tools/README.md](../tools/README.md)). `cmake --install build` installs the programs
(and, on Linux, a desktop entry and the icon).

**64-bit ARM** (Apple silicon, Raspberry Pi with a 64-bit OS, Windows on ARM, Linux on
ARM) builds the same way and has its own JIT. To cross-compile for ARM Linux from x86,
and to run the result there with qemu-user (Arch: `aarch64-linux-gnu-gcc` and
`qemu-user`; Debian and Ubuntu: `g++-aarch64-linux-gnu` and `qemu-user`; the `-L`
path below is where the cross C library is, `/usr/aarch64-linux-gnu` on both):

```sh
cmake -S . -B build-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake -DLEAPEMU_BUILD_GUI=OFF
cmake --build build-arm64
qemu-aarch64 -L /usr/aarch64-linux-gnu build-arm64/leapemu-cli --bios BIOS --cart GAME --state-hash
```

Results are identical on both architectures: the same input gives the same machine
state, byte for byte.

The tests also run in continuous integration (`.github/workflows/build.yml`): a build of
the core, CLI, tests and tools without third-party dependencies, and one with the GUI.

**Windows** (64-bit x86) is cross-compiled from Linux with MinGW-w64 (Arch:
`mingw-w64-gcc`; Debian and Ubuntu: `g++-mingw-w64-x86-64`); Windows on ARM and native
Windows compilers are untested. **macOS** should build from source as
above (with its own JIT for Apple silicon), but no build is tested or provided.

```sh
cmake -S . -B build-win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-w64-mingw32.cmake -DLEAPEMU_VENDOR_SDL=ON
cmake --build build-win64
```

`-DLEAPEMU_VENDOR_SDL=ON` builds SDL3 from source and links it statically, even if
the system has its own.

### Release builds

`tools/release/build.sh` (run from the tree's root) builds the release packages into
`.release/`, with their SHA-256 sums:
- **Linux x86-64 and arm64**, portable: built in an Ubuntu 22.04 container (podman
  or docker; `tools/release/Containerfile`), so they need only glibc 2.35 or newer.
  SDL3 and the C++ runtime are linked statically; SDL3 loads the system's X11 or
  Wayland and audio libraries when it runs.
  - Each is a `.tar.gz`: a folder with `leapemu` and `leapemu-cli`, which run from
    where they are extracted.
  - `.release/PKGBUILD` (`tools/release/PKGBUILD`, with the version and the
    archives' checksums filled in) packages a downloaded archive for Arch Linux and
    Arch Linux ARM: next to the archive, `makepkg -si` builds and installs
    `leapemu-bin`.
- **Windows x86-64**: cross-compiled with MinGW-w64 and linked statically (no DLLs
  beyond Windows' own), as a `.zip`. Its tests run under Wine when it is installed.

`tools/release/build.sh windows-x86_64` (or `linux-x86_64`, `linux-arm64`) builds one.

## Source layout

The core (`src/core/`) is plain C++20 with no third-party dependencies, so the GUI,
the CLI, the tests and the tools all share it.

| Path | What it is |
|---|---|
| `src/core/arc/cpu.*` | The ARCompact CPU: registers, STATUS32, interrupts, zero-overhead loops, delay slots; the reference interpreter |
| `src/core/arc/decode.cpp` | The cached interpreter: ROM code decoded once into handler tables, run in a tight loop |
| `src/core/arc/jit*.cpp`, `x64.h`, `a64.h`, `jit.h` | The JIT: a shared front end (blocks, linking, executable memory) and code generators for x86-64 and AArch64 with their own emitters ([jit.md](jit.md)) |
| `src/core/arc/xy.*` | The XY-memory DSP option (dual multiply-accumulate, address generators, burst DMA) that Schoolhouse Rock's sound mixer uses ([hardware.md](hardware.md)) |
| `src/core/arc/cache.h` | An experimental timing-only cache model, off by default ([timing.md](timing.md)) |
| `src/core/arc/disasm.cpp` | The disassembler (debugger, traces, `--disasm`) |
| `src/core/bus.*` | The paged physical address space: host pointers for memory, handlers for I/O, wait states per region |
| `src/core/machine.*` | The SoC and the console: memory map, timers, LCD and its DMA, touch ADC, EEPROMs, UART, sound voices and PCM channels, buttons, power; idle-loop skipping; save states; the in-memory fixes (unsigned cartridges, development uploads, Leapster 2 downloads and services) |
| `src/core/celp.*` | The LFC speech decoder (voice 7, [speech.md](speech.md)) |
| `src/core/syn.*`, `soundtrack.*` | SYN music (to MIDI) and Schoolhouse Rock's soundtracks (decoded by running the game's own codec) |
| `src/core/rom.*`, `assets.*`, `image.*` | ROM images (zip, WAV-wrapped, development uploads), the header and resource index, asset listing and export, PNG/BMP writing |
| `src/core/patch.*` | ROM patches kept apart from the image ([rom-tools.md](rom-tools.md)) |
| `src/core/state.h`, `rewind.*`, `movie.*` | Save-state serialization, the rewind history (XOR-delta snapshots), input movies ([tas.md](tas.md)) |
| `src/core/codesig.*` | Finding a ROM's routines by code signature: a length and a hash, never copied bytes |
| `src/core/drawcap.*` | Native draw capture: hooks on the games' own drawing (tile planes, the Cars races, the Flash player) and the layers interpolation draws ([interpolation.md](interpolation.md)) |
| `src/core/interp.*` | Frame interpolation: the playback clock for captured frames, and pixel-motion estimation for everything else |
| `src/core/flash/` | The Flash redraw: SWF parsing, timelines, JPEG, an anti-aliased rasterizer, and the renderer that redraws the player's display list at any scale |
| `src/core/scale.*` | Display filters (bicubic, Lanczos, MMPX, LCD) |
| `src/cli/main.cpp` | `leapemu-cli` |
| `src/gui/` | `leapemu`: the SDL3 + Dear ImGui frontend (`main.cpp`), the game list, the ROM window; `game_db.h` and `icon.h` are generated from `res/` |
| `res/` | The game database (`games.tsv`, and `make_game_db.py`, which also writes [compatibility.md](compatibility.md)), the icon and its generator, the desktop entry, Windows resources |
| `tests/` | Unit tests: CPU instruction semantics (hand-encoded programs), and the display path (capture, the Flash renderer, interpolation timing) |
| `tools/` | Development tools ([tools/README.md](../tools/README.md)): debugging and checking, the clean-room analysis tools, speech, the MAME oracle, timing calibration, release builds |
| `homebrew/` | A small SDK and example cartridge ([homebrew.md](homebrew.md)) |
| `cmake/` | Toolchain files for cross-compiling (64-bit ARM Linux, Windows) |

## How correctness is checked

Most of leapemu's behaviour can be checked exactly, because emulation is
deterministic: the same files, settings and input give the same machine state, byte
for byte (`leapemu-cli --state-hash`). The checks build on that.

- **Unit tests** (`ctest`): CPU instruction semantics on hand-encoded programs
  (flags, carries, delay slots, zero-overhead loops, addressing modes, interrupt
  entry and return), and the display path (capture, the Flash renderer, interpolation
  timing). They run in CI, and in the release builds for Linux x86-64 and for Windows
  (under Wine, when it is installed); the arm64 release is cross-compiled, so its tests
  are not run there.
- **The three CPU backends agree.** `tools/debug/backend_diff` compares the two
  interpreters frame by frame; `tools/debug/jit_diff` runs the JIT and the cached
  interpreter in lockstep and bisects a difference to the exact cycle. Every
  cartridge and download, booted with input for 30 s, gives the same state hash on
  all three.
- **The builds agree.** The Linux x86-64, Linux arm64 (under qemu) and Windows
  (under Wine) builds give the same state hash for the same run, with every backend
  each supports.
- **Against MAME** (below): instruction by instruction, for the first ~2.9 million
  instructions of the BaseROM.
- **Native capture against the screen.** `tools/debug/drawcap_check` replays each
  captured draw list and compares it with what the game sent to the LCD: every frame
  of the tile-plane and Cars engines' test runs is reproduced exactly.
  `tools/debug/flash_check` compares the Flash redraw with the LCD; it is close but
  not pixel-exact, since its anti-aliasing is leapemu's own
  ([interpolation.md](interpolation.md)).
- **Input movies** recorded in the GUI replay to the same hash in the CLI, on every
  backend.
- **Timing** is checked against footage of real hardware ([timing.md](timing.md)).
- **The GUI** can be run headless and scripted (`LEAPEMU_TEST_SCRIPT` and related
  variables, [tools/README.md](../tools/README.md)), with `SDL_VIDEO_DRIVER=offscreen`.

## Validation against MAME

`tools/oracle/` builds a Leapster-only MAME from the reverse-engineering fork
toadster172/mame@leapster. That build is used purely as a test oracle. `compare.py`
checks the two emulators instruction by instruction:

```sh
tools/oracle/mame_trace.sh uni15 - 3M tools/oracle/traces/uni15_3M.trace.gz
build/leapemu-cli --bios BIOS --mame-compat --trace ours.trace --trace-format mame \
                  --trace-limit 3000000 --frames 60
tools/oracle/compare.py ours.trace tools/oracle/traces/uni15_3M.trace.gz
```

The two agree for the first ~2.9 M instructions, including interrupts; the first
difference is a timer read one tick apart (scheduler granularity). The places where
leapemu deliberately differs from MAME are listed in
[hardware.md](hardware.md#known-divergences-from-mame).

## Contributing

See [CONTRIBUTING.md](../CONTRIBUTING.md), and [tools/README.md](../tools/README.md) for the
development tools.
