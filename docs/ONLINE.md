# Online play

Red Dead Revolver has no online play of its own (no Xbox Live, no system
link). Two ways are built; both sit on the game's own pad reads. The main
one is lockstep (option 3), started from the lobby on the desk; host
streaming (option 2) is for friends without the game.

## The lobby (src/net/rdr_lobby.c)

The desk's "Friends Menu" (Y; the prompt page roominstr.swui, whose update
sub_00039840 is hooked: its words become "Online Co-op", and its Y opens
the F1 menu's Online Co-op page instead of the empty Xbox Live page) and
PC Options > Online Co-op.

- The host opens UDP 27032 and shows one join code: every address of
  this PC that a friend could reach (the internet's view of it, from a
  STUN request to a public server; home networks; a VPN's 100.64/10), at
  most three, in Crockford base32 with a check letter ("EJ8CS-02G0H-E0":
  the bits are the count, then each address as a 2-bit kind (192.168/16,
  10/8, 100.64/10 or any) and the bits the kind leaves; 5 letters for a
  LAN address alone). The joining game sends its hello to every address in
  the code and keeps the first to answer; the hello carries a random id, so
  a guest that reached the host two ways (a router's hairpin) is one
  player. A friend can also type an address.
- The host sends each guest its save (UDATA and TDATA, packed) and its game
  options (Play As, co-op characters, players, screen, friendly fire, enemy
  health, Guns in Town, Protect Allies, widescreen) in 1000-byte parts, asked for again
  until all are in; the checksum agreeing, the guest is ready.
- Start: every machine writes the save into Saves/Online, a copy of its own
  settings.ini there, and starts the game again (the same exe and
  arguments) with RECOMP_SAVE_DIR, RDR_SETTINGS and the lockstep's
  variables set: RDR_LS_HOST=<players> or RDR_LS_JOIN=<host> with
  RDR_LS_SLOT (the lobby's player number), RDR_LS_SESSION (the game's id:
  the host ignores another game's hello), RDR_ONLINE_GAME (the host's
  options, applied over the settings file at the start). Leaving the
  online game starts the game again without them.
- Start in (the host's lobby; lobby.c rdr_lobby_set_start): the host's
  game is started with RDR_ONLINE_START (1 the book's Continue, 2 its New;
  unset: the menus by hand). Only the host's game acts on it
  (rdr_overrides.c, online_start_page / rdr_online_start_tick): it
  presses the host's own controller (into its pad report as it is sent)
  (Start through the logos and the title, A on the desk once its Enter
  prompt shows, the d-pad and A on the book, A on New's difficulty, Yes on
  its warning, Begin on the chapter page) and those presses reach every
  game as the host's pad, so every game goes the same
  way frame for frame; nothing is written to the game. It stops when a
  level other than lvl_front is named, or after five minutes.
- In the online game what would change one machine's game alone is
  locked: the cheats, Go to Chapter, the co-op settings, the free camera,
  the pause when the window loses the focus.

- The same game files everywhere: every machine runs the whole game, so a
  mod on one and not the other would part them at the first frame that
  reads it. The host's options carry whether it plays with the 2002
  preview's characters (11) and a fingerprint of its mods (12:
  rdr_mods_fingerprint: what the mods folders it uses serve, each file
  by its path and size, an archive by its directory of names and unpacked
  sizes; texture packs are left out, they only change the picture). A
  guest works out its own for the host's preview setting and, when they
  differ, says what to do and sends LB_MISMATCH (2: lacks the preview, 3:
  other mods) instead of READY; the host's roster shows it and Start waits
  for every guest to be ready. (Lobby version 3.)

Harness (two copies on one machine): host, join by code (also a code of
three addresses, one unreachable and two reaching the host: one player,
by the first to answer; a host with the preview's characters on and a
guest without them: "needs the 2002 preview" on both screens, Start held;
the same mods on both: the fingerprints agree and the game starts), the
save across
(both machines' Saves/Online the same), start, both started again and
joined the lockstep as players 1 and 2, and stayed the same through the
menus into chapter 1.

## The pad layer

Every frame the game reads its pads in sub_00047C40 (from the main loop,
after the clock, sub_00083670): XGetDeviceChanges (0x003BE81F) for pads
that came or went, XInputOpen (0x003BE692) / XInputClose (0x003BE6E8) for
each port, and per open port sub_00047940, which calls XInputGetState
(0x003BE6F4) and turns its XINPUT_STATE into the pad record at
0x004041A0 + port * 0x38 (+0x24 the digital buttons, +0x34 the handle).
These XDK calls are wrapped (src/rdr_hooks.c): a port can be given to
something other than a real pad (an online guest, or a recording)
with a virtual handle (0xF00D0000 + port) that the game opens, reads and
closes like any other.

## Option 2: the host streams (src/net)

The host runs the game; a guest runs the same exe as a viewer:

    "Red Dead Revolver.exe" --join <host>[:port] [--name <name>]

which starts no game: a window (SDL2) shows the host's picture, plays its
sound, and sends the controller (or keyboard) to the host 60 times a
second. On the host (`RDR_NET_HOST=1`; the play folder's "Host Online Game.bat"
and "Join Online Game.bat" are gone, the lobby replaces them) each guest
that says hello gets the lowest free pad port and from then on is a pad
there: Story Co-op with one more player per guest. UDP, port 27031.

- Picture: the renderer's present tap (toolkit `xbox_present_tap`, after
  the overlay, before the swap) reads the window back; a worker thread
  scales it to the stream size (default 960x540, `RDR_NET_SIZE`), makes a
  JPEG (stb_image_write, quality `RDR_NET_QUALITY`, 70) and sends it in
  1100-byte datagrams; `RDR_NET_FPS` (30) pictures a second. A picture
  missing a part is skipped.
- Sound: the mixer's tap (toolkit `xbox_audio_tap`, each 256-sample period,
  48 kHz s16 stereo) goes out as it is (1.5 Mbit/s).
- Guests: HELLO until WELCOME (port, picture size); PAD every 16 ms; BYE,
  or 5 s of silence, frees the port.

No NAT traversal yet: the host forwards UDP 27031, or both are on a LAN
or a VPN (Tailscale, ZeroTier).

## Option 3: lockstep (src/rdr_lockstep.c)

Every machine runs the whole game and only the pads cross the network;
that works only if the same pads make the same game, frame for frame.

### Making the game repeatable

- **The clock.** Everything the game times reads the CPU tick count
  (sub_00084880, rdtsc, 733 MHz on the console), and the XDK the kernel's
  performance counter and system time. In lockstep all three read a clock
  made of frames: each lockstep frame starts 1/60 s after the last, and
  each read by a thread adds 1000 ticks of that thread's own (so how often
  one thread read never changes another's time). The game seeds its random
  numbers from this clock at boot (0x00018161). Its own fixed-step switch
  ([0x0065AA9D], a developer flag nothing sets) held the game on the legal
  screen, so it is not used.
- **The pads.** The pad is read more than once a frame; a press landing
  between two reads was seen by one and not the other. One state a frame:
  the frame's first read.
- **Loading frames do not count.** The game's clock (sub_00083670) runs in
  the main loop, in the loading screens' loops (0x00066291, 0x00066509),
  on the loading screen's own thread (0x0006B7EF, while the main thread
  loads) and during the level loader's work (sub_0006AF70, its own thread).
  How many of those frames a load takes is the machine's speed, so the
  lockstep frame (the one pads, recordings and fingerprints go by) counts
  only the others. (Holding the main loop while the loader works instead
  deadlocked: the loader needs it.)
- **Fades.** A screen fade runs on its own thread (sub_00105B40, from
  0x00106720, [0x00663338] set meanwhile); the frame waits for it, handing
  the CPU over as NtYieldExecution does. The fades draw their grain from
  the game's second random-number generator (0x0040C094) once per fade
  frame at the host's pace (picture noise, not play) so it is left out of
  the fingerprint.

### The tools

- `RDR_LOCKSTEP_RECORD=<file>` records, per lockstep frame, every pad
  state the game read and every pad that came or went;
  `RDR_LOCKSTEP_PLAY=<file>` plays it back on virtual handles.
- `RDR_LOCKSTEP_HASH=1` prints each frame's fingerprint (the frame, the
  game's random numbers 0x00652A70, every player's place and health) and
  where frames come from; tools/lockstep_compare.py shows where two runs
  part.
- Online: `RDR_LS_HOST=<players>` and `RDR_LS_JOIN=<host>` (UDP 27032,
  `RDR_LS_DELAY` frames of input delay, 3). Every player's pad is a
  virtual pad on its port from the start (the host 0, guests in order);
  each machine reads its own controller (port 0 of the toolkit: the first
  pad, the keyboard) for frame + delay and sends its last eight to the
  host, which passes them on; no frame starts until every player's pad
  for it is in. Every 30th frame's fingerprint is compared:
  "[LS] DESYNC" in the log when they differ (and a line at the top of the
  picture; a fingerprint from a machine a few frames ahead waits for
  ours). The local pad is the toolkit's report for USB pad 0 (the host
  pad, or the keyboard and mouse through the game's own mapping) so
  keyboard play works online too. The kernel's performance counter runs
  at the console's 733 MHz on every machine (its frequency too).

### What the tests found (harness, one machine, same build)

A run recorded from the boot through the menus into chapter 1, and its pads
played into a second run, compared frame by frame:

1. Identical for 2,337 frames, parting at a Start press (two reads a
   frame). Fixed.
2. Then ~200 frames later with no input there: the screen after the legal
   screen came at a different frame. With fades held, the main menu still
   came at different frames (2,209 against 2,217): loading.
3. With loading frames left out and the kernel's clocks made of frames
   too: both runs reach the main menu at the same lockstep frame and draw
   the same random numbers in the same order; leaving the fades' grain out,
   every frame of the 2,741 compared is the same.
4. With the gameplay: a recording through the menus, the chapter 1 load and
   the tutorial (the stick turning the camera) played back: all 3,615
   frames the same.
5. Online, host and guest on one machine (UDP, 3 frames of delay), each
   with its own pad: with one player, ~12,000 frames the same through the
   menus, the load and Red walking the fenced path; with two (Story
   Co-op, the guest player 2), the same, each player moved by his own
   machine's pad and both machines drawing the same split screen.

6. From the lobby (above): the save and options sent, both started again
   and in step.

7. Showdown: through the desk to the line-up, the guest joins with its
   own pad as player 2, both pick, the host starts Twin Rocks; the match
   loads (its loading screen's "press A", below) and both machines stay
   the same. Each shows only its own player's view.
8. Three players (host and two guests, three games on one machine): all
   join the line-up with their own pads, the match on Twin Rocks loads,
   the three games' fingerprints are the same (frame 5550, all three
   moving), and each shows its own quarter over the whole window.
9. Four players (host and three guests, four games on one machine, 0.97):
   all four join ("everyone in: 4 players"), pick on the line-up, and the
   four-player hanging stage (lvl_hng4) loads; the four games'
   fingerprints match through the menus, the pause menu (shown whole on
   every screen) and play with all four moving and shooting (frames 900 to
   4300), and each shows only its own player's view over the whole window.
   About 850 MB a game. Two host cores for four games ran each at 2-5
   frames a second, so the "waiting for the other players" line showed
   now and then; on four PCs each runs its own game. Not yet tried on four
   real PCs.

### Showdown online

- Showdown's loading screen (sub_000685A0, on the level loader's thread)
  waits for A or Start once its load is done. Its frames are loading
  frames, where the pads do not move; online, its pad reads see A pressed
  and let go in turn, so each machine goes on when its own load is done.
- Own view: each machine presents only its own player's view rectangle
  (toolkit xbox_PresentSetViews with one rectangle; rdr_overrides.c
  online_view). Player k has view k; its port is its pad object's
  ([player+0x894], one of four at 0x006795C8). The views themselves stay
  where the game puts them on every machine, so every machine draws, and
  plays, the same game.

Still to do: tested only on one machine so far (two copies); no check yet
that both run the same build (a protocol version only; a different
build shows as "the games parted"); no way through a router without a
forwarded port or a VPN.
