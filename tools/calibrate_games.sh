#!/bin/bash
# Maintainer script: it expects save states and reference footage that are not part
# of the repository (docs/timing.md).
# Needs: LEAPEMU_BIOS (a BaseROM image) and LEAPEMU_ROMS (a folder of cartridge images).
: "${LEAPEMU_BIOS:?set LEAPEMU_BIOS to a BaseROM image}" "${LEAPEMU_ROMS:?set LEAPEMU_ROMS to a folder of ROMs}"
# Gameplay render rate (distinct frames/s) of the calibration scenes under one
# timing setting. Usage: calibrate_games.sh [leapemu-cli timing args...]
# e.g. calibrate_games.sh --cache 8,8 --fill 5,1.5
# Hardware references: Sonic X ~16, Go Diego Go ~16 (docs/timing.md);
# SpongeBob's movies are authored at 8 fps (7.5 = 8 ticks at 60 Hz).
R=$LEAPEMU_ROMS
JUMPS=""; for f in $(seq 60 60 780); do JUMPS="$JUMPS --press a@$f-$((f+6))"; done
T=default  # timing options come from "$@"
{
  echo "Sonic $(tools/emu_fps.sh build/video/emu_sonic/s_game.state "$R/Sonic X (USA).zip" $T "$@" --press right@0-780 $JUMPS)" &
  echo "Diego $(tools/emu_fps.sh build/video/emu_diego/s_game.state "$R/Go Diego Go! - Animal Rescuer (USA).zip" $T "$@" --press right@0-780)" &
  echo "SpongeBob $(tools/emu_fps.sh build/scratch/sb/j2400.state "$R/Spongebob Squarepants - Saves the Day (USA).zip" $T "$@")" &
  wait
} | sort | awk '{printf "%s %s  ", $1, $2} END {print ""}'
