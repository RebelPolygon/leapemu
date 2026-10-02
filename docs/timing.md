# CPU timing

The ARCtangent-A5 runs at 96 MHz, but much of its code executes from slow 16-bit
memory. Real hardware therefore retires fewer than one instruction per cycle.

- **Timer-paced sequences** (cutscenes, logo screens) run correctly at one instruction
  per cycle.
- **Throughput-paced sequences run too fast at one instruction per cycle.** Games step
  their logic once per rendered frame, so gameplay speed tracks rendering speed.

## Model

Each bus access adds wait states by region, at 1/8-cycle resolution:
- instruction fetch costs one 16-bit access per halfword;
- data accesses cost per 16/32-bit access.

Instruction-fetch costs are precomputed in the decode cache, so the model costs little.

| Region | 16-bit | 32-bit |
|---|---|---|
| BaseROM | 0 | 0 |
| Cartridge ROM | 0.75 | 1.5 |
| Main RAM | 1.0 | 2.0 |
| On-chip SRAM (VRAM) | 0 | 0 |
| Peripherals | 1.5 | 1.5 |

The BaseROM is on the main board; cartridge ROM sits behind the cartridge connector.
The fit shows the hardware runs BaseROM code (which includes the Flash player) at close
to one instruction per cycle, and cartridge code much slower.

To change the model:
- **GUI:** Tools > Settings > Emulation > Accurate timing.
- **CLI:**
  - `--timing` sets all regions (ROM applies to the cartridge too).
  - `--cart-timing` overrides the cartridge.
  - `--no-waits` runs one instruction per cycle.
  - `--mame-compat` also disables the waits, since MAME runs one instruction per cycle.

## Calibration

References:
- **Native games:** real-hardware playthroughs of Sonic X and Go Diego Go! Animal
  Rescuer, captured from TV output.
- **Flash games:** each embedded SWF movie declares its frame rate, 8 fps for nearly
  all of SpongeBob Saves the Day and Pet Pals. These are LeapFrog titles authored for
  this hardware, so the authored rate is taken as the hardware rate. That is an
  assumption; footage would confirm it.

Measured with:
- `tools/video_align.py` (sequence alignment);
- `tools/video_fps.py`, `tools/emu_fps.sh` and `tools/calibrate_games.sh` (distinct
  rendered frames per second). These are maintainer scripts: they expect save states
  and reference footage that are not part of the repository.

| Measurement | Reference | 1 instr/cycle | Old flat model | **Current** |
|---|---|---|---|---|
| Sonic X intro cutscene (timer-paced, 66 s) | 1.000× | 1.001× | 1.000× | **1.000×** |
| Sonic X gameplay | ~16 fps (hardware) | 23 fps | 17.8 fps | **15.9 fps** |
| Go Diego Go gameplay | ~16 fps (14–17, hardware) | 36 fps | 14.9 fps | **15.0 fps** |
| SpongeBob gameplay | 8 fps (authored) | 7.5 fps | 5.5 fps | **7.5 fps** |
| Pet Pals intro (not used in the fit) | 8 fps (authored) | 7.8 fps | 5.0 fps | **7.2 fps** |

Rendering rates are quantized because games wait for a 60 Hz tick after each frame:
- 20 fps is 3 ticks, 15 fps is 4, 12 fps is 5, and 7.5 fps is 8.
- Hardware runs Sonic and Diego mostly at 4 ticks per frame, with some 3-tick frames.
- The remaining ±10% is within the noise of these tests, because the emulator scenes
  are not identical to the recorded ones.

### What did not fit

**The old flat model.** It gave the BaseROM and the cartridge the same ROM cost (0.625
cycles per halfword) with RAM at 1.5. It matched Sonic and Diego but ran the BaseROM's
Flash player about 30% below the authored rate.

**A pure cache model.** The BaseROM writes the ARC cache-control registers:
- `IC_IVIC` and `IC_CTRL` for the instruction cache;
- `DC_IVDC`, `DC_CTRL` and `DC_FLSH` for the data cache;
- the boot code flushes the data cache.

So the SoC has instruction and data caches. A timing-only cache model is implemented
(`src/core/arc/cache.h`; CLI `--cache IKB,DKB[,LINE,WAYS]` and `--fill ROM16,RAM16`). A
sweep of 1–8 KiB caches, 16/32-byte lines and fill costs could not fit Sonic, Diego and
SpongeBob together:
- Sonic's hot loops stay cache-resident unless the caches are tiny;
- at those sizes, SpongeBob collapses to about 2 fps.

The model stays available for experiments but is off by default.

The model changes only timing. The cached interpreter and the reference interpreter
stay byte-identical under any settings, including the cache model
(`build/backend_diff BIOS CART NVRAM FRAMES [cache]`). The JIT supports the flat model
only; with the cache model on, the cached interpreter runs in its place.
