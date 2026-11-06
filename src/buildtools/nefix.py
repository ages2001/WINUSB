#!/usr/bin/env python3
"""
nefix.py <file> <expver>

Rewrites a Watcom-linked NE module into the layout of the Windows 1.x/2.x
drivers so that the loaders of Windows 1.x/2.x and INSTUSB (which links the
driver into WIN100.BIN / WIN200.BIN) accept it (as in WINVBE):
  * expected Windows version <expver> (0x201 for 2.x, 0 for 1.x)
  * plain library flags, target OS unknown, no fast-load area,
  * segments and resources re-aligned to 16 bytes (shift 4),
  * DOS header size fields covering the whole file.
"""
import struct
import sys

fn, ver = sys.argv[1], int(sys.argv[2], 0)
d = bytearray(open(fn, 'rb').read())
ne = struct.unpack_from('<I', d, 0x3C)[0]
assert d[ne:ne + 2] == b'NE'


def hw(off):
    return struct.unpack_from('<H', d, ne + off)[0]


nseg, segtab, restab, resnam = hw(0x1C), hw(0x22), hw(0x24), hw(0x26)
shift = hw(0x32)
NEW = 4

# collect segment data (+ relocations) and resources
segs = []
for i in range(nseg):
    off, ln, fl, mn = struct.unpack_from('<HHHH', d, ne + segtab + i * 8)
    data = b''
    if off:
        p = off << shift
        size = ln or 0x10000
        data = d[p:p + size]
        if fl & 0x100:
            n = struct.unpack_from('<H', d, p + size)[0]
            data += d[p + size:p + size + 2 + n * 8]
    segs.append((off, ln, fl, mn, data))

res = []                                 # (position of NAMEINFO, data)
rshift = 0
if restab != resnam:
    p = ne + restab
    rshift = struct.unpack_from('<H', d, p)[0]
    p += 2
    while True:
        typ = struct.unpack_from('<H', d, p)[0]
        if not typ:
            break
        cnt = struct.unpack_from('<H', d, p + 2)[0]
        p += 8
        for j in range(cnt):
            o, l = struct.unpack_from('<HH', d, p)
            res.append((p, bytes(d[o << rshift:(o << rshift) + (l << rshift)])))
            p += 12

first = min([s[0] << shift for s in segs if s[0]] + [len(d)])
nonres = struct.unpack_from('<I', d, ne + 0x2C)[0]
assert nonres + hw(0x20) <= first
# DOS stub + NE header at 400h as in the Microsoft drivers
NEWNE = 0x400
assert ne <= NEWNE
out = bytearray(d[:ne]).ljust(NEWNE, b'\0') + d[ne:first]
struct.pack_into('<I', out, 0x3C, NEWNE)
struct.pack_into('<I', out, NEWNE + 0x2C, nonres - ne + NEWNE)
res = [(p - ne + NEWNE, data) for p, data in res]
ne = NEWNE


def pad():
    while len(out) % (1 << NEW):
        out.append(0)


for i, (off, ln, fl, mn, data) in enumerate(segs):
    if not off:
        continue
    if ver < 0x200:
        # Windows 1.x: plain flags as in its drivers (preload, fixed,
        # relocations; data bit for the data segment)
        # code segments fixed: Windows 1.x does not fix up internal far
        # references to moveable code segments (no entry table thunks)
        nf = (fl & 0x0111) | 0x0040
        if not fl & 1:
            nf &= ~0x0010
        struct.pack_into("<H", out, ne + segtab + i * 8 + 4, nf)
    pad()
    struct.pack_into('<H', out, ne + segtab + i * 8, len(out) >> NEW)
    out += data
if res:
    struct.pack_into('<H', out, ne + restab, NEW)
    for p, data in res:
        pad()
        units = (len(data) + (1 << NEW) - 1) >> NEW
        struct.pack_into('<HH', out, p, len(out) >> NEW, units)
        # flags as in the Microsoft 1.x/2.x drivers: preload, moveable,
        # pure, priority bits 0C00h (the boot loader expects them preloaded)
        struct.pack_into('<H', out, p + 4, 0x0C70 if ver >= 0x200 else 0x0050)
        out += data.ljust(units << NEW, b'\0')

struct.pack_into('<H', out, ne + 0x32, NEW)                 # alignment shift
struct.pack_into('<H', out, ne + 0x34, 0)                   # resource segments
out[ne + 0x36] = 0                                          # target OS
out[ne + 0x37] = 0                                          # other flags
struct.pack_into('<HHH', out, ne + 0x38, 0, 0, 0)           # fast-load area
struct.pack_into('<H', out, ne + 0x3E, ver)                 # expected version
fl = struct.unpack_from('<H', out, ne + 0x0C)[0] & ~0x0300
struct.pack_into('<H', out, ne + 0x0C, fl)
struct.pack_into('<I', out, ne + 0x08, 0)                   # CRC
# the DOS header must describe the whole file: the 1.x/2.x Setup programs
# read e_cp * 512 bytes when they copy a module into WIN100/WIN200.BIN
struct.pack_into('<HH', out, 2, len(out) % 512, (len(out) + 511) // 512)
open(fn, 'wb').write(out)
