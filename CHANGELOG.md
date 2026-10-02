# Changelog

## 0.1.0 (2026-10-02)

The first public version.

- Leapster emulation (L-MAX games play too; its TV output is not emulated): the ARCompact CPU (interpreter, cached
  interpreter and a JIT for x86-64 and AArch64, all with identical results), the
  hardware (LCD, sound with all 8 voices and the LFC speech codec, touch screen,
  EEPROMs, timers, UART), on the original BaseROM.
- Every retail cartridge tested (62) reaches gameplay, as do the 6 prototypes and the
  13 Leapster 2 downloadable games tested (five through leapemu's own Leapster 2
  services).
- GUI: a game list with compatibility ratings, save states, rewind, frame stepping,
  fast forward, input movies, a gamepad (with a stylus cursor), display filters, and
  optional vector scaling and frame interpolation of Flash content and of the
  Cars races (whose road is redrawn from the game's own camera).
- ROM tools: a game's assets (Flash movies, speech, sounds, music, bitmaps, fonts),
  viewed, played and exported; a hex view with switchable patches.
- A CLI for running, testing and exporting without a window.
- Builds for Windows (x86-64) and Linux (x86-64 and arm64, portable; with a
  PKGBUILD to package them on Arch Linux).
