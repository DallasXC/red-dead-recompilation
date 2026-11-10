#!/usr/bin/env python3
"""
beta_look.py - the January 2002 preview's title picture for the 2002 look.

The preview's main menu stood on one picture, logo.tex: red, "RED DEAD
REVOLVER" in black at the left and a gunman's dark shape at the right.
The retail disc has none of it (its logo.xtex is another, older menu), so
the 2002 look's title (src/ui/rdr_hud2002.cpp) takes it from the player's
own preview disc: written here as a plain 32-bit .tga, top row first,
right way up (the preview stores its pictures upside down).

  python tools\\mods\\beta_look.py <preview .iso or SWAT.ZIP> <out folder>

Writes <out folder>\\2002look\\title2002.tga. The setup (setup_preview.py)
runs it into Mods\\Preview.
"""
import os, shutil, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import beta, beta_level, betatex


def member(swat, name):
    for n, do, size, packed in beta.entries(swat):
        if n.lower() == name:
            with open(swat, 'rb') as f:
                return beta.read(f, do, size, packed)
    return None


def write_tga(path, w, h, px):
    out = bytearray(struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x28))
    for r, g, b, a in px:
        out += bytes((b, g, r, a))
    with open(path, 'wb') as f:
        f.write(out)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    work = tempfile.mkdtemp(prefix="beta_look_")
    try:
        swat = beta_level.open_preview(src, work)
        d = member(swat, "logo.tex")
        if not d:
            sys.exit("the preview has no logo.tex")
        w, h, px = betatex.decode(d)
        rows = [px[y * w:(y + 1) * w] for y in range(h)]
        px = [p for row in reversed(rows) for p in row]       # right way up
        px = [(r, g, b, 255) for r, g, b, a in px]            # a picture: opaque
        os.makedirs(os.path.join(out, "2002look"), exist_ok=True)
        dst = os.path.join(out, "2002look", "title2002.tga")
        write_tga(dst, w, h, px)
        print("title: %s (%dx%d)" % (dst, w, h))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
