# Roadmap and open questions

What is not done, and what is not known, in one place. Each item links to where the
details are. These documents keep their own lists, which this page sums up:

| Document | Section |
|---|---|
| Compatibility | [Known issues affecting many games](compatibility.md#known-issues-affecting-many-games) |
| Tool-assisted play | [Not yet](tas.md#not-yet) |
| Leapster 2 downloadable games | [Not done yet](downloadables.md#not-done-yet) |
| JIT | [Not done yet](jit.md#not-done-yet) |
| CPU timing | [What did not fit](timing.md#what-did-not-fit) (the cache model) |
| Speech | [Open questions](speech.md#8-open-questions) |
| Hardware | items marked *unknown* throughout [hardware.md](hardware.md); the [LeapsterTV](hardware.md#leapstertv-observed-not-emulated) |
| Open BaseROM | [Known hard problems](open-bios/CLEANROOM.md#known-hard-problems), the boot spec's [open questions](open-bios/spec/boot.md#open-questions), the services spec's [next steps](open-bios/spec/services.md#4-next-steps-specification-team) |

## Next steps

1. **Accuracy from real hardware.** The CPU timing is fitted to footage of two games
   and the Flash games' authored rates; measurements on a real Leapster (instruction
   timing from cartridge ROM and RAM, LCD and DMA timing) would replace the fit
   ([timing.md](timing.md)); a model of the CPU's caches did not fit
   ([what did not fit](timing.md#what-did-not-fit)). The same goes for speech output,
   the brightness shade and the master volume's absolute level.
2. **Usability:** per-game settings; running emulation on its own thread.
3. **Tool-assisted play:** a piano-roll input editor, RAM search and watch, Lua
   scripting, and movies that start from a save state rather than power-on
   ([tas.md](tas.md#not-yet)).
4. **Leapster 2:** a Leapster 2 BaseROM dump is needed first. Then its SD slot, USB,
   16 MiB RAM and player profiles, for its own programs (the Leapster 2 games tested,
   downloads included, already play on the Leapster's BaseROM;
   [downloadables.md](downloadables.md)).
5. **More native capture.** Other engines (Wörterjäger's maze, Grundschule 2's native
   screens, The Backyardigans' own player, the gameplay of other native games) each
   need their own capture ([interpolation.md](interpolation.md#adding-engines-with-the-cars-races-as-an-example)).
6. **The LeapsterTV:** its controller port and TV video path
   ([hardware.md](hardware.md#leapstertv-observed-not-emulated)).
7. **Performance:** timing the AArch64 JIT on real hardware; computing flags only when
   they are read ([jit.md](jit.md#not-done-yet)).
8. **Open BaseROM (long-term research):** a clean-room replacement, so no LeapFrog
   BaseROM would be needed. None exists. [cart-bios-abi.md](cart-bios-abi.md) and
   [open-bios/](open-bios/CLEANROOM.md) are early research: how the system starts a
   cartridge, and how a cartridge finds the system's services. What those services do,
   and the Flash player that most games rely on, are not specified yet; that is most of
   the work.
   - The next steps are to record each service's behaviour (arguments, results, memory
     and registers touched, blocking) and how the system registers a cartridge's
     services ([services spec](open-bios/spec/services.md#4-next-steps-specification-team));
     the boot spec has its own [open questions](open-bios/spec/boot.md#open-questions).
   - The hard problems: speech needs a 20 KiB table set that is LeapFrog data (only
     the noise table can be regenerated from a public algorithm), and a compatible Flash
     player is by far the largest piece of work
     ([CLEANROOM.md](open-bios/CLEANROOM.md#known-hard-problems)).

## Open questions

**Hardware** ([hardware.md](hardware.md))
- The registers `0x0180'b000`–`b008` (reads), `0x0180'1000`–`1018`, and the probe of
  `0xe000'0000` at boot (a second ROM chip-select?).
- What `0x0180'c020` (the Contrast button's toggle) changes on the panel, and how the
  brightness register `c024` maps to the panel's response.
- Which of the power interrupt's flags (0x20, 0x400) the switch raises, and what GIO
  bits 12 and 30 (watched by the power monitor) report.
- The interrupt-enable bits 0x4000, 0x10000 and 0x40000 of `0x0180'0084`.
- The XY-memory DSP option's rounding and its modes other than the one Schoolhouse
  Rock uses.

**Sound and speech** ([speech.md](speech.md#8-open-questions))
- Bit-exact speech output, its level against the other voices, its start and end
  timing, and whether voice 7 raises the sound interrupt when it ends.
- The sound registers `0x000–0x04c`, `0x170–0x1a0` and `0x20f0`–`0x20f8` (`0x1c4` is
  the master volume).
- SYN music with the game's own instrument samples, rather than General MIDI stand-ins
  ([rom-tools.md](rom-tools.md)).

**ROM contents**
- Grundschule 2's 8-bit Flash sprites, whose palette the game builds at run time
  ([rom-tools.md](rom-tools.md)).
- The format of a Leapster 2 package's `eeprom.ltm` ([downloadables.md](downloadables.md#not-done-yet)).

**Display**
- On one Cars track, a few pixels near the horizon are drawn by a BaseROM routine
  outside the captured engine, and are shown as in the newer frame
  ([interpolation.md](interpolation.md#engine-the-cars-races-cars-cars-supercharged)).
- Vector scaling shows the original image for screens with things a game draws
  itself over Flash content.

**Builds**
- The Windows build is tested under Wine only, the Linux arm64 build under qemu only;
  macOS builds from source in principle, but nothing is tested
  ([building.md](building.md)).
