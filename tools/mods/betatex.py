#!/usr/bin/env python3
"""
betatex.py - the January 2002 preview's textures (.tex) as retail .xtex.

A preview .tex: 14-byte header (u16 width, height, format, levels, three
flags), then by format
   1, 14   256-colour palette (BGRA, 1024 bytes), 8-bit indices
  15, 16   16-colour palette (BGRA, 64 bytes), 4-bit indices (low first)
  17       24-bit RGB         18  32-bit RGBA        6  16-bit (1555)
and the levels, largest first. The retail game reads a linear A8R8G8B8
.xtex (format 0x12, one level) whatever else it was, so that is what this
writes, red first. PS2 alphas run 0..0x80; they are doubled.

Usage: betatex.py [--solid] <in.tex> <out.xtex>
       (--solid: opaque when it has no see-through holes -- characters)
       betatex.py --png <in.tex> <out.png>     (to look at it)
"""
import struct, sys


def bgra(e):
    return (e[2], e[1], e[0], e[3])


def decode(d):
    w, h, fmt, lv, f1, f2, f3 = struct.unpack_from('<7H', d, 0)
    p = 14
    px = []
    # The palettes are blue first: checked against the retail .xtex of the
    # same names (Red's red_tx_a: skin orange, bandana red, as retail; read
    # as RGBA they came out blue).
    if fmt in (1, 14):
        pal = [bgra(d[p + 4 * i:p + 4 * i + 4]) for i in range(256)]
        p += 1024
        px = [pal[i] for i in d[p:p + w * h]]
    elif fmt in (15, 16):
        pal = [bgra(d[p + 4 * i:p + 4 * i + 4]) for i in range(16)]
        p += 64
        for b in d[p:p + w * h // 2]:
            px.append(pal[b & 15]); px.append(pal[b >> 4])
    elif fmt == 17:
        px = [tuple(d[p + 3 * i:p + 3 * i + 3]) + (0x80,) for i in range(w * h)]
    elif fmt == 18:
        px = [tuple(d[p + 4 * i:p + 4 * i + 4]) for i in range(w * h)]
    elif fmt == 6:
        for i in range(w * h):
            v = struct.unpack_from('<H', d, p + 2 * i)[0]
            px.append(((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3, 0x80 if v & 0x8000 else 0))
    else:
        raise ValueError('format %d' % fmt)
    amax = max(a for r, g, b, a in px) if px else 255
    if amax <= 0x80:
        px = [(r, g, b, min(255, a * 2)) for r, g, b, a in px]
    return w, h, px


def main():
    a = sys.argv[1:]
    solid = bool(a) and a[0] == '--solid'
    if solid:
        a = a[1:]
    png = bool(a) and a[0] == '--png'
    if png:
        a = a[1:]
    if len(a) != 2:
        print(__doc__); sys.exit(1)
    w, h, px = decode(open(a[0], 'rb').read())
    if solid and px and min(p[3] for p in px) >= 0x80:
        # A character's skin with no holes in it: the varying alpha some
        # preview textures carry (O'Grady's: 189-255) drew the character
        # see-through; the retail characters are opaque.
        px = [(r, g, b, 255) for r, g, b, al in px]
    if png:
        from PIL import Image
        im = Image.new('RGBA', (w, h)); im.putdata(px); im.save(a[1]); return
    out = bytearray(struct.pack('<7H', w, h, 0x12, 1, 1, 0, 0))
    # Red first: the game reads format 0x12 as R, G, B, A in memory
    # (checked in a match: a texture written blue-first as pure red
    # was drawn pure blue).
    for r, g, b, al in px:
        out += bytes((r, g, b, al))
    open(a[1], 'wb').write(out)


if __name__ == '__main__':
    main()
