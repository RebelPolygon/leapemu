# Leapster homebrew

leapemu ships a minimal bare-metal SDK in `homebrew/` for writing third-party Leapster
software. The resulting cartridges exercise the emulator exactly the way commercial
cartridges do: they are booted by the BaseROM, validated, and handed control at their
entry point.

## Building

Install the ARC GNU toolchain. The last release with ARC600 support is
[2023.09](https://github.com/foss-for-synopsys-dwc-arc-processors/toolchain/releases/tag/arc-2023.09-release)
(`arc_gnu_2023.09_prebuilt_elf32_le_linux_install.tar.bz2`). By default the Makefile
expects it in `external/arc-gnu/`.

```sh
make -C homebrew                        # builds build/homebrew/hello.bin
make -C homebrew ARC_PREFIX=/opt/arc/bin/arc-elf32-
```

Code is compiled with `-mcpu=arc600_mul64`, which matches the Leapster's ARCtangent-A5
(barrel shifter, `norm`/`swap`, `mul64`).

## SDK contents

| File | Purpose |
|---|---|
| `sdk/mkcart.py` | Packs a flat binary into a cartridge image. It writes the header, the RIB, a boot descriptor, product info (including the "needed base system" record), and both checksums |
| `sdk/crt0.S` | Start-up code: sets up its own stack in RAM, copies `.data`, clears `.bss`, and calls `main` |
| `sdk/link.ld` | Places code at `0x80001000` in cartridge ROM, and data/bss/stack in `0x3c000200`–`0x3c100200` |
| `sdk/leapster.h` | Hardware registers: LCD, buttons, timer, UART |
| `hello/main.c` | Example program: draws to the LCD, reads the D-pad and A button, and prints to the debug UART |

Programs currently drive the hardware directly: interrupts stay off and no BaseROM
services are used. Wrappers for the system services could follow once the services are
documented (`docs/open-bios/spec/` covers how they are found, not yet what they do).

## Running

Homebrew cartridges are **unsigned**. LeapFrog BaseROMs refuse cartridges that lack
LeapFrog's "Approved Content" marks (see `docs/open-bios/spec/boot.md` §2–3). An open
BaseROM could accept any cartridge with valid checksums, but none exists
([roadmap.md](roadmap.md)).

On a LeapFrog BaseROM, leapemu loads such a cartridge **as unsigned** (GUI:
File > Load Unsigned ROM; CLI: `--allow-unsigned`). For that cartridge only, reset
patches, in memory only, the BaseROM's checks so that they report "pass"; the image
on disk is never modified.
- The checks are the 160-bit digest comparison, the copyright-verse spot check,
  and the sparse checksum (the sum of every 0xF94th word, compared with the value
  stored at the end of the image).
- Skipping the checks also runs dumps with a few damaged bytes. A copy of
  Schoolhouse Rock – America Rock (USA) whose checksums are off by two single-bit
  errors plays this way, and leapemu applies the option to that dump by itself (it
  knows its CRC); a real Leapster rejects it. (The prototypes tested don't
  need it: they are signed and checksummed like retail games.)
- Each patch point is found by a signature of the code around it (a length and
  a hash of the bytes, `src/core/codesig.h`) in one of its known compiled forms,
  and patched only if it occurs exactly once; the option works with Universal
  v1.5, UK v2.1 and Spanish v1.0.
- If the patch points aren't found, the option reports it and does nothing.

```sh
build/leapemu-cli --bios BIOS --cart build/homebrew/hello.bin --allow-unsigned --uart
```

Homebrew must not copy LeapFrog's verse, signature or other content into its images.
`mkcart.py` writes its own placeholder text in those fields.
