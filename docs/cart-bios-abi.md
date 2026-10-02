# Cartridge ↔ BaseROM interface (clean-room specification)

This document is the handoff for a future **open, clean-room replacement BaseROM**. It
describes *behaviour and data formats only*: what a cartridge expects from the system,
and what the system must do to start a cartridge.

**Rules for this file**

- **Never paste BaseROM code, disassembly, or constants copied from BaseROM code.** Write
  observations as behaviour ("the system reads X and expects Y"), with how each one was
  observed.
- Whoever implements the open BaseROM should work only from this file,
  `docs/hardware.md`, and public format documentation. They should not look at the
  original BaseROM disassembly.
- Sources we can cite freely:
  - [Leapster-Tools / LeapSplit](https://github.com/lfhacks/Leapster-Tools) (ROM format)
  - [toadster172/mame@leapster](https://github.com/toadster172/mame/tree/leapster) (hardware)
  - leapemu's black-box observations and hardware tests

Status: **early research**. Most of the ABI is still unknown, and no open BaseROM exists;
see [open-bios/CLEANROOM.md](open-bios/CLEANROOM.md) for what is and isn't covered.

Counts below (44, 54 cartridges) are of the dumps tested when each part was written;
62 retail cartridges are known now.

**Contents**
- [1. ROM image layout (BaseROM and cartridges share it)](#1-rom-image-layout-baserom-and-cartridges-share-it)
  - [AppTable header at image offset 0x100](#apptable-header-at-image-offset-0x100)
  - [RIB table](#rib-table)
  - [Product info entries (group 0x1003)](#product-info-entries-group-0x1003)
  - [Asset tables](#asset-tables)
  - [Bitmaps (asset types 9 and 0xD) and video (0xE)](#bitmaps-asset-types-9-and-0xd-and-video-0xe)
  - [Fonts (asset type 1)](#fonts-asset-type-1)
  - [SYN music (asset type 6)](#syn-music-asset-type-6)
  - [Development upload images (some prototypes)](#development-upload-images-some-prototypes)
- [2. Boot behaviour observed from the outside](#2-boot-behaviour-observed-from-the-outside)
- [3. Services a cartridge expects from the system](#3-services-a-cartridge-expects-from-the-system)
- [4. Persistent data](#4-persistent-data)

## 1. ROM image layout (BaseROM and cartridges share it)

Source: [Leapster-Tools / LeapSplit](https://github.com/lfhacks/Leapster-Tools). Verified
by leapemu `--info` on 44 cartridges and the v1.5 BaseROM.

### AppTable header at image offset 0x100

| Offset | Size | Field |
|---|---|---|
| 0x100 | 23 | ASCII `"Copyright LeapFrog     "` |
| 0x117 | 1 | 0 |
| 0x118 | 1+1 | table minor / major version |
| 0x11a | 2 | RIB count |
| 0x11c | 4 | device start address (where the image is mapped: cartridges `0x80000000`, BaseROM `0x40000000`) |
| 0x120 | 4 | device end address |
| 0x124 | 4 | pointer to full checksum |
| 0x128 | 4 | pointer to sparse checksum |
| 0x12c | 4 | "boot safe" function table (always 0 in known images) |
| 0x130 | 16 | reserved |
| 0x140 | 4 | address of the RIB ("LEAP") table |

### RIB table

- **Header:** `"LEAP"`, minor, major, u16 group count, then 24 reserved bytes.
- **Group entries:** u16 group id, u16 count, u32 address.
- **Known groups:**
  - 0x1000 boot
  - 0x1001 modules
  - 0x1003 product info
  - 0x1005 and 0x3001, both unnamed groups
  - 0x1006 assets
  - 0x1009 system apps
  - 0x100c Leapster datasets
  - 0x100d C-style datasets
  - 0x2000 apps

### Product info entries (group 0x1003)

Each entry is a u16 id, one byte each of minor and major version, and a u32 value. The
value is a pointer except for ids 1 and 8.

| Id | Meaning | Id | Meaning |
|---|---|---|---|
| 1 | product id | 2 | needed product ids |
| 3 | provided product ids | 4 | "copyright" string (a verse used as a signature) |
| 5 | security string | 6 | base-system boot |
| 7 | version string | 8 | ROM version |
| 0xa | part number | 0xb | part name / title |
| 0xc | build tool | 0xd | build user |
| 0xe | build machine | 0xf | build timestamp |
| 0x10 | build validation | | |

### Asset tables

Asset tables (SWF, SYN music, A-law RAW, LPC/CELP speech, GAS, instruments, PEG and
Flash bitmaps) are documented in
[LeapSplit](https://github.com/lfhacks/Leapster-Tools) and the
[SYN format gist](https://gist.github.com/BLiNXthetimesweeperGOD/3959b63c80ca144becd5543b03c0e805). Each table begins with 8 unknown bytes, a
u32 first-handle value and a u32 count, followed by pointers.

The asset group's entries are a u16 type, 2 bytes, and a u32 pointer to that type's
table. The types, as LeapSplit names them:

| Type | Assets | Type | Assets |
|---|---|---|---|
| 1 | Flash fonts | 6 | SYN music |
| 2 | instruments | 7 | SWF (Flash) movies |
| 3 | GAS audio | 9 | PEG bitmaps |
| 4 | speech (CELP, see speech.md) | 0xd | Flash bitmaps |
| 5 | A-law audio (8 kHz) | 0xe | video (AVI) |
| | | 0xf | soundtracks (streamed; see hardware.md) |
| | | 0x10 | cursors |

In the 54 cartridge dumps tested with leapemu:
- the type 7 assets start with `FWS` (uncompressed SWF);
- most type 5 assets start with `0xD5`, A-law silence.

Only a SWF records its own length (in its header).

### Bitmaps (asset types 9 and 0xD) and video (0xE)

Observed by leapemu. Every PEG bitmap and every 16-bit Flash bitmap of the 54 cartridge
dumps decodes to exactly its width and height.

**PEG bitmaps** start with a 16-byte header: u8 1, u8 bits per pixel, u16 width, u16
height, 6 zero bytes, and u32 the address of the pixel data (right after the header).
- Pixels are 16-bit 0x0RGB (4 bits each, the LCD's 12-bit colour) with 0xFFFF as
  transparent, or 8-bit RGB332.
- Data is a list of runs: a byte c below 0x80 is c+1 copies of the next pixel; otherwise
  c-0x7F pixels follow as they are.

**Flash bitmaps** start with a 20-byte header: u32 the address of the pixel data, u32
format, u32 width, u32 height, u32 bytes per row.
- **Format 2:** 16-bit pixels 0xTRGB, where T is transparency (0 opaque, 15 clear),
  coded per row:
  - `00 n`: n transparent pixels;
  - `01 n p`: n copies of p;
  - `02 n` or `03 n`: n pixels follow;
  - `04`: next row;
  - `05`: the end.

  Ratatouille's two also put skips (`00 n`) and runs (`01 n p`) among a literal's pixels.
- **Format 3:** 8-bit indexed pixels, uncompressed (0xFF is transparent).
  - Only Grundschule 2 uses it. The game builds the palette in RAM (256 RGB entries) when
    play starts, from values it computes; no table in the ROM matches it.
  - With that palette every one of the game's sprites comes out right, so one palette
    serves them all.

**Video** assets (Schoolhouse Rock) are a u32 0, a u32 length, then an AVI file (160x120,
12 fps, VP6, no audio track).

**Soundtracks** (type 0xF, Schoolhouse Rock) go with the videos:
- each is a u32 format (2), a u32 length, then the data;
- 64 bytes decode to 256 samples (2 bits per sample);
- at 11025 Hz each lasts as long as its video, to within 0.3%;
- they play through the PCM output channels (hardware.md);
- the decoder is the game's own code: a transform codec (band edges 0, 2, 4 … 124 of 128
  coefficients), about 22 kbit/s;
- the game registers it as an interface, entry 35 of the registry, table B: slot 0
  decode, 1 start, 2 stop.
  - Start is `(&handle)`. It allocates the codec's state, tagged `0xface`, and returns 0.
  - Decode is `(handle, descriptor)`. The descriptor holds a u32 output address, a u16
    sample count (256), a u32 input address and a u16 input length (64). It returns 0.
  - Stop is `(&handle)` and frees the state.
  - leapemu's ROM tools call these functions to export the songs (rom-tools.md).

### Fonts (asset type 1)

Observed by leapemu; every font of the 54 cartridge dumps decodes. These are the bitmap
fonts of the BaseROM's text engine (dynamic text).

| Offset | Contents |
|---|---|
| +0x04 | u32 address of the family name |
| +0x08 | u32 address of the full name |
| +0x10 | u8 height |
| +0x19 | u8 character set: 0x41 ISO 8859-15, 0x3F Mac Roman |
| +0x2C | 256 glyph records of 5 bytes: width, height, top (signed, above the baseline), left (signed), advance |
| +0x52C | 256 u32 addresses of the glyph bitmaps (0: none) |

A glyph bitmap has rows of (width+1)/2 bytes: 4-bit coverage, high nibble first.

### SYN music (asset type 6)

The command meanings come from the SYN format gist above. leapemu checked the layout
against all 2592 SYN assets of the 54 cartridge dumps (every track parses exactly to its
end marker), and filled in the rest:

- **Header:** u16 `0x0002`, u16 track count, then per track a u16 offset and a u16 id
  (big-endian).
- **Commands:**

  | Bytes | Meaning |
  |---|---|
  | `00`-`7F` d | a note of that pitch lasting d ticks; pitch 0 is a rest |
  | `81`-`84` | sequencer use, no operand |
  | `85` x | unknown; x is 0-7 |
  | `88` v | volume, set before notes as accents |
  | `89` p | program p of the BaseROM's bank |
  | `89 C0` n | the cartridge's own instrument n |
  | `8A` b d | pitch slide: b is centred on 0x80, over d ticks |
  | `8E` n | loop start: 127 = forever, else a count |
  | `8F 00` | loop end; loops do not nest |
  | `FF 00` | end of track |

- **Durations:** one byte, or `0x80|high, low` for longer ones.
- **Tick: 4 ms (250 Hz).** Clifford's title theme (7679 ticks) loops every 30.72 s in
  leapemu's audio. Its beat is 0.24 s (60 ticks), and a rendering from the SYN data lines
  up with the recording.
- **The BaseROM's bank follows General MIDI order.** Each program's note range fits its
  General MIDI instrument (basses low, flutes high, 120-125 sound effects). 126 and 127
  are drum kits: short notes on the General MIDI drum map (kick 36, snare 38).

### Development upload images (some prototypes)

The images of three prototypes (Go Diego Go March 2007, Cars Supercharged January 2007,
Pet Pals October 2006) are not plain dumps. At file offset 0x400000 (4 MiB) each has a
24-byte ASCII block header that is not part of the cartridge's address space:

```
LBK00000014000000018a813        "LBK", then hex digits: the block's start (0x400000)
                                 and, in the last 8, its last offset (length - 1)
```

Everything after the header is 24 bytes later in the file than the cartridge's own
pointers say. Every asset past 4 MiB is affected, and none before it:

- Flash movies, bitmaps, cursors and SYN music all start 24 bytes after their table entry.
- The BaseROM's Flash player checks for `FWS` at the table address, so a movie stored
  past 4 MiB never starts and the game stops at the LeapFrog screen.

Removing the header fixes every asset at once, with no other change. leapemu does this
when it loads an image (`join_rom_blocks`), whatever its CRC. The CRC that identifies the
image (game list, saves, states, movies, patches) is still that of the file as dumped.
Patch offsets and the ROM viewer count without the header.

## 2. Boot behaviour observed from the outside

- The system checks bit 26 of the strap register to see whether a cartridge is
  inserted (see hardware.md).
- It reads the AppTable header of the cartridge mapped at `0x80000000`. It also probes
  `0xe0000100` for a second header.
- The v1.5 BaseROM shows a region selector (U.S.A. / Canada / U.K.) before starting a
  cartridge.
- Native-code cartridges print their own framework diagnostics on the UART, for example
  Cars: Supercharged: `[Framework]cart version: TST04 … version check`.
- Flash cartridges run inside the BaseROM's Flash player ("MXFlash"). Native C
  cartridges call into BaseROM services.

## 3. Services a cartridge expects from the system

The interface registry and the services found so far are specified in
[open-bios/spec/services.md](open-bios/spec/services.md). The rest is not documented
yet. The method:
instrument leapemu to log every control transfer from cartridge space (`0x8xxxxxxx`)
into BaseROM space and back. Then document each entry point by its observable
behaviour: arguments, return values, and memory and hardware side effects. Candidates
include:

- the boot entry point, and how control first passes to the cartridge (boot group
  0x1000?)
- the MQX RTOS services: tasks, timers, events, memory allocation
- file/resource access by handle (the asset tables above)
- the Flash player and its native-extension ("custom library") interface
- audio (the SYN sequencer, sample playback, speech), input, touch, and the LCD/DMA
  presentation path
- EEPROM save access (the cartridge EEPROM, device ≠ 0x26)

## 4. Persistent data

- **System EEPROM (512 B):** touch calibration is stored at 0x00, with a duplicate copy
  at 0x30; the display setting, volume and a check byte at 0x60–0x63
  ([hardware.md](hardware.md#system-eeprom-contents-leapemu)). Where a region setting
  is kept, if anywhere, is unknown.
- **Cartridge EEPROM (up to 2 KiB):** game saves such as names and high scores. The
  format is per game.
