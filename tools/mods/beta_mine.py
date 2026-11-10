#!/usr/bin/env python3
"""
beta_mine.py - play the January 2002 preview's mine ("Red Dead Revolved",
PS2) in the Xbox game.

  python tools\\mods\\beta_mine.py <preview .iso or SWAT.ZIP> <game folder> <mods folder> [where]

  where: stage (default) -- the mine is added to Showdown as a stage of its
                     own, "The Old Mine", after the 14 retail ones
                     (mods\\front.dat, mods\\lvls_m2\\bmine2.dat,
                     mods\\lvls_m4\\bmine4.dat)
         showdown  -- the mine takes the place of Showdown's "Broken Creek"
                     stage (the farm), 2- and 4-player
                     (mods\\lvls_m2\\perf2.dat, mods\\lvls_m4\\perf4.dat)
         story     -- the mine takes the place of the first story chapter
                     (mods\\lvls_sng\\perfnew.dat)
         both      -- story and showdown

e.g.
  python tools\\mods\\beta_mine.py "..\\PS2 Beta\\preview.iso" "..\\Red Dead Revolver (USA)" mods

Delete the .dat files it wrote from the mods folder to get the originals
back ("addlevel.py remove ... bmine" takes the stage off the poster).
beta_level.py does the same for any of the preview's levels.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import beta_level

IN_PLACE = {
    "story": [("lvls_sng/perfnew.dat", "lvl_perfnew", True)],
    "showdown": [("lvls_m2/perf2.dat", "lvl_perf2", False), ("lvls_m4/perf4.dat", "lvl_perf4", False)],
}


def main():
    a = sys.argv[1:]
    if len(a) not in (3, 4) or (len(a) == 4 and a[3] not in ("stage", "story", "showdown", "both")):
        print(__doc__)
        sys.exit(1)
    swat, game, mods = a[:3]
    where = a[3] if len(a) == 4 else "stage"
    if where == "stage":
        beta_level.make_stage(swat, game, mods, "lvl_mine", "bmine", "The Old Mine",
                              "2002 preview", "levelselect_core2")
    else:
        hosts = IN_PLACE["story"] + IN_PLACE["showdown"] if where == "both" else IN_PLACE[where]
        beta_level.make_in_place(swat, game, mods, "lvl_mine", hosts)


if __name__ == "__main__":
    main()
