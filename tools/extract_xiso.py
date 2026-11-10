#!/usr/bin/env python3
"""extract_xiso.py - unpack an Xbox disc image into a folder (for build.bat).

    python tools/extract_xiso.py <image.iso> <dest folder> [--check-title 5454001A]

The same reader as the game's own installer (src/iso_install.c): an Xbox
disc's game files are an XDVDFS partition, whose volume descriptor
("MICROSOFT*XBOX*MEDIA" at 0 and 0x7EC of sector 32) gives the root
directory; a directory is a binary tree of entries (u16 left, u16 right in
4-byte units, u32 sector, u32 size, u8 attributes (0x10 directory), u8 name
length, the name). The partition is at 0 in an "xiso" (extract-xiso), or
after the video partition of a full disc dump (Redump): 0x18300000 (XGD2),
0x0FD90000 (XGD1), 0x02080000 (XGD3).

With --check-title, the image's default.xbe must carry that title ID (Red
Dead Revolver: 5454001A) before anything is written. Exit status 0 on
success, 2 when the image is not that game, 1 on any other failure.
"""
import os
import struct
import sys

SECT = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)


class Image:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, os.SEEK_END)
        self.size = self.f.tell()
        self.base = None
        for off in OFFSETS:
            v = self.read(off + 32 * SECT, SECT)
            if v and v[:20] == MAGIC and v[0x7EC:0x7EC + 20] == MAGIC:
                self.base = off
                self.root = struct.unpack_from("<II", v, 20)
                break

    def read(self, at, n):
        if at + n > self.size:
            return None
        self.f.seek(at)
        d = self.f.read(n)
        return d if len(d) == n else None

    def entries(self, sector, size):
        """(name, is_dir, sector, size) of a directory's entries."""
        if size == 0:
            return []
        if size > 64 << 20:
            raise ValueError("a directory too large")
        d = self.read(self.base + sector * SECT, size)
        if d is None:
            raise ValueError("a directory past the end of the image")
        out, stack, seen = [], [0], set()
        while stack:
            off = stack.pop() * 4
            if off in seen or off + 14 > size:
                continue
            seen.add(off)
            left, right, fs, fz, attr, nl = struct.unpack_from("<HHIIBB", d, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue                        # sector padding
            if off + 14 + nl > size:
                continue
            name = d[off + 14:off + 14 + nl].decode("latin-1")
            if left:
                stack.append(left)
            if right:
                stack.append(right)
            if not name or name in (".", "..") or any(c in name for c in "/\\:") \
                    or any(ord(c) < 32 for c in name):
                print("  skipped a bad name on the disc", file=sys.stderr)
                continue
            out.append((name, bool(attr & 0x10), fs, fz))
        return out

    def find(self, name):
        for n, is_dir, fs, fz in self.entries(*self.root):
            if n.lower() == name.lower() and not is_dir:
                return fs, fz
        return None


def title_id(img):
    """The title ID in the image's default.xbe, or None."""
    e = img.find("default.xbe")
    if not e:
        return None
    fs, fz = e
    head = img.read(img.base + fs * SECT, min(fz, 0x1000))
    if not head or head[:4] != b"XBEH":
        return None
    base, = struct.unpack_from("<I", head, 0x104)
    cert, = struct.unpack_from("<I", head, 0x118)
    c = cert - base
    if c < 0 or c + 12 > fz:
        return None
    cd = img.read(img.base + fs * SECT + c, 12)
    return struct.unpack_from("<I", cd, 8)[0] if cd else None


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    want = None
    if "--check-title" in argv:
        i = argv.index("--check-title")
        want = int(argv[i + 1], 16) if i + 1 < len(argv) else None
        args = [a for a in args if a != argv[i + 1]]
    if len(args) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 1
    src, dest = args
    try:
        img = Image(src)
    except OSError as e:
        print(f"Cannot open {src}: {e}", file=sys.stderr)
        return 1
    if img.base is None:
        print(f"{src} is not an Xbox disc image (no XDVDFS volume found).", file=sys.stderr)
        return 2
    if want is not None:
        t = title_id(img)
        if t != want:
            got = f"{t:08X}" if t is not None else "none"
            print(f"{src} is not Red Dead Revolver (USA): its title ID is {got}, "
                  f"not {want:08X}.", file=sys.stderr)
            return 2

    # Sizes first, for the progress; then the files.
    total, todo = 0, [(img.root, "")]
    files, walked = [], set()
    while todo:
        (sec, size), rel = todo.pop()
        if sec in walked:
            # a damaged image's folder pointing back at one already walked
            print(f"  skipped a folder the disc lists twice ({rel})", file=sys.stderr)
            continue
        walked.add(sec)
        for name, is_dir, fs, fz in img.entries(sec, size):
            r = os.path.join(rel, name)
            if is_dir:
                files.append((r, True, 0, 0))
                todo.append(((fs, fz), r))
            else:
                files.append((r, False, fs, fz))
                total += fz
    print(f"Unpacking {total / 1048576:.1f} MB from {src}\n  into {dest}")
    os.makedirs(dest, exist_ok=True)
    done, last, n = 0, -1, 0
    for rel, is_dir, fs, fz in files:
        path = os.path.join(dest, rel)
        if is_dir:
            os.makedirs(path, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        img.f.seek(img.base + fs * SECT)
        left = fz
        with open(path, "wb") as o:
            while left:
                chunk = img.f.read(min(left, 1 << 20))
                if not chunk:
                    print(f"\n  the image ends inside {rel}", file=sys.stderr)
                    return 1
                o.write(chunk)
                left -= len(chunk)
                done += len(chunk)
                pc = done * 100 // total if total else 100
                if pc != last:
                    last = pc
                    print(f"\r  {pc:3d}%", end="", flush=True)
        n += 1
    print(f"\nDone: {n} files.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
