#!/usr/bin/env python3
"""Post-process an Open Watcom LE VxD for the Windows 3.1 WIN386 loader (ADDHDR-like):
   - data pages start on a 512-byte boundary
   - preload page count filled in
   - entry point flags = exported + shared
   - MZ image size covers the whole file"""
import struct, sys
p = sys.argv[1]
dynamic = len(sys.argv) > 2 and sys.argv[2] == "--dynamic"   # Windows 95 VXDLDR file: entry flags only
d = bytearray(open(p, "rb").read())
le = struct.unpack_from("<I", d, 0x3C)[0]
assert d[le:le+2] == b"LE"
datapg = struct.unpack_from("<I", d, le + 0x80)[0]
nonres = struct.unpack_from("<I", d, le + 0x88)[0]
pad = (-datapg) % 512
if dynamic:
    pad = 0
if pad:
    d[datapg:datapg] = bytes(pad)
    struct.pack_into("<I", d, le + 0x80, datapg + pad)
    if nonres:
        struct.pack_into("<I", d, le + 0x88, nonres + pad)
objtab = struct.unpack_from("<I", d, le + 0x40)[0] + le
nobj = struct.unpack_from("<I", d, le + 0x44)[0]
pre = 0
for i in range(nobj):
    vsize, base, flags, pidx, pcnt, _ = struct.unpack_from("<6I", d, objtab + i * 24)
    if flags & 0x40:
        pre += pcnt
    if not dynamic:
        struct.pack_into("<I", d, objtab + i * 24 + 8, flags | 0x0004)
if not dynamic:
    struct.pack_into("<I", d, le + 0x84, pre)
ent = struct.unpack_from("<I", d, le + 0x5C)[0] + le
cnt, typ = d[ent], d[ent + 1]
if typ == 3:
    for i in range(cnt):
        d[ent + 4 + i * 5] |= 0x02
n = len(d)
struct.pack_into("<HH", d, 2, n % 512, (n + 511) // 512)
open(p, "wb").write(d)
print("%s: pad %d, %d preload pages" % (p, pad, pre))
