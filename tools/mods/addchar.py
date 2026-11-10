#!/usr/bin/env python3
"""
addchar.py - add a Showdown character (a copy of an existing one to start
from), instead of replacing one. EXPERIMENTAL.

  python tools\\mods\\addchar.py <game folder> <mods folder> <code> <copy of> [x]
  python tools\\mods\\addchar.py place <game folder> <mods folder> <code> <x>

  code      the new character's code, 3 letters (a-z), not one the game uses
  copy of   the character to copy (srd = Red, ncb = Bad Bear Billy, ...)
  x         where on the line-up its picture goes. The first page of the
            photo is 0-1023 and the 48 retail characters fill it; 1024-2047
            is the second page, 2048-3071 the third, and so on (the
            recompiled game turns the page when the hand is held against
            the photo's edge). Default: the next free place on the pages
            after the first, 14 to a page.
  place     moves a character already added to another x.

What a Showdown character is (docs/MODDING.md, "Adding characters"):
  front.dat  postermultiview.swui  <FONTGRIDDATA:y:<code>:0>  (the roster)
             characterlineup.swui  <POSMARKER:...:hotspot_<code>>
             ui_mphoto_<code>.xtex (its cut-out on the line-up photo)
             mp_<code>.xtex        (its portrait in the player box)
  swat.dat   m<code>.cr            (Showdown's: names its model, skeleton,
                                   animations), m<code>.swt ("swat type"),
                                   .dam, .msd, .rep, .sphs, ...; and the
                                   story set <code>.* -- copied from the
                                   character copied
             ("Failed to load swat type" when they are missing)
             unlockables.csv       m<code>,character,...  (without a row
                                   the line-up treats it as locked)
Weapons, special move and bot brain fall back to the tables' "default"
rows. Writes mods\\front.dat and mods\\swat.dat (a full copy of swat.dat,
~360 MB; dave.py merge, so it takes seconds).
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dave


def src_archive(game, mods, name):
    m = os.path.join(mods, name)
    return m if os.path.exists(m) else os.path.join(game, name)


def get(archive, name):
    data, entries = dave.read(archive)
    for n, do, size, packed in entries:
        if n == name:
            return dave.payload(data, do, size, packed)
    return None


PAGE_W, SLOT_W, SLOT_X0, SLOTS = 1024, 68, 20, 14


def markers(lineup):
    return {c.decode(): int(x) for x, c in
            re.findall(rb"<POSMARKER:(-?\d+),0,0:\d+,\d+:hotspot_(\w+)>", lineup)}


def next_slot(lineup):
    """The first free place after the first page: 14 to a page, 68 apart."""
    used = set(markers(lineup).values())
    n = 0
    while True:
        x = PAGE_W * (1 + n // SLOTS) + SLOT_X0 + SLOT_W * (n % SLOTS)
        if x not in used:
            return x
        n += 1


def place(game, mods, code, x):
    code = code.lower()
    front = src_archive(game, mods, "front.dat")
    lineup = get(front, "characterlineup.swui")
    new, k = re.subn(rb"<POSMARKER:-?\d+(,0,0:\d+,\d+:hotspot_%s>)" % code.encode(),
                     b"<POSMARKER:%d\\1" % int(x), lineup)
    if not k:
        sys.exit("no hotspot_%s in characterlineup.swui" % code)
    os.makedirs(mods, exist_ok=True)
    out = os.path.join(mods, "front.dat")
    dave.merge(front, out + ".new", {"characterlineup.swui": new})
    os.replace(out + ".new", out)
    print("%s now at x=%d on the line-up (page %d)" % (code, int(x), int(x) // PAGE_W + 1))


def add(game, mods, code, src, x=None, extra=None, cr_edit=None):
    """Add `code` as a copy of `src`. extra: more swat.dat files
    {name: bytes}; cr_edit: bytes -> bytes for m<code>.cr (and <code>.cr)."""
    add_many(game, mods, [dict(code=code, src=src, x=x, extra=extra, cr_edit=cr_edit)])


def add_many(game, mods, specs, front_extra=None, front_edit=None, swat_edit=None):
    """Add several characters with one rewrite of front.dat and swat.dat.
    Each spec: code, src (the character copied), x (None: next free place),
    extra ({swat.dat name: bytes}), cr_edit (bytes -> bytes), edit
    ((name, bytes) -> bytes, every swat.dat file of the copy), front
    ({front.dat name: bytes}: its own pictures, ui_mphoto_/mp_<code>).
    front_edit / swat_edit: {name: bytes} -> None, last changes to the files
    about to be written (string tables)."""
    front, swat = src_archive(game, mods, "front.dat"), src_archive(game, mods, "swat.dat")
    pmv = get(front, "postermultiview.swui")
    lineup = get(front, "characterlineup.swui")
    unl = get(swat, "unlockables.csv")
    data, entries = dave.read(swat)
    names = set(n for n, _, _, _ in entries)
    ffiles, files = {}, {}
    done = []
    # the new markers, put in after the loop: back row first -- the photo
    # is built in the list's order, so a later picture is drawn over an
    # earlier one (spec "row": 0 at the back; "hot": the hand's point)
    markers_new = []
    # and the roster's new lines, back row first too: the photo is built
    # in the roster's order (the retail roster and markers are both back
    # row first; with only the markers so, the back row's tall figures
    # stood over the middle row's -- user screenshots)
    roster_new = []
    for sp in specs:
        code, src = sp["code"].lower(), sp["src"].lower()
        if not re.fullmatch(r"[a-z]{3}", code):
            sys.exit("code: 3 letters (%s)" % code)
        if b"<FONTGRIDDATA:y:%s:" % code.encode() in pmv + b"".join(l for _, l in roster_new):
            sys.exit("%s is already in the roster" % code)
        if b"<FONTGRIDDATA:y:%s:" % src.encode() not in pmv:
            sys.exit("%s is not in the roster" % src)
        if code + ".cr" in names or "m" + code + ".cr" in names:
            sys.exit("the game already has %s.cr; pick another code" % code)
        m = re.search(rb"<POSMARKER:(-?\d+),0,0:(\d+),(\d+):hotspot_%s>" % src.encode(), lineup)
        if not m:
            sys.exit("no hotspot for %s in characterlineup.swui" % src)
        x = (next_slot(lineup + b"".join(line for _, line in markers_new)) if sp.get("x") is None
             else int(sp["x"]))
        row = re.search(rb"^m%s,character,[^\r\n]*" % src.encode(), unl, re.M)
        if not row:
            sys.exit("no unlockables row for m%s" % src)
        roster_new.append((sp.get("row", 0), b"<FONTGRIDDATA:y:%s:0>\r\n" % code.encode()))
        hw, hh = sp.get("hot") or (int(m.group(2)), int(m.group(3)))
        markers_new.append((sp.get("row", 0), b"<POSMARKER:%d,0,0:%d,%d:hotspot_%s>\r\n" % (
            x, int(hw), int(hh), code.encode())))
        own = sp.get("front") or {}
        for t in ("ui_mphoto_%s.xtex", "mp_%s.xtex"):
            if t % code in own:
                ffiles[t % code] = own[t % code]
                continue
            blob = get(front, t % src)
            if blob is None:
                sys.exit("front.dat has no " + t % src)
            ffiles[t % code] = blob
        mine = {}
        for n, do, size, packed in entries:
            for pre in ("m", ""):
                if n.startswith(pre + src + ".") and "/" not in n:
                    mine[pre + code + n[len(pre + src):]] = dave.payload(data, do, size, packed)
        if sp.get("cr_edit"):
            for n in ("m" + code + ".cr", code + ".cr"):
                if n in mine:
                    mine[n] = sp["cr_edit"](mine[n])
        mine.update(sp.get("extra") or {})
        if sp.get("edit"):
            for n in list(mine):
                mine[n] = sp["edit"](n, mine[n])
        files.update(mine)
        newrow = b"m" + code.encode() + row.group(0)[1 + len(src):]
        newrow = re.sub(rb"^(m\w+,character,[^,]*),\d+,[^,]*,[^,]*", rb"\1,1,NONE,0", newrow)
        unl = unl.rstrip(b"\r\n") + b"\r\n" + newrow + b"\r\n"
        names.add(code + ".cr")
        done.append((code, src, x))
    del data
    markers_new.sort(key=lambda t: t[0])
    roster_new.sort(key=lambda t: t[0])
    if roster_new:
        last = list(re.finditer(rb"<FONTGRIDDATA:y:\w+:\d+>\r?\n", pmv))[-1]
        pmv = pmv[:last.end()] + b"".join(line for _, line in roster_new) + pmv[last.end():]
    lineup = lineup.rstrip(b"\r\n") + b"\r\n" + b"".join(line for _, line in markers_new)
    ffiles["postermultiview.swui"] = pmv
    ffiles["characterlineup.swui"] = lineup
    ffiles.update(front_extra or {})
    if front_edit:
        front_edit(ffiles, front)
    os.makedirs(mods, exist_ok=True)
    out = os.path.join(mods, "front.dat")
    dave.merge(front, out + ".new", ffiles)
    os.replace(out + ".new", out)
    print("wrote " + out)
    files["unlockables.csv"] = unl
    if swat_edit:
        swat_edit(files, swat)
    out = os.path.join(mods, "swat.dat")
    dave.merge(swat, out + ".new", files)
    os.replace(out + ".new", out)
    print("wrote " + out)
    for code, src, x in done:
        print("added %s (a copy of %s), unlocked, at x=%d on the line-up (page %d)"
              % (code, src, x, x // PAGE_W + 1))


def main():
    a = sys.argv[1:]
    if len(a) == 5 and a[0] == "place":
        place(a[1], a[2], a[3], a[4])
        return
    if len(a) not in (4, 5):
        print(__doc__)
        sys.exit(1)
    add(a[0], a[1], a[2], a[3], a[4] if len(a) == 5 else None)


if __name__ == "__main__":
    main()
