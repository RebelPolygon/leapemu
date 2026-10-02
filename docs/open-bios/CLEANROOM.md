# Open BaseROM: clean-room process

**Status: research.** No open BaseROM exists. The specification in `spec/` covers how a
cartridge is started and how it finds the system's services; what the services do, and
the Flash player most games rely on, are not specified yet. This document sets out how
the work would be done.

The open BaseROM ("open BIOS") must contain no LeapFrog code or data. It would be
developed with the classic two-team clean-room method, the same approach used for the
IBM PC BIOS clones and for Sega v. Accolade-style interoperability work.

## Roles

**Specification team ("dirty").** May study the original system:
- run the original BaseROM and cartridges in leapemu or MAME and record observable
  behaviour;
- read cartridge code to learn what it *expects* from the system;
- read BaseROM disassembly when unavoidable.

The specification team writes **behavioural specifications only**, in
`docs/open-bios/spec/` and `docs/cart-bios-abi.md`. A spec may state:
- interfaces: entry addresses and calling conventions;
- data layouts;
- register and memory side effects;
- timing and error behaviour.

A spec must **never** contain:
- code or pseudo-code transcribed from the BaseROM;
- disassembly listings;
- constants or tables copied from BaseROM code, unless they are purely interface facts
  (addresses, magic numbers, field layouts) needed for compatibility.

The leapemu maintainers are on this team: they have seen parts of the v1.5 BaseROM
disassembly.

**Implementation team ("clean").** Will write the open BaseROM (planned: `openbios/`). It may use
only:
- `docs/open-bios/spec/`, `docs/cart-bios-abi.md` and `docs/hardware.md`;
- public documentation: the ARCompact ISA reference, the GNU toolchain manuals,
  [Leapster-Tools](https://github.com/lfhacks/Leapster-Tools) for the ROM format;
- the results of running its own code in leapemu (screens, logs, pass/fail output of
  test cartridges).

It must **not** look at:
- any LeapFrog BaseROM or cartridge image, or its disassembly;
- MAME/leapemu traces of LeapFrog code;
- `tools/oracle/` traces;
- the sections of `docs/speech.md` that are marked as derived from BaseROM disassembly;
- BaseROM routine addresses mentioned in `docs/hardware.md` and `docs/speech.md` (they
  locate code in a LeapFrog image; the interface facts around them are fine to use).

Questions go back to the specification team as written requests (planned:
`docs/open-bios/requests/`), and are answered in the spec.

## Provenance

Every spec statement records how it was learned, in one of these ways:
- *observed* (leapemu black-box run, with the command used);
- *public* (link);
- *cartridge-side* (what cartridge code does with the interface);
- *disassembly* (only when unavoidable, and only the resulting interface fact).

## Automated checks

- Planned: `tools/cleanroom/overlap.py`, to scan the built open BaseROM for any byte run
  of 16 or more bytes shared with a LeapFrog BaseROM image. Only the specification team
  will run it, because it needs the dumps; any hit will fail the build.
- The open BaseROM source tree must not contain binary blobs other than assets created
  by the project.

## Known hard problems

- **Speech codebook.** Cartridge speech (LFC) streams are decoded with a 20 KiB table set
  that ships in every LeapFrog BaseROM. The noise table can be regenerated from a
  documented public algorithm (BSD `random()`), but the pulse-shape, gain and
  reflection-coefficient tables are LeapFrog data. Until that is resolved the open
  BaseROM can play speech only if the user supplies those tables from their own dump.
- **Flash player.** Most cartridges are Flash (SWF) content played by the BaseROM's
  player plus per-cartridge native extensions. A compatible player is by far the largest
  piece of work, and is scheduled last.
