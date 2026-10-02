# Contributing

Thank you for helping. leapemu is a preservation project: please keep it free of
LeapFrog's copyrighted material.

## What to keep out

Please never:
- Attach, upload or link to BaseROM, cartridge or download images, or to sites that
  host them. Bug reports name games; they don't share them.
- Copy code from non-public sources (leaked SDKs or game source code). Public
  documentation and our own observations only; [docs/credits.md](docs/credits.md) lists the
  sources used.
- Put bytes of a BaseROM or game into the source. To find a routine in a ROM, use a
  code signature (a length and a hash; `tools/debug/code_sig` prints one).

## Reporting how a game plays

Open an issue with:
- the title and region, and the image's CRC-32 (File > Game Properties shows it);
- the BaseROM (its file name);
- the leapemu version (`leapemu --version`) and your platform (operating system, and
  x86-64 or ARM);
- what happened, and how far it got (see [docs/compatibility.md](docs/compatibility.md)
  for the tiers);
- if you can, an input movie (Tools > Movies > Record...) that shows it: a movie has no
  game data in it, only the buttons and stylus, and replays exactly.

## Changing the code

- C++20. The core (`src/core/`) has no third-party dependencies; keep it that way.
  The GUI uses SDL3 and Dear ImGui only.
- Match the surrounding code: its naming, its comment style (comments say why, in
  plain words) and its formatting.
- Results must not change by accident. Every CPU backend gives the same machine state,
  so an input movie or save state behaves the same everywhere:
  `leapemu-cli --cpu cached ... --state-hash` and `--cpu jit` must print the same hash,
  and the hash for a given input must not change unless the change is meant to. Run
  `ctest --test-dir build` too.
- Update the documentation (README.md and docs/) with the code. When you find
  something out about the hardware or a game, write down what you found and how:
  mark leapemu's own findings as such, say what was measured or traced, and add what
  is still unknown to [docs/roadmap.md](docs/roadmap.md). The docs are where this
  project's knowledge lives; [docs/building.md](docs/building.md) says how results
  are checked.
- Research towards an open BaseROM (none exists yet) follows a clean-room process: read
  [docs/open-bios/CLEANROOM.md](docs/open-bios/CLEANROOM.md) before working on it.

Development tools: [tools/README.md](tools/README.md) (`-DLEAPEMU_BUILD_TOOLS=ON`).
