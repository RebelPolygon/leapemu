# Development tools

None of these are needed to play. The C++ tools are built with
`cmake -DLEAPEMU_BUILD_TOOLS=ON` (off by default) and each prints its usage when run
without arguments; the header comment of each source file says what it is for.

- **`debug/`**: checking the emulator against itself.
  - `jit_diff`, `backend_diff`: run two CPU backends side by side and find where
    they first differ ([docs/jit.md](../docs/jit.md)).
  - `flash_check`, `drawcap_check`, `capture_survey`: check the vector redraw and the
    native draw capture against the emulated screen.
  - `interp_eval`, `interp_timing`, `cutscene_eval`, `swf_play`, `jpeg_check`:
    frame interpolation and the Flash renderer
    ([docs/interpolation.md](../docs/interpolation.md)).
  - `code_sig`: the signature of a run of a ROM's code, which is how leapemu finds the
    routines it hooks or patches without copying their bytes
    ([docs/interpolation.md](../docs/interpolation.md)).
- **`abi/`**: probing how cartridges use the BaseROM and how games draw, for the
  clean-room specification ([docs/cart-bios-abi.md](../docs/cart-bios-abi.md)) and
  for adding native capture ([docs/interpolation.md](../docs/interpolation.md#adding-engines-with-the-cars-races-as-an-example)).
  Most use an analysis build of the core with memory-access hooks.
  - `drawmap`: which code writes the frame sent to the LCD; `surfacemap`: which call
    chain drew each pixel of a drawing surface (a PNG coloured by writer);
    `regionmap`: which code writes a screen rectangle;
  - `framecalls`, `calltree`, `calllog`, `callmap`, `argscan`: functions by stores,
    call trees, calls with their arguments;
  - `blitlog`, `tilelog`: one blit or tile routine's calls; `cartreads`, `ioreaders`,
    `firstuse`: who reads the cartridge, an I/O register, an address first;
    `ribdump.py`: a ROM's resource index.
- **`speech/`**: `lfc2wav` decodes the LFC speech assets to WAV;
  `spectrogram.py` compares recordings ([docs/speech.md](../docs/speech.md)).
- **`oracle/`**: comparing the CPU with MAME's, instruction by instruction (see its
  README).
- **Timing calibration** (`calibrate_*.sh`, `emu_fps.sh`, `video_*.py`): maintainer
  scripts. They expect save states and hardware footage that are not part of the
  repository ([docs/timing.md](../docs/timing.md)).

All of them need your own BaseROM and cartridge dumps.

## Environment variables

- **JIT** (both programs): `LEAPEMU_JIT_STATS`, `LEAPEMU_JIT_NOCHAIN`, `LEAPEMU_JIT_DUMP`,
  `LEAPEMU_JIT_PERFMAP`; see [docs/jit.md](../docs/jit.md).
- **GUI test hooks**, for scripted runs of the real GUI:
  - `LEAPEMU_TEST_SCRIPT="ms:op[:x[:y]];..."`: steps at times in milliseconds after
    start. Ops include `cartN` (load the ROM in `LEAPEMU_TEST_CARTN`; `unsigned:path` to
    load it unsigned), `move:x:y` and `tap:x:y` (mouse, window coordinates),
    `key:SCANCODE[:MOD]` (a key press; MOD is an SDL modifier mask, e.g. 64 = left Ctrl),
    `btn:N` (hold console button bit N for 200 ms), `rstick:x:y` and `stylus` /
    `stylusup` (the gamepad stylus), `saveN` / `loadN`, `record` / `play` / `stop`
    (with `LEAPEMU_TEST_MOVIE`), `ff` / `ffoff`, `rwon` / `rwoff`, `settings:PAGE`, and
    `hash` (print the machine state's CRC).
  - `LEAPEMU_QUIT_AFTER_MS`: quit after that long. `LEAPEMU_DUMP_WINDOW=file.png`: save
    the last frame drawn. `LEAPEMU_WINDOW_SIZE=WxH`. `LEAPEMU_FRAME_STATS`: print the
    screen drawing times on exit.
  - Run with `SDL_VIDEO_DRIVER=offscreen SDL_RENDER_DRIVER=software` (and
    `XDG_DATA_HOME` pointing to a scratch folder): with a real window, the mouse's own
    position overrides the scripted one while the window has focus.
