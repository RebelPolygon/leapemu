# Leapster 2 downloadable games

The Leapster 2 could install games from LeapFrog's website onto its internal storage or
an SD card. Some titles, such as *Dragons to the Rescue*, were released only this way.
leapemu runs many of them on the original Leapster's BaseROM.

Load the game's `.bin` with File > Load ROM (or `--cart` in the CLI); it is recognised
automatically. If its package folder has a `meta.inf`, its `Name` is shown as the
title.

## The format [observed]

- A downloadable is laid out like a cartridge image:
  - `"Copyright LeapFrog"` at 0x100;
  - the device table at 0x118;
  - the `LEAP` resource index.
- It is linked to run from RAM: its device range starts at `0x3c80'0000`, not at the
  cartridge window (`0x8000'0000`).
- An installed package is a folder `Leapster/Apps/LSTR-0x<product id>-000000/`
  holding:
  - the program (`BinFile` in `meta.inf`);
  - a menu icon (a small SWF) and a preview image;
  - sometimes `eeprom.ltm`, the game's save data.
- Leapster 2 system programs (the SD card menu, and homebrew that replaces it, such
  as [.DMPSTER](https://github.com/toadster172/.DMPSTER)) instead start with the tag
  `JUMP`. They need the Leapster 2's BaseROM, and leapemu rejects them with an
  explanation.

## How leapemu runs one

- The image is copied into RAM at its link address on every reset.
- It is also shown in the cartridge window, where the BaseROM looks for a cartridge
  and follows its resource index to the program in RAM.
- It is not signed as a cartridge, so the BaseROM's signature and checksum checks
  are bypassed, as for unsigned homebrew (docs/homebrew.md).
- The download's code runs from RAM, and is cached and compiled like ROM: the pages
  of its image are watched for writes, and instructions written over are decoded
  again (and compiled blocks dropped) before the next instruction runs. Native-code
  downloads such as Letterpillar play at about 35× real time with the JIT
  (interpreted, they would run barely faster than real time).
- Save data goes through the cartridge save interface, as for cartridges (Toy
  Story 3 writes it).
- Results are deterministic and identical across the three CPU cores, and input
  movies replay exactly.

## Results with the v1.5 BaseROM

Tested with the 13 games of a Leapster 2 install, played by hand.

| Game | Result |
|---|---|
| Dragons to the Rescue | runs (gameplay) |
| Disney Fairies Demo | runs (gameplay) |
| Ni Hao, Kai-lan Demo | runs (gameplay) |
| Disney Pixar UP Demo | runs (gameplay) |
| Wolverine & the X-Men Demo | runs (gameplay) |
| Disney The Princess and the Frog Demo | runs (gameplay) |
| Disney-Pixar Toy Story 3 Mini-Game | runs (gameplay; saves) |
| Number Raiders, Letterpillar | run (one and two players), with leapemu's Leapster 2 services (below) |
| Rabbit River, Chicken Coop, Shape Shop | run (gameplay), with the same |
| Star Wars: Jedi Reading Demo | runs (gameplay) |

**Why some stop.** Games find the system's services through the interface registry
(docs/open-bios/spec/services.md). The Leapster's BaseROM has 16 registry entries;
the Leapster 2's has more. Five of the downloads use two of those extra entries,
through table B of each. These games are written for the Leapster 2, but the
Leapster's firmware differs little except for storage and the system's own data, so
supplying those few services is enough.

- **Entry 52: files.** Number Raiders and Letterpillar keep their save (high scores,
  the one-player game) in a file on the Leapster 2's storage,
  `B:\Leapster\Apps\<name>\eeprom.ltm`. The first call comes about 10.6 s after
  power-on, to check for the save. The slots used:

  | Slot | Function |
  |---|---|
  | 10 | `fopen(path, mode)` → file, or 0 |
  | 11 | `fclose(file)` |
  | 12 | `fread(ptr, size, count, file)` → items read |
  | 13 | `fwrite(ptr, size, count, file)` → items written |
  | 14 | `fseek(file, offset, whence)` |
  | 17 | the size of the file at a path |

- **Entry 54, slot 10: the current program.** Rabbit River, Chicken Coop and Shape
  Shop call it first, as `(buffer, 260, 1)`.
  - It returns the name of a text file that holds the running program's path.
  - The game reads that file whole (entry 52: size, `fopen`, `fread`) and keeps
    everything up to its last `\`: its own folder. Its save goes there, as
    `eeprom.ltm`, 2 KiB.
  - Building that folder, the game copies each part of the path into the memory
    that `fread` (entry 52's slot 12) points to, and reads it back. On the Leapster 2
    that must be harmless. With leapemu the copy is dropped and reads back empty, so
    the folder comes out as a few `\` and only the file name counts.
- With the v1.5 BaseROM neither entry exists. The slots the games read hold
  unrelated data: some calls jump to address 0 (the reset vector, so the console
  restarts), others to a stray address.

**leapemu's Leapster 2 services.** For the titles that need them (flag
`stub-services` in res/games.tsv: the five above), leapemu supplies these services.
- Every place a game reaches one of those tables does it the same way: `add rA,rA,
  0x270` (entry 52) or `0x288` (entry 54), then `ld_s rA,[rA,4]`.
- At reset, in the RAM copy of the download only, each of those adds becomes `mov
  rA,-2048` or `-1792`. The load then reads a pointer that leapemu places at
  `0xffff'f804` or `0xffff'f904`, which leads to its own table of functions. The
  image on disk is not changed.
- Each of leapemu's functions passes its arguments through a small I/O port, and
  leapemu answers the call itself. Slots not listed above return 0, as a failed call
  would, and are logged.
- Two files are emulated:
  - The current-program file, `B:\Leapster\System\CurrentProgram` (leapemu's
    name). It holds `B:\Leapster\Apps\<file>\<file>.bin`, where `<file>` is the
    download's file name without its extension.
  - The save, `eeprom.ltm` in any folder. It is the game's save memory (2 KiB), so it
    is saved in its `.sav` like a cartridge's save. It exists unless that memory is
    all zeros (or the game created it this session), and is 2 KiB long. A game
    creates it whole and then reads and writes records in it; none asks its size.
- It is on for those titles only, in the GUI and the CLI (`--stub-services` turns it
  on for any download), and is listed under Compatibility fixes in the ROM's
  Properties. Input movies record it, and it runs the same on every CPU core.
- None of the other eight downloads uses either table.
- The player's name: the Leapster 2 versions take it from the console's player
  profile. They play as the guest profile here (Letterpillar logs
  `GlobalDSD:[GUEST]`), so names in high-score tables are empty.

**Public documentation of the Leapster 2 registry.** .DMPSTER (GPL-3.0), a cartridge
and BaseROM dumper for the Leapster 2, declares four of its interfaces in its source
(`chorus.h`), located by registry entry:
- kernel (events, memory, interrupts, time, text output): entry 0;
- buttons: entry 4;
- LCD: entry 6;
- FAT32 file access: entry 13.

Entries 52 and 54 are not among them; what is known of them is from the games' use (above).

## Not done yet

- The Leapster 2 itself: its BaseROM (not available), SD card and internal storage,
  USB, and its larger RAM layout as the system uses it.
- Using a package's `eeprom.ltm` as the save. Its format is not established:
  - the 256-byte files in the Fairies, Wolverine and Toy Story 3 packages are
    identical (a default high-score table), so they are not each game's own data;
  - Toy Story 3, saving through the cartridge save interface in leapemu, writes a
    different layout;
  - Shape Shop's (2 KiB) holds a player's name and score.

  Games save to leapemu's own save file meanwhile.
