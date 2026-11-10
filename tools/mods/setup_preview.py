#!/usr/bin/env python3
"""
setup_preview.py - set up the January 2002 preview's characters and stages
(and the story's own people) for Showdown and Play As, in one go. The
launcher runs it (Extras > Set up the preview characters); it can also be
run by hand:

  python tools\\mods\\setup_preview.py <preview disc image (.iso) or its SWAT.ZIP> <game folder> <out folder>
  python tools\\mods\\setup_preview.py --people <game folder> <out folder>

--people makes only the story's people (story_npcs.py), from the game's own
files -- no preview disc needed -- into <out folder> (the play folder's
Mods\\Story People: what Play As and story co-op offer while the preview
characters are off; Mods\\Preview has them too, for when they are on). The
full setup makes that folder as well, beside <out folder>.

<out folder> is the play folder's Mods\\Preview (the launcher's choice). It
is made beside as <out>.new and only takes the place of <out> once
everything is in, so a failed or stopped run leaves the old one as it was.

  1. numpy and Pillow (the line-up pictures), installed for this user when
     Python does not have them;
  2. beta_showdown.py: the preview's characters (beta_roster.py) and its
     stages (beta_level.py);
  3. beta_look.py: the preview's title picture, for the 2002 look;
  4. story_npcs.py: the story's people, on the line-up's third page.

Prints "PROGRESS <percent> <what>" lines for the launcher, and "ERROR
<what>" when it stops. Needs about 2 GB free.
"""
import os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable


def say(pct, text):
    print("PROGRESS %d %s" % (pct, text), flush=True)


def fail(text):
    print("ERROR " + text, flush=True)
    sys.exit(1)


def need_modules():
    try:
        import numpy, PIL  # noqa: F401
        return
    except ImportError:
        pass
    say(1, "Installing numpy and Pillow for Python (once)")
    r = subprocess.run([PY, "-m", "pip", "install", "--user", "--quiet", "numpy", "pillow"],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout)
        fail("Could not install numpy and Pillow. Run: python -m pip install numpy pillow")
    # a --user install is found from the next start of Python on: the steps
    # below are run as their own processes


def check_image(image):
    """The preview's disc: an ISO 9660 image with SWAT.ZIP at its root."""
    if not image.lower().endswith(".iso"):
        with open(image, "rb") as f:            # its SWAT.ZIP, given directly
            if f.read(4) != b"PK\x03\x04":
                fail("Choose the January 2002 preview's disc image (.iso), or its SWAT.ZIP.")
        return
    with open(image, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        if pvd[1:6] != b"CD001":
            fail("That file is not the 2002 preview's disc image (it is not a PlayStation 2 disc "
                 "image; an Xbox one is the game itself, not the preview).")
        root = pvd[156:190]
        f.seek(int.from_bytes(root[2:6], "little") * 2048)
        d = f.read(int.from_bytes(root[10:14], "little"))
    if b"SWAT.ZIP" not in d.upper():
        fail("That disc image is not the January 2002 preview of Red Dead Revolver (no SWAT.ZIP on it).")


def run_step(args, lo, hi, label):
    """Run a step, its "== stage i of n" lines moving the bar from lo to hi."""
    p = subprocess.Popen([PY, "-u"] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors="replace")
    last = []
    for line in p.stdout:
        line = line.rstrip()
        last = (last + [line])[-30:]
        m = re.match(r"== stage (\d+) of (\d+): (.*)", line)
        if m:
            i, n = int(m.group(1)), int(m.group(2))
            say(lo + (hi - lo) * (i - 1) // max(n, 1), "Stage %d of %d: %s" % (i, n, re.sub(r" \(lvl_\w+\)$", "", m.group(3))))
        elif line.startswith("== characters"):
            say(lo, "The preview's characters")
    if p.wait() != 0:
        print("\n".join(last))
        fail("%s failed (the lines above say why)." % label)


# What a folder was made by: one more each time what the setup makes
# changes (people added, a character fixed). Written into preview.txt /
# people.txt ("setup version N"); the launcher (rdr_settings.c,
# RDR_SETUP_VERSION) says when a folder was made by an older one.
# 2: Dirk and Gilbert; Long Coat Red's head (mod212.fit_skeleton).
# 3: 19 more of the story's people (the line-up past 144).
# 4: archives written with a whole name at least every 32 entries
#    (dave.MAX_RUN) -- longer runs crashed a lookup (Bat Boy).
# 5: the story's people's names (Jesse Lynch, Katie O'Grady ...).
# 6: the preview's trd is Teen Red (2002), not a target.
SETUP_VERSION = 6


def put_in_place(new, out):
    old = out + ".old"
    shutil.rmtree(old, ignore_errors=True)
    if os.path.isdir(out):
        os.replace(out, old)
    os.replace(new, out)
    shutil.rmtree(old, ignore_errors=True)


def make_people(game, out, lo, hi):
    """The story's people alone, from the game's own files, in <out>."""
    new = out + ".new"
    shutil.rmtree(new, ignore_errors=True)
    os.makedirs(new, exist_ok=True)
    say(lo, "The story's people")
    run_step([os.path.join(HERE, "story_npcs.py"), "add", game, new], lo, hi, "Adding the story's people")
    with open(os.path.join(new, "people.txt"), "w") as f:
        f.write("The story's people (townsfolk, soldiers, Atlas ...) for Play As and story co-op,\n"
                "made by setup_preview.py --people on %s.\n" % time.strftime("%Y-%m-%d"))
        f.write("setup version %d\n" % SETUP_VERSION)
    put_in_place(new, out)


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "--people":
        game, out = (os.path.abspath(x) for x in a[1:])
        if not os.path.isfile(os.path.join(game, "swat.dat")):
            fail("The game's files are not in " + game)
        need_modules()
        t0 = time.time()
        make_people(game, out, 2, 99)
        say(100, "Done in %d minutes" % max(1, round((time.time() - t0) / 60)))
        return
    if len(a) != 3:
        print(__doc__)
        sys.exit(2)
    image, game, out = (os.path.abspath(x) for x in a)
    if not os.path.isfile(image):
        fail("No such file: " + image)
    if not os.path.isfile(os.path.join(game, "swat.dat")):
        fail("The game's files are not in " + game)
    parent = os.path.dirname(out)
    os.makedirs(parent, exist_ok=True)
    free = shutil.disk_usage(parent).free
    if free < 2 * 1024 ** 3:
        fail("About 2 GB of free space is needed on that drive (%.1f GB free)." % (free / 1024 ** 3))
    check_image(image)
    need_modules()
    new = out + ".new"
    shutil.rmtree(new, ignore_errors=True)
    t0 = time.time()
    say(2, "Reading the preview's disc")
    run_step([os.path.join(HERE, "beta_showdown.py"), image, game, new], 3, 85, "Converting the preview")
    say(85, "The preview's title picture (the 2002 look)")
    r = subprocess.run([PY, "-u", os.path.join(HERE, "beta_look.py"), image, new],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    if r.returncode != 0:
        print(r.stdout)
        print("(the 2002 look's title picture was not made: the rest goes on)")
    say(86, "The story's people")
    run_step([os.path.join(HERE, "story_npcs.py"), "add", game, new], 86, 92, "Adding the story's people")
    with open(os.path.join(new, "preview.txt"), "w") as f:
        f.write("The January 2002 preview's characters and stages, made by setup_preview.py\n"
                "from %s on %s.\n" % (os.path.basename(image), time.strftime("%Y-%m-%d")))
        f.write("setup version %d\n" % SETUP_VERSION)
    put_in_place(new, out)
    make_people(game, os.path.join(os.path.dirname(out), "Story People"), 93, 99)
    say(100, "Done in %d minutes" % max(1, round((time.time() - t0) / 60)))


if __name__ == "__main__":
    main()
