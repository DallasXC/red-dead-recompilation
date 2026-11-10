# Menu and launcher designs

Mockups for the F1 menu and the launcher, drawn only with the game's own art
(read from the player's disc at run time, never shipped):

| Piece | Where it comes from |
|---|---|
| Menu text, button glyphs (A B X Y) | `berryliumx.fonttex` + pages (swat.dat; the Xbox build of berrylium) |
| Page titles, option lists | `regulatorcondensed.fonttex` |
| Paper titles | `goldrush.fonttex`, `woodcut.fonttex` |
| Launcher logo | `logo.fonttex` (the Red Dead Revolver logo's typeface) |
| Scroll rules | `pausescrolls.xtex` (line at texture row 61; the top one drawn flipped) |
| Options background | `options_bg_10k.xtex` (front.dat) |
| Slider | `mp_arrow.xtex` (chevrons), `mp_xspot.xtex` (the red X), a 1 px groove |
| Paper | `front_paper.xtex`, corner flourishes from `profiles.xtex` (front.dat) |
| Portrait | `book_red.xtex` (front.dat, the chapter book's cover cameo; detail is in alpha) |

## Font format (.fonttex)

`<f11I` header: [0] global scale 1.02, [2] tracking, [3] space width,
[11] glyph count. Then one 10-byte record per glyph `<IBBBBbB`: character,
x, y, w, h, y offset from the baseline (signed), flags. Then u32 page count
and, per page, the glyph index it ends at. Pages are `<name>_NN.xtex`,
paletted (0x0E 8-bit / 0x10 4-bit), palette first, linear pixels.
berrylium's atlas has no '%': lay the rest out again left to right.

## Measurements (1280 x 720, from the game's Player Options)

Scroll lines at y 78 and 646. Title rows 124..203 (regulatorcondensed x4.12
y3.2, red 0.288,0.038,0 doubled). Rows: berrylium 0.75 -> x1.5 y1.2, label
x 160, baselines 252 + 56 n, combo triangles at x 705 / 1105, value centred
at 905. Slider: '-' 655, '+' 1150, chevrons 702 / 1105. Prompts: berrylium
1.0 -> x2.0 y1.6, baseline ~590. Unselected rows white at alpha 0.5.
Pause menu: dim 0.584, rules 600 px wide, items 43 px apart, berrylium
x1.55 y1.1 centred with drop shadow.

## Native pages

- `.swui` pages are plain text; the page factory (sub_00028E10) builds a
  class per known name (cheats -> sub_00027F10, ...), anything else gets the
  generic page (0x52C bytes, sub_00038000).
- Page handlers find their widgets by name (cheats: sub_00030850 walks
  page+0x40, type 4 = combo, name at widget+0x114).
- Every level archive carries its own `pause.swui`, so a mods swat.dat is not
  enough: serve edited pages from the archive lookup (sub_000D4250) instead.
- The pause menu's "Xbox Live" item (callback `xblive`) does nothing on PC.
- Tried in the harness (mods swat.dat + the level's tomb2.dat): renaming that
  item to "PC Options" with target `pcoptions` and adding `pcoptions.swui`
  works; the game draws the new page itself (combos, sliders, prompts). The
  generic page class takes no input though (not even B), so a native page
  needs the factory hooked to give it an options class, plus code that reads
  and writes its widgets by name.
- The page factory's table (name -> size, constructor) is in sub_00028E10;
  gameplayeropt is 0x474 / sub_00028070, gamepicture 0x4A8 / sub_00027F30.
