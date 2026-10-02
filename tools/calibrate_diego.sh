#!/bin/bash
# Maintainer script: it expects save states and reference footage that are not part
# of the repository (docs/timing.md).
# Needs: LEAPEMU_BIOS (a BaseROM image) and LEAPEMU_ROMS (a folder of cartridge images).
: "${LEAPEMU_BIOS:?set LEAPEMU_BIOS to a BaseROM image}" "${LEAPEMU_ROMS:?set LEAPEMU_ROMS to a folder of ROMs}"
# Diego zipline duration (white flash end -> black frame) under a timing; prints emulator seconds.
T=$1; D=build/video/cald_$(echo $T | tr , _); rm -rf $D; mkdir -p $D
./build/leapemu-cli --bios "$LEAPEMU_BIOS" --cart "$LEAPEMU_ROMS/Go Diego Go! - Animal Rescuer (USA).zip" \
  --load-state build/video/emu_diego/s_menu2.state --timing $T --frames 1800 \
  --touch 20,18@20-32 --touch 36,40@400-412 --screenshot $D/z --shot-every 2 >/dev/null 2>&1
python3 - "$D" <<'PY'
import glob,struct,zlib,sys,numpy as np
def load(p):
    d=open(p,'rb').read(); i=d.index(b'IDAT'); n=struct.unpack('>I',d[i-4:i])[0]
    return np.frombuffer(zlib.decompress(d[i+4:i+4+n]),np.uint8).mean()
b=np.array([load(f) for f in sorted(glob.glob(sys.argv[1]+'/z-*.png'))])
w=np.nonzero(b>235)[0]
if len(w)==0: print("no white flash (navigation missed)"); sys.exit()
lw=w[-1]; k=np.nonzero((b<15)&(np.arange(len(b))>lw))[0]
if len(k)==0: print("no black frame"); sys.exit()
d=(k[0]-lw)/30; print(f"zipline+wipe {d:.2f}s -> speed factor {4.40/d:.3f}x hardware")
PY
