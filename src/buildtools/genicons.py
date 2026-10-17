#!/usr/bin/env python3
"""genicons.py - draws the WINDRV program icons (32x32, 16 VGA colors) and writes
   src/inc/wicons.h  : AND masks and device-dependent XOR bits (VGA/EGA 4 planes
                       scan-line interleaved, monochrome) for Program Manager groups
                       of Windows 3.0 that the DOS installers write directly
   src/win/*.ico     : the same icons as Windows 3.x icon files for the programs
   and doc/icons.png : a preview"""
import os, struct
from PIL import Image, ImageDraw

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
PAL = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
       (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255),
       (255, 0, 255), (0, 255, 255), (255, 255, 255)]
BLACK, DRED, DGRN, DYEL, DBLU, DMAG, DCYN, LGRY, DGRY, RED, GRN, YEL, BLU, MAG, CYN, WHT = range(16)
T = 16          # transparent


def canvas():
    im = Image.new('P', (32, 32), T)
    im.putpalette(sum(PAL, ()) + (255, 0, 255) * 240)
    return im, ImageDraw.Draw(im)


def panel(d, col, edge=None):
    """a raised rounded tile that frames the glyph, with a light top/left bevel"""
    if edge is None:
        edge = BLACK
    d.rounded_rectangle([1, 1, 30, 30], 4, fill=col, outline=edge)
    d.line([(4, 2), (27, 2)], fill=WHT)
    d.line([(2, 4), (2, 27)], fill=WHT)
    d.line([(5, 29), (27, 29)], fill=DGRY)
    d.line([(29, 5), (29, 27)], fill=DGRY)


def usb_icon():
    # white USB trident on a blue tile
    im, d = canvas()
    panel(d, DBLU)
    d.line([(16, 6), (16, 26)], fill=WHT, width=2)
    d.polygon([(16, 3), (13, 9), (19, 9)], fill=WHT)              # arrow head
    d.ellipse([13, 24, 19, 30], fill=WHT)                        # stem base dot
    d.line([(16, 19), (9, 13)], fill=WHT, width=2)               # left branch
    d.rectangle([5, 9, 11, 14], fill=CYN, outline=WHT)           # left square
    d.line([(16, 15), (23, 10)], fill=WHT, width=2)              # right branch
    d.ellipse([20, 6, 26, 12], fill=CYN, outline=WHT)            # right circle
    return im


def speaker_icon():
    im, d = canvas()
    panel(d, DCYN)
    d.rectangle([4, 12, 10, 20], fill=DGRY, outline=BLACK)       # magnet box
    d.polygon([(10, 12), (18, 5), (18, 27), (10, 20)], fill=LGRY, outline=BLACK)  # cone
    d.line([(10, 12), (10, 20)], fill=WHT)
    for r, col in ((5, CYN), (9, CYN), (13, WHT)):               # sound waves
        d.arc([18 - r, 16 - r, 18 + r, 16 + r], 305, 55, fill=col, width=2)
    return im


def disk_icon(bench=False):
    im, d = canvas()
    panel(d, DGRY if not bench else DBLU)
    d.rectangle([4, 9, 27, 24], fill=LGRY, outline=BLACK)        # drive body
    d.line([(5, 10), (26, 10)], fill=WHT)
    d.line([(5, 10), (5, 23)], fill=WHT)
    d.line([(6, 23), (26, 23)], fill=DGRY)
    d.rectangle([6, 20, 10, 22], fill=GRN if not bench else RED) # activity LED
    d.line([(13, 21), (25, 21)], fill=DGRY)
    if bench:
        d.pieslice([9, 5, 23, 19], 180, 360, fill=WHT, outline=BLACK)   # speed gauge
        for a in (200, 230, 260, 290, 320, 350):
            import math
            x0 = 16 + 6 * math.cos(math.radians(a)); y0 = 12 + 6 * math.sin(math.radians(a))
            d.point([(x0, y0)], fill=BLACK)
        d.line([(16, 12), (21, 8)], fill=RED, width=2)           # needle
        d.ellipse([15, 11, 17, 13], fill=BLACK)
    else:
        d.ellipse([17, 12, 25, 20], fill=DBLU)                   # platter
        d.ellipse([19, 14, 23, 18], fill=BLU)
        d.point([(20, 15), (21, 15)], fill=WHT)
    return im


def note_icon():
    # bright musical notes on a light tile
    im, d = canvas()
    panel(d, DMAG)
    d.line([(12, 7), (12, 23)], fill=WHT, width=2)               # stems
    d.line([(24, 4), (24, 20)], fill=WHT, width=2)
    d.polygon([(12, 7), (24, 4), (24, 9), (12, 12)], fill=WHT)   # beam
    d.ellipse([6, 20, 13, 26], fill=YEL, outline=WHT)            # note heads
    d.ellipse([18, 17, 25, 23], fill=CYN, outline=WHT)
    return im


def camera_icon():
    # webcam sphere on a stand
    im, d = canvas()
    panel(d, DCYN)
    d.rectangle([13, 25, 19, 28], fill=DGRY, outline=BLACK)      # stand foot
    d.line([(16, 24), (16, 26)], fill=DGRY, width=2)             # neck
    d.ellipse([6, 5, 26, 25], fill=LGRY, outline=BLACK)          # body sphere
    d.arc([8, 7, 20, 19], 150, 300, fill=WHT)                    # highlight
    d.ellipse([11, 10, 23, 22], fill=DBLU, outline=BLACK)        # lens ring
    d.ellipse([13, 12, 21, 20], fill=BLU)                        # glass
    d.ellipse([15, 14, 18, 17], fill=WHT)                        # catchlight
    d.ellipse([22, 6, 25, 9], fill=RED)                          # status light
    return im


def mouse_icon():
    # two-button mouse with a cord
    im, d = canvas()
    panel(d, DGRY)
    d.line([(16, 6), (16, 2)], fill=BLACK, width=2)              # cord up to the tile edge
    d.arc([8, 1, 16, 7], 0, 180, fill=BLACK)
    d.rounded_rectangle([9, 6, 23, 29], 6, fill=LGRY, outline=BLACK)   # body
    d.arc([10, 7, 22, 15], 150, 360, fill=WHT)                   # top highlight
    d.line([(16, 7), (16, 14)], fill=BLACK)                      # button split
    d.line([(9, 14), (23, 14)], fill=DGRY)
    d.rectangle([15, 8, 17, 12], fill=DBLU, outline=BLACK)       # wheel
    return im


ICONS = [('usb', usb_icon()), ('speaker', speaker_icon()), ('disk', disk_icon()),
         ('bench', disk_icon(True)), ('note', note_icon()), ('camera', camera_icon())]


def bits(im):
    px = im.load()
    andm = bytearray(128)
    xor4 = bytearray(512)
    xor1 = bytearray(128)
    for y in range(32):
        for x in range(32):
            v = px[x, y]
            byte, bit = y * 4 + x // 8, 0x80 >> (x % 8)
            if v == T:
                andm[byte] |= bit
                continue
            for p in range(4):
                if v & (1 << p):
                    xor4[y * 16 + p * 4 + x // 8] |= bit
            r, g, b = PAL[v]
            if r + g + b >= 384:
                xor1[byte] |= bit
    return andm, xor4, xor1


def ico(im):
    """Windows 3.x icon file: one 32x32 16-color image"""
    px = im.load()
    andm, _, _ = bits(im)
    hdr = struct.pack('<IiiHHIIiiII', 40, 32, 64, 1, 4, 0, 512 + 128, 0, 0, 16, 0)
    pal = b''.join(struct.pack('<BBBB', b, g, r, 0) for r, g, b in PAL)
    xor = bytearray()
    for y in range(31, -1, -1):
        for x in range(0, 32, 2):
            a, b = px[x, y], px[x + 1, y]
            a = 0 if a == T else a
            b = 0 if b == T else b
            xor.append((a << 4) | b)
    amask = bytearray()
    for y in range(31, -1, -1):
        amask += andm[y * 4:y * 4 + 4]
    img = hdr + pal + bytes(xor) + bytes(amask)
    return struct.pack('<HHH', 0, 1, 1) + struct.pack('<BBBBHHII', 32, 32, 16, 0, 1, 4, len(img), 22) + img


def carr(name, data):
    s = 'static const unsigned char %s[%d] = {\n' % (name, len(data))
    for i in range(0, len(data), 16):
        s += '    ' + ', '.join('0x%02X' % b for b in data[i:i + 16]) + ',\n'
    return s + '};\n'


def main():
    out = ['/* wicons.h - generated by tools/genicons.py: WINDRV program icons for the',
           '   Program Manager groups of Windows 3.0 (AND mask, XOR bits for 4-plane VGA/EGA',
           '   drivers, scan lines interleaved, and for monochrome drivers) */', '']
    names = []
    for name, im in ICONS:
        a, x4, x1 = bits(im)
        out.append(carr('ic_%s_and' % name, a))
        out.append(carr('ic_%s_x4' % name, x4))
        out.append(carr('ic_%s_x1' % name, x1))
        names.append(name)
    out.append('#define ICON_LIST \\')
    out.append(' \\\n'.join('    ICON_ENTRY(%s)' % n for n in names))
    open(os.path.join(ROOT, 'src/inc/wicons.h'), 'w').write('\n'.join(out) + '\n')
    for name, im in ICONS:
        open(os.path.join(ROOT, 'src/win/%s.ico' % name), 'wb').write(ico(im))
    prev = Image.new('RGB', (len(ICONS) * 72, 72), (192, 192, 192))
    for i, (name, im) in enumerate(ICONS):
        rgb = Image.new('RGB', (32, 32), (192, 192, 192))
        px = im.load()
        for y in range(32):
            for x in range(32):
                if px[x, y] != T:
                    rgb.putpixel((x, y), PAL[px[x, y]])
        prev.paste(rgb.resize((64, 64), 0), (i * 72 + 4, 4))
    os.makedirs(os.path.join(ROOT, 'doc'), exist_ok=True)
    prev.save('/tmp/claude-0/icons.png')


if __name__ == '__main__':
    main()
