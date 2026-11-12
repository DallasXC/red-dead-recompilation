#!/usr/bin/env python3
"""
beta_char.py - a character of the January 2002 preview (PS2) as a
new Showdown character, in its preview look.
EXPERIMENTAL.

  python tools\\mods\\beta_char.py list <preview .iso or SWAT.ZIP>
  python tools\\mods\\beta_char.py add  <preview .iso or SWAT.ZIP> <game folder> <mods folder> <code> <preview character> [copy of] [x]

  code               the new character's code, 3 letters, not one the game uses
  preview character  the preview's code (red = the preview's Red; "list")
  copy of            the retail character whose moves, voice, weapons and
                     the rest it gets (default srd, Red)
  x                  where on the line-up it stands (default the next free
                     place on the second page; see addchar.py)

e.g.
  python tools\\mods\\beta_char.py add "..\\PS2 Beta\\preview.iso" "..\\Red Dead Revolver (USA)" mods brd red

It is addchar.py (a copy of the retail character under the new code) plus
the preview's models: <preview>_lod_01/02.mod and hat_<preview>_lod_01/02
become <code>_lod_0N.xmod and hat_<code>_lod_0N.xmod (the preview's
"2.1x" models keep each vertex relative to its bone: the copy of the same
mesh the retail disc still carries in the older text format when there is
one, else the model moved into the model's space with the skeleton,
mod212.py), the preview's
skeleton <code>.skel, and m<code>.cr names them (FileNameRender,
FileNameSkel; no separate face model, as in the preview). Textures the
models name are the preview's own, converted (betatex.py); a name the
retail game also has (often another picture: the same character redrawn)
is renamed in the models.

Characters normally load their models from a resource pack
(<name>_lods.xrsc); one made this way has none, and the game loads its
.xmod files instead -- that needs the recompiled game from this build on
(src/rdr_hooks.c, "Character models from .xmod files").
"""
import os, re, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
sys.path.insert(0, HERE)
import addchar, beta_level, dave, mod212


def counts(data):
    """(tex1s, adjuncts, primitives) of a model, text or binary."""
    if data[:13] in (b"version: 2.12", b"version: 2.00") or data[9:11] == b"2." and b"\0" in data[:16]:
        try:
            h = struct.unpack_from("<11I", data, 14)
            return (h[3], h[7], h[8])
        except struct.error:
            return None
    t = data[:600].decode("latin-1")
    v = dict(re.findall(r"(\w+):\s*(\d+)", t))
    try:
        return (int(v["tex1s"]), int(v["adjuncts"]), int(v["primitives"]))
    except KeyError:
        return None


def run(*a):
    subprocess.run([PY] + list(a), check=True, stdout=subprocess.DEVNULL)


def preview_chars(swat):
    out = subprocess.run([PY, os.path.join(HERE, "beta.py"), "list", swat],
                         check=True, capture_output=True, text=True).stdout
    names = set(n.lower() for n in re.findall(r"\b([A-Za-z0-9]+)_lod_01\.mod\b", out))
    crs = set(n.lower() for n in re.findall(r"\b([A-Za-z0-9]+)\.cr\b", out))
    return sorted(names & crs)


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "list":
        work = tempfile.mkdtemp(prefix="beta_char_")
        try:
            print(" ".join(preview_chars(beta_level.open_preview(a[1], work))))
        finally:
            shutil.rmtree(work, ignore_errors=True)
        return
    if not (6 <= len(a) <= 8 and a[0] == "add"):
        print(__doc__)
        sys.exit(1)
    swat, game, mods, code, pc = a[1:6]
    code, pc = code.lower(), pc.lower()
    src = a[6].lower() if len(a) > 6 else "srd"
    x = a[7] if len(a) > 7 else None
    work = tempfile.mkdtemp(prefix="beta_char_")
    try:
        swat = beta_level.open_preview(swat, work)
        b = os.path.join(work, "b")
        for pat in (pc + "_lod_0*.mod", "hat_" + pc + "_lod_0*.mod", pc + ".skel"):
            run(os.path.join(HERE, "beta.py"), "extract", swat, b, pat)
        if not os.path.isdir(b):
            sys.exit("the preview has no %s_lod_01.mod (try 'list')" % pc)
        files = {}
        texnames = set()
        retail = addchar.src_archive(game, mods, "swat.dat")
        rdata, entries = dave.read(retail)
        rget = {n: (do, sz, pk) for n, do, sz, pk in entries}
        skel_text = None
        for f in os.listdir(b):
            if f.lower() == pc + ".skel":
                skel_text = open(os.path.join(b, f), "r", errors="replace").read()
        for f in os.listdir(b):
            low = f.lower()
            p = os.path.join(b, f)
            m = re.match(r"(hat_)?%s_lod_0([12])\.mod$" % re.escape(pc), low)
            if m:
                beta_level.upgrade_109(p)
                data = open(p, "rb").read()
                if data.startswith(b"version: 2.1") and not m.group(1):   # hats: on the head, as they are
                    # The preview's 2.1x models keep each vertex relative to
                    # its bone; the retail loader wants them in the model's
                    # space (drawn as they are, the body is a tangle). The
                    # retail disc still carries several of the preview's
                    # characters in the older text format, which it reads
                    # right: the same mesh when the counts agree.
                    rn = low[:-4] + ".xmod"
                    rd = dave.payload(rdata, *rget[rn]) if rn in rget else None
                    if rd is not None and counts(rd) == counts(data):
                        print("  %s: the retail disc's copy of the same mesh" % low)
                        data = rd
                    else:
                        # Moved into the model's space with the skeleton's
                        # bind positions (mod212.py).
                        try:
                            if skel_text is None or not mod212.is_212(data):
                                raise ValueError("no skeleton" if skel_text is None else "not 2.12")
                            data = mod212.to_model_space(data, skel_text)
                            print("  %s: moved into the model's space" % low)
                        except (ValueError, IndexError, struct.error) as e:
                            if m.group(2) == "1" and not m.group(1):
                                sys.exit("%s: a 2.1x model this tool cannot convert (%s)" % (f, e))
                            print("  (%s left out: %s)" % (f, e))
                            continue
                files["%s%s_lod_0%s.xmod" % (m.group(1) or "", code, m.group(2))] = data
                texnames |= set(t.decode().lower() for t in re.findall(rb"([A-Za-z][A-Za-z0-9_\-]{2,40})", data))
            elif low == pc + ".skel":
                files[code + ".skel"] = open(p, "rb").read()
        if code + "_lod_01.xmod" not in files:
            sys.exit("the preview has no %s_lod_01.mod (try 'list')" % pc)
        # textures: always the preview's own pictures. The retail game has
        # many of the same names with other pictures in them -- the same
        # character redrawn, parts laid out elsewhere (Navarro's zzt02) --
        # and the preview's models map their own layout: a retail texture
        # of the same name dressed them in scraps (user report). A name the
        # retail archive has is renamed, the same length, in this
        # character's models; a renamed picture already added (another
        # preview character wearing it) is used again.
        have = set(rget)
        t = os.path.join(work, "t")
        run(os.path.join(HERE, "beta.py"), "extract", swat, t, "*.tex")
        ptex = {f.lower()[:-4]: os.path.join(t, f) for f in os.listdir(t) if f.lower().endswith(".tex")}
        conv = renamed = 0
        for n in sorted(texnames):
            if n not in ptex:
                continue
            out = os.path.join(work, n + ".xtex")
            try:
                run(os.path.join(HERE, "betatex.py"), ptex[n], out)
            except subprocess.CalledProcessError:
                print("  (texture %s not converted)" % n)
                continue
            body = open(out, "rb").read()
            name = n
            if n + ".xtex" in have and dave.payload(rdata, *rget[n + ".xtex"]) == body:
                continue                            # added before, by another
            if n + ".xtex" in have:
                name = None
                alpha = "0123456789abcdefghijklmnopqrstuvwxyz"
                if len(n) >= 3:
                    for k in range(len(alpha) ** 2):
                        cand = n[:-2] + alpha[k // len(alpha)] + alpha[k % len(alpha)]
                        if cand == n or cand in ptex or cand + ".xtex" in files:
                            continue                # (taken: by this character's other pictures)
                        if cand + ".xtex" in have:
                            if dave.payload(rdata, *rget[cand + ".xtex"]) == body:
                                name = cand         # added before: the same picture
                                break
                            continue
                        name = cand
                        break
                if name is None:
                    print("  (texture %s: no free name of its length; the retail one stays)" % n)
                    continue
                pat = re.compile(rb"(?<![A-Za-z0-9_\-])" + re.escape(n.encode()) + rb"(?![A-Za-z0-9_\-])", re.I)
                for k in list(files):
                    if k.endswith(".xmod"):
                        files[k] = pat.sub(name.encode(), files[k])
                renamed += 1
            files[name + ".xtex"] = body
            conv += 1
        del rdata
        if renamed:
            print("  %d texture name(s) the retail game uses for other pictures: renamed" % renamed)
        skel = code if code + ".skel" in files else src

        def cr_edit(cr):
            cr = re.sub(rb"FileNameRender\s+\w+", b"FileNameRender  " + code.encode(), cr)
            cr = re.sub(rb"FileNameSkel\s+\w+", b"FileNameSkel  " + skel.encode(), cr)
            return re.sub(rb"(\n\s*)Face\s+1", rb"\1Face  0", cr)

        addchar.add(game, mods, code, src, x, extra=files, cr_edit=cr_edit)
        print("%s: the preview's %s (%d model files, %d textures converted)"
              % (code, pc, sum(1 for k in files if k.endswith(".xmod")), conv))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
