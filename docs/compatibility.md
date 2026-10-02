# Compatibility

How far each title gets in leapemu, with the Leapster Universal v1.5 BaseROM
(`155-10072-a`). Ratings describe how far a game gets, not how closely every detail
matches the hardware:

| Rating | Meaning |
|---|---|
| ■■■■□ **In-game** | Reaches gameplay and responds to input. Not a completed playthrough, nor a check of every activity |
| ■■■□□ **Menus** | The game's own screens work (title, menus, name entry), but not gameplay (or it is not checked yet) |
| ■■□□□ **Intro** | Starts, then stops at the LeapFrog screen or its intro |
| ■□□□□ **Broken** | Does not start |
| □□□□□ **Untested** | Not tried yet |

The fifth square is left for a rating that is not given: no game is claimed to play
perfectly.

In the game list, right-click > Compatibility sets your own rating for a game. It
replaces leapemu's rating on your computer only, and is marked as yours.

Every cartridge and prototype listed reaches gameplay, a few with a fix leapemu applies
by itself ([below](#fixes-leapemu-applies-by-itself)).

**How these were tested** (leapemu 0.1.0, default settings, the JIT): each game was
booted and taken into gameplay (a game or activity started, with the buttons or the
stylus), by hand or with scripted input, and checked briefly there. A few were
played much further (Sonic X, Go Diego Go!, Cars, SpongeBob Saves the Day, Pet Pals).
So a rating says a game gets that far, not that everything in it was seen working. Notes are kept for the titles
that don't reach gameplay. The same data (res/games.tsv) gives the GUI's game list its names,
regions and compatibility. Reports of how far a game plays are welcome, as an issue on
the project's repository: title, region, the image's CRC-32 (Game Properties shows
it), BaseROM, and what happened.

Results are identical with every CPU core (interpreter, cached interpreter, JIT). The
optional display enhancements (View > Vector Scale to Window, View > Interpolation) are
not part of these results.

## Games not in the database

A cartridge image that leapemu does not know is listed as a cartridge, unless the image
itself shows otherwise:
- **Homebrew:** built with something other than LeapFrog's ToolPad.
- **Prototype:** the tooltip and Properties say which of these marks it has. Each one is
  found in known prototypes and in none of the 62 retail dumps tested.
  - a development upload (a block header at 4 MiB; see
    [cart-bios-abi.md](cart-bios-abi.md));
  - an image not the size of a ROM chip (a build output rather than a dump of a chip);
  - a placeholder part number (`123-4567`), or none of the `152-` cartridge part numbers
    every retail cartridge has;
  - a version string saying not to ship it.

Prototypes are signed and checksummed like retail games: the BaseROM accepts them as
they are. A prototype with none of these marks (one built late enough to carry its final
part number, and dumped from a chip) is listed as a cartridge. Its file name says what
it is, and adding it to res/games.tsv gives it its type.

## Fixes leapemu applies by itself

A few games reach gameplay only because leapemu adjusts how it loads or serves them. The
fixes are applied automatically, to those games only; the tables below name them in each
game's notes, and Game Properties shows them too. They work in memory: the image files
are never changed.

### Content checks skipped

The BaseROM checks a cartridge's contents before starting it. The known dump of
Schoolhouse Rock – America Rock has two damaged bits, so the check fails and the
BaseROM shows its "?" cartridge screen. leapemu recognises that dump and skips the
check for it, in memory only, as File > Load Unsigned ROM does for homebrew.

### Development uploads

Some prototypes are development uploads rather than chip dumps: a 24-byte block header
sits at 4 MiB, so everything after it is out of place and the game cannot start.
leapemu finds the header in any image and removes it when loading (the image's CRC is
still that of the file). See [cart-bios-abi.md](cart-bios-abi.md#development-upload-images-some-prototypes).

### Leapster 2 downloads

The downloadable games are programs for the Leapster 2's internal storage, not
cartridge images. leapemu loads each into RAM as the Leapster 2 does, and skips the
cartridge checks, which they have nothing to pass. See [downloadables.md](downloadables.md).

### Leapster 2 services

Five of the downloads also use system services the Leapster BaseROM doesn't have (the
Leapster 2's file functions and its record of the current program), and cannot reach
gameplay without them. leapemu supplies its own versions of those few services to
these five games only, and points the games' few calls to them there (a change to
their code in memory). See [downloadables.md](downloadables.md).

<!-- games:begin (generated from res/games.tsv) -->

## Cartridges

| Title | Region | Tier | Notes |
|---|---|---|---|
| 1st Grade | USA | ■■■■□ In-game |  |
| Animal Genius | USA | ■■■■□ In-game |  |
| Bratz World - The Jet Set | USA | ■■■■□ In-game |  |
| Cars | Germany | ■■■■□ In-game |  |
| Cars | USA | ■■■■□ In-game |  |
| Cars - Supercharged | USA | ■■■■□ In-game |  |
| Clifford - The Big Red Dog - Reading | USA | ■■■■□ In-game |  |
| Cosmic Math - Arcade-Style Learning! | USA | ■■■■□ In-game |  |
| Creature Create | USA | ■■■■□ In-game |  |
| Demo - Herbst 2004 II | Germany | ■■■■□ In-game |  |
| Digging for Dinosaurs | USA | ■■■■□ In-game |  |
| Disney Fairies | USA | ■■■■□ In-game |  |
| Disney Princess | USA | ■■■■□ In-game |  |
| Disney Princess - Worlds of Enchantment | USA | ■■■■□ In-game |  |
| Disney Prinzessinnen - Zauberhaftes Lernen | Germany | ■■■■□ In-game |  |
| Dora - Retter der Wildnis | Germany | ■■■■□ In-game |  |
| Dora the Explorer - Camping Adventure | USA | ■■■■□ In-game |  |
| Findet Nemo | Germany | ■■■■□ In-game |  |
| Finding Nemo | USA | ■■■■□ In-game |  |
| Foster's Home for Imaginary Friends | USA | ■■■■□ In-game |  |
| Get Puzzled! | USA | ■■■■□ In-game |  |
| Go Diego Go! - Animal Rescuer | USA | ■■■■□ In-game |  |
| Grundschule 1 | Germany | ■■■■□ In-game |  |
| Grundschule 2 - Musik in Gefahr | Germany | ■■■■□ In-game |  |
| I Spy - Treasure Hunt | USA | ■■■■□ In-game |  |
| Kindergarten | USA | ■■■■□ In-game |  |
| Lernen mit Leap | Germany | ■■■■□ In-game |  |
| Letter Factory | USA | ■■■■□ In-game |  |
| Madagascar | USA | ■■■■□ In-game |  |
| Math Baseball | USA | ■■■■□ In-game |  |
| Mit Bruno Bleistift lernst du Malen & Schreiben | Germany | ■■■■□ In-game |  |
| Mr. Pencil's Learn to Draw & Write | USA | ■■■■□ In-game |  |
| My Amusement Park | USA | ■■■■□ In-game |  |
| Numbers on the Run | USA | ■■■■□ In-game |  |
| Pet Pals | USA | ■■■■□ In-game |  |
| Ratatouille | Germany | ■■■■□ In-game |  |
| Ratatouille | USA | ■■■■□ In-game |  |
| Reading with Phonics - Mole's Huge Nose | USA | ■■■■□ In-game |  |
| Schoolhouse Rock - America Rock | USA | ■■■■□ In-game | Fix: [content checks skipped](#content-checks-skipped) |
| Schoolhouse Rock - Grammar Rock | USA | ■■■■□ In-game |  |
| Sonic X | USA | ■■■■□ In-game |  |
| Spider-Man - Schachmatt den Schildersaboteuren! | Germany | ■■■■□ In-game |  |
| Spider-Man - The Case Of The Sinister Speller | USA | ■■■■□ In-game |  |
| Spongebob Schwammkopf - Zeitreise durch das Wurmloch | Germany | ■■■■□ In-game |  |
| Spongebob Schwammkopf hat alles im Griff | Germany | ■■■■□ In-game |  |
| Spongebob Squarepants - Saves the Day | USA | ■■■■□ In-game |  |
| Star Wars - Jedi Math | USA | ■■■■□ In-game |  |
| Tangled | USA | ■■■■□ In-game |  |
| The Backyardigans | USA | ■■■■□ In-game |  |
| The Batman - Multipliziere, Dividiere und Regiere | Germany | ■■■■□ In-game |  |
| The Batman - Strength in Numbers | USA | ■■■■□ In-game |  |
| The Disney-Pixar Collection | USA | ■■■■□ In-game |  |
| The Penguins of Madagascar | USA | ■■■■□ In-game |  |
| Thomas & Friends - Calling All Engines! | USA | ■■■■□ In-game |  |
| Toy Story 3 | USA | ■■■■□ In-game |  |
| Up | USA | ■■■■□ In-game |  |
| Vorschule | Germany | ■■■■□ In-game |  |
| Wall-E | Germany | ■■■■□ In-game |  |
| Weltraum-Mathe - Lernen im Arcade-Stil! | Germany | ■■■■□ In-game |  |
| Woerterjaeger - Lernen im Arcade-Stil! | Germany | ■■■■□ In-game |  |
| Wolverine and the X-Men | USA | ■■■■□ In-game |  |
| Zahlenjaeger - Lernen im Arcade-Stil! | Germany | ■■■■□ In-game |  |

## Prototypes

| Title | Region | Tier | Notes |
|---|---|---|---|
| Cars (prototype, July 26 2005) | USA | ■■■■□ In-game |  |
| Cars Supercharged (prototype, January 10 2007) | USA | ■■■■□ In-game | Fix: [block headers removed](#development-uploads) |
| Go Diego Go Animal Rescuer (prototype, April 5 2007) | USA | ■■■■□ In-game |  |
| Go Diego Go Animal Rescuer (prototype, March 15 2007) | USA | ■■■■□ In-game | Fix: [block headers removed](#development-uploads) |
| Pet Pals (prototype, October 17 2006) | USA | ■■■■□ In-game | Fix: [block headers removed](#development-uploads) |
| Wall-E (prototype, December 20 2007) | USA | ■■■■□ In-game |  |

## Leapster 2 downloadable games

Run on the Leapster's own BaseROM, each [loaded as on a Leapster 2](#leapster-2-downloads);
see [downloadables.md](downloadables.md).

| Title | Tier | Notes |
|---|---|---|
| Chicken Coop | ■■■■□ In-game | Fix: [Leapster 2 services](#leapster-2-services) |
| Disney Fairies Demo | ■■■■□ In-game |  |
| Disney Pixar UP Demo | ■■■■□ In-game |  |
| Disney The Princess and the Frog Demo | ■■■■□ In-game |  |
| Disney-Pixar Toy Story 3 Mini-Game | ■■■■□ In-game |  |
| Dragons to the Rescue | ■■■■□ In-game |  |
| Letterpillar | ■■■■□ In-game | Fix: [Leapster 2 services](#leapster-2-services) |
| Ni Hao, Kai-lan Demo | ■■■■□ In-game |  |
| Number Raiders | ■■■■□ In-game | Fix: [Leapster 2 services](#leapster-2-services) |
| Rabbit River | ■■■■□ In-game | Fix: [Leapster 2 services](#leapster-2-services) |
| Shape Shop | ■■■■□ In-game | Fix: [Leapster 2 services](#leapster-2-services) |
| Star Wars: Jedi Reading Demo | ■■■■□ In-game |  |
| Wolverine & the X-Men Demo | ■■■■□ In-game |  |

<!-- games:end -->

## Known issues affecting many games

- **Speed:** CPU timing comes from a calibrated wait-state model (see
  [timing.md](timing.md)); gameplay runs within about 10% of hardware in the titles
  measured.
- **Vector Scale to Window** shows the original 160×160 image for screens with things
  the game draws itself.
- **Leapster 2 BaseROM:** not available; Leapster 2-specific features (SD card,
  internal storage) are not emulated.
