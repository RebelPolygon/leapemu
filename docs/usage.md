# Using leapemu

How to set leapemu up and use it: the files it needs, the GUI (menus, settings, the
game list, controls, display options, saves), the command-line frontend
`leapemu-cli`, homebrew, and the CPU backends.

**Contents**
- [The BaseROM and games](#the-baserom-and-games)
- [The GUI](#the-gui)
  - [Command line](#command-line)
  - [Menus](#menus)
  - [Settings](#settings)
  - [Game list](#game-list)
  - [ROM tools](#rom-tools)
  - [Controls](#controls)
  - [Display](#display)
  - [Touch calibration](#touch-calibration)
  - [Power](#power)
  - [Saves](#saves)
  - [The data folder](#the-data-folder)
  - [TAS](#tas)
- [The command line (`leapemu-cli`)](#the-command-line-leapemu-cli)
- [Homebrew](#homebrew)
- [CPU backends](#cpu-backends)
- [CPU timing](#cpu-timing)

## The BaseROM and games

You need a BaseROM, the console's system ROM, dumped from your own Leapster, as well
as your own cartridge dumps: leapemu does not provide or link to any. Raw `.bin`
images, `.zip` archives and images wrapped in a `.wav` container all load directly.
Nothing runs without a BaseROM: until one is chosen, the GUI asks for it.

| File | Identity | Notes |
|---|---|---|
| `155-10072-a` | Leapster Universal v1.5 (2003), CRC `af05e5a0` | recommended; this is what is tested |
| `leapster2_1004` | Leapster UK v2.1 (2005) | despite the file name, *not* a Leapster 2 BaseROM |
| `leapster2_1008` | Leapster Spanish v1.0 (2006) | ditto |
| `leapster2_1006` | Leapster German v2.1, CRC `a69ed8ca` | bad dump: read with address line A19 stuck, so every second 512 KiB is a copy and half the BaseROM is missing. It cannot boot; leapemu says so when it is loaded. A new dump is needed |
| `am29pl160cb-90sf` | LeapsterTV Universal v2.1.11 (2006) | does not run yet: the TV model's controller port and video output are not emulated ([hardware.md](hardware.md#leapstertv-observed-not-emulated)) |

Real Leapster 2 BaseROMs (`152-12659`, `152-12206`, `152-12076`) have not been obtained
yet. Leapster cartridges are made to work on every Leapster model, so the original
BaseROM runs the Leapster 2's games too (all those tested,
[compatibility.md](compatibility.md)).

## The GUI

```sh
build/leapemu --bios path/to/155-10072-a.wav "path/to/Pet Pals (USA).zip"
```

### Command line

`leapemu [options] [ROM]`: `-b/--bios FILE`, `-f/--fullscreen`,
`-s/--scale N` (window size), `-t/--state FILE` (load a save state),
`-m/--movie FILE` (play an input movie), `-v` (more logging), `-h/--help`, `--version`.
With no ROM (or `--bios` alone, which starts the console without a cartridge), it
shows the game list, or asks for a BaseROM if none is chosen yet.

### Menus

- **File:** Load ROM, Load Unsigned ROM (homebrew), Recent, Game Properties (the
  running game's ROM tools), Open Data Folder, Exit. Files can also be dragged
  onto the window.
- **Emulation:** Pause, Reset, Stop (closes the game at once, back to the game
  list), the console's Power Switch, Speed (with a fast forward toggle), Rewind,
  Frame Back / Frame Advance, Save State / Load State (a quick slot and four
  more), and Console: Start Without Cartridge, Recalibrate Touch, and the
  console's brightness, contrast and volume buttons.
- **View:** Window Size (1×–8×), Filter, Fullscreen, Integer Scaling, Vector
  Scale to Window, Interpolation (hack, buggy); Volume, Mute; Screenshot.
- **Tools:** Settings, Movies (see TAS below), Debugger (CPU registers,
  disassembly with breakpoints, memory viewer, and the MQX debug console).
- **Help:** Controls, Shortcuts, About (credits).
- In fullscreen the menu bar shows while the mouse is at the top of the screen.

### Settings

Tools > Settings (Ctrl+,). Changes apply at once.
- Paths: the BaseROM; where game saves go (next to the ROM, and a folder for the
  rest); the game list's folders, Search Subfolders and Show When No Game Is
  Running; the data folder.
- Emulation: CPU core, idle loop skip, accurate timing, fast forward speed
  (unthrottled by default), rewind; pause and/or mute while the window is in the
  background.
- Audio & Video: volume and mute; filter, integer scaling, vector scaling,
  interpolation, VSync, and SDL's render driver. The GPU shows the picture
  (scaling, Nearest, Bilinear); the other filters, vector scaling and interpolation
  are drawn on the CPU.
- Controls and Shortcuts: every key and gamepad button, two keys each.

### Game list

Tools > Settings > Paths > Add Folder... lists the games in that folder
(and its subfolders, with Search Subfolders) while nothing is running: title,
region, type, compatibility and size, sortable by any column. Double-click or Enter
plays one. It is sorted by type (cartridges, downloads, prototypes), then by title.
A game missing from leapemu's database is listed as a prototype (or as homebrew)
when its image shows it: see [compatibility.md](compatibility.md).
When a game turns the console off, its cartridge comes out and the list shows again;
Emulation > Power On starts the same game. If the game has not shut down within 10
seconds of switching the power off, leapemu turns the console off itself.
- Right-click: Play, Play as Unsigned, Compatibility, Game Properties, Contents, Hex &
  Patches and Open Containing Folder.
- Names and compatibility come from the game database (`res/games.tsv`, see
  [compatibility.md](compatibility.md)), else from the file name.
- Compatibility is shown as five squares (Broken 1 to In-game 4, Untested none).
  Right-click > Compatibility, or the game's Properties, sets your own rating, which
  replaces leapemu's on your computer (marked with *). What each file holds is
  cached, so later scans are quick.

### ROM tools

From the game list (right-click), or File > Game Properties for the running game; see
[rom-tools.md](rom-tools.md).
- Info: the game, its file (with where its save is) and its ROM header; your
  compatibility rating; Play (or Restart) and Open Containing Folder;
- Contents: the header's tables and the game's assets (Flash movies, speech, sounds,
  music, Schoolhouse Rock's songs, bitmaps, fonts, ...): listened to and viewed in place,
  and saved as .swf, .wav, MIDI, .bmp, TrueType or as stored. The songs are decoded by
  running the game's own codec;
- Hex & Patches: a hex view of the ROM where edits go into named patches, kept in a
  file apart from the ROM and switched on and off. Enabled patches apply when the
  game starts.

### Controls

The defaults. Every key and gamepad button can be changed in Tools > Settings >
Controls and Shortcuts.
- The arrows (or WASD) = D-pad, X or Space = A, Z = B, C = Hint, Backspace = Home,
  Enter = Pause (the console's Pause button). Left mouse = stylus.
- The console's brightness, contrast and volume buttons have no keys by default:
  they are in Emulation > Console. Their settings are kept in the console's memory,
  like on the real one.
- Ctrl+P = pause / resume, Ctrl+R = reset, Ctrl+W = stop (close the game).
- Hold Tab = fast forward, Shift+Tab = fast forward on / off, `-` / `=` = slower /
  faster (Emulation > Speed: 0.25× to 4×, or unthrottled). Sound plays at every
  speed, pitched with it; fast forward is silent above 4×.
- Hold `` ` `` (backquote) = rewind, at the speed the game ran (with Tab: faster).
  Emulation > Rewind (on by default) keeps snapshots every 0.1 s, about 11 KB each,
  up to 256 MB (roughly half an hour); rewinding restores the exact state. During
  fast forward the snapshots are every 0.1 s of real time, not of game time, so
  they don't slow it down.
- `,` = frame back (one frame, exactly), `.` = frame advance, like a video player;
  both repeat while held.
- F1 = quick load, Shift+F1 = quick save; F2–F5 / Shift+F2–F5 = slots 1–4.
- Ctrl+M = mute, F12 = screenshot, F11 or Alt+Enter (or Ctrl+F) = fullscreen,
  Escape leaves it. Ctrl+O = load ROM, Ctrl+, = settings, Ctrl+Q = exit.
- Gamepads (Xbox, PlayStation, Switch and others, through SDL): d-pad or left
  stick, South = A, East = B, West = Hint, Start = Pause, Back = Home, right
  bumper = fast forward, left bumper = rewind. The right stick moves a stylus
  cursor (it starts at the centre of the screen, and fades out after 3 s unused);
  the right trigger or a right-stick click presses there.

### Display

- **Filter:** Nearest (pixel perfect, the default), Sharp Bilinear, Bilinear,
  Bicubic and Lanczos (sharper smoothing, good for the Flash games' cartoon art and
  photos), MMPX (rebuilds smooth diagonals from pixel art such as Sonic X), and LCD
  (the handheld's pixel grid and slow response). They also apply to the redraws
  below: layers at the original resolution are filtered, and vector graphics already
  drawn at the window's resolution are only smoothed.
- **Vector Scale to Window** (off by default) redraws Flash content as vectors at
  the window's resolution, instead of enlarging the original 160×160 image; it
  also redraws the road of the Cars races. It works with or without
  interpolation.
- **Interpolation** (a hack, off by default; buggy: parts of characters can still
  slide, stretch or pop) smooths motion between game frames.
  - Leapster games draw gameplay at ~15 fps; interpolation shows in-between images
    at your display's refresh rate.
  - It follows the game's own drawing where it can, and otherwise estimates motion
    from the pixels in layers (camera scroll plus objects, pixels never blended).
    It follows the game's drawing for:
    - **Flash content**, in any cartridge (Flash games, and the menus and
      cutscenes of native games), drawn by the BaseROM's Flash player. Each
      object's transform animates between Flash frames. Characters made of
      separate parts keep their joints together (a skeleton is inferred from
      the frames). Drawings the timeline swaps in (blinks, mouth poses) switch
      half-way and move with the part they sit on. Dynamic text is drawn smoothly
      upscaled.
    - **The tile-plane engine** of Sonic X and Go Diego Go! Animal Rescuer. Planes
      and sprites are redrawn at in-between positions.
    - **The races of Cars and Cars Supercharged.** The road is redrawn from the
      camera at in-between positions; the sky, the car and roadside objects
      follow.
  - Game speed and logic are unchanged; it adds about one game frame (~60 ms) of
    display lag, and is disabled while fast-forwarding.
- Both check every redrawn frame against the real screen and show the original
  image whenever they cannot reproduce it (for example something the game draws
  itself). See [interpolation.md](interpolation.md).
- **Screenshot** (F12) saves the original 160×160 image to the data folder's
  `screenshots/` folder, named after the game and the time.

### Touch calibration

It happens automatically on first boot. To redo it, use
Emulation > Console > Recalibrate Touch.

### Power

Emulation > Power Switch Off works like the real switch: the console
reacts at once (even in the middle of a game), saves its settings, plays its
power-off animation and turns off. Like the real one, it also turns itself off
after about 15 minutes without input. Emulation > Stop closes the game at once
instead.

### Saves

A cartridge's save is a `.sav` file next to its ROM
(`Pet Pals (USA).zip` → `Pet Pals (USA).sav`), as in other emulators. leapemu
checks for changed save data about once a second, and also writes it on reset,
when the game is closed and on exit. Each write replaces the file whole (a new file,
then renamed over the old one), so an interrupted write leaves the previous save
intact; a crash before the next write can lose the last second of changes. When the ROM's folder is not writable, or with Tools > Settings > Paths >
Next to the ROM off, it goes to the saves folder chosen there (by default the data
folder's `saves` folder). A save is found wherever it was kept before, and is
written to the current place from then on. Loading a save state also restores the
save data it contains.

### The data folder

File > Open Data Folder opens it: `~/.local/share/leapemu/leapemu` on
Linux, `%APPDATA%\leapemu\leapemu` on Windows, `~/Library/Application Support/leapemu/leapemu`
on macOS. It holds `leapemu.ini` (settings), `states/` (save states), `saves/`,
`patches/`, `screenshots/`, `gamelist.cache`, and `nvram/system.eep`: the console's own
memory (touch calibration and its settings). Saves from older versions
(`nvram/<crc>.eep`) are imported automatically.

### TAS

Input movies, rerecording, frame advance and frame back, and frame, lag and
input counters (Tools > Movies). See [tas.md](tas.md).

## The command line (`leapemu-cli`)

`leapemu-cli` runs the same emulator without a window: for tests, scripted runs,
traces, exports and checks. It needs a BaseROM (`--bios`), and runs for `--frames`
(60 by default) or `--seconds`. Every run is deterministic: the same files, settings
and input always give the same machine state, which `--state-hash` prints as a
CRC-32. That is how the CPU backends, input movies and the other platforms' builds
are checked against each other.

```sh
leapemu-cli --bios BIOS --info                                    # ROM header information
leapemu-cli --bios BIOS --cart GAME.zip --seconds 60 \
            --save GAME.sav --screenshot out.png --uart           # a headless run
leapemu-cli --bios BIOS --cart GAME.zip --touch 80,80@300-320 \
            --press a@400 --state-hash                            # scripted input
leapemu-cli --bios BIOS --cart GAME.zip --load-state s.state \
            --seconds 10 --save-state t.state                     # from and to a save state
leapemu-cli --bios BIOS --trace t.txt --trace-limit 100000        # instruction trace
leapemu-cli --bios BIOS --disasm 4002b5f4 40                      # disassembler
leapemu-cli --bios BIOS --cart GAME.zip --export-assets out/      # the game's assets
```

| Options | |
|---|---|
| **Files** | |
| `--bios FILE`, `--cart FILE` | the BaseROM and the cartridge (`.bin`, `.zip`, `.wav`-wrapped; a Leapster 2 download's `.bin` too) |
| `--save FILE` | the cartridge's save, as a `.sav` file (read at start, written at the end) |
| `--nvram DIR` | keep the EEPROMs (touch calibration, saves) in a folder instead |
| `--load-state FILE`, `--save-state FILE` | a save state before / after the run |
| `--allow-unsigned` | boot an unsigned cartridge (homebrew, [homebrew.md](homebrew.md)) |
| `--patches FILE` | apply a patch file's enabled patches ([rom-tools.md](rom-tools.md)) |
| `--stub-services` | give any download leapemu's Leapster 2 services ([downloadables.md](downloadables.md)) |
| **Input** | |
| `--press BTN@F[-F2]` | hold a button during frames F to F2 inclusive (without F2: six frames, F to F + 5): `a`, `b`, `up`, `down`, `left`, `right`, `hint`, `home`, `pause`, `volup`, `voldown`, `brightup`, `brightdown`, `contrast`; repeatable. Frames count from 0 at the start of the run |
| `--touch X,Y@F[-F2]` | touch the screen at X,Y (pixels) during frames F to F2, the same way; repeatable |
| `--power-off F` | switch the power off at frame F |
| `--movie FILE`, `--record-movie FILE` | play an input movie from power-on / record the run's input as one ([tas.md](tas.md)) |
| **Output** | |
| `--screenshot FILE`, `--shot-every N` | the last frame as PNG, and every Nth frame as `FILE-<frame>.png` |
| `--audio-out FILE` | the sound, as a WAV file (32 kHz mono, 16-bit) |
| `--uart` | the debug UART (the system's MQX console) on standard output |
| `--state-hash` | a CRC-32 of the final machine state |
| `--interp MODE`, `--interp-dump DIR`, `--interp-steps N`, `--interp-scale K` | write what the display would show, with frame interpolation (`off`, `motion`, `native`), N images per frame, at K× ([interpolation.md](interpolation.md)) |
| **CPU and timing** | |
| `--cpu BACKEND` | `interpreter`, `cached` or `jit` (the default on x86-64 and AArch64; [jit.md](jit.md)) |
| `--no-idle-skip` | run the system's idle loop instead of skipping it |
| `--timing ...`, `--cart-timing ...`, `--no-waits` | the wait-state model ([timing.md](timing.md)) |
| `--cache ...`, `--fill ...` | the experimental cache model (timing.md) |
| **Debugging** | |
| `--trace FILE`, `--trace-limit N`, `--trace-format native\|mame` | an instruction trace (the `mame` format compares with the MAME oracle, [building.md](building.md#validation-against-mame)) |
| `--mame-compat`, `--straps HEX` | MAME's quirks, and the strap register's value ([hardware.md](hardware.md)) |
| `--break ADDR` | stop when execution reaches an address; repeatable |
| `--disasm ADDR N` | disassemble N instructions and exit |
| `--profile` | where the CPU spends its time |
| `--info` | the ROM header, and exit |
| `--export-assets DIR`, `--export-raw` | write the game's assets, readable or as stored, and exit |
| `-v`, `-vv` | more logging |

`leapemu-cli --help` lists them too. Exit status:

| Status | Meaning |
|---|---|
| 0 | Success |
| 1 | A file could not be read (the BaseROM, cartridge, patches, movie, state) or written (a screenshot, the state, the save, the movie, the audio) |
| 2 | Wrong usage: an unknown or malformed option, or options that cannot be combined |
| 3 | The CPU stopped: a fault, a halt or a breakpoint |

## Homebrew

`homebrew/` contains a small bare-metal SDK and a hello-world. See
[homebrew.md](homebrew.md). Homebrew cartridges are unsigned; to run them on
a LeapFrog BaseROM, use File > Load Unsigned ROM (`--allow-unsigned` in the CLI).

## CPU backends

Choose one under Tools > Settings > Emulation, or with `--cpu` in the CLI.

| Backend | Description |
|---|---|
| Interpreter | Fetches and decodes every instruction; the reference implementation |
| Cached Interpreter | Decodes ROM code once and dispatches pre-decoded handlers in a tight loop. About 2× the interpreter on CPU-bound games |
| JIT Recompiler (x86-64 and AArch64; the default on both) | Compiles ROM code to native machine code in blocks. 3.5–5.5× the cached interpreter. See [jit.md](jit.md) |

**Idle-loop skipping** is on by default in every backend. When the RTOS idle task is
provably spinning with nothing to do (identical state on consecutive passes, and no
interrupt pending), time jumps straight to the next hardware event. Flash games spend
~80% of their time there, so they run 10–35× real time. How it works, and why it
changes the exact machine state but not what the system does:
[hardware.md](hardware.md#the-idle-loop-and-skipping-it-leapemu).

All three backends produce **byte-identical machine state**.
`tools/debug/backend_diff.cpp` compares the two interpreters every frame, and
`tools/debug/jit_diff.cpp` compares the JIT with the cached interpreter, down to the
cycle.

## CPU timing

Gameplay speed is calibrated against real-hardware footage. The CPU model charges
wait states for slow cartridge-ROM and RAM accesses, as the real Leapster has them.
It matches recorded native-game frame rates to within ~10%, and lets Flash-based games
(SpongeBob, Pet Pals) reach their authored frame rates. Timer-paced sequences such as
cutscenes match exactly. See [timing.md](timing.md).
