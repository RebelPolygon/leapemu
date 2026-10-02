# MAME Leapster oracle

A Leapster-only build of MAME from toadster172's fork (the `leapster` branch). It is the
reference "oracle" for the standalone emulator, and `mame_trace.sh` turns it into
per-instruction ARCompact CPU traces.

## Setup

The oracle is not part of this repository. To set it up:

1. Clone [toadster172/mame](https://github.com/toadster172/mame), branch `leapster`.
   The traces here were made at commit `499760d9100b` ("cpu/arcompact: Correct typo
   on setting negative flag for TST_S", 2026-04-01).
2. Apply `arcompact-debugger-regnames.patch` (below).
3. Build it as shown below.
4. Point the scripts at the build, either with `MAME_BIN` or by symlinking the clone
   to `external/mame-leapster-oracle/`.

MAME needs the BaseROMs as raw images named as in its driver:

- `leapster/155-10072-a.bin` (uni15, CRC32 af05e5a0)
- `leapster/leapster2_1004.bin` (uk21)
- `leapster/leapster2_1006.bin` (ger21, flagged BAD_DUMP by the driver)
- `leapster/leapster2_1008.bin` (sp10)
- `leapstertv/am29pl160cb-90sf.bin` (uni2111)

Put them under a rompath given by `MAME_ROMPATH` (default: `external/mame-oracle-roms/`).
Dumps that circulate wrapped in a WAV container need the 60-byte header stripped, e.g.
`tail -c +61 X.wav > X.bin`.

`-verifyroms leapster` still reports the set as "bad". That is expected: `us21`
(`152-11265_2.1.bin`) has never been dumped, and `ger21` is a known bad dump.

## Build

```sh
cd path/to/mame   # the toadster172/mame clone, branch leapster
make SUBTARGET=leapster SOURCES=src/mame/leapfrog/leapster.cpp \
     TOOLS=0 NOWERROR=1 OPTIMIZE=2 SYMBOLS=0 -j32
```

- The build used gcc 16.1.1 and needed no REGENIE and no clang. There were warnings
  (`-Wsfinae-incomplete`, deprecated `wstring_convert`) but no errors.
- A build from scratch takes about 8 minutes of wall time at -j32. `makedep` pulls in
  `leapster_a.cpp` (sound) by itself.
- If a build is interrupted, gcc can leave **0-byte `.o` files** and archives without an
  index behind. The next link then fails with `libsqlite3.a: archive has no index`. To fix
  it, run `find build -name '*.o' -size 0 -delete; rm build/linux_gcc/bin/x64/Release/*.a`
  and run make again.

### Local patch (`arcompact-debugger-regnames.patch`)

The arcompact core registers its debugger state with the disassembler names, for example
`r60(LP_COUNT)` and `r57(M-LO)`. The debugger expression parser treats `(` as a function
call, so those registers could not be used in a `tracelog` expression. The patch cuts the
`(...)` suffix off the *state symbol* only, which leaves `r60` for LP_COUNT. The
disassembly output and emulation behaviour do not change.

Register symbols in expressions after the patch (lower case): `pc`, `curpc`, `status32`,
`lp_start`, `lp_end`, `r0`..`r25`, `r26_gp`, `r27_fp`, `r28_sp`, `r29_ilink1`,
`r30_ilink2`, `r31_blink`, `r32`..`r63` (`r60` is LP_COUNT).

## Usage

```sh
tools/oracle/mame_trace.sh                                  # uni15, no cart, 200000 insns -> traces/leapster_uni15_200000.trace.gz
tools/oracle/mame_trace.sh uni15 - 1M                       # 1,000,000 instructions
tools/oracle/mame_trace.sh uni15 path/to/cart.bin 50k out.trace   # with a cart, plain-text output
tools/oracle/mame_trace.sh -S leapstertv -n 100000          # Leapster TV (uni2111)
tools/oracle/mame_trace.sh -N -s 20                         # no trace: 20 emulated s, PNG of final frame -> snapshots/
tools/oracle/mame_trace.sh -h                               # all options
```

The positional arguments are `[BIOS] [CART] [INSNS] [OUT]`. `CART` may be `-` for no cart.
The trace is capped at **exactly INSNS instructions** (default 200000). The `-s` seconds
value is only a safety cap for trace runs. It does apply in `-N` snapshot mode.

## Trace line format

There is one line per executed instruction, in execution order. Line 1 is the reset
vector. Each line has 37 space-separated 8-digit upper-case hex fields, then ` | `, then
MAME's disassembly:

```
PC STATUS32 LP_COUNT LP_START LP_END r0 r1 ... r25 r26(gp) r27(fp) r28(sp) r29(ilink1) r30(ilink2) r31(blink) | PPPPPPPP: <disasm>
```

- **All register values are the state BEFORE the instruction at PC executes.** The effect
  of an instruction shows up on the next line.
- Field 1 is PC. It repeats as `PPPPPPPP:` before the disassembly.
- Field 2 is STATUS32 as MAME stores it: Z=0x800, N=0x400, C=0x200, V=0x100, E2=0x4,
  E1=0x2. MAME resets it to 0.
- Field 3 is LP_COUNT (r60). Fields 4 and 5 are the aux registers LP_START and LP_END.
- Fields 6 to 37 are r0..r31.
- Split with `line.split(' | ')[0].split()` to get the 37 values.

The first 3 lines of `leapster -bios uni15` with no cart (fields 7 to 32 shortened here):

```
40000000 00000000 00000000 00000000 00000000 00000000 00000000 ... 00000000 00000000 00000000 00000000 | 40000000: J 0x4002b5f4
4002B5F4 00000000 00000000 00000000 00000000 00000000 00000000 ... 00000000 00000000 00000000 00000000 | 4002B5F4: MOV r28_SP, 0x03000800
4002B5FC 00000000 00000000 00000000 00000000 00000000 00000000 ... 03000800 00000000 00000000 00000000 | 4002B5FC: BL.D 0x4002b658
```

Full line example:

```
4002B670 00000000 00000000 00000000 00000000 00000000 4002B754 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 02FFFF40 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 030007F0 00000000 00000000 4002B604 | 4002B670: MOV_S r2 <- 0x40000100
```

A line is about 375 bytes uncompressed. 200k instructions come to 75 MB as plain text and
0.96 MB as `.gz`.

### Semantics worth knowing when diffing against a new core

- The reset PC is `0x40000000`, from `set_default_vector_base(0x40000000)`. The BIOS is
  mapped at 0 and mirrored at 0x40000000. All registers and STATUS32 are 0 at reset, and
  MAME resets them all.
- A delay-slot instruction (`.D` branches) gets its own line. The branch target comes
  after it.
- The zero-overhead loop jump back to LP_START is not an instruction, so it has no line of
  its own. LP_COUNT is decremented after the instruction that ends at LP_END. You see the
  new value on the next line.
- A taken interrupt shows up as the next line having PC = vector (INTVECTORBASE + 8*n).
  MAME checks interrupts only when no delay slot is pending.
- While the CPU sleeps (debug ZZ bit), MAME calls `debugger_wait_hook`, not the
  instruction hook, so **sleep cycles produce no lines**. If the trace ends up with fewer
  lines than requested, the CPU probably slept until the `-s` cap.
- The trace is deterministic. Two runs of the 200k uni15 trace were byte-identical. The
  system EEPROM is not NVRAM-backed in this driver, so every run is a cold boot with a
  blank EEPROM. The script also points cfg/nvram/etc. at a throwaway work dir and uses
  `-noreadconfig`.
- Inserting a cart changes execution from line 326 on (cart-present bit C in
  `0x01809004`).

## How the tracing works (and the gotchas)

`mame_trace.sh` writes a debugger script and runs:

```
QT_QPA_PLATFORM=offscreen SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
leapster leapster -bios uni15 -noreadconfig -rompath ... -video none -sound none -nothrottle \
  -debug -debugger qt -debugscript trace.cmd -seconds_to_run 120 [-cart X.bin]
```

with `trace.cmd`:

```
dasm WORK/first.dasm,pc,1,0
trace WORK/trace.fifo,maincpu,noloop,{tracelog "%08X ... %08X | ",pc,status32,r60,lp_start,lp_end,r0,...,r31_blink}
tracelog "FIRST %08X ... %08X | \n",pc,status32,...
step #199999
trace off
quit
```

Gotchas:

1. **`-debugger none` does not work.** The `none` module calls `go()` inside
   `wait_for_debugger()`, before `process_source_file()` gets a chance to read the script,
   so the script never runs. The Qt debugger with `QT_QPA_PLATFORM=offscreen` is fully
   headless and works. No X or Wayland is needed.
2. **The first instruction is never traced.** `-debug` stops at the first instruction
   hook, and the `trace` command runs during that stop. The script therefore writes that
   line itself: a `FIRST` tracelog line holding the reset registers, plus a `dasm` of the
   instruction. A small filter splices them into line 1.
3. **Debugger numbers are hex.** `step 200000` runs 0x200000 = 2,097,152 instructions.
   Use `step #200000`.
4. `noloop` is required. Without it the tracer folds loops into `(loops for N
   instructions)`.
5. `tracelog` output (no newline) comes before the tracer's own `PC: disasm\n`, which is
   what yields one line per instruction.
6. The trace goes to a FIFO read by `python3 | gzip -1`, so nothing large touches the
   disk. The work dir is `tools/oracle/traces/.work.XXXXXX`, deliberately not `/tmp`. On
   this machine `/tmp` is a 16 GB tmpfs, and a 1-emulated-second trace (tens of millions
   of lines) filled it. The debugger's parser mangles paths containing spaces or commas.
7. Speed is about **25-28k instructions/s** (200k in about 8 s of wall time). That is 3-4
   orders of magnitude slower than real time, because `tracelog` re-parses 37 expressions
   per instruction. **Always cap by instruction count.** 1 emulated second is up to about
   96M instructions, roughly an hour and 35 GB uncompressed. For multi-million-instruction
   traces, the next step would be a small C++ trace hook in the fork.
8. MAME silently falls back to the default BIOS when given an unknown `-bios` name. The
   script checks the name against `-listxml` first.
9. The Leapster sound device prints `Wrote XXXX to YYY` and `Set volume` to stdout. The
   script sends MAME's stdout to the work dir log. Use `-k` to keep it.

## Snapshots (what the BIOS shows)

`-N` runs without the debugger, using `-video soft` on SDL's offscreen driver. When
`-seconds_to_run` expires, MAME saves the final frame (160x160 native) as a PNG.

- `snapshots/leapster_uni15_2s.png` (and a 4x `_x4` version): black screen with a white
  "+" at about (10,10) and a small blue **hourglass** at the centre, about (73-82, 72-83).
- `snapshots/leapster_uni15_15s.png`, `_20s.png`: only the white "+" at the top-left.
  This is the **touch-screen calibration** screen, drawn in display format 3 (8bpp 332).
  The driver returns `0x63FFBFFF` from `0x01809004`, where bit 14 ("T") is clear, which
  makes the BIOS boot into touch calibration. It waits for a touch at that target. With
  Pet Pals inserted, the result is the same.
- `ger21` (bad dump) and `leapstertv` show an all-black screen at 15 s.
