# Modding Red Dead Revolver (recompiled)

The game's data is unchanged from the disc: a handful of archives and
loose files. The recompiled build adds two things that make editing that
data practical:

- a **mods folder** that the game reads in preference to the disc, so the
  original files are never touched, and
- tools to **unpack and repack the archives** and to **convert textures**
  (`tools\mods\`, Python 3, no other packages needed).

## The mods folder

The play folder has a `Mods\` folder, and the game looks in it by itself
(another folder can be given with `RECOMP_MODS_DIR`). When the game opens a file from the disc, it first
looks in `mods\` at the same relative path, and if a file is there it reads
that one instead:

| The game wants | Put your version at |
| --- | --- |
| `Red Dead Revolver (USA)\swat.dat` | `mods\swat.dat` |
| `Red Dead Revolver (USA)\lvls_sng\bomb.dat` | `mods\lvls_sng\bomb.dat` |
| `Red Dead Revolver (USA)\bik\<movie>.bik` | `mods\bik\<movie>.bik` |

Each replacement is logged in `logs\last_run.log` as
`[MODS] D:\swat.dat -> ...\mods\swat.dat`, which is the quickest way to
check that a mod was picked up. To turn every mod off, rename the folder.

A replacement is the **whole file**: two mods that both change `swat.dat`
have to be merged into one archive.

**Several mods**: a mod can have a folder of its own in `Mods\`
(`Mods\My Mod\swat.dat`), and the launcher's Mods page switches each one
on or off (`src/ui/rdr_mods.c`; a subfolder named as one of the game's
own, like `lvls_m2` or `bik`, belongs to the loose files instead). The
game is given the ones that are on: `RECOMP_MODS_DIR` can name several
folders, `;` between them; the first that has a file wins. The 2002 preview characters use that: switched on
(launcher > Extras), the game looks in `Mods\Preview\` first, then in
`Mods\` (in the project: `mods\Preview\`, `mods\`). Characters a mod adds
(any `m<code>.cr` with its `m<code>.swt` in a mods folder's `swat.dat`
that the game's own does not have) are offered by **Play As** and story
co-op, named as on the line-up (`swat.strtbl`); `src/ui/rdr_modchars.c`.

## What is on the disc

| File | What it is | Editable |
| --- | --- | --- |
| `swat.dat` (358 MB, 45,881 files) | Almost everything shared: characters (`.xmod`), animation (`.anim`, `.gait`), AI (`.actb`, `.stateb`, `.tbl`), textures (`.xtex`), effects (`.swptx`), tunables (`.csv`), subtitles (`.txt`), text (`.strtbl`) | Yes: dave.py |
| `front.dat` | Menus and front end: menu screens (`.swui`), fonts, the menu textures and models | Yes: dave.py (tested in game) |
| `lvls_sng\*.dat` (35) | Story levels: geometry, the level's own textures, effects and scripts | Yes: dave.py |
| `lvls_m2\`, `lvls_m4\` | Showdown (multiplayer) levels | Yes: dave.py |
| `pupshow.dat` | Facial animation (lip sync) for the cutscenes (`.iani`) | Yes: dave.py |
| `bik\*.bik` | Pre-rendered movies (Bink) | Replace the file |
| `banks.dat`, `stream0-4.dat` | Sound banks and streamed audio/speech | Not yet: a different format |

## Archives: dave.py

The `.dat` archives start with the text `Dave`. Each holds a flat list of
files; names are lower case.

```
python tools\mods\dave.py list    "..\Red Dead Revolver (USA)\swat.dat"
python tools\mods\dave.py extract "..\Red Dead Revolver (USA)\swat.dat" work\swat
python tools\mods\dave.py extract "..\Red Dead Revolver (USA)\swat.dat" work\swat "*.csv"
   (edit files in work\swat)
python tools\mods\dave.py build   work\swat mods\swat.dat "..\Red Dead Revolver (USA)\swat.dat"
```

- `build` packs **every file** under the folder, so extract everything
  before you rebuild (extracting only `*.csv` and rebuilding would give
  an archive holding nothing but the tables).
- Give the original archive as the last argument to `build`: the files it
  keeps uncompressed stay uncompressed. The game reads some of them in
  place, so this matters.
- Adding a file the game never asks for does nothing; removing one it
  needs crashes the game. Rename or delete nothing unless you know what
  reads it.
- Two names in `swat.dat` contain `?`, which Windows cannot put in a file
  name; they are extracted as `%3f` and turned back by `build`.
- A full `swat.dat` extracts to about 1 GB and takes a few minutes to
  rebuild.

Format details are at the top of `dave.py`.

## Tunables: .csv

1,031 plain comma-separated tables in `swat.dat` (and a few in each level
archive) hold the game's numbers. They open in Excel or any text editor;
keep the header row and the column count. Some worth a look:

| File | What it sets |
| --- | --- |
| `money_poweruptable.csv`, `health_poweruptable.csv`, `pistol_poweruptable.csv` | What dropped pickups give and how often money drops |
| `shop_default.csv`, `shop_ingame_lvl_*.csv` | What the shops stock, bundle sizes, how many are on hand |
| `playerranking.csv` | Showdown ranks: points needed, name, badge texture |
| `playerweapondroprate.csv`, `holdweapon.csv` | Weapon drops and handling |
| `bodyregiontbl_*.csv` | Hit zones and damage multipliers by weapon and difficulty |
| `*firetbl*.csv`, `*firetable*.csv` | How often, and how accurately, enemies and bosses fire |
| `lvl_*_itembox_probabilities.csv` | Item box contents per level |

`dave.py extract ... "*.csv"` pulls out just the tables for reading. To
change them, extract the whole archive, edit, and rebuild.

## Text

- **Subtitles**: `*_en.txt` and the other `.txt` files in `swat.dat` are
  UTF-16 text, one line per subtitle, fields split by tabs: the time it
  appears (`00:02.5`), the first line of text, the second line, then a
  number (5 throughout: likely seconds on screen). Notepad keeps the
  encoding if you save as "UTF-16 LE".
- **Game text** (menus, mission tips, item names): `*.strtbl` files, a
  binary table of names and strings (`swat.strtbl`, `lvl_<level>.strtbl`).
  There is no converter for these yet; short edits that keep the
  string's length can be made in a hex editor.

## Textures: xtex.py

`.xtex` is the game's texture format. The common ones are block-compressed
(DXT) and convert to and from DDS, which GIMP, Paint.NET and Photoshop
(with a plug-in) open:

```
python tools\mods\xtex.py info   work\front\front_paper.xtex
python tools\mods\xtex.py export work\front\front_paper.xtex front_paper.dds
   (edit front_paper.dds, save as DDS in the same format, with mipmaps if
    the original had them)
python tools\mods\xtex.py import front_paper.dds work\front\front_paper.xtex work\front\front_paper.xtex
```

| Format | What | How many | xtex.py |
| --- | --- | --- | --- |
| 0x16 | DXT1 (BC1) | ~2,200 | yes |
| 0x1A | DXT5 (BC3) | ~1,700 | yes |
| 0x18 | DXT3 (BC2) | ~30 | yes |
| 0x0E / 0x10 | 8-bit / 4-bit paletted | ~800 | not yet |
| 0x11 / 0x12 | uncompressed 24 / 32-bit | ~30 | not yet |
| others | 8 or 16-bit single channel | ~70 | not yet |

The size may change (higher-resolution textures work, sides a power of
two); `import` takes the DDS's size and mipmaps and keeps the rest of the
original header. Keep the format the same as the original: the game
picks how to draw a texture by it, and `import` refuses a mismatch.

The header, for anyone writing tools: 14 bytes, little-endian u16 width,
height, format, most mip levels (9 means the full chain), and three flag
words, then the levels largest first, each exactly
`width * height * bits_per_pixel / 8` bytes (the 2x2 and 1x1 levels are
not padded to whole blocks as they are in DDS).

## Texture packs

Textures can also be replaced as the game draws them, without touching the
archives: the way texture packs for emulators work:

1. Switch on **Dump textures** (launcher or F1 menu, Graphics page) and
   restart. Every texture the game loads is saved to `texture_dump\` as
   `<hash>.tga` (32-bit, with alpha). Play through the places you want to
   change, then switch it off again (dumping slows loading).
2. Edit the ones you want (GIMP, Paint.NET, Photoshop: save as TGA, 24 or
   32-bit, uncompressed or RLE). A larger size is fine: 2x or 4x for an
   HD pack; the game's coordinates are scaled to it.
3. Put them in a folder of their own in `Mods\textures\` (the play
   folder's; the project's is `mods\textures\`): that folder is the pack,
   switched on and off on the launcher's Mods page, and may have
   subfolders of its own. The name only has to **start** with the 16-digit
   hash: `3f2a9c0e11d4b7a2_saloon_sign.tga` works. When two packs that are
   on replace the same texture, the first by name wins
   (`RECOMP_TEXTURE_PACK` takes several folders, `;` between them;
   `=<folder>` is that folder's own files only).

`logs\last_run.log` says how many replacements were found
(`[TEXPACK] 12 replacement textures in ...`). The hash is taken over the
texture's decoded pixels and size, so it is the same on every machine and
every run. Textures the game rewrites as it goes (movies, some effects)
cannot be replaced this way.

### An HD pack made from your own textures

The game's textures cannot be shipped, so an upscaled pack is made on your
PC from your own dump: dump the textures as above, then

```
python tools\mods\upscale_pack.py "<play folder>\texture_dump" "<play folder>\Mods\textures\HD pack" --scale 4
```

With nothing else it enlarges them cleanly (Lanczos and a light sharpen,
on the CPU). Much sharper is an AI upscaler: download
[Real-ESRGAN ncnn Vulkan](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan)
(free, runs on the graphics card), unpack it, and add
`--esrgan "<its folder>\realesrgan-ncnn-vulkan.exe"` (`--model` picks one
of its models; `realesrgan-x4plus` by default). The transparency of each
texture is enlarged on its own, so cut-out edges stay clean; the font pages
and tiny textures are left alone; it can be stopped and run again (pictures
already made are skipped). Needs Python 3 and Pillow (`pip install pillow`).

## Replacement cutscenes

The game's movies (`bik\*.bik`, 720x480 Bink) can be replaced by ordinary
video files (upscaled, remastered, 60 fps, any size and shape): put
`<movie name>.mp4` (or `.wmv`, `.m4v`) in the play folder's `Mods\videos\`,
for example `Mods\videos\titles.mp4` for `bik\titles.bik`. When the game starts
that movie, Windows' own decoder plays the file instead, with its own sound,
full window at its own shape (720x480 or 640x480 files are shown 4:3).
Esc, Enter, Space or a controller's A, B or Start skips it, as the game's
own. A file Windows cannot open leaves the game's movie playing; the log
says which (`[MOVIE] ...`). To get the originals out to work on, ffmpeg
reads Bink: `ffmpeg -i titles.bik titles.mp4`.

## Showdown: characters and stages

Showdown's characters and stages are rows of `Unlockables.csv` (in
`swat.dat`): 48 characters (`maka`, `mbdg`, ...: the four letters name the
character's files) and 16 stages (`lvl_bridge` ... `lvl_colseum`), each with
what unlocks it. The stages themselves are the archives in `lvls_m2\`
(two players) and `lvls_m4\` (four). What is known so far about adding new
ones:

- **A new character** works too (experimental): see "Adding
  characters" below.
- **A new stage** works today, next to the 14 retail ones: see
  "Adding stages" below.
- The game looks for `lvls_m2\lvl_colseum2.dat` while the disc has
  `lvls_m2\colseum2.dat` (the Colosseum). The log's "missing" line for it
  at start is expected.

### Adding stages

The stage list on the Showdown poster is not in the game's code: it is the
`LevelTable` in `postermultiview.swui` (in `front.dat`), one line a stage:

```
<LOOKUPDATA:lvl_perf,	levelselect_perf,	Broken Creek,	boyhood home>
```

Its parts: the level, the poster's picture (a texture in `front.dat`), the title and
the line under the picture. When a stage is picked the game builds the
archive's name from the level's: the part after `lvl_`, the player count's
digit, `.dat`: `lvl_perf` with two players is `lvls_m2\perf2.dat`, holding
the level `lvl_perf2`; with three or four, `lvls_m4\perf4.dat` and
`lvl_perf4`. Nothing else names the stages, so a new line plus two new
archives is a new stage. `tools\mods\addlevel.py` does both:

```
python tools\mods\addlevel.py list   "..\Red Dead Revolver (USA)" mods
python tools\mods\addlevel.py add    "..\Red Dead Revolver (USA)" mods bcreek perf "Old Creek" "a copy to edit"
python tools\mods\addlevel.py remove "..\Red Dead Revolver (USA)" mods bcreek
```

`add` copies an existing stage (here Broken Creek) to
`mods\lvls_m2\bcreek2.dat` and `mods\lvls_m4\bcreek4.dat` with its files
renamed for the new level (`lvl_bcreek2.lvl`, `.spwn`, ...), and adds the
line to `mods\front.dat` (starting from the one already there, so stages
add up). Then change the two archives with `dave.py` to make the stage
your own; `beta_mine.py` (below) does that for the preview's mine, which it
adds as "The Old Mine" (`lvl_bmine`).

Limits, from the code that reads the list (`sub_000274A0`) and loads the
archive (`sub_0005FF90`):

- **50 stages.** The list is a fixed table of 50 rows (14 used, so 36
  free). The game does not check: a 51st row would be written over the
  row count. `addlevel.py` refuses past 50.
- **7 characters** for the name after `lvl_`. The archive name is 8.3 and
  the player-count digit takes the 8th place (`lvl_colseum` ->
  `colseum2.dat`).
- **31 characters** for the title, the subtitle and the picture's name
  (32-byte cells); no `,` `:` or `>` in them (they separate the fields).
- New stages are **never locked**: `Unlockables.csv` only lists the
  retail 14, and a stage it does not know is shown.
- The poster's picture has to be a texture in `front.dat` (128x64 like
  the others); reuse one, or add your own with `xtex.py`.
- Archives that are not on the disc are read from `mods\` only (the
  install-to-hard-disk cache list, 50 entries at `0x004062E0`, is just
  for the disc's files).

Verified on Linux: a copy of Broken Creek added as a 15th stage shows on
the poster (left from Twin Rocks), loads from `mods\lvls_m2\` and plays
with a bot.

Story chapters are listed the same way (`BookContents` in
`bookchapterview.swui`, 33 of 50 rows) but play in the order of
`levelflow.csv`, and the save keeps per-chapter results, so adding
chapters has not been tried. The disc also holds levels no chapter uses:
`lvls_sng\saloon.dat`, `theater2.dat` and `royale.dat`.

### Adding characters (experimental)

The Showdown roster is data as well. A character is:

- a `<FONTGRIDDATA:y:<code>:0>` line in `postermultiview.swui` (front.dat):
  the roster, read into a list that grows as needed: no limit of 48;
- a `hotspot_<code>` marker in `characterlineup.swui` (front.dat): where
  on the line-up photo the hand points. The photo is built at run time from
  a background and each unlocked character's cut-out, `ui_mphoto_<code>`,
  drawn at its marker; `mp_<code>` is the portrait in the player box. The
  48 retail characters fill the photo (x 0-1023). A marker at 1024-2047
  puts the character on a second page of the line-up, 2048-3071 on a third,
  and so on: hold the stick (or WASD) against the right-hand edge of the
  photo to turn to the next page, against the left-hand edge to go back
  (the recompiled game, `src/rdr_hooks.c`, "Showdown line-up: more than one
  page"). Each page shows the same street with its own characters;
- a row `m<code>,character,...` in `unlockables.csv` (swat.dat). Without
  one the line-up treats the character as locked and hides it (stages are
  the other way round). Rows added at the end leave the saved unlock bits
  of the others where they were. A profile keeps 256 bits, one per row,
  221 used by the retail table; the recompiled game treats every row past
  the retail 221 as unlocked and never writes a bit past 255 (a profile
  made before the mod has those rows clear, and the preview's 55 go past
  256: the line-up showed a few of them at random);
- in swat.dat, Showdown's set `m<code>.cr` (names the model, skeleton and
  animations: `FileNameRender srd`, ...), `m<code>.swt` (the "swat type":
  "Failed to load swat type" without it), `.dam`, `.msd`, `.rep`, `.sphs`,
  `.acc`, `.ars`, and the story's set `<code>.*`.

Starting weapons, special move and bot brain fall back to the "default"
rows of their tables when the code is not listed. The name under the
portrait is the code in capitals ("BRD").

```
python tools\mods\addchar.py "..\Red Dead Revolver (USA)" mods brd srd
```

adds `brd` as a copy of Red (`srd`): the files above, copied under the new
code, into `mods\front.dat` and `mods\swat.dat` (a full 400 MB copy;
`dave.py merge` copies the untouched entries as they are packed, so it
takes seconds). New characters go on the second page, 14 to a page, 68
apart (x 1044, 1112, ...), standing where the copied character's cut-out
stands (front, middle or back row). To move one:

```
python tools\mods\addchar.py place "..\Red Dead Revolver (USA)" mods brd 1300
```

The photo is built when the Showdown poster loads (at start-up and after
each match), so a change to what is unlocked shows from then on.
Tried on Windows: "BRD" can be picked, plays a match at Twin Rocks, and
quitting back to the desk works. Not yet looked
at: whether the per-character statistics in the save (favourite character,
most hated enemy) have room past 48: try it on a separate profile.

**The preview's characters, in their preview look:**

```
python tools\mods\beta_char.py list "..\PS2 Beta\preview.iso"
python tools\mods\beta_char.py add  "..\PS2 Beta\preview.iso" "..\Red Dead Revolver (USA)" mods brd red
```

adds `brd` as above (a copy of retail Red: his moves, voice, weapons) with
the preview's Red model (white shirt and red gloves, no long coat) its
hat and skeleton (`m<code>.cr`: `FileNameRender brd`, `FileNameSkel brd`,
`Face 0`). Two things stood in the way:

- A character's models come from its resource pack (`<name>_lods.xrsc` and
  its name table `.xrnt`: compiled models, not `.xmod` files), and only
  from there; with no pack it loads nothing and is invisible (only the gun
  in its hand was drawn). The recompiled game now loads such a character's
  `<name>_lod_01/02.xmod`, `hat_<name>_lod_01/02.xmod` from the archives
  the way levels load zone models (`src/rdr_hooks.c`, "Character models
  from .xmod files"; retail characters all have packs and are unaffected).
- The preview's "version: 2.1x" models keep each vertex relative to its
  bone, where the retail loader wants them in the model's space: drawn
  as they are, the body is a tangle of triangles. The retail disc still
  carries some of the preview's characters in the older text format, which
  it reads right (Red's `red_lod_01.xmod`: the same mesh, same textures), so
  the tool uses that copy when the counts agree. Otherwise the model is
  moved into the model's space: the 2.12 binary layout is now decoded
  (`tools\mods\mod212.py`: the text format's fields in order, without
  their names), and each vertex gets its bone's bind position added (the
  preview's skeletons have no rotations in the bind pose; checked against
  the retail disc's copy of Red: 2767 of 2799 corners agree to a
  millimetre).

**The whole preview roster at once:**

```
python tools\mods\beta_roster.py list "..\PS2 Beta\preview.iso" "..\Red Dead Revolver (USA)"
python tools\mods\beta_roster.py add  "..\PS2 Beta\preview.iso" "..\Red Dead Revolver (USA)" mods_beta
```

adds the preview's 56 human characters that convert (of 61; the
preview's horse, bull, buffalo, wolves, dogs and rooster walk on four
legs with animations of their own and are left out; `ann`, `jim`, `fat`,
`rwc` and `slm` have no models on the preview's disc at all: only their
`.cr` and moves; `jim` is the shooting range's flat target) with one
rewrite of the two archives:

- the long-coat Red (`coc`, "Long Coat Red"): its skeleton has two cloth
  anchors (`root_coat`, `coattail`) its model does not number, so the
  model's bones were one or two off (the toes half a metre underground);
  they are taken out first (`mod212.fit_skeleton`), and the coat's
  vertices blended between bones are put at their weighted places;

- each its own model, hat, skeleton and textures, the preview's own
  pictures converted. Many of the preview's texture names are in the
  retail game too, mostly with another picture in them (the character
  redrawn, its parts laid out elsewhere: Navarro's `zzt02`), and dressed
  in those a preview model wore scraps; such a name is renamed (same
  length, e.g. `zzt02` -> `zzt00`) in the models that use it. A hat is
  kept as it is (relative to the head, as the retail hats; moved into the
  model's space with the body it floated a metre up), and a separate face
  (`cls`, `fchk`, `fggl`, `fugl`, `wsk`: `<name>_face.mod`, the same 2.12
  format as the retail `<name>_face.xmod`) is added with `Face 1` and the
  preview's FaceOffset: without it O'Grady's and Chicken's heads had no
  face. Every skeleton is put onto a retail one: the preview's format
  hands the animations' channels out by a bone's place in the file, the
  retail one names them: in the old format Christina's arms folded,
  Young and Teen Red floated, and O'Grady and Old flew apart aiming a
  rifle. Which retail skeleton: the one of the same name when the retail
  game has it (Young Red's is the preview's, proportions and all; O'Grady,
  Chicken, Christina ...), else the copied character's, else Red's; with
  the preview's proportions where both have a bone, an extra bone's
  vertices moved onto its nearest retained ancestor (mod212.py
  `onto_skeleton`, `remap_bones`; `std_skeleton`, the retail 52 in order,
  only when that fails). Standing height: the animations carry the root
  at one height for everyone, and a character's `.swt` lowers its body by
  its own skeleton's root (`OffsetY`, about root - 1.12 in every retail
  character; `DuckOffsetY` about half of that, `Height` and
  `StandHeightForCameras` with it). Copied from Red's, Young and Teen
  Red and Old floated a quarter of a metre; they now get the retail
  character's of the same skeleton, or the copied one's, moved by the
  root difference (`stand_on_ground`). Textures of a character are made
  opaque when they have no see-through holes (`betatex.py --solid`:
  O'Grady's carried alpha 189-255 and he was drawn see-through);
- pictures drawn from the model (`tools\mods\charpic.py`): the cut-out
  standing on the line-up photo (front view, bind pose, sepia, as the
  retail ones: 256x128 8-bit, on its side) and the player-box portrait
  (ink: near-black with the darkness in alpha, stored upside down, as
  `mp_srd`);
- a name, written into `swat.strtbl` (`strtbl.py`'s new `add`; the line-up
  shows the string keyed by the code): the retail character it became,
  with "(2002)" or "'02" ("Chicken (2002)"), the preview's own name where
  it has one (its AI tables call `scb` Bat Boy), else its preview code;
- its weapon and special moves as the preview set them up. The preview's
  own Showdown screen, `2000.swui`, has three tables: WeaponTable
  (default pistol; `pgj` dynamite; `cls`, `zzt`, `zz2`, `old`, `nod`, `snp`
  rifle; `bil`, `mrk`, `ntl` double gun; `scb` knife), Special1Table and
  Special2Table (Rapid Fire for the Reds, Counter `mrk`, Dynamite Barrier
  `pgj`, Ghost `cls`, Glide and Glide Attack `scb`, Super Tackle and Super
  Armor `tck`, Spin Fire `ugl`). Retail Showdown takes the starting
  weapons from `CharWeaponTable` in `postermultiview.swui` (front.dat; a
  code not listed gets the "default" row, Red's Widowmaker), so a row is
  added per character (Six_Shooter, Twin_Revolvers, Sawn_Off,
  Halcon_Rifle, Knife, Dynamite; a thrower gets a pistol first, as the
  retail throwers do, since a special move wants a gun drawn), and the
  `.ars` to match. The special moves go into the `.swt`
  (`NormalSpecialMove`, `MeleeSpecialMove`). The retail engine's list
  (the table at `0x004045CC`: name, kind) is the preview's from Counter
  on (`spmove_countershoot, dynamitebarrier, ghosteffect, ricochet`
  (the preview's "Dancing B"), `armor, tackle, glide, glideattack,
  crazyhorse`) the preview's first two, Rapid Fire and Spin Fire, are
  gone (Red's `spmove_lockblast` and `spmove_crazyshoot` stand in). Where
  the preview gave none, none (not the copied retail character's);
- flight, for the preview's flier Bat Boy (`scb`; the preview's `nrd2`
  wears the same model and is the same character). The retail engine
  still has all of it: the `FlightData` move-set block (parsed by the
  class at vtable `0x003E70AC`; only the condor `cnd` and `npa` carry one
  on the retail disc), Bat Boy's glide animations (`scb_for_gld`,
  `scb_for_gld_ant`, `scb_for_chg`: `scb` is a cut character whose
  animations, skeleton and textures are still in swat.dat), the special
  moves `spmove_glide` / `spmove_glideattack`, and Bat Boy's AI
  (`batboy.tbl`: search, attack, `MPDiveAttack`; `postermultiview.swui`
  even has a commented-out `PlayerAI_BatBoy` player brain). The tool puts
  the preview's own flight values (`scb.msd`: flight speed 10, gravity
  -0.2, glide 6 s, height 4) into a retail-format `FlightData` block (the
  condor's, with the fields the preview's lacks) in his `.msd`, points
  his `.rep`'s `_for_gld`, `_for_gld_ant` and `_for_chg` at the glide
  animations (retail characters point them at standing still), and gives
  him the `batboy` brain in `AIBrainTable`. None of that is read unless
  the `.swt` says the character has the flight move set: bit 0 of
  `SpecialMovesets` (`SwatCreatureType` `+0x1DC`; the condor `cnd` and
  `npa` have 1, the rest 0 or 2). The tool sets it (and no fall damage,
  as the condor), and puts the block in the story copy too (its `.msd` is
  indented with spaces; the Showdown one with tabs). With it, Bat Boy
  takes off: about 11 m up (`AscensionScale` 0.8: the climb is the
  launch clip's rise between the two `AscensionSampleFrame`s times that,
  for one play of the clip; the preview's 2.5 took him 35 m up), then
  glides down for six seconds (the preview's `MaxGlideDuration`) and drops
  the rest of the way. Played by a person: the melee button with the gun put away (the
  game's own way: `sub_00143660` starts the `MeleeSpecialMove`, his
  `spmove_glideattack`, only while the controller's `+0xA8` is 0, no gun
  drawn), or **Black** with the gun drawn, as the game's hint text says
  (`swat_xbx.strtbl` "HowToSpecialMove"; nothing in the retail game wires
  that for a person, so `rdr_overrides.c` `flier_black` does the game's
  own glide steps: CanDoSpecial `+0x198`, pay `+0x19C`, take off
  `sub_0025A980`; in the air, Black or the melee button dives,
  `sub_0025AC90`, at the aimed-at character or the nearest one ahead);
- moves and voice of the retail character it became when there is one
  (the preview's Chicken moves and sounds as Chicken), else Red's. The
  preview's own movement and damage files were written for its older
  engine (damage 0.1 where retail has 20). The voice is the preview's own
  where it had one: its `.swt`'s `CreatureSoundSet` (101-121) names a bank
  the retail `banks.dat` still holds (`snd_<n>.hd/.bd`, PS2 HD/BD banks in
  a DAVE archive; `tools\mods\beta.py list banks.dat`), so Bat Boy (106),
  Pig Josh (107), O'Grady (120) and eleven more speak as in the preview;
  the others keep the copied character's.
- codes `q..` (`x..` when taken), checked against every name in every
  archive: `gma` is free in swat.dat, but a story level has the
  grandmother's `gma_lods.xrsc`, and a Showdown character called `gma`
  loaded her model pack and hung the match's loading.

The whole preview stands on one line-up page, the second: three rows
(18/18/19, the tallest at the back), each row a third of a step over from
the one behind; each cut-out is drawn for its row (feet at 178, 218 or 254
of the photo's 256, the figure's height in proportion to the model's) and
its hotspot (where the hand points and what it picks) is the figure's
face (`lay_out`; before, each copied the retail character's, often a
back-row head above a front-row figure). Old's model is the older 2.00
format (`mod212.parse200`), now drawn too.

Verified in the harness: the line-up pages, the drawn cut-outs and
names, a two-player match at Twin Rocks with two preview characters,
Bat Boy with pistol and knife in the HUD.

Special moves, what is known (the player's class, vtable `0x003D9FA8`):
`CanDoSpecial` (`+0x198`, `sub_0013E840`: by the move, always, or the gold
meter under the health bar), paying for it (`+0x19C`, `sub_0013E8A0`:
one segment of the meter, `sub_0005B1F0`, or the Infinite Dead Eye cheat
outside Showdown) and the start of the kind-3 moves (`+0x1A0`,
`sub_00143370`). A person's character: `sub_00143660` (each update in
battle mode) starts the MeleeSpecialMove (`.swt`, read to `+0x274`; the
NormalSpecialMove is `+0x2B4`) on the melee button or Right Trigger with
no gun drawn; the button edges are `+0x600` (Left Trigger), `+0x604`,
`+0x608` (B), `+0x60C` (Right Trigger), `+0x610` (right stick click),
1 up, 2 pressed, 0 held, 3 let go (`sub_0013B530`, by the button table at
`0x0040BFD8`, rewritten at run time by the controls set-up). The
controller's `+0xA8` is 0 with no gun, 1-3 drawing and drawn, 5-6
putting it away. The AI's way in is its script interpreter's special-move
action (`sub_00139780`: a button of the brain's controller,
`[controller+0xA8] == 3`, a move of kind 1). Nothing starts a special for
a person's Black: the PC build does it for fliers (above).
`RDR_SPECIAL_DUMP=1` logs those fields whenever they change (with the
pad's buttons, the flight state and the height while flying) and the
gates of `sub_00143660` when the melee button is pressed.

Textures converted from the preview (`betatex.py`) are written red first
now: the game reads format 0x12 as R, G, B, A in memory (a texture
written blue first as pure red came out pure blue in a match). Earlier
conversions (the preview stages' textures too) had red and blue
swapped; build them again.

Verified on Linux: a story level played as `mbrd` (`RDR_PLAY_AS=mbrd`) shows
the preview's Red, animated.

## The story's people in Showdown

`tools\mods\story_npcs.py add <game folder> <mods folder>` (run by the
launcher's Extras page after the preview) adds 59 of the story's characters the
line-up never offers (Brimstone's shopkeepers and townsfolk, Father
Driscoll, Wilbur (the duel on the train), the train's conductor, "Hatchet" Schultz, soldiers, braves,
bandits) on the line-up's pages after the preview's, 42 to a page. Each is the game's
own story character from swat.dat (its .cr, .dam, .sphs, .hot: model,
skeleton and face as in the story) under a new code (`w..`), with Red's
weapons, unlock row and hit points (a townsman has few), and Red's moves:
his .rep and .msd, and his .swt but for who the character is (name, voice,
hat, height). A townsman's own .rep names the townsfolk's animations and
gaits (`bty_*`, `ani_dck_for_wlk`), which only the story levels' archives
hold: a Showdown match crashed loading the first such character (the gait
set calling through a wild pointer, 0x41200000). Its cut-out and portrait
are drawn from its model and, where the face is a model of its own
(`<name>_face.xmod`, the .cr's `Face 1`: most of them), from that too, on
the head bone's place plus the .cr's `FaceOffset` (charpic.py
`head_place`/`with_face`; the preview's characters with a separate face
get the same). Names are the game's where it has one (swat.strtbl, the
cutscenes' cast lists in `*_pupshow_*.csv`: `brb` is Wilbur), else what it
is. Three whose models are still in the old text format are left out
(Sheriff, Young Red's story double, one bandit).

The line-up page holds at most 150 elements (its element list at `+0x40`,
the count at `+0x298`; `sub_00019EF0` adds past the end): the photo, paper
and group, and one marker a character: 158 characters crashed the
poster's loading. The recompiled game now keeps the markers past the
150th in a list of its own (`src/rdr_hooks.c`, the hooks on `sub_00019EF0`
and `sub_00019D60`), so the line-up takes up to 400 markers over 8 pages,
the game's 48 included: up to 352 new characters. The extras use 115 of
them (56 preview, 59 story), 163 in all; the tools stop adding at 400.
Play As has a list of its own, of up to 240 characters (the game's 54 and
186 more): a mod character past that is on the line-up only.

## The January 2002 preview ("Red Dead Revolved", PS2)

The preview disc's `SWAT.ZIP` is not a zip but the retail archive's
ancestor (a "DAVE" archive with plain-text names); `tools\mods\beta.py`
lists and extracts it:

```
python tools\mods\beta.py levels  SWAT.ZIP
python tools\mods\beta.py extract SWAT.ZIP work\beta "lvl_mine*"
```

It holds 75 level files (72 levels plus three old copies: `_lvl_msn`, `lvl_core_k`, `lvl_trainbak`), 25,479 files in all. Besides early versions of retail levels,
there are levels that did not ship, with geometry: a mine (`lvl_mine`,
`lvl_minenite`, `lvl_minelight`; ~380 models), a training ground
(`lvl_tng`), two deathmatch arenas (`lvl_dm1`, `lvl_dm2`), `lvl_perf75`,
`lvl_floor`, `lvl_msj`, a low sheriff's town (`lvl_sherlo`, `lvl_sherlo2`),
and small interiors: saloon, barber, bath house, general store, gun shop,
lawyer, station, estate, residence. Some names have no content left
(`lvl_coliseum`, `lvl_dino`, `lvl_cave`, `lvl_monu`).

What carries over to the Xbox game: checked against the retail level
archives (`lvls_sng\*.dat`):

- The level descriptions (`.lvl`, `.bnds`, `.bnd`, `.lnes`, `.bxes`,
  `.spwn`, `.lgt`) are the same text formats; a preview `.lvl` and the
  retail one of the same level differ only in which zones they list.
- Zones: the preview's `.znes` and the retail `.xznes` are the same text;
  the retail ones add `resourceIndex`/`resourceIndexXbox`, an index into
  the level's packed resource file (`lvl_<name>.xrsc`, "RSC0", chunks of
  models, textures and palettes). The retail executable still contains
  the loaders for the older forms: the strings `.znes`, `.mod` /
  "version: 2.00", `.tex`.
- Models: preview `.mod` files are binary "version: 2.00" (some are
  shared unchanged with retail); retail `.xmod` is 2.12.
- Textures: preview `.tex` has the retail header (width, height, format,
  levels), mostly format 9 (8-bit with a 256-colour palette).

A test, with the archive look-ups logged (RDR_FILE_LOG=1, every name the
game asks a Dave archive for): the first story level is `lvl_perfnew`
(lvls_sng\perfnew.dat), not the ranch. With its `.lvl` replaced by the
preview's `lvl_mine.lvl` and the mine's files added to
`mods\lvls_sng\perfnew.dat`, the game reads the new list: its lights,
fog and LOD tables, then all twelve of the mine's `.bnds` (the preview's
bounds load as they are), then asks for `lvl_mine_background.xznes`
(the `.znes` in the list becomes `.xznes`, with no fall-back to the
preview's `.znes`) and stops with "Couldn't load file".

Playing the mine. No converter was needed after all: with no
`lvl_<name>.xrsc` in the level's archive, each zone loads its model and
bounds by name (`<level>_<zone>.xmod` and `.bnd` (lvl_perfnew_ground.xmod
for the mine's `zone_ground_01`)) and the model loader reads the
preview's "version: 2.00", "1.10" and "2.1x" models as they are. The
"version: 1.09" ones (35 of the mine's ~600) it does not, but they are
text and differ from 1.10 in three places only (1.10 adds a
`reskins:` count after `matrices:` and an `attributes:` line closing each
material, and 1.09 leaves junk in the unused tex2 column of its `adj`
rows) so the tools rewrite them as 1.10. So:

```
python tools\mods\beta_mine.py <preview .iso or SWAT.ZIP> <game folder> mods [stage|showdown|story|both]
```

adds the mine to Showdown as a stage of its own, "The Old Mine" (the
default, `stage`: `addlevel.py` above, `lvl_bmine`), or builds it in
place of Showdown's "Broken Creek" (`showdown`: `lvls_m2\perf2.dat`,
`lvls_m4\perf4.dat`) or of the first story chapter (`story`): its
`.lvl`, zones also as `.xznes`, bounds and models renamed for the level,
spawn points, no `.xrsc`, with the textures its models name converted from
the PS2 format (`tools\mods\betatex.py`: 4- and 8-bit paletted, 16-, 24-
and 32-bit, to a linear A8R8G8B8 `.xtex`). Every player gets the mine's
spawn points: those with ground under them (`on_ground`): three of the
mine's five are in parts its `.lvl` does not load (the second base), where
a player fell forever. Left out: the host level's scripts (in the
story: enemies, cutscenes, the story's player script: the player gets
Showdown's `BR_playerAI`). Verified on Linux: "The Old Mine" is on the
Showdown poster, loads, draws (ground, cliffs, mountains, sky, tunnels)
and Red walks on its bounds.

Any other preview level goes in the same way, as a new stage:

```
python tools\mods\beta_level.py list "..\PS2 Beta\preview.iso"
python tools\mods\beta_level.py add  "..\PS2 Beta\preview.iso" "..\Red Dead Revolver (USA)" mods lvl_dm2 bdm2 "Deathmatch Two"
```

`list` shows the 72 levels and which have spawn points of their own
(those without start where Broken Creek's players would, which may be
in the air or under ground). How complete each one is varies: some are
whole levels, some a few test pieces, and what a level takes from outside
its own files (shared props, scripts) is not brought along.

What the build does to each level besides converting it:
- the level's own pieces are put under the name it loads as
  (`lvl_<name>2_<piece>`): models, bounds, and the animations and skeletons
  of its animated props;
- `box_SHADOWMAP_*` boxes are dropped from its `.bxes`: each wants a
  pre-drawn shadow texture (`lvl_<level>_shbox_<name>`) the preview never
  had, and `imgShadowBox::Init` stops the game at the first one missing;
- with `--static`, its animated props (`zone_<x>_anim_01` in the `.znes`:
  mill wheels, flags, swinging signs, the breakable "crack" pieces) are
  left out. The preview's animations are an older format ("cmp"); some
  play, others come out empty and the zone's first update divides by zero
  (`sub_000D8B40`, through `sub_000803F0`), which stops the load.
  `RDR_ZONE_LOG=1` names each zone as it first updates, so the last one in
  the log is the one that stopped it.

**The way players set them up** is the launcher's Extras page (see the
[user guide](USER_GUIDE.md#the-2002-preview-characters)), which runs
`setup_preview.py <preview .iso or SWAT.ZIP> <game folder> <out folder>`
(into the play folder's `Mods\Preview`): numpy and Pillow
installed if missing, `beta_showdown.py` (below), `story_npcs.py`, all into
`<out>.new`, which takes `<out>`'s place only when everything is in. It
prints `PROGRESS <percent> <what>` lines for the launcher.

**All of them at once** (`beta_showdown.py`): the characters (above) and
every level that was seen to load and play in a two-player match in the
test harness:

| Stage | Preview level | |
|---|---|---|
| Twin Rocks (2002) | `lvl_hng` | static |
| Brimstone (2002) | `lvl_sher70` | static |
| Brimstone Night | `lvl_sherNite` | static |
| Low Brimstone, Low Brimstone 2 | `lvl_sherLo`, `lvl_sherLo2` | |
| Sheriff Office | `lvl_sheriff` | static |
| The Saloon (2002) | `lvl_saloon` | |
| Broken Creek (2002) | `lvl_perf75` | static |
| Fallen Creek (2002) | `lvl_flash` | static |
| Fairground (2002) | `lvl_ghost` | |
| Canyon | `lvl_cyj` | |
| The Bridge (2002), Bath House | `lvl_bridge`, `lvl_bath` | |
| The Station, The Estate, Residence | `lvl_station`, `lvl_estate`, `lvl_resid` | |
| Bounty Office | `lvl_tombInt` | |
| Lawyer Office, Clothing Shop, Gun Shop, General Store | `lvl_lawyer`, `lvl_cloShop`, `lvl_gunShop`, `lvl_genStore` | |
| The Jail (2002) | `lvl_core` | static |
| Mission Jail | `lvl_msj` | very dark |
| The Old Mine, The Mine at Night | `lvl_mine`, `lvl_mineNite` | static |
| Deathmatch Two | `lvl_dm2` | |
| Training Ground | `lvl_tng` | static, one spawn point |
| Barber Shop | `lvl_barber` | |
| The Floor, Arena, Arena Three (test) | `lvl_floor`, `lvl_arena`, `lvl_arena3` | test grids |

Several interiors stand on the preview's own placeholder ground, a
coloured grid (texture `grid`), as they did in the preview.

Left out: `lvl_cover` and `lvl_coreMulti` (test levels: they crash while loading), `lvl_chs` (Chase: like The Mission, a file it asks for is missing), `lvl_tomb` (the Bounty Office's street: "zone gun ... needs a
bound", a zone whose collision the preview does not have), `lvl_msn` (The
Mission: a file it asks for is not in the preview), and the levels without
spawn points of their own.

## Movies

The `bik\` movies are Bink 1 files. A replacement goes at
`mods\bik\<same name>.bik`; keep the resolution and frame rate of the
original. (Untested so far: the format is the same, so it should play.)

## What is not moddable yet

- **Models, animation, levels** (`.xmod`, `.anim`, `.skel`, level
  geometry): the formats are known by name only; they can be swapped
  between files of the same kind (one character's model for another) but
  not edited.
- **Audio** (`banks.dat`, `stream*.dat`): not a Dave archive, not yet
  examined.
- **Game code**: the recompiled code is C generated from the game, so
  behaviour changes go in as small patches in `src\` (as the cheats in
  `src\ui\rdr_cheats.c` do) rather than as files in `mods\`.
