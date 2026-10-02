#!/bin/bash
# Maintainer script: it expects save states and reference footage that are not part
# of the repository (docs/timing.md).
# Needs: LEAPEMU_BIOS (a BaseROM image) and LEAPEMU_ROMS (a folder of cartridge images).
: "${LEAPEMU_BIOS:?set LEAPEMU_BIOS to a BaseROM image}" "${LEAPEMU_ROMS:?set LEAPEMU_ROMS to a folder of ROMs}"
# Runs the Sonic X act-1 rail slide under a timing setting and aligns it with
# the hardware recording. Usage: calibrate_slide.sh R16,R32,M16,M32,S16,S32,I16,I32
set -e
T=$1; D=build/video/cal_$(echo $T | tr , _); rm -rf $D; mkdir -p $D
./build/leapemu-cli --bios "$LEAPEMU_BIOS" --cart "$LEAPEMU_ROMS/Sonic X (USA).zip" \
  --load-state build/video/emu_sonic/s7400.state --timing $T --frames 1800 \
  --touch 38,40@30-40 --touch 28,80@300-330 --screenshot $D/r --shot-every 1 >/dev/null 2>&1
python3 tools/video_align.py "${LEAPEMU_VIDEO:?set LEAPEMU_VIDEO to the Sonic X hardware recording}" 131 135 446:436:93:20 "$D/r-0[0-1]*.png" 60 29.97 | tail -2
