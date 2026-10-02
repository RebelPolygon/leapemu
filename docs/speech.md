# Leapster speech (sound voice 7, "LFC")

Voice 7 of the Leapster sound chip plays compressed speech. LeapFrog calls the format
**LFC**. LeapFrog-Tools calls it "LPC", and MAME's notes call it "CELP". The same
hardware and format are used by the LeapPad line.

leapemu's decoder is `src/core/celp.{h,cpp}` (`leap::CelpDecoder`). The offline tool is
`tools/speech/lfc2wav.cpp`.

**Status:**
- The format is fully decoded, and every game that speaks does so in leapemu.
- The decoder produces intelligible speech (see "Validation"), but it is not known
  to be bit-exact with the chip (rounding, saturation and level are open questions,
  §8).

Sections are marked by the evidence behind them:
- **[confirmed]**: checked against the BaseROM's or cartridges' data, or against the
  emulator's behaviour running the BaseROM. Not against a real Leapster: no speech
  from hardware has been recorded and compared.
- **[hypothesis]**: consistent with everything seen, but not proven. The synthesis
  arithmetic (rounding, saturation, the end of a stream) is in this class, although
  the bitstream format around it is confirmed (§4).
- **[unknown]**.

---

**Contents**
- [1. Prior work and sources](#1-prior-work-and-sources)
- [2. Hardware interface](#2-hardware-interface)
  - [Registers [confirmed from BaseROM disassembly and traces]](#registers-confirmed-from-baserom-disassembly-and-traces)
  - [Other audio-init writes [unknown purpose]](#other-audio-init-writes-unknown-purpose)
  - [Codebook [confirmed]](#codebook-confirmed)
- [3. Where streams live [confirmed]](#3-where-streams-live-confirmed)
- [4. Bitstream format [confirmed]](#4-bitstream-format-confirmed)
  - [Details not verified against hardware [hypothesis]](#details-not-verified-against-hardware-hypothesis)
- [5. Validation](#5-validation)
- [6. How `Machine` drives the decoder](#6-how-machine-drives-the-decoder)
- [7. Tools](#7-tools)
- [8. Open questions](#8-open-questions)

## 1. Prior work and sources

| Source | What it gives |
|---|---|
| [nathanfarlow/leapfrog-voice-decoder](https://github.com/nathanfarlow/leapfrog-voice-decoder) (OCaml, "LFC to WAV") | **The key source.** It is a complete reverse-engineering of the bitstream and synthesis. Its tables are hand-transcribed; the author notes that some values are "off by one or two". leapemu instead reads the tables from the BaseROM codebook, where they are exact. |
| [ResHax thread "LeapFrog LPC/LFC format"](https://reshax.com/topic/985-leapfrog-lpclfc-format/) | Early community notes: the first u16 is a "compression level", streams end with `C0 0F`, and the codec is probably LPC. No decoder. |
| [LeapFrog Wiki: CELP Compression](https://leapfrog.fandom.com/wiki/CELP_Compression) | Marketing-level description: CELP, 8 kHz, "2–4 kbps". The bitrate is **not** accurate for Leapster content (see §4). |
| [lfhacks/Leapster-Tools](https://github.com/lfhacks/Leapster-Tools) (LeapSplit; successor to [BLiNXthetimesweeperGOD/LeapFrog-Tools](https://github.com/BLiNXthetimesweeperGOD/LeapFrog-Tools)) | The ROM asset tables. Asset id 4 is the LFC table. Its `bin4/bin6/bin8` names are the 4/6/8-pulse modes. |
| [toadster172/mame@leapster, `leapster_a.cpp`](https://github.com/toadster172/mame/blob/leapster/src/mame/leapfrog/leapster_a.cpp) | Voice layout and the 20224-byte codebook note. MAME has an unmapped `celp_codebook_w` and no decoder. Its LeapPad driver (`leappad.cpp`) is an 8051 skeleton with no sound. |

**Codec identity [confirmed from the format; the vendor is unknown]:** this is not a
standard codec. It is not FS-1016 CELP, G.728 or G.729, and no matching Sensory, TI or
DSP Group format is known. It is a small LeapFrog-specific analysis-by-synthesis coder:
- a 10th-order lattice synthesis filter with scalar-quantised reflection coefficients
  (38 bits per frame);
- excitation per 4 ms sub-frame, either:
  - *multi-pulse*: 4, 6 or 8 signed pulses, a gain index, and a 6-bit vector-quantised
    "shape" giving the other pulses' amplitudes relative to the loudest; or
  - one of 256 fixed noise vectors (the stochastic "codebook" part of "CELP").
- no long-term (pitch) predictor or adaptive codebook: pitch is carried by the explicit
  pulse positions.

The noise table is exactly what BSD `random()` gives with its default seed, using
`Q15((random() % 2000 − 1000) / 1000)`. This was verified bit-exactly against the ROM
(8192 of 8192 values), which suggests it came out of a plain C/Unix encoder toolchain.

No patent or other public reference to the codec was found.

---

## 2. Hardware interface

### Registers [confirmed from BaseROM disassembly and traces]

| Address | Width | Written by | Meaning |
|---|---|---|---|
| `0x0180'20e0` | 16 | BaseROM audio init (`0x40024f08`) | Codebook page, low half: `(codebook_ptr >> 15) & 0xffff`. The v1.5 BaseROM writes `0x8000`. |
| `0x0180'20e4` | 16 | BaseROM audio init (`0x40024f10`) | Codebook page, high half: `codebook_ptr >> 31`. The v1.5 BaseROM writes `0`. |
| `0x0180'40fc` / `0x0180'4100` | 16 | `0x40028d72` / `0x40028d7c` | Voice 7 start address, high and low halves. This is the same slot layout as voices 0–6 (`0x0c4 + 8·7`). The value is the **LFC stream address** (a cartridge address such as `0x800a521c`). |
| `0x0180'4158` | 16 | `0x40025cfa` | Voice 7 volume (`0x13c + 4·7`). Games use `0x7fff`, `0x7a83` and `0x5e35`. |
| `0x0180'41c0` | 16 | `0x40025d06` | Always written with the same value as `0x158`. The voices 0–6 equivalent is `0x1a4 + 4n`. The purpose is **[unknown]**: perhaps a second channel (L/R) or a volume-ramp target. |
| `0x0180'2070` | 8/32 | `0x40024d00` | Command register. `0x10<<3 \| 7 = 0x87` starts voice 7. Voice 7 was never seen being stopped: it runs to its end marker. |
| `0x0180'2078` | R | driver | Active-voice status. **Bit 6 = voice 7 busy.** The audio task polls this bit to sequence speech (see §5). |

The codebook address is derived as follows:
- The BaseROM looks up the codebook resource (id `0x10040001`; a string in the BaseROM names it
  `kAudioRsrcLFCCodebook`). Its resource
  table entry at BaseROM offset `0x298` holds `0x40000900`.
- It asserts `(ptr & 0x7fff) == 0x0900`.
- It writes `ptr >> 15` to `0x20e0` and `0x20e4`.

Only the 32 KiB page number reaches the chip, so **the hardware must add the fixed
offset 0x900 itself** (`codebook = ((hi<<16 | lo) << 15) + 0x900`). This is why the BaseROM
asserts on the low 15 bits. The rule is **[hypothesis]**, but it is the only reading that
fits the assertion.

### Other audio-init writes [unknown purpose]

- `0x0180'4000 – 0x404c` (stride 4, 20 halfwords) are written once at init with the
  constants `d225 e4b5 6466 c0ce e43e daaa 6848 be50 bf21 dad3 4bb1 eada d8b3 cde1 30d6
  bd0f ef6e cf76 1eb9 925a`. They do not look like a symmetric FIR filter. They could
  be a key, a DSP microcode patch or noise seeds. Speech decodes correctly without them.
- `0x0180'20f4` is written `8` before those writes and `0x104` after, and later
  `0x3000` / `0xc000`. It looks like a control or mode register.
- `0x0180'20f8` is read, then written `0x10`. `0x0180'2000` is written `0x10`.
  `0x0180'20f0` is a read-modify-write byte mask (`0x400269ec`), probably amplifier or
  mute GPIO.
- `0x0180'41c4` is written `0x2cc1` / `0x1734`: the master volume
  ([hardware.md](hardware.md#sound-0x01804000-16-bit-registers)). `0x170–0x180` and `0x184–0x1a0` are
  written `0x0400` / `0x7fff`.

### Codebook [confirmed]

- The codebook is 0x4f00 bytes at BaseROM offset **0x900–0x57ff**.
- It is **byte-identical in all ten system-ROM dumps** examined:
  - Leapster v1.5, `am29pl160cb`, UK, German and Spanish Leapster ROMs;
  - LeapPad, LeapPad Canada, My First LeapPad (both), Little Touch LeapPad.
- Its MD5 is `d2047ddcb4332193e7d28441b7d0f23b`.
- All values are little-endian `int16` in Q15:

| Codebook offset | BaseROM offset | Size | Contents |
|---|---|---|---|
| `0x000` | `0x0900` | 64×3 | Pulse shape VQ, mode 0 (4 pulses): amplitude ratios of pulses 2–4, sorted descending |
| `0x180` | `0x0a80` | 64×5 | Pulse shape VQ, mode 1 (6 pulses) |
| `0x400` | `0x0d00` | 64×7 | Pulse shape VQ, mode 2 (8 pulses) |
| `0x780` | `0x1080` | 3×32 | Pulse gain (amplitude of the loudest pulse), one table per mode, descending |
| `0x840` | `0x1140` | 32 | Noise gain, descending from 2534 to 0 |
| `0x880` | `0x1180` | 32+16+16+8+8+8+8+8+4+4 | Reflection coefficient quantisers k0…k9 |
| `0x960` | `0x1260` | 48×15 | **[unknown]** 48 descending positive 15-entry vectors (3528–31828). They are not used by any stream in the 44 carts. They may be shape tables for a mode that was never shipped (3 × 16?). |
| `0xf00` | `0x1800` | 256×32 | Noise excitation vectors (BSD `random()`, see above) |

---

## 3. Where streams live [confirmed]

- The chain to the LFC table is:
  1. cartridge header at `0x100` ("Copyright LeapFrog");
  2. the `LEAP` RIB;
  3. group `0x1006` (assets);
  4. asset id **4**, which is the LFC table.
- The LFC table has `+8` = the first handle, `+12` = the count, and `+16` = `count`
  absolute u32 pointers.
- Handles are what the software asks for. For example, Pet Pals' first handle is
  32768, and handle 33169 is at `0x800a521c`.
- Every voice 7 start address seen in the Pet Pals traces is an entry in that table.
- All 44 carts have tables: 36,000+ streams in total, 7–38 minutes of speech per cart.
- For **~96 %** of streams, the decoded length ends **exactly** where the next table
  entry begins. Most of the rest end within 1–3 bytes of the next entry (padding) or
  before an unrelated asset. This confirms that the parse is byte-exact.
- LeapSplit's "scan until `C0 0F`" corresponds to the end-of-stream mask `0x0fc0`, but
  scanning for it can stop early inside data.

---

## 4. Bitstream format [confirmed]

- All fields are little-endian 16-bit words.
- Bit fields are taken **LSB first** within a single word; no field crosses a word
  boundary.
- A frame is 6 sub-frames × 32 samples = 192 samples = 24 ms at 8 kHz.

```
u16  mode                       0,1,2 = 4,6,8 pulses per pulsed sub-frame
u16  mask[0]                    \ header of frame 0
     filter[0]  (2 words)       /  (filter present only if mask has any present bit)
repeat for frame i = 0,1,...:
     if mask[i] is the end marker: stop
     u16 mask[i+1]; filter[i+1]    <- the NEXT frame's header comes first (read-ahead)
     sub-frame records of frame i, j = 0..5 in time order
```

**mask:**
- Bit `5−j` = sub-frame j present.
- Bit `11−j` = sub-frame j is pulsed; otherwise it is noise.
- Bits 12–15 are unused.
- A sub-frame that is not present is 32 zero samples and has no record.
- The **end marker** is a mask with no present bits and all six pulsed bits set, which is
  `0x0fc0` (bytes `C0 0F`).

**filter:**
- Word 0: `k0:5 k1:4 k2:4 k3:3`. Word 1: `k4:3 k5:3 k6:3 k7:3 k8:2 k9:2`.
- The fields are indices into the k tables.
- A frame without a filter keeps the previous coefficients.

**noise record (1 word):** `row:8 gain:5 sign:1`.
- Excitation: `e[i] = q15(±noise_gain[gain] · noise[row][i])`.

**pulse record:** word 0 = `gain:5 shape:6 …`.
- Mode 0 (2 words):
  - word 0 continues `pad:1 p0:4`;
  - word 1 = `s0 s1 s2 s3 (1 bit each) p1:4 p2:4 p3:4`;
  - positions are ×2 (even samples only).
- Mode 1 (3 words):
  - word 0 continues `p0:5`;
  - word 1 = `s0..s5 p1:5 p2:5`;
  - word 2 = `p3:5 p4:5 p5:5 pad:1`.
- Mode 2 (4 words): the same as mode 1, then:
  - bit 15 of word 2 is `s6`;
  - word 3 = `p6:5 p7:5 pad:5 s7:1`.
- Amplitudes: pulse 0 = `gain[mode][gain]`; pulse n>0 = `q15(gain · shape[mode][shape][n−1])`.
- The sign bit (1 = negative) is applied to each pulse.
- A later pulse at the same position overwrites an earlier one.

**Synthesis:**
- A 10-stage lattice runs for each sample x:
  ```
  f = x
  for j = 9 … 0:  f = sat16(f + q15(k[j]·b[j]));  b[j+1] = sat16(b[j] + q15(−k[j]·f))
  b[0] = f; out = f
  ```
- `q15(a·b) = (a·b + 0x4000) >> 15`, i.e. round half up with an arithmetic shift.
- Filter state is zeroed at stream start.

**Bitrates:** these are measured, averaged over carts.
- Mode 0 ≈ 8.4–9.2 kbit/s.
- Mode 1 ≈ 11.5–13.6 kbit/s.
- Mode 2 ≈ 14–15.5 kbit/s.

The "2–4 kbps" figure on the fan wiki is wrong for the Leapster. Games choose the mode per
title: Pet Pals is mostly mode 0, and Get Puzzled! and Ratatouille are all mode 2.

### Details not verified against hardware [hypothesis]

- **Rounding.** The chip's exact rounding and saturation in the lattice are taken from
  the OCaml reference, which was presumably tuned by ear. There is no bit-exact
  hardware capture.
- **End of stream.** The decoder adds a 160-sample (20 ms) zero-input ring-down after
  the end marker before reporting idle, which avoids a click. The real chip may stop
  immediately. The OCaml reference also adds 32 samples of leading silence; leapemu
  does not.
- **Output level.** Decoded speech often reaches full scale, and a few clips saturate.
  How the chip scales voice 7 against the A-law voices is unknown.

---

## 5. Validation

1. **Tables [confirmed]:**
   - The ROM tables agree with the published OCaml tables in every value checked:
     the first and last values of k0, k9, gain[0], gain[2] and the noise gain.
   - The noise table was regenerated bit-exactly.
2. **Stream framing [confirmed]:** decoded lengths line up with table spacing
   (~96 % exact across 44 carts), and no stream has a mode above 2.
3. **Audio [confirmed]:** decoded clips are intelligible speech, with clean voiced
   harmonics (pitch ~150–300 Hz) and formants.
4. **In the emulator [confirmed]:**
   - Pet Pals speaks: the codebook is loaded from `0x40000900` at boot.
   - The audio task now waits for bit 6 of `0x2078` to clear before it starts the next
     line; the next start comes ~1–6 M cycles after the previous end. Without the
     decoder, lines were started back-to-back every ~130 ms.
   - Consecutive clips end exactly where the next asset begins (e.g. `0x8017f378` ends
     at `0x8017f446`, which is the next start).

---

## 6. How `Machine` drives the decoder

- **The codebook.** Writes to `0x0180'20e0` / `20e4` give the page of the codebook
  (low and high halves). `Machine::celp_load_codebook` copies the 20 KB codebook from
  there into the decoder. The codebook is in the BaseROM, so a copy made when the
  register is written is the same as the chip reading it live.
- **Start and stop.** A voice command (`0x0180'2070`) starting voice 7 calls
  `celp_.start(voices_[7].start)`; any other command for voice 7 stops it. The
  command is acknowledged with IRQ 0x1d like any voice's.
- **Busy.** Bit 6 of the active-voices register (`0x0180'2078`) reads
  `celp_.active()`. The system's audio task polls it before starting the next line;
  before the decoder existed, lines started back to back every ~130 ms.
- **Mixing.** Each 8 kHz voice sample adds the decoder's next sample, scaled by voice
  7's volume register (`0x4000` = unity) and a fixed factor. Games set voice 7 to
  `0x7fff`, raw sound effects to `0x30d9` and music voices to about `0x0300–0x1e7c`,
  so speech is the loudest source, which seems intended; the chip's real scaling of
  voice 7 against the A-law voices is unknown.
- **Save states** include the decoder's state and the codebook page.

## 7. Tools

```sh
cmake -S . -B build -DLEAPEMU_BUILD_TOOLS=ON && cmake --build build --target lfc2wav
build/lfc2wav --bios BIOS --rom GAME.zip --list                                                # handle, address, mode, bytes, seconds
build/lfc2wav --bios BIOS --rom GAME.zip --index 33169 -o out.wav                    # by handle
build/lfc2wav --bios BIOS --rom GAME.zip --addr 800a521c -o out.wav                  # by address/offset
build/lfc2wav --bios BIOS --rom GAME.zip --all outdir/                               # every entry
tools/speech/spectrogram.py out.png a.wav b.wav                                      # visual check
```

## 8. Open questions

- The purpose of sound registers `0x000–0x04c`, `0x170–0x1c4` and `0x20f0/0x20f4/0x20f8`.
- The codebook block `0x960–0xeff`.
- Bit-exact chip output (rounding and saturation) and start/end timing. A hardware
  recording of a known handle would settle these.
- Whether the chip raises the sound IRQ (0x1d) when voice 7 finishes. Polling alone is
  enough for Pet Pals.
- LeapPad cartridges were not examined. They are presumably the same format, since the
  codebook is identical.
