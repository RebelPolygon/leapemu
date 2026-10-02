#!/usr/bin/env python3
"""Package a flat ARC binary as a Leapster cartridge image.

Layout (see docs/open-bios/spec/boot.md):
  0x000-0x0ff  zero
  0x100        AppTable header ("Copyright LeapFrog     ", version 1.0, 1 RIB,
               device range, checksum pointers, RIB pointer at 0x140)
  0x144        RIB ("LEAP" v1.0) with a boot group (0x1000) and product info (0x1003)
  0x200...     boot descriptor, product-info strings, placeholder security block
  code_base    the program (linked at CODE_BASE, entry = first byte)
  end-8/end-4  sparse / full checksums

The security block (product info id 5) is a placeholder: the image is unsigned.
It boots on the open BaseROM, or on a LeapFrog BaseROM in leapemu with
--allow-unsigned / "Allow unsigned cartridges".
"""
import argparse, struct, time

BASE = 0x80000000
CODE_BASE = 0x80001000

def sparse_sum(img, span):
    s, off = 0, 0
    while off + 4 <= span:
        s = (s + struct.unpack_from('<I', img, off)[0]) & 0xffffffff
        off += 0xF94
    return s

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('binary'); ap.add_argument('output')
    ap.add_argument('--title', default='leapemu homebrew')
    ap.add_argument('--part', default='HB-0001')
    ap.add_argument('--size', type=lambda x: int(x, 0), default=0x100000, help='image size (power of two)')
    ap.add_argument('--ram', type=lambda x: int(x, 0), default=0x3c000200, help='module data area address')
    ap.add_argument('--ram-size', type=lambda x: int(x, 0), default=0x100000)
    ap.add_argument('--product-id', type=lambda x: int(x, 0), default=0x0002F001,
                    help='this cartridge\'s product id (0x0002Fxxx reserved here for homebrew)')
    a = ap.parse_args()

    code = open(a.binary, 'rb').read()
    size = a.size
    if CODE_BASE - BASE + len(code) > size - 16:
        raise SystemExit('program too large for image size')
    img = bytearray(size)
    img[CODE_BASE - BASE:CODE_BASE - BASE + len(code)] = code
    end = BASE + size - 0xC

    # Strings and blocks placed after the RIB.
    heap = 0x400
    def put(data):
        nonlocal heap
        addr = BASE + heap
        img[heap:heap + len(data)] = data
        heap = (heap + len(data) + 3) & ~3
        return addr
    title = put(a.title.encode() + b'\0')
    part = put(a.part.encode() + b'\0')
    date = put(time.strftime('%b %d %Y %H:%M:%S').encode() + b'\0')
    tool = put(b'leapemu mkcart\0')
    # Product-ID lists (ids 2 and 3): u16 version 0x0100, u16 count, 8 reserved
    # bytes, then `count` u32 product ids. The needed id 0x00010001 is the base
    # system that all examined cartridges require (docs/open-bios/spec/boot.md).
    provided = put(struct.pack('<HHII', 0x0100, 1, 0, 0) + struct.pack('<I', a.product_id))
    needed = put(struct.pack('<HHII', 0x0100, 1, 0, 0) + struct.pack('<I', 0x00010001))
    verse = put(b'Built with the leapemu homebrew SDK.\0')
    security = put(b'UNSIGNED HOMEBREW - no LeapFrog Approved Content signature.\0')
    build_user = put(b'homebrew\0')
    build_machine = put(b'leapemu\0')
    validation = put(bytes(20))  # no digest: unsigned
    descriptor = put(struct.pack('<6I', 0x100, a.ram, a.ram_size, 0, CODE_BASE, CODE_BASE) + bytes(24))

    # Header.
    img[0x100:0x117] = b'Copyright LeapFrog     '
    struct.pack_into('<BBHIIIII', img, 0x118, 0, 1, 1, BASE, end, end + 8, end + 4, 0)
    struct.pack_into('<I', img, 0x140, BASE + 0x144)

    # RIB with two groups.
    rib = 0x144
    boot_tab, info_tab = 0x200, 0x210
    img[rib:rib + 8] = b'LEAP' + bytes([0, 1]) + struct.pack('<H', 2)
    struct.pack_into('<HHI', img, rib + 32, 0x1000, 1, BASE + boot_tab)
    struct.pack_into('<HHI', img, rib + 40, 0x1003, 14, BASE + info_tab)
    struct.pack_into('<II', img, boot_tab, 0x01000007, descriptor)
    infos = [(0x1, a.product_id), (0x2, needed), (0x3, provided), (0x4, verse), (0x5, security),
             (0x7, date), (0x8, 0), (0xA, part), (0xB, title), (0xC, tool), (0xD, build_user),
             (0xE, build_machine), (0xF, date), (0x10, validation)]
    for i, (iid, val) in enumerate(infos):
        struct.pack_into('<HBBI', img, info_tab + 8 * i, iid, 0, 1, val)
    assert heap < CODE_BASE - BASE, 'metadata overflow'

    # Checksums: sparse at end+4 (image size - 8), full at end+8 (image size - 4).
    span = end - BASE
    struct.pack_into('<I', img, size - 8, sparse_sum(img, span))
    full = sum(struct.unpack_from('<%dI' % ((size - 8) // 4), img, 0)) & 0xffffffff
    struct.pack_into('<I', img, size - 4, full)
    open(a.output, 'wb').write(img)
    print(f'{a.output}: {size} bytes, entry {CODE_BASE:08x}, {len(code)} bytes of code')

main()
