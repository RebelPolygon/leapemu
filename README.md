<p align="center"><img src="res/leapemu.svg" width="128" alt=""></p>
<h1 align="center">leapemu</h1>
<p align="center">An emulator for the LeapFrog Leapster</p>

leapemu emulates the LeapFrog **Leapster** handheld: its cartridges, the L-MAX titles,
and the Leapster 2's games and downloadable games, which run on the original Leapster's
BaseROM. (The Leapster 2 itself, with its own BaseROM, SD card and storage, is not
emulated.) It is written from scratch in C++20, for x86-64 and 64-bit ARM, with a GUI
and a command-line frontend.

## Features

- **Compatibility:** the 62 retail cartridges tested reach gameplay, as do 6
  prototypes and the 13 Leapster 2 downloadable games tested
  ([compatibility list](docs/compatibility.md)).
- **Accuracy:** CPU timing calibrated against footage of real hardware; all eight sound
  voices, including the LFC speech codec; the touch screen, with automatic calibration;
  the console's own settings and save memory.
- Save states, rewind, fast forward, frame stepping (forwards and back), and input
  movies with rerecording ([TAS](docs/tas.md)). Emulation is deterministic: a movie
  replays the same with the same ROMs and its recorded emulation settings.
- A game list with compatibility ratings (yours or leapemu's).
- Keyboard and gamepad, every key rebindable; on a gamepad, the right stick moves a
  stylus cursor.
- Display filters (pixel-perfect, smooth, MMPX for pixel art, an LCD look), plus an
  optional redraw at any resolution of Flash content and of the Cars races' road,
  and an experimental frame interpolation ([how they work](docs/interpolation.md)).
- ROM tools: a game's Flash movies, speech, music, sounds, bitmaps and fonts, played,
  viewed and exported; a hex view with switchable patches ([ROM tools](docs/rom-tools.md)).
- Homebrew: a small SDK, and unsigned cartridges run ([homebrew](docs/homebrew.md)).

## Getting started

1. **Download** a release: for Windows, the `.zip`; for Linux (x86-64 or arm64), the
   `.tar.gz`, which runs from where it is extracted (on Arch Linux, the release's
   `PKGBUILD` packages it: `makepkg -si` next to the archive). Or **build** leapemu
   (below).
2. **Dump your own** BaseROM (the console's system ROM, often called the BIOS) and
   cartridges. leapemu does not provide or link to any. `.bin` images, `.zip`
   archives and `.wav`-wrapped dumps all load. The tested BaseROM is Leapster Universal
   v1.5 (`155-10072-a`, CRC `af05e5a0`); others are listed in
   [docs/usage.md](docs/usage.md#the-baserom-and-games).
3. **Run** `leapemu`. Choose the BaseROM when it asks, then add the folder with your
   games, or open a single ROM with File > Load ROM (or `leapemu GAME.zip`).

Settings are under Tools > Settings; the full guide to the GUI is
[docs/usage.md](docs/usage.md).

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

This needs CMake 3.24 or newer and a C++20 compiler. The GUI uses SDL3 (the system's,
or fetched) and Dear ImGui (fetched), so the first configure needs network access.
Options, 64-bit ARM, installing and the source layout are in
[docs/building.md](docs/building.md).

## Controls

| Leapster | Keyboard | Gamepad |
|---|---|---|
| D-pad | Arrow keys, WASD | D-pad, left stick |
| A / B | X or Space / Z | South / East |
| Hint, Home, Pause | C, Backspace, Enter | West, Back, Start |
| Stylus | left mouse button | right stick + right trigger |

| leapemu | Key, gamepad |
|---|---|
| Pause / reset / stop | Ctrl+P / Ctrl+R / Ctrl+W |
| Fast forward | hold Tab (Shift+Tab: on / off); right bumper |
| Rewind | hold `` ` ``; left bumper |
| Frame back / frame advance | `,` / `.` |
| Quick load / save; slots 1–4 | F1 / Shift+F1; F2–F5 / Shift+F2–F5 |
| Fullscreen, screenshot, mute | F11, F12, Ctrl+M |
| Settings | Ctrl+, |

Every key and gamepad button can be changed in Tools > Settings > Controls and
Shortcuts.

## Documentation

| Document | What it covers |
|---|---|
| [Using leapemu](docs/usage.md) | The GUI (menus, settings, game list, controls, display, saves) and the command line |
| [Compatibility](docs/compatibility.md) | How far each game plays |
| [Tool-assisted play](docs/tas.md) | Input movies, rerecording, counters; how save states, rewind and determinism work |
| [ROM tools](docs/rom-tools.md) | A game's assets, hex view and patches |
| [Leapster 2 downloadable games](docs/downloadables.md) | How leapemu runs the games sold as downloads for the Leapster 2, and the Leapster 2 system services it provides for them |
| [Display enhancements](docs/interpolation.md) | Redraw at any resolution and frame interpolation, and how the games' own drawing is captured (tile planes, the Cars races, the Flash player) |
| [Building and development](docs/building.md) | Build options, other platforms, release builds, the source map, how correctness is checked |
| [Hardware](docs/hardware.md), [timing](docs/timing.md), [JIT](docs/jit.md), [speech](docs/speech.md) | How the Leapster and the emulator work |
| [Homebrew](docs/homebrew.md), [cartridge ↔ BaseROM interface](docs/cart-bios-abi.md), [open BaseROM](docs/open-bios/CLEANROOM.md) | Writing your own cartridges, and early research towards an open BaseROM |
| [Roadmap and open questions](docs/roadmap.md) | What's next, and what is not known yet |

## Contributing

Game reports and changes are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). Never
attach or link to ROM or BaseROM images.

## Credits

leapemu builds on public research, most of all the Leapster drivers in MAME (David
Haywood) and in toadster172's MAME fork (Alice Shelton), the ROM-format work of the
LeapFrog-Tools and Leapster-Tools projects, and Nathan Farlow's LFC speech decoder. The
full list is in [docs/credits.md](docs/credits.md).

## License

leapemu is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version. See [LICENSE](LICENSE),
and [THIRD-PARTY.md](THIRD-PARTY.md) for the licenses of the software it includes.

It contains no LeapFrog software: BaseROM and cartridge images are yours to provide.
