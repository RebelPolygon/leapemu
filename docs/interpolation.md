# Frame interpolation and redraw

Leapster games step their logic and draw a new frame roughly every 4th display
refresh, so gameplay runs at about 15 fps (Flash content at 8–12). leapemu can show
in-between images, and can redraw some content at the window's resolution. Both are
display-only: emulation, game speed and save states are unaffected.

| Feature | Setting (View menu) | What it does |
|---|---|---|
| Frame interpolation | Interpolation (hack, buggy); off by default | In-between images at the display's refresh rate. The image lags the emulation by about one game frame |
| Redraw at the window's resolution | Vector Scale to Window; off by default | Flash content as vectors, and the Cars races' road, drawn at the window's resolution instead of enlarged |

Motion is known in one of two ways:

| | How | Validation | Code |
|---|---|---|---|
| **Native capture** | Hooks on the game's own drawing record each frame as a draw list, which is redrawn at in-between positions | Each captured frame is replayed and compared with the emulated LCD image. Tile planes and the Cars races reproduce it pixel for pixel (pixels they cannot are kept as a static layer, or the frame is not used); the Flash redraw is anti-aliased, so it is close, not identical. The in-between images are interpolations: no real frame exists to compare them with | `src/core/drawcap.cpp`, `src/core/flash/` |
| **Pixel motion** (fallback) | Camera scroll and moving objects estimated from the two images | None: an estimate | `src/core/interp.cpp` |

Native capture covers three engines: the tile-plane engine of Sonic X and Go Diego Go!
Animal Rescuer, the Cars races, and the BaseROM's Flash player (all Flash content, in
any cartridge).

**Contents**
- [Native capture](#native-capture): hooks, draw lists, validation
- [Display](#display): the playback clock, between two frames
- Engines: [tile planes](#engine-tile-planes-sonic-x-go-diego-go-animal-rescuer),
  [the Cars races](#engine-the-cars-races-cars-cars-supercharged),
  [the Flash player](#engine-the-baserom-flash-player-all-flash-content)
- [Pixel motion](#pixel-motion-the-fallback)
- [Tools](#tools)
- [Adding engines, with the Cars races as an example](#adding-engines-with-the-cars-races-as-an-example)

## Native capture

### Hooks

The emulator can run observation hooks on specific instructions
(`arc::Cpu::set_pc_hook`). They cost nothing when unused and never change emulated
state: save states are byte-identical with capture on or off. Capture runs only in
Native mode, and costs about 4% of emulation speed.

Each hook is found by a **code signature**: the length of a run of the routine's code
and a hash of its bytes (`src/core/codesig.h`). The same code at a different address
in each cartridge or BaseROM is found automatically, and none of the code itself is in
leapemu's source. `tools/debug/code_sig` prints the values for a run of a ROM image.

### Draw lists

For a supported engine, the hooks record each game frame as a draw list:

| Element | What is recorded |
|---|---|
| **Plane** | Scroll position, clip rectangle, draw mode; snapshots of the tilemap, tiles and palettes |
| **Sprite** | Its engine object (its identity across frames), position, scale (Cars), and the pixels its draw changed |
| **Floor** (Cars) | The camera's position, heading and horizon; snapshots of the ground's tilemap, tiles, palettes and the engine's tables |
| **Flash frame** | The player's whole display list (below) |

### Validation

When the game sends a frame to the LCD (the framebuffer DMA), the capture replays its
draw list and compares the result with the real image:
- pixels the replay does not reproduce are kept as a static top layer, shown as in the
  newer frame;
- a frame with more than a quarter of its pixels unexplained is not used: the display
  shows the LCD image (or pixel motion) instead.

Flash frames are validated differently, since their redraw is anti-aliased
([below](#validation-and-coverage)).

## Display

### The playback clock

Captured frames arrive unevenly: Flash frames 2 to 17 ticks apart (7–8 typically),
the Cars races' 4 or 5. The display plays them back on its own clock, about one
typical interval behind the newest frame, and places everything along the frames' own
timestamps, so motion is continuous even when a frame is early or late.
- The clock runs up to 1.5× faster when it falls behind, and slows to 0.75× when it
  is about to run out of frames. It never passes the newest frame.
- Motion into a frame lasts at most 1.5 typical intervals, so a longer gap is shown
  as the previous frame held, then moving.
- Frames more than 20 ticks apart are not blended (a long pause, a loading screen):
  the newer one is shown.

Earlier, each transition was stretched over the gap before it. A frame that arrived
early cut the motion short with a jump; one that arrived late left the image still.
In the Cars races (frames 4 or 5 ticks apart) that stalled 21 times in 600 frames.
`tests/display_tests.cpp` checks that frames arriving 4 or 5 ticks apart move
continuously.

### Between two frames

Between frames A and B:

| Element | In between |
|---|---|
| Planes | Scroll from A's position to B's, keeping the engine's 1/4096-pixel precision |
| Sprites | Glide from A's position to B's, and from A's scale to B's: the pixels drawn in B are scaled around the object's position |
| Floor | Redrawn from a camera between A's view and B's |
| Flash | Each part's transform, by the rules [below](#matching-parts-between-frames) |
| Everything else | As in B |

Objects are matched between frames by their engine object. Jumps larger than a few
tiles are cuts, not motion.

The GUI draws each layer as a texture at its fractional position at the window's
resolution. The CLI writes the same output with
`--interp native --interp-dump DIR --interp-steps N --interp-scale K`.

## Engine: tile planes (Sonic X, Go Diego Go! Animal Rescuer)

**The engine.** The two games share a software renderer that works like a console
video chip. Each frame is composed in a 160×160 16-bit (0x0RGB) surface, converted to
12-bit colour and sent to the LCD.
- **Planes:** a 32×32 ring tilemap of 8×8 4-bit tiles, 16 palettes, a scroll position
  with 12 fractional bits, a clip rectangle, and an opaque, transparent (colour 0) or
  alpha-blended mode (DrawPlane).
- **Sprites:** drawn per object by a sprite-layer routine that walks the objects of
  one priority (DrawSpriteLayer).

**Hooks.** In DrawPlane, once it has streamed new tiles into the ring tilemap; in
DrawSpriteLayer, around each object's draw.

**Checked.** `drawcap_check` reproduces every frame of both games over 30 s of play.

## Engine: the Cars races (Cars, Cars Supercharged)

**The engine.** An earlier version of the same engine, with a perspective floor in the
style of the SNES's Mode 7:
- **The sky:** two planes as above (scroll in whole pixels), clipped at the horizon
  and scrolled sideways with the camera's heading.
- **The floor:** drawn row by row from the horizon down. Each row shows the ground at a
  depth from a table, along the camera's heading, from its position. The ground is a
  128×128 ring tilemap of 8×8 4-bit tiles with 16 palettes, which the game refreshes
  around the camera every frame.
- **Objects** (the car and its shadow, roadside objects, opponents, the HUD): sprites
  as above, drawn at a scale.
- **The camera tilts when the car jumps:** the horizon row moves down by up to about
  30 rows, and back.

**The floor's arithmetic.** The floor's context holds the horizon row H, the camera's
position (cx, cy) in 1/4096 texel, and its heading; sin and cos are entries of a
256-step table, ×4096.

```
for each screen row y from H − 1 to 159:
    r  = y − H + 1 + r0             # depth table row; r0 = 10 (9 in Cars Supercharged)
    d  = 80 · depth[r]              # depth[r] = 65536 / (r + 1)
    a  = low32(−d · cos)            # the CPU's mul64 leaves the low word in MMID
    b  = low32( d · sin)
    du = a >> 17,  dv = b >> 17     # the step along the row, per pixel
    u  = cx − 80·du + (b >> 10) − (b >> 13)      # (b >> 10) − (b >> 13) ≈ 7b / 8192:
    v  = cy − 80·dv − ((a >> 10) − (a >> 13))    # the row starts left of a point ahead
    for each of the 160 pixels:  pixel = texel(u, v);  u += du;  v += dv

texel(u, v):                        # (u >> 12, v >> 12) in a 1024×1024 ring
    entry = map[(v >> 15) & 127][(u >> 15) & 127]   # bits 0–11 tile, 12–15 palette
    byte  = tiles[entry.tile][((v >> 12) & 7) · 4 + ((u >> 13) & 3)]
    index = (u >> 12) & 1 ? byte >> 4 : byte & 15
    return palette[entry.palette][index]
```

Starting r0 rows below the table's origin keeps d · 4096 within 31 bits. The low-word
behaviour of `mul64` is in [hardware.md](hardware.md#cpu).

**Hooks.** The floor routine's entry (the camera and data as it is about to draw), the
engine's DrawPlane, and its sprite-layer loop (each object's position and scale: +0x30
times +0x38, or +0x34 if set). Draws to off-screen surfaces are left out.

**Redraw.**
- A captured frame is redrawn with the engine's own integer arithmetic, so it matches
  the screen exactly.
- At a fractional view (between frames, or at a higher resolution) the same formulas
  run in floating point: the depth between table rows is taken as 1 / depth (exact for
  the table's shape), and the heading between table steps.
- Between frames, the camera's position, heading and horizon move continuously, and
  the floor is redrawn from there: at the window's resolution with Vector Scale to
  Window, or at 160×160 (exact at the frames themselves). Sprites glide and scale; the
  sky planes scroll, their clips covering both frames' while the horizon moves.

**Builds.** The three builds compile this code differently, so each has its own
signatures, and the floor's data addresses are read from the instructions of the
routine found (and checked against each other):
- Cars, and the January 2007 Cars Supercharged prototype;
- Cars (Germany);
- Cars Supercharged (whose floor starts one row further up the depth table).

**Checked.** `drawcap_check` reproduces every frame of every race tried, except a few
pixels the game draws with a BaseROM routine on one track (kept as a static layer).

## Engine: the BaseROM Flash player (all Flash content)

The Leapster's Flash games, and the menus and cutscenes of most native games, are
Flash 5 movies stored in the cartridge, displayed by the BaseROM's Flash player. So
this works for any cartridge.

### Hook and snapshot

**Hook.** The player's routine that draws one display object. It tells the capture
that the player is active, and gives it the player's context.

**Snapshot.** At the next framebuffer DMA, the capture reads the player's whole
display list, and keeps it when it changed (the player redraws dirty regions in
several DMAs per frame). For each object:
- parent and depth; clip depth (masks); visibility;
- local matrix and colour transform;
- the character's type, SWF tag and id, and a pointer to its definition in the
  cartridge's SWF data.

**The view** comes from the player itself: its view matrix and render rectangle,
whose units are a fraction of a pixel. So movies that zoom their view, like the
BaseROM's Leapster logo, render correctly.

### Redraw

The renderer (`src/core/flash/`) draws the display list from the SWF definitions,
parsed from the cartridge per the public SWF specification:
- shapes with solid, gradient and bitmap fills: lossless, and JPEG through a baseline
  and progressive decoder (`src/core/flash/jpeg.cpp`: DefineBits with JPEGTables,
  DefineBitsJPEG2, DefineBitsJPEG3 with its alpha channel);
- morph shapes (shape tweens) at the player's ratio, which is interpolated too;
- strokes with round caps and joins;
- static text in embedded fonts;
- clip-depth masks, which may overlap or cross each other's ranges (a part is drawn
  inside every mask that covers it), and colour transforms.

Smooth bitmap fills are sampled bilinearly when enlarged, and nearest-neighbour at 1×,
like the player. A mask made of a nested clip, or of a shape or kind the renderer
cannot draw, makes the frame incomplete: it falls back to the LCD image rather than
showing unmasked parts.

**Performance.** Rasterisation is anti-aliased, at any scale. Above 1× the image is
split into horizontal bands of at least 32 rows, a few per thread, which a pool of
persistent worker threads takes as each becomes free (the bands' costs differ widely).
Each band keeps coverage and mask buffers for its own rows, and skips shapes outside
them. A frame that does not change (held, or a static menu) is not drawn again. Most
of the time goes into filling pixels, since the backgrounds are bitmaps. For Go Diego
Go!'s cutscenes on a 32-thread CPU, a frame takes:

| Scale | Median |
|---|---|
| 4× | 1.0 ms |
| 6× | 1.9 ms |
| 8× | 3.4 ms |

### Matching parts between frames

Objects are matched between Flash frames by their **timeline slot**: the path of depths
from the root through levels and clips to the part. Then these rules decide how each
part gets from one frame to the next. Nothing here is specific to a game; the rules use
only the two frames' transforms and the drawings' bounds (and the frame before them).

**Turns follow an arc.** Each part's rotation, scale and skew are interpolated
separately, not the raw matrix entries, so rotating parts keep their size. Its position
follows the arc of its own turn (the change between the frames is a rotation about
some point), not a straight line. This holds only when the change is a turn with an
even scale; a part that stretches unevenly or skews moves in a straight line, since no
single point then stays still.

**Only the same drawing glides.** A drawing the timeline swaps in (a blink, a new mouth
or pupil pose) has its own placement; blending between two unrelated placements made
eyelids and pupils slide and stretch. Swapped drawings change at the midpoint: before
it, the previous frame's display list moves on; from it, the next frame's arrives. So
everything that glides moves continuously across the midpoint.

**Big changes are redraws, not motion.** The same drawing jumping more than 64 px,
turning more than 60° or flipping between two frames is a cut, or a drawing reused for
another limb (a galloping dog's legs).

**Re-placed small drawings.** A small drawing that moves further than 1.5 times its own
size (and over 4 px) is also a redraw, unless a sibling carries it (a hand on its
forearm, found by the skeleton below) or it keeps moving as it already was.
- *Why:* frame-by-frame animation re-places drawings, and a movie stores that exactly
  like motion. SpongeBob Saves the Day's cutscenes reuse one small oval as Mr. Krabs'
  pupil in one frame and a drop by his mouth in the next, 27 px away; gliding flew it
  across his face.
- These cutscenes are authored at 8 frames a second and the player shows every one, so
  no real in-between frame exists to recover; the parts' relations are the only
  evidence.
- The frame before the pair decides: a thrown ball's step continues the one before it
  (within half its length), while the reused oval had sat still. Without that frame,
  or for a part that was not in it, the part glides. The interpolator's queue keeps the
  frame for this.

**Pose changes move as a group.** A new pose swaps many drawings of a character while
its other parts move. With drawings swapping at the midpoint and the rest gliding, the
in-betweens used to mix the two poses: Mr. Krabs with one pose's eyes and the other
pose's claw sliding across.
- The parts that change in a transition (moving, swapped or new; still parts such as
  the background are left out) are grouped by touching, within each clip.
- In a group whose area is at least 30% swapped drawings, every part moves by the
  group's shared movement (the median step of its moving parts). A character swinging
  across the screen while its drawings change still travels smoothly, and its swapped
  drawings travel with it.
- A part that also moves further of its own than half its size (and 4 px) changes at
  the midpoint with the drawings, keeping only the shared movement; a smaller movement
  of its own (a leg in a walk) still glides.
- In the SpongeBob, Go Diego Go! and Pet Pals cutscenes this made the in-betweens
  smoother overall (`interp_timing`'s unevenness 1.16 → 1.14, 0.65 → 0.64 and
  0.93 → 0.82) and removed the mixed poses. `cutscene_eval` takes `POSE=0` and
  `REUSE=0` to compare without these rules.

**New parts follow what they sit on.** Characters are often built of many sibling
parts that each move (Go Diego Go's cutscenes animate every part of Diego separately).
A part that appears or is swapped in has no earlier placement, so it moves with the
gliding sibling it sits on, taking that sibling's in-between offset in their parent's
space; without one underneath, the nearest gliding sibling close by. Otherwise new eyes
and mouths would sit at their final place while the face was still on its way there.

**A skeleton keeps joints together.** Flat parts also turn about the joints between them
(shoulder, elbow, wrist), and interpolating each alone pulls joints apart mid-way. So a
skeleton is inferred from the two frames: a gliding part is carried by a larger gliding
sibling if it either
- keeps still relative to it (another layer of the same limb), or
- turns about a joint with it: a point of the part that stays in place relative to the
  sibling, inside both.

Its motion relative to the carrier is interpolated as a turn about that joint, so the
joint holds throughout, also while the part turns and stretches at once. For Go Diego
Go!'s intro, the separation mid-way of parts that meet at a joint fell from 0.34 px to
0.23 px (1× screen pixels).

**Clips.** A clip the player re-creates keeps gliding, and its children stay in step.
A part glides only if its parent does and it belongs to the same movie: the children
of a clip that was replaced by another start over with it.

**The view** is interpolated too, so a movie that pans or zooms moves smoothly. A view
change that flips, turns more than about 60°, scales by more than 2× or jumps more than
64 px is a cut: the newer view is used.

### Text

**Dynamic text fields** (DefineEditText) are laid out by a separate text engine in the
BaseROM, using the device fonts "Leapster Prop" and "Leapster Mono" (4-bit anti-aliased
bitmap fonts, not outlines). Two hooks, found by code signature, capture what it draws:
- the routine that draws one field, which names its display object;
- its glyph blitter: glyph bitmap, size, screen position, colour and clip rectangle.

Each field keeps the glyphs of its latest drawing, with care for how the player
redraws:
- **Dirty regions.** The player redraws only dirty regions, and a field overlapping one
  is drawn clipped to it: only its glyphs inside the region are drawn again. So a
  drawing replaces only the glyphs inside the region being redrawn (the engine's
  drawing surface holds that clip rectangle when a field starts drawing). Otherwise,
  typing on SpongeBob's keyboard emptied the neighbouring keys, and a moving highlight
  left copies of itself behind.
- **Moved or reused fields.** A field drawn at a new place (it moved), or a different
  field in a reused display object, loses the glyphs not drawn this time.
- **Unseen fields.** A field never seen drawn is treated as empty, but only if the LCD
  shows nothing there that the redraw lacks. After loading a save state a static field
  is not redrawn, so such frames fall back instead of losing text. Glyphs drawn outside
  any field also make a frame fall back.
- A text change always produces a new frame.

The glyphs follow the field's interpolated position. Their coverage is upscaled
bilinearly and steepened around 50% for crisper edges; at 1× it matches the original
exactly.

**Outline fonts.** A field that uses one of the movie's embedded fonts (DefineEditText's
UseOutlines) is not drawn through the device-font blitter: the player lays the text out
itself, then draws each glyph's shape from the font's DefineFont2 record. This is very
common for names on profile screens and the letters typed into them (Clifford, Cars,
The Penguins of Madagascar, the German keyboards of Ratatouille and SpongeBob, among
many others).
- A third hook, in that glyph loop, records for each glyph the address of its shape in
  the cartridge, and its placement in the field's own space: the font height as a scale
  (16.16, the 1024-unit EM square to twips) and the pen position in twips.
- The whole text is placed every time the field is drawn, so each drawing replaces the
  field's glyphs (and an emptied field has none).
- The redraw draws them as shapes in the field's colour, through the field's current
  transform, so the text is sharp at any scale and moves with the field.
- These fields used to be drawn empty: a name of a few letters changes only a few
  percent of a wide field's pixels, below the per-field check below, so the redraw was
  used without the text. Booting all cartridges, the worst field difference fell from
  34% to 5% (Ratatouille (Germany)), from 22% to 8% (Cars (Germany)) and from 19% to 1%
  (SpongeBob Schwammkopf: Zeitreise durch das Wurmloch).

### Validation and coverage

**Validation.** Each redraw at 1× is compared with the LCD, and used only while at most
a quarter of the pixels differ clearly. The LCD is often mid-update while the player
sends its dirty regions, so a mismatch has to persist for several LCD updates before
the display falls back. The typical difference is about 5%, from anti-aliased edges.
- Each text field's area is also compared on its own, since missing text is too small
  for the whole-screen comparison: a field that differs in over 10% of its pixels
  counts as a mismatch.
- The redraw is not used when the game shows something the player did not draw (Cars
  (Germany)'s keyboard letters are drawn by the cartridge's code over the Flash frame),
  or when a frame has content the renderer does not draw yet. Everything else the
  player draws is redrawn.

**Coverage.** Booting all 44 cartridges for 40 s produced 2,553 Flash frames
(`flash_check`):
- all of them redraw completely;
- the median difference from the LCD is 2%, from anti-aliased edges and fractional
  positions that the player's aliased rasteriser rounds to whole pixels.

## Pixel motion (the fallback)

For everything without a usable draw list (native games with other engines, frames that
fail validation), interpolation estimates motion from the two images, in layers: a
camera scroll, plus objects that each move on their own. Pixels are never blended.
1. **Camera.** The shift (up to ±16 px each way) that best maps the previous image
   onto the newest, scored on a sub-sampled grid by luma difference, with a small
   penalty for larger shifts.
2. **Foreground.** Pixels the camera shift does not explain (a colour distance over 24)
   are foreground. If more than half the image is foreground, it is a scene change: the
   images switch instead.
3. **Objects.** Foreground pixels are grouped into 8-connected objects, bridging gaps of
   up to 2 px so a sprite's separate parts (shoes and body) stay one object. Each object
   gets its own displacement, searched coarse (±16 px in steps of 2) then fine; it is
   *explained* if the displacement maps it closely enough (an average colour distance of
   at most 40).
4. **In between.** The background is taken, camera-shifted, from the nearer of the two
   images, avoiding pixels covered by foreground there (from the other image, or the
   nearest visible background within 6 px). Explained objects glide from their old
   position to their new one; the others switch at the midpoint.

Frames more than 8 ticks apart switch at once. This works for scrolling 2D games; it
cannot describe a perspective floor that moves differently on every row, which is why
the Cars races looked broken before they had their own capture.

## Tools

Built with `-DLEAPEMU_BUILD_TOOLS=ON` ([tools/README.md](../tools/README.md)); each
prints its usage without arguments.

| Tool | What it does |
|---|---|
| `drawcap_check BIOS CART STATE TICKS [dump.png]` | Replays each captured draw list (tile planes, Cars) and reports the pixels it fails to reproduce; dumps screen, replay and residual of the first frame with one |
| `flash_check BIOS CART STATE\|- TICKS [dump]` | Redraws each captured Flash frame and reports its difference from the LCD |
| `capture_survey BIOS CART FRAMES [dump_prefix]` | Boots a cartridge with scripted input and reports how its frames are covered: redrawn by native capture, or left to pixel motion (those can be dumped) |
| `swf_play BIOS CART SWF_ADDR FIRST LAST SCALE OUT` | Renders a movie's frames to PNG, played from the cartridge by `flash::Timeline` (the timeline's control tags only, no ActionScript) |
| `interp_eval BIOS CART SWF_ADDR [frames] [scale]` | Interpolates frames N−1 → N+1 of a movie, compares the result with frame N, and measures joint drift |
| `cutscene_eval BIOS CART STATE SWF_ADDR\|- TICKS [scale]` | The same with the emulator's own captures. `STRIPS=prefix STEPS=k` renders each transition at k moments, `PAIR=n` lists how one transition's parts are matched, `SKELETON=0`, `POSE=0` and `REUSE=0` turn rules off for comparison |
| `interp_timing BIOS CART STATE TICKS [sub] [dump]` | Plays a state through the interpolator as the GUI does, `sub` samples per tick, and reports pops (samples that change far more than their neighbours) and how unevenly the image changes. `SCALE=k` also times the redraw at k×; `GOLD=file` saves the redraws, or compares them with an earlier run's |
| `jpeg_check BIOS CART [out]` | Decodes every JPEG in a cartridge's movies (all 1,126 in the 44 cartridges, including the 8 progressive ones) |

The GUI prints its own screen drawing times on exit with `LEAPEMU_FRAME_STATS=1`, and
`leapemu-cli --interp ... --interp-dump DIR` writes what the display would show.

## Adding engines, with the Cars races as an example

Other native games use their own drawing code, and each needs its own capture. This
is how the Cars races were added, in steps that apply to any engine. Every step ends
in a check that is exact, because the emulator is deterministic and the hooks only
observe.

1. **Which code makes the image.** `tools/abi/drawmap.cpp` records the framebuffer
   DMA's source and counts the stores into it by instruction. In Cars, the LCD image
   was blitted from a 160×160 16-bit surface, the same surface format as the
   tile-plane engine's.
2. **Who draws each part of that surface.** `tools/abi/surfacemap.cpp` maps, for every
   pixel of the surface, the call chain of the last store to it in one frame. It
   showed four writers: a tile routine (the sky), one routine for the whole floor, a
   sprite routine (the car, objects) and the HUD. `framecalls`, `calllog` and
   `calltree` then give the call counts, arguments and callers.
3. **Recognise the structure.** The tile routine had DrawPlane's shape (a 32×32 ring
   tilemap, a function per draw mode, a clip rectangle), with a different plane
   layout. The sprite loop was DrawSpriteLayer's (the step to the next object at
   +0x68). The floor routine was new: reading its loop gave the arithmetic above.
4. **Re-implement, and compare.** A scratch program re-implemented the floor and
   compared it, at the routine's return, with what the game had drawn: every pixel of
   every frame matched (once a frame that started in the middle of the routine was
   left out). Only then was it worth building on.
5. **Find the parameters, and see whether they move smoothly.** The floor reads a
   context: camera position and heading changed in small steps every frame, so the
   view could be interpolated. (The horizon was constant in every test drive, and the
   first capture treated it as fixed; a jump showed it is not: the camera tilts.)
6. **Capture.** Hooks at the routines' entries, located by code signature. Other
   builds of the game compile the routines differently and keep their data elsewhere,
   so each build has its own signatures, and the data's addresses are read from the
   routine's own instructions, checked against each other.
7. **Validate.** `drawcap_check` must reproduce every frame exactly (whatever it
   cannot is kept as a static layer, and a frame with too much of it is not used).
   Then the display: in-between images at several scales, the timing (frames 4 or 5
   ticks apart), and the cases that move what was assumed fixed.

Steps 1–3 use `tools/abi/drawmap.cpp`, `surfacemap.cpp`, `framecalls.cpp`,
`calllog.cpp`, `calltree.cpp`, `regionmap.cpp` and `tilelog.cpp`, and
`leapemu-cli --disasm` for reading code. Only the observed behaviour goes into
leapemu; the game's code is located by signature, never copied.
