#!/usr/bin/env python3
"""
beta_roster.py - every character of the January 2002 preview ("Red Dead
Revolved", PS2) as a Showdown character, in its preview look. EXPERIMENTAL.

  python tools\\mods\\beta_roster.py list <preview .iso or SWAT.ZIP> <game folder>
  python tools\\mods\\beta_roster.py add  <preview .iso or SWAT.ZIP> <game folder> <mods folder> [character ...]
  python tools\\mods\\beta_roster.py fix  <mods folder>   (one made before the 50-row fix)

`add` with no characters adds them all (the humans: the preview's horse,
bull, buffalo, wolves, dogs and rooster walk on four legs with their own
animations and are left out), all on the line-up's second page in three
rows (tallest at the back, as the retail page stands), named after the retail character it became ("Chicken '02") or, for
those the retail game dropped, after its preview code. Run it on a mods
folder without them (it refuses codes already there).

What each gets, from the preview (see beta_char.py for one at a time):
  - its model (<render>_lod_01/02), moved into the model's space with its
    own skeleton (the .cr's FileNameSkel; mod212.py), and its hat;
  - its skeleton, as <code>.skel; its textures, the preview's own pictures
    converted (betatex.py) -- a name the retail game also has (usually with
    another picture in it) is renamed in the models;
  - its pictures, drawn from the model (charpic.py): the cut-out on the
    line-up photo and the portrait in the player box;
  - its name, in swat.strtbl (front.dat and swat.dat).
  - its weapon and special moves, from the preview's own Showdown set-up
    (2000.swui: WeaponTable, Special1Table, Special2Table): the starting
    weapon in Showdown's CharWeaponTable (postermultiview.swui) and the
    .ars, a pistol beside a thrown one (a special move needs a gun drawn);
    the special moves as NormalSpecialMove / MeleeSpecialMove in its .swt
    (the retail engine's list is the preview's from Counter on; Rapid Fire
    and Spin Fire are gone: Red's Lock Blast and the Crazy Shoot instead),
    none where the preview gave none;
  - the preview's flier, Bat Boy (scb: Glide, Glide Attack): its own flight
    values (FlightData, from the preview's scb.msd, in the retail format of
    the condor's cnd.msd) and its glide animations, which the retail game
    still has (scb_for_gld, scb_for_gld_ant, scb_for_chg), in its .rep,
    and, played by the computer, the retail game's own Bat Boy brain
    (batboy.tbl, still on the disc: it circles and dives).
Moves, voice and the rest are the retail character's it is copied
from: the same character's when the retail game has it (the preview's
Chicken gets retail Chicken's voice and moves), else Red's, or as given
(code=copy, e.g. jes=ctn). The preview's own were written for an
older engine (its .swt damage is a fraction of retail's, its moves file a
tenth the size) and do not load in the retail one as they are; the
preview has no voice sets of its own (its sounds are mission dialogue).
"""
import io, os, re, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
sys.path.insert(0, HERE)
import addchar, beta, beta_char, beta_level, betatex, charpic, dave, mod212, strtbl

from PIL import Image


def run(*a):
    subprocess.run([PY] + list(a), check=True, stdout=subprocess.DEVNULL)


def cr_field(cr, key):
    m = re.search(rb"\n\s*" + key.encode() + rb"\s+(\S+)", cr)
    return m.group(1).decode().lower() if m else None


def preview_roster(b):
    """[(preview code, render name, skeleton)] of the humans, one per model."""
    out, seen = [], set()
    files = sorted(os.listdir(b))
    # a character named after its model wins over others sharing it (the
    # preview's scb over nrd2, which wears scb's model)
    files.sort(key=lambda f: 0 if f[:-3].lower() == (cr_field(open(os.path.join(b, f), "rb").read(),
                                                                "FileNameRender") or "") else 1)
    for f in files:
        if not f.lower().endswith(".cr") or f.startswith("."):
            continue
        cr = open(os.path.join(b, f), "rb").read()
        pc = f[:-3].lower()
        anim, render, skel = cr_field(cr, "FileNameAnim"), cr_field(cr, "FileNameRender"), cr_field(cr, "FileNameSkel")
        if anim != "red" or not render or not skel or render in seen or not pc.isalnum():
            continue
        if pc.startswith("jtng"):
            continue                        # a training-level stand-in
        seen.add(render)
        out.append((pc, render, skel))
    return out


# The preview's own Showdown set-up (2000.swui): each character's weapon and
# special moves, as the retail engine names them. The retail special-move
# list is the preview's from Counter on (1 Counter ... 9 Crazy Horse); the
# preview's Rapid Fire and Spin Fire are gone, nearest: Red's lock-and-blast
# and the crazy shooting.
WEAPONS = {0: "Six_Shooter", 1: "Twin_Revolvers", 2: "Sawn_Off", 3: "Halcon_Rifle", 4: "Knife",
           5: "Dynamite"}
SPECIALS = {1: "spmove_lockblast", 2: "spmove_crazyshoot", 3: "spmove_countershoot",
            4: "spmove_dynamitebarrier", 5: "spmove_ghosteffect", 6: "spmove_ricochet",
            7: "spmove_armor", 8: "spmove_tackle", 9: "spmove_glide", 10: "spmove_glideattack",
            11: "spmove_crazyhorse"}
THROWN = ("Knife", "Dynamite")
MELEE = ("spmove_glideattack", "spmove_tackle", "spmove_grab")
# Names the preview itself gives (its AI tables: BatBoy.tbl for scb), and
# for some it leaves unnamed, by what they are (coc: Red in his long coat).
OWN_NAMES = {"scb": "Bat Boy", "coc": "Long Coat Red", "yrd": "Young Red",
             # trd: an early teen Red (user, after it was first taken for a
             # shooting target); nrd a later redesign of Red, closer to the
             # final one (user: "New Red (2002)")
             "trd": "Teen Red", "nrd": "New Red"}


TABLE_ROWS = 50
# Who gets one of the few CharWeaponTable rows first: the ones whose
# weapon is the point of them (Bat Boy's knives, Pig Josh's dynamite...).
WEAPON_ORDER = ("Knife", "Dynamite", "Twin_Revolvers", "Halcon_Rifle", "Sawn_Off")


def weapon_rows(weapons):
    """[(code, cells)] for CharWeaponTable, best first; a thrower (Knife,
    Dynamite) also carries a pistol, as the retail throwers do (chk:
    Six_Shooter, Dynamite): a special move needs a gun drawn."""
    out = []
    for w in WEAPON_ORDER:
        for c, cw in sorted(weapons.items()):
            if cw == w:
                out.append((c, (b"Six_Shooter,\t" + w.encode()) if w in THROWN else w.encode()))
    return out


def table_rows(swui, table, rows, drop_default=True):
    """swui with rows (code, cells) added to LOOKUP table, as many as its 50
    rows allow (rows that only repeat its "default" row taken out first when
    drop_default)."""
    m = re.search(rb"<LOOKUP:" + table + rb">\r?\n", swui)
    if not m or not rows:
        return swui
    end = swui.find(b"<LOOKUP:", m.end())
    end = len(swui) if end < 0 else end
    body = swui[m.end():end]
    lines = body.split(b"\n")
    data = [l for l in lines if l.startswith(b"<LOOKUPDATA:")]
    default = next((l for l in data if l.startswith(b"<LOOKUPDATA:default,")), None)
    if drop_default and default is not None:
        cells = default.split(b",", 1)[1]
        keep = []
        for l in lines:
            if l.startswith(b"<LOOKUPDATA:") and l is not default and l.split(b",", 1)[1] == cells:
                continue
            keep.append(l)
        lines = keep
        data = [l for l in lines if l.startswith(b"<LOOKUPDATA:")]
    room = TABLE_ROWS - len(data)
    added = rows[:max(0, room)]
    for c, cells in rows[len(added):]:
        print("  %s: no room in %s (50 rows), the default" % (c, table.decode()))
    new = b"".join(b"<LOOKUPDATA:%s,\t%s>\r\n" % (c.encode(), cells) for c, cells in added)
    return swui[:m.end()] + new + b"\n".join(lines) + swui[end:]


def fit_tables(swui):
    """A postermultiview.swui made before the 50-row limit was known (its
    CharWeaponTable over 50 rows: the front end crashed): rows repeating
    the default dropped, then the added ones in WEAPON_ORDER kept while
    rows last. Returns (swui, rows dropped)."""
    m = re.search(rb"<LOOKUP:CharWeaponTable>\r?\n", swui)
    if not m:
        return swui, 0
    end = swui.find(b"<LOOKUP:", m.end())
    end = len(swui) if end < 0 else end
    lines = swui[m.end():end].split(b"\n")
    data = [l for l in lines if l.startswith(b"<LOOKUPDATA:")]
    if len(data) <= TABLE_ROWS:
        return swui, 0
    default = next(l for l in data if l.startswith(b"<LOOKUPDATA:default,"))
    cells = default.split(b",", 1)[1]
    added = [l for l in data if re.match(rb"<LOOKUPDATA:[qxzvj][a-z]{2},", l)]
    retail = [l for l in data if l not in added and (l is default or l.split(b",", 1)[1] != cells)]
    def rank(l):
        c = l.split(b",", 1)[1]
        for i, w in enumerate(WEAPON_ORDER):
            if w.encode() in c:
                return i
        return len(WEAPON_ORDER)
    added = sorted(added, key=rank)[:max(0, TABLE_ROWS - len(retail))]
    rest = [l for l in lines if not l.startswith(b"<LOOKUPDATA:")]
    body = b"\n".join(added + retail) + b"\n" + b"\n".join(rest)
    return swui[:m.end()] + body + swui[end:], len(data) - len(added) - len(retail)


def preview_setup(swui):
    """{table: {code: [values]}} from 2000.swui's LOOKUP tables."""
    out, cur = {}, None
    for m in re.finditer(rb"<LOOKUP(DATA)?:([^>]*)>", swui):
        if not m.group(1):
            cur = out.setdefault(m.group(2).decode(), {})
        elif cur is not None:
            k, _, v = re.split(r"([:,])", m.group(2).decode(), maxsplit=1)
            cur.setdefault(k.lower(), []).append(v)
    return out


def abilities(setup, pc, render):
    """(arsenal, normal special, melee special) of a preview character."""
    def get(table, default=None):
        t = setup.get(table, {})
        return t.get(pc) or t.get(render) or ([default] if default is not None else [])
    try:
        weapon = WEAPONS.get(int(get("WeaponTable", t_default(setup))[0]), "Six_Shooter")
    except ValueError:
        weapon = "Six_Shooter"
    s1 = [SPECIALS.get(int(v)) for v in get("Special1Table") if v.isdigit()]
    s2 = [SPECIALS.get(int(v)) for v in get("Special2Table") if v.isdigit()]
    normal = next((x for x in s1 + s2 if x and x not in MELEE), None) or next((x for x in s1 if x), None)
    melee = next((x for x in s1 + s2 if x in MELEE and x != normal), None)
    return weapon, normal, melee


def t_default(setup):
    return (setup.get("WeaponTable", {}).get("default") or ["0"])[0]


def root_height(skel_text):
    """The root bone's height in a .skel (its place in the bind pose)."""
    bones, world = mod212.skel_bones(skel_text)
    for (n, _p, _o), w in zip(bones, world):
        if n == "root":
            return w[1]
    return None


SWT_NUM = rb"(\n[ \t]*%s[ \t]+)(-?[0-9.]+(?:[eE][-+]?\d+)?)"


def swt_get(b, key):
    m = re.search(SWT_NUM % key, b)
    return float(m.group(2)) if m else None


def swt_set(b, key, v):
    return re.sub(SWT_NUM % key, lambda m: m.group(1) + b"%f" % v, b, count=1)


def stand_on_ground(b, showdown, ref, new_root, ref_root, ref_swt):
    """A copied .swt's standing values for a skeleton whose root is at
    new_root: the reference character's (ref_swt: its .swt, "m": its
    Showdown one), moved by the difference of the roots (OffsetY by it all,
    DuckOffsetY by about half, as the retail characters'; the heights the
    cameras and the AI use in proportion)."""
    base = ref_swt.get("m" if showdown else "") or ref_swt.get("") or b
    dy = new_root - ref_root
    # (the retail heights follow the root only roughly: a giant's camera
    # stays near the others')
    k = min(max(new_root / ref_root, 0.7), 1.15)
    for key, f in ((b"OffsetY", lambda v: v + dy),
                   (b"DuckOffsetY", lambda v: v + 0.55 * dy),
                   (b"Height", lambda v: v * k),
                   (b"StandHeightForCameras", lambda v: v * k)):
        v = swt_get(base, key)
        if v is not None:
            b = swt_set(b, key, f(v))
    m = re.search(rb"\n[ \t]*EdgeModel[ \t]+(\S+)", base)
    if m and base is not b:
        b = re.sub(rb"(\n[ \t]*EdgeModel[ \t]+)\S+", lambda mm: mm.group(1) + m.group(1), b, count=1)
    return b


# The preview's line-up page: everyone on one page (the second), standing
# in three rows as the retail page does -- each cut-out is copied 1:1 onto
# the 1024 x 256 photo, so a figure's place in its picture is its place on
# the photo (retail: back row feet near y 190, middle 245, front 253, 16 to
# a row). Before, 14 to a page in one row over four pages, each marker's
# hotspot (where the hand points, and what it picks) copied from the retail
# character it was made from -- often that one's back-row head, high above
# a preview figure standing at the front (user: the selection did not
# line up with the cut-outs).
LINEUP_ROWS = (                  # (feet y, height of a figure of median height)
    (178.0, 136.0),              # back: drawn first, the others in front of it
    (218.0, 150.0),
    (254.0, 160.0),
)


def lay_out(specs, page=1):
    """Give each spec its place on the line-up (x, row, hotspot) and draw
    its cut-out there: the tallest at the back, in rows across one page
    (page 1: the second, x 1024-2047)."""
    drawn = [sp for sp in specs if sp.get("pic")]
    if not drawn:
        return
    heights = sorted(sp["pic"][2] for sp in drawn)
    median = heights[len(heights) // 2] or 1.0
    # one whose picture could not be drawn keeps the copied character's
    # cut-out and hotspot -- copied from Red (the front row) so far: it
    # stands in the front row
    order = sorted(drawn, key=lambda sp: -sp["pic"][2]) + [sp for sp in specs if not sp.get("pic")]
    n, rows = len(order), len(LINEUP_ROWS)
    counts = [n // rows + (1 if r >= rows - n % rows else 0) for r in range(rows)]
    k = 0
    for r, cnt in enumerate(counts):
        feet, base = LINEUP_ROWS[r]
        # pictures 128 wide from x 0 to 896 (the photo 1024), one step for
        # every row, each row a third of a step over from the one behind
        # (lined up, a back figure's coat, a middle one's body and a front
        # one's legs ran together into one tall shape in the sepia)
        step = 896.0 / (max(counts) - 1 + 2.0 / 3.0)
        x0 = step * r / 3.0 + step * (max(counts) - cnt) / 2.0
        row = order[k:k + cnt]
        row.sort(key=lambda sp: specs.index(sp))         # the roster's order across
        for i, sp in enumerate(row):
            sp["x"] = addchar.PAGE_W * page + int(round(x0 + step * i))
            sp["row"] = r
            if not sp.get("pic"):
                continue
            mm, tex, h = sp["pic"]
            px = max(base * 0.8, min(base * 1.18, base * h / median))
            blob, im = charpic.cutout_xtex(mm, tex, px, feet)
            sp["front"]["ui_mphoto_%s.xtex" % sp["code"]] = blob
            # the hand points at the face: the figure's centre across, an
            # eighth of the way down from the top of its head
            sp["hot"] = (64, int(round(feet - px + px * 0.12)))
        k += cnt
    for sp in specs:
        sp.pop("pic", None)
    print("line-up: %d on page %d, rows of %s" % (n, page + 1, "/".join(str(c) for c in counts)))


def with_flight_flag(swt):
    """swt with bit 0 of SpecialMovesets set: the flight move set. The
    engine reads its FlightData only for a character whose .swt says so
    (SwatCreatureType +0x1DC, sub_00097xxx; tested at 0x001436A0 before a
    glide starts and wherever the flight state is looked at): the condor
    cnd and npa have 1, the rest 0 (or 2). With 0, FlightData sat unread and
    Bat Boy never left the ground."""
    m = re.search(rb"(SpecialMovesets[ \t]+)(\d+)", swt)
    if m:
        swt = swt[:m.start(2)] + str(int(m.group(2)) | 1).encode() + swt[m.end(2):]
    # and no fall damage, as the condor: a glide ends 10 m up or more, and
    # landing from it took 40 % of his health (harness)
    for key in (b"FallDmgMaxDamage", b"FallDmgPerMeter"):
        swt = re.sub(rb"(" + key + rb"[ \t]+)[-\d.]+", rb"\g<1>0.000000", swt)
    return swt


def with_flight(msd, fly):
    """msd with the FlightData block put in before its GatlingGunData
    block (at the start of that line, so the indent matches neither -- the
    game's parser reads the blocks by name, not by indent)."""
    m = re.search(rb"\n([ \t]*)GatlingGunData\s*\{", msd)
    if not m:
        return msd
    k = m.start(1)
    return msd[:k] + fly + msd[k:]


def flight_block(template, preview_msd):
    """The retail FlightData block (a condor's, cnd.msd) with the preview
    character's own flight values."""
    i = template.find(b"\tFlightData {")
    j = template.find(b"\n\t}", i) + 3
    block = template[i:j + 2 if template[j:j + 2] == b"\r\n" else j]
    m = re.search(rb"FlightData \{(.*?)ClipData", preview_msd or b"", re.S)
    for k, v in re.findall(rb"(\w+)\s+(-?[\d.]+)", m.group(1) if m else b""):
        block = re.sub(rb"(\n\t\t" + re.escape(k) + rb"\s+)\S+", lambda mm: mm.group(1) + v, block)
    # The climb: the retail engine raises a flier by its launch clip's
    # rise between AscensionSampleFrame[0] and [1] times AscensionScale,
    # for one play of the clip (sub_0025B2xx). The preview's 2.5 was for
    # its own engine; with Bat Boy's clip (0.32 m over those frames) it
    # took him 35 m up in about 1.4 s, far above the camera, which never
    # saw the launch. 0.8: about 11 m, a little over half the rock at
    # Ancient Monolith, as the TGS 2002 trailer shows him.
    block = re.sub(rb"(\n\t\tAscensionScale\s+)\S+", rb"\g<1>0.800000", block)
    return block


def retail_names(game):
    """{code: display name} of the retail characters (swat.strtbl)."""
    data, entries = dave.read(os.path.join(game, "front.dat"))
    for n, do, size, packed in entries:
        if n == "swat.strtbl":
            blob = dave.payload(data, do, size, packed)
            break
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".strtbl")
    tmp.write(blob)
    tmp.close()
    try:
        keys, langs = strtbl.parse(tmp.name)
    finally:
        os.unlink(tmp.name)
    recs = langs[0] if langs else {}
    return {k: recs[strtbl.elf_hash(k)] for k in keys
            if len(k) == 3 and k.isalpha() and strtbl.elf_hash(k) in recs}


def pick_tex_name(n, rget, pfiles, renames):
    """A texture name of n's length that neither game nor preview uses."""
    alpha = "0123456789abcdefghijklmnopqrstuvwxyz"
    used = set(renames.values())
    if len(n) < 3:
        return None
    for k in range(len(alpha) ** 2):
        c = n[:-2] + alpha[k // len(alpha)] + alpha[k % len(alpha)]
        if c != n and c + ".xtex" not in rget and c + ".tex" not in pfiles and c not in used:
            return c
    return None


def pick_code(pc, taken):
    """pc itself when free, else a free 3-letter code close to it."""
    cands = []
    if len(pc) == 3 and pc.isalpha():
        cands.append(pc)
    letters = re.sub(r"[^a-z]", "", pc)
    for first in "qxzvj":
        cands.append(first + (letters + "xx")[1:3])
        cands.append(first + (letters + "xx")[:2])
    for c in cands:
        if c not in taken:
            return c
    import itertools, string
    for t in itertools.product(string.ascii_lowercase, repeat=3):
        c = "".join(t)
        if c not in taken:
            return c


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "fix":
        # an older mods folder: CharWeaponTable back within its 50 rows
        front = os.path.join(a[1], "front.dat")
        swui, n = fit_tables(addchar.get(front, "postermultiview.swui"))
        if n:
            dave.merge(front, front + ".new", {"postermultiview.swui": swui})
            os.replace(front + ".new", front)
        print("%s: %d CharWeaponTable rows taken out" % (front, n))
        return
    if len(a) == 3 and a[0] == "list":
        cmd, swat, game = a
        mods, want = None, None
    elif len(a) >= 4 and a[0] == "add":
        cmd, swat, game, mods = a[:4]
        want = [w.lower() for w in a[4:]] or None
    else:
        print(__doc__)
        sys.exit(1)
    work = tempfile.mkdtemp(prefix="beta_roster_")
    try:
        swat = beta_level.open_preview(swat, work)
        b = os.path.join(work, "b")
        run(os.path.join(HERE, "beta.py"), "extract", swat, b, "*.cr")
        run(os.path.join(HERE, "beta.py"), "extract", swat, os.path.join(work, "ui"), "2000.swui")
        sw = os.path.join(work, "ui", "2000.swui")
        setup = preview_setup(open(sw, "rb").read()) if os.path.exists(sw) else {}
        roster = preview_roster(b)
        names = retail_names(game)
        src_swat = addchar.src_archive(game, mods or game, "swat.dat")
        rdata, rentries = dave.read(src_swat)
        rget = {n: (do, sz, pk) for n, do, sz, pk in rentries}
        taken = set()
        for n in rget:
            m = re.match(r"m?([a-z]{3})\.cr$", n)
            if m:
                taken.add(m.group(1))
        taken |= set(names)
        # Every name any of the game's archives starts with: a code the story
        # uses for someone else (gma, Grandma: gma_lods.xrsc in a level)
        # would load that one's model pack instead.
        for root, _, fs in os.walk(game):
            for f in fs:
                if f.lower().endswith(".dat"):
                    for n in dave.names(os.path.join(root, f)):
                        m = re.match(r"(?:hat_|m)?([a-z]{3})[._]", n.lower())
                        if m:
                            taken.add(m.group(1))
        plan = []
        pmv_front = addchar.get(addchar.src_archive(game, mods or game, "front.dat"), "postermultiview.swui")
        in_roster = set(c.decode() for c in re.findall(rb"<FONTGRIDDATA:y:(\w+):", pmv_front))
        same = {"fchk": "chk", "fggl": "ggl", "fhdh": "hdh", "fugl": "ugl", "ann": "ani", "zz2": "zzt",
                "nrd2": "srd", "mrd": "mrd"}
        for pc, render, skel in roster:
            # Moves, voice and weapons: the retail character it became, when
            # there is one (Chicken's for the preview's Chicken), else Red's.
            src = same.get(pc, pc if pc in in_roster else "srd")
            if src not in in_roster:
                src = "srd"
            if want is not None:
                hit = [w for w in want if w.split("=")[0] == pc]
                if not hit:
                    continue
                if "=" in hit[0]:
                    src = hit[0].split("=")[1]
            code = pick_code(pc, taken)
            taken.add(code)
            base = OWN_NAMES.get(pc) or names.get(same.get(pc, pc)) or names.get(pc[-3:])
            if base and "\n" in base:
                label = base + " '02"
            else:
                label = (base or pc.capitalize()) + "\n(2002)"
            plan.append((pc, render, skel, code, src, label))
        if cmd == "list":
            for pc, render, skel, code, src, label in plan:
                w, nm, ml = abilities(setup, pc, render)
                print("%-6s model %-5s skeleton %-5s -> %s as %s  %-28s %s %s %s" % (
                    pc, render, skel, code, src, label.replace("\n", " "), w, nm or "-", ml or ""))
            print("%d characters" % len(plan))
            return

        # Everything the plan needs from the preview, in one pass each.
        m_dir = os.path.join(work, "m")
        for pat in ("*_lod_0*.mod", "*_face.mod", "*.skel", "*.tex", "*.msd", "*.swt"):
            run(os.path.join(HERE, "beta.py"), "extract", swat, m_dir, pat)
        pfiles = {f.lower(): os.path.join(m_dir, f) for f in os.listdir(m_dir)}
        tex_cache = {}
        condor = dave.payload(rdata, *rget["cnd.msd"]) if "cnd.msd" in rget else None
        # the retail game's sound banks (banks.dat: snd_<n>.hd/.bd, a
        # creature's CreatureSoundSet n): see "voice" below
        try:
            banks = {n.lower() for n, _, _, _ in beta.entries(os.path.join(game, "banks.dat"))}
        except (OSError, SystemExit):
            banks = set()

        def texture(name):
            n = name.lower()
            if n in tex_cache:
                return tex_cache[n]
            im = None
            if n + ".tex" in pfiles:            # the preview's own (see below)
                try:
                    w, h, px = betatex.decode(open(pfiles[n + ".tex"], "rb").read())
                    im = Image.new("RGBA", (w, h))
                    im.putdata(px)
                except Exception:
                    im = None
            if im is None and n + ".xtex" in rget:
                blob = dave.payload(rdata, *rget[n + ".xtex"])
                p = os.path.join(work, "t.xtex")
                open(p, "wb").write(blob)
                try:
                    run(os.path.join(HERE, "xtex.py"), "export", p, os.path.join(work, "t.dds"))
                    im = Image.open(os.path.join(work, "t.dds"))
                    im.load()
                except Exception:
                    im = None
            tex_cache[n] = im
            return im

        specs, strings, skipped, weapons, fliers = [], {}, [], {}, []
        renames = {}
        for pc, render, skel, code, src, label in plan:
            files, texnames = {}, set()
            skel_text = None
            if skel + ".skel" in pfiles:
                skel_text = open(pfiles[skel + ".skel"], "r", errors="replace").read()
                files[code + ".skel"] = open(pfiles[skel + ".skel"], "rb").read()
                # numbered as the model numbers its bones (the long-coat
                # Red's cloth anchors are not; mod212.fit_skeleton)
                if render + "_lod_01.mod" in pfiles:
                    fitted = mod212.fit_skeleton(skel_text, open(pfiles[render + "_lod_01.mod"], "rb").read())
                    if fitted != skel_text:
                        print("  %s: skeleton without its cloth anchors" % pc)
                        skel_text = fitted
                        files[code + ".skel"] = fitted.encode("latin-1", "replace")
            if skel_text is None:
                skipped.append((pc, "no skeleton " + skel))
                continue
            model = None
            # Bones out of the retail order: the skeleton rebuilt as the
            # retail 52 and the body models moved onto it (mod212.py,
            # std_skeleton) -- when every body model is one it can move.
            # Every one onto a retail skeleton (mod212.onto_skeleton): the
            # retail format names each bone's channels, where the preview's
            # hands them out by a bone's place -- with it O'Grady and Old
            # flew apart aiming a rifle and Young and Teen Red floated (user
            # reports). Which: the retail one of the same name (Young Red's
            # is the preview's, proportions and all), else the copied
            # character's (its animations are laid out for it -- Christina's,
            # hair and all), else Red's.
            if skel + ".skel" in rget:
                target = skel
            elif src + ".skel" in rget:
                target = src
            else:
                target = "srd"
            std = None
            same_places = False
            if target + ".skel" in rget:
                ttext = dave.payload(rdata, *rget[target + ".skel"]).decode("latin-1")
                std = mod212.onto_skeleton(skel_text, ttext)
                # A model in an older format cannot be moved onto other
                # bones; the retail skeleton still serves when every bone
                # keeps its place: each named bone where it was, an extra
                # (Old's coat tail) where the retail one has a bone hung
                # from the same parent (Red's tail_root).
                pb, _ = mod212.skel_bones(skel_text)
                tb, _ = mod212.skel_bones(ttext)
                tn = [b[0] for b in tb]
                same_places = all(
                    (i < len(tb) and tn[i] == n) if n in tn else
                    (i < len(tb) and tn[i] not in [b[0] for b in pb] and tb[i][1] == pp)
                    for i, (n, pp, _o) in enumerate(pb))
            if std:
                print("  %s: onto the retail %s skeleton" % (pc, target))
            else:
                std = mod212.std_skeleton(skel_text)
            plain = {}
            plain_ok = False
            lod1_height = None
            for hat in ("", "hat_"):
                for lod in ("1", "2"):
                    key = "%s%s_lod_0%s.mod" % (hat, render, lod)
                    if key not in pfiles:
                        continue
                    data = open(pfiles[key], "rb").read()
                    tmp = os.path.join(work, "u.mod")
                    open(tmp, "wb").write(data)
                    beta_level.upgrade_109(tmp)
                    data = open(tmp, "rb").read()
                    # A hat is drawn on the head, its vertices relative to it
                    # (as the retail hats): moved by a bone it floated over
                    # the character (seen: O'Grady's, a metre up).
                    if mod212.is_212(data) and not hat:
                        try:
                            if lod == "1":
                                data = mod212.to_model_space(data, skel_text)
                                lod1_height = mod212.model_height(data)
                            else:
                                data = mod212.lod_to_model_space(data, skel_text, lod1_height)
                        except (ValueError, IndexError, struct.error) as e:
                            if not hat:
                                print("  %s: %s %s" % (pc, key, e))
                            continue
                    name = "%s%s_lod_0%s.xmod" % (hat, code, lod)
                    if std and not hat:
                        plain[name] = data
                        if mod212.is_212(data):
                            data = mod212.remap_bones(data, std[1], std[2])
                        elif same_places:
                            plain_ok = True             # left as it is: bones in place
                        else:
                            std = None
                    files[name] = data
                    texnames |= set(t.decode().lower() for t in
                                    re.findall(rb"([A-Za-z][A-Za-z0-9_\-]{2,40})", data))
                    if not hat and lod == "1":
                        model = data
            if model is None:
                skipped.append((pc, "model %s not convertible" % render))
                continue
            if std and plain_ok:
                # the bones keep their places: the models as they were
                files.update(plain)
                model = files.get(code + "_lod_01.xmod", model)
            if std:
                files[code + ".skel"] = std[0].encode("latin-1")
                print("  %s: skeleton rebuilt (%d bones)" % (pc, std[0].count("bone ")))
            else:
                files.update(plain)                 # as they were
                model = files.get(code + "_lod_01.xmod", model)
            if code + "_lod_02.xmod" not in files:
                files[code + "_lod_02.xmod"] = model
            # A separate face (cls, fchk, fggl, fugl, wsk, fac): the head
            # is drawn by it -- left out, the head had no texture or no
            # face (user report: O'Grady, Chicken). Same 2.12 format as the
            # retail game's <name>_face.xmod, placed by the .cr's FaceOffset.
            face_offset = None
            if render + "_face.mod" in pfiles:
                tmp = os.path.join(work, "u.mod")
                open(tmp, "wb").write(open(pfiles[render + "_face.mod"], "rb").read())
                beta_level.upgrade_109(tmp)
                data = open(tmp, "rb").read()
                if data.startswith(b"version: 2."):
                    files[code + "_face.xmod"] = data
                    texnames |= set(t.decode().lower() for t in
                                    re.findall(rb"([A-Za-z][A-Za-z0-9_\-]{2,40})", data))
                    pcr = os.path.join(b, pc + ".cr")
                    if not os.path.exists(pcr):
                        pcr = next((os.path.join(b, f) for f in os.listdir(b) if f.lower() == pc + ".cr"), pcr)
                    if os.path.exists(pcr):
                        mo = re.search(rb"FaceOffset[ \t]+([-0-9.eE]+)[ \t]+([-0-9.eE]+)[ \t]+([-0-9.eE]+)",
                                       open(pcr, "rb").read())
                        if mo:
                            face_offset = b" ".join(mo.groups())
            # Textures: always the preview's own pictures. Many of their
            # names are in the retail game with other pictures -- the same
            # character redrawn, its parts laid out elsewhere (Navarro's
            # zzt02) -- and the preview's models map their own layout, so a
            # retail texture of the same name dressed them in scraps (user
            # report). Such a name is renamed, the same length, in the
            # models (one new name per texture, shared by every character
            # wearing it).
            for n in sorted(texnames):
                if n + ".tex" not in pfiles:
                    continue
                name = n
                if n + ".xtex" in rget:
                    name = renames.get(n) or pick_tex_name(n, rget, pfiles, renames)
                    if not name:
                        print("  %s: texture %s keeps the retail picture (no free name)" % (pc, n))
                        continue
                    renames[n] = name
                    pat = re.compile(rb"(?<![A-Za-z0-9_\-])" + re.escape(n.encode())
                                     + rb"(?![A-Za-z0-9_\-])", re.I)
                    for k in list(files):
                        if k.endswith(".xmod"):
                            files[k] = pat.sub(name.encode(), files[k])
                out = os.path.join(work, name + ".xtex")
                try:
                    run(os.path.join(HERE, "betatex.py"), "--solid", pfiles[n + ".tex"], out)
                    files[name + ".xtex"] = open(out, "rb").read()
                except subprocess.CalledProcessError:
                    pass
            front = {}
            pic = None
            try:
                # (Old's is the older 2.00 format: user, his line-up
                # picture and portrait were Red's)
                mm = mod212.parse(model) if mod212.is_212(model) else mod212.parse200(model)
                if code + "_face.xmod" in files:
                    # its separate face drawn on the head, as the game does
                    sk = files.get(code + ".skel")
                    sk = sk.decode("latin-1") if isinstance(sk, bytes) else (sk or skel_text)
                    at = charpic.head_place(sk)
                    if at and face_offset:
                        at = tuple(a + float(b) for a, b in zip(at, face_offset.split()))
                    mm = charpic.with_face(mm, mod212.parse(files[code + "_face.xmod"]), at)
                tex = charpic.textures_for(mm, texture)
                front["mp_%s.xtex" % code], _ = charpic.portrait_xtex(mm, tex)
                # the cut-out waits for the line-up's layout (its row and
                # size): see lay_out below
                pic = (mm, tex, charpic.model_height(mm))
            except Exception as e:
                print("  %s: pictures not drawn (%s); the copied character's are used" % (pc, e))

            def cr_edit(cr, code=code, face=code + "_face.xmod" in files, face_offset=face_offset):
                cr = re.sub(rb"FileNameRender\s+\w+", b"FileNameRender  " + code.encode(), cr)
                cr = re.sub(rb"FileNameSkel\s+\w+", b"FileNameSkel  " + code.encode(), cr)
                if not face:
                    return re.sub(rb"(\n\s*)Face\s+[01]", rb"\1Face  0", cr)
                cr = re.sub(rb"(\n\s*)Face\s+0", rb"\1Face  1", cr)
                if face_offset:
                    cr = re.sub(rb"FaceOffset[ \t]+[-0-9.eE]+[ \t]+[-0-9.eE]+[ \t]+[-0-9.eE]+",
                                lambda m: b"FaceOffset  " + face_offset, cr)
                return cr
            # Standing on the ground: the animations carry the root at one
            # height for every character, and each .swt's OffsetY lowers
            # (or raises) the body by its own skeleton's root height from it
            # (retail: OffsetY ~ root - 1.12 throughout, Young Red -0.265,
            # Red -0.037). Copied from Red, Young and Teen Red and Old kept
            # Red's and floated a quarter of a metre (user report). The
            # values now come from the retail character of the skeleton's
            # name (its shadow model too), else the one copied, moved by
            # how far this skeleton's root is from that one's.
            ref = skel if skel + ".swt" in rget else src
            stand = None
            try:
                new_root = root_height(files[code + ".skel"].decode("latin-1"))
                rs = ref if ref + ".skel" in rget else "srd"
                ref_root = root_height(dave.payload(rdata, *rget[rs + ".skel"]).decode("latin-1"))
                if new_root and ref_root:
                    stand = (ref, new_root, ref_root,
                             {pre: dave.payload(rdata, *rget[pre + ref + ".swt"])
                              for pre in ("", "m") if pre + ref + ".swt" in rget})
                    if ref != src or abs(new_root - ref_root) > 0.005:
                        print("  %s: root %.3f, standing as %s (root %.3f)" % (pc, new_root, ref, ref_root))
            except (KeyError, ValueError, IndexError):
                stand = None
            weapon, normal, melee = abilities(setup, pc, render)
            # Its own voice. A character's .swt names its sound bank
            # (CreatureSoundSet n: banks.dat snd_<n>.hd/.bd, PS2 HD/BD banks
            # of VAG sounds). The preview's named characters have their own,
            # 101-121 (Bat Boy 106, Pig Josh 107, O'Grady 120 ...; its disc's
            # loose voice clips NNv..vag are those sets' sounds), and the
            # retail banks.dat still holds every one of them -- the retail
            # characters were renumbered (O'Grady 153, Pig Josh 151) and
            # Ugly Chris kept 115. The copied retail character's number gave
            # them its voice (Red's, 6, for most: user, "import any sounds the
            # characters make in the preview"). Where the preview's bank is on
            # the disc it is used; the others (0, the preview had none) keep
            # the copied one's.
            voice = None
            ps = pfiles.get(pc + ".swt") or pfiles.get(render + ".swt")
            if ps:
                mv = re.search(rb"CreatureSoundSet\s+(\d+)", open(ps, "rb").read())
                if mv and int(mv.group(1)) > 0 and "snd_%d.hd" % int(mv.group(1)) in banks:
                    voice = int(mv.group(1))
            fly = None
            if "spmove_glide" in (normal, melee) or "spmove_glideattack" in (normal, melee):
                if condor:
                    pm = pfiles.get(pc + ".msd") or pfiles.get(render + ".msd")
                    fly = flight_block(condor, open(pm, "rb").read() if pm else None)

            def edit(n, b, code=code, weapon=weapon, normal=normal, melee=melee, fly=fly, stand=stand,
                     voice=voice):
                if n in ("m%s.ars" % code, "%s.ars" % code):
                    if weapon in THROWN:
                        return b"NumTypes 2\r\nSix_Shooter\t\t\t0\r\n%s\t\t\t\t0" % weapon.encode()
                    return b"NumTypes 1\r\n%s\t\t\t0" % weapon.encode()
                if n in ("m%s.swt" % code, "%s.swt" % code):
                    # the preview's special moves, none where it gave none
                    # (not the retail character's it is copied from)
                    for key, v in ((b"NormalSpecialMove", normal), (b"MeleeSpecialMove", melee)):
                        v = (v or "spmove_none").encode()
                        b = re.sub(rb"(" + key + rb"\s+)\S+", lambda m, v=v: m.group(1) + v, b)
                    if fly:
                        b = with_flight_flag(b)
                    if voice:
                        b = re.sub(rb"(CreatureSoundSet[ \t]+)\d+", lambda m: m.group(1) + str(voice).encode(), b)
                    if stand:
                        b = stand_on_ground(b, n.startswith("m"), *stand)
                    return b
                if fly and n in ("m%s.msd" % code, "%s.msd" % code) and b"FlightData" not in b:
                    # before GatlingGunData, however the file indents it:
                    # the Showdown copies use tabs, the story ones two
                    # spaces (a tab-only search left the story copy -- the
                    # one Play As uses -- without its flight)
                    return with_flight(b, fly)
                if fly and n in ("m%s.rep" % code, "%s.rep" % code):
                    # the glide's own animations (still in the retail game,
                    # its preview flier's: scb_for_gld ...)
                    for slot in (b"_for_gld_ant", b"_for_gld", b"_for_chg"):
                        b = re.sub(rb"(\n" + slot + rb"\t)\S+", lambda m, slot=slot: m.group(1) + b"scb" + slot, b)
                    return b
                return b
            specs.append(dict(code=code, src=src, extra=files, cr_edit=cr_edit, front=front, edit=edit, pic=pic))
            strings[code] = label
            if weapon != WEAPONS[0]:
                weapons[code] = weapon          # the pistol: the default row
            if fly:
                fliers.append(code)
            print("%s -> %s (%s)  %s, %s%s%s%s" % (pc, code, label.replace("\n", " "), weapon, normal or "-",
                                               ", melee " + melee if melee else "", ", flies" if fly else "",
                                               ", voice %d" % voice if voice else ""))
        del rdata
        for pc, why in skipped:
            print("  %s left out: %s" % (pc, why))
        if not specs:
            sys.exit("nothing to add")
        lay_out(specs)

        def with_names(files, archive):
            data, entries = dave.read(archive)
            for n, do, size, packed in entries:
                if n == "swat.strtbl":
                    base = files.get(n) or dave.payload(data, do, size, packed)
                    files[n] = strtbl.add(base, strings)
            pmv = files.get("postermultiview.swui")
            if pmv is not None:
                # Showdown's starting weapons: CharWeaponTable (else the
                # "default" row, Red's Widowmaker). Its LOOKUP table holds
                # 50 rows (sub_00019C20's 50 x 50 cells; more ran past it
                # and crashed the front end, sub_000274A0), 49 used. Rows
                # that only repeat the default are taken out to make room,
                # and a preview character whose weapon is the pistol gets
                # the default; the others in WEAPON_ORDER while rows last.
                pmv = table_rows(pmv, b"CharWeaponTable", weapon_rows(weapons))
                # a flier played by the computer flies: the retail game's
                # own Bat Boy brain (batboy.tbl: circles, dives at you)
                pmv = table_rows(pmv, b"AIBrainTable",
                                 [(c, b"batboy") for c in fliers], drop_default=False)
                files["postermultiview.swui"] = pmv
        addchar.add_many(game, mods, specs, front_edit=with_names, swat_edit=with_names)
        print("%d characters added" % len(specs))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
