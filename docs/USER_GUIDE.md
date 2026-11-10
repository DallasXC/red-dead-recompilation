# User guide

How to play Red Dead Revolver on PC with this port: the first start, the
settings, controls, the in-game menus, cheats, co-op and troubleshooting.
To build the game first, see the [README](../README.md#getting-started) or
[BUILDING.md](BUILDING.md).

- [The play folder](#the-play-folder)
- [Controllers](#controllers)
- [The launcher](#the-launcher)
- [Frame rate](#frame-rate)
- [Mouse and keyboard](#mouse-and-keyboard)
- [The in-game menus](#the-in-game-menus): Quick menu, PC Options, Go to Chapter
- [Cheats](#cheats)
- [Story co-op](#story-co-op-experimental)
- [Online co-op](#online-co-op-experimental)
- [The 2002 look](#the-2002-look)
- [Free camera](#free-camera)
- [Sharp text](#sharp-text)
- [Mods and texture packs](#mods-and-texture-packs)
- [Hotkeys](#hotkeys)
- [Known issues](#known-issues)
- [Troubleshooting and reporting a problem](#troubleshooting-and-reporting-a-problem)
- [Advanced: runtime switches](#advanced-runtime-switches)

## The play folder

`build.bat` makes a ready-to-play folder inside the project folder:

```
Red Dead Recompilation  Red Dead Revolver.exe          the game, built from your own disc
  SDL2.dll                       controller support
  Game\                          the game's files (linked to your extracted disc)
  Saves\  Mods\  Logs\  settings.ini
  Mods\Preview\                  the 2002 preview characters, once set up (launcher > Extras)
  Mods\Story People\             the story's people alone, once set up (launcher > Extras)
  Tools\mods\                    the tools that set them up
```

Start **Red Dead Revolver.exe**; it is the only thing to start. If the
`Game` folder has no game in it yet, the launcher opens on an **Install**
page: choose your Red Dead Revolver (USA) Xbox disc image (`.iso`) and its
files are unpacked into `Game\` (once, a few minutes, with the progress
shown), then the launcher carries on as usual. If the disc is already
unpacked somewhere, choose **Already unpacked?** and pick its `default.xbe`
instead: that folder is used as it is. (`build.bat` normally links
`Game\` to the disc it built from, so this is only needed for a play folder
moved to another PC. Dragging the `.iso` onto the exe works too.)

Settings live in `settings.ini` beside the exe; delete it to go back to the
defaults.

**Save backups.** Each start that finds the saves changed since the last
time copies `Saves\UDATA` and `Saves\TDATA` to `Saves\Backup\1` before the
game runs; the two copies before it move to `2` and `3`, and the oldest goes.
If a save is ever spoiled (a crash while saving, a full disk), close the
game and copy `UDATA` and `TDATA` from one of the backups back into
`Saves\`. Online games are not backed up (their saves are the host's, kept
in `Saves\Online`).

**Update check.** When the launcher opens, it asks GitHub
(api.github.com) once whether there is a newer release, and shows a link
if there is. Nothing is downloaded or installed, and nothing about you or
your game is sent. Switch it off on the launcher's About page (**Check for
updates**), or with `update_check = 0` in `settings.ini`. (Started with `play.bat` from the project folder instead, the
game uses the project's own `settings.ini`, `saves\`, `mods\` and `logs\`.)

## Controllers

Xbox, PlayStation (DualSense and DualShock 4, USB or
Bluetooth, no DS4Windows or Steam needed), Switch Pro and most other pads
work (through SDL2; Xbox pads also through XInput), in the launcher and in
the game, and can be plugged in while the game runs. The log lists what SDL
finds at start (`[INPUT] SDL 2.30.9: 1 device(s)`) and names each pad it
opens (`[INPUT] controller 1: DualSense Wireless Controller (SDL)`). The
original pad's two extra buttons are the shoulder buttons: **LB / L1 is
Black** (zoom with a rifle; with other guns, the special move) and
**RB / R1 is White** (next weapon). **Mouse and keyboard** can be switched on in the launcher's
Controls page: see below.

**Button pictures** (Controls): with a PlayStation controller the game's
prompts show the PS2 version's cross, circle, square and triangle (still
on the disc) instead of Xbox's A, B, X and Y: "As the controller" picks by
the first controller plugged in, or choose Xbox or PlayStation. The game's
own menus change from the next menu or level that loads; PC Options at once.
Playing on keys and mouse with no controller, the prompts show key caps
instead: in PC Options and in the game's own menus and prompts: **Enter**
for A and **Esc** for B (as the menus take them), and the keys you bound to
X and Y.

## The launcher

A settings window, the launcher, opens before the game every time, until
you switch **Show this window at every start** off on its About page. After
that it opens when you hold **Shift** while the game starts (or pass
`--launcher`). Its window can be dragged to any size or maximized; the
page scales to fit and, when taller or wider, shows more:

- **Display**: window, borderless window or fullscreen, window size (1280 x 720, 1600 x 900,
  1920 x 1080, 2560 x 1440 or 3840 x 2160; the next one down if it does
  not fit the screen), the game's own
  **widescreen** mode (16:9, with more of the world to the sides), keep the
  shape or stretch, the game's **black bars** at the top and bottom (off,
  in cutscenes only, or always), vertical sync, the **frame rate**, the
  frame counter.
- **Graphics**: internal resolution (1x-6x the original 480 lines), FXAA,
  anisotropic texture filtering, smooth or sharp upscaling, and **Dump
  textures** for making texture packs (see [MODDING.md](MODDING.md): replacement
  textures go in `mods\textures\`).
- **Controls**: vibration strength, stick deadzone, inverted camera axes,
  and **mouse and keyboard**: mouse sensitivity, invert up/down and
  left/right, and every
  control rebindable (select it, press the new key, mouse button or turn
  the mouse wheel; Esc cancels).
- **Players** (Controls page): which player the keyboard and mouse are, and
  which controller each of the four players holds (Controller 1-4 in the
  order Windows connected them, or None). For two-player Showdown on one
  PC with the keyboard and a pad: keyboard and mouse = Player 1, Player 1
  controller = None, Player 2 controller = Controller 1. Default: the
  keyboard and Controller 1 are Player 1, Controller 2 is Player 2. Adding
  a third or fourth player takes effect at the next start.
- **Mods**: each mod and texture pack switched on or off (see
  [Mods and texture packs](#mods-and-texture-packs)).
- **Extras**: the [2002 preview characters](#the-2002-preview-characters):
  set them up from the preview's disc image, and switch them on or off.
- **Play** starts the game, **Quit** closes the window.

## Frame rate

60 is the console's. 30 halves it. 120, 144, 165, 240 and
Unlimited run the game faster than the console ever did: the game times
its movement by the clock, not by frames, so it plays at the normal speed,
just smoother. The few things that counted screen refreshes instead (the
fades of the level title cards and transitions) are kept to the right
speed too: they are timed by the real time between frames, so they keep
their speed whatever the setting, the monitor or vertical sync do to the
frame rate. If anything still looks or behaves wrong, go back to 60. With
vertical sync on, the frame rate cannot go past the monitor's refresh
rate.

## Mouse and keyboard

The mouse turns the camera and aims directly, as
in a PC shooter: its movement goes into the game's camera as an angle
every frame (about 0.07 degrees a count at 100 % sensitivity), with no
deadzone, acceleration curve or top speed: a fast flick turns as far
as you moved. Aiming down the sights turns slower, as the game does with
a stick. Where the game uses a different camera (the gatling gun) the
mouse drives the right stick instead, found by itself. In a **duel** pull
the mouse back to grab the gun, push it forward to draw, then (from
the moment the cursor shows, slow motion included) the mouse
moves the aiming cursor itself, one to one (half the screen for 1600
counts at 100 %; "Duel aim sensitivity" in F1 scales it), left button to
place each lock, middle button to fire early. In the front end the desk's
book, Showdown poster and journal can be clicked: the one in front opens,
another is turned to (from the poster or the journal, a click on the book
or on the outer arrow goes back to the book). The **shop** before a
mission and the stores in town take the mouse too (the arrows at the top
change item, "Done" and "Buy This Item" are buttons), and so does the
**weapon select** (click a row to choose it, its arrows to change the
weapon, "Done"). In the **sheriff's journal** the mouse no longer turns the
pages; point at an entry to move the hand there and click to open it.
Both directions can be
inverted (Controls page). Left button fires, right button is the left
trigger (gun up). The **mouse wheel** changes weapon (up: previous, down:
next, as left and right on the d-pad). **E** is the action button (X on
the pad: doors, shops, talking, taking cover, reviving), **R** reloads (Y),
**F** is B (melee, back), Space is A (jump, dodge). **Mouse 5** or **Caps
Lock** starts Dead Eye (the right stick click; the middle button does too).
**Z** zooms a rifle (the original pad's Black button;
Q is Black too, for the special moves), and moving diagonally with the
gun up strafes at full speed. Every key is rebindable, mouse side
buttons and the wheel included. While the game has the focus, Windows'
Sticky Keys / Filter Keys shortcuts (five Shifts, Shift held) and the
Alt / F10 menu key are kept from taking the keyboard away from it, and a
key Windows forgot to release is released (the log says so with
"[KEY]").

### Typing a name

On the Gunslinger Profiles sheet (Change Name), type
the name: letters, Space, Backspace, Enter to accept, Esc to go back.
The controller's letter grid still works too.

### The mouse in menus

The pointer appears over the pause menu, its
Options pages and confirmation boxes, and in the front end over the
book's menu (New / Change Player / Player Options), the Gunslinger
Profiles sheet and the option pages: point at an item to highlight it,
left click to choose, right click to go back. On option rows click the
left or right half of the value to change it; drag a slider or click
along it. The button prompts along the bottom ("A Enter", "X Options",
"B Back") and the page arrows are clickable; the wheel steps the
highlight. In the Showdown line-up your hand follows the mouse; take the
pointer past the photo's left or right side to turn to the next page of
characters (the preview's and the story's: [the 2002 preview
characters](#the-2002-preview-characters)). In the
Sheriff's Journal and the line-up's small option menus the mouse steps
the highlight. Keyboard: Enter chooses, Esc or Backspace goes back; in
play Esc pauses.

## The in-game menus

### Quick menu

**F1**, or clicking **both sticks** on the controller,
brings up a narrow panel at the left with what is changed in the middle of
play: Invincibility, Infinite Dead Eye, Infinite Ammo, Freeze NPCs, the
Free Camera, Money, Enemy health, Story co-op (players and screen), the
frame counter: and **All PC Options** for everything else. The game stays
in view beside it. Esc, B or F1 closes it.

### PC Options

In the game's own pause menu (where Xbox Live was), or All
PC Options in the Quick menu: a list drawn as the game's Options page, each
entry a page drawn as its options pages, with the game's own fonts and
pictures read from your copy. Its pages: **Go to Chapter**, **Cheats**, **Spawner**, **Story
Co-op**, **Display**, **Graphics** and **Controls**. **Cheats** has the
game's own **cheats** (Invincibility, Infinite
Dead Eye, Golden Gun, Bite the Bullet, Sad Stars, No HUD, No Distortion; no
need to unlock them first), and our own: **Infinite Ammo** (clips
and reserves stay full: no reloading), **Money** (in a mission: add
or take away a step of $100 to $100,000 with left/right, or type an amount
in "Set to" and press Enter) and **Always Excellent** (every mission is
rated Excellent on the results screen, with what that unlocks), and
**Unlock Everything** (every Showdown character and stage, every journal
page in the Extras and the cheats, for all profiles; kept with your
saves; it asks for a second click, and **Undo Unlock Everything** locks
again exactly what it unlocked), **Play as** (on the Story Co-op page,
and in the chapter book under Player Options: story levels start with any of the 48 Showdown
characters instead of the chapter's own: their looks, moves and abilities;
from the next level that loads; click it for the whole list with a search
box (type part of a name) or step through with left/right; the names
are the game's own from the Showdown line-up), **Freeze NPCs** (everyone else stops
where they stand: no moving, shooting or noticing you), **Guns in Town**
(draw and shoot in Brimstone, where the story keeps your guns holstered; the townsfolk can be hurt and killed),
**Enemy health**
(100-500 %: story enemies take that much more shooting to bring down:
each hit does less, so health bars and the bosses' tricks work as ever;
Showdown is not changed), **Protect Allies** (the people on your side or
not in the fight (the barmaids in the saloon fight, the ones you escort
or guard, the townsfolk) take no damage), the **Free
Camera** and the **Showdown power-ups** (Speed Up, Untouchable, Health
Regeneration, Triple Damage, Everyone Is Drunk and the rest, by the game's
own names; the weapon ones (burning, venomous, explosive ammunition and
so on) under "Weapon power-ups"; what each does is written under its
name; worn by every player while switched on,
in the story too; one picked up in play takes their place until it runs
out), and the
Display, Graphics and Controls pages,
which change the running game at once (window size and widescreen wait for
the next start). Arrows/d-pad move, Enter/A changes, left/right step a
value, LB/RB switch pages, Esc/B go back a step. **F2** shows
or hides the frame counter, **F11** or **Alt+Enter** switches fullscreen.

### Go to Chapter

PC Options > **Go to Chapter**: A (or Enter) on a chapter, then
again to confirm. The level you are in is left and the chapter loads
straight away; your progress and profile are put back as they were.

## Cheats

All of these are on the **Cheats** page of PC Options (and the most used
ones in the Quick menu). The full descriptions are under
[PC Options](#pc-options) above.

| Cheat | What it does |
|---|---|
| The game's own cheats | Invincibility, Infinite Dead Eye, Golden Gun, Bite the Bullet, Sad Stars, No HUD, No Distortion: no need to unlock them first |
| Infinite Ammo | clips and reserves stay full: no reloading |
| Money | add or take away money in a mission, or type an amount |
| Always Excellent | every mission is rated Excellent, with what that unlocks |
| Unlock Everything | every Showdown character and stage, journal page and cheat (undo available) |
| Play as | story levels with any of the 48 Showdown characters |
| Freeze NPCs | everyone else stops where they stand |
| Guns in Town | draw and shoot in Brimstone, stores included; the townsfolk can be hurt and killed, as in the Mr. Kelley fight (shopkeepers too, so shop first) |
| Townsfolk Come Back | with Guns in Town: the people shot dead in Brimstone (shopkeepers, the paper boy and the rest) come back at their own places about a minute later |
| Enemy health | 100-500 %: story enemies take more shooting |
| Always Win Duels | in a duel against the computer the opponent's shots miss: take your time |
| Give Weapon | PC Options > Spawner > Weapons: any of the game's weapons, given with its ammunition (to the chosen co-op player) |
| Fill ammunition | PC Options > Spawner > Weapons: full clips and reserves for every weapon carried |
| Protect Allies | the people on your side or not in the fight (the barmaids in the saloon fight, the ones you escort or guard, the townsfolk) take no damage; enemies and players are not changed |
| Spawner | PC Options > Spawner: bring in the people the level has loaded but not brought in yet, in front of you (or a co-op player); see below |
| Showdown power-ups | the power-ups, worn by every player, in the story too |
| Free Camera | see [Free camera](#free-camera) |

### Spawner

PC Options > **Spawner** lists the people of the level you are in, by
name, with whether they fight you and how many are waiting to come in. A
level loads all of its people when it starts and brings them in as the
story goes on; the spawner brings in the ones still waiting, right now, in
front of you. **How many** sets how many come with each press (1, 2, 3, 5
or all); in story co-op **In front of** picks the player. Online, any
player can use it: they come in on every player's game at the same moment,
in front of whoever asked. Only the people the level has loaded can come
in, so each level offers its own, and once everyone is in there is nobody
left to bring in (people killed may come back in). Cutscene-only people
are not offered. The levels with animals (the ranch, the stagecoach, the
Mission chapters) offer their horses, cattle and buffalo the same way.
**New people (experimental)** makes someone who was never in the level:
pick a **Character** (one of the level's own people, or anyone in the
Play As list: the story's, Showdown's, the preview's, the mods'), how
they **Act** (like one of this level's people: walking about town, joining
the fight, guarding..., or, further down the list, like someone from
another chapter, whose ways are loaded for them) and their **Side** (as that person, against you,
or on your side), then **Make**. **How many** makes 1, 2, 3, 5 or 8 at
once, **Where** puts them in front of you, all around you or further
off, and **Guns** gives them guns as their side would have, none, or
guns for all; **Health bar** On shows each one's name and health bar at
the top of the screen, as the level's bosses have. **Remove the people made** takes everyone you made out of
the level again. Each level offers its own ways of
acting, so a fight is best made in a level with fighting in it; someone
from another chapter may need things this level does not have, so if
someone just stands there, try another. The level's own animals (horses,
cattle, buffalo on the ranch) are in the Character list too; they act as
that level's animals do, so a cow may wander off to its herd. Other
chapters' horses, cattle, buffalo and dogs are listed after them. A level can
only have so many people and animals in at once (the ranch: 15); the hint
under **Make** says how much room is left, and nothing more comes in once
it is full.
The same page's **Weapons** gives any of the game's weapons, with its
ammunition, to you (or the co-op player chosen there).

## Story co-op (experimental)

**Weapons and money** (Story Co-op page): **Shared**: one purse: what
anyone earns is player 1's to spend in the game's shop, and player 1's
weapons go to every player each chapter. **Each player's own**: players
2-4 keep their own money and weapons from chapter to chapter and buy
weapons on the **Co-op Shop** page (PC Options, or the Quick menu during
co-op) at the game's prices; online, each player shops on their own
machine. Player 1's money and weapons are the profile's, as ever.

**Duels** (Story Co-op page): **Player 1**: the story's duels are player
1's, as in a one-player game; the others wait. **Every player**: when
player 1 duels, players 2-4 duel the same opponent at the same time, each
on their own half of a split screen: draw and shoot together, and the
first good shot wins it for everyone. The story goes on from player 1's
duel (if player 1 is shot, the duel is lost as before).

PC Options > **Story Co-op**, the Quick menu, or **Story Co-op** in
the chapter book under Player Options (to set it up before a chapter): 2, 3 or 4
players in the story levels, each on their own controller (Controls page:
who holds which; player 2 holds Controller 2 by default, and a third or
fourth player with none takes the next free one: restart the game after
choosing 3 or 4 so their controllers are plugged in). **Player 2/3/4 as**:
each player's character: the story's own Red, Jack Swift, Annie
Stoakes, Shadow Wolf and Buffalo Soldier first, then every Showdown
character (player 1's is **Play as**, which offers the same five). **Co-op
screen**: Shared (everyone on player 1's camera) or Split (top/bottom or
side by side for two; quarters for three or four; cutscenes play full
screen, then the split comes back) or **Separate windows**: player 1 in
the game's window and every other player in a window of their own, each
the screen's own shape: drag one to another monitor, and **F11** or a
double click there makes it fill that monitor (again: back to a window).
Keys pressed in a player's window go to the game as usual; closing one
hides it until the screen setting changes. Each player can open the Quick
menu with their own controller (both sticks clicked): it opens over their
part of the split screen, or in their own window, worked by their
controller, while the others play on. One menu at a time. The pause menu
pauses everyone; it shows in the game's window and the window of the
player who paused. **Friendly fire**: off by default,
the players' bullets do not hurt each other. A player who is revived, or
falls out of the level, comes back just behind player 1, where player 1
has walked; one with no controller on their port follows player 1 along.
The screen changes at once; the players, and who they are, when the next
level loads, or now: **Restart Level** in the pause menu starts the
chapter afresh with them, and **Apply now** (Story Co-op page, Quick menu)
does the same. The keyboard and mouse belong to one player only (the
mouse no longer turns the other players' cameras in Showdown or co-op).

**A second keyboard and mouse** (Windows): two players on one PC, each
with a keyboard and a mouse. Plug in the second set, then in the game open
PC Options > **Controls**: set **Second keyboard and mouse are** to the
player who uses them (Player 2, say), choose **Pick the second keyboard
and mouse**, press any key on the second keyboard, then click with the
second mouse (Esc on the second keyboard if that player has no mouse of
their own). From then on the second set plays that player, with the same
keys as the first (the second mouse turns that player's camera), and the
first set no longer moves them. It is remembered until **Forget them**;
a keyboard or mouse plugged into a different USB port may need picking
again. The menus work from either keyboard. A player with no controller
who only now has the second set joins the next time the game starts. Not
online (each machine there has one player).

## Online co-op (experimental)

> **Highly experimental and still buggy**: games can freeze or fall out of
> step. To play together over the internet, local split-screen co-op
> shared through [Parsec](https://parsec.app) (one PC runs the game, the
> others join its screen with their controllers) is the reliable way for
> now.

Play Story Co-op with friends over the internet or a LAN. Everyone needs
the game (built from the same version of the port), and everyone plays
their own copy of it: only the controllers cross the network, so the
picture is sharp and there is no video lag.

1. **Open Online Co-op.** On the desk, press **Y** (top right, where the
   Xbox had its Friends Menu; clicking it works too), or PC Options >
   **Online Co-op**.
2. **The host** picks **Host a game** and how many players, and gets one
   **join code**. It holds every way to reach this PC, and the friends'
   game tries them all:
   - **at home**, on the same network;
   - **over a VPN** such as Tailscale or ZeroTier (the easiest way over the
     internet: nothing to set up on the router);
   - **over the internet** when the host's router forwards **UDP port
     27032** to this PC.

   Click the code to copy it, and send it to your friends. Each hosted
   game has a code of its own: it carries a key, so only someone with
   the code can join and receive the host's save. **Your IP address:**
   for play over the internet the code holds your PC's internet (public
   IP) address, and anyone who has the code can read it: every online
   game works this way, as the others' games have to know where to send
   their packets. Give the code only to people you trust. To keep the
   address out, switch off **Internet address in the code** before you
   host and play over a VPN such as Tailscale: the code then holds only
   addresses nobody outside your network or VPN can reach.
3. **The friends** pick **Join a game**, type or paste the code and join
   (the host's IP address alone works at home or over a VPN, not over
   the internet). The host's save and game options are sent to them;
   each shows **ready**.
4. **The host picks Start in and presses Start.** Every game starts
   again, together, on a copy of the host's save (`Saves\Online`: nobody's
   own saves are touched). **Start in** says where the games go:
   - **Continue the story** (the default) or **A new game**: they go there
     by themselves (past the logos, the title, the desk and the book)
     with the host's profile (the one played last), straight into the
     chapter's loading. Nobody needs to press anything meanwhile.
   - **The menus (by hand)**: the host's controller works the menus, as
     on one PC; pick a chapter and play.

   Online games leave out the game's movies (the pre-rendered cutscenes):
   they play at each PC's own speed, which parted the games. The menus
   before a mission (weapon select, the shop and Start / Weapon
   Selection / Shop) work as usual: the host's controller (player 1)
   buys and chooses, and every PC sees the same. Each player is a player in Story Co-op, on
   their own controller (or keyboard and mouse).

**Showdown online**: after the start, go to Showdown on the desk as
usual. In the line-up each friend presses A on their own controller to
join, and everyone picks a character; the host starts the match. Up to
four players, one per PC. Each player sees only their own part of the
split screen, over the whole window (two players: a wide strip, as on the
Xbox; three or four: a quarter). It is drawn at that part's size, so raise
**Resolution scale** (Graphics) for a sharper picture. A cutscene or the
pause menu shows the whole screen. Online Story Co-op works the same way:
each player sees their own quarter.

Everyone must have the same game files: the [2002 preview
characters](#the-2002-preview-characters) set up when the host has them on,
and the same mods switched on. The lobby checks this when a friend joins
and says what is different ("needs the 2002 preview", "different mods");
Start waits until everyone matches. Texture packs do not matter.

While playing online the host's options are used, and cheats, chapters
and co-op settings can't be changed (every game has to stay the same). A
line at the top of the picture says when the game waits for someone.
**Online Co-op > Leave the online game** goes back to your own game.

How it works: [ONLINE.md](ONLINE.md). (An older, experimental way, where
the host streams its picture to the others, is still there for testing:
`RDR_NET_HOST=1` on the host, `--join <address>` on the others; see
ONLINE.md.)

## Free camera

**F6** (or the toggle in the Quick menu) detaches the
camera from the game: left stick or WASD flies, right stick or the mouse
looks, right trigger / A (Space) rises, left trigger / X (E) sinks, the left
stick click (Shift) goes four times as fast. The game does not get the
controller while it is on (Start still pauses), and the HUD is hidden
(its counters are models in front of the game's camera); F6 again puts the
game's camera back where it was.

**Who flies it, who plays.** While the free camera is on, the Quick menu
(F1) and the Cheats page show a row under its switch for the keyboard and
mouse and one for each player's controller: **Flies the camera** or
**Plays**. A device that plays keeps moving its player under the free
camera: for filming: the keyboard and mouse on the camera, and one or two
controllers on the players in co-op. For a few seconds after F6 a line at
the top says who is flying it and who is playing. (By default everything
flies the camera, as before.)

## Sharp text

The game's own lettering (menus, subtitles, the money and
ammunition counts) is drawn four times finer than the console's: its font
pages are enlarged with clean edges as they load, and its main menu font,
Ray Larabie's Berylium, is drawn from its outlines. Graphics > **Sharp game
text** switches it off.

## The 2002 look

PC Options (or the launcher) > Display > **2002 look** gives the game the
look of the January 2002 preview, from art the finished game still has on
its disc (nothing to set up):

- **TIME** at the top left: how long you have been in this chapter.
- **Money** at the top right, with the preview's small cents.
- At the bottom left, the revolver's **rounds in an arc**, one gone with
  each shot (two arcs with a gun in each hand; other weapons show their
  picture), and the **ammunition** count.
- The **health bar**, green turning yellow and red as it runs down, the
  thin red line under it and the red **badge** at its end: the Dead Eye
  meter (the preview called it "valor"), 0 to 10.
- **Dark blue curved bands** at the top and bottom while playing
  (**2002 blue bands**, just below 2002 look, switches them off).
- The **pause menu** tinted brown, in the preview's lettering, with
  "Paused" at the bottom right (from the next chapter or level loaded).
- The **title**: the preview's red title picture ("RED DEAD REVOLVER"
  in black and a gunman's shape) with "Press Start", from the game's own
  title screen (after the intro) to the desk. It comes from your own preview disc, so it needs the 2002
  preview characters set up (launcher > Extras; set them up again if you
  did it before version 1.0.4).

The crosshair, the bosses' health bars and the prompts stay as they are.
In split screen the game's own HUD is kept.

## The 2002 preview characters

Before Red Dead Revolver came out, a preview of it ran on the PlayStation 2
("Red Dead Revolved", January 2002). Its characters and stages can be
brought into this game: **56 characters** (preview versions of Red, Annie,
O'Grady, Chicken, Pig Josh, a long-coat Red, Bat Boy who flies, and many
more), **31 stages** for Showdown, and the story's own people (Brimstone's
townsfolk, shopkeepers, soldiers...) as Showdown characters too. They can
also be played in the story through **Play As** and story co-op.

**What you need**: a disc image (`.iso`) of the January 2002 preview, your
own copy (it is not included and cannot be shared here), Python 3 (the
build installed it), and about 2 GB of free space.

**Setting them up** (once):

1. Open the launcher: it opens before the game every time unless you
   switched that off; then hold **Shift** while the game starts.
2. Go to **Extras** and choose **Set up the preview characters**.
3. Pick the preview's disc image in the window that opens. The setup runs
   by itself (5 to 10 minutes) with its progress on the page. The disc
   image is only read; everything made goes in `Mods\Preview\`.
4. When it says **Done**, the preview characters are switched on. Press
   **Play**.

**Using them**:

- **Showdown**: the line-up has more pages. Push the hand past the right
  edge of the photo (or click the arrows) to turn to the next page: the
  preview's characters are on page 2, the story's people on page 3 (on
  page 2 when the preview's are off: a page with nobody on it is skipped). The
  preview's stages are in the stage list, marked (2002) where the retail
  game has a stage of the same name.
- **Story**: F1 > **Play As** (or the chapter book's Play As) lists them
  after the game's own characters; pick one and start or restart a level.
  Story co-op's **Player 2-4 as** offer the same list.
- **On and off**: launcher > Extras > **Preview characters**. Your saves
  are the same either way. Off, a level or match never sees them; a Play
  As choice of one waits until they are on again.
- **Online co-op**: the game checks, when a friend joins, that they have
  the preview characters exactly as the host has them (on or off) and the
  same mods switched on. If not, the host's lobby says "needs the 2002
  preview" or "different mods" beside them, and their screen says what to
  set up; Start waits until everyone matches.
- **Removing them**: launcher > Extras > **Remove the preview
  characters** (select it twice). This deletes `Mods\Preview\` only.

**The story's people without the preview**: launcher > Extras > **Set up
the story's people** makes Brimstone's townsfolk, the soldiers, Atlas
(Professor Perry's strongman) and the rest from your own copy of the game
(no preview disc needed, a few minutes) into `Mods\Story People\`.
With the preview characters off, Play As, story co-op and Showdown offer
them from there. (With the preview characters on they come from
`Mods\Preview\`; set the preview up again to add Atlas there.)

**Good to know**: these characters were made for an earlier version of the
game, so they borrow a retail character's moves and voice where the
preview's own do not fit, and a few look stiff (the long coat does not
swing). Five of the preview's 61 character entries cannot be brought over:
the preview's disc has no 3D models for them, only their settings (its
entries `ann`, `jim`, `fat`, `rwc` and `slm`).

## Mods and texture packs

Each mod goes in a folder of its own inside `Mods\`, with its files where
the game has them (`Mods\My Mod\swat.dat`, `Mods\My Mod\lvls_m2\...`): they
are used instead of the game's own. Each texture pack goes in a folder of
its own inside `Mods\textures\` (`.tga` files named by the texture's code).

The launcher's **Mods** page lists them all, each with an **On/Off**
switch (also in the game's PC Options, from the next start). A mod or pack
you add is on until you switch it off. When two change the same file or
texture, the one higher in the list (by name) is used. **Open the Mods
folder** and **Open the texture packs folder** show where to put them.
Files put straight into `Mods\` or `Mods\textures\`, as before, show as
**Loose files in Mods** and **Loose textures**, and switch on and off the
same way.

Graphics > **Dump textures** writes the game's textures out to start a
pack from; `Tools\mods\upscale_pack.py` turns that dump into an HD pack
(an AI upscaler, or a clean enlargement), and the game's movies can be
replaced with video files in `Mods\videos\`: both in
[MODDING.md](MODDING.md#an-hd-pack-made-from-your-own-textures). Making mods (new Showdown stages and characters, editing the
game's files) is in [MODDING.md](MODDING.md). Everyone in an online game
should have the same mods on.

## Hotkeys

| Key | What |
|---|---|
| F1, or both sticks clicked | Quick menu (Esc, B or F1 closes it) |
| F2 | frame counter |
| F6 | free camera |
| F11 or Alt+Enter | fullscreen |
| Shift held while the game starts | the launcher |

## Known issues

What is known and not fixed yet (the [changelog](../CHANGELOG.md) has what
was):

- **Story co-op** is experimental: the story's scripts were written for one
  player, so a few scenes put player 2 back beside player 1, and some
  conversations and cutscenes are player 1's alone.
- **The 2002 look**: the game's own money box can still appear for a
  moment next to the 2002 money (at a chapter's start, or when money is
  paid).
- **Spawned Jody** (the paper boy, made with the Spawner): on some graphics
  cards he brings white streaks and shapes across the picture. The level's
  own Jody is fine. Not solved yet.
- **Online play** is highly experimental: everyone needs the same build,
  the same game files and the same mods on, and games can still freeze or
  fall out of step. Split screen shared through Parsec is the reliable way to
  play together for now.
- **Separate windows** (co-op): each player's view is drawn at a quarter of
  the internal resolution; raise Graphics > Render scale to keep it sharp.
- **Chapter 8 (Rogue Valley)**: after Go to Chapter, Red was once seen not
  taking damage; the log now notes when the game's own "cannot be hurt"
  switch changes (`[HEALTH]`): please send it if it happens.
- **New people (Spawner)** is experimental: people acting like someone
  from another chapter may stand still where that chapter's paths or
  scripts are missing, and some (the bridge's gunners) are not made at
  all where their cannon is missing.
- **Replacement movies** need Windows' own video decoder (any Windows 10
  or 11).
- **The stagecoach chase above 60 frames a second** (Chapter 14, Sunset
  Canyon): the coach is thrown off its track and flung around the map,
  and the chapter cannot be finished. Until it is fixed, the frame rate
  is held at 60 in that chapter whatever Display > Frame rate says (the
  option says so while it is held), and your setting comes back when you
  leave the chapter.

## Troubleshooting and reporting a problem

- If the game stops on an error, a box says so and offers to **copy the
  log** to the clipboard (paste it into your report) or **open the Logs
  folder**.
- Every start writes its log to `Logs\last_run.log`; the start before it
  is kept as `Logs\previous_run.log`, so a crash log is not lost to the
  next start. Please attach the log to a bug report, with what you were
  doing (a short video helps a lot).
- Something looks or plays wrong at a high frame rate: go back to 60.
- **A laptop or integrated graphics** (Intel UHD and the like) stutters:
  set Graphics > **Render scale** to 1x and anisotropic filtering to 4x or
  less, and keep the frame rate at 60. Plug the laptop in and use its
  best-performance power mode.
- **A laptop's touchpad does not move the camera:** Windows stops the
  touchpad while keys are held (so it is not moved by a palm while
  typing), and the movement keys are held all the time in a game. In
  Windows Settings > Bluetooth & devices > Touchpad > Taps, set
  **Touchpad sensitivity** to **Most sensitive**. Touchpads that report
  where the finger is rather than how far it moved work from 1.0.3 on.
- Settings in a bad state: delete `settings.ini` to go back to the
  defaults.
- A pad is not found: the log lists what SDL found at start
  (`[INPUT] SDL ...: N device(s)`) and each pad it opens.

## Advanced: runtime switches

The ones this game needs (audio codec, pushbuffer execution, single guest
CPU, USB pads, …) are switched on by `src/main.c` automatically; see
[dev/BRINGUP.md](dev/BRINGUP.md) for why. These are the ones you may want to add:

| Variable | Effect |
|---|---|
| `RECOMP_DASH_RETURN=0` | quit when the game asks for an Xbox dashboard page (e.g. Xbox Live "New Account") instead of staying in the game |
| `RECOMP_GPU=0` | draw on the CPU (the old software rasteriser) instead of the graphics card |
| `RECOMP_GPU_VERBOSE=1` | log each render target and shader the graphics-card renderer creates |
| `RECOMP_RASTER_THREADS=N` | threads for the software rasteriser when `RECOMP_GPU=0` (default: CPU cores minus 2, at most 16) |
| `RECOMP_FULLSCREEN=1` | fullscreen (normally set from settings.ini, as are the next ones) |
| `RECOMP_WINDOW_SIZE=WxH` | the window's size, e.g. `1920x1080` |
| `RECOMP_WIDESCREEN=1` | tell the game the TV is 16:9 |
| `RECOMP_RENDER_SCALE=1-8` | internal resolution, times 480 lines |
| `RECOMP_FXAA=1`, `RECOMP_ANISO=1-16` | FXAA; anisotropic filtering |
| `RECOMP_VSYNC=0/1`, `RECOMP_FPS_CAP=n` | vertical sync; frame-rate limit |
| `RECOMP_VBLANK_HZ=30-500` | the rate the game's vertical blank ticks at, which sets its frame rate (60 on the console) |
| `RECOMP_MODS_DIR=path` | read files from here in preference to the disc folder (the play folder's `Mods\`) |
| `RDR_NO_SAVE_BACKUP=1` | do not copy the saves to `Saves\Backup` at start |
| `RDR_ANY_XBE=1` | start with a `default.xbe` that is not the USA version 1 the port was made for (it will most likely not work; for testing only) |
| `RDR_UI_PARCHMENT=1` | the menus in their own parchment look instead of the game's art |
| `RDR_NATIVE_MENUS=0` | leave the game's own menus alone (no PC Options, no Play As / Story Co-op in the book) |
| `RDR_SHARP_TEXT=0`, `RDR_VECTOR_TEXT=0` | the game's lettering as on the console; berrylium enlarged from its bitmap instead of drawn from Berylium's outlines |
| `RECOMP_ASPECT=stretch` | fill the window |
| `RECOMP_GL_PRESENT=0` | show frames the old way (through guest memory) instead of straight from the graphics card |
| `RECOMP_WATCHDOG_SECS=N` | after N seconds, dump the guest call stack and stop (debugging; it fires whether or not the game is stuck) |
| `RECOMP_MISSING_FILES=1` | log every file the game asks for and doesn't find |
| `RECOMP_FB_DUMP=prefix` | write framebuffer BMPs (debugging) |
| `RECOMP_READ_LOG=1` | log every file read (debugging; slows loading) |
| `RECOMP_DRAW_LOG=1` | log a line every 200 draws (debugging) |
