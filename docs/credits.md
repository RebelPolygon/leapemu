# Sources and prior work

leapemu builds on public research. No MAME code runs in leapemu, and it contains no
LeapFrog software; you must provide your own dumps.

| Source | Used for |
|---|---|
| [toadster172/mame, branch `leapster`](https://github.com/toadster172/mame/tree/leapster): Alice Shelton's Leapster driver ([`leapster.cpp`](https://github.com/toadster172/mame/blob/leapster/src/mame/leapfrog/leapster.cpp), [`leapster_a.cpp`](https://github.com/toadster172/mame/blob/leapster/src/mame/leapfrog/leapster_a.cpp)) and ARCompact fixes | The hardware register model (timers, LCD/DMA, ADC touch, EEPROM, UART, sound), and the test oracle in `tools/oracle/` |
| [MAME](https://github.com/mamedev/mame): David Haywood's original [Leapster driver](https://github.com/mamedev/mame/blob/master/src/mame/leapfrog/leapster.cpp), [ARCompact core](https://github.com/mamedev/mame/tree/master/src/devices/cpu/arcompact) and [software list](https://github.com/mamedev/mame/blob/master/hash/leapster.xml) (BSD-3-Clause) | BaseROM identification and CRCs; cross-checking the CPU core |
| [ARCompact ISA Programmer's Reference](http://me.bios.io/images/d/dd/ARCompactISA_ProgrammersReference.pdf) (ARC International, 2008) | Instruction set semantics |
| [lfhacks/Leapster-Tools](https://github.com/lfhacks/Leapster-Tools) (LeapSplit), successor to [BLiNXthetimesweeperGOD/LeapFrog-Tools](https://github.com/BLiNXthetimesweeperGOD/LeapFrog-Tools) | The ROM header / RIB / asset-table format |
| [Leapster documentation gist](https://gist.github.com/BLiNXthetimesweeperGOD/cc98ea1ddb439c886f1921a7fb9312ba) and [SYN format gist](https://gist.github.com/BLiNXthetimesweeperGOD/3959b63c80ca144becd5543b03c0e805) (BLiNXthetimesweeperGOD) | System overview, developers, file formats |
| [nathanfarlow/leapfrog-voice-decoder](https://github.com/nathanfarlow/leapfrog-voice-decoder) | The LFC speech codec (see [speech.md](speech.md)) |
| [ResHax: LeapFrog LPC/LFC format](https://reshax.com/topic/985-leapfrog-lpclfc-format/), [LeapFrog Wiki: CELP Compression](https://leapfrog.fandom.com/wiki/CELP_Compression) | Early speech-format notes |
| [Synopsys ARC GNU toolchain](https://github.com/foss-for-synopsys-dwc-arc-processors/toolchain) | Building homebrew and test ROMs |
| [MMPX](https://casual-effects.com/research/McGuire2021PixelArt/) (Morgan McGuire and Mara Gagiu, MIT license) | The MMPX filter (`src/core/scale.cpp` is a port of its reference code) |
| [toadster172/.DMPSTER](https://github.com/toadster172/.DMPSTER) (GPL-3.0) | Public documentation of Leapster 2 system interfaces (see [downloadables.md](downloadables.md)); no code is used |
| [SDL 3](https://libsdl.org) (zlib license) and [Dear ImGui](https://github.com/ocornut/imgui) (MIT license) | The GUI (fetched at build time, not part of this repository; their licenses are in [THIRD-PARTY.md](../THIRD-PARTY.md)) |

The licenses of the third-party software in the GUI (Dear ImGui, SDL 3) and of the MMPX
filter are in [THIRD-PARTY.md](../THIRD-PARTY.md).
