#!/usr/bin/env python3
"""
xtex.py - Red Dead Revolver textures (.xtex) to DDS and back.

An .xtex is a 14-byte header -- u16 width, height, format, the most mip
levels it may have (9 = the full chain), and three u16 flags kept as they
are -- then the levels, largest first, each exactly width*height*bpp/8
bytes (no padding of the tiny levels). The block-compressed formats are
stored linearly, as DDS stores them:

  0x16  DXT1   (most textures)     0x18  DXT3 (rare)  0x1A  DXT5
  (others: 0x0E 8-bit and 0x10 4-bit palettes, 0x11 RGB, 0x12 ARGB, ...
   -- not converted by this tool)

Usage:
  xtex.py info   <file.xtex>
  xtex.py export <file.xtex> <file.dds>        (open it in GIMP, Paint.NET...)
  xtex.py import <file.dds> <original.xtex> <new.xtex>
      Same DXT format as the original; the size may change (powers of two).
      Mip levels are taken from the DDS -- if the original has them, save
      the DDS with mipmaps too.

With dave.py: extract an archive, convert the textures you want, edit,
import, rebuild the archive into the mods folder.
"""
import struct, sys

FMT = {0x16: b"DXT1", 0x18: b"DXT3", 0x1A: b"DXT5"}
BLOCK = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16}
BPP = {b"DXT1": 4, b"DXT3": 8, b"DXT5": 8}


def dds_level(w, h, four):
    """A level's size in a DDS: whole 4x4 blocks."""
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * BLOCK[four]


def xtex_level(w, h, four):
    """A level's size in an .xtex: the bits it needs, no block padding
    (a 2x2 DXT1 level is 2 bytes, 1x1 none)."""
    return w * h * BPP[four] // 8


def levels(w, h, four, cap, size):
    """The levels an .xtex holds: largest first, until its data runs out
    (field 4 of the header is the most it may have)."""
    out, off = [], 0
    while len(out) < cap and off < size:
        n = xtex_level(w, h, four)
        out.append((w, h, off, n))
        off += n
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    return out


def read_xtex(path):
    d = open(path, "rb").read()
    w, h, fmt, mips, f1, f2, f3 = struct.unpack_from("<7H", d, 0)
    return d, w, h, fmt, max(1, mips), (f1, f2, f3)


def cmd_info(path):
    d, w, h, fmt, cap, flags = read_xtex(path)
    four = FMT.get(fmt)
    n = len(levels(w, h, four, cap, len(d) - 14)) if four else "?"
    print(f"{path}: {w}x{h} format 0x{fmt:02X} ({four.decode() if four else 'not DXT'}) "
          f"levels {n} (cap {cap}) flags {flags} data {len(d) - 14} bytes")


def cmd_export(src, dst):
    d, w, h, fmt, cap, _ = read_xtex(src)
    if fmt not in FMT:
        sys.exit(f"{src}: format 0x{fmt:02X} is not one this tool converts")
    four = FMT[fmt]
    lv = levels(w, h, four, cap, len(d) - 14)
    body = b""
    for lw, lh, off, n in lv:
        b = d[14 + off:14 + off + n]
        body += b + b"\0" * (dds_level(lw, lh, four) - len(b))
    mips = len(lv)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000 | (0x20000 if mips > 1 else 0)
    caps = 0x1000 | ((0x8 | 0x400000) if mips > 1 else 0)
    hdr = struct.pack("<4s7I", b"DDS ", 124, flags, h, w, dds_level(w, h, four), 0, mips)
    hdr += b"\0" * 44
    hdr += struct.pack("<2I4s5I", 32, 0x4, four, 0, 0, 0, 0, 0)
    hdr += struct.pack("<4I", caps, 0, 0, 0) + b"\0" * 4
    open(dst, "wb").write(hdr + body)
    print(f"wrote {dst}: {w}x{h} {four.decode()} levels {mips}")


def cmd_import(dds, orig, dst):
    od, ow, oh, fmt, cap, flags = read_xtex(orig)
    if fmt not in FMT:
        sys.exit(f"{orig}: format 0x{fmt:02X} is not one this tool converts")
    d = open(dds, "rb").read()
    if d[:4] != b"DDS ":
        sys.exit(f"{dds}: not a DDS file")
    h, w = struct.unpack_from("<2I", d, 12)
    mips = struct.unpack_from("<I", d, 28)[0] or 1
    four = d[84:88]
    if four != FMT[fmt]:
        sys.exit(f"{dds}: {four.decode(errors='replace')}, but the original is {FMT[fmt].decode()} "
                 f"-- save it as {FMT[fmt].decode()}")
    if w & (w - 1) or h & (h - 1):
        sys.exit(f"{dds}: {w}x{h} -- the sides must be powers of two")
    old = len(levels(ow, oh, four, cap, len(od) - 14))
    if old > 1 and mips == 1:
        print(f"note: the original has {old} mip levels and this DDS none -- "
              f"it will shimmer at a distance; save with mipmaps")
    body, off, lw, lh = b"", 128, w, h
    for _ in range(mips):
        n = dds_level(lw, lh, four)
        body += d[off:off + xtex_level(lw, lh, four)]
        off += n
        lw, lh = max(1, lw // 2), max(1, lh // 2)
    # Same number of levels: the original's header as it was; otherwise
    # the count itself.
    new_cap = cap if mips == old else mips
    out = struct.pack("<7H", w, h, fmt, new_cap, *flags) + body
    open(dst, "wb").write(out)
    print(f"wrote {dst}: {w}x{h} {four.decode()} levels {mips}")


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "info":
        cmd_info(a[1])
    elif len(a) == 3 and a[0] == "export":
        cmd_export(a[1], a[2])
    elif len(a) == 4 and a[0] == "import":
        cmd_import(a[1], a[2], a[3])
    else:
        print(__doc__)
        sys.exit(1)
