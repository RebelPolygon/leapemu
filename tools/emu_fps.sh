#!/bin/bash
# Maintainer script: it expects save states and reference footage that are not part
# of the repository (docs/timing.md).
# Needs: LEAPEMU_BIOS (a BaseROM image).
: "${LEAPEMU_BIOS:?set LEAPEMU_BIOS to a BaseROM image}"
# Distinct-frame rate in gameplay: emu_fps.sh STATE CART TIMING|default [extra args...]
ST=$1; C=$2; T=$3; shift 3; D=build/video/efps_$$; rm -rf $D; mkdir -p $D
[ "$T" = default ] && TA=() || TA=(--timing "$T")
./build/leapemu-cli --bios "$LEAPEMU_BIOS" --cart "$C" --load-state $ST "${TA[@]}" --frames 780 \
  --screenshot $D/f --shot-every 1 "$@" >/dev/null 2>&1
python3 - $D <<'PY'
import glob,struct,zlib,sys,numpy as np
def load(p):
    d=open(p,'rb').read(); i=d.index(b'IDAT'); n=struct.unpack('>I',d[i-4:i])[0]
    return np.frombuffer(zlib.decompress(d[i+4:i+4+n]),np.uint8)
fs=sorted(glob.glob(sys.argv[1]+'/f-*.png'))[180:780]
prev=None; n=0
for f in fs:
    x=load(f)
    if prev is not None and np.any(x!=prev): n+=1
    prev=x
print(f"{n/10:.1f} distinct frames/s")
PY
rm -rf $D
