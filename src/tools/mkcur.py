#!/usr/bin/env python3
# mkcur.py - draws the autoscroll cursors of WINWHEEL.EXE (32x32 monochrome .cur files):
# ASALL.CUR (origin mark: a dot with four arrow heads) and ASUP / ASDOWN / ASLEFT / ASRIGHT.CUR
# (the dot with the arrow head of the scroll direction). White shapes with a black outline,
# hot spot in the middle.
# usage: mkcur.py <output directory>
import os, struct, sys

N = 32
C = 16                      # hot spot / centre


def tri(tip, base1, base2):
    """inside test of a filled triangle"""
    (x1, y1), (x2, y2), (x3, y3) = tip, base1, base2

    def side(ax, ay, bx, by, px, py):
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax)

    def f(x, y):
        px, py = x + 0.5, y + 0.5
        d1 = side(x1, y1, x2, y2, px, py)
        d2 = side(x2, y2, x3, y3, px, py)
        d3 = side(x3, y3, x1, y1, px, py)
        neg = d1 < 0 or d2 < 0 or d3 < 0
        pos = d1 > 0 or d2 > 0 or d3 > 0
        return not (neg and pos)
    return f


def dot(r):
    return lambda x, y: (x + 0.5 - C) ** 2 + (y + 0.5 - C) ** 2 <= r * r


HEADS = {
    "up": tri((C, 1), (C - 7, 9), (C + 7, 9)),
    "down": tri((C, N - 1), (C - 7, N - 9), (C + 7, N - 9)),
    "left": tri((1, C), (9, C - 7), (9, C + 7)),
    "right": tri((N - 1, C), (N - 9, C - 7), (N - 9, C + 7)),
}


def cursor(shapes):
    inside = [[any(s(x, y) for s in shapes) for x in range(N)] for y in range(N)]

    def outline(x, y):
        if inside[y][x]:
            return False
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                xx, yy = x + dx, y + dy
                if 0 <= xx < N and 0 <= yy < N and inside[yy][xx]:
                    return True
        return False
    xor_rows, and_rows = [], []
    for y in range(N - 1, -1, -1):                      # bottom-up
        xb = bytearray(N // 8)
        ab = bytearray(N // 8)
        for x in range(N):
            bit = 0x80 >> (x % 8)
            if inside[y][x]:
                xb[x // 8] |= bit                       # white
            elif not outline(x, y):
                ab[x // 8] |= bit                       # transparent
        xor_rows.append(bytes(xb))
        and_rows.append(bytes(ab))
    bmp = struct.pack("<IiiHHIIiiII", 40, N, N * 2, 1, 1, 0, 0, 0, 0, 2, 0)
    bmp += bytes([0, 0, 0, 0, 255, 255, 255, 0])
    bmp += b"".join(xor_rows) + b"".join(and_rows)
    hdr = struct.pack("<HHH", 0, 2, 1)
    hdr += struct.pack("<BBBBHHII", N, N, 0, 0, C, C, len(bmp), 6 + 16)
    return hdr + bmp


def main():
    out = sys.argv[1]
    files = {"ASALL.CUR": [dot(3.5)] + list(HEADS.values()),
             "ASUP.CUR": [dot(3.5), HEADS["up"]],
             "ASDOWN.CUR": [dot(3.5), HEADS["down"]],
             "ASLEFT.CUR": [dot(3.5), HEADS["left"]],
             "ASRIGHT.CUR": [dot(3.5), HEADS["right"]]}
    for name, shapes in files.items():
        with open(os.path.join(out, name.lower()), "wb") as f:
            f.write(cursor(shapes))


if __name__ == "__main__":
    main()
