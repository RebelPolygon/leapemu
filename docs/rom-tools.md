# ROM tools

A game's ROM window opens from the game list (right-click: Properties, ROM Contents,
Hex Editor & Patches) or, for the running game, from File > Game Properties. It has three
tabs.

## Info

In three parts:
- **Game:** title, region, type, notes and compatibility fixes from the game database,
  and the compatibility rating: leapemu's, or yours (chosen here or from the game list's
  right-click menu), which replaces it on your computer.
- **File:** the path, size and format, CRC-32, where the game's save file is (or will
  be), and its patches.
- **ROM header:** internal title, part number, version and build date.

Play starts the game (Restart, if it is the one running); Open Containing Folder shows
its file.

## Contents

- **Header:** where the image is mapped, the resource index ("RIB") and the checksum
  pointers, and some product information (product id, ROM version, build tool).
- **Resource groups:** the RIB's groups (boot, modules, product info, assets, ...). The
  layout is in [cart-bios-abi.md](cart-bios-abi.md).
- **Assets:** the asset group's tables, by type, each asset by handle, with its offset
  and size and its first bytes. Double-click one (or right-click > Show in Hex) to see it
  in the hex view.

**Viewing.** Clicking a bitmap or a font (or right-click > View) shows it in a preview
window: a bitmap at up to 4x over a checkerboard where it is transparent, a font as a sheet
of its characters. Listening stops when you leave the Contents tab.

**Listening.** Speech, A-law sounds, SYN music and soundtracks have a play button, and
right-click > Listen does the same.
- Speech, sounds and soundtracks play as the console would.
- Music is rendered from its notes with simple stand-in instruments (General MIDI
  families, and a drum kit). The notes and timing are the game's, but the instruments
  are not; the game's own instrument samples are not used yet. A song that loops forever
  plays its loop twice.

**Saving.** Right-click > Save As writes one asset in a readable form. Save Raw As writes
it exactly as stored. Export All writes every asset into a folder per type, in readable
form, or as stored with Raw ticked.

| Type | Readable form | As stored |
|---|---|---|
| SWF (Flash) | `.swf`, exactly (a SWF's header gives its length) | the same |
| Speech (CELP) | `.wav`, decoded with the BaseROM's codebook (8 kHz) | `.bin` |
| A-law audio | `.wav` (8 kHz) | `.bin` |
| SYN music | `.mid` (standard MIDI file) | `.syn` |
| PEG bitmaps, Flash bitmaps | `.bmp` (32-bit, with transparency) | `.bin` |
| Flash fonts | `.ttf` (TrueType) | `.bin` |
| Video | `.avi` | `.bin` |
| Soundtracks | `.wav` (11025 Hz), decoded by the game's own codec (below) | `.bin` |
| Others (instruments, GAS audio, fonts, cursors) | `.bin`, as stored | `.bin` |

**Fonts** become TrueType fonts:
- each glyph's pixels are traced into outlines, so at the font's own size (its height in
  pixels) it renders the glyph's silhouette pixel for pixel;
- the 4-bit anti-aliasing cannot be kept in outlines: the silhouette keeps the pixels at
  least half covered, so edges are hard rather than anti-aliased like the original;
- characters map to Unicode through the font's own character set (ISO 8859-15 or Mac
  Roman), and the font keeps its own name.

One kind of Flash bitmap is not decoded: Grundschule 2's 8-bit sprites. The game builds
their palette in RAM when play starts; it is not stored as a table in the ROM. Those are
saved as stored.

**MIDI files:**
- one track per SYN track, at 120 bpm with 125 ticks per quarter note, so a MIDI tick
  is the sequencer's 4 ms tick;
- the BaseROM's instruments map to General MIDI programs, and its drum kits to channel
  10;
- a cartridge's own instruments become piano, named in a text event;
- pitch slides are not included.

**Soundtracks** (Schoolhouse Rock's songs) are coded with a codec that is part of the
game's own code, so leapemu does not decode them itself; it runs the game's decoder:
- a machine of the tools' own boots the BaseROM and the cartridge until the game has
  registered its codec (about half a second of emulated time);
- then, for each soundtrack, leapemu calls the codec's start, decode (once per 64-byte
  block, 256 samples) and stop functions on that machine, as the game's player does;
- the result is exactly what the game decodes when it plays the song, sample for sample.
  The game ends a song when its video ends, so the file can run a fraction of a second
  past what the game plays.

It needs a BaseROM. Decoding a 3-minute song takes about a second.

Only SWF sizes are stored in the ROM. Other assets are taken to run up to where the next
asset starts, so a raw export can include some bytes that follow the asset.

`leapemu-cli --bios BIOS --cart GAME --export-assets DIR [--export-raw]` does the same
from the command line.

## Hex & Patches

The hex view shows the ROM with its enabled patches. Click a byte and type hex digits to
change it. Arrows and Page Up/Down move, and Delete or Backspace puts a byte back.
- **Patches:** edits go into the selected patch; the first edit starts "Patch 1". Patches
  can be added, renamed (double-click) and deleted, and switched on and off with their
  checkbox. Changed bytes are orange in the selected patch and blue in other enabled
  patches.
- **Go to:** takes an address (`807AFFA4`) or a file offset.
- **Find:** takes hex bytes (`43 02`) or text in quotes (`"FWS"`).
- **Addresses:** shows where each row is mapped, or with it off, its offset in the file.
- Hovering a byte shows its offset and address, the asset it is in, and the original value
  if a patch changed it.

**Patch files.** A game's patches are kept in the data folder's `patches` folder, in a
text file named after the game's CRC-32 (`0134af49.txt`), so they can be shared:

```
leapemu-patches 1
cart_crc 0134af49
cart_title Clifford - The Big Red Dog - Reading

patch on Magenta background
007affbb ff 00
```

Each edit line is a file offset, the original bytes and the new bytes. When the game
starts, the enabled patches are applied in order.
- An edit is applied only where the ROM still holds its original bytes. So a patch made
  for one version of a game cannot damage another; it is reported as not matching.
- A patched ROM no longer passes the BaseROM's digest check. So a patched game boots with
  the unsigned patches (as File > Load Unsigned ROM does).
- The game keeps its save file: saves are tied to the unpatched ROM.
- Changes made while the game runs apply when it restarts (Restart Game, in the patch
  list).
- Input movies record which patches were active (`cart_patches` in the movie file). A
  movie made with patches plays only with the same patches, and one made without only
  without them.
- `leapemu-cli --patches FILE` applies a patch file's enabled patches.
