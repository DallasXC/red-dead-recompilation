#!/usr/bin/env python3
"""
strtbl.py - read Red Dead Revolver's string tables (.strtbl, in front.dat
and swat.dat; extract them with dave.py).

Format:
  u32 languages, then u32 offset per language (several may share one, or
  point at the end of the file: none)
  the key names: u32 length, text, a 0 byte -- each
  at each language's offset: u32 count, then records
    { u32 ELF hash of the key, u16, u32 length + font name,
      u32 length + UTF-16 text (with its 0), f32, f32, u16 }

Usage:
  strtbl.py dump <file.strtbl>            every key and its (first language's) text
  strtbl.py get  <file.strtbl> <key>...   the text of these keys
"""
import struct, sys


def elf_hash(s):
    h = 0
    for c in s.encode("latin-1"):
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    return h


def parse(path):
    b = open(path, "rb").read()
    nl = struct.unpack_from("<I", b, 0)[0]
    offs = struct.unpack_from("<%dI" % nl, b, 4)
    first = min(o for o in offs)
    # the key list ends where the first language begins; it starts after a
    # short header we skip by finding the first plausible key entry
    keys = []
    o = 4 + 4 * nl
    while o < first:
        n = struct.unpack_from("<I", b, o)[0]
        if 0 < n < 200 and o + 4 + n < first and b[o + 4 + n] == 0 \
                and all(32 <= c < 127 or c >= 160 for c in b[o + 4:o + 4 + n]):
            break
        o += 1
    while o + 4 <= first:
        n = struct.unpack_from("<I", b, o)[0]
        if n > 200 or o + 4 + n >= len(b):
            break
        keys.append(b[o + 4:o + 4 + n].decode("latin-1"))
        o += 4 + n + 1
    langs = []
    for off in sorted(set(x for x in offs if x + 4 <= len(b))):
        p = off
        n = struct.unpack_from("<I", b, p)[0]
        p += 4
        recs = {}
        for _ in range(n):
            h, _flag = struct.unpack_from("<IH", b, p)
            p += 6
            ln = struct.unpack_from("<I", b, p)[0]
            p += 4 + ln
            ln = struct.unpack_from("<I", b, p)[0]
            txt = b[p + 4:p + 4 + 2 * ln].decode("utf-16-le").rstrip("\0")
            p += 4 + 2 * ln + 10
            recs[h] = txt
        langs.append(recs)
    return keys, langs


def add(data, entries, font=b"regulatorcondensed"):
    """The table with these keys set (key -> text): a key already there gets
    the new text, others are added (to the key list and the first
    language's records). Text in the other languages is not touched."""
    b = bytes(data)
    nl = struct.unpack_from("<I", b, 0)[0]
    offs = list(struct.unpack_from("<%dI" % nl, b, 4))
    first = offs[0]
    hdr = 4 + 4 * nl
    unk, nkeys = struct.unpack_from("<II", b, hdr)
    o = hdr + 8
    keys = []
    for _ in range(nkeys):
        n = struct.unpack_from("<I", b, o)[0]
        keys.append(b[o + 4:o + 4 + n].decode("latin-1"))
        o += 4 + n + 1
    if o != first:
        raise ValueError("unexpected key list layout")
    p = first
    count = struct.unpack_from("<I", b, p)[0]
    p += 4
    recs = []
    for _ in range(count):
        st = p
        p += 6
        p += 4 + struct.unpack_from("<I", b, p)[0]
        p += 4 + 2 * struct.unpack_from("<I", b, p)[0] + 10
        recs.append(b[st:p])
    rest = b[p:]
    have = {struct.unpack_from("<I", r, 0)[0]: i for i, r in enumerate(recs)}
    for k, text in entries.items():
        t = (text + "\0").encode("utf-16-le")
        rec = (struct.pack("<IH", elf_hash(k), 1) + struct.pack("<I", len(font)) + font
               + struct.pack("<I", len(t) // 2) + t + struct.pack("<ff", 0.8, 0.8) + b"\0\0")
        h = elf_hash(k)
        if h in have:
            recs[have[h]] = rec
        else:
            recs.append(rec)
            have[h] = len(recs) - 1
            if k not in keys:
                keys.append(k)
    keyblob = b"".join(struct.pack("<I", len(k)) + k.encode("latin-1") + b"\0" for k in keys)
    lang0 = struct.pack("<I", len(recs)) + b"".join(recs)
    newfirst = hdr + 8 + len(keyblob)
    end = newfirst + len(lang0) + len(rest)
    newoffs = []
    for x in offs:
        if x == first:
            newoffs.append(newfirst)
        elif x >= len(b):
            newoffs.append(end)
        else:
            raise ValueError("more than one language with text")
    return (struct.pack("<I", nl) + struct.pack("<%dI" % nl, *newoffs)
            + struct.pack("<II", unk, len(keys)) + keyblob + lang0 + rest)


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] in ("dump", "get"):
        keys, langs = parse(a[1])
        recs = langs[0] if langs else {}
        for k in (keys if a[0] == "dump" else a[2:]):
            print("%s\t%s" % (k, recs.get(elf_hash(k), "").replace("\n", "\\n")))
    else:
        print(__doc__)
        sys.exit(1)
