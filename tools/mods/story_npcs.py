#!/usr/bin/env python3
"""
story_npcs.py - the story's own people as Showdown characters: Brimstone's
townsfolk and shopkeepers, the train's passengers and robber, soldiers,
guards, braves... everyone the retail line-up never offers. EXPERIMENTAL.

  python tools\\mods\\story_npcs.py list <game folder>
  python tools\\mods\\story_npcs.py add  <game folder> <mods folder> [npc ...]

`add` puts them on the line-up's third page when the preview's characters
(tools\\mods\\beta_roster.py) stand on the second, else on the second, in three rows as the
retail page stands, into <mods folder> (front.dat and swat.dat: made from
the game's, or added to the ones already there -- run it after
beta_roster.py on the same folder to have both).

Each one is the story character as the game has it in swat.dat: its own
.cr (model, skeleton, face), .dam, .sphs and .hot, under a new code (w..),
as the Showdown version (m<code>.*) and the story one; its moves Red's
(.rep, .msd, and his .swt but for name, voice, hat and height: KEEP_RED),
the files it has none of, and its weapons and unlock row, from Red. Its
line-up cut-out and player-box portrait are drawn from its model and its
face (charpic.py). Names: the game's own where it has one (the string tables,
the cutscenes' cast lists), else what it is (Barber, Townswoman ...).
Models still in the old text format (Sheriff, Young Red's story double,
one bandit) are left out.
"""
import contextlib, io, os, re, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import addchar, beta_roster, charpic, dave, mod212, strtbl, xtex

# (the game's code, the name on the line-up). None: the game's own name
# (swat.strtbl), which every one listed with None has.
NPCS = [
    # Brimstone
    ("own01", "Gunsmith"), ("own02", "Banker"), ("own03", "Realtor"),
    ("own04", "Shopkeeper"), ("own05", "Barber"), ("own06", "Fitch"),
    ("own07", "Barman"), ("bma", "Barmaid"), ("jdy", "Jody"),
    ("np1", "Townsman"), ("np2", "Townsman"), ("np3", "Pedro"),
    ("npa", "Townswoman"), ("npb", "Townswoman"),
    ("pdr", None), ("bt1", "Bartender"),
    # the train (Wilbur: the one you duel in the passenger car, brb in
    # lvl_traincar's .enm; tdv is the "conductor" of lvl_traingood's --
    # named the train robber before: user)
    ("brb", "Wilbur"), ("htj", None), ("tdv", "Conductor"),
    # soldiers, braves, bandits
    ("bsd", None), ("sdg", None), ("sfv", None), ("sdh", "Eli"),
    ("sdt", "Soldier"), ("gsg", "Guard"), ("in1", "Brave"), ("in4", "Chief"),
    ("amg", None), ("nod", "Old Timer"),
    # and the rest with a name in the cutscenes or the tables
    ("scd", None), ("pld", None), ("snp", None), ("frj", "George"),
    ("gip", "Larouche"), ("pck", "Ozwald"), ("kya", "Jesse"),
    ("ct2", "Ashley"),
    # the ranch's hands, Dirk and Gilbert (lvl_ranch's .enm: Type rhd, one
    # model for both; user: "add Dirk and Gilbert")
    ("rhd", "Dirk"), ("rhd", "Gilbert"),
    # Professor Perry's strongman (lvl_shernite's .enm: Type rok, Name Atlas;
    # his .swt calls him Granite)
    ("rok", "Atlas"),
    # and more, now that the line-up holds more than 144 (the game keeps the
    # rest of a page's elements aside: rdr_hooks.c, sub_00019EF0). No name
    # of their own in the game's tables: what they are. (Left out: nkd, a
    # body for clothes; the second takes of characters already here --
    # bsd2, dg2, ky2, scd2, sev1-3, tt2, tw7, zz2, ywn, kt3.)
    ("bt2", "Bartender"), ("np4", "Townsman"), ("dcg", "Katie O'Grady (1st Encounter)"),
    ("kt2", "Katie O'Grady (2nd Encounter)"), ("jes", "Jesse Lynch"), ("brc", "Showman"),
    ("tca", "Trapper"), ("gdn", "Ghost"), ("in2", "Brave"), ("in3", "Brave"),
    ("rsd", "Officer"), ("dar", "Colonel Darren"), ("nsd", "Soldier"),
    ("dvy", "Griffin"), ("gsh", "Bandit"), ("mrd2", "Young Red (Bloody Hand)"), ("sn2", "Bandit"),
    ("gsw", "Townsman"), ("sdw", "Townsman"),
]

# The line-up page held at most 150 elements (its list at +0x40, the
# count at +0x298: sub_00019EF0 added past the end -- a crash loading the
# poster with 158 characters): its photo halves, paper and group, and one
# marker a character. The game now keeps the rest aside (rdr_hooks.c);
# the line-up's own pages run to 8 (LU_MAXPG): 48 retail, 55 preview, these.
MAX_MARKERS = 400
PER_PAGE = 42

SRC = "srd"     # the files a character has none of, weapons and unlock row: Red's

# Red's, not the story character's own: its weapons (.ars), its skeleton by
# name (.skel), and how it moves -- the .rep (each move's animation) and
# .msd (the move set). A townsman's .rep names the townsfolk's animations
# and gaits (bty_*, ani_dck_for_wlk ...), which are in the story levels'
# archives only: in Showdown the first one was missing, and the match
# crashed loading the character (user report: four of them; reproduced
# with the Shopkeeper, the gait set at 0x0021C770 calling through a wild
# pointer). Red's moves are a Showdown player's anyway (the duel's
# draw, the dives, the specials).
KEEP_RED = ("ars", "skel", "rep", "msd")
# What of its .swt is its own: the rest (dodges, vaults, aim) is Red's.
SWT_OWN = (b"DefaultName", b"Male", b"CreatureSoundSet", b"BaseSkeleton", b"Hat", b"OffsetY",
           b"StepType")


def split_name(s):
    """Two lines for a two-word name, as the retail line-up writes them; a
    name with a note in brackets has the note on the second line."""
    if "\n" in s or " " not in s:
        return s
    if " (" in s:
        a, b = s.split(" (", 1)
        return a + "\n(" + b
    a, b = s.split(" ", 1)
    return a + "\n" + b


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "list":
        cmd, game = a
        mods, want = None, None
    elif len(a) >= 3 and a[0] == "add":
        cmd, game, mods = a[:3]
        want = [w.lower() for w in a[3:]] or None
    else:
        print(__doc__)
        sys.exit(1)
    swat = addchar.src_archive(game, mods, "swat.dat") if mods else os.path.join(game, "swat.dat")
    front = addchar.src_archive(game, mods, "front.dat") if mods else os.path.join(game, "front.dat")
    data, entries = dave.read(swat)
    get = {n: (do, size, packed) for n, do, size, packed in entries}
    fnames = {n for n, _, _, _ in dave.read(front)[1]}

    def payload(n):
        return dave.payload(data, *get[n])

    pmv = addchar.get(front, "postermultiview.swui")
    roster = set(c.decode() for c in re.findall(rb"<FONTGRIDDATA:y:(\w+):", pmv))
    names = beta_roster.retail_names(game)

    work = tempfile.mkdtemp(prefix="story_npcs_")

    def texture(name):
        n = name.lower() + ".xtex"
        if n not in get:
            return None
        b = payload(n)
        if struct.unpack_from("<3H", b, 0)[2] not in xtex.FMT:
            return None
        src, dds = os.path.join(work, "t.xtex"), os.path.join(work, "t.dds")
        open(src, "wb").write(b)
        with contextlib.redirect_stdout(io.StringIO()):
            xtex.cmd_export(src, dds)
        from PIL import Image
        return Image.open(dds).convert("RGBA")

    taken = set()

    def pick_code(npc):
        """w + two letters, used by nothing in either archive."""
        letters = [ch for ch in npc if ch.isalpha()] + list("abcdefghijklmnopqrstuvwxyz")
        for x in letters:
            for y in "abcdefghijklmnopqrstuvwxyz":
                c = "w" + x + y
                if c in taken or c in roster:
                    continue
                if any(n.startswith((c + ".", c + "_", "m" + c + ".")) for n in get) or \
                        any(n.startswith(("ui_mphoto_" + c, "mp_" + c + ".")) for n in fnames):
                    continue
                taken.add(c)
                return c
        return None

    lineup = addchar.get(front, "characterlineup.swui")
    room = MAX_MARKERS - lineup.count(b"<POSMARKER:")
    specs, strings = [], {}
    try:
        for npc, label in NPCS:
            if want and npc not in want:
                continue
            if cmd == "add" and len(specs) >= room:
                print("  %s and after: no room on the line-up (%d markers at most)" % (npc, MAX_MARKERS))
                break
            if npc + ".cr" not in get:
                print("  %s: no %s.cr in swat.dat" % (npc, npc))
                continue
            cr = payload(npc + ".cr").decode("latin-1")
            render = re.search(r"FileNameRender\s+(\S+)", cr).group(1)
            model = render + "_lod_01.xmod"
            if model not in get:
                print("  %s: no %s" % (npc, model))
                continue
            try:
                mm = mod212.parse(payload(model))
            except AssertionError:
                print("  %s: %s is in the old text format, left out" % (npc, model))
                continue
            # the face, where it is a model of its own (most of them): drawn
            # on the head for the pictures, as the game draws it
            sk = re.search(r"FileNameSkel\s+(\S+)", cr)
            if render + "_face.xmod" in get and sk and sk.group(1) + ".skel" in get:
                try:
                    face = mod212.parse(payload(render + "_face.xmod"))
                    at = charpic.head_place(payload(sk.group(1) + ".skel").decode("latin-1"), cr)
                    mm = charpic.with_face(mm, face, at)
                except (AssertionError, ValueError, IndexError, struct.error) as e:
                    print("  %s: face not drawn (%s)" % (npc, e))
            label = label or names.get(npc)
            if not label:
                print("  %s: no name" % npc)
                continue
            label = split_name(label)
            if cmd == "list":
                print("%-6s model %-6s as %s" % (npc, render, label.replace("\n", " ")))
                continue
            code = pick_code(npc)
            if not code:
                sys.exit("no free code for " + npc)
            tex = charpic.textures_for(mm, texture)
            front_files = {"mp_%s.xtex" % code: charpic.portrait_xtex(mm, tex)[0]}
            own = {}
            for n in get:
                m = re.fullmatch(re.escape(npc) + r"\.(\w+)", n)
                if m and m.group(1) not in ("cr_", "xtex", "tbl"):
                    own[m.group(1)] = n
            extra = {}
            src_ext = {n.split(".", 1)[1] for n in get if n.startswith(SRC + ".")}
            for ext, n in own.items():
                if ext not in src_ext and ext not in ("skel",):
                    b = payload(n)
                    extra["m%s.%s" % (code, ext)] = b
                    extra["%s.%s" % (code, ext)] = b

            def edit(n, b, code=code, own=own):
                m = re.fullmatch(r"m?" + code + r"\.(\w+)", n)
                if not m or m.group(1) not in own or m.group(1) in KEEP_RED:
                    return b                    # Red's: see KEEP_RED
                mine = payload(own[m.group(1)])
                ext = m.group(1)
                if ext == "swt":
                    # Red's, as the moves are; who it is, its own
                    for key in SWT_OWN:
                        mo = re.search(rb"\n([ \t]*)" + key + rb"[ \t]+([^\r\n]*)", mine)
                        if mo:
                            b = re.sub(rb"(\n[ \t]*" + key + rb"[ \t]+)[^\r\n]*",
                                       lambda _m, v=mo.group(2): _m.group(1) + v, b, count=1)
                    mine = b
                if ext == "cr":
                    # Showdown's hit points (a townsman has few): Red's
                    hp = re.search(rb"MaxHitPoints[ \t]+[-\d.]+", b)
                    if hp:
                        mine = re.sub(rb"MaxHitPoints[ \t]+[-\d.]+", lambda _m: hp.group(0), mine)
                if ext == "swt" and n.startswith("m"):
                    # as the retail Showdown versions differ from the story
                    # ones (mcls / cls): fists that hurt, no edge model
                    mine = re.sub(rb"(PunchDamage[ \t]+)[-\d.]+", rb"\g<1>20.000000", mine)
                    mine = re.sub(rb"(EdgeModel[ \t]+)\S+", rb"\g<1>NoEm", mine)
                return mine

            specs.append(dict(code=code, src=SRC, extra=extra, front=front_files, edit=edit,
                              pic=(mm, tex, charpic.model_height(mm))))
            strings[code] = label
            print("%s -> %s (%s)" % (npc, code, label.replace("\n", " ")))
        if cmd == "list" or not specs:
            return
        # the third page when the preview's characters are on the second
        # (their front.dat already there), else the second
        page = 2 if os.path.isfile(os.path.join(mods, "front.dat")) else 1
        # no more than PER_PAGE to a page (59 on one stood too close to pick)
        for i in range(0, len(specs), PER_PAGE):
            beta_roster.lay_out(specs[i:i + PER_PAGE], page=page + i // PER_PAGE)

        def with_names(files, archive):
            d, ents = dave.read(archive)
            for n, do, size, packed in ents:
                if n == "swat.strtbl":
                    base = files.get(n) or dave.payload(d, do, size, packed)
                    files[n] = strtbl.add(base, strings)

        addchar.add_many(game, mods, specs, front_edit=with_names, swat_edit=with_names)
        print("%d characters added" % len(specs))
    finally:
        import shutil
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
