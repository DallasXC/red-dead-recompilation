# Contributing

Thanks for helping. Bug reports from play are the most useful thing there
is: the game is big and every chapter, character and option combination
can turn up something new.

## Reporting a problem

Open an issue with:

1. **The log.** Every start writes `Logs\last_run.log` in the play folder
   (the start before it is kept as `Logs\previous_run.log`, so a crash log
   survives the next start). Attach the whole file.
2. **What you were doing**: the chapter or Showdown stage, the characters,
   and the options and cheats that were on (Story Co-op, Play As, frame
   rate...).
3. **A short video or screenshot**, if you can. It is often the fastest
   way to show what looks wrong.

Please never attach game files, disc images, or anything from
`src/recomp/gen/`.

## Contributing code

- Build first: [docs/BUILDING.md](docs/BUILDING.md).
- **No game-derived data in the repository**, ever: no generated C, no
  extracted files, textures or models, no disc keys. `.gitignore` covers
  the usual places; check `git status` before committing.
- Fixes to the game are keyed by the address of the game function they
  change (`sub_XXXXXXXX`). Say in a comment what the function does and how
  you found out; the code's comments are the main documentation.
- Changes to the xboxrecomp toolkit go in
  `patches/xboxrecomp-posix-x86_64-mmio.patch` (a diff against the pinned
  commit, see [docs/BUILDING.md](docs/BUILDING.md#the-toolkit-and-its-patch)).
- A fix worth remembering gets an item in
  [docs/dev/BRINGUP.md](docs/dev/BRINGUP.md): the symptom, the cause, the
  fix and how it was checked.
- Keep changes focused, and describe in the pull request how you tested
  them in the game.
