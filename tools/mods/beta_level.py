#!/usr/bin/env python3
"""
beta_level.py - play a level of the January 2002 preview (PS2) in
the Xbox game, as a Showdown stage of its own.

  python tools\\mods\\beta_level.py list  <preview .iso or SWAT.ZIP>
  python tools\\mods\\beta_level.py add   <preview .iso or SWAT.ZIP> <game folder> <mods folder> <level> [name] ["Title"] ["subtitle"] [image] [--static]

  level     the preview's level, e.g. lvl_dm2 ("list" shows them all)
  name      the new stage's short name (default: "b" and the level's name,
            7 characters at most -- lvl_dm2 -> bdm2)
  Title     the poster's title (default: the level's name)
  --static  leave out the level's animated props (zone_<x>_anim_01: mill
            wheels, flags, swinging signs, breakable pieces); some of the
            preview's animations stop the retail engine while the level
            loads (Twin Rocks, Brimstone, Fallen Creek...)

e.g.
  python tools\\mods\\beta_level.py add "..\\PS2 Beta\\preview.iso" "..\\Red Dead Revolver (USA)" mods lvl_dm2 bdm2 "Deathmatch Two"

The stage is added after the others (addlevel.py: mods\\front.dat,
mods\\lvls_m2\\<name>2.dat, mods\\lvls_m4\\<name>4.dat); run it again to
rebuild it, "addlevel.py remove" to take it out. beta_mine.py is this for
the mine, with the extra choice of putting it in place of Broken Creek or
the first story chapter.

How it works (docs/MODDING.md): the retail game still has the loaders for
the preview's formats. A zone file (.znes) is asked for as .xznes -- the
same text -- and when the level has no packed resource file (.xrsc) each
zone's model and bounds are loaded by name, "<level>_<zone>.xmod" and
".bnd"; the model loader reads the preview's "version: 2.00", "1.10" and
"2.1x" models as they are, and "1.09" ones are rewritten as 1.10 (three
small differences, upgrade_109). The host stage's (Broken Creek's) own
geometry is left out; the level's spawn points are given to every player.
Textures the models use are converted from the preview's PS2 format
(betatex.py). What the preview level needs from outside its own files
(shared props, scripts) may be missing.
"""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
HOST = "perf"           # the retail stage a new one starts from


def iso_file(iso, name, out):
    """Copy file `name` from the root of an ISO 9660 image to `out`."""
    with open(iso, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[1:6] != b"CD001":
            raise SystemExit("not an ISO 9660 image: " + iso)
        root = pvd[156:156 + 34]
        lba = int.from_bytes(root[2:6], "little")
        size = int.from_bytes(root[10:14], "little")
        f.seek(lba * 2048)
        d = f.read(size)
        p = 0
        while p < len(d):
            n = d[p]
            if n == 0:
                p = (p // 2048 + 1) * 2048
                continue
            fid = d[p + 33:p + 33 + d[p + 32]].decode("latin-1").split(";")[0]
            if fid.upper() == name.upper():
                flba = int.from_bytes(d[p + 2:p + 6], "little")
                fsize = int.from_bytes(d[p + 10:p + 14], "little")
                f.seek(flba * 2048)
                with open(out, "wb") as o:
                    left = fsize
                    while left:
                        chunk = f.read(min(left, 1 << 20))
                        o.write(chunk)
                        left -= len(chunk)
                return
            p += n
    raise SystemExit(name + " is not in " + iso)


def run(*a):
    subprocess.run([PY] + list(a), check=True, stdout=subprocess.DEVNULL)


def upgrade_109(path):
    """A "version: 1.09" model as "1.10", which the retail loader reads.

    The two text formats differ in three places: 1.10 has a "reskins:"
    count after "matrices:", an "attributes:" line closing each material,
    and its "adj" rows (vertex, normal, color, tex1, tex2, tangent indices)
    hold only valid indices -- 1.09 files leave junk in unused columns
    (e.g. a tex2 index of 14526040 with no tex2s). Returns True if changed."""
    data = open(path, "rb").read()
    if not data.startswith(b"version: 1.09"):
        return False
    nl = b"\r\n" if b"\r\n" in data[:64] else b"\n"
    lines = data.split(nl)
    counts = {}
    for l in lines[:16]:
        if b":" in l:
            k, _, v = l.partition(b":")
            try:
                counts[k.strip()] = int(v)
            except ValueError:
                pass
    lim = [counts.get(k, 0) for k in (b"verts", b"normals", b"colors", b"tex1s", b"tex2s", b"tangents")]
    out, in_mtl = [], False
    for l in lines:
        t = l.strip()
        if t.startswith(b"version:"):
            out.append(b"version: 1.10")
            continue
        if t.startswith(b"mtl "):
            in_mtl = True
        elif in_mtl and t == b"}":
            out.append(b"\tattributes:\t0")
            in_mtl = False
        elif t.startswith(b"adj"):
            f = t.split()
            if len(f) == 7:
                idx = [int(x) for x in f[1:]]
                idx = [i if 0 <= i < n else 0 for i, n in zip(idx, lim)]
                l = b"\tadj" + b"".join(b"%6d" % i for i in idx)
        out.append(l)
        if t.startswith(b"matrices:"):
            out.append(b"reskins: 1")
    open(path, "wb").write(nl.join(out))
    return True


def spawn_points(text):
    """Every point of every list in a .spwn: [(pos line, heading line)]."""
    pts = []
    lines = [l.strip() for l in text.splitlines()]
    for i, l in enumerate(lines):
        if l.startswith("Position") and i + 1 < len(lines) and lines[i + 1].startswith("Heading"):
            pts.append((l, lines[i + 1]))
    return pts


def bound_tris(beta, src):
    """The triangles of the bounds the level loads: the .lvl's "Bounds"
    files name nodes bound_<x>_01, each <src>_<x>.bnd (text, version 1.01:
    "v" rows, then "tri a b c m" / "quad a b c d m")."""
    names = {f.lower(): f for f in os.listdir(beta)}
    lvl = open(os.path.join(beta, names[src + ".lvl"]), errors="ignore").read()
    m = re.search(r"Bounds\s*\{([^}]*)\}", lvl)
    tris = []
    for b in (m.group(1).split() if m else []):
        f = names.get(b.lower())
        if not f:
            continue
        for node in re.findall(r"node\s+bound_(\w+?)_\d+\s*\{", open(os.path.join(beta, f), errors="ignore").read()):
            g = names.get("%s_%s.bnd" % (src, node.lower()))
            if not g:
                continue
            vs = []
            for l in open(os.path.join(beta, g), errors="ignore"):
                t = l.split()
                if not t:
                    continue
                try:
                    if t[0] == "v":
                        vs.append(tuple(float(x) for x in t[1:4]))
                    elif t[0] == "tri":
                        tris.append(tuple(vs[int(i)] for i in t[1:4]))
                    elif t[0] == "quad":
                        a, b2, c, d = (vs[int(i)] for i in t[1:5])
                        tris += [(a, b2, c), (a, c, d)]
                except (ValueError, IndexError):
                    pass
    return tris


def ground_under(tris, x, y, z, below=4.0, above=1.0):
    """1 if some bound triangle is under (x, z) within [y-below, y+above]."""
    for (ax, ay, az), (bx, by, bz), (cx, cy, cz) in tris:
        d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
        if abs(d) < 1e-9:
            continue
        l1 = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d
        l2 = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d
        l3 = 1 - l1 - l2
        if min(l1, l2, l3) < -1e-4:
            continue
        h = l1 * ay + l2 * by + l3 * cy
        if y - below <= h <= y + above:
            return 1
    return 0


def on_ground(pts, beta, src):
    """The spawn points with ground under them. Some preview levels list
    points in parts the .lvl does not load (the mine's second base), where
    a player would fall forever."""
    tris = bound_tris(beta, src)
    if not tris:
        return pts
    good = []
    for pos, head in pts:
        try:
            x, y, z = (float(v) for v in pos.split()[1:4])
        except ValueError:
            continue
        if ground_under(tris, x, y, z):
            good.append((pos, head))
    print("  spawn points with ground under them: %d of %d" % (len(good), len(pts)))
    return good or pts


def spawn_file(pts):
    """Four lists, one per player, from the mine's points (players take
    different ones where there are enough)."""
    out = []
    for k in range(4):
        mine = pts[k::4] or pts
        out.append("NumSpawnPoints%d\t%d " % (k + 1, len(mine)))
        for pos, head in mine:
            out += [pos + " ", head + " ", ""]
    return "\n".join(out) + "\n"


def drop_shadow_boxes(text):
    """A .bxes without its SHADOWMAP boxes: each wants a pre-drawn shadow
    texture (lvl_<level>_shbox_<name>) the preview never had, and the game
    stops at the first one it cannot find (imgShadowBox::Init)."""
    boxes = re.split(r"(?m)^(?=Box )", text)
    head, boxes = boxes[0], boxes[1:]
    keep = [b for b in boxes if not re.match(r"Box\s+box_SHADOWMAP_", b, re.I)]
    if len(keep) == len(boxes):
        return None
    head = re.sub(r"NumBoxes\s+\d+", "NumBoxes %d" % len(keep), head)
    return head + "".join(keep)


def drop_anim_zones(text):
    """A .znes without its animated zones (node zone_<x>_anim_01): the
    preview's animations ("cmp" format) do not all play in the retail
    engine -- some come out empty and the zone's update divides by zero
    (sub_000D8B40), stopping the level's loading. The props go; the level
    loads."""
    m = re.match(r"\s*NumInstNodes\s+\d+\s*", text)
    if not m:
        return None
    nodes = re.split(r"(?m)^(?=node )", text[m.end():])
    keep = [n for n in nodes if n.strip() and not re.match(r"node\s+zone_\w*_anim\w*\s*\{", n)]
    if len(keep) == len([n for n in nodes if n.strip()]):
        return None
    return "NumInstNodes %d\n\n" % len(keep) + "".join(keep)


def build(swat_dir, tex_dir, wanted, retail, out, level, story, src, static=False):
    """retail: the archive to start from; out: where to write it; level:
    the level name it is loaded as (lvl_perf2); src: the preview level
    (lvl_mine) whose files are in swat_dir."""
    work = tempfile.mkdtemp(prefix="beta_mine_lvl_")
    try:
        run(os.path.join(HERE, "dave.py"), "extract", retail, work)
        files = {f.lower(): f for f in os.listdir(work)}

        def put(name, src=None, text=None):
            dst = os.path.join(work, files.get(name.lower(), name.lower()))
            if text is not None:
                open(dst, "w", newline="\n").write(text)
            else:
                shutil.copyfile(src, dst)

        beta = swat_dir
        for f in os.listdir(beta):
            path = os.path.join(beta, f)
            low = f.lower()
            put(low, path)
            if low.endswith(".bxes"):
                t = drop_shadow_boxes(open(path, "r", errors="replace").read())
                if t is not None:
                    put(low, text=t)
            if low.endswith(".znes"):
                t = drop_anim_zones(open(path, "r", errors="replace").read()) if static else None
                if t is not None:
                    put(low, text=t)
                    put(low[:-5] + ".xznes", text=t)
                else:
                    put(low[:-5] + ".xznes", path)
            # the level's own pieces under the name it loads as; animated
            # zones (zone_<x>_anim_01) find lvl_<level>_<x>_anim.anim and
            # .skel the same way, and without them sample an empty
            # animation (a division by zero in sub_000D8B40)
            if low.startswith(src + "_") and low.endswith((".bnd", ".mod", ".anim", ".skel")):
                rest = low[len(src) + 1:]
                if low.endswith(".mod"):
                    put(level + "_" + rest[:-4] + ".xmod", path)
                else:
                    put(level + "_" + rest, path)
        names = {f.lower(): f for f in os.listdir(beta)}
        put(level + ".lvl", os.path.join(beta, names[src + ".lvl"]))
        if src + ".spwn" in names:
            pts = on_ground(spawn_points(open(os.path.join(beta, names[src + ".spwn"])).read()), beta, src)
            if pts:
                put(level + ".spwn", text=spawn_file(pts))
        for f in list(os.listdir(work)):
            if f.lower() == level + ".xrsc":
                os.remove(os.path.join(work, f))
        if story:
            put(level + "_var_1.enm", text="PlayerBrain\tBR_playerAI\n\n")
        for s, src in wanted:
            run(os.path.join(HERE, "betatex.py"), src, os.path.join(work, s + ".xtex"))
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        run(os.path.join(HERE, "dave.py"), "build", work, out + ".new", retail)
        os.replace(out + ".new", out)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def levels(swat):
    """The preview's levels: {lower name: (name, has spawn points)}."""
    out = subprocess.run([PY, os.path.join(HERE, "beta.py"), "list", swat],
                         check=True, capture_output=True, text=True).stdout
    names = set(re.findall(r"\b(lvl_[A-Za-z0-9]+)\.lvl\b", out))
    spwn = set(m.lower() for m in re.findall(r"\b(lvl_[A-Za-z0-9]+)\.spwn\b", out))
    return {n.lower(): (n, n.lower() in spwn) for n in names}


def open_preview(swat, work):
    if swat.lower().endswith(".iso"):
        print("reading SWAT.ZIP from the disc image...")
        z = os.path.join(work, "SWAT.ZIP")
        iso_file(swat, "SWAT.ZIP", z)
        return z
    return swat


def prepare(swat, src, work):
    """Extract the preview level src (lvl_mine) and the textures its models
    use. Returns (dir of its files, textures [(name, path)])."""
    beta, tex = os.path.join(work, "beta"), os.path.join(work, "tex")
    print("extracting %s from the preview..." % src)
    run(os.path.join(HERE, "beta.py"), "extract", swat, beta, src + ".*")
    run(os.path.join(HERE, "beta.py"), "extract", swat, beta, src + "_*")
    if not os.path.exists(os.path.join(beta, src + ".lvl")) and \
            src + ".lvl" not in [f.lower() for f in os.listdir(beta)]:
        sys.exit("%s is not a level of the preview (try 'list')" % src)
    run(os.path.join(HERE, "beta.py"), "extract", swat, tex, "*.tex")
    up = sum(upgrade_109(os.path.join(beta, f)) for f in os.listdir(beta)
             if f.lower().endswith(".mod"))
    print("%d version 1.09 models brought up to 1.10" % up)
    texs = {f.lower()[:-4]: f for f in os.listdir(tex) if f.lower().endswith(".tex")}
    wanted = set()
    for f in os.listdir(beta):
        if f.lower().endswith(".mod"):
            d = open(os.path.join(beta, f), "rb").read().lower()
            for m in re.findall(rb"[a-z0-9_\-]{3,40}", d):
                if m.decode() in texs:
                    wanted.add(m.decode())
    good = []
    for s in sorted(wanted):
        p = os.path.join(tex, texs[s])
        try:
            run(os.path.join(HERE, "betatex.py"), p, os.path.join(work, "check.xtex"))
            good.append((s, p))
        except subprocess.CalledProcessError:
            print("  (texture %s not converted)" % s)
    print("%d textures" % len(good))
    return beta, good


def make_stage(swat, game, mods, src, name, title, desc="2002 preview", image="levelselect_perf",
               static=False):
    """The preview level src as a new Showdown stage lvl_<name>."""
    sys.path.insert(0, HERE)
    import addlevel
    src = src.lower()
    work = tempfile.mkdtemp(prefix="beta_level_")
    try:
        swat = open_preview(swat, work)
        beta, good = prepare(swat, src, work)
        # a fresh copy of Broken Creek under the new name (so running this
        # again starts from the farm again), then the level goes into it
        for n in (2, 4):
            addlevel.copy_stage(game, mods, HOST, name, n, from_disc=True)
        rows = addlevel.with_front(game, mods, lambda rows: None)
        if "lvl_" + name not in [r[0].lower() for r in rows]:
            addlevel.add(game, mods, name, HOST, title, desc, image, copy=False)
        for n in (2, 4):
            out = os.path.join(mods, "lvls_m%d" % n, "%s%d.dat" % (name, n))
            print("building " + out + " ...")
            build(beta, None, good, out, out, "lvl_%s%d" % (name, n), False, src, static)
        print("done: pick \"%s\" on the Showdown poster" % title)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def make_in_place(swat, game, mods, src, hosts):
    """The preview level src in place of retail levels: hosts is
    [(archive path under the game folder, level name, story?)]."""
    work = tempfile.mkdtemp(prefix="beta_level_")
    try:
        swat = open_preview(swat, work)
        beta, good = prepare(swat, src.lower(), work)
        for rel, level, story in hosts:
            retail = os.path.join(game, *rel.split("/"))
            out = os.path.join(mods, *rel.split("/"))
            print("building " + out + " ...")
            build(beta, None, good, retail, out, level, story, src.lower())
        print("done")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    a = sys.argv[1:]
    static = "--static" in a
    a = [x for x in a if x != "--static"]
    if len(a) == 2 and a[0] == "list":
        work = tempfile.mkdtemp(prefix="beta_level_")
        try:
            lv = levels(open_preview(a[1], work))
        finally:
            shutil.rmtree(work, ignore_errors=True)
        for k in sorted(lv):
            print("%-22s %s" % (lv[k][0], "" if lv[k][1] else "(no spawn points of its own: players start where Broken Creek's would)"))
        print("%d levels" % len(lv))
        return
    if not (5 <= len(a) <= 9 and a[0] == "add"):
        print(__doc__)
        sys.exit(1)
    swat, game, mods, src = a[1:5]
    src = src.lower()
    if not src.startswith("lvl_"):
        src = "lvl_" + src
    base = re.sub(r"[^a-z0-9_]", "", src[4:])
    name = a[5].lower() if len(a) > 5 else ("b" + base)[:7]
    title = a[6] if len(a) > 6 else src[4:]
    desc = a[7] if len(a) > 7 else "2002 preview"
    image = a[8] if len(a) > 8 else "levelselect_perf"
    make_stage(swat, game, mods, src, name, title, desc, image, static)


if __name__ == "__main__":
    main()
