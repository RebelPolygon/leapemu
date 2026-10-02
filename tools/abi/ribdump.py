#!/usr/bin/env python3
"""Dump the RIB groups of a Leapster ROM image (format per lfhacks/Leapster-Tools).
Usage: ribdump.py IMAGE [GROUP_ID_HEX ...]  -- prints group table and raw words of selected groups."""
import struct, sys, zipfile
def load(p):
    if p.endswith('.zip'):
        z = zipfile.ZipFile(p); return z.read([n for n in z.namelist() if n.lower().endswith('.bin')][0])
    d = open(p, 'rb').read()
    return d[d.index(b'data') + 8:] if d[:4] == b'RIFF' else d
img = load(sys.argv[1]); want = [int(x, 16) for x in sys.argv[2:]]
base, = struct.unpack_from('<I', img, 0x11c); rib, = struct.unpack_from('<I', img, 0x140)
r = rib - base; groups, = struct.unpack_from('<H', img, r + 6)
print(f'device base {base:08x}, RIB at {rib:08x}, {groups} groups')
for g in range(groups):
    gid, cnt, addr = struct.unpack_from('<HHI', img, r + 32 + 8 * g)
    print(f'  group {gid:04x} count {cnt:3d} at {addr:08x}')
    if gid in want and base <= addr < base + len(img):
        o = addr - base
        words = struct.unpack_from('<' + 'I' * min(16, cnt * 4), img, o)
        print('     ' + ' '.join(f'{w:08x}' for w in words))
