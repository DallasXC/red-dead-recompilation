# Changelog

1.1.0 is the first public release. The port is built from source on each
player's PC, from their own copy of the game. This is what it can do,
grouped by area, newest work first within each. The full story, fix by fix, is the development log,
[docs/dev/BRINGUP.md](docs/dev/BRINGUP.md) (the numbers below are its
items).

## Since 1.1.0

- **Keyboard and mouse are on by default** for a new settings file, and
  with them on for player 1 the prompts show key caps even when Windows
  reports a controller (user: a first build had no mouse and Xbox's button
  pictures, with no controller plugged in; Windows listed a virtual one).
- Only `build.bat` and `play_debug.bat` are left at the top of the
  project: `play.bat`, `make_preview.bat` and `play_preview.bat` are gone
  (the play folder's exe starts the game, and the launcher's Extras page
  sets up the preview's characters and stages).
- The icon as a 512 x 512 PNG (`assets/icon/icon.png`); the trailer and the
  Discord server on the README.

## 1.1.0: first public release (November 2026)

### Controls

- **Button pictures** (Controls): the PS2 version's cross, circle, square
  and triangle with a PlayStation controller (or always), Xbox's
  otherwise; key caps in our menus when playing on keys and mouse (196).
- Clicking **A Accept** in PC Options and the launcher takes the row with
  the cursor (197).
- On keys and mouse, the game's own menus and prompts show **key caps**
  (Enter, Esc and the keys bound to X and Y) (202).
- PlayStation button pictures in our menus no longer come out as other
  letters ("Â") (203).
- Clicking **Accept** in PC Options and the launcher now acts on the
  highlighted row (207).

### The 2002 look

- **2002 blue bands** (Display): the 2002 look without its bands. The 2002
  HUD no longer shows over loading screens, and its title waits for the
  game's own title screen (210).
- **2002 look** (Display): the HUD as the January 2002 preview drew it
  (TIME, money with cents, the revolver's rounds in an arc, the green
  health bar and the red Dead Eye badge, the dark blue curved bands) from
  the preview's art still on the retail disc (no setup), and the pause
  menu in the preview's brown tint and lettering (188).

- The 2002 look's **title**: the preview's red title picture with "Press
  Start", from your preview disc (the preview setup makes it) (193).
- The Spawner page no longer shows ImGui's "conflicting ID" box (194).

### Fixes

- **Fill ammunition** fills a given weapon too, to its maximum (208).
- The spawner's people come in **in front of you** and stay there, whatever
  they act like (209).
- Showdown's line-up: each pick's ring shows only on the page its
  character stands on, also when player 1 picks for every place (200).
- A rare freeze after pressing Start (the game waiting for a sound that
  never stopped) now goes on after a few seconds (201).
- Spawned characters no longer draw white streaks across the screen (198).
- Visual Studio is installed slim: the compiler and Windows SDK only (199).
- `build.bat -Compiler clang` builds with LLVM-MinGW instead of Visual
  Studio: about 175 MB to download, no installer (206, 212, 213).
- **Red Dead Revolver.exe has an icon** of its own (214).
- **Save backups**: each start that finds the saves changed copies them to
  `Saves\Backup\1` first, keeping the two copies before it (215).
- The game checks that it is the **USA release (version 1)** it was made
  for, and says so instead of running with its fixes in the wrong places
  (215).
- settings.ini is written whole or not at all; long folder names and mod
  lists are read back in full (215).
- Folders named with accents and other letters work, and the window is
  sharp on scaled displays (215).
- The online lobby checks every length in the host's save before using it
  (215).
- The spawner, Give Weapon and Fill Ammo stay in step online, whichever
  player uses them (checked: 3,900 frames, every command from both
  machines, no difference) (216).
- **Online join codes carry a key**: only someone with this game's code
  can join and receive the host's save (an address alone still works at
  home and over a VPN). Codes are a few letters longer, and both sides
  need this version (217).
- `build.bat` checks every download: each zip against its SHA-256, each
  installer against its publisher's signature; Python packages at fixed
  versions (217).
- Cheats are applied on the game's own thread, not the drawing thread
  (217).
- The stagecoach's rail physics now moves in whole steps above 62.5
  frames a second (219); the chase still breaks above 60, see 224.
- The demo no longer starts while the online lobby or any of our menus
  is open (218).
- **Crashes after some characters load** (seen loading the tomb after the
  train, and in the bank): the mod-character model fallback left a stale
  entry in the game's model list for every model a retail character
  lacks. It now only loads models that exist as files, and the heap
  checks its neighbours before joining them (220).
- **Go to Chapter from the main menu** no longer ends up in the first
  chapter: if a press reaches the book first, the warp goes on from that
  level to the chapter (226).
- The 2002 preview's stages are described as ported as they are, and
  may not all work as Showdown stages.
- **The frame rate is held at 60 in the stagecoach chapter** (Sunset
  Canyon): above 60 the chase still breaks; a known issue for now (224).
- **PC Options: Accept saves and leaves the page**, back to the list
  (225).
- **The shop and the weapon screen show your controller's buttons**:
  PlayStation marks with a PlayStation controller, key caps on keys and
  mouse (222).
- Online: one cause of the games parting on slower PCs removed (waits
  for the sound to stop ended on a different frame on each PC); and
  when they do part ("DESYNC"), each machine's log shows what each of
  the last frames held, to find what parts them (223).
- The log now says when a sound plays only silence, or when sound is
  lost on its way out, to track down cutscenes with no sound (221).
- **Internet address in the code** (the lobby's Host page): off keeps
  your public IP address out of the join code, for play over a VPN
  (218).
- **Faster generated code** with the LLVM-MinGW build (`-O2`, 4-8 % less
  CPU per frame measured), or with Visual Studio through `build.bat
  -FastCode`; builds with and without it play online together (217).
- The "Audio didn't halt" freeze is ended without touching the sound
  system: the wait is cut short instead (217; the first build after this
  update regenerates the code, about 15 minutes).
- A damaged disc image can no longer hang the install; archive tools
  refuse names outside their folder; the log falls back to %TEMP% when
  the play folder can't be written; removing the preview characters goes
  to the Recycle Bin; an uninstall section in the README; a new banner
  (217).
- **Licence**: MIT for the project's own code (LICENSE); see
  THIRD_PARTY_NOTICES.md for the rest (215).

### Laptops

- Touchpads that report finger positions now move the camera; Windows'
  touchpad pause while typing is explained in the user guide (190).
- The log no longer gets a line for every file read and every 200 draws
  (a hitch on slow disks and laptops) (189).

### Online co-op (highly experimental)

- The weapon select and the shop before a mission work online (they took
  no input), and the chapter's loading screen after them ends (204, 205).
- Movies are left out of online games: they played at each PC's own speed
  and the games came out of them apart (195).

- **Start in** on the host's lobby page: **Continue the story** or **A new
  game** takes every game past the logos, title, desk and book straight
  into the chapter's loading, by itself (the host's controller is pressed
  for it, so every game follows); **The menus** as before (192).
- For now, split screen shared through Parsec is the recommended way to
  play over the internet (online games can still freeze, e.g. both on the
  loading screen after a new game).

- **Online Co-op lobby** on the desk (Y or a click, where the Friends Menu
  was) and in PC Options: host or join with one join code that works at
  home, over a VPN and over the internet; the host's save and options go
  to everyone, and every game starts together (100, 102, 103).
- **Lockstep online play**: every player runs the whole game and only the
  controllers cross the network. A host and a guest stay frame-for-frame
  identical through the menus, chapter loads and gameplay, with one or two
  characters.
- **Host streaming**: the host plays and streams its picture and sound to
  guests, whose controller or keyboard is a player in the host's game
  (`RDR_NET_HOST=1` / `--join`; for testing now that the lobby is in).
- A status line in the picture while waiting for the other players, and if
  two games ever part.

### Story co-op (experimental)

- **A second keyboard and mouse** (Windows): two players on one PC, each
  with a keyboard and mouse. Picked in the game (Controls); the second
  mouse turns its player's camera (216).
- **Duels**: player 1's alone, as in a one-player game, or every player
  duels the same opponent at once, each on their own half of the split
  screen (126).
- **Weapons and money**: shared (player 1 buys and picks the loadout for
  everyone, one purse) or each player's own (kept between chapters, a
  Co-op Shop page in PC Options and the Quick menu) (124).

- 2, 3 or 4 players in the story levels, each on their own controller,
  shared or split screen; any character for each player; friendly fire on
  or off; set up from the chapter book or changed with Restart Level (32-36,
  53, 57, 58, 61, 81).

### Fixes (November 2026)

- build.bat no longer takes the 2002 preview's disc image for the game, and
  finds an extracted game inside the project folder too (186).
- Guns in Town: **guns in the stores** too: draw and shoot inside, the
  store keeping its own camera (182).
- Spawner: **animals from other chapters** (horses, cattle, buffalo,
  dogs); someone from another chapter who needs something this level does
  not have is left out instead of ending the game (183).
- Line-up: the story's people named rightly: Jesse Lynch, Katie O'Grady
  (1st and 2nd Encounter), Colonel Darren, Young Red (Bloody Hand),
  Griffin (184); the preview's Teen Red (185).
- README: what is unique to this port first, and tutorials for the
  story's people, the preview characters and the spawner.
- Spawner: new people can **act like anyone from the story's other
  chapters** (their AI comes with them); the game no longer ends with
  "Heap overrun" when one needs more memory than the level left (181).
- **The crash using Bat Boy's ability** (and other random crashes with
  the preview characters or the story's people): the setup wrote its files
  in a way the game reads with very deep calls, which ran out of room on
  Windows. Set the preview characters and the story's people up again
  (180).
- **The music cutting out**, and a Showdown's end that froze after it:
  a sound being turned on at the same moment as another one finishing
  could make the game drop the music (178).
- Logs: a game whose picture stands still for 20 s writes where it is
  stopped to the log, and goes on (178).
- Showdown line-up: **more than 144 characters** (room for up to 400 in all, the game's 48 included, over 8 pages), and
  19 more of the story's people (set them up again) (177).
- Logs: what the music's voice does (turned off, silent, stopped) is
  written to the log, to find why the music cuts out (176).
- Spawner: New people's **Health bar**: the people made show their name
  and health bar at the top of the screen, as bosses do (175).
- Bat Boy: a take-off that crashed the game (a pose with no animation)
  is now skipped and logged instead (173).
- Quick menu: Always Excellent (174).
- Co-op: **each player's own menu**: the Quick menu opens over the part
  of the screen (or in the window) of the player who opened it, worked by
  their controller, while the others play on (171).
- Showdown line-up: computer players' rings no longer show over another
  page (172).
- Online: **Showdown** starts (the match waited at its loading screen),
  and each player sees only their own part of the split screen, over the
  whole window (167).
- Separate windows: cutscenes and the pause menu fill every window (168).
- Preview characters: **Long Coat Red's head** in place (set the preview
  up again); the launcher says when a setup is out of date (169, 170).
- Launcher: the window can be **resized or maximized**; the page scales
  and uses the room (more rows when taller, wider rows when wider) (165).
- Spawner: the level's own **animals** (horses, cattle, buffalo) can be
  made too; making stops when the level holds all it can, instead of
  crashing, and the menu says how many more fit (162, 163).
- Menus: a slider's amount no longer runs into a long name (164).
- Menus: a **scroll bar** beside every list that is longer than the page
  (pause menu, quick menu, launcher), which can be dragged or clicked
  (161).
- Spawner: New people now has **How many** (1 to 8 a press), **Where** (in
  front, all around you, or further off) and **Guns** (as their side,
  none, or armed); they face you when they come in. **Remove the people
  made** takes them all out again (160).
- Spawner: **New people (experimental)**: make anyone from the story,
  Showdown, the preview or the mods in the level you are in, acting like
  one of that level's people, on their side, against you or with you
  (145). The people brought in now come in front of you, not behind (144).
  A new person no longer crashes the game after dying, and comes in where
  you are; the spawner no longer overwrites memory next to the people it
  brings in (146).

- Co-op: a player whose controller goes to sleep is no longer teleported
  along to player 1 (138); the players no longer lock on to each other
  (139); talking to the paper boy or a shopkeeper ends properly (142).

- Guns in Town: **Townsfolk Come Back**: people shot dead in Brimstone
  return a minute later (140).

- Spawner: **Fill ammunition** for every weapon a player carries (141).

- Display: **Black bars**: off, in cutscenes only, or always (as the
  game): with them off the whole screen is picture, in play and in
  cutscenes (136).

- The chapter book's Story Co-op (and Play As) answer a mouse click where
  the words are drawn (134).

- Showdown's line-up: a picked character's ring stays on the page it was
  picked on (135).

- Co-op Shop: with weapons and money shared, the page says why it is empty
  and lets you switch to each player's own (137).

- Display: a borderless window at the screen's own size (1920 x 1080 on a
  1080p screen) covers the whole screen; it had opened at 1600 x 900 in the
  middle (133).

- Showdown's line-up skips a page with nobody on it: with the preview
  characters off, the story's people are the second page (132).

- Co-op: the money counting sound no longer runs on after money is added:
player 2's counter had been counting its whole total up from nothing
  each time (131).

- Co-op: the players no longer snap back to player 1 on the train
  (chapter 3) or in Brimstone (127); entering Brimstone's stores and the
  Sheriff's Office cutscene no longer go black (128); the shared screen
  fills the screen again after split screen (129).
- The story's people (townsfolk, soldiers ...) can be played with the
  preview characters off: launcher > Extras > Set up the story's people,
  no disc needed; Atlas, Professor Perry's strongman, is one of them now
  (130).

- Co-op in the Battle Royale chapters (23 and 24): the picture no longer
  goes black when the chapter moves into its arena, and the duels are
  player 1's again: player 2 had taken the first duel and player 1 was
  left waiting for good (125).

- Clicking with a laptop's trackpad (a tap) works in the game's menus
  (123).

- The game hung before Press Start on a PC with the last build: the disc
  reads are as before again, with a fallback for 4096-byte-sector disks
  (115).
- Freeze NPCs stops people walking a path too (the second chapter) (118).
- Invincibility works in Showdown (119).
- Everything build.bat makes now stays inside the project folder: the
  Red Dead Recompilation play folder (moved in from beside it, saves and
  all), the toolkit and SDL2 (117).
- The launcher no longer stays in its parchment look when the game's files
  arrive late (116).

- A laptop stopped after the opening movies with no Press Start: reads
  from the disc's files failed on disks with 4096-byte sectors (115).
- Co-op: talking to people works again (114).
- The chapter book's Play As and Story Co-op close on Accept, and the PC
  Options pages make the game's menu sounds (113).
- A freeze now leaves a "[HANG]" line in the log saying where (112).

### Cheats and options

- **Always Win Duels**: the opponent draws and fires but misses (120).
- The spawner brings in the levels' animals too (horses, cattle, buffalo)
  (122).
- **Give Weapon** (Spawner page): any of the game's 50-odd weapons, to any
  co-op player, online too (121).
- **Spawner**: bring in the people the level has loaded and not brought in
  yet, by name, in front of any player; works in co-op and online (110).
- **Protect Allies**: the people on your side or not in the fight (the
  saloon fight's barmaids, the ones you guard, the townsfolk) take no
  damage (111).
- **Guns in Town**: draw and shoot in Brimstone, where the story keeps the
  guns holstered; the townsfolk can be hurt and killed (94, 99).
- **Play as** any of the 48 Showdown characters in the story.
- **Enemy health** 100-500 %, with boss bars and scripted reactions intact
  (73, 83).
- The game's own cheats without unlocking them; **Infinite Ammo**,
  **Money**, **Always Excellent**, **Unlock Everything** (with undo),
  **Freeze NPCs**, the **Showdown power-ups** in the story too (70, 80).
- Switching to another program during a cutscene or a load no longer
  pauses the game when you come back (158).
- Preview characters: the target cut-out is named **Target**, `nrd` **New
  Red**; the story people get the ranch hands **Dirk** and **Gilbert**
  (in place of a second Bartender and a third Townsman) (159).
- **Replacement cutscenes**: a video file in `Mods\videos\` (MP4, WMV)
  plays instead of the game's movie of that name, with its own sound (151).
- **HD texture packs from your own textures**: `Tools\mods\upscale_pack.py`
  makes one from the texture dump, with an AI upscaler or without (152).
- The launcher says when a **newer release** is out (About page; it can be
  switched off), and the **version** is on the title screen, the launcher
  and the log (153).
- A **first-start page** in the launcher: the screen, sharpness, black bars
  and controller or keyboard, before the first game (154).
- After a crash a box says so and offers to **copy the log** or open the
  Logs folder (155).
- Each settings page has **This page's defaults** (156).
- The game's disc and its folder are never written: opens asking to write
  there are made read-only, and creating or replacing files is refused
  (157).
- Controls can no longer get stuck if the game stops receiving the
  controller's reports: it reads the controller directly until they come
  again (150).
- Story co-op: **Separate windows**: each player's view in a window of
  its own, for a monitor each (149).
- With the black bars off, the HUD comes back after cutscenes and duels,
  and the little square in the bottom right corner is gone (148).
- **Free camera** (F6) (82, 85); the keyboard and each player's controller
  either fly it or keep playing, for filming (147).
- **Go to chapter** from the pause menu (39, 60, 63, 65).

### Menus and controls

- **Quick menu** (F1 or both sticks) and **PC Options** in the game's own
  pause menu, drawn with the game's own art and fonts (74, 75, 79, 84).
- **Mouse and keyboard**: direct mouse aim, mouse duels, every key
  rebindable, the desk, line-up, shops, weapon select and journal by mouse
  (38, 40, 41, 46, 47, 50, 68).
- **Controllers by name** through SDL2: Xbox, DualSense, DualShock 4,
  Switch Pro, plugged in while the game runs (28, 56, 69).
- **Sharp game text**: the game's lettering four times finer (76).
- A launcher (settings window) before the game, at every start until
  switched off; it installs the game from its disc image when the play
  folder has none, and switches each mod and texture pack on or off
  (106, 107).

### Showdown and mods

- **The story's people** (Brimstone's townsfolk, shopkeepers, Wilbur,
  soldiers, braves...) as Showdown characters, with their faces (92, 95-97).
- **The January 2002 preview**'s characters and stages in Showdown, Bat
  Boy flying included (37, 45, 49, 54, 55, 86-90, 93, 98); set up and
  switched on from the launcher's Extras page, 56 characters with the
  long-coat Red, and playable in the story through Play As (105).
- **Play As** lists any character a mod adds (105).
- More than 48 characters on the line-up (31).
- Mods folder, texture packs and texture dumping, new stages and
  characters: [docs/MODDING.md](docs/MODDING.md).

### Graphics and performance

- **GPU renderer** on the graphics card, vertex programs included (19, 59).
- Widescreen, window sizes, internal resolution up to 6x, FXAA,
  anisotropic filtering, high frame rates (27).
- Faster menus and chapter loads (14, 17, 64-66).

### Getting it running

- **`build.bat` asks which compiler to use** on the first build: Visual
  Studio (the default) or Clang (LLVM-MinGW, smaller, no admin). The
  answer is remembered; `build.bat -Compiler msvc` or `-Compiler clang`
  changes it. The README now says what PC is recommended.
- One-click build (`build.bat`) and a ready-to-play folder with one thing
  to start, `Red Dead Revolver.exe` (71, 72, 106); build straight from the disc image, dropped on
  `build.bat` or picked in a window (104).
- The fixes that took the game from a hang at start to playable: sound,
  movies, the legal screen, loading, Xbox Live screens, Showdown crashes
  and more (1-30, 42-44, 48, 51, 52, 62).
