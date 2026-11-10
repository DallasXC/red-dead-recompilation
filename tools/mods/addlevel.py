#!/usr/bin/env python3
"""
addlevel.py - add new stages to Showdown's level list (instead of replacing
one), list them, or take them out again.

  python tools\\mods\\addlevel.py list   <game folder> <mods folder>
  python tools\\mods\\addlevel.py add    <game folder> <mods folder> <name> <copy of> "<Title>" "<subtitle>" [image]
  python tools\\mods\\addlevel.py remove <game folder> <mods folder> <name>

  name      the new stage's short name, 1-7 letters/digits/_ (the game makes
            the archive name from it: lvls_m2\\<name>2.dat, lvls_m4\\<name>4.dat,
            and the level inside is lvl_<name>2 / lvl_<name>4)
  copy of   the existing stage the new one starts as a copy of (hng, fort,
            shernit, bridge, ghost, coach, perf, ranch, core, gun, flash,
            mans, int, colseum -- or one you added before)
  Title     the name on the poster; subtitle the line under the picture
            (at most 31 characters each, no "," ":" or ">")
  image     a front.dat picture for the poster (default: the copied stage's,
            e.g. levelselect_core2)

e.g.
  python tools\\mods\\addlevel.py add "..\\Red Dead Revolver (USA)" mods bcreek perf "Old Creek" "a copy to edit"

"add" writes mods\\front.dat (the menu's list, starting from the copy already
in mods\\ if there is one) and mods\\lvls_m2\\<name>2.dat + lvls_m4\\<name>4.dat
(the copied stage, its files renamed to the new level's name). Change those
two archives with dave.py to make the stage your own -- beta_mine.py does
that for the preview's mine.

Limits (docs/MODDING.md, "Adding stages"):
  * 50 stages in all (the game's list holds 50 rows; 14 are used, so 36 can
    be added). The game does not check, so this tool does.
  * 7 characters for the name (8.3 file names: the player-count digit takes
    the 8th place).
  * 31 characters for the title and the subtitle.
New stages are never locked (the unlock list only knows the original 14).
"""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
SWUI = "postermultiview.swui"
MAX_ROWS = 50
ROW = re.compile(rb"^<LOOKUPDATA:([^,>]*),\s*([^,>]*),\s*([^,>]*),\s*([^,>]*)>", re.M)


def run(*a):
    subprocess.run([PY] + list(a), check=True, stdout=subprocess.DEVNULL)


def front_src(game, mods):
    m = os.path.join(mods, "front.dat")
    return m if os.path.exists(m) else os.path.join(game, "front.dat")


def table(swui):
    """(start, end) of the LevelTable block, and its rows [(match)]."""
    i = swui.find(b"<LOOKUP:LevelTable>")
    if i < 0:
        sys.exit("front.dat: no LevelTable in " + SWUI)
    j = swui.find(b"\n", i) + 1
    end = j
    rows = []
    for line in swui[j:].split(b"\n"):
        s = line.strip()
        if s.startswith(b"<LOOKUPDATA:"):
            m = ROW.match(s)
            if m:
                rows.append([g.strip().decode("latin-1") for g in m.groups()])
            end += len(line) + 1
        elif s.startswith(b";") or not s:
            # comments and blank lines inside the table are fine, but a
            # blank line after the last row ends it
            if rows and not s:
                break
            end += len(line) + 1
        else:
            break
    return j, end, rows


def with_front(game, mods, edit):
    """Extract front.dat, let edit(rows) change the rows, rebuild into mods."""
    src = front_src(game, mods)
    work = tempfile.mkdtemp(prefix="addlevel_front_")
    try:
        run(os.path.join(HERE, "dave.py"), "extract", src, work, SWUI)
        p = os.path.join(work, SWUI)
        s = open(p, "rb").read()
        a, b, rows = table(s)
        new = edit(rows)
        if new is None:
            return rows
        body = b"".join(("<LOOKUPDATA:%s,\t%s,\t%s,\t%s>\r\n" % tuple(r)).encode("latin-1") for r in new)
        s = s[:a] + body + b"\r\n" + s[b:].lstrip(b"\r\n")
        full = tempfile.mkdtemp(prefix="addlevel_full_")
        try:
            run(os.path.join(HERE, "dave.py"), "extract", src, full)
            open(os.path.join(full, SWUI), "wb").write(s)
            out = os.path.join(mods, "front.dat")
            tmp = out + ".new"
            os.makedirs(mods, exist_ok=True)
            run(os.path.join(HERE, "dave.py"), "build", full, tmp, os.path.join(game, "front.dat"))
            os.replace(tmp, out)
        finally:
            shutil.rmtree(full, ignore_errors=True)
        return new
    finally:
        shutil.rmtree(work, ignore_errors=True)


def stage_archive(game, mods, name, n, from_disc=False):
    rel = os.path.join("lvls_m%d" % n, "%s%d.dat" % (name, n))
    for base in ((game,) if from_disc else (mods, game)):
        p = os.path.join(base, rel)
        if os.path.exists(p):
            return p
    return None


def copy_stage(game, mods, src, dst, n, from_disc=False):
    """lvls_mN/<src>N.dat -> mods/lvls_mN/<dst>N.dat with the level renamed."""
    a = stage_archive(game, mods, src, n, from_disc)
    if not a:
        sys.exit("no lvls_m%d\\%s%d.dat in the game or mods folder" % (n, src, n))
    old, new = "lvl_%s%d" % (src, n), "lvl_%s%d" % (dst, n)
    ob, nb = old.encode(), new.encode()
    work = tempfile.mkdtemp(prefix="addlevel_stage_")
    try:
        run(os.path.join(HERE, "dave.py"), "extract", a, work)
        for f in os.listdir(work):
            if old not in f:
                continue
            p = os.path.join(work, f)
            data = open(p, "rb").read()
            if ob in data:
                if len(ob) == len(nb) or b"\0" not in data:
                    # text files (.lvl, .csv, ...) or a same-length name
                    data = data.replace(ob, nb)
            # keep the original too: anything inside a binary file that still
            # names the old level finds it
            open(os.path.join(work, f.replace(old, new)), "wb").write(data)
            if len(ob) == len(nb):
                os.remove(p)
        out = os.path.join(mods, "lvls_m%d" % n, "%s%d.dat" % (dst, n))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        tmp = out + ".new"
        run(os.path.join(HERE, "dave.py"), "build", work, tmp, a)
        os.replace(tmp, out)
        return out
    finally:
        shutil.rmtree(work, ignore_errors=True)


def check_field(what, s):
    if len(s) > 31:
        sys.exit("%s is %d characters; the game keeps 31" % (what, len(s)))
    if re.search(r"[,:>\t]", s):
        sys.exit("%s cannot hold , : > or a tab" % what)


def add(game, mods, name, src, title, desc, image=None, copy=True):
    name, src = name.lower(), src.lower()
    for p in ("lvl_",):
        name = name[len(p):] if name.startswith(p) else name
        src = src[len(p):] if src.startswith(p) else src
    if not re.fullmatch(r"[a-z0-9_]{1,7}", name):
        sys.exit("name: 1-7 of a-z 0-9 _ (the game builds an 8.3 file name from it)")
    check_field("title", title)
    check_field("subtitle", desc)
    state = {}

    def edit(rows):
        keys = [r[0].lower() for r in rows]
        if "lvl_" + name in keys:
            sys.exit("lvl_%s is already in the list" % name)
        if "lvl_" + src not in keys:
            sys.exit("lvl_%s is not in the list (see 'list')" % src)
        if len(rows) >= MAX_ROWS:
            sys.exit("the list is full (%d stages, the game holds %d)" % (len(rows), MAX_ROWS))
        img = image or rows[keys.index("lvl_" + src)][1]
        check_field("image", img)
        state["n"] = len(rows) + 1
        return rows + [["lvl_" + name, img, title, desc]]

    if copy:
        # check the list first so a bad name doesn't leave archives behind
        with_front_check(game, mods, name, src)
        for n in (2, 4):
            print("wrote " + copy_stage(game, mods, src, name, n))
    with_front(game, mods, edit)
    print("wrote %s (%d of %d stages)" % (os.path.join(mods, "front.dat"), state["n"], MAX_ROWS))


def with_front_check(game, mods, name, src):
    work = tempfile.mkdtemp(prefix="addlevel_chk_")
    try:
        run(os.path.join(HERE, "dave.py"), "extract", front_src(game, mods), work, SWUI)
        _, _, rows = table(open(os.path.join(work, SWUI), "rb").read())
    finally:
        shutil.rmtree(work, ignore_errors=True)
    keys = [r[0].lower() for r in rows]
    if "lvl_" + name in keys:
        sys.exit("lvl_%s is already in the list" % name)
    if "lvl_" + src not in keys:
        sys.exit("lvl_%s is not in the list (see 'list')" % src)
    if len(rows) >= MAX_ROWS:
        sys.exit("the list is full (%d stages, the game holds %d)" % (len(rows), MAX_ROWS))


def remove(game, mods, name):
    name = name.lower()
    name = name[4:] if name.startswith("lvl_") else name

    def edit(rows):
        keep = [r for r in rows if r[0].lower() != "lvl_" + name]
        if len(keep) == len(rows):
            sys.exit("lvl_%s is not in the list" % name)
        return keep

    with_front(game, mods, edit)
    print("took lvl_%s out of %s" % (name, os.path.join(mods, "front.dat")))
    for n in (2, 4):
        p = os.path.join(mods, "lvls_m%d" % n, "%s%d.dat" % (name, n))
        if os.path.exists(p):
            print("  (its archive %s is still there; delete it if you like)" % p)


def list_rows(game, mods):
    rows = with_front(game, mods, lambda rows: None)
    for i, r in enumerate(rows):
        name = r[0][4:] if r[0].lower().startswith("lvl_") else r[0]
        where = []
        for n in (2, 4):
            a = stage_archive(game, mods, name, n)
            where.append(("mods" if a and a.startswith(os.path.abspath(mods)) or (a and a.startswith(mods)) else "disc") if a else "MISSING")
        print("%2d  %-12s %-22s %-24s %s  (2p: %s, 4p: %s)" % (i + 1, r[0], r[1], r[2], r[3], where[0], where[1]))
    print("%d of %d stages" % (len(rows), MAX_ROWS))


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "list":
        list_rows(a[1], a[2])
    elif len(a) in (7, 8) and a[0] == "add":
        add(a[1], a[2], a[3], a[4], a[5], a[6], a[7] if len(a) == 8 else None)
    elif len(a) == 4 and a[0] == "remove":
        remove(a[1], a[2], a[3])
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
