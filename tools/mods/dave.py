#!/usr/bin/env python3
"""
dave.py - list, extract and rebuild Red Dead Revolver's "Dave" archives
(swat.dat, front.dat, the level .dat files, ...).

Format, as the game reads it (sub_000D5340 opens one, sub_000D4250 compares
names, sub_000D4330 unpacks them):

  0x000  "Dave"  u32 count  u32 dir_bytes  u32 name_bytes
  0x800  count entries of { u32 name_offset, u32 data_offset,
                            u32 size, u32 packed_size }
         (name_offset relative to the name table)
  0x800 + dir_bytes   the name table: names packed 6 bits a character,
         4 characters in 3 bytes, low bits first, from a 64-character
         alphabet (" #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~"
         after a 0 terminator code); a name ends at code 0. A name whose
         first code is 56 or more shares a prefix with the entry before it:
         length (code0 - 56) + 8 * (code1 & 31) (code1 has bit 5 set, so it is
         never the 0 terminator), the rest from code 2
         (sub_000D4540).
  data   raw DEFLATE (no zlib header) when packed_size < size, else stored.
         Entries are sorted by name (the game binary-searches them).

Usage:
  dave.py list    <archive.dat>
  dave.py extract <archive.dat> <out_dir> [pattern]
  dave.py merge   <archive.dat> <out.dat> <in_dir>
      A copy of the archive with the files under in_dir added (or
      replacing ones of the same name); the rest are copied as they are,
      so it is quick even for swat.dat.
  dave.py build   <in_dir> <archive.dat> [original.dat]
      Every file under in_dir. Give the original archive too and the files
      it keeps uncompressed stay uncompressed (recommended).

A "?" in a name is written "%3f" in the extracted file's name (Windows
cannot hold "?"); build turns it back.

A rebuilt archive goes in the mods folder (see docs/MODDING.md) under the
same path as the original -- e.g. mods/swat.dat -- and the game reads it
instead of the disc's.
"""
import fnmatch, os, struct, sys, zlib

ALPHA = "\0 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~"
CODE = {c: i for i, c in enumerate(ALPHA)}


def codes_at(tab, off):
    out = []
    while True:
        b0, b1, b2 = tab[off], tab[off + 1], tab[off + 2]
        off += 3
        for c in (b0 & 0x3F, (b0 >> 6) | ((b1 & 0x0F) << 2),
                  (b1 >> 4) | ((b2 & 0x03) << 4), b2 >> 2):
            out.append(c)
            if c == 0:
                return out


def text(codes):
    return "".join(ALPHA[c] if c < len(ALPHA) else "?" for c in codes if c)


def unpack_names(tab, offsets):
    names, prev = [], ""
    for off in offsets:
        c = codes_at(tab, off)
        if c[0] >= 56:
            n = (c[0] - 56) + 8 * (c[1] & 31)
            name = prev[:n] + text(c[2:])
        else:
            name = text(c)
        names.append(name)
        prev = name
    return names


def pack_codes(codes):
    codes = list(codes) + [0]
    while len(codes) % 4:
        codes.append(0)
    out = bytearray()
    for i in range(0, len(codes), 4):
        a, b, c, d = codes[i:i + 4]
        out += bytes([(a | (b << 6)) & 0xFF, ((b >> 2) | (c << 4)) & 0xFF, ((c >> 4) | (d << 2)) & 0xFF])
    return bytes(out)


# The game unpacks a shared prefix by recursing into the entry before
# (sub_000D4540, 24 bytes of stack a level, under a 256-byte buffer), so a
# run of shared names is a run of nested calls. The game's own archives
# write a whole name at least every 32 entries (runs of 31 at most); ours
# used to share whenever they could, and a merged swat.dat had runs of
# nearly 6000 -- some 140 KB of stack for one lookup, more than a game
# thread other than the main one has: the lookup ran off its stack and
# crashed (the preview characters' Bat Boy taking off).
MAX_RUN = 31


def pack_names(names):
    """The name table for names (sorted), and each name's offset in it."""
    tab, offs, prev, run = bytearray(), [], "", 0
    for name in names:
        offs.append(len(tab))
        n = 0
        while n < min(len(name), len(prev), 255) and name[n] == prev[n]:
            n += 1
        if n >= 3 and run < MAX_RUN:
            tab += pack_codes([56 + (n & 7), 0x20 | ((n >> 3) & 31)] + [CODE[ch] for ch in name[n:]])
            run += 1
        else:
            tab += pack_codes([CODE[ch] for ch in name])
            run = 0
        prev = name
    return tab, offs


def longest_run(tab, offsets):
    run = best = 0
    for off in offsets:
        run = run + 1 if codes_at(tab, off)[0] >= 56 else 0
        best = max(best, run)
    return best


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, count, dirb, nameb = struct.unpack_from("<4sIII", data, 0)
    if magic != b"Dave":
        sys.exit(f"{path}: not a Dave archive (magic {magic!r})")
    names = data[0x800 + dirb:0x800 + dirb + nameb]
    raw = [struct.unpack_from("<IIII", data, 0x800 + i * 16) for i in range(count)]
    decoded = unpack_names(names, [r[0] for r in raw])
    entries = [(decoded[i], r[1], r[2], r[3]) for i, r in enumerate(raw)]
    return data, entries


def names(path):
    """The names in an archive, reading only its directory."""
    with open(path, "rb") as f:
        head = f.read(16)
        magic, count, dirb, nameb = struct.unpack_from("<4sIII", head, 0)
        if magic != b"Dave":
            return []
        f.seek(0x800)
        d = f.read(dirb + nameb)
    raw = [struct.unpack_from("<IIII", d, i * 16) for i in range(count)]
    return unpack_names(d[dirb:dirb + nameb], [r[0] for r in raw])


def payload(data, do, size, packed):
    blob = data[do:do + packed]
    return zlib.decompress(blob, -15) if packed < size else blob


# "?" is a name character the archives use but Windows file names cannot
# hold: on disk it is written "%3f" ("%" is not in the alphabet).
def to_file(name):
    return name.replace("?", "%3f")


def safe_dest(out, name):
    """Where an archive entry goes under out, or None for a name that would
    land outside it (.., an absolute path, a drive): a damaged or made-up
    archive cannot write elsewhere."""
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
    if not parts or any(p == ".." or ":" in p for p in parts) or name[:1] in ("/", "\\"):
        return None
    root = os.path.abspath(out)
    dst = os.path.abspath(os.path.join(root, *parts))
    if os.path.commonpath([root, dst]) != root:
        return None
    return dst


def from_file(rel):
    return rel.replace("%3f", "?")


def cmd_list(path):
    _, entries = read(path)
    for name, do, size, packed in entries:
        print(f"{size:10d} {packed:10d}  {name}")
    print(f"{len(entries)} files")


def cmd_extract(path, out, pattern="*"):
    data, entries = read(path)
    n = 0
    for name, do, size, packed in entries:
        if not fnmatch.fnmatch(name, pattern):
            continue
        dst = safe_dest(out, to_file(name))
        if dst is None:
            print(f"skipped {name!r}: not a name inside the archive's folder", file=sys.stderr)
            continue
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        with open(dst, "wb") as f:
            f.write(payload(data, do, size, packed))
        n += 1
    print(f"extracted {n} files to {out}")


# Files the original archives keep uncompressed although they would pack
# (read in place or streamed): kept that way when no original is given.
STORE = {".iani"}


def cmd_build(src, path, like=None):
    raw_in_original = set()
    if like:
        _, entries = read(like)
        raw_in_original = {name for name, _, size, packed in entries if packed >= size}
    files = []
    for root, _, names in os.walk(src):
        for fn in names:
            full = os.path.join(root, fn)
            rel = from_file(os.path.relpath(full, src).replace(os.sep, "/").lower())
            bad = [c for c in rel if c not in CODE]
            if bad:
                sys.exit(f"{rel}: characters the archive cannot hold: {''.join(set(bad))}")
            files.append((rel, full))
    files.sort(key=lambda t: [CODE[c] for c in t[0]])
    nametab, offs = pack_names([rel for rel, _ in files])
    dirb = (len(files) * 16 + 0x7FF) & ~0x7FF
    nameb = (len(nametab) + 0x7FF) & ~0x7FF
    out = bytearray(0x800 + dirb + nameb)
    struct.pack_into("<4sIII", out, 0, b"Dave", len(files), dirb, nameb)
    out[0x800 + dirb:0x800 + dirb + len(nametab)] = nametab
    for i, (rel, full) in enumerate(files):
        raw = open(full, "rb").read()
        if rel in raw_in_original or (not like and os.path.splitext(rel)[1] in STORE):
            packed = raw
        else:
            comp = zlib.compressobj(9, zlib.DEFLATED, -15)
            packed = comp.compress(raw) + comp.flush()
            if len(packed) >= len(raw):
                packed = raw
        do = len(out)
        out += packed
        out += bytes((-len(out)) & 0x7FF)      # 2 KB aligned, as the originals
        struct.pack_into("<IIII", out, 0x800 + i * 16, offs[i], do, len(raw), len(packed))
    with open(path, "wb") as f:
        f.write(out)
    print(f"wrote {path}: {len(files)} files")


def merge(path, out_path, files):
    """A copy of archive `path` with `files` ({name: bytes}) added or
    replaced (None: removed). The other entries are copied as they are
    packed (no recompression), so even swat.dat (358 MB) takes seconds."""
    data, entries = read(path)
    keep = {name: (do, size, packed) for name, do, size, packed in entries}
    raw_original = {name for name, _, size, packed in entries if packed >= size}
    new = {}
    for name, raw in files.items():
        name = name.lower()
        if raw is None:
            keep.pop(name, None)
            continue
        bad = [c for c in name if c not in CODE]
        if bad:
            sys.exit(f"{name}: characters the archive cannot hold: {''.join(set(bad))}")
        if name in raw_original:
            packed = raw
        else:
            comp = zlib.compressobj(9, zlib.DEFLATED, -15)
            packed = comp.compress(raw) + comp.flush()
            if len(packed) >= len(raw):
                packed = raw
        new[name] = (raw, packed)
        keep.pop(name, None)
    names = sorted(list(keep) + list(new), key=lambda n: [CODE[c] for c in n])
    nametab, offs = pack_names(names)
    dirb = (len(names) * 16 + 0x7FF) & ~0x7FF
    nameb = (len(nametab) + 0x7FF) & ~0x7FF
    head = bytearray(0x800 + dirb + nameb)
    struct.pack_into("<4sIII", head, 0, b"Dave", len(names), dirb, nameb)
    head[0x800 + dirb:0x800 + dirb + len(nametab)] = nametab
    pos = len(head)
    with open(out_path, "wb") as f:
        f.write(head)
        for i, n in enumerate(names):
            if n in new:
                raw, blob = new[n]
                size = len(raw)
            else:
                do, size, packed = keep[n]
                blob = data[do:do + packed]
            f.write(blob)
            pad = (-(pos + len(blob))) & 0x7FF
            f.write(bytes(pad))
            struct.pack_into("<IIII", head, 0x800 + i * 16, offs[i], pos, size, len(blob))
            pos += len(blob) + pad
        f.seek(0)
        f.write(head)
    return len(names)


def cmd_merge(path, out_path, src):
    files = {}
    for root, _, fns in os.walk(src):
        for fn in fns:
            full = os.path.join(root, fn)
            rel = from_file(os.path.relpath(full, src).replace(os.sep, "/").lower())
            files[rel] = open(full, "rb").read()
    n = merge(path, out_path, files)
    print(f"wrote {out_path}: {n} files ({len(files)} added or replaced)")


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "list":
        cmd_list(a[1])
    elif len(a) >= 3 and a[0] == "extract":
        cmd_extract(a[1], a[2], a[3] if len(a) > 3 else "*")
    elif len(a) == 4 and a[0] == "merge":
        cmd_merge(a[1], a[2], a[3])
    elif len(a) in (3, 4) and a[0] == "build":
        cmd_build(a[1], a[2], a[3] if len(a) > 3 else None)
    else:
        print(__doc__)
        sys.exit(1)
