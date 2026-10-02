# Leapster hardware reference

This is the register-level model that `src/core/machine.cpp` implements. Most of it
comes from the reverse engineering by Alice Shelton in
[toadster172/mame@leapster](https://github.com/toadster172/mame/tree/leapster)
(BSD-3-Clause; see
[`leapster.cpp`](https://github.com/toadster172/mame/blob/leapster/src/mame/leapfrog/leapster.cpp)
and [`leapster_a.cpp`](https://github.com/toadster172/mame/blob/leapster/src/mame/leapfrog/leapster_a.cpp)).
The CPU follows the
[ARCompact ISA Programmer's Reference](http://me.bios.io/images/d/dd/ARCompactISA_ProgrammersReference.pdf). Our own findings are marked **(leapemu)**. Anything marked *unknown*
is an open question.

The model covers the "DART" SoC of the original Leapster, the L-MAX (a Leapster with a
TV output) and the LeapsterTV. The Leapster 2 adds an SD slot, USB and more RAM. None
of that is documented yet: no Leapster 2 BaseROM has been dumped and studied.

**Contents**
- [CPU](#cpu)
  - [The idle loop, and skipping it (leapemu)](#the-idle-loop-and-skipping-it-leapemu)
  - [Interrupts](#interrupts)
- [Memory map](#memory-map)
- [Peripherals (`0x0180'xxxx`)](#peripherals-0x0180xxxx)
  - [12 bpp pixel packing (LCD mode 4)](#12-bpp-pixel-packing-lcd-mode-4)
  - [Strap register `0x0180'9004`](#strap-register-0x01809004)
  - [Power switch and power-off (leapemu)](#power-switch-and-power-off-leapemu)
  - [ADC / touchscreen](#adc--touchscreen)
  - [Sound (`0x0180'4000`, 16-bit registers)](#sound-0x01804000-16-bit-registers)
  - [PCM output (observed in Schoolhouse Rock)](#pcm-output-observed-in-schoolhouse-rock)
- [System EEPROM contents (leapemu)](#system-eeprom-contents-leapemu)
- [Known divergences from MAME](#known-divergences-from-mame)
- [LeapsterTV (observed, not emulated)](#leapstertv-observed-not-emulated)

## CPU

- ARCtangent-A5 (ARCompact ISA) at 96 MHz, little-endian. 32-bit instructions are stored
  as two halfwords, high halfword first, and so are long immediates.
- Extensions in use: barrel shifter, `norm`/`swap`, and `mul64`; and the XY-memory DSP
  option, whose dual multiply-accumulate instructions (major 0x05, sub-ops 0x0c, 0x10
  and 0x14) appear in the BaseROM but are not run at boot. Schoolhouse Rock's sound
  mixer uses them ([below](#pcm-output-observed-in-schoolhouse-rock)).
- The core ignores the low address bits on misaligned 16/32-bit data accesses; this was
  tested on hardware. The Flash player relies on it: it has a null-pointer bug that reads
  a "pointer" from address 4.
- The reset vector is `0x4000'0000`, which is also the initial `INT_VECTOR_BASE`. The
  BaseROM moves the vector base to RAM later.
- `mul64` writes the low word to both `MLO` and `MMID` (toadster's finding).

### The idle loop, and skipping it (leapemu)

The system runs the MQX real-time OS. When no task is ready, its idle task spins in
a loop that reads the power/clock control register (`0x0180'9008`) on every pass.
Flash games spend about 80% of their time there, so executing it is most of the
cost of emulating them.

leapemu skips it, without changing what the system does:
- **Scheduling.** The machine is event-driven: the CPU runs until the next device
  event (a timer overflow, an 8 kHz voice sample, a 32 kHz PCM sample, a touch
  sample), then the devices are updated (`Machine::run_cycles`). Devices only change
  state, and only raise interrupts, between these slices; nothing writes memory
  during one but the CPU itself.
- **The hint.** The instruction that reads `9008` is recorded as the idle-loop hint.
- **The test.** When execution reaches the hint twice in a row with identical
  registers, STATUS32 and loop registers (a hash of them), interrupts enabled and
  none pending, the loop has made no progress and cannot until an interrupt comes.
  Interrupts only come at the end of the slice, so the rest of the slice is skipped
  (`Cpu::run`, `idle_state_hash`): the time the loop would have spun passes at once.
- The hint moves: the RTOS reads `9008` from more than one place. Each new place
  becomes the hint (and, for the JIT, ends compiled blocks there, [jit.md](jit.md)).
- **Not bit-identical to spinning.** When the interrupt comes, the CPU is at the
  loop's head rather than somewhere inside the loop, so the registers it saves differ,
  and exact timings from then on shift a little. The system behaves the same, but the
  machine state is not the same as with the loop run (20 s of Pet Pals give different
  state hashes). That is why an input movie records whether skipping was on, and
  plays back with that setting ([tas.md](tas.md)). Every CPU backend skips in the
  same places, so they still agree with each other.
- `--no-idle-skip` (CLI) or Tools > Settings > Emulation turns it off.

### Interrupts

Vectors sit at `INT_VECTOR_BASE + 8*n`. Level-2 vectors are the ones selected in
`AUX_IRQ_LEV` (aux 0x200, reset value 0xc0). Priority is 7, 6, 31…8, 5, 4, 3.

| Vector | Source |
|---|---|
| 0x10 | ADC (touchscreen) FIFO data |
| 0x12 | Framebuffer DMA complete |
| 0x19 | Timer 0 overflow |
| 0x1a | Timer 1 overflow |
| 0x1b | UART (byte received, or transmitter ready) |
| 0x1d | Sound voice command acknowledged |
| 0x1e | Timer 2 overflow |

The interrupt source flag register `0x0180'0080` reports sources that share a line.
Reads OR in 0x80, which the EEPROM code polls as a ready bit, and ADC sets bit 8.
Writing 1s clears flags.

## Memory map

| Range | Contents |
|---|---|
| `0x0000'0000 – 0x007f'ffff` | BaseROM (2 or 8 MiB), also mirrored at `0x4000'0000` |
| `0x0180'0000 – 0x0180'ffff` | Peripherals (below) |
| `0x0300'0000 – 0x0300'ffff` | 64 KiB on-chip SRAM: framebuffer, plus the stack before MQX starts |
| `0x0301'0000 – 0x0302'ffff` | Write-ignored. The v1.5 BaseROM's screen clear overruns into it |
| `0x3c00'0000 – 0x3fff'ffff` | Main RAM. Real units have 2 MiB (16 MiB on Leapster 2); we map the whole window as MAME does |
| `0x8000'0000 – …` | Cartridge ROM (4–16 MiB) |
| `0xe000'0000` | Probed by the BaseROM at boot (reads the header at +0x100). Unmapped here; *unknown* (second ROM chip-select?) |

## Peripherals (`0x0180'xxxx`)

| Address | R/W | Function |
|---|---|---|
| `0030` | W | EEPROM command. Bits 31:16 = byte address, 15:8 = device (0x26 = system EEPROM, anything else = cartridge EEPROM), 7:0 = write data |
| `0034` | W | EEPROM control: writing 2 commits the write |
| `0038` | R | EEPROM data for the current command's address |
| `004c` | R | **GIODataIn**. Buttons, active low: Right 7, Down 8, Left 9, B 13, A 14, Vol− 15, Vol+ 16, Contrast 17, Bright+ 24, Bright− 25, Pause 26, Hint 28, Home 29, Up 31. **(leapemu)** Bit 11 is the power switch: 0 = switched off (below). The other bits carry power/dock state and must read 1 |
| `0080` | R/W | Interrupt source flags (see above) |
| `0084` | R/W | **(leapemu)** Interrupt enables for those flags. The BaseROM v1.5 sets `0x0005'4520` at boot by read-modify-write: 0x100 ADC, 0x20/0x400 power (below), 0x4000, 0x10000 and 0x40000 unknown |
| `0090–009c` | R/W | ADC channel controls 0–3. Channel 0 is the touchscreen; bit 15 enables it |
| `00a0` | R | ADC FIFO pop: `channel<<16 \| sample<<5` |
| `00a8` | R | ADC status. Bit 8 = 1 means the FIFO is empty |
| `1000–1018` | R | *unknown*, reads 0 |
| `2070` | W | Sound voice command: bits 2:0 = voice, bits 31:3 = command (0x10 = start, anything else = stop). The chip acknowledges with IRQ 0x1d |
| `2078` | R | Active voices: bit 7 = voice 0, bits 0–6 = voices 1–7 |
| `4000–4fff` | W16 | Sound voice registers (below) |
| `8088` | R/W | LCD framebuffer base, as an offset into `0x0300'0000` (16 bits) |
| `808c` | R/W | LCD stride in bytes |
| `8090` | R/W | LCD format. Bit 31 = enable, low bits = mode: **3** = 8 bpp RGB332 (calibration), **4** = 12 bpp, two pixels packed into three bytes |
| `8800` | W | DMA control. Writing 0x1b copies `lines × stride` words from the source to VRAM (framebuffer base + start offset), then raises IRQ 0x12 |
| `8804/8808/880c/8810` | W | DMA source address / stride in words / line count / start offset |
| `9004` | R | Configuration straps (below) |
| `9008` | R/W | CPU power / clock control. The MQX idle task reads it on every pass of its wait loop and writes 1, 5 and 0 to it (probably lowering the clock while idle). MAME calls it a clock divider. **(leapemu)** Not modelled; leapemu uses the polling instruction as its idle-loop hint |
| `b000/b004/b008` | R | *unknown*. We return 0 / 0xffffffff / 1 so the BaseROM proceeds |
| `b000` | W | **(leapemu)** Bit 8 set = power off: the BaseROM's last act when it shuts down (below) |
| `c000`–`c024` | W | LCD controller set-up, written once at boot: `c000`/`c004`/`c008` = 0xcafe00ff / 0xcafe1027 / 0xcafe203f, `c00c`/`c010` = 0xcafe3027 / 0xcafe4027, `c014` = `c018` = `c01c` = 1, `c020` = 0xbf then 0x35, `c024` = 0x80 then 0x30. Only the two below change later |
| `c020` | W | Written 0x35 at boot; each Contrast press (bit 17, a toggle) switches it between 0x35 and 0x95. Effect unknown. **(leapemu)** Stored, not shown |
| `c024` | W | **Brightness** (LCD level), from the display setting (below): 0x30 by default, 0x24–0x3d over the setting's range. Each Brightness Down press lowers it by 1–2, each Brightness Up press raises it by 1–2. **(leapemu)** Shown as a shade over the screen: lighter above 0x30, darker below. The strength is an estimate; how the real panel responds has not been measured |
| `d084/d088/d08c` | R/W | Timer 0: count / control (bit 0 = IRQ enable) / reload limit |
| `d400/d404/d408` | R/W | Timer 1 |
| `d800/d804/d808` | R/W | Timer 2 |
| `d510` | W | UART transmit: the MQX debug console (Tools > Debugger > UART Console in the GUI) |
| `d514` | R/W | UART status. Reads 0xa0 (TX ready, RX empty); writing 0x44 requests a TX-ready interrupt |

The timers count up at 16 MHz, which is 6 CPU cycles per tick. When a timer reaches its
limit it wraps to 0 and, if enabled, raises its interrupt.

### 12 bpp pixel packing (LCD mode 4)

Each 3-byte group `b0 b1 b2` holds two pixels:
`p0 = R:b1[7:4] G:b0[7:4] B:b0[3:0]` and `p1 = R:b1[3:0] G:b2[7:4] B:b2[3:0]`.

### Strap register `0x0180'9004`

Bit layout, per toadster's notes: `UUUU UCSS SLDU UUUU FTUU UUUU UUUP UUUQ`.

- **C (bit 26):** 1 = no cartridge inserted.
- **S:** LCD panel variant.
- **L, D:** logging level, and debug behaviour on errors.
- **F (bit 15):** clear means Flash runs uncapped at 10× the SWF frame rate.
- **T (bit 14):** **(leapemu)** When clear, the BaseROM runs the factory touch
  calibration (five crosshairs in LCD mode 3) and stores the result in the system
  EEPROM. When set, it uses the stored calibration, even if there is none, in which
  case touch input is garbage.
  - leapemu reports T set, except when the EEPROM's calibration area is blank.
  - While calibrating, leapemu taps the crosshairs automatically (`auto_calibrate`).
  - MAME returns `0x63ffbfff` (T clear), so it recalibrates on every boot.
- **P, Q:** checked by PEG-engine games.
- **Bit 21:** **(leapemu)** set = the system can turn its own power off. The
  BaseROM's power-off routine reads it: if clear, it spins forever instead of
  writing `b000` bit 8. leapemu reports it set.

### Power switch and power-off (leapemu)

Found by tracing the BaseROM v1.5 (the three GIO bits a power monitor watches, then
holding each low):
- A BaseROM power monitor runs about every 0.8 s (a periodic callback posts its
  event, flag 0x10, on each pass). It watches three GIODataIn inputs,
  each described by a mask and a polarity in its state, and acts on a change seen on
  two consecutive checks:
  - bit 11, active low: **the power switch** (confirmed);
  - bit 30 and bit 12: unknown, perhaps battery or dock state. Holding either low
    changed nothing visible.
- Switching off makes the BaseROM:
  1. set up GPIO outputs (`0x0180'0040`–`0x0180'0060`);
  2. save its settings to the system EEPROM (addresses `0x60`–`0x63`);
  3. play its power-off animation: black, a white flash, then a CRT-style collapse to
     a line and a fading dot (about 2.4 s);
  4. set bit 8 of `0x0180'b000`, which cuts the power.
- The BaseROM also powers off by itself after about 15 minutes without input,
  through the same sequence (Pet Pals, idle from power-on).
- leapemu then stops the CPU and shows a black screen until reset (switching on).
- **The power interrupt (line 0x18).** Its handler (BaseROM `400800cc`) reads
  `0x0180'0080`: flag 0x20 or 0x400 masks both (and 0x1) in `0x0180'0084` and calls
  the shutdown routine `40080df0` with reason 1 or 2. That routine records the
  reason, marks a shutdown in progress and wakes the shutdown task. The monitor's
  bit-11 path calls it with reason 4.
- Real units shut down almost as soon as the switch moves, even in the middle of
  a game. The polled path cannot do that: the monitor runs in a system task that a
  busy native game can hold off (Sonic X only let it run at the end of a minigame).
  So leapemu treats switching off as the hardware interrupt: flag 0x20 on line 0x18
  (when enabled), besides bit 11 reading low. Which of flags 0x20 and 0x400 the
  switch really raises is not known (both shut down the same way); 0x20 is used.
  The animation then starts within a few frames.

### ADC / touchscreen

- When channel 0 is enabled, the BaseROM expects 17 packets whose contents it ignores.
- After that, each touch sample is 12 packets: `p p x y p p p p x y p p`. Here `p` is
  pressure: 0 while touching, 0x7ff when not. `x`/`y` are raw 11-bit ADC values in the
  range 0..0x7f0.
- We produce one sample at 60 Hz.
- The calibration fits any linear mapping. leapemu maps screen pixels linearly onto
  0..0x7f0.

### Sound (`0x0180'4000`, 16-bit registers)

- **Voices 0–4:** pitched A-law samples.
  - Start address: high halfword at `0c4 + 8n`, low halfword at `+4`.
  - The high 16 bits of `end` (at `104 + 8n`) are the loop point; the low 16 bits are
    the loop target.
  - Pitch at `15c + 4n` is 1.15 fixed-point bytes per 8 kHz output sample.
- **Voices 5–6:** raw 8 kHz A-law from start to end.
- **Voice 7:** LFC speech (a custom CELP-style codec), implemented in
  `src/core/celp.cpp`. The codebook page goes to `0x0180'20e0/e4`, and bit 6 of
  `2078` means "speech busy". See `docs/speech.md`.
- **Volume:** at `13c + 4n`, where 0x4000 = unity. The BaseROM also writes the same
  values to `1a4 + 4n` (purpose unknown).
- **Master volume (leapemu):** `1c4`, from the volume buttons' setting (system EEPROM
  `0x62`). Found by booting with each setting and logging writes. Roughly 3 dB per
  step:

  | Setting | 0–8 | 16–24 | 32–40 | 48–56 | 64 | 72 | 80 | 88 (default) | 96 | 104 | 112 | 120 | 128 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|---|
  | `1c4` | 0 | 0x00fe | 0x0200 | 0x0407 | 0x081d | 0x0b84 | 0x1059 | 0x1734 | 0x20f0 | 0x2ec1 | 0x425e | 0x5e35 | 0x7fff |

  A volume press ramps the register towards the new value. leapemu scales the mix by
  it with 0x4000 as unity, like the voice volumes; the absolute level on hardware is
  not measured.
- The sound hardware appears to be shared with the LeapPad line.

### PCM output (observed in Schoolhouse Rock)

Games that stream sound (Schoolhouse Rock's soundtracks) use two DMA channels that play
16-bit samples at 32 kHz from circular 512-byte buffers (256 samples), besides the voices.
leapemu mixes them with the voices into its 32 kHz output. (The voices still run at 8 kHz
and each of their samples is output four times, so games that do not use the channels
sound exactly as before.)
- **Registers:**
  - channel 0: `0x0180'3000` control, `0x0180'3004` buffer address;
  - channel 1: `0x0180'3008` control, `0x0180'300c` buffer address.
- **Control:** 0 stops a channel, 0x7f01 starts it.
- **Position:** reading an address register gives the channel's current position in its
  buffer (the low 9 bits).
- **Interrupts:** interrupt flag bits 20 (channel 0) and 21 (channel 1) in `0x0180'0080`,
  masked in `0x0180'0084`. The game makes vector 0x13 level 2 (`AUX_IRQ_LEV` bit 19).
- **Enable:** bit 10 of sound register `0x0180'20f4` is set while the channels run.
- **Buffers:** the game's are in video RAM (`0x0300'ee00`, `0x0300'f200`). Its interrupt
  handler refills the half not being played, 128 samples at a time.
- **The rate:** 32 kHz. The mixer upsamples a stream 8 times (three doubling filter
  stages) and then resamples it with a step of 2.75625 for 11025 Hz streams (88200 /
  32000) or 2.0 for 8000 Hz ones. At 32 kHz the game consumes a soundtrack at exactly its
  own rate (2756 bytes a second, as its video's length requires).
- **The channels' use:** in Schoolhouse Rock channel 1 carries the music and channel 0 is
  silent.

**The XY-memory DSP option** (`src/core/arc/xy.{h,cpp}`). The refill mixes and resamples
the streams with it: XY memory with address generators, a burst DMA to and from system
memory, core registers 32–55 as XY-memory ports, and dual 16×16 multiply-accumulate
instructions. The names come from the GNU binutils ARC tables; how they behave is not
publicly documented. leapemu's model:

| Part | leapemu's model |
|---|---|
| Ports r32–r47 (`x0_u0` … `y3_u1`) | the word at pointer AX*n* / AY*n*, which then moves by its modifier 0 or 1 |
| Ports r48–r55 (`x0_nu` … `y3_nu`) | the same, without moving |
| Pointers AX0–3, AY0–3 | aux `0x80`–`0x87` |
| Modifiers MX00 … MY31 | aux `0x88`–`0x97`. Bits 15–0 are a signed step; bits 27–16 a wrap-around length in the pointer's units (0: none); bit 29 16-bit data (the pointer counts halfwords, and a value is in the upper 16 bits) |
| Wrap-around buffers | start at the pointer rounded down to a power of two at least as long |
| Bursts | BURSTSYS (aux `0x99`) is the system address, BURSTXYM (`0x9a`) the XY word address, BURSTSZ (`0x9b`) the byte count − 1, with bit 30 for system → XY and bit 29 for Y memory rather than X. They finish at once; XYCONFIG (`0x98`) bit 4 is busy |
| `muldw` / `macdw` / `msubdw` (major 5, sub 0x0c / 0x10 / 0x14) | two lanes of Q15 × Q15 → Q31, saturating, into ACC1 (r56, upper lane) and ACC2 (r57, lower lane); the result is each accumulator rounded to its upper 16 bits. This is what MACMODE (aux `0x41`) = 0x0c is taken to select |

The checks:
- the mixer's input is exactly the codec's output, sample for sample;
- the mixer's 32 kHz output matches a straightforward resampling of the codec's output
  (correlation 0.9996, 2.8% difference);
- the soundtrack is consumed at exactly real time;
- results are identical with all three CPU cores.

The filter itself makes these details checkable: it upsamples ×8 through three
half-band stages (17, 3 and 2 coefficients per side, each side summing to 0.5). Other
readings of the 16-bit data, the wrap-around lengths or the burst banks give output that
correlates at most 0.87.

Details that the output does not pin down (the rounding, and modes other than 0x0c) may
still differ from the hardware.

## System EEPROM contents (leapemu)

The system EEPROM is 512 bytes (device 0x26). The v1.5 BaseROM stores the touch
calibration twice, at 0x00 and 0x30, apparently as a redundant copy.

The button settings are at 0x60–0x63, written when the console powers off. Found by
pressing each button, switching off, and comparing the EEPROM:

| Address | Contents |
|---|---|
| 0x60 | display setting: Brightness Down +10, Brightness Up −10 per press (50 by default; Contrast left it unchanged in the test) |
| 0x61 | 100; unchanged by every button |
| 0x62 | volume, steps of 8 (88 by default, 0–128); see the master volume above |
| 0x63 | check byte: 0x60–0x63 sum to 0xFF |

The rest of the layout is unknown. At power-on the BaseROM turns the display setting into
the brightness register `c024` (found by writing each value to 0x60, with a matching check
byte, and logging the register):

| 0x60 | 0–10 | 20 | 30 | 40 | 50 | 60 | 70 | 80 | 90–100 |
|---|---|---|---|---|---|---|---|---|---|
| `c024` | 0x3d | 0x3a | 0x37 | 0x34 | 0x30 | 0x2d | 0x2a | 0x27 | 0x24 |

With no cartridge inserted, the BaseROM shows an animated "insert cartridge" screen: an
arrow pointing into the Leapster's slot.

## Known divergences from MAME

leapemu executes the first ~2.9 M instructions of the v1.5 BaseROM identically to the
oracle, including 4 interrupts; see `tools/oracle/`. The first difference is a 1-tick
difference in a timer count read, caused by scheduler granularity. The core
deliberately differs from MAME in these places, all marked `MAME-DIFF` in the source:

- **`ADC` / `SBC` carry:** we compute the carry with the full carry-in. MAME computes
  it from `src2 + C`, so it is wrong when `src2 = 0xffffffff`.
- **Multi-bit `ROR`:** MAME rotates by `31 − n` instead of `32 − n`.
- **Loop end on a taken branch:** the zero-overhead loop does not trigger when a taken
  32-bit branch lands on `LP_END`.
- **`AUX_IRQ_LEV` reads:** they return the register; MAME returns 0. `--mame-compat`
  restores MAME's behaviour.
- **Strap bit 14:** set by default (see above).

## LeapsterTV (observed, not emulated)

The LeapsterTV BaseROM (`152-11594`, v2.1.11) differs from the handheld's in two ways
that leapemu does not model yet, so it shows a blank screen.
- **A serial device at `0x0180'd600`.** A periodic driver sends it commands and reads its
  replies: data at `+0x10`, status at `+0x14`. Bit 7 means the device is present and bit 5
  that a byte is ready. Replies are checked against `0xAA` (an acknowledgement) and
  `0x5X` codes. This is most likely the TV unit's wired controller. With nothing there,
  the driver waits forever for bit 5. Answering "present, acknowledged" lets the BaseROM
  run on, idle.
- **The picture.** It writes the `0x0180'd700` block about 100 times a second (the
  handheld BaseROM: once at start) and hardly uses the LCD framebuffer DMA. It also
  touches `0x2400'000C` / `0x2400'001C`, which nothing else uses. Its frames go out
  through a TV video path whose registers are not known yet.

Emulating it means working out both from the BaseROM's code, and a capture from real
hardware would help to check the result.
