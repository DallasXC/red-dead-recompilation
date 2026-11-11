#!/usr/bin/env python3
"""
beta_showdown.py - the January 2002 preview ("Red Dead Revolved", PS2) in
Showdown, all at once: its characters (beta_roster.py) and the levels of it
that play as stages (beta_level.py), into one mods folder. EXPERIMENTAL.

  python tools\\mods\\beta_showdown.py <preview .iso or SWAT.ZIP> <game folder> <mods folder>

Takes a few minutes (each stage is two archives of ~30 MB). Start from a
mods folder without them (the launcher's Extras page makes Mods\\Preview new).
The stages are the ones a two-player match was seen to load and play in
(docs/MODDING.md has the list and the ones left out, and why).
"""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import beta_level

# (preview level, stage name, title, static) -- names are lvl_<name>2/4 in
# lvls_m2/m4; static: without the animated props (beta_level.py --static)
STAGES = [
    ("lvl_hng", "bhng", "Twin Rocks (2002)", True),
    ("lvl_sher70", "bsher70", "Brimstone (2002)", True),
    ("lvl_sherNite", "bshernt", "Brimstone Night", True),
    ("lvl_sherLo", "bsherlo", "Low Brimstone", False),
    ("lvl_sherLo2", "bsherl2", "Low Brimstone 2", False),
    ("lvl_sheriff", "bsherif", "Sheriff Office", True),
    ("lvl_saloon", "bsaloon", "The Saloon (2002)", False),
    ("lvl_perf75", "bperf75", "Broken Creek (2002)", True),
    ("lvl_flash", "bflash", "Fallen Creek (2002)", True),
    ("lvl_cyj", "bcyj", "Canyon", False),
    ("lvl_ghost", "bghost", "Fairground (2002)", False),
    ("lvl_bridge", "bbridge", "The Bridge (2002)", False),
    ("lvl_bath", "bbath", "Bath House", False),
    ("lvl_station", "bstatio", "The Station", False),
    ("lvl_estate", "bestate", "The Estate", False),
    ("lvl_tombInt", "btombin", "Bounty Office", False),
    ("lvl_resid", "bresid", "Residence", False),
    ("lvl_lawyer", "blawyer", "Lawyer Office", False),
    ("lvl_cloShop", "bcloshp", "Clothing Shop", False),
    ("lvl_gunShop", "bgunshp", "Gun Shop", False),
    ("lvl_genStore", "bgenstr", "General Store", False),
    ("lvl_core", "bcore", "The Jail (2002)", True),
    ("lvl_msj", "bmsj", "Mission Jail", False),
    ("lvl_mine", "bmine", "The Old Mine", True),
    ("lvl_mineNite", "bminent", "The Mine at Night", True),
    ("lvl_dm2", "bdm2", "Deathmatch Two", False),
    ("lvl_tng", "btng", "Training Ground", True),
    ("lvl_barber", "bbarber", "Barber Shop", False),
    ("lvl_floor", "bfloor", "The Floor (test)", False),
    ("lvl_arena", "barena", "Arena (test)", False),
    ("lvl_arena3", "barena3", "Arena Three (test)", False),
]


def main():
    a = sys.argv[1:]
    if len(a) != 3:
        print(__doc__)
        sys.exit(1)
    swat, game, mods = a
    work = tempfile.mkdtemp(prefix="beta_showdown_")
    try:
        swat = beta_level.open_preview(swat, work)
        print("== characters")
        subprocess.run([sys.executable, os.path.join(HERE, "beta_roster.py"), "add", swat, game, mods],
                       check=True)
        for i, (src, name, title, static) in enumerate(STAGES):
            print("== stage %d of %d: %s (%s)" % (i + 1, len(STAGES), title, src))
            beta_level.make_stage(swat, game, mods, src.lower(), name, title, static=static)
        print("done: %s" % mods)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
