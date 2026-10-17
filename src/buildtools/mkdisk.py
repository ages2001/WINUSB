#!/usr/bin/env python3
"""mkdisk.py out.img SIZE_MB part[,part...]   part = type:sizeMB[:label]  (type 06/0E/04/01, 'E' prefix -> logical in extended)
Example: mkdisk.py d.img 512 06:200:DATA1 E06:100:LOG1 E06:100:LOG2
Creates MBR (+EBR chain for E-prefixed parts) and formats FAT partitions with mkfs.fat."""
import struct, subprocess, sys

HEADS, SPT = 255, 63

def chs(lba):
    c = lba // (HEADS * SPT)
    if c > 1023:
        return b"\xfe\xff\xff"
    h = (lba // SPT) % HEADS
    s = lba % SPT + 1
    return bytes([h, ((c >> 2) & 0xC0) | s, c & 0xFF])

def entry(boot, typ, start, size):
    return bytes([boot]) + chs(start) + bytes([typ]) + chs(start + size - 1) + struct.pack("<II", start, size)

def fmt(img, start, size, typ, label):
    fat = "16" if typ in (0x04, 0x06, 0x0E) else ("32" if typ in (0x0B, 0x0C) else "12")
    args = ["mkfs.fat", "-F", fat, "-h", str(start), "--offset", str(start), "-n", label[:11] or "NO NAME", img, str(size // 2)]
    subprocess.run(args, check=True, capture_output=True)

def main():
    out, mb = sys.argv[1], int(sys.argv[2])
    total = mb * 2048
    with open(out, "wb") as f:
        f.truncate(total * 512)
    prim, logi = [], []
    for p in sys.argv[3:]:
        f = p.split(":")
        t = f[0]
        lab = f[2] if len(f) > 2 else "W3TEST"
        if t.startswith("E"):
            logi.append((int(t[1:], 16), int(f[1]) * 2048, lab))
        else:
            prim.append((int(t, 16), int(f[1]) * 2048, lab))
    mbr = bytearray(512)
    cur = SPT
    ents = []
    for typ, size, lab in prim:
        ents.append((typ, cur, size, lab)); cur += size
    ext_start = None
    if logi:
        ext_start = cur
        ext_size = sum(s + SPT for _, s, _ in logi)
        ents.append((0x0F, ext_start, ext_size, None))
    for i, (typ, st, sz, lab) in enumerate(ents):
        mbr[446 + 16 * i:446 + 16 * (i + 1)] = entry(0, typ, st, sz)
    mbr[510:512] = b"\x55\xaa"
    struct.pack_into("<I", mbr, 440, 0x57335354)
    with open(out, "r+b") as f:
        f.write(mbr)
    for typ, st, sz, lab in ents:
        if lab is not None:
            fmt(out, st, sz, typ, lab)
    if logi:
        pos = ext_start
        for i, (typ, sz, lab) in enumerate(logi):
            ebr = bytearray(512)
            ebr[446:462] = entry(0, typ, pos + SPT, sz)
            struct.pack_into("<I", ebr, 446 + 8, SPT)
            nxt = pos + SPT + sz
            if i + 1 < len(logi):
                nsz = logi[i + 1][1] + SPT
                ebr[462:478] = entry(0, 0x05, nxt, nsz)
                struct.pack_into("<I", ebr, 462 + 8, nxt - ext_start)
            ebr[510:512] = b"\x55\xaa"
            with open(out, "r+b") as f:
                f.seek(pos * 512); f.write(ebr)
            fmt(out, pos + SPT, sz, typ, lab)
            pos = nxt

if __name__ == "__main__":
    main()
