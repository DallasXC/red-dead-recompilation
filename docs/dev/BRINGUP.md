# Bring-up log

Target: boot → Rockstar logo (`bik/rslogo.bik`) → title movie → main menu.

Bring-up host: Linux x86-64 (2 cores), GCC 13, xboxrecomp v0.16.0
(`f0f8a46`), OpenGL backend under Xvfb, frames checked through
`RECOMP_FB_DUMP`. Nothing below is Linux-specific unless it says so.

## Pipeline numbers

| Step | Result |
|---|---|
| disasm | 19,368 functions, 1.35M instructions (~10 min) |
| func_id / abi | 15,209 classified; 4,232 `__thiscall` |
| names | 101 from the game's own log strings (`names/string_names.json`) |
| recomp | 19,368 translated, 0 failed, 2.47M lines in 78 chunks (~3 min) |
| build | 149 steps, ~4 min on 2 cores with GCC `-O1` |
| kernel | 148 imports, all routed, no ordinal remap |

## Milestones

| # | Milestone | Status |
|---|---|---|
| 1 | Runtime + generated code compile and link | ✅ |
| 2 | Entry point runs, CRT init, save dirs (`TDATA/UDATA\5454001a`) | ✅ |
| 3 | D3D device, `swat.dat` TOC read, DirectSound up | ✅ |
| 4 | Rockstar logo movie plays (Bink → NV2A, correct colour) | ✅ |
| 5 | Title movie plays to the title card | ✅ |
| 6 | Legal screen accepts input | ✅ |
| 7 | Main menu (bookshelf, "Player in Progress") | ✅ |
| 8 | New game: tutorial level loads and runs (Red at the ranch, targets) | ✅ |
| 9 | Legal screen and front end render correctly | ✅ |
| 10 | Playable frame rate | in progress: GPU renderer (item 19); 6-8 fps in game on a 7950X before it |

## Runtime switches this title needs

`src/main.c` (`rdr_default_env`) sets these unless you set them yourself.

| Switch | Why |
|---|---|
| `RECOMP_AC97_READY=1` | DirectSoundCreate waits for the AC'97 codec; without it `dxSystem::Init` hangs in DSOUND |
| `RECOMP_PB_EXEC=1` | the pushbuffer must actually execute or nothing is drawn and GET never moves |
| `RECOMP_PB_WALK=dma` | faithful DMA walk; GET published per command |
| `RECOMP_VBLANK=paced` | D3D's flip/vblank ISR |
| `RECOMP_TITLE_KEVENTS=1` | D3D 5849 keeps a pre-built KEVENT in `.data` (0x00348FCC); its vblank ISR sets it. Without this the bridge treats the guest address as a host handle → SIGSEGV in `SetEvent` |
| `RECOMP_GUEST_LOCK=1` | one guest CPU. Bink starts its reader thread before filling the thread's context and relies on the creator running first, as on the console. On a multi-core host the reader read a stale handle (`0x900`) from recycled memory and crashed when the title movie opened |
| `RECOMP_USB=1` | controller through emulated OHCI |
| `RECOMP_KEYBOARD=1` | keyboard as pad 1 |
| `RECOMP_CONTIG_AVOID_IMAGE=1` | keep contiguous memory out of the image's physical range, so the USB controller model cannot mistake a descriptor for a game global (crash B) |
| `RECOMP_FB_WINDOW=1` | the game window; without it frames are drawn but never shown (the first Windows run played the logo's audio with no picture) |

## Fixes

Each entry: symptom → cause → fix → where.

1. **Hang in `dxSystem::Init` → DSOUND** (`sub_003987E8`). DirectSound waits
   for the AC'97 codec. → `RECOMP_AC97_READY=1`.

2. **SIGSEGV at guest `0xFE801100` (APU registers), Linux only.** The toolkit
   unmaps the APU aperture to trap it, but `xbox_PosixMmioFault` only had a
   decoder on arm64; on x86-64 POSIX every APU access was a crash. → new
   `mmio_ctx_t` in `mmio_decode.h`: the Windows `CONTEXT` on Windows, a
   register struct filled from `ucontext_t` on x86-64 POSIX, so the one
   x86-64 decoder serves both. APU hook routed through it.
   `patches/xboxrecomp-posix-x86_64-mmio.patch`. The toolkit's own
   `tests/mmio_decode` suite passes against it on Linux.

3. **Spin in DSOUND `sub_0039F275`, Linux only.** AC'97 channel reset: write
   RR (bit 1) to `0xFEC0010B+ch`, read back once, spin while set. On Windows
   and arm64 the toolkit's write trap clears RR as the write lands; on x86-64
   POSIX that trap was disabled. → same `mmio_ctx_t` bridge for the AC'97
   page. (Same patch.)

4. **SIGSEGV in `SetEvent` from D3D's vblank ISR** once the pushbuffer ran.
   Static KEVENT in D3D's data. → `RECOMP_TITLE_KEVENTS=1`.

5. **Freeze on the first Bink frame** in D3D's two-frames-ahead throttle
   (`sub_0033CCE0` → `sub_0033C9D0`, loop at `0x0033CB07`). D3D polls the
   word at `device+0x30` (device at `[0x00347208]` = `0x00347210`; word at
   `0x80001000`) for the GPU's completed count. The executor writes
   `BACK_END_WRITE_SEMAPHORE_RELEASE` only to an address set with
   `nv2a_pb_set_semaphore_target()`, which nothing had set. →
   `src/rdr_overrides.c` binds it from the device once D3D has filled it in.
   **This is the fix that got the Rockstar logo on screen.**

6. **SIGSEGV in Bink's reader thread** (`sub_00380F20`) when the title movie
   opened: waited on token `0x900`. → `RECOMP_GUEST_LOCK=1` plus the patch's
   "creator keeps the CPU across PsCreateSystemThreadEx". This makes it rarer
   but has **not** fixed it (see Open issues).

7. **`stream0.dat`–`stream4.dat`** are all opened at startup; the game probes
   `stream5.dat` too and does not need it.

8. **No controller, Linux only.** OHCI had no x86-64 POSIX decoder either and
   switched itself off. → OHCI routed through `mmio_ctx_t` as well. (Same
   patch.)

9. **Freeze on the legal screen** (the screen stays, sound and input still
   alive). Not input: the pad was registered and Start reached the game's pad
   table. The main thread was asleep in D3D (`KeWaitForSingleObject` at
   `0x0033CB2D` on the KEVENT `0x00348FDC`). When D3D finds the GPU far
   behind (the legal text is ~1,500 draws) it puts a *software method*
   (`NO_OPERATION` with value 5) in the pushbuffer and sleeps until the GPU's
   interrupt for it has been handled; D3D's PGRAPH handler (`0x0033FA00`) then
   sets that event. The runtime raises no PGRAPH interrupt (its registers are
   plain memory, so D3D's write-1-to-clear ack cannot be seen), so nothing
   ever set it. → Toolkit patch: the pushbuffer walk calls a title hook when
   it reaches a software method (`nv2a_pb_set_sw_method_hook`), and
   `xbox_KeSetEventFromHost` sets a guest KEVENT from it. Title:
   `rdr_overrides.c` sets `0x00348FDC` for method 5. Found with the new
   `RECOMP_THREAD_DUMP`.

10. **Crash loading a new game** (`enmManager::Load` → wild pointer). A
    571-arm `switch` (`sub_000FC1C0`) was lifted with only its first 256 arms;
    the rest went out as unresolvable indirect jumps
    (`[ICALL] Failed to resolve VA 0x000FE334`). → Toolkit patch: jump tables
    are read up to 1,024 entries.

11. **memcpy dropping 3-byte tails.** CRT memcpy's trail-byte table
    (`0x001890CC`) has an arm the disassembler never made a function, so every
    copy ending in three odd bytes copied nothing
    (`Failed to resolve VA 0x00189104`, hundreds per run). →
    `names/extra_functions.json` adds it; `tools/apply_names.py` merges it
    into the function list before lifting. Three arms of the CRT's x87
    special-case tables (`0x0018C0E4`, `0x0018C0EC`, `0x0018C0FA`, reached by
    `jmp [ebx]`) were missing the same way and are in the same file. Any
    `[ICALL] Failed to resolve VA` in a log is a candidate for this list.

12. **Legal screen text garbled** (letters missing, "R k t G m"). The
    rasteriser classed any batch under 8x8 pixels as object space and skipped
    it; every small glyph quad (a, c, e, o, r, s...) vanished. → Toolkit
    patch: small *and* near the origin. Found by dumping the font textures
    (`RECOMP_TEX_DUMP`, they decode fine) and tracing the frame
    (`RECOMP_FRAME_TRACE`).

13. **Block of vertical stripes over the legal text.** Two font quads with a
    black diffuse colour and texture coordinates ~36,000 texels out (black on
    black on the console). The screen-space path draws the bare texel, so
    they tiled the font page. → Toolkit patch skips that shape only.
    (Running that path through the combiners instead darkened the menu's
    book pages; left for later, see the comment in `raster_indexed`.)

14. **5-6 fps in the menus on a 16-core PC.** The NV2A is a software
    rasteriser. Measured here (`[GPUPROF]`, gdb sampling): the executor
    thread was busy the whole frame, half of it in the register-combiner
    interpreter and a third in texture sampling, and the menus' full-page
    layers (the screen-space path) never used the raster thread pool. →
    Toolkit patch: combiners decoded once per program (bit-exact against the
    old evaluator over 420M pixels, `RECOMP_RC_CHECK=1`), Morton tables for
    swizzled textures, no divides in power-of-two wrapping (together ~1.6x
    per thread); screen-space triangles now go to the pool too, and the pool
    defaults to cores - 2 (was cores - 4) and also runs on Linux/macOS.
    `RECOMP_RASTER_THREADS=<n>` overrides.

15. **Printed photographs drawn as negatives** (the Twin Rocks poster, the
    portrait in the menu book). They are white images in a texture's alpha,
    inked by the register combiners with a dark constant; the screen-space
    path drew the bare texel. → It now runs the combiners like every other
    batch, with a fog factor of 1 (a zero factor turned every page whose
    final combiner mixes in fog into the fog colour: black pages).

16. **No title-movie audio.** Its DirectSound buffer is contiguous memory
    whose physical number a heap buffer had also been named with
    (`MmGetPhysicalAddress`); the APU model resolved the number to the heap
    copy, which is silent. First fixed with `RECOMP_APU_PREFER_WINDOW=1`,
    which broke the opposite case (item 18).

17. **Rasteriser scaling on many cores.** Two per-pixel writes to shared
    memory (a statistics counter beside the render state, and the pool's
    per-thread counters packed into one array) kept one cache line moving
    between all 16 pool threads. Measured on the user's 16-core PC before
    the fix: a movie frame took longer per thread than one thread alone.
    Also: rows are walked by span instead of bounding box (movie playback
    2-2.5x on one thread), decoded-texture cache, integer blends.

18. **Loud static in the menus and in game.** The in-game music and effect
    banks live in heap pages whose physical numbers are also live
    contiguous blocks; item 16's "prefer the window" sent the APU to the
    wrong copy, which it played as PCM. Both copies hold live sound at
    once, so no rule about a bare number can be right. → (toolkit)
    `MmGetPhysicalAddress` now hands out a separate alias number
    (`0x08000000 | page`) for a heap page whose number is a live window
    block; the APU, OHCI and pushbuffer models read the alias as heap.
    `RECOMP_APU_PREFER_WINDOW` is no longer set. Checked headless: menu,
    intro movie and the first chapter play tonal audio, and the voices that
    read through the alias are clean (`RECOMP_APU_TRACE` VSTAT).

19. **GPU renderer.** The NV2A's pixels now go to the host graphics card
    (OpenGL 3.3, `src/kernel/nv2a_gl.c` in the toolkit). The executor still
    decodes the pushbuffer, runs vertex programs and fixed-function
    transform on the CPU and clips; triangles, the register combiners
    (compiled to GLSL per program), the four texture stages, fog, alpha
    test, blending, depth and stencil run on the GPU into one texture per
    render target. Render-to-texture is sampled in place; only the frame
    being shown is read back into guest memory at each flip. Also: vertex
    programs are decoded once per program and each distinct vertex of a
    batch is transformed once (big batches on the thread pool).
    `RECOMP_GPU=0` keeps the software rasteriser. Headless on 2 cores with
    a software GL driver: logo and legal screen 49-60 fps (were 4-6), in
    game 11-12 (were 2-3).

20. **Fixed-function texture matrices** (`SET_TEXTURE_MATRIX_ENABLE` /
    `SET_TEXTURE_MATRIX`) are now applied; `RECOMP_FFP_TEX_MATRIX=0` ignores
    them.

21. **Upside-down photos in the menu book; black characters; oversized
    film grain.** The book pages, the photos on them and several full-screen
    effects are drawn as *inline* vertex arrays in fixed-function mode, and
    the executor read those as screen coordinates. They are not: the photos'
    matrix flips y (`y' = 256 - y`), the film-grain overlay's halves its
    coordinates, and some in-game effects carry a full perspective matrix.
    → (toolkit) inline fixed-function batches go through the composite
    matrix like any other. `RECOMP_FFP_INLINE=0` restores the old reading.

22. **Every other frame doubled, with a stale strip on the right** (GPU
    renderer only). The GL state cache skipped `glActiveTexture` when the
    texture was already bound on the requested unit, so a following upload
    or parameter change landed on another unit's texture: once a render
    target's, which an upload re-specified at another size. Each frame
    copies the back buffer into a 512x256 blur texture and blends it back;
    on alternate frames that copy filled only 455 columns.
    → (toolkit) the cache tracks the active unit; deleted texture names are
    forgotten.

23. **Square blocks over the chapter intro on NVIDIA** (GPU renderer only).
    The game's colour-grading pass reads the surface it is writing, which
    GL leaves undefined. → (toolkit) such a stage samples a copy taken just
    before the draw.
24. **Crash while loading, after `[ICALL] Failed to resolve VA 0x001890E4`.**
    The CRT memcpy's trail-byte table at `0x001890CC` has four arms; the
    unaligned-short path jumps through it (`jmp [ecx*4+0x001890DC]`), and the
    0- and 1-byte arms (`0x001890DC`, `0x001890E4`) were not functions, so
    single trailing bytes were dropped (fix 11 added the 3-byte arm).
    → both arms in `names/extra_functions.json`.

25. **Any Xbox Live screen crashed** (Friends Menu, Y at the desk). The Live
    UI loads its font `Z:\BeryliubNewNormal.bmf` through XAPI
    `GetFileInformationByHandle`, which asks the kernel for
    `FileFsVolumeInformation` and `FileInternalInformation`; neither was
    implemented, so the call failed with DOS error 317, the UI object kept a
    null handle and the menu called through it. → (toolkit) both queries
    implemented. The Sign-In screen now opens and shows "New Account".

26. **"New Account" on the Live Sign-In screen quit the game.** It asks for
    the Xbox dashboard's account sign-up page (launch data type 1, reason 7),
    which on the console reboots into the dashboard. → (toolkit) dashboard
    side trips (any reason but the main menu or an error) return to the
    title with `STATUS_NOT_SUPPORTED`; the game stays on the Sign-In screen
    and B backs out ("Not Signed In"). `RECOMP_DASH_RETURN=0` restores the
    exit. Partition0's image is also sized to the 512 KB config area that
    XOnline reads sector by sector.

27. **Widescreen hung the game before the logo.** The toolkit answered the
    dashboard's video setting (XC_VIDEO) with the XGetVideoFlags bits
    unshifted (0x3); XAPI shifts the stored value right by 16 (sub_001911D7),
    so every title saw 4:3. With the bit in the right place, D3D then
    refused every 16:9 mode: it allows a widescreen present only when the
    AV-pack query carries 0x10000 (0x0033F486), and device creation failed
    silently, leaving the game spinning on an empty pushbuffer. → (toolkit)
    XC_VIDEO stores the flags in the high word, RECOMP_WIDESCREEN sets the
    widescreen bit, and the AV-pack answer carries the dashboard's video
    flags. In widescreen the game renders anamorphic 720 x 480 with a wider
    field of view; the presenter shows it at 16:9.

28. **Controller ignored while the keyboard worked** (reported on Windows).
    Xbox pad n read XInput slot n, so a pad Windows put in slot 2 or later
    (a virtual pad in slot 1: Steam Input, DS4Windows, a wheel) never
    reached the game. → (toolkit) pad n is the n-th connected XInput slot,
    rescanned when it disconnects or every few seconds; the log names the
    slot (`[INPUT] controller 1 is XInput slot N`).

29. **Crash about a minute into a Showdown match** (user report, then seen
    twice on the user's PC; also the co-op crash in XInputSetState). When
    a hit shook the pad, XAPI sent the rumble as a SET_REPORT on the
    control pipe (`21 09`, logged as `21 09 value 0214 len 6`); the
    toolkit's USB pad answered class requests to the device with a stall,
    and XAPI's USB code then read through a -1 pointer (guest 0x003C29E2,
    host access violation at guest VA 0xFFFFFFFF). → (toolkit) class
    requests from the host (SET_REPORT, SET_IDLE, ...) are accepted; the
    rumble bytes are forwarded as before. Verified on the user's PC: four
    minutes of a match, shot and blown up, no stall, no crash.
30. **Showdown loading screen turns to coloured noise** (user report). About
    ten seconds after a match's loading screen comes up (600 frames), the
    picture under "Press START" became the same pattern of noise and stripes
    every time, at any render scale. The game draws the loading picture once
    into a 720x480 target of its own and then copies it to the screen every
    frame. The GPU renderer frees a surface nobody has drawn to for 600
    frames, and it did so without reading it back first: so the next copy
    came from guest memory, which had never held the picture (level data).
    Not seen in the Linux test runs because their per-report dumps read every
    surface back. Confirmed on the user's PC with RECOMP_GPU_SYNC_FLAG (every
    surface read back each flip): clean for minutes. → (toolkit) idle
    surfaces are read back before they are freed; the read-back already
    drops the GPU copy when the title has reused the memory.
31. **Showdown line-up full at 48** (the photo is 1024 wide and the retail
    characters fill it). The photo is built once per poster load by
    sub_00043CD0 (base ui_characterlineupl/r24b, each unlocked character's
    ui_mphoto_<code> drawn at its hotspot x = [el+0x4C]+[el+0x58], clipped
    to 0..1023; textures CompPhotoL/R into photoleft/photoright of the
    poster's pages [poster+0x1C] and [poster+0x50]; it also appends each
    unlocked hotspot to the line-up's list, sub_0002C780 on
    [poster+0x1C]+0x470). The hand moves freely, clamped to the photo
    (sub_0002E250), and sub_0002D430 picks the nearest hotspot via
    sub_0002A090 (screen position: hotspot + its size + XlsToSwuiOffset,
    scaled by the photo); the pick is an index into the unlocked roster
    (sub_000245E0). → (rdr_hooks.c) hotspots at x >= 1024 make further
    pages: each page's photo built at poster load by running the builder
    with that page's hotspots shifted into place (list count reset between
    runs: run twice, the list held every hotspot twice and picks past the
    roster gave the token no target), the shown page's hotspots shifted and
    the others' moved out of reach for the pick, the page turned when the
    hand is held against an edge (stick from [state+0xC]), turned to a
    character's page when the hand is put on it (sub_0002ACD0 from
    sub_0002C7D0), extra page textures released with the poster
    (sub_00043BA0; released like 0x00043CA0: count +0x1C, sub_0017F850,
    free: sub_0017FCA0 is for images and failed in FindHeap).
32. **Story co-op: a second cutscene from another chapter** (user report:
    "the cutscene of the level my save is on"). After chapter 1's opening,
    Sheriff Bartlett's "I got your telegram" (lvl_tomb_pupshow_01) played.
    Player 2 was made with BR_playerAI to keep the level's opening from
    playing twice: but BR_playerAI is Brimstone's (the town's) player
    brain, whose start state plays the town's cutscenes by level variables
    (br_playerai_start.stateb: brim_playerAI_BrimOpening ->
    lvl_tomb_pupshow_01). Found by logging the puppetshow loader
    (sub_001B26C0, RDR_PUP_LOG=1). → player 2 gets
    playerbrain_multipartner_tester (one state, no conditions or actions).
    hero_playerAI (equally empty but "nofire") left player 2 unable to
    move; Multiplayer_Player_Brain runs Showdown's last-man-standing states.
33. **Story co-op: player 2 started away from player 1.** Players are put
    at their start by sub_00137370 (start points: per slot a count at
    +8+4k and up to 10 entries of 0x24 at +0x18+k*0x168; per character
    sub_001EB490 position, sub_001EB4C0 heading, sub_00219D60 physics,
    found with RECOMP_WATCH on player 1's position). Story levels fill only
    slot 1's list. → (rdr_hooks.c) in co-op the extra slots borrow slot 1's
    list, 2 units apart; and since a level's script can move player 1
    elsewhere during the opening (chapter 1 does), an extra player still
    more than 8 units away once the opening is over is moved beside player
    1 with the same three calls (rdr_overrides.c). Verified: beside player
    1, walks on the ground.
34. **Crash on Restart Level after a death in co-op** (user report; XAPI
    read through -1 under sub_003C38DC, called from the game's rumble
    sub_00047940 -> XInputSetState). The rumble URB of player 2's pad
    (feedback block 0x00652736) was in the host's control queue when it was
    queued again: the queue (host+0x41C, linked at URB+0x24) looped on
    itself. → (rdr_hooks.c) sub_003C3998 does not queue an URB that is
    already in the queue. Not reproduced in the test harness (no death
    there); needs checking on the user's PC. Superseded by 37.
35. **Story co-op: player 2 still not beside player 1; player 2 stuck dead**
    (user report). The placement in 33 ran once, at the end of the opening,
    and missed when player 1 was moved later. → (rdr_overrides.c) after
    each start of a level (and whenever player 1 jumps more than 6 units in
    a frame) player 2 is watched for 600 of player 1's updates outside
    cutscenes and moved beside player 1 whenever more than 8 units away
    ("[COOP] player 2 moved beside player 1"). Player 2 at health <= 0
    while player 1 lives is revived 4 s later beside player 1 with the
    Showdown respawn's calls (sub_0006D000: health reset through
    [p+0x18], vtable +4, [p+0xC] +8, +0x94 then +0x4C8 sub_0005AFC0(1.0),
    +0x28/+0x184): "[COOP] revived". Verified in the harness: moved
    beside after the opening; health poked to 0 ("coopkill 1" on the
    overlay test flag) restored to 250 and placed. A real death still to
    be seen on the user's PC.
36. **Story co-op: the controller moved both players** (user report,
    "occasionally"). Each character reads its pad object at +0x894
    (0x006795C8 + 0xB0 * port); after a spawn player 2's could point at
    port 1's ("[COOP] player 2 read pad 006795C8; now port 2's" in the
    harness). → (rdr_overrides.c) each co-op player's pad object is set to
    its own port's every update. Verified: pad 2's stick moves only player
    2, pad 1's only player 1.
37. **Crash right after entering a Showdown match with the preview
    characters** (user report; logs/chartest.log): XAPI's control queue
    again (write through -1 at guest 0x003C391D, under XInputSetState from
    the rumble, sub_00047940). → (rdr_hooks.c) XInputSetState
    (sub_003BE767) for the game's feedback blocks (0x006526F0 + 0x46 *
    port) no longer goes over the emulated USB at all: the motor speeds
    (+0x42/+0x44, scaled by the rumble setting) go straight to the host
    pad, the block's status is set to done and "pending" returned, which
    the game polls for. RDR_USB_RUMBLE=1 restores the USB path.

38. **Duels ignored the mouse** (user report, story and co-op; pads fine).
    The duel's camera reads the look channels (14/15) the mouse goes into
    but does not turn with them; its grab, draw and aim read the right
    stick. → (rdr_overrides.c, mouse_look) a camera that has not turned
    the mouse's way for 4 frames of mouse movement (channels 20/21:
    bearing in the mouse's direction, or any tilt) hands the mouse to the
    right stick (rdr_input.c) for half a second at a time; between, the
    counts go to both, until the camera is seen turning with the mouse
    again. Verified in the harness (Showdown High Noon): pull back grabs,
    push draws, the targeting cursor follows ("[LOOK] the camera does not
    turn with the mouse", RDR_LOOK_DEBUG=1).
39. **Level warp** (F1 > Cheats > Go to chapter). Writing a level into
    0x00652E70 and restarting does not reload (see "Go to level" below);
    the warp takes the game through its own menus instead (rdr_overrides.c,
    warp_step): in a level Start, the pause menu's Quit, the warning's
    YES (left, then A; stage 2 after it: presses during the fade to black
    crashed in sub_0017D010); in the front end (level name "lvl_front":
    the stale player list keeps rdr_in_level true there) A on the desk's
    book, story_newgame, then on the chapter page (bookchapterview,
    Update sub_000359C0 now wrapped) the contents cell of the shown
    chapter ([page+0x45C] indexed by ([[page+0x44C]+0x100] - [page+0x464])
    / 2, "lvl_perfnew_1") holds "<level>_<var>" while Begin reads it. New
    resets the profile's progress: the profile (0x00653508 + 0xB18 *
    [0x00657798]) is copied before and put back once the level runs; the
    cell too. Verified in the harness: chapter 1 -> chapter 4 (the train),
    front end -> chapter 4.
40. **Option pages by mouse** (user report): COMBO rows (vtable
    0x003C6BA0; value box +0x127C x, +0x1280 y, +0x1284 w, +0x1288 h) take
    the focus under the pointer, a click on the value's left or right half
    steps it; SLIDERs (0x003C6EF8: value +0x144, step +0x148, the n-th on
    the n-th "groove" texture) are stepped (d-pad, so the game keeps the
    value) towards where they are pressed or dragged; their arrows step.
    The pop-ups the manager never makes current (Showdown Options,
    difficulty, warnings, save) are pointed at while shown. The journal and
    the line-up's small option menus: the mouse steps the highlight (no
    places to point at found), the prompts click.
41. **Line-up hand shook under the mouse** (user report). The hand was put
    under the pointer, then the game's "let go" pick pulled it onto the
    nearest marker, and the next small mouse movement took it back.
    Movements under 3 pixels are ignored and the let-go pick is no longer
    made (the pick while moving already chooses the character).

42. **The game kept pausing by itself** (user video, chapter 4 after a
    warp): the warp only counted itself done after copying the profile in
    the book, which it had not done, so in the new level it went on opening
    the pause menu and choosing Quit. The profile is now copied when the
    warp is asked for, and the warp is done once the front end has been
    reached and the named level runs; nothing is pressed in a level after
    the front end; two minutes and it gives up.
43. **Menus dead after a level** (arrows not clickable on the desk): the
    players' list keeps the last level's player after quitting, so the
    front end counted as "in a level". The loaded level ([0x0065AA0C]+0x14)
    being lvl_front now means no player (rdr_cheats.c, player_va).
44. **No Start in Showdown from the keyboard**: Enter, Start's key, was
    made A in menus. In the front end it is now A and Start together
    ("Press START" on the poster and the match's loading screen); in the
    pause menu A only, which Start would close.

45. **Preview stages: shadow boxes and one more x87 arm.** Seven of the
    2002 preview's levels stopped at load with `imgShadowBox::Init():
    couldn't find ShadowMap texture` (`0x001BDA00`): their `.bxes` carry
    `box_SHADOWMAP_<name>` boxes, and the retail game wants a pre-drawn
    `lvl_<level>_shbox_<name>` texture for each, which the preview never
    had. `beta_level.py` drops those boxes. Two others (`lvl_tng`,
    `lvl_flash`) crashed with `[ICALL] Failed to resolve VA 0x0018C18E`:
    another arm of the CRT's x87 special-case tables (item 11), now in
    `names/extra_functions.json` (build.bat regenerates once for it).

46. **A duel's aim by mouse unplayable** (user video, story train duel):
    the cursor would not move in some directions and crawled in the
    others. Two causes. The mouse went to the camera's look channels or
    to the right stick by a guess from the camera's turning: the duel's
    camera drifts by itself, and a mouse moving its way was given to the
    channels, where nothing reads it. And the cursor is a point on the
    screen moved by a velocity the stick pushes (`sub_00146530`: player
    `+0x67C` across, `+0x680` up, velocity `+0x87C`/`+0x880`, the level's
    `.duel` file's CursorSpeed 0.6 times slow motion's time, friction
    CursorFriction), so a mouse through the stick was capped at a full
    throw (about a third of the screen in two seconds) and drifted on.
    → A duel is now known for sure: the player's duel object
    (`[char + 0x190]`, its state: 1 grab, 3 draw, 4 aim; what scripts read
    as `duel_state`). In a duel the mouse is never the look channels'; in
    the grab and the draw it is the right stick, every count paid out
    (no saturation); in the aim (state 4) it moves the cursor itself,
    before the character's update (`player_update`), and stops the
    velocity: 1600 counts across half the screen at 100 %, "Duel aim
    sensitivity" (F1) on top of the mouse sensitivity. Verified in the
    harness (Showdown High Noon): `RDR_TEST_DUEL_MOUSE=-1600,0` puts the
    cursor half a screen left in one frame, up is up.
    Then "floaty in the slow motion, perfect after": the slow motion
    (world time x 0.25, the cursor's own time unscaled) starts in the
    draw, state 3, where the cursor already shows and moves, and there
    the mouse was still the stick. → The cursor is moved directly in
    states 3 and 4 (`RDR_DUEL_AIM3=0`: the stick in the draw, as before).
    `RDR_DUEL_TRACE=1` logs each update's state, cursor, velocity, the
    cursor's time and the world's.
47. **The desk's items clickable** (user request): the book, the Showdown
    poster and the journal are entered with a click when in front, and
    turned to with a click when not (the far one in two steps); before,
    a click anywhere was A on the one in front. Places per item in front,
    in the 640x448 menu space (rdr_overrides.c, hud_scan); the prompts
    come first. Verified in the harness: book -> journal -> poster ->
    Showdown by clicks.
    Back to the book did not work: the desk is not a ring. From the book
    left is the poster and right the journal; between those two left and
    right; and the book is reached from either with d-pad *down* (left
    from the journal is the poster: what a click on the book sent).
    → A click on the book (journal in front), or on the outer arrow of
    the poster or the journal (which lead nowhere on the pad), is d-pad
    down; the poster <-> journal clicks are one step. The book's place
    with the journal in front covers the "A Enter / X Options" prompts
    drawn over it, and there the book wins (the arrows still come first).
    `RDR_MENU_DEBUG=1` logs which item is in front. Verified in the
    harness: journal -> click the book -> book; book -> poster -> click
    its left arrow -> book.
48. **The Rockstar logo held still for five seconds** (user video): after
    the logo movie the game fades to black on a thread of its own while
    it loads (`sub_001067C0(5.0 s, ...)` from `sub_000180E0`, the fade
    loop `sub_00105B40`, `[0x00663338]` set while it runs). Its fades are
    incremental: each frame subtracts from the picture (blend op reverse
    subtract) the growth of the fade since that buffer was last drawn,
    rounded to a whole step of 255, and records the unrounded target as
    done. At 500 frames a second every step rounds to nothing: the
    picture held its first dimmed frame for five seconds, then the titles
    cut in (measured on the user's PC: 450 presents a second, all the
    same image). → While the flag is set the presenting thread paces
    flips to 60 a second (rdr_timing_tick), as on the console; the
    loading behind it goes on. The level transitions use the same loop.

49. **Preview line-up pages nearly empty, preview characters in scraps**
    (user video and screenshot, play_preview.bat). The line-up hides a
    locked character; the lock is a bit per unlockables.csv row
    (`sub_00057C90` tests `[mgr]+0x24` saved and `mgr+0x18` session bits,
    256 each; `sub_00057D30` sets one). The preview adds 55 rows to the
    retail 221: a profile made before has rows 221-255 clear, and rows
    256+ have no bit at all (the test read the next fields; a set would
    have written over them). → Both replaced in rdr_hooks.c: rows past the
    retail 221 are always unlocked, nothing past bit 255 is written; the
    Unlock Everything cheat stops at 256. Verified in the harness with a
    copy of the user's profile: line-up page 5 full (it showed 6).
    The scraps: the roster tool took a retail texture whenever one had the
    preview's name, and many hold another picture (Navarro's `zzt02`:
    other parts, other places). → The preview's own pictures always,
    renamed in the models where the retail game has the name
    (beta_roster.py). Verified: Navarro (`qzx`) played in the story with
    `RDR_PLAY_AS=mqzx`, dressed right; 20 cut-outs redrawn.

50. **Desk: a click while the mouse moved turned the view twice** (user
    video). The prompts go while the view turns from one item to the next,
    with them the "pointer page", and the mouse then stepped the d-pad
    again. → On the desk (seen in the last 3 s) the mouse never steps
    (rdr_input_desk). **Preview characters**: hats floated (2.12 hats
    moved by a bone as if bodies), faces missing (separate face models
    left out), Christina's arms folded (her hair bones took the arms'
    channels in the old skeleton format): beta_roster.py/mod212.py, see
    MODDING.md; checked by playing each in chapter 1 (`RDR_PLAY_AS`).
    A first try wrote such skeletons as version 101 (channels by name):
    Christina then ran with her legs flung about and could not aim, Young
    and Teen Red floated (user). Rebuilt instead onto the retail skeleton
    her animations are made for (ctn, 58 bones), or the retail 52 in
    order (Teen Red): both run with their feet on the ground in the
    harness. O'Grady see-through: his texture's alpha (189-255): made
    opaque.
    Gun sounds lost mid-match (user, not reproduced): the sound voices in
    use are logged every 20 s when their count changes (`[SND] voices
    held`), to see a leak next time.

51. **Window dragged under the taskbar** (user): grabbing the title bar
    sent the window down past the bottom of the screen for good. The
    pointer is confined to the client area while playing; in Windows' move
    loop that pulled it down out of the title bar, the loop moved the
    window after it, the client area moved down, and again. → No
    confining between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE or after a
    press on the frame (rdr_input.c); and a window whose title bar ends up
    off every monitor's work area is put back (toolkit fb_present.c, after
    a move, a display change and at start). **Keys**: the game's prompts
    name the pad's buttons by their PS2 glyphs (BUTTON_SQUARE = X: "ENTER
    Saloon? Press X", cover, revive; BUTTON_TRIANGLE = Y: "Press Y to
    reload"; BUTTON_CIRCLE = B: melee, back). Defaults now X = E, Y = R,
    B = F, and Dead Eye (the right stick click) on Mouse 5 and Caps Lock;
    settings.ini files with the old defaults move over (keys_version 2).
    **Flash menus**: the shop before a mission and the weapon select after
    it, the stores in town and every page of the sheriff's journal are
    Flash movies, not swui pages: a menu object (sub_000188C0: name at
    +0, player at +0x24), its frame sub_00018490 (hooked: rdr_swf_menu),
    its variables read and set by sub_00018600 / sub_00018620. The shop's
    and weapon select's places are fixed; the weapon select's row is the
    movie's currentSelection; the menu before them (Start / Weapon
    Selection / Shop, "startChpMenu") has its hand at curMenSel. These
    movies read the pad at their own pace: a 60 ms step or a quick click
    went unseen, so a click there is one press held 180 ms (under their
    250 ms repeat), let go as soon as another movie runs (the shop's
    "Done" also closed the weapon select behind it). The journal's hand is
    curMenSel too, its open entries the bits of mainUnlock (contents) or
    unlock (sections); on a section page an entry sits at selectionStartV
    + k * menuLineOffset (the right column columnOffsetV higher) in the
    movie, about 219 + 0.519 v in a 640x360 frame. A sideways mouse
    movement had stepped the d-pad there (the swui "journal" page asked
    for mouse steps) and turned its pages. RDR_UNLOCK_ALL=1 (tests) opens
    every unlock row, the journal's included.

52. **Crash unloading Brimstone after shopping** (user logs, three times:
    the General Store, Tailor, Gun Shop and Saloon visited, then the
    chapter ended): `Free(%p) - Prev node no longer points back to us`
    (sub_0005D8F0, from the level teardown sub_0005E910 -> sub_00086370
    -> sub_00086290, which empties the global name maps: each node, +0 its
    key string, +8 the next, is freed with its key). The heap's blocks
    follow 16-byte headers (+0 the block before, bit 0 in use; +4 the
    size; the next starts 16 + the size rounded to 16 on). The third log
    (with the block dumped) showed the node's header cleared: "not in
    use", the block before named as 0: the node was freed already, its
    memory by then someone else's; next to it a "SND_4098" record. → The
    free (wrapped, rdr_hooks.c) makes the game's checks first, as the
    game does (small-block pools, unchecked heaps and guard words passed
    through), and a block that fails them is left out and logged
    (`[HEAP] free of ... left out`) instead of ending the game. Harness:
    2000 frees checked, none left out; Brimstone unloads. Who frees it
    first is still to find (the log line names the caller and the last
    Flash menu). Fifth log: that free left out, the next key in the same
    name map pointed at no heap at all ("FindHeap failed on ptr"): the
    map's nodes are overwritten, not only freed twice. FindHeap's fatal
    error is passed over too (it returns 0; the free leaves the pointer
    alone), and every free of the node's address (0x81DE5C1C, the same in
    all the user's runs) is logged with its callers, to catch the first.
    Sixth log: the chapter ended and the next (lvl_bomb) loaded. The watch
    saw no earlier free of the node: it was never freed, but overwritten
    in place (its header 0/2, the next nodes' keys 2 and 4) while the
    stores were visited; next to it lies a "SND_4098" sound record. The
    writer is still to find; the teardown now passes over the damage.

53. **Co-op characters, 3-4 players** (user request). Each extra player's
    spawn (rdr_coop_level_start) now writes that player's own type over
    the descriptor's, through the Play-as hook (rdr_play_as_spawn): the
    story's player types srd, jck, ani, ttk (Shadow Wolf), bfs (Buffalo
    Soldier) head the Play-as list, then the Showdown ones. Harness: four
    players in Brimstone as Red, Jack, Annie and Shadow Wolf, quarters,
    each on its own pad. A third or fourth player without a controller on
    the Controls page now takes the next free one (rdr_effective_pad_map);
    before, they had none.
54. **Preview characters: O'Grady and Old flew apart aiming a rifle,
    Young and Teen Red floated** (user). Their skeletons were the
    preview's format (O'Grady's and Old's left as they were, being in
    order; Young and Teen Red's rebuilt as the retail 52), which binds an
    animation's channels by a bone's place; the retail format names each
    bone's channels. → beta_roster.py puts every preview skeleton onto a
    retail one: the same name's when there is one (yrd: the retail Young
    Red's is the preview's exactly), else the copied character's, else
    srd's. Harness: Young Red (qyr) stands and runs as the retail Young
    Red (yrd) does; Old runs normally. Old's models are an older format
    that cannot be moved onto other bones: a retail skeleton still serves
    when every bone keeps its place (his coat tail where Red's tail_root
    hangs, from the pelvis), so he gets Red's.
55. **Old, Young and Teen Red still floated** (user, after 54). Not the
    skeleton: the retail Young Red's .cr pointed at the preview's model,
    and the preview's .cr at the retail model, both floated the same in
    the harness. The animations carry the root at one height for every
    character; each one's .swt lowers the body by its own skeleton's
    root height from it (`OffsetY`, ~ root - 1.12 for every retail
    character: Young Red -0.265, Red -0.037). The preview characters were
    copies of Red's .swt. → beta_roster.py sets OffsetY (DuckOffsetY by
    about half, the camera and AI heights in proportion, the shadow model)
    from the retail character of the same skeleton, or the one copied,
    moved by the root difference. Harness: Young Red, Teen Red and Old on
    their shadows. It moved others too: the preview "Big Oaf" (root 1.35
    against the retail frk's 0.95) had sunk 0.4 m, Mr. Kelley 0.26 m, Jin
    floated 0.37 m.
56. **Keyboard and mouse dead in the menus after unplugging the
    controllers** (user). The menus read one port, the controller that
    pressed START ([[0x0067991C]+4]); with it unplugged nothing reached
    them. → When that port has no controller, the menus are given the
    keyboard's port (rdr_menu_port_take, from pad_filter); logged
    "[INPUT] menus: port N has no controller". The keyboard's port is
    logged as it changes ("[INPUT] keyboard and mouse: port N"). The menu
    map lives in the heap (0x8...), which the old test (below 64 MB) never
    accepted.
57. **Co-op: a revived player landed outside the level and fell, the
    camera with it** (user). Revived players were put 1.5 m to one side
    of player 1, whatever was there. → Player 1's trail (where it stood
    while alive, a point every 0.75 m) gives the place: the newest point
    1.2 m x k behind it, on its level. A player far below player 1 (12 m)
    for a second is put back the same way ("fell; put back").
    **Friendly fire** (user request): Cheats > Story co-op > Friendly fire,
    off by default: a bullet's hit (sub_00088050: a1+8 the one hit, a2+0x1C
    who fired, the pair Showdown's own team check compares) from one
    co-op player on another does nothing.
58. **3-4 player split screen at 30 fps** (user). The executor thread is
    the limit (drawing 8.1 s of every 10, transform 3.9 s; each view draws
    the whole scene). → A repeated index is drawn from its first place
    instead of copying its 128-byte output (a gigabyte a second at three
    views); a triangle wholly inside is tested without copying its three
    vertices; a transform of a few hundred vertices wakes a few pool
    threads, not all. GPUPROF now says how many vertices were transformed
    on the executor thread alone (batches too small for the pool).
59. **Vertex programs on the GPU** (user: 3-4 player co-op at 30 fps on a
    Ryzen 9 / RTX 5080; Steam Deck wanted). GPUPROF on the user's PC showed
    every transformed vertex on the executor thread (no batch reached the
    256 the pool needs) and the frame read-back taking a sixth of the time.
    → The toolkit now runs a batch's vertex program on the host GPU:
    nv2a_vsh_glsl writes the decoded program as GLSL (operation for
    operation what the CPU interpreter does: sources, masks, the R1 rule
    for a paired ILU, rcc's clamps, NaN compares), nvgl_vsh_prepare
    compiles it once per program and input layout, the executor copies the
    index range of each vertex array as the title laid it out (interleaved:
    one copy) and the batch's 192 constants as one block of a texture
    buffer; batches with the same program, layout and GPU state are still
    drawn as one (each vertex names its constant block). The near plane is
    clipped with gl_ClipDistance (w and z, as the CPU clip did). A batch the
    GPU cannot take (a program writing c[], a sparse index range, an
    unknown format) is transformed on the CPU as before; RECOMP_VSH_GPU=0
    keeps all on the CPU. The finished frame is no longer read back into
    guest memory at each flip when the GPU draws the window
    (RECOMP_FLIP_READBACK=1 restores it). Harness: the executor's own time
    outside GL fell ~9x in a 3-player split; frames match the CPU path;
    the user's PC front end went from ~120 to ~215 fps.
60. **Chapter select from a level stuck on the pause menu, or back to the
    main menu** (user). The warp focused Quit and pressed A in the same
    frame: the press went to Return to Game, the menu closed, the warp
    pressed Start again. And after Begin it kept working the front end's
    pages while the chapter's own menus (Start / Weapon Selection / Shop)
    were up, and waited for a level of the chapter's name (lvl_traincar_1
    plays lvl_train). → Quit is focused, then pressed half a second later;
    after Begin the warp only waits for a level to run (any but the front
    end) and puts the profile back then, with no two-minute limit.
61. **Co-op: enemies standing about, not shooting** (user video: three
    players, one controller). Players 2 and 3 had nobody on their ports and
    stayed at the start; the enemies around player 1 seemed to have them in
    mind. → A co-op player whose port has no controller (and is not the
    keyboard's) is brought along behind player 1 when 15 m away
    (rdr_port_live from pad_filter). How the story AI picks among several
    players is not changed.
62. **Menu music stopped for good** (user: after picking many characters in
    the Showdown line-up, or idling in the menus). In the log the music's
    stream reads stopped mid-track (offset 114.8 MB of a track the next
    attract loop played to 118 MB) while the game ran on: its stream voice
    no longer consumed buffers. Not reproduced in the harness. → The
    toolkit's voice processor checks every 30 frames for a stream voice that
    is on but in none of the voice lists (never processed again) and puts it
    back on its list after 1.5 s ("[APU] voice N on but in no list"); a
    stream voice on but not moving for 3 s is logged with its state.

63. **Chapter select straight into the chapter** (user: "it takes you to
    the main menu and starts it from the book; is there any way to bypass
    this?"). How a chapter starts: the front-end screen (the screen
    manager's screens: vtable, list of transitions at +8, each a condition
    at vtable+0xC and a target at +8) is entered by the pause menu's Quit
    (sub_0006BAA0: unload the level, "Loading...", lvl_front, the desk).
    Begin (sub_00035910) names the level (sub_00056550 -> 0x00652E70,
    variant 0x00652E60), resets and commits the story session at
    0x00652138 (sub_0003FB80/sub_0003FBF0) and sets its flags; the
    front-end screen's transition to the game screen then fires
    (condition sub_0006BA40, which tells the game screen the level,
    sub_00064980 on [0x0065A6D4]), and the game screen's enter
    (sub_0006B190) runs the chapter: its loading, weapon selection, shop,
    start menu and the level itself. A screen's enter may return a
    transition, which the manager (sub_00064760) follows at once. → From a
    story level, the warp's Quit enters the front-end screen hooked: it
    clears the pause menu's quit request (bit 0 of 0x0065A828,
    sub_0005DF40(0,0): left set, the started chapter went straight to
    the main menu through the pause screen's transition, condition
    sub_0005DF20), does what Begin and the condition do with the warp's
    level instead of the book's, and returns the transition to the game
    screen. The front end, its loading screen and the book never load;
    the profile is not reset (no New), so nothing is put back. Harness:
    lvl_tomb -> lvl_traincar (shop, start menu, plays on), then
    lvl_traincar -> lvl_fort the same way. RDR_WARP_VIA_BOOK=1: the old
    way. The book way (from the front end) now puts the profile back as
    soon as the chapter is named, before its shop: put back after it, the
    shop's purchases were undone.
64. **The game's thread waiting on the drawing thread at every kickoff**
    (user log, 4 players: ~70 fps with the drawing thread busy 6.7 s in
    10). D3D's push-buffer kickoff (sub_0033C7F0) and its plain flush
    (sub_0033C790) set PFB_WBC bit 16 (0x00100410, the write-back cache
    flush) and spin until it clears; the toolkit clears it only from the
    push-buffer thread, between commands or between passes, so every
    kickoff waited for the drawing thread to come round: the two threads
    took turns. Profile (perf, harness, 4-player co-op): a third of all
    samples in that loop. → With the flip not held for vblanks
    (RECOMP_FLIP_PACING off, the default) the kickoff writes DMA_PUT
    without the flush wait, and the plain flush returns at once; the ring's
    real back-pressure (DMA_GET as the walker consumes) is unchanged.
    RDR_KICKOFF_WAIT=1: as on the console. Present's own wait for the
    vblank acknowledgement (sub_0033F700) is left: the emulated vblank
    runs at ~311 Hz, at most ~3 ms a frame.

65. **Chapter select still slow in the pause menu** (user: "it is doing it
    slowly still in the pause menu"). The warp drove Quit and the warning's
    Yes by button presses with two-second gaps, and the pause page then
    fades for two seconds before it asks to quit (sub_0002FB80: +0x470
    counts frame time to 2.0, then the quit request). → Once Start has the
    pause page up, the warp does what the warning's Yes does
    (sub_00032C40: quitting flag +0x46D, +0x3B0 cleared, the level's
    tallies reset in a story) with the fade already run out: the page's
    next update asks to quit and the chapter loads (item 63). The pause
    menu shows for a frame or two. RDR_WARP_VIA_MENUS=1: by the buttons.
66. **Push-buffer thread's own cost** (user log after item 64: 75-78 fps
    with 4 players, the thread busy 6.5 s in 10 of which 3.6 s neither
    transform nor GL). perf: a sixth in whole-surface hashes each time a
    render target became current again (split screen switches targets
    many times a frame), a sixth in the method survey's per-word count,
    a fifth in the per-word method calls. → Toolkit a5d896b: render
    targets re-checked by a sampled hash (64 bytes per KB, whole every
    16th frame); the survey only under RECOMP_PB_SCAN; runs of indices,
    inline vertices and vertex-program constants handed to the executor
    in one call (nv2a_pb_exec_bulk). Harness: that thread's own time a
    frame from ~8 ms to ~2.3 ms.
67. **Pause when the window loses focus** (user). WM_ACTIVATEAPP (inactive)
    → the game's own pause menu at the next frame, in play only (not a
    menu, a cutscene or a warp). Settings: pause_on_focus (on).
68. **Unbinding a key** (user). Delete while a binding waits for its key
    leaves the control with none.

69. **Controllers by name** (user: "show the specific controller"). The
    toolkit's xbox_InputPortName names what is on a host port: SDL's model
    name ("PS5 Controller", "Xbox One Controller", "Nintendo Switch Pro
    Controller"), "XInput device" for a pad seen only through XInput (which
    does not say its model). The controller choices read "Controller 2: PS5
    Controller" / "not connected". Controllers start before the launcher,
    so its list has the names too.

70. **What each Showdown power-up does** (user: "a sub-text explaining what
    each power-up cheat does"). Every power-up on the Cheats page now has
    its effect on the hint line. Sources: the manual, powerpac316's
    Multiplayer FAQ (GameFAQs, 2004: the effect families, flaming,
    poisonous, freezing, piercing and explosive; sticky dynamite, the cannon
    strike, fake cards) and the game's effect data (swat.dat cr_*.arfx:
    durations; the projectile effects pj_hotfoot, pj_forgedcard, ...).
    Retribution and the Spirit Knife are not described anywhere found:
    their hints say only what the data shows, until checked in a match.

71. **Ready-to-play folder** (user). build.bat ends with tools/package.ps1:
    "Red Dead Recompilation" beside the project, with Red Dead Revolver.exe,
    its DLLs, Install Game from ISO.bat, Settings.bat, README.txt, and
    Game\ (a junction to the extracted disc when there is one), Saves\,
    Mods\ (textures\), Logs\, settings.ini: the project's settings,
    saves and mods copied in once, never moved or overwritten. The exe
    sees the layout (main.c, player_folder): game, saves, mods, texture
    packs and the log (Logs\last_run.log, the one before kept) beside
    itself, no .bat needed; no game yet: a message says how to install it.
72. **Installing from the disc image** (user). iso_install.c unpacks an
    Xbox disc image's XDVDFS partition (volume descriptor at its sector 32,
    directories as binary trees of entries) found at 0 (xiso) or after a
    full dump's video partition (XGD1/2/3 offsets) into Game\, with a
    progress console, then the game starts: an .iso dropped onto the exe or
    onto Install Game from ISO.bat (--install-iso). Names from the disc are
    used as single path components only. Tested on images built by an
    independent writer (balanced and list trees, plain and offset
    partitions): every file identical.

73. **Enemy health option** (user: optional enemy scaling). A character's
    health object [char+0x18] holds the health at +0x10 and its maximum at
    +0x14 (Red: 300/300). Cheats > World > Enemy health (100-500 %,
    settings enemy_health): a story enemy's health and maximum are
    multiplied the first time it is shot (the bullet-hit hook,
    sub_00088050, before the damage), once per health object per level;
    players and Showdown untouched. An enemy whose first damage is an
    explosion or fire is toughened only from its first bullet on. Not yet
    checked in a fight in the harness. More enemies (spawning) is a
    separate, larger piece of work. Replaced by item 83 (damage cut, health
    left alone).
74. **Menus in the game's own art** (user chose designs A and C1 from
    mockups; docs/UI_DESIGN.md). rdr_gameart.c reads swat.dat/front.dat
    (Dave archives, a small raw-DEFLATE inflater, DXT1/3/5, 4/8-bit
    palettes, .fonttex fonts) from the player's disc; rdr_ui_game.cpp
    draws with it. F1 opens a list drawn as the pause menu (the picture
    dimmed 0.584, the two pausescrolls rules, berrylium centred); each
    entry opens a page drawn as the game's options pages (options_bg_10k,
    the red regulatorcondensed title, rows with solid triangles, sliders
    with the mp_arrow chevrons and the mp_xspot X, the hint, the A/B
    prompts). The pages: Go to Chapter (a row a chapter, A twice), Cheats,
    Story Co-op (Play As with it), Display, Graphics, Controls. B / Esc /
    right click go back a step. The launcher is the same black page with
    RED <> DEAD in the logo's typeface (logo.fonttex) over a red band with
    "Recompilation" cut out of it, and the chapter book's portrait of Red
    (book_red) as a photograph. Fonts are enlarged 4x at load (cubic,
    smoothstep edge, box-filtered from 8x) and mipmapped. Without the
    game's files (or RDR_UI_PARCHMENT=1) the menus keep the parchment look.
75. **PC Options in the pause menu; Play As and Story Co-op in the book**
    (user). The .swui pages are read a line at a time (sub_00084F00,
    hooked): pause.swui's "Xbox Live" item becomes "PC Options" (each
    level archive has its own copy, so the line is changed as it is read,
    not in a mods archive), and the book's left page (storyselect.swui)
    gets Play As and Story Co-op under Player Options (Bounty Hunter moves
    below). The pause page's button handler (sub_0002FAB0) answers xblive
    by opening the F1 menu; the book's (sub_00037E40) answers PCPLAYAS /
    PCCOOP with that page of it, closing back to the book. A held from the
    game's menu is not a press for ours until let go. RDR_NATIVE_MENUS=0
    leaves the game's menus alone. A page of the game's own (tried: a new
    pcoptions.swui behind the item) is drawn by the game but takes no
    input: unknown names get the generic page class (sub_00028E10's
    default, 0x52C bytes, sub_00038000).
76. **Sharp game text** (user: "upscale the fonts of the game"). The
    game's lettering is bitmap fonts, 128 x 128 pages with letters ~20
    texels tall. A new toolkit hook (nvgl_set_texture_hook) lets the title
    replace a texture as it goes to the GPU; rdr_sharptext.c knows the
    game's font pages by their coverage (read from the disc, matched texel
    for texel) and returns them 4x bigger, redrawn as in item 74. Bigger
    replacements (this and texture packs) are now always mipmapped.
    Graphics > Sharp game text (sharp_text, on by default);
    RDR_SHARP_TEXT=0 for a run. The menu font berrylium is Ray Larabie's
    Berylium (free font): drawing it from its outlines is a possible next
    step.
77. **The money sparkle and the tutorial's controller in split screen**
    (user: "lens flares that occur when you gain money ... corrected for
    each respective framing"). Money paid (the HUD's AddMoney,
    sub_001D9DA0) starts a sparkle (effect 0x41) at [0x0067A16C],
    [0x0067A170]: the money box in screen pixels, left there by the HUD's
    layout (sub_001D9420) for the last view drawn. Showdown never makes the
    sparkle, so only co-op split screen showed it wrong: at the last
    player's money box whoever was paid. Each HUD's spot is now kept as its
    view is drawn (override of the HUD draw, vtable +0x18 = sub_001D98D0)
    and put in place while its player's money changes (the player's
    AddMoney, vtable +0x16C = sub_0013E2F0, and its update, which pays the
    combo). The tutorial's controller picture (controllerXbox_* models,
    vtable 0x003EB3D0, draw sub_001DF140) has a camera of its own with a
    full-screen viewport and was drawn in every view's pass at its
    full-screen place; in split screen it is drawn in player 1's pass
    only, its viewport the screen's shape scaled into player 1's view,
    right-aligned. Test command (RDR_OVERLAY_FLAG): "pay <player> <n>".

78. **Menus that would not let go** (user video: the book's Play As page
    could only be left by alt-tabbing; a click on Play As started the
    chapter after). Two halves of one cause: while our menu is open its
    window hook keeps every key and button from the window's own handler,
    which keeps the game's key table (the toolkit's). (1) A button that went
    down for the game and came up in the menu stayed down in that table:
    the click on the book's Play As was a held A once the page closed: the
    book acted again. Releases now still reach the table while the menu is
    open (presses stay the menu's). (2) The menu's own (ImGui) state missed
    the release of the key or click that closed it (that release went to
    the game), so on the next opening Esc and clicks were "already down" and
    did nothing until the window lost the focus. The menu now starts with
    nothing down each time it opens (and the pointer where it is). F1 also
    reopened the last book page: F1 and both sticks now always open the
    Quick menu (item 79).
79. **Quick menu** (user: "a smaller Quick menu ... pulled up quickly during
    gameplay"). F1 is a narrow panel at the left in the game's art (black,
    the pause menu's scroll rules, the red title, rows at 0.76 of an
    options page's size): Invincibility and Infinite Dead Eye (the game's
    cheat bits 6 and 10), Infinite Ammo, Freeze NPCs, Free Camera, Money
    (+/- $1,000), Enemy health (100-500 %), Story co-op (players, screen,
    Apply now), Frame counter, All PC Options (the pause menu's list; B
    comes back here), Return to Game. The full pages are reached from that
    list (or the pause menu's PC Options) only.
80. **What each power-up does, under its name** (user: "still no new
    explanations"). Item 70 put it on the hint line, shown only for the row
    under the cursor; ToggleSub draws it under each power-up's name, one or
    two lines (rdr_ui_theme.cpp).
81. **Co-op changes on Restart Level** (user: "restart a level to adjust
    the amount of players"). Restart Level restarts in place
    (sub_0005EA50 -> sub_00137650 respawns the players there are; the
    set-up that makes them, sub_00138090 + rdr_coop_level_start, does not
    run). When its flag [0x00652014] goes up and Story co-op (count,
    characters, Play as) differs from what the level was made with
    (s_coop_made_sig, recorded at each level start), the flag is taken down
    and the chapter is started afresh through the level warp (item 39),
    which makes the players as set. Tried first: renaming the loaded level
    ([0x0065AA0C]+0x14) so sub_0005EA50 takes its reload path: the name is
    put back before it is read, and its reload (0x0006C5xx) keeps the
    players it has. Apply now (Quick menu, Story Co-op page) does the same at
    once. The split-screen choice was already live (rdr_coop_views reads it
    each frame). Joining a running level without a restart would need the
    set-up's per-player HUD (sub_001DA450, sub_0013EA80) and view
    (sub_00051770) made by hand: not done.
82. **Free camera hides the HUD** (user: "the hud elements are drawn in
    front of the camera"). The HUD's counters are models just in front of
    the game's camera; seen from the free camera they hang where it was.
    While the free camera is on the game's No HUD cheat (bit 14) is set,
    and put back as it was after (rdr_freecam.c).

83. **Enemy health, again** (user: boss health bars did not move; Ugly
    Chris no longer stumbled when shot in the feet with the Sheriff on his
    shoulder, at 500 %). Item 73 multiplied health and maximum at the first
    bullet. Now nothing of the health is touched: a story enemy's damage is
    divided by the setting at its TakeDamage: vtable +0x9C of the
    character classes, reached through the game's indirect calls:
    sub_00076560 (0x003CE070: the townsfolk and gunmen), sub_001394B0
    (0x003D9C30) and sub_00091500 (0x003CF7B0; the other two call it
    directly, so each damage is cut once). sub_00091500 applies armour and
    the "tough armour" break (sub_00090110: damage summed against the
    type's +0x1F8, hits against +0x1FC, what scripts read as isstunned =
    [char+0xC8]), then the health object's sub_00272090 takes the damage
    off and tells its listeners. Health bars, healthpercent in the scripts
    (Chris's GotShot: healthpercent < 0.4) and every reaction the bullet hit
    (sub_00088050) decides before the damage see the game's own values.
    Checked with a test command (npchurt: damage through the vtable as the
    game calls it): 30 at 500 % took 9 of a townsman's 100 (6 after the
    cut, x1.5 inside). Explosions and fire are cut too now. Ugly Chris's
    stumble and the boss bars still want checking in the user's game.
84. **Quick menu: Money's left arrow added** (user video). A click anywhere
    on a choice row stepped it on; a click on its left arrow (or the left
    half of its value) now steps back (rdr_ui_theme.cpp, clicked_left;
    harness: $2,227 -> $1,227 -> $3,227). The Quick menu's list is now a
    whole number of rows high, so no half row shows over the hint.
85. **Free camera: black wedges and ghosts** (user video). The story's
    letterbox bars (two quads in front of the game's camera, sub_001D5F80)
    cut black wedges across the picture seen from elsewhere: not drawn
    while the free camera is on. The old-film blur kept the last frames
    over the picture as the camera flew: No Distortion (cheat bit 15) is
    set too while it is on, and put back after.
86. **Preview line-up pictures: backwards, and grey** (user). charpic.py
    guessed each model's facing from its feet; six of the 55 preview
    models came out from behind ("Big Oaf", "Bloody" Tom, Ydg, Dcg, two flat
    targets). Every preview model faces -z (all drawn from both sides), so
    that is used. The retail cut-outs are dark cool greys the line-up tints
    sepia (48 measured: luminance 4/19/32/53/115 at the 5/25/50/75/95th
    percentiles; 0.89/1.03/1.12 per channel); the preview's were drawn
    bright and warm and came out washed-out grey on the poster. Their greys
    are now mapped onto the retail ones. Takes a new mods_preview (delete
    it, run make_preview.bat).
87. **Bat Boy flies** (user: the TGS 2002 trailer shows him high up, more
    than halfway up the rock at Ancient Monolith). His FlightData was in
    place but never read: the engine looks at a character's flight state
    only when its .swt has bit 0 of SpecialMovesets (read to the creature
    type's +0x1DC; tested at 0x001436A0 and through the AI's flight code):
the condor and npa have it, nobody else. beta_roster.py now sets
    it for fliers (and no fall damage, as the condor), and also puts the
    FlightData into the story copy, which Play As uses (it is indented
    with spaces and the block went only where tabs were). In the harness,
    playing as him in chapter 1: the melee button with no gun drawn (the
    game's own way, his MeleeSpecialMove spmove_glideattack) takes off;
    Black with a gun drawn (the game's own hint text, which nothing in the
    retail game wires for a person) does the same through flier_black
    (rdr_overrides.c: the game's glide steps; in the air Black dives at the
    target). Not seen yet: a Showdown match with him played by the
    computer (the batboy brain).
88. **Bat Boy, after 87** (user video: he flies, but the dive does
    nothing, the camera loses him, no launch animation). The climb is his
    launch clip's rise between AscensionSampleFrame[0] and [1] times
    AscensionScale, for one play of the clip (F+0x1F4, +0x1EC, sub_0025B2xx);
    the preview's 2.5 took him 35 m up in 1.4 s, out of the camera's sight
    for the whole launch. 0.8 (beta_roster.py): about 11 m, and the camera
    shows the launch (crouch, spring, wings out). The dive
    (sub_0025AC90: only while gliding, state 3; it keeps the target and
    reads its position as it goes) found no target: nothing locked in the
    air. Now Black or the melee button in the air dives at the character
    the game would aim at, else the nearest living one ahead (the players,
    Showdown's opponents among them, and the people the level updated
    last) within 60 m; pressed while still climbing, it dives once he
    glides. Harness, the ranch: up 11 m, dive, recovery, glide down. The
    camera still trails him on the way up.
89. **The preview on one line-up page** (user: four pages, and the hand's
    choice did not line up with the figures). The line-up copies each
    cut-out 1:1 onto its 1024 x 256 photo (sub_00043CD0), so a figure's
    place in its own picture is its place on the photo: the retail page
    stands its 48 in three rows that way (back row feet near y 190, front
    253; the hotspot (where the hand points, nearest wins) is each
    face). The preview's were drawn alike, feet at the bottom, 14 to a
    page, each hotspot copied from the retail character it was made from
    (a back-row one's head high over a front-row figure). beta_roster.py
    lay_out now puts all 55 on the second page in three rows (tallest at
    the back; feet at 178, 218, 254; each row a third of a step over, so
    a coat, a body and a pair of legs do not run into one shape in the
    sepia), draws each cut-out for its row and points its hotspot at the
    face; markers are listed back row first. Harness: one page, the hand
    on Christina's face at the back. Old (an older 2.00 model,
    mod212.parse200) gets his own cut-out and portrait instead of Red's.
90. **The preview's own voices** (user). A character's .swt names its
    sound bank: CreatureSoundSet n is banks.dat's snd_<n>.hd/.bd (a DAVE
    archive (the preview's archive format) of PS2 HD/BD banks of VAG
    sounds; "IECSsreV"). The preview's named characters have their own,
    101-121 (Bat Boy 106, Pig Josh 107, O'Grady 120; its disc's loose
    NNv..vag clips are those banks' sounds), and the retail banks.dat still
    holds all of them: the retail characters were renumbered (O'Grady
    153, Pig Josh 151) and Ugly Chris kept 115, the same layout (736-byte
    headers alike). The preview characters were copies of a retail one's
    .swt and spoke with its voice (Red's for most). beta_roster.py now
    gives the 14 that had one their own bank; the rest (0 in the preview:
    no voice of their own) keep the copied one's. In the earlier .vag
    survey (MODDING) the clips had looked like mission dialogue.
91. **Menu music cutting out: a status line** (user: the menu music dies
    after cycling through many Showdown characters, or after idling in a
    menu; not seen in the harness: ten minutes of the line-up, the music
    went on). The toolkit's APU now writes a line a minute to the log:
    voices in the lists, on, streaming, and the most of each that minute,
    and at once when more than 200 of the 256 are in use (a title running
    out of voices would show there); with the lines already there for a
    stream voice on but still for 3 s and a voice on but in no list, the
    next log with the music gone should say which.
92. **The story's people in Showdown** (user: Brimstone's citizens, the
    train's people). tools/mods/story_npcs.py: 39 story characters on a
    third line-up page, each the game's own (swat.dat holds every one's
    files and model pack), drawn for the line-up from its model (retail
    xmod 2.12, its DXT textures through xtex.py's DDS export). With 55 more
    the poster crashed loading: a swui page's element list holds 150
    (sub_00019EF0 writes past it), so 39 fit. Harness: page 3 drawn, the
    hand naming "Old Timer"; a match as Wilbur, aiming and strafing.
93. **O'Grady '02 bent into a stick figure** (user video: a black frame
    with pole arms and red shards, whenever he was some way off). Not
    the rifle or the shotgun: his far LODs. The preview keeps most models'
    vertices relative to their bones, and beta_roster.py moves every one
    into the model's space; but O'Grady's lod_02 and lod_03 are in place
    already (1.53 m tall as they are, against 1.52 m for lod_01 once
    moved), and moved again they stood 2.89 m tall. A further LOD is now
    moved only if that brings it nearer lod_01's height
    (mod212.lod_to_model_space); of the 55, only his changed. Found with a
    test switch that makes every Showdown player one character
    (RDR_SHOWDOWN_AS=mqls, rdr_hooks.c) and by drawing each LOD.
94. **Guns in Brimstone** (user: in the fight with Mr. Kelley the
    townsfolk run about and can be shot; could the gun work in the other
    Brimstone visits?). The left trigger did nothing there: the player's
    button edges for it (+0x600), the fire button and the right stick are
    forced up while bit 1 of the player's +0xA04 bit set is on, and that bit
    is "inside a zone of type 4": sub_00145FE0 tests the player against
    the level's zones every update ([0x0065AA48], a list, each zone's type
    at +0x58; type 3 is bit 0). Brimstone's streets, shops and saloon are
    one type-4 zone; the Battle Royale's last day (lvl_royfinal) has none.
    Cheats > World > Guns in Town (settings.ini `town_guns`) gives those
    zones another type while it is on (rdr_overrides.c town_guns_zones,
    from the player's update) and 4 back when it is off. Harness: Red
    draws, aims and fires in Brimstone (LT edges 0 -> 3, draw state 3, the
    ammo going down); a townsman takes damage through his TakeDamage
    (vtable +0x9C, the "npchurt" test). Not yet seen: one of the harness's
    shots landing on a townsman (the lock went to the far end of the
    street); the user's test will say.
95. **Faceless story people on the line-up** (user screenshot: most of the
    39 had blank heads in their cut-out and portrait). Most story
    characters' faces are a model of their own, `<name>_face.xmod`, its
    vertices around the head bone (the .cr's `Face 1`); the pictures drew
    the body only. charpic.py now adds the face on the head bone's bind
    place plus the .cr's FaceOffset (head_place/with_face), for the story
    people and the preview's (O'Grady, Chicken, Gigolo, Whiskey).
96. **A Showdown of story people crashed loading** (user: at once, four
    players; log: a wild call to 0x41200000 under the character's set-up,
    sub_00087E90 -> the gait set at 0x0021C770). Reproduced in the
    harness with three Shopkeepers (RDR_SHOWDOWN_AS now takes a list,
    one character a player). Their .rep (the animation for each move)
    names the townsfolk's animations and gaits, which are in the story
    levels' archives and not loaded in Showdown. story_npcs.py now gives
    them Red's .rep and .msd and his .swt but for name, voice, hat and
    height; three Shopkeepers, and then Ashley, a Brave and the Barmaid
    together, played a match.
97. **Line-up pages 2 and 3, after 96** (user screenshots). The back
    row's tall figures stood over the middle row's: the photo is built in
    the roster's order (postermultiview's FONTGRIDDATA lines), not the
    markers': retail has both back row first, and addchar.py now puts
    the new roster lines in that order too (harness: page 3 drawn back to
    front). The mouse could not reach pages 2 and 3: the stick turns a
    page by pushing the hand against the photo's side, and the mouse's
    hand is held to the photo; now the pointer taken past the photo's
    side turns it (rdr_hooks.c rdr_lineup_pointer; once, until the pointer
    is back over the photo; harness: 3 -> 2 on the left, 2 -> 3 on the
    right). And "Train Robber" was the train's conductor (tdv, the
    "conductor" of lvl_traingood's .enm): named so now; Wilbur (brb) is
    the one you duel in the passenger car.
98. **Bat Boy's camera, after 88.** On the launch the follow camera rose
    with him but kept its pitch, and for most of the climb he was above
    the picture (harness frames, the ranch). rdr_flier_camera
    (rdr_overrides.c, from the camera hook sub_00180870_enter): with a
    flier in the air (flight state 1-6) within 20 m of the camera, the
    camera is turned up or down just enough to keep him within 0.3 of the
    picture's height (as a tangent) of its centre. The game's camera goes
    on from the matrix it was given, so the turn carries over smoothly.
    (Raising the camera's RiserStrength tunable, at 2, changed nothing.)
    Harness: in the picture through the whole climb, the glide and dive.
99. **Guns in Town: bullets went through the townsfolk** (user: no
    reaction at all; in the Mr. Kelley fight they bleed and die). The
    bullets' ray (sub_0015B590 -> sub_0014E000 -> the collision world's
    +0xDC, mask 0x00408B0F) did meet a townsman's body, and its hit went to
    sub_00088050: which stopped at 0x00088707: the one hit has to be
    hurtable by the shooter (vtable +0xB8, sub_0008BC20 -> sub_0008F9E0),
    and that is a 6 x 6 team table at 0x004065A8 (shooter's team
    [char+0xB4] the row, the one hit's the column, bit 1 = hurt). Players
    are team 0; Brimstone's townsfolk are team 3 (row 0: 0, the player's
    own side), the Mr. Kelley fight's team 4 (6). With Guns in Town on, in a
    level with the no-gun zone, column 3 of the players' rows takes column
    4's value (rdr_overrides.c town_guns_teams), and is put back with the
    option off or elsewhere. Found with RDR_DMG_LOG (each bullet hit and
    TakeDamage), RDR_RAY_LOG and the "raytest" test command (the bullets'
    ray from player 1 to each character). Harness: townsmen hit by bullets
    now lose health (100 -> 85, 100 -> 25).
100. **Online Co-op from the desk** (user: the Friends Menu instead of
    .bat files). The desk's prompt page (roominstr.swui; its class's update
    sub_00039840 is vtable 0x003C91D0 +8, so an indirect call and hooked
    without a regen) showed "Y Friends Menu" and Y opened an empty Xbox
    Live page. The text (0x003C9224) now says "Online Co-op" and Y there
    opens the F1 menu's Online Co-op page (the game does not see that Y:
    cleared in the XInputGetState wrapper while the prompt shows). The page
    is the lobby, src/net/rdr_lobby.c (docs/ONLINE.md): host or join by
    join code, the host's save and options to everyone, and Start starts
    every machine's game again in lockstep. Harness: two copies, joined by
    code, started, in step into chapter 1.
101. **The start's lettering still coarse** (user screenshots: the legal
    screen, "Loading game", "Press START"). Those are not .fonttex pages:
    the game makes two font pictures of its own at the start (a serif, every
    Latin-1 letter in rows, 256x256 and 512x256), so Sharp game text never
    knew them (RDR_SHARP_DEBUG lists the pictures it passes over, and writes
    out the ones it enlarges). They are now known by their pictures' hashes
    and enlarged 4x like the font pages (rdr_sharptext.c boot_atlas).
    Harness: "Press START" at 1600x900 with clean edges where it had steps.
    Back from the desk's Online Co-op goes back to the desk.
102. **The desk's Online Co-op not clickable** (user: fix 101's attempt
    did nothing). The prompt is not on the page of places the mouse looks
    at (frontendbuttons, with the arrows and "A Enter"): it is the
    roominstr page's readout, so no Y glyph was ever found, and the click
    there went to the journal behind it. hud_scan now tests the readout's
    own place ([page+0x454], with the glyph left of it) on the desk
    (desk_coop_hit) and sends Y, which opens the lobby once a press. Also a
    new profile's desk still said "Friends Menu": the readout takes its
    words once, before the page's first update renamed them: renamed at
    start now. Harness: a fresh profile's desk shows "Online Co-op", and a
    click on it opens the lobby.
103. **One join code** (user: why three?). There was one code per address:
    the internet's view of the PC, each home network, a VPN. Now one code
    holds them all (at most three; docs/ONLINE.md), the joining game calls
    every one and keeps the first to answer, and a random id in its hello
    keeps a guest that reached the host two ways one player. Harness: a
    code of an unreachable address and two reaching the host: one player,
    the save across, started in lockstep.
104. **Building from a disc image** (user: a setup for anyone new). The
    game's own installer (--install-iso, src/iso_install.c) is in the exe,
    which needs the disc to be built: so the first step is build.bat's.
    With no extracted disc, tools\build.ps1 takes a disc image dropped on
    build.bat (or -Iso), else the only .iso beside the project, else one
    picked in a window, and once Python is there unpacks it with
    tools\extract_xiso.py (the same XDVDFS reader: xiso or Redump dump; the
    image's default.xbe must be title 5454001A) into
    "Red Dead Revolver (USA)" beside the project. Checked: a test xiso at
    offset 0 and at the XGD1 offset unpack byte for byte; another title ID
    is refused.
105. **The 2002 preview characters from the launcher, and in Play As**
    (user: a launcher option, Play As with the preview's and any mod's
    characters, the rest converted, plain words).
    - Launcher page **Extras** (rdr_ui_pages.cpp PageExtras; also in the
      in-game list, switch only): "Set up the preview characters" picks
      the preview's disc image (GetOpenFileName; zenity elsewhere) and
      runs tools/mods/setup_preview.py with Python in the background
      (rdr_preview_setup.c: "py -3", then "python"; its PROGRESS lines
      on the page; Play waits), into Mods\Preview; "Preview characters"
      switches them; "Remove" deletes the folder. package.ps1 copies
      tools\mods into the play folder's Tools\mods. "Ride Out" is "Play".
    - Several mods folders (toolkit kernel_path.c: RECOMP_MODS_DIR with
      ';', the first with the file wins): on, Mods\Preview;Mods
      (rdr_settings_apply_env; RDR_MODS_BASE keeps the player's own, so
      a game started again for online play can switch it back off).
    - Play As and story co-op list the characters the mods add
      (rdr_modchars.c: m<code>.cr + m<code>.swt in a mods folder's
      swat.dat the game's lacks, named from its swat.strtbl), after the
      game's own; choices are kept by code until the mods are read
      (settings play_as_code). The lobby passes "preview on" (option 11,
      lobby version 2); a guest without them is told to set them up.
    - The long-coat Red (coc) converts: two cloth anchor bones its model
      does not number (mod212.fit_skeleton drops them), and its coat's
      blended vertices placed by their reskins (to_model_space). ann, jim,
      fat, rwc and slm have no models on the preview's disc.
    - beta_char.py gave a character's second preview texture the same new
      name as its first (the preview's Red: hat and gloves white); fixed.
    Harness: 56 characters listed for Play As from Mods\Preview; the
    preview's Red and the long-coat Red played in Brimstone, textured.
106. **One thing to start** (user: the launcher by default for new
    players; the .bat files as programs). New settings show the launcher
    at every start (show_launcher 1; files that have it keep theirs). The
    play folder's four .bat files are gone, their work done by the exe:
    with no game in Game\ it asks for the Xbox disc image (main.c
    player_folder; 107: on the launcher's Install page; dropping the .iso
    on the exe still works), Settings.bat is the launcher that now opens anyway, and
    Host/Join Online Game.bat (the streaming host, before the lobby) are
    replaced by the desk's Online Co-op. The exe knows its play folder by
    its own name, so an empty folder with no Game\ still works.
    package.ps1 removes the old .bat files by name. The project's own
    (build.bat, play.bat, ...) stay: build.bat has to run before there is
    any exe, and a downloaded unsigned .exe meets SmartScreen and
    antivirus warnings a .bat does not.
107. **Installing and mods in the launcher** (user: set the game up from
    its disc image in the launcher for new players; switch mods and
    texture packs on and off there).
    - A play folder whose Game\ has no game opens the launcher on an
      **Install** page (main.c rdr_game_missing; the window picker of 106
      is gone): the disc image picked there is unpacked in the background
      (rdr_preview_setup.c rdr_game_install_*, iso_install.c with its
      progress and now the image's title ID checked first: another game's
      disc is refused before a file is written), Play waits, and when it
      is in the launcher loads the game's own art (rdrui::game::Retry)
      and goes on as usual. Closed without installing, the exe says what
      to do.
    - Launcher page **Mods** (also in the game's list, from the next
      start): each folder in Mods\ is a mod and each folder in
      Mods\textures\ a texture pack, with an On/Off switch; the loose
      files and textures are one more of each. Off ones are kept in
      settings.ini (mods_off, texture_packs_off), so new ones are on. At
      the start the ones that are on become RECOMP_MODS_DIR (after the
      preview's) and RECOMP_TEXTURE_PACK (toolkit nv2a_gl.c: several
      folders, the earlier wins a texture; "=<dir>" without subfolders).
    Harness: an empty play folder opened on Install, a test disc image
    installed from it and the launcher switched to the game's art; mods
    ModA on and ModB off, PackA on and PackB off gave
    "Mods/ModA;Mods" and "textures/PackA;=textures".
108. **Online: the same preview and mods on every machine** (user: a
    check that both have the 2002 preview). Fix 61's check only covered a
    host with the preview and a guest without; any other difference in
    the files the mods serve (an older preview set-up, a mod on one side)
    would part the two games without a word. The host now sends a
    fingerprint of its mods (rdr_mods.c rdr_mods_fingerprint: per mods
    folder in order, each file's path and size; a Dave archive by its
    names and unpacked sizes, so another zlib packing the same files
    agrees; .txt left out); the guest compares its own for the host's
    preview setting, and says "set the preview up" or "switch on the same
    mods" instead of getting ready (LB_MISMATCH; roster states 2 and 3).
    Harness: preview on the host only -> "needs the 2002 preview" both
    sides; the same (no) mods -> fingerprints equal, started in lockstep.
109. **An extracted disc anywhere; the PC Options list** (user: build.bat
    only took a .iso, so an extracted disc under another name could not be
    built; the list ran over the prompts). build.ps1 now takes an
    extracted copy beside the project (any folder with default.xbe and
    swat.dat), a folder dropped on build.bat, or default.xbe picked in its
    window as well as a .iso. The launcher's Install page has "Already
    unpacked?": its default.xbe picked, that folder is the game (settings
    game_folder; main.c use_game_copy), title ID and swat.dat checked. The
    pause menu's PC Options list fits its rows between the title and the
    prompts (nine since Mods and Extras: smaller and closer above six).
110. **Spawner** (user: bring in enemies, people and horses, in co-op and
    online; "if it's not possible to load enemies from outside of a level,
    just focus on the enemies already loaded"). A level makes all its
    people when it loads (enmManager [0x0065A998]; instances of 0x003CE070
    linked from +0x24 through +0x1B8); one the story has not brought in yet
    has no controller ([inst+0xC] = 0). Watched in gdb in the bar fight:
    the manager's per-instance update (sub_00074D10) brings one in through
    sub_00072CA0 (thiscall on the manager: revive if dead, a spawn point,
    the instance's Spawn, vtable +0x10 sub_00076FD0) when byte
    [[inst+0x158]+0x40] is set (sub_00076900, the level's script), and
    takes it out when not. The spawner sets that byte, calls sub_00072CA0
    as from the update (return 0x00074EA8) and puts the newcomer in front
    of the player (rows of three, facing them; sub_001EB490 / sub_001EB4C0
    as co-op's placing). Cutscene actors (AI table "ACTOR", name at
    [inst+0x158]+0x42) are left out: brought in, Dan stood still. PC
    Options > Spawner lists the level's kinds by name (Showdown's "m" +
    type, the story people's table, else "Gunman n" / "Townsperson n"),
    enemy or friendly (the teams' table, below), how many wait; How many
    (1, 2, 3, 5, all) and In front of (a co-op player). Harness: Smitty and
    Navarro brought in in front of Red in the bar fight, and they fought.
    Requests are commands (rdr_cmd_run: low byte the command, next the
    count, high half the kind + 1): alone or in local co-op they run at the
    next pad read on the game's thread; online they ride in this player's
    input (LSN_PAD 22: the 18-byte pad and 4 bytes of command,
    rdr_lockstep.c) and every machine runs them at the same pad read, so the
    games stay the same. Horses are not in the enemy manager: not done yet.
111. **Protect Allies** (user: "give anyone who is innocent / a companion we
    are supposed to protect infinite health", the bar fight's barmaids).
    The teams' table at 0x004065A8 is 6 x 6 bytes, a team's row against
    each team; the players' row 0 reads 0 7 7 0 6 6: bit 0 set for the
    teams that fight the players (the bar fight's brawlers, team 2), clear
    for the barmaids (teams 0 and 4) and the shopkeeper (3). With Cheats >
    Protect Allies on, a level person (0x003CE070) whose team is not
    against the players, and who is not a player, has its damage made 0 at
    its TakeDamage (the hook of item 83): checked at each hit, so one who
    turns on you can be hurt. Story only. Harness: the barmaids took 50,
    40, 25 and 11 and stayed at 390 / 400 / 400 while the brawlers went
    down. Sent to online guests with the host's options (lobby option 13).
112. **Freezes: a hang report** (user logs: chapter 1 in split-screen co-op
    froze after a while; another froze as a chapter started from the book).
    In the first the guest CPU's report stopped altogether: one guest
    thread ran on with no kernel call (a loop with no way out), and the
    logs could not say where. The toolkit now names it: a guest thread that
    holds the guest CPU and has made no kernel call for 4 s gets a
    "[HANG]" line with the guest return addresses under and above its last
    call's stack pointer (kernel_bridge.c hang_report, from the summary
    every 2 s; again every 20 s). Also: co-op's "no friendly fire" skip of
    a bullet's hit (sub_00088050) now clears the shot's byte +8 as the
    game's own "not this one" exit does, instead of leaving it as it was.
113. **The book's Play As and Story Co-op: Accept, and the menu sounds**
    (user: Accept did not close them; "it would be neat if they made a
    sound effect like the other in-game menus"). On the pages opened from
    the chapter book, A (or Enter, or the Accept prompt) accepts and goes
    back to the book; on a switch or a choice it accepts instead of
    stepping it (the arrows and a click still step it), and a name picked
    from a list accepts too. The menu sounds are the game's own: every
    swui page's Move / Accept / Back slots call sub_0009B400 (thiscall on
    the sound player [0x0065B944], a static at 0x00578960; an index into
    the menus' table at 0x003D1150: the front end's 2, 1, 3
    (sub_000196B0/C0/D0) and the pause page's own 5 and 4), found by
    breaking on the voice allocation (sub_002C8020) as the pause menu's
    cursor moved. The PC Options pages call it on the game's thread at its
    next pad read (rdr_ui_sound): a tick as the cursor moves, Accept and
    Back.
114. **Co-op: nobody could be talked to** (user video, the train: "Press X
    to talk" up, X did nothing). A player's look for someone to talk to
    (sub_0013DFE0, thiscall on the player: the nearest person in reach and
    in front among the enemy manager's people) puts them in one global
    talk partner [0x0065A9A0] (sub_000736A0); the HUD of the player who
    found them shows the prompt. Every co-op player runs it each frame,
    and the next one's look (nobody near) set the partner back to 0 before
    X was read. Now a player who finds nobody does not clear what another
    player found in the last 0.1 s (rdr_hooks.c, rdr_coop_talk_scan).
115. **A laptop: no Press Start after the opening movies** (user log: 11,162
    reads failed, st=0xC0000001, one 339,968-byte music-stream read at
    197,634,048 over and over). The title opens its streams unbuffered
    (FILE_NO_INTERMEDIATE_BUFFERING), and the toolkit passed that on as
    FILE_FLAG_NO_BUFFERING: then Windows wants each read aligned to the
    host disk's sector, and the title aligns to the console's 2048-byte
    DVD sectors: fine on 512-byte-sector disks, refused on 4096-byte
    ones (many laptop NVMe drives). First the flag was no longer passed
    on; the next build hung before Press Start on the user's own PC ("Audio
    didn't halt after 300 frames", the title's wait for its sounds to stop):
with that change and the thread slots of item 112 reading guest
    memory at every kernel call as the only Windows-side differences, both
    were taken back: the handles are unbuffered again, and a read the disk
    refuses (ERROR_INVALID_PARAMETER) is made again through a buffered
    twin of the handle (ReOpenFile, kept until the handle is closed). The
    thread slots read guest memory only with RECOMP_THREAD_DUMP.
116. **The launcher in the parchment look on a laptop** (user: "it ran the
    wrong launcher"). The launcher draws in the game's own art once it has
    read it from swat.dat / front.dat; a look that failed once was kept
    for good, so a launcher that first looked before the game's files were
    there (or in another folder) stayed in parchment. Now the look is tried
    again when the game folder changes, and the launcher looks again every
    two seconds (five times at most) while it is in parchment and the game
    folder has a swat.dat.
117. **One folder** (user: "What is the point of the red-dead-recompilation
    folder at that point? ... can we just keep the folder contained within
    that folder?"). The project folder holds the source and the build;
    build.bat made the play folder, the toolkit clone and SDL2 beside it.
    Now they are made inside it: Red Dead Recompilation\ (the play folder,
    the disc unpacked into its Game\), deps\xboxrecomp\, deps\SDL2-*\.
    An extracted disc beside the project is still used where it is; a play
    folder an earlier build made beside the project is moved in with its
    saves, settings and mods (package.ps1), and a toolkit clone or SDL2
    already beside it is used.
118. **Freeze NPCs on the second chapter** (user: it "didn't work on the
    second level"). In lvl_hng the gunmen walking their paths kept walking:
    their own update was skipped, but a character's controller (vtable
    0x003CF54C, +8 sub_0008AE60, run by the enemy manager through
    sub_000871B0) moves it too (a gdb watchpoint on a gunman's position:
    sub_0008AE60 -> sub_0008A150 -> sub_000D1D60). The controllers' update
    is skipped too while the cheat is on: not the players' (the same
    class; the controller's character is [ctrl+8]). Harness: the four
    gunmen stood still, Red walked, and they went on when it was off.
119. **Cheats in Showdown** (user: "infinite health do not work on
    showdown"). Each Showdown player has its own cheat word
    ([[0x00652EE8 + n*0x114]]+0x44; the menus set player 1's), but no
    Showdown code asks for Invincibility. The players' TakeDamage (vtable
    0x003D9FA8 +0x9C, sub_0013C330) is wrapped: in Showdown, a player whose
    word has bit 6 takes no damage. Harness (two-player Showdown, the test
    command "phurt <player> <damage>"): player 2 150 -> 120 -> 90, player 1
    with Invincibility 150 -> 150.
120. **Always Win Duels** (user: "an always win duel cheat ... the opponent
    doesn't draw their guns or they miss"). Watchpoints in Chapter 23's
    first duel (lvl_roydusk 6, Red standing still) on the player's duel
    object ([char+0x190]: +0 the state, +0x30 the player, +0x34 the
    opponent) and health: the opponent's bullet hit (sub_00088050) decides
    the duel at once (sub_000961B0 -> sub_000951F0: state 6) and then does
    6,180 damage: so cancelling only the damage left Red dead. With the
    cheat on, a computer's shot at a player in a duel is skipped where the
    hit begins (rdr_hooks.c, as co-op's friendly fire), so the opponent
    draws and fires and misses; between players (Showdown's High Noon) it
    stays fair. Harness: three shots missed, Red grabbed, drew, aimed and
    shot the opponent (60 -> 15 -> 0) and the story went on ("That should
    leave a bruise!"). Cheats > Player.
121. **Give Weapon** (user: "receive any weapon of your choosing from the
    game"). The weapons are swat.guns' table ([0x00676210], [0x0067621C]
    of them, 0x410 bytes each, the name first: 55 on the retail disc,
    Revolver to the Sacred Bow and Dios Martillo); a character's weapons
    [char+0x80] (+8 a byte per weapon: carried). The characters' AddWeapon
    is vtable +0x104 (sub_00092880; the player's sub_00139E50 calls it and
    tells the HUD): (weapon, 0, 0, 0) adds it with its ammunition; a third
    argument of 1 only touches the count of one carried (the path taken
    for upgrades). Spawner page > Weapons: a searchable list (the gatling
    guns and level-only copies left out), Give to the player chosen there;
    a command like the spawner's, so it works in co-op and online.
    Harness (chapter 2): Widowmaker, Dynamite and Sawn-off given, the
    weapon wheel showed them.
122. **The spawner: animals.** The levels' animals are the enemy manager's
    too, class 0x003CE398 (Chapter 11's ranch: horses "shr", cows "bul",
    buffalo "bfl"; AI tables RANCH_HORSE, COW1.., BUFFALO), and come in the
    same way: the spawner lists them as "(animal)". Harness: a waiting
    buffalo brought in beside Red at the ranch. Only the levels with
    animals have them (the ranch, the stagecoach, the two Mission
    chapters).
123. **No clicks with a laptop's trackpad** (user). The game's menus read
    the left button as a state at each poll (xbox_FramebufferKeyDown), and
    a trackpad's tap sends the press and the release together: between
    two polls, so the click was never seen (a mouse's click lasts longer
    than a frame). A mouse button's press now reads as held for at least
    80 ms (toolkit, keyboard.c). The menus over the game and the launcher
    take their clicks as events, and were not affected.
124. **Co-op: weapons and money, shared or each player's own** (user: "one
    where the money is combined ... player one decides on what to buy and
    the weapon loadout, and it applies to each player. The other, where
    things are separated ... a shop page in PC Options, or ... a quick
    menu"). Each player object has its own money ([player+0x9FC], an int;
    GetMoney vtable +0x164 adds the HUD's counting-up; SetMoney +0x168 =
    sub_0013E2B0 sets it and the HUD) and weapons, but only player 1's
    are the profile's. Story Co-op > Weapons and money:
    - Shared: AddMoney to players 2-4 goes to player 1 (player_add_money),
      their HUDs show player 1's purse, and 2 s into a chapter they are
      given player 1's weapons (AddWeapon): player 1 shops and picks the
      loadout for all.
    - Each player's own: 2 s into a chapter players 2-4 are given back the
      money and weapons they ended the last one with (kept in
      Saves\coop_loadouts.txt; online, every machine starts the session
      with the same, in memory only), and buy on the Co-op Shop page (PC
      Options, and the Quick menu in co-op) at the game's own prices
      (invmasterlist.csv's Shop Cost, read from swat.dat): a command
      (RDR_CMD_BUY), so online each player buys on their own machine and
      every machine makes the sale at the same frame. The game's own shop
      screen cannot be one player's alone online: every machine runs the
      same game.
    Harness (chapter 2, two players): player 2 bought the Widowmaker for
    $2,000 ($2,227 -> $227, a second buy refused), the file kept it;
    shared, $100 paid to player 2 went to player 1 ($2,227 -> $2,327).
125. **Co-op in the Battle Royale chapters (23, 24).** lvl_roydusk 6 and
    lvl_royfinal 7 go on into their arena, lvl_royale, which is made
    current (sub_0007A250) with the same players and nothing loaded. Co-op
    took that for a new level and ended: the split screen went on with one
    player in its two views, a black picture. A switch between two lvl_roy*
    levels with the same player 1 now keeps co-op.
    Then the arena gave each player's character its own player brain for
    the new enemy file (sub_00137790 -> sub_001393F0: [char+0x158] = the
    character's brain for [0x0065A998], from a map at [char+0x348]):
    playerbrain_royale_6 for both. Both ran its intro (state
    "Royale_Intro"), and the first one through took the duel: player 2
    dueled, player 1 waited in the state before for good (its way on,
    condition 0:0x783 on DuelReady/DuelTime, did not come true while
    another player held the duel). Found by polling each brain's current
    state ([brain+0x78]), then watchpoints on the states' pending action
    and the duel's creation (sub_000E62C0's property 0x87 ->
    sub_00095440 + sub_00090340). Players 2-4 now keep the empty brain
    their spawn gave them (coop_brain_keep, put back each frame while the
    chapter goes on), so the duel is player 1's, as in one-player games.
126. **Co-op duels: every player duels** (Story Co-op > Duels; user: "co-op
    dueling ... every player duels"). The duel manager [0x0065B904] plays
    every duel object it holds at once (Showdown's High Noon, the story's
    duels against several men): sub_00095440 takes one from it (+0x14 the
    duel's settings), sub_00090340 puts it on a character ([char+0x190],
    [duel+0x30] = the character), and sub_00095560 starts them all (the
    manager's +0x80 set; sub_000950A0 on each, which points a player's at
    the level's duel opponent, [enemy file+0x24]). The player's side of a
    duel is the character's own (sub_0014A0B0 feeds it the character's
    input, sub_00095B70; the aim is sub_00146530), not the brain's. So
    when the manager starts a duel of player 1's, players 2-4 get one of
    the same settings, started the same way (the manager's +0x30, its
    count of players' duels, raised), against player 1's opponent; when
    player 1's is decided (state 6) or the manager stops, theirs are taken
    off (sub_00090340 with 0 frees one). The split screen stays through
    the duel's letterboxed camera, each player seeing their own duel.
    Harness, Chapter 23: both players in the first duel, the opponent
    killed mid-duel (test command npchurt): player 2's duel taken off,
    the story went on to the second duel, and both dueled again.
    Lobby options v6: the setting goes to every machine.
127. **Co-op: "rubber banding"** (user video: chapter 3's train, and
    Brimstone). Extra players were put beside player 1 (coop_put_beside,
    "moved") whenever they were 8 m off within 600 updates of play after
    the level started or player 1 jumped more than 6 units in an update.
    On the train a character's place is its car's (it changes by a car's
    length as one steps across) so every crossing re-armed the check and
    player 2, in another car, was pulled back to player 1 again and again
    (the log: 17 "moved" in a minute); in Brimstone player 2 was pulled
    back for walking off in the first ten seconds. Now the check runs once,
    at the first update of play after the level starts or after a jump
    that comes during a cutscene or within 180 updates of one (a script
    moving player 1), and only places a player who is far off then.
128. **Co-op: Brimstone's stores and the Sheriff's Office were black**
    (user video: entering the General Store; the Sheriff's Office cutscene
    at the chapter's end). Brimstone's interiors are levels of their own in
    the same archive (tomb.dat: lvl_bank, lvl_bar, lvl_closhop,
    lvl_genstore, lvl_gunshop, lvl_tombsher), made current with the same
    players and nothing loaded, as the Battle Royale's arena is (125).
    Co-op took each for a new level and ended, putting the player slots and
    the local player count back with the split screen's views still in
    place. A switch between levels of one archive (rdr_overrides.c
    chapter_group: those, the Royale's, lvl_barfight/lvl_theater,
    lvl_core/lvl_jailnew, lvl_mansinta/b, lvl_woods/lvl_waterfall) with
    the same player 1 now keeps co-op, and the other players are put
    beside player 1 at the first update of play (they stood where they had
    been in the other level: inside the store's walls, back in town).
    Harness (Brimstone, two players, test command `tp 1 -7.1 0.9 15.2` to
    the store's door, X): the store's inside in both views, out again into
    town. A view the game adds that is not a player's is now left alone
    with the whole list, and the list is logged when it changes
    (`[COOP] views:`).
129. **Co-op: the shared screen kept the top half** after split screen was
    switched off (user). One screen now also sets player 1's view to the
    whole screen.
130. **The story's people without the preview** (user: "cannot play as NPC
    characters without preview characters") **and Atlas** (user: the
    strongman of Professor Perry's chapter). The story's people were made
    into Mods\Preview with the preview's characters (setup_preview.py runs
    story_npcs.py there: one swat.dat and front.dat must hold both), so
    they went when the preview was switched off. setup_preview.py
    --people makes them alone from the game's own files (no disc)
    into Mods\Story People (launcher > Extras > Set up the story's
    people; the full setup makes it too), which the game uses in the
    preview's place while that is off (rdr_settings.c). Atlas is lvl_shernite's
    `Type rok, Name Atlas` (his .swt says Granite): story_npcs.py adds him
    (wra, page 3), and the spawner names rok Atlas.
131. **Co-op: the money counting sound ran on** (user: "the noise of the
    money being added to the total ... plays for way too long"). The HUD's
    money counter (sub_001D7760, the HUD at [player+0x890]) rolls its
    digits from what they show (+0x5436) to the money (+0x53F8), playing
    the sound player's 0xE when it starts and 0xF when it ends. The shared
    purse mirrored player 1's money onto players 2-4 with the game's
    SetMoney (sub_0013E2B0 -> sub_001D4E10), which blanks the counter's
    digits: so after player 1's counter had added the $8, player 2's
    counted the whole $2,000-odd up from nothing, its sound playing all the
    way. Now player 2-4's counter is player 1's, copied each update (the
    money, the digits, their step timers), rolling with it and never
    counting on its own; a level's start (separate money) sets the digits
    at once, and the Co-op Shop counts down from what is shown. Harness
    (Brimstone, two players, test command pay): player 2's counter rolled
    $2,227 -> $2,262 with player 1's, digit for digit, its own count never
    started.
132. **Showdown line-up: no empty page** (user: the story's people without
    the preview characters stood behind an empty preview page). story_npcs.py
    lays them out on the third page only when the preview's characters
    are in the same folder (their front.dat there first), else on the
    second; and the line-up (rdr_hooks.c lu_close_gaps) skips any page
    nobody stands on, moving the later pages' markers up: for a folder
    made the old way.
133. **A borderless window at the screen's size** (user: "launching in
    borderless 1080p, it launches in a window still just without the
    border"). The toolkit's window (fb_present.c) fits its size into the
    desktop's work area (the screen less the taskbar) and 1920x1080
    does not fit a 1080p screen's, so it stepped down to 1600x900 in the
    middle. A borderless window now has the whole monitor for room: at the
    screen's size it covers it, as borderless fullscreen does.
134. **Story Co-op in the chapter book with the mouse** (user video: the
    item was lit, a click did nothing). The book is drawn on a page that
    leans away, so its rows grow taller and further apart down the page;
    the mouse's calibration (rdr_overrides.c, s_page_cal "storyselect") was
    linear, measured on New..Player Options, and put Play As and Story
    Co-op's targets up to 15 lines above where they show: a click on the
    words fell between them. Measured again from the user's frame with all
    six rows (Continue..Story Co-op: centres 185/204/221/240/262/289 for
    page y 110..210, 640x360): now a curve (quadratic in page y), each row
    as tall as the spacing there, and the rows' slant to the left. Harness:
    the pointer on Story Co-op's words, a click: "[MENU] book: PCCOOP", the
    Story Co-op page open.
135. **Showdown line-up: a picked character's ring on the other page**
    (user: "the reticule stays on the screen"). The ring round a picked
    character is a child of that player's hand group ([lu+0x4F4+0x4C
    player+0x30], the pointing hand itself hidden once picked; its rings
    are page elements +0xC0+8 player and +0xC4, the second [block+0x34],
    the tip). A player's entry (lu+0x624+0x40 i): +0x14 in, +0x24 still
    choosing (cleared by the pick). Now (rdr_hooks.c, lu_tokens) the page
    a ring was picked on is noted; on another page the hand group is put
    out of sight, and back when its page shows again or the pick is let go.
    Harness: player 1 picks Red on page 1, player 2 turns to page 2: no
    ring there; back on page 1 the ring is round Red again.
136. **Black bars: an option** (Display, "Black bars": Off, In cutscenes
    only, Always: the game's). The game's letterbox is three things:
    - while playing, the world is drawn in a band 1.2 times wider than the
      screen: [0x00652B51] set at a level's start (0x000641B2), the view's
      aspect times 1.2 ([0x00404500], sub_000524F0) and its viewport cut
      to match (sub_000520A0): 400 of 480 lines; split screen clears it
      (0x00051AB7), which is why the split views had no bars;
    - two black quads with rough edges over that (sub_001D5F80, 125);
    - in cutscenes, the film object's own bands (sub_001DDC70 ->
      sub_001DD140, at least 20 lines on a 16:9 screen; heights as
      fractions at [[film+0x1C]+0x10..0x1C], drawn unless all four are 0).
      Found with RDR_PRIM_LOG in Brimstone's first cutscene: two quads a
      frame from 0x001DD213.
    Off: the flag kept clear (the game's own value put back when on
    again), the quads not drawn, the cutscene bands' heights kept at 0
    (and put back if switched on during that cutscene: without them the
    strips showed whatever had been drawn there). Harness, Brimstone: the
    whole 480 lines in play and in the cutscene, and the bars back when
    switched on again. The non-letterboxed path keeps the game's own FOV
    correction (0x00052705), as split screen does.
137. **Co-op Shop page in shared mode** (user: "nothing appears"). With
    weapons and money shared there is nothing to buy there (player 1
    buys in the game's stores) and the page only said so. It now says it
    plainly and has the "Weapons and money" choice itself.
138. **Co-op: player 2 teleported along in Brimstone** (user video: player
    2 left standing, then beside player 1). The user's log: the DualSense
    on player 2's port switched itself off ("controller 1 disconnected
    (SDL)") while player 2 waited, and the rule for a player with nobody on
    their port (enemies stood about near an unplayed character; it is
    brought along behind player 1 once 15 m behind) moved player 2 five
    times. That rule is now for a port that has never had a controller
    since the game started (rdr_overlay.cpp, rdr_port_ever): a controller
    gone to sleep or unplugged leaves its player where they were.
139. **Co-op: no lock-on to the other players** (user: "the game still
    treats the co-op player as a target and will lightly lock onto them").
    The characters' relation questions are vtable +0x38 (sub_0008F9E0,
    teams' table bit 1), +0x3C (sub_0008FAD0, bit 2) and +0x40
    (sub_0008F9A0, bit 0, enemy); for two players +0x3C answers yes when
    their sides (sub_0008B580, from [char+0x94]) differ, as in Showdown.
    RDR_REL_LOG showed the aim's candidate search over every pair of
    nearby characters (sub_001B0A80, the list at 0x00679A58) asking +0x3C
    both ways about the two co-op players: yes. Now (rdr_overrides.c,
    coop_may_aim, an override of the virtual) two story co-op players
    answer no, friendly fire on or off; bullets still meet them.
    RDR_COOP_AIM_OLD=1 gives the game's answer back.
140. **Guns in Town: the townsfolk come back** (user: the shopkeepers, the
    paper boy, Billy Cougar did not). One of a level's people (enmManager
    [0x0065A998], +0x24 then +0x1B8) killed lies for its body's time
    ([inst+0x298], 10 s), then the manager's update (sub_00074D10) takes
    it out ([inst+0xC] 0, [inst+8] 0): and, dead, never brings it in
    again: only the living are brought in. Harness ("enmkill", "enmdump"):
    Jody, "Kid" Cougar and a townsman killed stayed out, health 0, still
    wanted. The spawner's bring-in (sub_00072CA0) revives. Now, with Guns
    in Town and Cheats > World "Townsfolk Come Back" (default on), in
    Brimstone and its buildings, one of the level's people not against the
    players who has been dead and out for 60 s is brought in where the
    level puts them (one whose body the game keeps is taken out first,
    after 90 s, the way the update does). Harness: Jody and the townsman
    back, health 100, 60 s after. The option rides in the lobby's Guns in
    Town word (bit 1; lobby version 7).
141. **Fill ammunition** (Spawner page; user: "an ammo spawner for the guns
    that are spawned"). A character's weapon list ([[player+0xC]+0x80]),
    its table [+0x20] (items +0, u16 count +4, a "held" byte each [+8]),
    an item's gun per hand: +0x20 the clip's size, +0x24 loaded, and the
    ammo pool [+0x28] with the most it carries at +4 and what is left at
    +8 (the revolver: 6 of 6, 150 of 150). Fill sets both to their most for
    every gun the chosen player carries, as a command (online, with that
    player's input). Harness: drained to 1 and 10, filled to 6 and 150.
142. **Co-op: the talk partner is player 1's** (user videos: the talk's
    camera stayed on the paper boy; a store's menu opened again each time
    it was closed until player 1 left the store; co-op only). The scripts'
    "player" is player 1 ([0x00652E38], [0x00652E4C]); their
    playertalkingtome is partner == me. Since 114 any player's find was
    kept, so player 2 standing by the paper boy or the shopkeeper kept
    them the partner after player 1 had finished. Now another player's
    look is undone (partner and its 1.5 s hold [0x0065A9A4] as before it),
    as in a one-player game.
143. **Guns in the stores: not yet.** A store's interior has one zone of
    type 3 (bit 0 of [player+0xA04]), which both keeps the guns holstered
    (the draw check in the player's update, 0x0014A6A5) and gives the
    store its fixed camera (sub_00149970's sub_00148E30 / sub_00149550).
    Clearing it (harness, the General Store) let Red draw: and the
    camera became the street's, through the walls. Both are read inside
    the player's update, so separating them needs hooks there.
144. **Spawner: people came in behind the player** (user: "I don't think
    the spawner works properly"). The spawner put them along (sin, cos) of
    the player's heading [+0x5AC]; a character faces -z of its rotation,
    which is minus that (player 1 at heading -1.47: rotation's third row
    -0.995, 0.103, facing +x): so they came in behind, out of sight.
    Now in front, facing the player.
145. **New people** (Spawner, "New people (experimental)"; user: "a spawner
    that creates an npc ... if it's possible to load them from other
    levels"). A level's people come from its enemy file, plain text
    ("Enemy { Table .. Type .. Name .. Alignment .. Spawn 1 { Point x y z
    Angle a } }"), read by enmManager::Load (0x00075E50): the archives
    ([0x0065ABB0] vtable +0x24, name, "enm") give a stream, a tokenizer
    (sub_00085DE0) reads it, and for each block: Table, Type, the type
    (sub_00097900, a .swt in swat.dat), an instance (0x2A4 bytes,
    sub_00077D40; the type's +0x1B0 1 or 3 or sub_00096F30: an animal
    class), the rest of the block (sub_000779B0), "}", the end of the
    manager's list, then its AI table by name (sub_000764C0). rdr_npc.c
    takes the same steps for a block it writes: the archive driver's open,
    read, seek, size and close (the table at [0x00406268], called through
    it, so overridden without a regen) serve "rdrnpcmake.enm" from memory.
    The newcomer is brought in like a waiting one and put in front of the
    player. Any character the Play As list has (the story's, Showdown's,
    the preview's, the mods') can be made; it acts as one of the level's
    people does (that person's AI table, [table+0x42] its name), on that
    person's side, against the players (team 1) or with them (team 0).
    Found on the way:
    - A Showdown bot's table (default_bot) clears the "wanted in" byte in
      its first update and the manager took the new one out again within
      0.1 s; the people made are kept wanted while they live.
    - Swat.dat's own tables are not all usable: AIsneaker's start state
      has a command the game no longer knows (fatal "unknown command
      type"); generic ones (assassin, enemygroup1) stood still. So only the
      level's own tables are offered.
    - Bad Bessie made in Brimstone ended the game: "Couldn't find an empty
      hash table slot": the archives' cache of names not found
      ([0x00662368], 0x708 nodes, sub_000DA050) filled with her animations
      looked for under other names. Each new person now gets a new, empty
      cache with room for 0x1000.
    Harness, Brimstone: a second Jody, Jack Swift and Bad Bessie (acting as
    a townsman: she walks about town), three bandits; a warp after making
    them loads the next level as usual. Animals and horses not yet.
146. **Spawner: the game crashed after a new person died** (user log:
    Billy Cougar made, shot, and later the game crashed; he was "not in
    front of me" either). The spawner placed people the way a player is
    placed (sub_00137370): the point written at [p+0x5A0] and handed to
    the position setter (sub_001EB490), the heading at [p+0x5AC]. That is
    a player's own copy of its position; a level person (enmInstance) is
    0x2A4 bytes, so 16 bytes went past its end into the next heap block.
    For a person just made that was its own ammunition slots
    ([[inst+0x80]+0x14] 4..7 read -21.7, 0.8, 10.0, 1.7), and the first
    reset after death (sub_00076DD0 -> sub_000906F0 -> sub_0015D280) or
    the body's take-out read them as pointers. For the level's own people
    it was whatever followed them. The point now goes to the setter from
    the guest's stack, and the setter keeps the heading. A new person also
    gets the reset the loader gives every person once all are read
    (0x00076238, vt+4): its team, its spawn point, its timers. Without it
    the bring-in left a new person where its controller had last been
    (harness: always -11.3, 26.1 in Brimstone). The archives' cache of
    names not found is replaced once a level, not once a person.
    Harness, Brimstone: Billy made in front of player 1, killed, taken
    out, back 60 s later; no crash (before: the crash every time).
147. **Free camera: players move under it** (user: a trailer, "2
    controllers in and create cinematic shots using the keyboard or
    controller ... assign controls to the freecam and players right there
    when it is toggled"). The free camera took every port's controller and
    the keyboard (merged into its port). Now, while it is on, the keyboard
    is read into a state of its own (rdr_input_merge into an empty one)
    and each part goes its own way: settings freecam_kbm (the keyboard and
    mouse) and freecam_pads (bit n: player n+1's controller), 0 flies the
    camera, 1 plays: a playing keyboard is laid over its port's
    controller, and the mouse then turns the game's camera again
    (mouse_look). The rows are under the switch in the Quick menu and the
    Cheats page while it is on; a line at the top for 5 s after F6 says
    who flies it and who plays. Harness: the keyboard's stick flew the
    camera, then (set to play) moved player 1 with the camera still.
148. **Black bars off: the HUD after cutscenes and duels; the corner
    square** (user: "after a cutscene I did not have a ui until I enabled
    the black bars again"; "this little white square in the bottom right
    corner ... with a red square on top"). The cutscene bands' heights
    (the HUD's band object, [hud+0x1C] +0x10..+0x1C) were cleared every
    frame; the band object's own update (sub_001DD530, from the HUD's
    update, vtable +4 = sub_001D90E0) moves them towards its targets and
    decides from them when the bands are in and out. Now the HUD's update
    is wrapped: the heights are put back for it and cleared after it, so
    the view and the bands' drawing see none and the update its own. The
    skipped bar quads (sub_001D5F80) also end by setting the tint colour
    back to white (sub_00181530(-1)) before the HUD is drawn; that is done
    still. The square: a HUD piece ([hud+0x30], sub_001E6D10, from the
    HUD's draw) placed at 680, 454 of 720x480, under the bottom bar where
    the game hides it; found by making each immediate-mode caller draw
    points instead (RDR_PRIM_POINTS) in Chapter 23's first duel: 0x001E556D
    (sub_001E5500). While the bars are not drawn its first word's first
    word is 0 for the HUD's drawing (it then draws nothing) and put back
    after. Harness, the duel: the square gone with the bars off; bars on,
    off and on again in the duel shown and gone at once.
149. **Co-op: separate windows** (user: "an option to create actual
    separate game windows for each co-op player ... one player on one
    monitor, and another player on the other"). Co-op screen 3: the
    players' views are laid out as quarters of the frame (each the
    screen's shape: nothing stretched) and the toolkit's GL present shows
    each part on its own (xbox_PresentSetViews, toolkit patch): the first
    in the game's window, the others each in a window it makes on the
    presenting thread with the same pixel format and context
    (wglMakeCurrent on its DC, SwapBuffers), whose messages it pumps;
    keys and mouse buttons there are posted on to the game's window, F11
    or a double click fills its monitor, closing hides it. A cutscene
    (full screen) goes to every window. Headless, the other views are
    dumped beside the frame (<prefix>v1_NNNNN.bmp). Harness, Brimstone
    with two players: Red's view in the frame, Jack's in view 1, both
    whole and 16:9 with their HUDs.
150. **Stuck controls safeguard** (an idea from another port of the game,
    dennisabalo18-bot/red-dead-revolver-pc: on a slow PC its player walked
    on forever with the controls dead, and once it crashed in
    XInputGetState reading a device record of 0xCC). The game reads a
    controller through the XDK's USB stack, and XInputGetState
    (0x003BE6F4) copies out the last report the guest's driver got: if the
    driver stops asking, the last state stays. The toolkit now counts the
    reports each pad hands the guest (ohci_pad_reports, toolkit patch); a
    pad the game reads (its handle in the pad records, 0x004041D4 +
    port * 0x38) with none for a second is read straight from the PC
    (usb_gamepad_report, the same report the USB pad would give) without
    calling the XDK's reader, until its reports come again; logged both
    ways. Harness: no false alarm in normal play; with the reports taken as
    stopped (RDR_TEST_PAD_STALL=1) the stick still walked Red about.
151. **Replacement cutscenes** (user: "Replacement Cutscenes as a
    feature"; the idea from the other port). The movies start in
    sub_000705A0(names, count, ...): "%sbik\%s.bik" (0x003CDB40) for each,
    the music and screen set up, sub_001F7500 opens them; 0 back when they
    could not be opened, and its callers (seven) go on as after the movie.
    Now replaced (rdr_hooks.c, a regen): when every name has a file in
    Mods\videos\ (.mp4, .wmv, .m4v), the toolkit plays them
    (xbox_MoviePlay, toolkit patch: the title's thread asks and waits; the
    presenting thread, which has the GL context and is idle meanwhile,
    plays them in the ack loop (nvgl_movie_tick) with Media
    Foundation's source reader: 32-bit frames into a texture, at the
    file's shape (720 or 640 by 480 shown 4:3); 16-bit PCM through
    waveOut, which is the clock; Esc, Enter, Space, A, B or Start skip once
    let go), then the game's call is made with a count of 0: the same set
    up, nothing to play. Harness (Linux, no decoder): RDR_TEST_MOVIES=1
    takes every movie as replaced and plays none: the intro and the
    chapter movies went by and the game went on.
152. **HD texture packs** (user: "Upscaled Texture Packs"). Packs worked
    already (dump, replace by hash, packs folder); the game's textures
    cannot be shipped, so tools/mods/upscale_pack.py makes a pack from the
    player's own dump: Real-ESRGAN ncnn Vulkan when given (colour through
    it, alpha enlarged on its own), else Lanczos and an unsharp mask; font
    pages and tiny textures left alone; resumable. Checked: a 64x32 RGBA
    test texture came out 256x128 with its transparent hole kept.
153. **Version and update check**: VERSION (raised with each release) is
    compiled in (RDR_VERSION): the log's first line, a corner of the front
    end (not in levels), the launcher's About. The launcher asks GitHub's
    releases API for the latest release in the background (WinHTTP;
    rdr_update.c) and shows a newer one under Play/Quit and on About, whose
    row opens its page; nothing is downloaded. Settings: update_check.
    RDR_UPDATE_TEST=<json file> for tests.
154. **First start**: the launcher opens on a Welcome page when there was no
    settings.ini: display mode, sharpness (render scale), black bars, and
    controller or keyboard and mouse; Done (or any page) goes on to the
    rest.
155. **Crash box**: after the last-chance handler has logged and written
    the minidump, a box says the game stopped: Yes copies the log's start
    and its last 96 KB to the clipboard, No opens the Logs folder.
    RDR_NO_CRASH_BOX=1 leaves it out.
156. **This page's defaults**: Display, Graphics and Controls each reset
    their own settings (rdr_ui_pages.cpp page_defaults).
157. **The disc is read-only** (pre-release: "the save folder can't touch
    original saves"). Saves were already apart (RECOMP_SAVE_DIR: the play
    folder's Saves; the log now names it). The toolkit's NtCreateFile now
    treats the disc (\Device\CdRom0, D:) as the console does: an open asking
    to write is made read-only, one that would create or replace a file is
    refused (STATUS_ACCESS_DENIED); logged once each.
158. **Alt-Tab in a cutscene or a load** (user: "the game will auto-pause
    after tabbing back in. Please disable that"). The pause on losing the
    focus is asked for at the next frame of play; a loss during a load
    (no frames of play) was still waiting when play came back, and paused
    then. Only a loss in the last second pauses now.
159. **Names and two more story people** (user). beta_roster.py: trd is a
    shooting target (was "Teen Red"), nrd "New Red". story_npcs.py: the
    ranch hands Dirk and Gilbert (lvl_ranch's .enm, Type rhd: one model
    for both); the line-up holds 144 at most (its page's 150 elements:
    a fixed list at +0x40, the count at +0x298, sub_00019EF0), so the
    second Bartender (bt2) and the third Townsman (np4) are left out.
160. **Spawner: how many, where, guns, take them out again** (user: "work
    with the spawner. Start with the easiest"). New people gets **How
    many** (1, 2, 3, 5, 8 a press), **Where** (in front in rows of three,
    all around you in a ring of 5 m, or further off at 15 m) and **Guns**
    (as their side, none, armed), sent as RDR_CMD_NPCOPT (count<<8 |
    place<<16 | arms<<24) before the Make, so online games make the same.
    Each person is placed and turned to face you through the position and
    heading setters (sub_001EB490 / sub_001EB4C0, a vector on the guest
    stack). **Remove the people made** (RDR_CMD_NPCDEL) takes each one out
    the way the level does (wanted byte 0, vt+0x14, [i+8] = 0); a person's
    own script can ask to come back in, so the removed are kept on a list
    that the keep-in pass honours until the level changes. Harness: three
    made, removed, all three still out 15 s later.
161. **Scroll bars** (user: "it is hard to tell when there is more to a
    menu"). The pause menu's pages, the quick menu and the launcher had
    only two small triangles; each now has a bar (rdr_ui_game.cpp
    game::ScrollBar, called inside the rows' child): arrows at the ends
    (dim at the end of the list, a click scrolls a quarter), a track and a
    thumb sized to how much shows, which can be dragged, a click on the
    track jumps there, and the wheel over the bar scrolls. Nothing is
    drawn when everything fits. The character search list's bar is
    brighter.
162. **Spawner: animals** (the spawner list, step 4). The loader makes an
    animal when the type's kind ([type+0x1B0]) is 1 or 3, or sub_00096F30
    says so: 0x5E8 bytes, sub_000794C0 (vtable 0x3CE398) or, for kind 3,
    sub_000749F0 (0x3CDDA0). rdr_npc_make does the same, and gives the
    manager's riders' table one more entry ([mgr+0x18], [mgr+0x14] entries
    of 12 bytes (rider, ?, animal) one per animal the level loaded;
    sub_00073030 / sub_000730B0 look for an animal's entry or a free one).
    The level's animals' AI tables follow the people's in rdr_npc_acts
    (.animal); an animal is made only with an animal's ways and the other
    way round. Its controller does not take the spawn point (a cow with
    COW1's ways came in at 0, 0), so it is put in front through the
    position and heading setters. Harness, ranch: cows, horses and
    buffalo made, in rows and in a ring, and sent away again.
163. **No more than the level can hold.** The controllers are a fixed pool
    ([0x0065A960], [0x0065A980]; 15 on the ranch, 8 used by the level),
    shared by people and animals. The bring-in (sub_00072CA0) takes a free
    one (sub_00072C50) without checking there was one, and with none it
    crashed (the eighth animal made: sub_00076FD0 through a controller of
    nothing). rdr_npc_room counts the free ones; making, the spawner and
    Townsfolk Come Back stop when there are none, and the menu says how
    many more fit.
164. **Slider rows** (user screenshot: "Mouse sensitivity" ran into its
    "100%"). The amount was drawn before the left arrow with a '-' and a
    '+'; it is now after the right arrow, the track a little shorter, and
    the '-' and '+' are gone (the arrows say the same). The launcher's rows
    end a little before its new scroll bar.
165. **The launcher can be any size** (user: "free drag the launcher ...
    and the ui will dynamically adjust"). The window has a sizing frame
    and a maximize button (Windows: WS_OVERLAPPEDWINDOW, at least 640 x
    410; SDL: SDL_WINDOW_RESIZABLE). The board is scaled to fit and grows
    into what is left: taller, the rows take the extra height (the
    portrait and Play / Quit stay in the middle, the prompts and the
    bottom rule go to the bottom; the background picture is drawn as its
    top and bottom halves); wider, the rows get up to 400 more units and
    the rules are drawn on across (the picture in three parts, the gaps
    filled with a stretched strip of the rule); past that it is centred on
    black. While an edge is dragged, Windows runs its own message loop:
    the page is drawn from WM_SIZE so it follows the mouse; ImGui's fonts
    are stretched meanwhile and made again at the new size when the drag
    ends (fonts_follow). Tests: RDR_LAUNCHER_SIZE=WxH.
166. **Spawner: fighting where the level has no fighters** (step 5;
    findings, nothing changed). Swat.dat holds about 2,700 AI tables,
    other levels' among them (barfight_goon1, bankrobbery_gunman ...), and
    a made person can be given any of them by name. Harness:
    - Brimstone (lvl_tomb 2): default_bot (Showdown's) stood still;
      bankrobbery_gunman, agressivecovertest and bossenemygroup came in,
      ran about and never fired (player's health 300 throughout). The
      town is a no-guns level (the player cannot draw either), so the
      level itself may be what stops a fight.
    - The ranch: bankrobbery_gunman ended the game, "Heap overrun (5728
      bytes requested)": the game's bump heap (sub_0005D7xx: [heap+0x10]
      the next free byte, [heap+0xC] its end) has no room for a table the
      level did not load. The level's own tables are loaded already, so
      the menu keeps offering only those.
    A way forward would be a heap of our own for what the spawner loads,
    and finding what keeps Brimstone's people from fighting.
167. **Online Showdown** (the 4-player online Showdown, first steps).
    Harness, host and guest on one machine: through the desk to the
    line-up, the guest joins as player 2 with its own pad, both pick, the
    host starts Twin Rocks.
    - The match never began: both stood at the loading screen (25 min).
      Showdown's loading screen (sub_000685A0, on the level loader's
      thread while the main thread loads, which waits for it) asks for A
      or Start once its load is done (0x00068699: the pad record's edge).
      Its frames are loading frames, not lockstep frames, so the pads do
      not move there. Online, its reads now see A pressed and let go in
      turn (rdr_lockstep.c, s_press_loop): each machine goes on when its
      own load is done, as a chapter's load does.
    - Own view: each machine shows only its own player's part of the
      split screen, over the whole window (online_view, rdr_overrides.c;
      the toolkit's xbox_PresentSetViews with one rectangle). Player k has
      view k, and its port is its pad object's ([player+0x894], one of
      four at 0x006795C8). The views stay where the game puts them on every
      machine: what a view sees must not depend on whose machine it is.
      Two players: Showdown's strips (about 4:1, its HUD laid out for
      them; forcing quarters left the HUD laid out for a strip); three or
      four: quarters. Online story co-op uses quarters. A cutscene or the
      pause menu: the whole screen.
    Harness: in the match, the same fingerprints on both machines (no
    DESYNC), the host shows player 1's view, the guest player 2's. Then
    three players (host and two guests): all join, the match loads, the
    same fingerprints on all three (frame 5550), each its own quarter.
168. **Separate windows: cutscenes and the pause menu** (user screenshots:
    a soldier's close-up cropped to player 2's quarter; the pause menu cut
    into each window's corner; "player 2-4 unaffected by the menu
    opening"). Every window shows the whole picture while the HUD is off
    ([0x0040C730]: the talks that hide it without the cinema flag) or when
    the game draws one view. The pause menu ([0x00651FF4]) shows in the
    game's window and in the window of the player who opened it
    ([0x00652020], the game's "Player N" on the menu); the others keep
    the picture they had (toolkit: a rectangle with x1 <= x0 holds its
    window). The game is one game: it stops for everyone while paused, and
    a menu of each player's own while the others play would need the game
    to go on under it: not done.
169. **Long Coat Red's head** (user screenshot: an orange head, and a
    face upside down at his knees). His model numbers its bones with the
    coat's 40 (hung from root_coat, a cloth anchor it does not count)
    before the neck and head: 3..42 the coat, 43 neck, 44 head. The
    skeleton numbered them as its file lists them, so the head's 228
    vertices went to a coat tail's bone and his own head bone carried 3.
    mod212.drop_bones(first=True) puts a dropped anchor's children first
    among its parent's. Rendered from the converted model: head, scarf and
    coat in place. Needs the preview set up again.
170. **What the setup made** (user: Dirk and Gilbert not in the line-up).
    With the preview characters on, the game reads the line-up in
    Mods\Preview, made when the preview was set up: before they were
    added; "Set up the story's people again" only remakes Mods\Story People
    (used with the preview off). setup_preview.py now writes "setup
    version N" in preview.txt / people.txt; the launcher's Extras page says
    when a folder was made by an older one and to set it up again.
171. **Each player's own menu** (user: "everyone has their own menu in
    separate window ... on split screen, that would be neat too"). The
    menu (F1, both sticks, the pause menu's PC Options) belongs to whoever
    opened it: the port whose sticks were clicked, the keyboard player's
    for F1, the player who paused ([0x00652020]) for PC Options. Only
    that port's controller works it and is kept from the game; the others
    play on (the keyboard and mouse too, when it is not theirs). In split
    screen it opens over that player's part of the screen (the view of the
    player whose pad object is that port, rdr_port_view; the menus' design
    space fitted into it); with Separate windows, in that player's window
    (toolkit xbox_PresentSetOverlayView: the overlay drawn after that
    window's picture). One menu at a time: another player's sticks while it
    is open do nothing. Test command "menuport <port>". Harness: player
    2's Quick menu in the bottom half, and in player 2's window, player 1
    playing on.
172. **Line-up rings, computer players** (user screenshot: rings of other
    players' picks over another page). A computer player's entry (picked
    for it by player 1's hand) reads "still choosing" every other frame
    (harness, RDR_LINEUP_LOG), and its ring was put back each time; the
    page's ring rule (102) now takes "choosing" only once it has lasted
    half a second.
173. **Bat Boy's take-off crash** (user: one player and three computer
    players, all preview characters, "as soon as I tried to use the
    ability"). The crash dump: an access at 0x746F6C63 ('clot', text) in
    the pose blend sub_000DA930, from the flight's climb (sub_0025B200 ->
    sub_000D8E60 -> sub_000D8B40): one of the poses it blends points at no
    animation. Not reproduced in the harness (Bat Boy taking off, gliding
    and diving on Twin Rocks, against four of himself and against other
    preview characters). sub_000DA930 is now hooked: a blend whose poses
    lie outside the game's memory is skipped (the pose keeps its last
    frame) and logged ("[ANIM] pose blend skipped ... from <caller>"), so
    the game goes on and the log says which.
174. Quick menu: **Always Excellent** beside Always Win Duels (user).
175. **Spawner: bosses** (the spawner list, step 6). An .enm's BossType
    (read to [inst+0x1BC], at least 1; 1 for most people, 3 the level's
    boss, 2 its seconds) decides, at the bring-in (0x00072D8C), whether the
    person gets a name and health bar on the HUD (sub_001EB090): the
    bars of the canyon's Franco and Jose. New people's **Health bar** (On)
    writes BossType 2 (3 is left alone: levels look for their boss by it,
    sub_00072FF0) and the character's own name, not its code. Harness,
    Brimstone: "Kid Cougar" with his bar at the top right; gone again when
    the people made are removed. The made people's BossType otherwise 1, as
    the levels' (it was 0, read as 1).
    The pose-blend guard (173) read its poses one argument early and
    skipped real blends in Brimstone; fixed before release (the poses are
    the second and third of five).
176. **The music cutting out: what the log will say** (user: the music
    stops at random, every sound effect plays on, since the start). The
    user's logs: no voice ever streams ("0 streaming" each minute): the
    music is a looping voice the game refills, not a stream, so the stream
    checks (out of packets, still, out of its list) never applied. The
    toolkit's APU now times each voice from VOICE_ON and, for those on 20 s
    or more (in Brimstone: voices 85 and 86, looping), logs with the time
    since start: the game turning one off ("voice N off after T s"), one
    going silent or heard again (its volume to 0xFF0 or past), one stopped
    moving for 3 s (once it has moved: the submix voices that sit at 1 are
    left out), and each minute the list of them (list, paused, position,
    volume). The next log of a cut-out says which of these the music did.
177. **More than 144 characters on the line-up** (user: "If there is a
    144 character limit, can we extend it?"). The limit was a swui page's
    element list: 150 slots inside the page (+0x40, the count at +0x298),
    filled by sub_00019EF0 with no check: past 150 it wrote over the page
    (the 158-character crash of 92). Both are hooked now: an element past
    the 150th is kept in a list of ours for that page (the game's own add
    still runs, into the last slot, which is put back after), and the lookup
    by name and type (sub_00019D60, thiscall: name at +0x28, type at +0x48,
    case-insensitive) looks there when the page's own list has no match.
    Only the line-up's markers go past 150 (the last elements of
    characterlineup.swui): points on the photo found by name, never drawn,
    so the page's own loops over its list need none of them. The line-up's
    own marker table grows (LU_MAXHS 512, 8 pages). story_npcs.py: 400
    markers at most, 42 a page, and 19 more of the story's people (a second
    bartender, the dancing and saloon girls, the gambler, the showman, the
    trapper, the ghost, two braves, an officer, soldiers, a drifter, bandits
    and townsmen); setup version 3, so an older People or Preview folder
    says to set up again. Harness: 163 characters, 4 pages, no crash, a
    character on page 4 picked. Needs the regenerated build (two new hooks).
178. **The music cutting out, and a Showdown's end that froze** (user: "a
    crash and the music had cut out shortly before"; the log of that run,
    from 176's logging). The log: the music's voice (85) on, but in none
    of the voice lists ("list 0"), its position still (never processed
    again) twice in the menus before the match; then, at the match's end
    (the music turned off, the results shown), the picture stopped for
    good with one guest thread running for seconds at a time (the window
    was closed on it). How a playing voice leaves every list: when a voice
    goes idle the APU traps the front end (SE2FE_IDLE_VOICE, the voice in
    FEDECPARAM) and DirectSound's service routine reads FEDECMETH/
    FEDECPARAM, unlinks that voice and frees it. Here the service routine
    runs alongside the title's threads, whose PIO methods were carried out
    at once and each overwrote FEDECMETH/FEDECPARAM: in the harness about
    30 a minute arrive while the front end is trapped, among them
    SET_CURRENT_VOICE 0x55 (voice 85) before the music voice's parameters.
    A routine reading FEDECPARAM after one took that voice for the idle one
    and unlinked a playing voice (the music; and whatever DirectSound
    then did with the idle one it never freed). On the console those
    methods wait in the FIFO until the trap is cleared. Toolkit (apu_vp.c):
    while the front end is trapped, a method no longer touches the decode
    registers (logged, the first four, and counted on the minute's status
    line); VOICE_ON takes a voice that is still in a list out of it first
    (a second link would loop the list and drop the voices after it;
    logged); the status line counts voices on in no list; a list that
    loops is logged. Harness, a Showdown of four preview characters: 152
    methods kept in five minutes, no voice on in no list, the menu music
    and the match's sounds as before.
    And a **stall report** (toolkit, on in every run): the picture standing
    still for 20 s (RECOMP_STALL_SECS, 0 off) prints the main guest
    thread's registers, recent indirect calls and stack (GS lines, as the
    watchdog's, for tools/stackwalk.py), again at 60 s, and "moves again"
    if it does: a game the user closes on a freeze leaves where it was.
    Test command "hang <s>" holds the picture.
179. **Online Showdown with four players** (user: "I meant 4 player
    online showdown"). Harness, four games on one machine (host and three
    guests, 0.97): all four in, the line-up picked by each game's own pad,
    lvl_hng4 loaded; the same fingerprints on all four at every check
    (frames 900, 1200, 1400, 3171, 3480, 4300: menus, pause menu, play
    with all four moving and firing), no DESYNC, each game showing its own
    player's view. Earlier four games ran out of memory here; now about
    850 MB each, 3.5 GB in all. Left: four real PCs.
180. **The crash taking off as Bat Boy** (user: "a full showdown (1 player,
    me and 3 npcs) all beta characters, as soon as I tried to use the
    ability, I had a crash"; never seen in the harness). The crash.dmp left
    on the user's PC (8 November): an access violation in generated code,
    reading 0x746F6C63 ("clot") as an address; the guest stack it captured
    holds return address 0x000D4585 over and over, every 24 bytes for 4 KB
    and more: sub_000D4540 calling itself. That is the archive's name
    unpacker: a name in a Dave archive's table that shares a prefix with
    the entry before it (first code 56 or more) is unpacked by unpacking
    that entry first, so a run of shared names is a run of nested calls
    (the comparator sub_000D45B0, for sub_000D49C0's binary search). The
    game's own archives write a whole name at least every 32 entries (runs
    of 31 at most, swat.dat and front.dat both); tools/mods/dave.py shared
    whenever it could, and every archive the setup writes had long runs:
    5986 in the user's Preview and Story People swat.dat, 2499 in a
    preview stage. A lookup landing at the end of one went thousands of
    calls deep. On Linux a guest thread's host stack is 8 MB and it
    passes; on Windows the default is 1 MB, and each guest call is a host
    call of several times the guest's 24 bytes. dave.py now writes a whole
    name after 31 shared ones (MAX_RUN, pack_names; the names read back the
    same, and a rewritten swat.dat's longest run is 31), setup version 4
    (the launcher says to set up again), and the toolkit gives the guest's
    threads 8 MB of host stack on Windows too (reserved, kernel_bridge.c).
    Harness: four Bat Boys on the rewritten archives, taking off.
181. **Spawner: people from other chapters** (step 7; user: "characters and
    AI from other levels"). Characters from anywhere were already offered;
    their ways ("Acts like") were the level's own only, because a table
    the level had not loaded ran its heap out (166). The game's memory is
    memMemoryAllocator heaps: a list at [0x0065A5F8] (sub_0005D510 makes
    one and links it in; sub_0005CD10(base, size, 1, 0) gives it its
    memory), the current one [0x0065A608]; each a bump region (+8 base,
    +0xC size, +0x10 used) with 31 free lists (+0x24; [block+4] size,
    [block+0x14] next) and small-block pools (+0xE8.., pages up to 0x7800
    taken from the heap); sub_0005D650 (thiscall: size + 3; ret 0x10)
    allocates and ends the game when the bump runs out; a free finds its
    heap by address (memMemoryAllocator::FindHeap), so a block may come
    from any heap in the list. In the ranch the master heap (0x004257E0,
    46 MB of the contiguous window) has 1 KB left and the level's heap
    (0x81000010) 2-36 KB. Now sub_0005D650 is hooked (needs the
    regeneration): a request a big heap (1 MB or more) cannot give: as
    its allocator would look: a free block the same size or more than 16
    bytes bigger, or the bump; a small one as a pool page: goes whole to
    an overflow heap of ours, made then (8 MB from the low RAM,
    xbox_HeapAlloc: storage apart from the window). A call from inside the
    allocator (a pool's page) stays where it is. Normal play never uses
    it (harness: a preview Showdown, the ranch before the spawner).
    "Acts like" now also lists the story levels' people:
    tools/mods/npc_others.py reads every chapter's enemy file
    (lvls_sng/*.dat, <level>_var_<n>.enm) and writes
    src/rdr_npc_others.inc: one of each kind a chapter has (chapter,
    character, side, guns; 163; animals, actors and tests left out). The
    choice goes as a command (RDR_CMD_NPCOTHER, the index) before the
    make, so online games make the same. Their side: "As that person" is
    against the players if they fought the player in their own chapter,
    else on nobody's. Harness: the ranch, bankrobbery_gunman (the old
    "Heap overrun") and three of the bridge's Myers made (5728 bytes from
    the overflow heap, six times), then a warp to Chapter 2; Chapter 2,
    two of Myers fought Red (his health down). Test commands "npcother
    <i>", "heaps".
182. **Guns in the stores** (user: "guns in stores"; 143 left it). Inside a
    store the player's zone test (sub_00145FE0, thiscall on the player)
    asks sub_0007EE20(point, 3, 0) (returning at 0x0014617A) and sets
    bit 0 of [player+0xA04]; the draw check (0x0014A691), the fire check
    (0x001461C4) and the store's fixed camera (sub_00149970, from
    0x0014AA23, and the functions it calls) all read it. Three hooks (the
    regeneration): the zone test, while sub_00145FE0 runs for a player,
    answers "not in a store" with Guns in Town on (guns and fire as in
    the street) and remembers that player; sub_00149970 sets the bit
    again for its own call and clears it after, so the store keeps its
    camera. Harness, the General Store (Brimstone, `tp 1 -7.1 0.9 15.2`,
    X): its camera, Red with his gun up.
183. **Spawner: animals from other chapters; another chapter's table
    needing what a level lacks.** tools/mods/npc_others.py also writes
    src/rdr_npc_animals.inc: the chapters' horses, cattle, buffalo and
    dogs (the train's and rail-shooter's horses, which carry riders on a
    script, left out), offered in the Character list as "Cow (Chapter 11,
    animal)" (codes 2000 + i), made with their own chapter's table; dogs
    are people to the game (an animal class refused them), so they are
    made as people. An AI table names things of its own chapter by key: a
    crack zone (property 0x25, 0x000E2E6B: a herd's pasture, a gunman's
    cover) or a cannon (0x24, 0x000E2DFD: the bridge's gunners), and a
    level without it ended the game ("aiEntity::aiEntity: No crack zone /
    cannon whose name matches key %d in file %s"). While the spawner makes
    and brings in someone (g_rdr_npc_making), the fatal hook gives a
    missing crack zone the level's first zone (the table goes on: they
    go there or stand), and a missing cannon (or a level with no zones)
    marks the make bad: the one made is not brought in, or is sent away,
    and logged; the same for a state command naming a cannon
    ("aiActState::SetCommand ... no cannon", 0x000FF117, the train's
    cannon gunner), and the gunners of cannons and gatling guns are left
    out of the list (160 people). Harness: Chapter 11's cow and horse in
    Chapter 2; every one of the 160 made in turn (the ranch, Brimstone),
    none ending the game: crack zones given, cannons refused.
184. **Line-up names** (user screenshots): "Gambler" (jes) is Jesse Lynch;
    "Dancing Girl" (dcg) and "Saloon Girl" (kt2) are Katie O'Grady, the
    sheriff's daughter, at her first and second encounters ("Katie
    O'Grady (1st Encounter)", "(2nd Encounter)"); "Soldier" (dar,
    DefaultName Daren) Colonel Darren; "Bandit" (mrd2, Redhand, no hat)
    Young Red (Bloody Hand); "Drifter" (dvy) Griffin (story_npcs.py, and
    the C names for the spawner and Play As). A name with a note in
    brackets has the note on its second line. Harness: the six on the
    line-up, each name under its portrait in two lines. Set up the story's
    people again for the line-up.
185. **The preview's "Target" is Teen Red** (user: "Target (2002) is
    supposed to be an early Teen Red"): trd, first named for the shooting
    target it was taken for, is "Teen Red (2002)" (beta_roster.py
    OWN_NAMES); setup version 6.
186. **build.bat took the 2002 preview's disc image for the game** (user,
    a fresh install on a laptop: "is not an Xbox disc image ... despite
    the original game already being there, unpacked"). With no extracted
    game where it looked (the folder beside the project named Red Dead
    Revolver (USA), the play folder's Game\, any folder beside the
    project with default.xbe and swat.dat), it took the only .iso beside
    or inside the project: the PS2 preview, kept there for the
    launcher's Extras. Now an extracted copy is also looked for inside the
    project folder and in the folder itself, and must be Red Dead
    Revolver's (title ID 5454001A in its default.xbe's certificate); a
    disc image is only taken if it is an Xbox one ("MICROSOFT*XBOX*MEDIA"
    at sector 32 of the game partition, at the offsets extract_xiso.py
    tries), others are listed as skipped, and one given that is not says
    what the build needs (and where the preview goes). Checked with
    PowerShell 7: the script parses; the tests tell the game's XBE and a
    fake Xbox image from the preview's archive.
187. **The 2002 preview: what more it holds** (user: "look into what we
    can do with the 2002 preview"; findings, nothing shipped). SWAT.ZIP
    (25,479 files) beyond the characters and stages already brought over:
    - Missions: 326 enemy files (.enm) for 39 levels: the preview's
      story as it stood in January 2002: lvl_perf75 (31 versions; var 1
      is Red's childhood at Broken Creek with Billy the Kid, Red's mother,
      Ugly Chris, Whiskey, Gigolo, Hedgehog, Chicken), lvl_sher70 (33),
      lvl_shernite (14), lvl_sheriff (9; 16 people), lvl_hng (6),
      lvl_tomb (5), lvl_train (5), lvl_tng (4, the training ground),
      lvl_cyj (17 people), lvl_flash ... Every AI table they name is
      still in the retail swat.dat (leftovers), their characters are
      retail ones or the preview's own (jin, bil, ydg: our q codes).
      Test (tools of beta_level.py, the preview's lvl_perf75 built in
      place of Chapter 1 with its var 1 enemy file, types mapped): the
      level loads its people, then stops on their AI. With the retail
      leftovers of the tables, at a missing action file
      ("HelpMyMommy.actb": the leftover states were edited after the
      preview and name files that never shipped) and, stubbed, at
      "unknown boolean type '%d'" (0x001052E8: a state condition the
      final game no longer knows: the leftovers were not kept up to
      date). With the preview's own AI files put in the level's archive
      (it is looked in before swat.dat, so they win for that level only),
      the same "unknown boolean type": the stateb format kept its version
      (1.0) while its condition types changed. So the missions need
      either the preview's states translated to the final condition
      types (the .stateb format worked out first), or the preview's
      people placed as they were with retail AI tables chosen for each
      (their behaviour the final game's, the scripted bits lost).
    - Menus: 22 .swui pages (main, pause, shop, sound, the 1000-2100
      pages) in the same tag language as the retail ones (<BUTTON:>,
      <TEXT:>, <FONT:>), with their own fonts (.fontproj + .tga: Ammo,
      Score, Timer, ToughNums, EnemyName ...) and textures: a "2002
      look" for the menus and HUD fonts is a matter of converting fonts
      and textures and mapping the preview's page numbers to the retail
      ones' actions.
    - Sounds: 273 .vag (PS2 ADPCM): the preview characters' own
      sounds already use some (147).
    - AI and weapon tables (.csv: accuracy, body regions, fire tables)
      as the preview tuned them.

188. **The 2002 look** (user: "2002 menus/HUD look instead", with a
    30-minute video of the January 2002 build). Display > "2002 look"
    (settings `look2002`). The art is not the preview's own files: the
    retail swat.dat still carries the preview's HUD art, unused: the
    fonts timer (digits, ':' and a "TIME" picture as 'T'), toughnums,
    score (big digits, 'a'-'j' the small cents), ammo, and hud_bullet,
    hud_valor, hud_rifle, hud_sawedoff, hud_knife, hud_dyna, hud_fist
    (rdr_gameart reads them as it reads the menus' art; no setup). The
    preview's swat7.hud gave the parts and colours (HealthAndValor,
    Shutters, the weapons' arcs). Drawn by the overlay
    (src/ui/rdr_hud2002.cpp) over the picture, in a 640x360 frame scaled
    by the picture's height, anchored left and right: TIME (the time
    spent in the chapter, held in pauses, menus and cutscenes, new each
    level), $ with small ".00" top right (above the bosses' bars), the
    revolver's rounds in an arc (one per round loaded; two arcs for two
    guns), other weapons' pictures, the count (loaded + reserve, as the
    game's), the health bar (green, through yellow, to red, as
    HealthFull/Mid/EmptyColor), a red line and a badge (0-10) for the
    Dead Eye (the preview called it "valor", the retail game too
    (playervalor)) and the dark blue curved bands of the preview's
    battle "shutters" (the game's black bars are left out while they show,
    cutscenes keep them).
    - The game's parts mask [0x0067A1BC] (0x3FF in a level; the scripts'
      HUD command sub_000E62C0 sets and clears bits): found by clearing
      bit by bit (harness test command `hudmask`, the ranch): bit 0 the
      health bar, 1 the Dead Eye meter (both the health object
      [hud+0x28], sub_001E3030 / sub_001E2520), 4 the bosses' bars, 7 the
      ammunition ([hud+0x2C], sub_001E4230). The game's own health, Dead
      Eye and ammunition are left out by clearing 0x83 for the HUD's
      drawing only (hud_draw around sub_001D98D0, put back after), so the
      game's own switching still decides when ours show.
    - Read (rdr_overrides.c, rdr_hud2002_read): the health object +4 the
      health shown, +0x1C the Dead Eye tanks, +0x20 how full (harness:
      3 and 1.0); health [player+0x18] +0x10/+0x14 (300/300, 225 after a
      75 hit); money +0x9FC; the weapon held [[player+0xC]+0x80]+0x48, its
      item in the table at +0x20, a gun per hand (clip +0x20, loaded
      +0x24, reserve [+0x28]+8); its name from the weapons table, whose
      words pick the picture.
    - One view only (story alone, Showdown with one player): in split
      screen the game's HUD stays.
    - The pause menu (rdr_hooks.c, pause2002, from the next level load:
      each level carries its pause.swui): the preview's pause.swui as
      far as the retail page allows: brown tint (0.30, 0.20, 0.15)
      instead of the dark one, the scrolls hidden (kept: the page places
      them by name), font02 (the preview's lettering, in retail too) for
      the rows, "Paused" pulsing at the bottom right.
    Harness (the ranch, 640x360): the parts in place, the game's three
    gone and the crosshair and bosses' bars kept; the rounds count down,
    the knife's picture on the d-pad; the pause page tinted with
    "Paused".
189. **The log written through on every read** (user's laptop log:
    hitching, 83,000 lines in a session). The toolkit printed a line for
    every file read ("[READ]", 11,900, each flushed while the level
    streams) and every 200 draws ("[GPU] draw #", 64,000). Both only when
    asked now: RECOMP_READ_LOG=1, RECOMP_DRAW_LOG=1 (toolkit patch). The
    same log: Intel UHD graphics at render scale 2x, 17-56 fps; the user
    guide now suggests 1x on integrated graphics.
190. **Touchpads that report positions** (user: "the trackpad with the
    laptop still does not work with mouse controls"). Raw mouse input
    with MOUSE_MOVE_ABSOLUTE (some touchpads' drivers, remote desktops,
    Parsec, pen tablets: 0..65535 across the screen or the virtual
    desktop) was dropped. Now the difference from the last report, in
    screen pixels (the fraction kept), is the move; a jump of more than an
    eighth of the range, or a report after a quarter second of none, is a
    finger put down again. Windows' own touchpad pause while keys are held
    is a setting (Touchpad sensitivity: Most sensitive), in the user guide.
191. **Credits.** The README credits dennisabalo18-bot's
    red-dead-revolver-pc for the stuck-controls safeguard (150) and the
    replacement cutscenes idea (151), and says online co-op is highly
    experimental, with split screen over Parsec recommended for now.

192. **Online: Start in** (user: "an option to select the profile, or
    start a new game in the host menu? That way, the game can start
    instantly loading the level"; both games had stopped on the loading
    screen after a new game). The restart itself stays (every game has to
    start from its boot to stay in step), the menus go: the host's lobby
    has Start in (the menus by hand, Continue the story (the default),
    or A new game) given to the host's game alone as RDR_ONLINE_START.
    That game presses its own controller (rdr_input_text_press, now with
    6/7 for d-pad up/down): Start every 1.5 s through the logos and the
    title until lvl_front is named ([0x00652E70]); on the desk, once it
    listens (frontendbuttons the current page with its "Enter" prompt,
    PROMPT1, shown: the page is there while the front end still loads,
    and presses then went nowhere), A until the book is up; on the book (storyselect) the d-pad until story_loadgame or
    story_newgame has the focus ([page+0x31C]), then A; A on
    difficultyselect, the warning's Yes (left, then A), Begin (A) on
    bookchapterview. It stops when another level is named (the chapter
    loading) or after five minutes. Those presses are the host's pad, which
    every game reads in the same frame (lockstep), so every game goes the
    same way; nothing is written to the game. Harness (one game,
    RDR_ONLINE_START_SOLO=1 for tests): Continue: Brimstone (lvl_tomb)
    loads with no hand on the controller; New: up/down to New, the
    warning's Yes, Begin, Chapter 1 (lvl_perfnew) and its opening.
    Online, the presses go into the host's own pad report as it is sent
    (rdr_lockstep.c, rdr_online_start_merge: down 4 frames, up 4)
    (through the keyboard's filter they would go to the keyboard's port,
    nobody's when the keyboard is on another player) and the waits count
    the game's frames ([0x0065AA98]), not the clock: a slow machine's desk
    takes as many frames to come up as a fast one's. Harness (host and
    guest on one machine, 3 frames a second): New: A on the desk, the
    book's New, the warning's Yes, Begin, both games loading Chapter 1,
    their fingerprints the same (frame 1406).

193. **The 2002 look's title.** The preview's main menu stood on logo.tex
    (512x448, upside down as the preview keeps its pictures): red, the
    logo in black, a gunman's dark shape. The retail logo.xtex is another,
    older menu (blue, "Multi-Player / New Game"), so the picture comes
    from the player's preview disc: beta_look.py writes it right way up as
    a 32-bit .tga, Mods\Preview\2002look\title2002.tga, during the
    preview setup (a failure there does not stop the setup). Shown by the
    overlay (rdr_hud2002_title) over the game's title (from the first
    level named after the logos (the attract level, lvl_sher70) until
    lvl_front is first named (rdr_title_phase)) at 4:3 in the middle,
    the sides filled with its red, "Press Start" pulsing at the bottom
    right in font02 where the preview had "Press X To Confirm". Harness:
    the picture over the title, Start, the desk.
194. **"Message from Dear ImGui" over the Spawner page** (user
    screenshot: "Programmer error: a visible item with conflicting ID").
    The level's people and New people each have a "How many" row in the
    same window: one ID. New people's rows now have an ID scope of their
    own; and ImGui's conflict box (on by default in its debug builds'
    settings) is off unless RDR_IMGUI_DEBUG=1.

195. **Online: movies left out** (users' logs, host and guest: both in
    Chapter 2 with two players, the host waiting 100 s at lockstep frame
    1806 for the guest's pads right after bridge01.bik opened, then
    "DESYNC at frame 2610"). A Bink movie plays by the clock on the wall
    while the main loop goes on counting frames, so each machine came out
    of it at a frame of its own. Online (lockstep mode 4) sub_000705A0 is
    called with no movies in it (what it does when none open: its
    callers go on as after the movie) on every machine alike.
196. **Button pictures by controller** (Controls > Button pictures: As the
    controller, Xbox, PlayStation). The disc's fonts carry the buttons
    twice: berrylium and damaged with the PS2 version's pictures (cross,
    circle, square, triangle at 0xBA 0xBE 0xBD 0xBC), berryliumx and
    damagedx with Xbox's: the loader asks for "<name>x" through the format
    "%sx" (0x003CAC64, sub_000553B0 and its free sub_00055E80). Changing
    that format (tried first) loaded berrylium, but the free went to the
    other name and FindHeap failed; so the pictures are swapped on the
    GPU instead: rdr_sharptext.c, which already knows each font page by its
    coverage, pastes the PS2 font's button pictures into the Xbox page's
    button boxes as the page is uploaded (installed now with sharp text off
    too: then only that). PlayStation is chosen by the first controller's
    name (PS3/PS4/PS5, DualShock, DualSense, PlayStation, Sony). Our own
    menus (rdr_ui_game Text/Prompt) take the same pictures from berrylium;
    with no controller and keys and mouse on, their prompts are key caps
    (Enter, Esc). Harness (buttons = 2): the book's prompts "cross Accept,
    circle Back".
197. **"A Accept" did nothing** (user: "None of the Accept options on the
    PC Options menu work"). The prompt's click was used only on the chapter
    book's pages; elsewhere it now takes the row under the cursor (Enter,
    next frame), in the launcher too.
198. **Spawned characters' white streaks** (user video: Jody made with the
    spawner acting as a townsman: white streaks over the street, a
    building drawn see-through; the level's own Jody fine; the log's count
    of degenerate or NaN triangles rose from ~300,000 to ~1,600,000 in ten
    seconds after the makes). Harness: thin white lines near made Jodys
    with a townsman's table. A vertex program's output at NaN, infinity or
    absurdly far (more than a million pixels off) is now cut away in the
    GPU vertex shader (toolkit patch, nv2a_gl.c: its clip distances set far
    below zero), so such a model's triangles shrink to their good corners
    instead of streaking across the screen. The cause (the made
    character's bones not set up for the table it acts by) is still open.
199. **Visual Studio, slimmer** (user: "can we cut down the size of Visual
    Studio 2022?"). build.ps1 installed the whole C++ workload with its
    recommended extras (6-8 GB). Now only Microsoft.VisualStudio.Component
    .VC.Tools.x86.x64 and the Windows 11 SDK (22621), with --nocache (the
    installer's package cache not kept); CMake, which came with the
    workload, comes from pip ("cmake<4") when Visual Studio has none.
200. **Line-up rings, player 1 picking for every place** (user video:
    the rings of the picks made on other pages stood over every page). The
    page code (196's rings) went by a player's entry's +0x1C as "the
    player", but that is the place the entry's pointer is picking for:
    player 1 picking for all four moves on from place to place, so the
    wrong hand group was put away and the picked ones stayed. Now each
    place's hand group (lu + 0x4F4 + 0x4C place, +0x30) that the game
    leaves on a picked character (sub_0002AEA0, finding the character by
    name through sub_0002ACD0) is remembered with that character's page
    and put out of sight on the others; the entry's own ring (+0x2C, put
    by sub_0002AF20 while its +0x24 says picked) likewise. A hand a
    player's pointer is moving is never hidden. Harness: picks on pages 1
    and 4, then pages 4, 3, 2, 1: each ring only over its page.
201. **Stuck after Start, "Audio didn't halt"** (user logs, twice: after
    Start on the title, the log repeating "Audio didn't halt after 300
    frames", one voice held in state 0x15). sub_000179F0 (the title, the
    profile, "playerChoice") stops the sounds and waits, a frame at a time,
    until sub_000992B0 says the sound manager ([0x0065B944]) has nothing
    playing: its four channels ([+4] + 0x2C i, busy while +0x24 holds a
    sound) and its 48 streams ([+8] + 0x34 i, +4). The soft
    fatal error (rdr_hooks.c) let the wait go on forever. Now, 5 s after
    the game's own limit, whatever the manager still holds is let go
    (logged "[SND] the wait for quiet: ... let go") and the game goes on.
    Not reproduced here (a held channel the harness made was cleared by
    the stop before the wait).
202. **Key caps in the game's own prompts** (user: the main menu and the
    game still showed Xbox's letters on keys and mouse). The sharp-text
    hook (rdr_sharptext.c) now draws, with no controller and mouse aim on,
    a key cap over each button letter's box on the font pages
    (rdr_art_keycap, rdr_gameart.c: a rounded parchment cap with a dark
    rim, the key's name in Berylium, thickened): A "Enter", B "Esc" (as
    the menus take them), X and Y the keys bound to them. Our own
    prompts name X's and Y's keys too.
203. **"Â" for the PlayStation buttons in our menus** (user screenshot: "Â
    Quit"). Our menus read berrylium (the PS2 buttons' font) without
    saying that it was packed without '%' (the sharp-text hook did), so
    every letter after '%' was one off: 0xBE came out as the letter
    before it. Read with "%" skipped now.
204. **Online: the menus before a mission took no input** (user: in the
    shop before the mission neither player could buy, switch or leave).
    The weapon select, the shop and "Start / Weapon Selection / Shop"
    (Flash menus) run in the prelevel loop (sub_00069B20, frame from
    0x00069C15) while the level loads, so their frames were loading
    frames, where the pads do not move. A loading frame in which one of
    those menus ran is now a lockstep frame (pads passed round; its
    fingerprint the frame's number only, the load going on beside it at
    each machine's pace); the frames between, and the wait after Start,
    stay loading frames. Harness, two machines: shop browsed, dynamite
    bought (3536 -> 3316 on both), Done, weapon select Done, Start.
205. **Online: the chapter's loading show never ended**. After the shop
    the story's loading screen (sub_00066CA0: Red turning, then the camera
    closing in) stood for good: it is timed by the clock, and the
    lockstep clock only moved on lockstep frames. Loading frames now move
    it as any frame does, and the first lockstep frame after a load puts
    every machine's clock half an hour on from where the load began (the
    same on all of them; the game takes a big step after a load anyway).
    Harness: both machines into lvl_bomb, fingerprints equal at frames
    3000, 3500 and 3900.
206. **LLVM-MinGW instead of Visual Studio** (user: could llvm-mingw be
    used?). The port builds with it: cross-built here (llvm-mingw
    20250826, UCRT, x86_64; SDL2's MinGW package) to a 35 MB
    rdr_recomp.exe, after two changes: GCC's -fno-tree-pre / -fno-gcse
    only for GCC, and a missing declaration (sub_00019BD0) Clang 21 calls
    an error. CMake for MinGW puts the exe in build\Release and copies
    libc++.dll and libunwind.dll beside it. build.ps1 -Compiler clang
    fetches the Windows zip (about 175 MB) into deps\, CMake and Ninja
    from pip, SDL2's MinGW package into deps\mingw\, and builds in
    build-clang\; package.ps1 takes -Release. Not yet run on Windows.
207. **"Accept" in PC Options, again** (user video: Accept clicked on the
    Cheats, Spawner and Story Co-op pages, nothing). 197 sent an Enter key
    after the click, but the click on the prompt takes the keyboard focus
    off the rows (the prompts are drawn in the menu's own window), so the
    Enter reached none. The row the cursor was last on is now remembered
    (rdr_ui_theme.cpp row_begin) and AcceptCursorRow() makes it clicked
    the next time it is drawn (a switch switches, a choice steps, an
    action runs) in PC Options and the launcher.
208. **Fill ammunition filled nothing** (user). A weapon given with Give
    Weapon comes with an ammunition pool the level never stocked: the
    Lightning's 0 of 0 (harness), so filling it to its most was 0. Each
    weapon's own most is in swat.guns (the table at [0x00676210], 0x410
    each, +0x38: the revolver 150, upgraded 200, the knife 20; -1 for
    none): a pool below it is raised to it, one with no figure gets 200,
    then filled. Harness: the Lightning 0 -> 200, the knives 9 -> 20.
209. **The spawner's "in front"** (user: in the first chapter, people made
    in front came in by the tutorial's targets). A person acting as one of
    the level's own can be started where that one's table starts it. Made
    people now go through the character's setters (sub_001EB490 position,
    sub_001EB4C0 heading, sub_00219D60 its physics told) as animals and
    the level's own people brought in already did, and are held there for
    their first 180 updates: put back whenever they are found more than
    6 m away (logged "[NPC] ... held in place n time(s)"). Harness,
    Chapter 1: the training dummy and Red's father found by the practice
    area on their first update, put back once, stayed (4.3 m in front).
    The practice gunmen (PERFNEW_ENEMY1/2, groups Path1/Path2) are put
    back at the start of their paths every update whatever is done: after
    five they are sent away and the Spawner page says why ("they keep to
    a path of their own in this level: pick another way of acting").
210. **2002 look: the HUD over loading screens, the title over the intro**
    (user video). The 2002 HUD read the HUD flag, which a chapter's
    loading screen and the fade after it leave set while the new level's
    player already exists: it now shows only while the game draws its own
    HUD (hud_draw stamps the time; within 0.25 s). The title picture with
    its "Press Start" covered the intro from the end of the logos; it now
    waits for the game's own title menu (the Flash menu 'logo01X' seen).
    New setting look2002_bands ("2002 blue bands", Display): the bands off.
211. **"Again to ride"** (Go to Chapter, armed) now reads "Press again to
    start" (user).
212. **-Compiler clang: "CMake not found"** (user, the first run on
    Windows). Visual Studio's own CMake was always there before, so pip's
    was never needed; the lookup through Python failed on the user's PC.
    Now it looks where the module says and in Python's Scripts folders
    (per-user and the install's), and when none has it, takes Kitware's
    own zip (CMake 3.31.6, about 45 MB) into deps\cmake\; Ninja the same
    (ninja-win.zip, deps\ninja\). A failed pip install no longer ends the
    build.
213. **-Compiler clang: "Invalid character escape '\G'"** (user). CMake
    writes the given compilers' paths into its own .cmake files as they
    are, and in those a backslash escapes: F:\Games\... broke the resource
    compiler's. The paths go to CMake with forward slashes now, and a
    configure that stopped half way (a cache, no build.ninja) is cleared
    before the next.
214. **The exe's icon** (user). Our own drawing, nothing from the game: a
    revolver's cylinder end on, cream with dark chambers and red primers,
    on a rounded square in the menus' red with a cream rim, the banner's
    diamond at its centre, worn with red specks (tools/make_icon.py: drawn
    at 1024 and scaled, the small sizes from a plain version). It is
    resource 1 of assets/icon/rdr_recomp.rc, compiled into the exe on
    Windows (Visual Studio's rc and LLVM-MinGW's both), and the game's
    window takes it too (the toolkit's window class loads icon 1).
    Harness: the LLVM-MinGW cross build has it in .rsrc. The LLVM-MinGW
    build ran on the user's PC (1.0.9).
    The game's window and the taskbar still showed Windows' blank one
    (user): the icon 1 above is the separate views' class only; the game's
    own window is the toolkit's framebuffer window (fb_present.c,
    "XboxRecompFramebuffer") and the launcher's is ours, and neither class
    named an icon. Both take resource 1 now, at the large and small sizes
    (RegisterClassEx, hIcon and hIconSm; toolkit patch).
215. **The pre-release review's fixes** (docs: the review's findings
    table; B = blocker, H = high, M = medium, L = low).
    - B1, B2: the licence. LICENSE (MIT) for our code; the two toolkit
      files the patch changes that xboxrecomp took from xemu
      (src/apu/apu_core.c, apu_vp.c) are LGPL-2.1-or-later and stay so,
      with a dated change notice in each (toolkit patch) and the LGPL's
      text in LICENSES/. THIRD_PARTY_NOTICES says which is which.
    - H1: the hooks are tied to one build. main.c refuses a default.xbe
      that is not the USA version 1 (its size, header time stamp, title
      ID, region and version) with a message; RDR_ANY_XBE=1 lets one
      through for tests. build.ps1 checks the XBE's SHA-256 before the
      generated code is made from it (-AnyGame to skip). Harness: the
      retail XBE starts; one with its time stamp changed is refused
      (exit 1); RDR_ANY_XBE=1 starts it.
    - H2, M9: an application manifest (assets/icon/rdr.manifest):
      activeCodePage UTF-8, so the "A" calls and fopen take the UTF-8
      paths the runtime decodes (a folder named with accents works), and
      per-monitor DPI awareness. MinGW takes it from the .rc (resource 24,
      1), Visual Studio merges it from the sources. Harness: the MinGW exe
      has RT_MANIFEST 1. Windows 10 1903 or later reads activeCodePage.
    - H3: the online lobby's launch() reads the host's save package with
      every length checked against what arrived (the option count, each
      name and file size, in 64-bit sums), and takes only relative names
      inside Saves\Online (no "..", ':' or leading slash). A welcome with
      a player number or count this port never sends is ignored; a
      failed allocation is reported. Harness: two instances, lobby, join,
      the save sent, both started again in lockstep.
    - H4: build.ps1 keeps what is in the toolkit folder as a patch file
      (deps\xboxrecomp-before-reset-<time>.patch) before it resets it.
    - M1: Saves\Backup\1-3 (src/rdr_backup.c): at each start, when the
      saves changed since Backup\1 (a fingerprint of every file), UDATA
      and TDATA are copied there first and the older copies move down.
      Not in an online game (its saves are the host's). RDR_NO_SAVE_BACKUP=1
      turns it off. Harness: changed, unchanged and rotated starts; the
      game makes the copy at start.
    - M4: settings.ini lines up to 4 KB (game_folder and the mod lists can
      be 1023 characters). L5: settings.ini is written to a .tmp and moved
      over the old one once whole.
    - M7: .gitignore: build-*/, Saves/, Logs/, /mods/, /Mods/, disc images,
      dumps and recordings (tools/mods/ is ours and stays tracked).
    - M8: the README's online line says how it works over the internet
      (port 27032 forwarded, or a VPN).
    - L1: opengl32.dll from the system folder only. L2: play.bat and
      play_debug.bat find an LLVM-MinGW build (build-clang). L6: the
      update check is in the README and the user guide, with how to
      switch it off. L9: realloc's old block kept on failure (lobby,
      picture sender, lockstep recording).
216. **The spawner online; a second keyboard and mouse** (user: "We also
    haven't tried the spawner on co-op/online co-op"; "Let's try to get a
    second keyboard and mouse for player 2 working").
    - The spawner online: each command (make, options, "acts like", send
      away, Give Weapon, Fill Ammo) is one u32 riding with its player's
      input and run on every machine at the same frame (rdr_cmd_run at
      the pad read); what the run functions read is the command, the
      game's memory and the host's options. Harness (sp_online.sh): host
      and guest through the shop into chapter 1, commands from both
      sides (npcopts, two makes, a spawn, Give Weapon from the guest,
      Fill Ammo, send away): every one at the same frame on both, the
      same people at the same addresses and places, 3,927 frames' hashes
      compared, none different. The one thing a machine could have
      differed by, Give Weapon's RDR_GIVE_A3 (a bring-up switch for
      AddWeapon's third argument), is gone: always 0, as before.
    - A second keyboard and mouse (Windows, local co-op and Showdown's
      split screen; not online). Raw input names the device of every
      key and movement (header.hDevice). "Second keyboard and mouse are"
      (Controls, kbm2_player) gives the second set to a player; "Pick"
      takes the next key's keyboard and the next click's mouse (their
      RIDI_DEVICENAME, settings.ini kb2_device / mouse2_device; Esc on
      the new keyboard: no mouse). Keyboards are registered for raw input
      only while kbm2_player is set; legacy key messages still come.
      Then (rdr_input.c): the second set's keys, buttons, wheel and counts
      go to tables of their own and rdr_input_merge2 makes that player's
      pad from them with the same bindings (Enter, Esc, clicks in the
      menus as for player 1); player 1's controls read raw input from
      every other device (key_down, its Enter/Esc/clicks, its wheel), so
      the second set never moves player 1. Shift, Ctrl and Alt are kept
      by side (MakeCode, RI_KEY_E0); focus lost clears both tables. The
      camera hook (mouse_look) keeps its state per mouse and gives the
      second mouse to the camera whose pad object is the second set's
      port (mouse2_is_for), else the stick. pad_filter merges it into
      that port, which counts as live and is plugged in at start
      (RECOMP_USB_PADS). Off (kbm2_player 0, nothing picked, online):
      player 1's input is read as before.
      Harness (kb2.sh, RDR_TEST_KB2=1: the second set on with no devices;
      kb2key/kb2mouse inject it): story co-op, 2 players, chapter 1. W
      on the second keyboard for 3 s: player 2 walked 12 m, player 1
      did not move; 1,800 counts on the second mouse: player 2's camera
      bearing 1.1545 -> 5.2777 (2.16 rad = 1800 x 0.0012), player 1's
      unchanged. The device picking and the raw input itself need
      Windows and two keyboards: not run here. The LLVM-MinGW build
      compiles it.
217. **The review's remaining findings, and its speed-ups** (user: "let's
    work on the items in 3 and 4").
    - M3, the lobby: the join code carries a key now, 20 random bits made
      for each hosted game (rand_s, /dev/urandom), and HELLO carries it
      (LB_VERSION 8). A newcomer without it is refused (REFUSE 4: "did
      not take that"), except with no key at all from a home network, a
      VPN's range or this PC (an address typed in); wrong keys are
      answered five times a second at most. A welcomed guest's chunk
      requests are served 128 chunks (128 KB) per 100 ms at most. A path
      from the host's save that would not fit is refused rather than cut.
      Harness: the harness's address is outside the private ranges
      (192.0.2.2), so it is the internet's case: the code with one key bit
      changed (tools: badcode.py) refused twice and told; the real code
      joined, the save came whole, both started in lockstep. Twice.
    - M2, the downloads: build.ps1 holds the SHA-256 of every zip
      (LLVM-MinGW, CMake, Ninja, both SDL2s); a different file is deleted
      and the build stops. The hashes: CMake's from Kitware's own list;
      CMake, both SDL2 packages and LLVM-MinGW also from files inside the
      zips against the copies the user's PC unpacked from its own
      downloads (all equal); Ninja's from one download. The installers
      (Git, Python, Visual Studio) run only when Windows says they are
      signed by their publisher (not testable here). pip: capstone 5.0.9
      (what the generated code here was made with; a capstone that is not
      5 is replaced), CMake 3.31.6, Ninja 1.13.0. PowerShell 7 (Linux) here:
      build.ps1 parses with no errors (1.0.12's changes included), and
      Test-Download passes all five zips and catches one changed bit.
    - M5, the cheats: the drawing thread (the NV2A executor, the toolkit's
      nv2a_ack_thread, calls the overlay) wrote ammunition, the HUD's
      mask, the level change and the results' records while the game ran.
      rdr_cheats_tick now runs on the game's thread, at each pad read
      (XInputGetState, sub_003BE6F4): every frame, in menus and paused too,
      and online at the same point everywhere. The level asked for from
      the menu is published with a barrier and a volatile flag. Harness:
      Go to chapter (lvl_tomb 2) loaded it; with infinite ammunition on, a
      drain to 1/10 came back to 6/150 at once. (Infinite ammunition keeps
      the most seen since it was switched on; it never filled a gun.)
    - M6, the wait for quiet (201): read again. A channel's +0x20 is its
      state (0 idle, 1, 2, 3: sub_0009B4F0 sets it from its voice), but a
      stream's +4 is a pointer to its sound (sub_00098B80 reads +0x40
      through it), and 1.0.6 set those to 0. Now nothing in the sound
      manager is touched: sub_000992B0 (is anything playing?) is wrapped,
      and while a wait has gone on 5 s past the game's own limit it answers
      "quiet" (renewed as long as the game still waits) and the game goes
      on. (The hook changes the set of wrapped functions: build.bat
      regenerates once.)
    - L3: the archive tools (dave.py, beta.py) skip a name that would land
      outside the folder ("..", absolute, a drive). L4: the disc installer
      (iso_install.c) and extract_xiso.py never walk a folder twice, and
      the installer never an entry twice within one: a made-up image with
      a folder pointing at the root and an entry pointing at itself hung
      the old installer (killed after 10 s) and installs with the new.
      L7: docs/dev ships in source archives (the README links it). L10:
      a log that cannot be written goes to %TEMP% and says so. L11: the
      preview's removal goes to the Recycle Bin. L12: README's
      "Uninstalling". L8: the banner's lettering on a sunset of our own
      (no game picture). N1: -Wall -Wextra on our C: 6 warnings, 0 now.
    - O2 (guest RAM without volatile): not done. Guest threads and the
      hardware's registers share that memory; volatile is what keeps both
      right. O3 (a GL program cache on disk): measured first: a session
      builds 45-60 programs once each, and Windows' drivers keep their own
      cache of them; the GPU line in the log now says how long building
      them took ("programs built in N ms (the longest M ms)") so a slow
      card shows itself.
    - O1, -O2 for the generated code (CMake RDR_GEN_O2; on for build.bat
      -Compiler clang, -FastCode for Visual Studio; -ffp-contract=off in
      both modes, so no compiler fuses a multiply and an add). Clang 18,
      two builds alike but this: compile 275 s (-O1) and 379 s (-O2).
      Standing in chapter 1 for 120 s (measure.sh: each thread's CPU time
      from /proc, per presented frame), twice: 1,156 -> 1,252 and 1,226 ->
      1,329 frames (+8 %, the harness draws on the CPU); the guest's two
      busiest threads 12.99 -> 11.91 and 36.19 -> 34.85 ms a frame (-4 to
      -8 %). Less than the review's estimate (10-30 %). Online, a host at
      -O1 and a guest at -O2 (lsmix.sh: the shop, chapter 1, the spawner
      from both): 3,197 frames' hashes compared, none different.
    - M6 in the harness: "soundbusy 1" (a test flag: the check always
      says playing) and Start on the title: "Audio didn't halt" repeated,
      5 s later "[SND] the wait for quiet: gone on 5 s", and the game went
      on to the main menu.
218. **The demo in the online lobby; the IP address in the join code**
    (user: "when sitting in the online co-op menu, the game's demo mode
    takes over and starts playing"; "Isn't that join code a security
    issue?").
    - The demo: the front end's FrontEndToAttract transition (vtable
      0x003CBA90, made in sub_0005F340, in the list at +8 of the
      front-end screen [0x0065A810]; nodes {transition, next},
      sub_00064830) counts a float at +0xC down by the frame's time
      (sub_000644F0) and plays attract.bik when it runs out; a press on a
      pad sets it again. Our menus take the pads, so it ran out in the
      lobby. rdr_attract_hold_tick (game thread, at the pad reads) keeps
      each such timer where it was while a menu of ours is open or the
      lobby is hosting or joining. Harness, the desk with the lobby
      hosting for 15 minutes: before, attract.bik played and the main
      menu was built again; after, see below.
    - The address: a join code holds the host's addresses so the guests'
      games can reach it, the internet one (from STUN) included. Hiding it
      inside the code would not keep it: the code's key opens it, and
      every guest's PC sees the host's address in the packets anyway.
      "Internet address in the code" (the lobby's Host page,
      online_share_ip, on by default as before): off, the code holds only
      home-network and VPN addresses, which nobody outside can reach.
    - The demo, harness: before, the lobby hosting on the desk for 15
      minutes, attract.bik played and the main menu was built again;
      after, "[MENU] the demo held off (its timer at 101.7)" and no
      attract.bik in 15 minutes.
219. **The stagecoach flung into the sky at high frame rates** (user video,
    lvl_coach with fps_cap 500: once the coach chase starts, the coach
    flies off and around the map, the reins stretched after it; the
    level cannot be finished. At 60 it plays as it should: user).
    The coach is a rail vehicle ("RailVehicle": NumAxles, NumHorses,
    StartRail; loaded in sub_0020A130), moved by sub_00207790 (this = the
    level's vehicles, vtable 0x003EDAA8; arg = the vehicle; once a frame
    from sub_00208AC0). Its physics runs in slices: slice = frame time
    [0x00406504] / real time [0x0040652C] x [0x0040D034] (0.016 s), and
    `if (dt > slice) do { step(slice) } while ((dt -= slice) > slice)`:
the rest of the frame thrown away, and no step at all when the
    frame is a slice or shorter (fcomp, test ah 0x41). Above 62.5 frames
    a second the coach's physics never ran while its horses went on; a
    slow frame then ran one slice that met it all at once. The game's
    clock clamps a frame to 0.0001 .. 0.1 s, so at 500 a frame is 2 ms.
    rdr_hooks.c wraps sub_00207790: when a frame is no longer than a
    slice, the vehicle is left as it is (ret 4) and the frames' time added
    up; once it is more than a slice, it is moved once with that time (the
    frame time and the real time both set for that call, then put back):
    as the console moved it at 60 frames a second. Above a slice a frame
    (60 and below; online, 1/60) the game's own call, unchanged.
    "[COACH] rail vehicles moved in slices of ..." once a level says it is
    in use. Tests: "framems N" moves the game's clock N ms a frame (as at
    1000/N frames a second), "coachlog 1" logs the vehicles' places,
    "coachold 1" the game's own way. (The hook set changes: build.bat
    regenerates once.)
    Not checked in the harness: the chase comes after the canyon's fight
    on foot and the ride, which the harness cannot play through (the
    level's start, with "framems 8", moves no rail vehicle); left for the
    user's check at fps_cap 500.
220. **Crashes after a character loads: a dangling entry in the model
    table** (user crash logs: loading lvl_tomb 5 after lvl_trainend, an
    access violation inside the heap's free with a damaged block size
    0x0B0B0C10; then in lvl_bank, sub_000865E0 reading guest 0xBF800000,
    the float -1.0, as a pointer: called from the LOD loader at
    0x00273F5D through sub_00184870).
    - The cause was ours. The character model fallback for mods (the
      preview's Red, plain .xmod files: the hook on sub_00084140) asked
      the level path, sub_00184870 -> sub_00184730, for every model a
      character's pack lacked. Retail characters do lack some: no second
      LOD or no hat (own02_lod_02 and hat_own02_lod_01 in lvl_bank;
      dua, roo, mom, msk, frj in the harness logs). The level path looks
      in the game's table of loaded models (0x00678998: buckets of
      {name, model, next}, sub_000865E0 finds, sub_000867D0 adds,
      sub_001847B0 releases by value) and, on a miss, adds "<name>" ->
      nothing, in a 12-byte block and a copy of the name from the heap
      in use. While a character loads that is not the level's heap; the
      character's memory is let go with it and the entry stays. The next
      lookup in that bucket followed it into whatever had taken the
      memory (the -1.0), and adding or removing a neighbour wrote into
      it (the damaged heap block).
    - Now the fallback runs only when "<name>.xmod" is in the archives
      (the check 0x00083CC0 the pack-less loader already uses), and
      loads it with the loader itself (sub_001846B0, "model", the same
      four arguments) with no entry in the table; sub_001847B0 releases
      a model the table does not hold by its count, as before. A retail
      miss stays a miss, as on the console.
    - The heap's free (rdr_heap_free_ok, the hook on sub_0005D8F0) also
      checks the neighbours of a block before joining them: a free
      neighbour whose list links point outside the heap or off the
      16-byte grid is left alone and logged ("[HEAP] ... the free block
      before/after it has broken links", with the words), instead of
      being unlinked through them.
    - The free's own checks followed the block's back link before
      checking it: a damaged one (harness, after a warp to a level name
      that is not on the disc, "lvl_bank": the game's "not found on CD"
      path freed blocks whose headers held 0xFF950040) read off the
      guest's memory and crashed inside the check. The header, the
      previous block and the size are now checked against the heap
      before they are followed.
221. **Cutscene sound sometimes missing** (user: a cutscene, and the
    intro film after a relaunch, with no sound). Not found yet. The
    relaunch log has the film's voice (85) on for 75 s at volume 4A1,
    moving, and no "device stopped" line, so the voice was played; what
    is not known is whether it held sound. Two watches in the toolkit
    (always on, one line each time):
    - "[APU] voice N (...) has played only zeros for 5 s: nothing was
      written into its buffer": a voice at a volume that can be heard
      whose samples are exact zeros (the title's decoder, Bink's for
      the films, did not fill it), and "has sound again" after. Voices
      that never leave position 1 (a submix's) are not judged.
    - "[APU] the voices have sound but what goes out has been silent for
      5 s": the sound was lost after the voices (muted, the device).
222. **The shop's and the weapon screen's buttons by controller** (user,
    with a picture of the weapon screen: "It would be nice if the options
    in the Shop and Weapon selected were translated as well for whatever
    respective device is being used"). Those screens' A, B, X and Y are not
    font letters (196, 202 swap those) but 32 x 32 pictures of their own
    (Flash art), found in a texture dump of the weapon screen and the shop
    (RECOMP_TEXTURE_DUMP): A ae7507efc47a3202, B 3c4ed18d671845e1, X
    80619f645f353124, Y 62810b34cf1c21e2: the hashes the GPU hook gets,
    the same on every copy of the disc. (The disc also carries the PS2
    version's round buttons, which the game uploads but does not show;
    they are in no archive our art reader opens, so they are not used.)
    rdr_sharptext.c's hook now draws them 4 times bigger: with a
    PlayStation controller a dark round button with the PS2 mark from
    berrylium (A cross, B circle, X square, Y triangle), on keys and mouse
    a key cap (A Enter, B Esc, X and Y their keys), as the prompts. Harness
    (buttons = 2): the weapon screen's and the shop's "cross Done",
    "[TEXT] the shop's A button: PlayStation".
223. **Online: what parted, when the games part** (user: "a lot of desync
    when it comes to lower performing devices when playing online").
    The fingerprint every 30th lockstep frame says only that the games
    parted somewhere in those 30 frames. Each frame's parts (the frame,
    the game's own frame count and clock, both random-number seeds, the
    players' count, places and health) are now kept for the last 128
    frames, and at the first "[LS] DESYNC" every machine writes them from
    the last frame that agreed ("[LS] what each frame held ..."): the
    host's and a guest's logs side by side show the first frame and the
    first part that differed. The suspects on a slower machine are what
    runs by the clock on the wall rather than by frames: the sound (the
    emulated APU plays in real time; a game that waits for a sound to end
    sees it end on a frame of each machine's own), and the threads the
    main loop does not wait for.
    - One of those removed: the waits for quiet (sub_000992B0, "is the
      sound manager still playing?", asked a frame at a time by the
      title, the loading screens and the level flow: 0x00017E8A,
      0x000655A3, 0x000663C8, 0x0006C7AA, 0x0006DC6C ...) ended on the
      frame each machine's sound happened to end. In lockstep the wait is
      now done inside the frame: the question asked again with the CPU
      handed to the other threads between, until quiet or 0.3 s, then
      "quiet": so the game hears "quiet" on its first ask on every
      machine.
224. **The stagecoach still flung at high frame rates; held at 60 there**
    (user: "The Stagecoach bug is still there, as a temporary fix, can
    we automatically just cap the framerate to 60 for now"). 219's
    rail-vehicle slices did not cure it, so something else in the chase
    runs by frames. rdr_level_rate_tick (rdr_overrides.c, from
    rdr_timing_tick on the presenting thread) watches the level
    ([[0x0065AA0C]+0x14]): in lvl_coach, with a frame rate above 60 set,
    the vblank rate is held at 60 (xbox_VblankSetHz) and put back to the
    setting on the way out ("[RATE] ..." in the log); the Frame rate
    option says why while it is held. USER_GUIDE, Known issues. (219's
    wrapper stays: at 60 a frame is longer than a slice, so it calls the
    game's own way.)
225. **PC Options: Accept saves and leaves the page** (user: the Accept
    buttons "do not do as they should inside the pc options menu, and
    save and exit the current respective menu the player is in").
    Clicking the "A Accept" prompt on a page took the row with the cursor
    (207), which only changed that row. It now saves the settings
    (settings.ini) and goes back to the list: or, for a page opened
    from the chapter book, back to the book. A and Enter on a row still
    change the row.
226. **Go to Chapter from the main menu started the first chapter** (user:
    "not properly sending the player to the chapter when in the main
    menu"). The user's log: "warp to lvl_tomb 1 asked", "menu closed", and
    lvl_perfnew loading at once, with none of the warp's book steps. A
    press reached the game's book before the warp swapped the chapter
    page's level, and Begin took the book's own (the first chapter).
    Reproduced in the harness with A pressed every 0.6 s after the warp
    was asked (with the user's current save: the warp alone, untouched,
    still goes New, difficulty, chapter page, Begin, lvl_tomb).
    - A warp still waiting (stage 1) that finds itself in another level
      after the front end now goes on from that level, as from any level:
      Start, the pause menu, straight to the chapter ("[LEVEL] warp:
      'lvl_perfnew' started instead of the chapter; going on from it").
      Harness: lvl_perfnew, Start, quit at once, lvl_tomb 1.
    - The likely press: the Enter or click that closed the menu, still
      down after it closed. The keyboard's and mouse's own handling does
      not run while the menu is open, so it took the held key or button
      for a new press. Held A, B and Start are now kept from the game
      until released, after the keyboard and mouse are merged too (the
      controller's were already). Not reproduced in the harness, whose
      injected keys and clicks did not leak with or without this.

## Settings, launcher, overlay (src/ui)

- `settings.ini` (rdr_settings.c) becomes RECOMP_* switches at start-up.
- The GPU renderer now draws the window itself (nv2a_gl.c `nvgl_present`):
  render targets at RECOMP_RENDER_SCALE x the guest size (viewport and
  scissor scaled; guest memory keeps the 1x image: uploads scale up,
  read-backs scale down), FXAA while scaling to the window, letterboxing to
  4:3 or 16:9, then the title's overlay hook, then the swap.
- Cheats (rdr_cheats.c): the pause menu's cheats are bits of the profile
  word at +0x44 (sub_00056BA0). Bits: 6 Invincibility, 7 Golden Gun,
  10 Infinite Dead Eye, 12 Bite the Bullet, 13 Sad Stars, 14 No HUD,
  15 No Distortion; 8 and 9 are the debug Show Position / Show AI Info.
  Big Money is a call (player vtable +0x16C, add $10,000), not a bit.
- Always Excellent: the results screen (sub_00065A60) rates a mission on
  four counts against the level record [0x006577A0 + level*4] (40 slots,
  the current level from sub_000567D0): +0x14 time <= target, +0x18 damage
  <= target, +0x1C accuracy >= target, +0x20 combo >= target; three or
  more is "Great" (shown as Excellent). The cheat opens those targets wide
  each frame while on and puts the originals back when switched off.
- Money: player = [0x00675930 + [0x00652024]*4] (vtable 0x003D9FA8;
  GetMoney +0x164 = sub_0013E220, SetMoney +0x168, AddMoney +0x16C).
  Money is player+0x9FC; the HUD counter object is [player+0x890], whose
  +0x53F8 is the amount it shows and hands back, so both are written.
- Frame rate: the game moves by the clock (sub_00019120, rdtsc / 733 MHz)
  and only flips on vblank, so the frame rate is the vblank rate. The
  toolkit's vblank now runs at RECOMP_VBLANK_HZ (xbox_VblankSetHz from the
  menu); 30 is the presenter's cap instead.
- Frame rates above 60, part two: [0x00409DAC] is the length of one
  screen refresh (set at D3D start, 1/60). A few things step by it once
  per frame instead of using the clock (the blocking fade loop of the
  level title cards and transitions (sub_00105B40, t += swap interval *
  period at 0x001061AC), a progress animation at 0x0006FBA0, and
  0x00066CAF / 0x001F4883) so at 240 Hz they ran four times too fast.
  rdr_overrides.c first kept the period at 1 / vblank rate. That ran them
  slow wherever the frames came slower than the vblank (Unlimited = 500 Hz
  vblank under vsync on a 240 Hz monitor: the title card at ~half speed).
  Now rdr_timing_tick (once per presented frame: the present is per
  guest flip) measures the time between presents, divides by the swap
  interval [0x004097B8] (the fade multiplies by the mode it asked for,
  [0x0040987C]; modes 1-3 are intervals 1-3, 4-6 are 1-3 "or immediate"),
  smooths it (restarting when the interval changes) and clamps it to
  1/vblank .. 1/30 s: one vblank's worth of real time. Other frame-counted bits ([0x0065AA98]: a
  "within 6 frames" check, alternate-frame effects) are left alone. The
  frame pacing itself is the game's: sub_0017D680(mode) sets the swap
  interval [0x004097B8] (puppetshow cutscenes run at 2, i.e. 30 fps;
  Bink at 1), and sub_0017C300 waits for the vblank counter [0x00678350]
  that the D3D vblank callback sub_0017BEE0 bumps.
- Infinite Ammo: from the AI script conditions table at 0x00407E18
  (name -> id; "loadedbullets" 0xA4, "ammoleft" 0x6E) and the evaluator's
  switch (0x000F89DF): character = [player+0x0C], weapon list =
  [character+0x80] (current index +0x48; [list+0x20] = {item*[], count,
  valid bytes*}); an item holds two guns (hands); gun+0x24 = loaded,
  gun+0x28 -> pool, pool+8 = rounds left; -1 means endless for either.
  The HUD shows loaded + left. The cheat holds each at the most it has
  been seen holding.
- Mouse look (after Mercenaries Recompiled, which adds raw mouse counts
  as angles at its camera's angle accumulation): RDR's camera is CamMach,
  a dataflow program loaded from basecam.ccm (swat.dat) and evaluated each
  frame. It does not read the pad; the player code fills its input
  channels in sub_0013EC00, among them channel 14 (aim across =
  sub_0014CA40(stick x)) and 15 (aim up/down = sub_0014CB10(stick y)):
  deadzone 0.25, a log curve, a jump to a top rate at full throw (2.3 /
  0.8), times the sensitivity slider (response objects 0x00675F48 /
  0x00675F80 / 0x00675FB8). The program uses them linearly as turn rates
  (BaseAimRate or ZoomedAimRate x channel x frame time into bearing and
  tilt), so mouse counts x angle / (frame time x K) added to them turn the
  camera by that angle. Measured by injecting counts and reading the
  bearing/tilt the player republishes (channels 20/21): K = 1.25 rad/s
  across, 1.0 up/down, linear; channel 15 positive tilts down and channel
  14 positive turns left (the bearing grows; the first mouse-look build
  had left and right swapped). The hook:
  the channel container's GetChannel (vtable +4, sub_002730A0) is an
  indirect call; the one that returns to 0x0013F694 comes right after 14
  and 15 are stored, so the wrapper adds the mouse then (once per game
  frame's worth, re-added on the frame's second call). The stick layer
  stays as the fallback for cameras that skip this path.
- RECOMP_WATCH on Linux x86-64 now single-steps the trapped store (trap
  flag) and re-protects at once, so a busy page no longer hides the
  writer of the watched field.
- Menus and the mouse: [0x00651FF4] is the pause-menu flag (set by
  sub_0002F8C0, cleared at 0x0002F99A; 0x000709D9 turns gameplay input off
  from it). In the pause menu, or with no player loaded (the front end),
  the mouse works the menus.
- Pointing at menu items: the menus are swui pages (.swui in swat.dat /
  front.dat). Page manager 0x00651ED4: stack +0xA0 (count +0x128),
  current page +0x118 = [0x00651FEC]; sub_00029E30 calls each active
  page's vtable +8 (Update) every frame. Page: widgets +0x40 (count
  +0x298), focus index +0x31C, name +0x1C, shown +0x3D; SetFocus =
  sub_00019BD0(index, 1). Button (vtable 0x003C62E0): position +0x4C/
  +0x50 plus layout offset +0x58/+0x5C in the 640x448 space, text shown
  +0x114, font settings +0x190 (+4 byte 0 CENTER, +8/+0xC scale),
  selectable when +0xD8 and +0xE6. The 13 distinct Update methods of the
  35 page classes are wrapped (rdr_overrides.c); in a level the current
  page is hit-tested in the 640x448 space over the image (CENTER text is
  centred on the screen, widths ~14.8 x scale a character) and the item
  under a moved pointer gets the focus through SetFocus; a click is A
  over an item. The front end draws its pages onto 3D objects: the book's
  storyselect page and the profile sheet are calibrated from frames
  (position in image heights, which hold in 4:3 and 16:9); the desk and
  the chapter pages keep the stepping.
- Menus from the keyboard and mouse everywhere (rdr_input.c,
  rdr_overrides.c, rdr_hooks.c): in a menu Enter is A, Esc and Backspace
  are B; in play Esc is Start (pause). A key held across the change waits
  for its release (the Esc that opened the pause menu does not close it).
  The button prompts ("A Enter", "X Options", "B Back", ...: two text
  widgets, vtable 0x003C6B30, name +0x28, label +0x121, shown +0xE6, one
  showing a glyph by name: BUTTON_CONFIRM A, BUTTON_BACK B,
  BUTTON_ALT1_OPTIONS X, BUTTON_ALT2_MISC Y) and the page arrows
  (*_ARROW_LEFT/RIGHT, 0x003C6A50, 95x48) are clickable as that button,
  on the front end's frontendbuttons/roominstr pages, the book and profile
  sheet (through their calibration), the flat option pages and every page
  in a level; with prompts on screen the pointer shows, a click elsewhere
  is A (or, on a page of items, A over an item only) and the wheel steps.
  In the Showdown line-up the hand of the keyboard's player follows the
  pointer (rdr_lineup_pointer: tip under the pointer through the hand's
  SetPosition, clamped to the photo, then the game's pick, sub_0002D430);
  only while that hand is on the photo. Tests: "pointerpx x y" on the
  overlay flag, and k<hex> host keys in RECOMP_PAD_LIVE (toolkit).
- Mouse and keyboard (rdr_input.c): raw mouse (WM_INPUT) is counted; the
  camera hook above takes the counts each frame while it runs, otherwise
  they become the right stick (counts per second / (1600 / sensitivity),
  15 ms smoothing, lifted past a 0.2 deadzone). Keys become buttons,
  merged into pad 1 in the input filter.
- Controllers (toolkit): Windows now opens pads through SDL2 as well as
  XInput, so PlayStation, Switch and other non-XInput pads work; SDL is
  preferred for a port it has a pad on. build.ps1 downloads the SDL2 VC
  development package; without it the build falls back to XInput only.
- Crash deep into play (seen once, headless, ~4,600 frames in): XAPI's
  gamepad report handler sub_003BFAA2 (device vtable +0x24, called from the
  URB completion sub_003BF95C) copies (URB length - 2) bytes from the
  report, and the length at URB+0x66 read 0xFFFFF106 (the emulated USB
  host controller racing the guest's driver) so the copy ran off the end
  of memory (fault at guest 0x84000000). rdr_overrides.c now wraps the
  handler: a length outside 2..34 is dropped (logged as "[RDR] controller
  report with length ... dropped"); a real pad never sends more than 20.
- Mods: RECOMP_MODS_DIR (toolkit, kernel_path.c): a file there at the
  same path as one in the game folder is opened instead. See MODDING.md
  for the Dave archive and xtex texture formats.
- Showdown power-ups (rdr_overrides.c, powerups_apply): the reward
  manager [0x006766AC] (vtable 0x003DC9CC) pools 0x80 reward sets
  (vtable 0x003DC978, 0x14 bytes: +4 a list of {reward, next}, +0x10 a
  listener). sub_0015E470(manager, "name[:level][,...]") makes a set with
  one reward per name, found by name hash in the manager's table
  [+0x30] (count [+0x34]) of factories built from the registries at
  0x006774C8 + i*0x10 (gb_*, cr_*, pj_*). A character wears one set in
  [[character+0xC]+0x264]; the pick-up message (sub_0013AAD0 case 0xB)
  frees the old one (sub_0015DFB0: each reward gets message 6, back to
  the pool), then set->+0x14(0, character, [character+0xC]+0x268),
  set->+0xC(0, 0) and stores it. The cheat does the same from the
  player's update whenever the slot is empty. Display names are the
  strtbl keys of the same names (swat.strtbl).
- Frame rate in heavy scenes (split screen ~19 fps on an RTX 5080): the
  game's vertex programs run on the CPU in the toolkit (nv2a_vsh_interp.c),
  and the user's log ([GPUPROF]) put 70 % of every split-screen frame there
  ("transform"), 45 % even in the front end. They now run 64 vertices at a
  time, instruction by instruction across the lanes
  (nv2a_vsh_run_block): the per-instruction decoding is paid once a block
  and the arithmetic loops vectorise. Same results lane for lane as the
  per-vertex path (3,000 random programs compared); 4-6x faster on a
  skinning-like program. RECOMP_VSH_BLOCK=0 goes back.
- Story co-op fixes (user reports): (1) player 1 ran twice as fast:
  its update (sub_0014A0B0) ran twice a frame once player 2 was spawned
  (54:27 updates in 5 s, RDR_COOP_LOG=1); each player now updates once a
  frame (player_update in rdr_overrides.c). (2) Quitting a co-op level
  broke the front end (garbage in two halves): the players' list is not
  emptied when a level ends, so co-op never saw the end and kept writing
  the old level's views into the front end's list, with [0x00652E54] (local
  players) still 2. The end is now seen when another level is made current
  (sub_0007A250, rdr_hooks.c) or [0x0065AA0C] changes, and the player slots
  and local count are put back. (3) The black band across the middle of
  split screen and the clipped top line of text: the story's letterbox is
  two black quads per view (sub_001D5F80, 31 lines top and bottom); found
  by logging the callers of the immediate-mode begin sub_001825D0
  (RDR_PRIM_LOG) while RECOMP_PIXEL_PROBE watched the band. Not drawn while
  split screen shows the players' views. (4) The opening cutscene played
  twice, once per player: each player is made with the level's player
  brain (the .enm's PlayerBrain, parsed into [0x0065A998]+0x30), and the
  story's brain plays the level's opening. Extra players are made with
  Showdown's BR_playerAI instead (swat.dat), which drives the player and
  nothing else; verified: one opening, player 2 moves with its pad.
  (5) Player 1 still ran and animated at double speed in split screen
  (user video): player 2's slot (0x00675968 + 0x148, made as a copy of
  player 1's) kept player 1 in its character list (+0x118), so player 2's
  slot drove player 1 as well as its own: the slot read [p1, p2] in
  memory. The copy now leaves the list (+0x118..+0x13F) empty; the spawn
  puts player 2 there. Measured: player 1's distance per frame back to the
  single-player figure. (Player 2 still starts at the level's second
  spawn list, away from player 1: moving every copy of its position put
  it there but left its collision behind: it fell through the ground.)
  (6) Then both players followed both controllers ("mirrored"): with the
  list emptied the spawn still added player 2 second (+0x11C), and the
  empty first entry led player 2's controller back to player 1. Player 2
  is now moved to the first entry. Verified: each stick moves only its own
  player, also after Restart Level.
- New Showdown stages (user request: add levels instead of replacing
  them). The stage list is data: the LevelTable in front.dat's
  postermultiview.swui, read by the swui LOOKUP parser (sub_0001A130 makes
  a 0x13FAC-byte table, sub_000274A0 adds a row: 50 rows of 0x664 bytes,
  up to 50 cells of 32 bytes each, the row count at +0x13F88: no bound
  check). The poster (sub_0002D1D0) shows every row whose name the
  unlock manager (0x006585C0, sub_00057CE0) does not know or has
  unlocked, and wraps over the table's own count. Begin copies the row's
  name to 0x00652678 and calls sub_00056550(name, [0x00652E60]), which
  writes the next level (0x00652E70, 64 bytes, lower-cased) and its
  number ([0x00652E60]: the story's variation, Showdown's player count);
  sub_0005FF90 builds the archive: name+4 cut to 8 characters, the
  number's digit in the 8th place when above 1 (except lvl_front), under
  lvls_m4 ([0x00652E20] > 2 players), lvls_m2 (Showdown, [0x00652E30] ==
  1) or lvls_sng. So a row plus lvls_m2\<x>2.dat / lvls_m4\<x>4.dat is
  a stage: tools/mods/addlevel.py (verified: a 15th stage on the poster,
  loaded from mods\lvls_m2, played with a bot). Limits: 50 rows, 7
  characters of name, 31 per cell.
- Go to level (superseded by the level warp, 39; rdr_cheats.c, test command "level"):
  Restart Level (pause.swui's RESET button; the handler at 0x00029C8C)
  sets the HUD menu's flag 0x00652014 (0x00651ED4 + 0x140), read by the
  game object (0x0065A908, sub_00070990) on its way out of a pause. The
  reset (sub_0005EA50) reloads the archive only when the level flow's
  level ([0x0065AA08], sub_0007BA90) differs from the loaded one
  ([0x0065AA0C]+0x14); otherwise, and always in Showdown ("MultiWin"),
  it restarts in place, so a new name at 0x00652E70 is not read. The
  reload request is [[0x0065A6A4]+0x10] (sub_0006C8D0 -> sub_0006C400),
  ignored while a child mode runs: setting it in a Showdown match did
  nothing.
- Preview levels as stages (tools/mods/beta_level.py): "version: 1.09"
  models are rewritten as 1.10 (reskins count, material attributes line,
  junk tex2 indices zeroed), and spawn points with no loaded bound under
  them are dropped (the mine's list has three in its unloaded second
  base; a player there fell forever).

- Characters with models as .xmod files (rdr_hooks.c): a character's
  render set loads its models from its resource pack only:
  sub_00274550 / the pack's completion (0x00073CC5) call sub_00274400,
  which asks sub_00273F20 for the LODs, hat and the rest only when a pack
  ("<name>_lods.xrsc") came in, and those ask sub_00084140(name, 'MOD0'),
  which searches the loaded name tables only. Levels' zones go through
  sub_00184730, which falls back to "<name>.xmod" (sub_000846B0 ->
  sub_00184450, quiet when missing). Now: with no pack and
  "<name>_lod_01.xmod" in the archives (sub_00083CC0), sub_00274400 asks
  for the models anyway, and a MOD0 miss from those loaders takes the
  level path. The preview's 2.1x models keep vertices relative to their
  bones (the root's 1.125 subtracted: red_lod_01's first vertex 0, -0.012,
  -0.100 against the retail text copy's 0, 1.113, -0.076), so the tools use
  the retail disc's text copies of the same meshes where there are any.

## Notes for the Windows build

Fixes 2, 3 and 8 are Linux-only gaps in the toolkit; the Windows build gets
the same behaviour from its vectored exception handler. Applying the patch on
Windows is harmless (the decoder's context type there is still `CONTEXT`).
Everything else applies to both.

## Earlier open issues (now fixed)

A. **Bink reader crash at the start of the title movie** (intermittent,
   about 1 run in 3). Root cause: the toolkit's `PsCreateSystemThreadEx`
   ignored **CreateSuspended**. Bink creates each movie reader suspended,
   fills in the reader's context (including the event it waits on), then
   calls `NtResumeThread`; the reader ran at once instead and read a stale
   handle. Fix (toolkit patch): a suspended spawn waits on a resume event
   that `NtResumeThread` sets. Two scheduler changes made along the way
   (creator keeps the CPU across thread creation; no hand-over inside a
   non-blocking kernel call) narrowed the window and match the console, so
   they stay.

B. **Crash as the front end loads, right after the title movie.** The
   resource loader's chunk-translator list (head at `0x0065ABB8`, 15 static
   nodes) ended up holding "PMT0" instead of a node pointer, and
   `sub_00083F50` followed garbage. Reproduced on Windows too.
   `RECOMP_WATCH=0x0065ABB8` (with the Linux backtrace added to the patch)
   named the writer: the toolkit's **USB host controller model**
   (`ohci_thread` -> `ohci_walk_eds` -> `wr32`). XPP builds its endpoint
   descriptors in contiguous memory and hands the controller their physical
   addresses; one block sat at physical `0x0065ABB0`. `bus_resolve` treats
   any address inside the loaded image as the image, and this image is 8 MB
   (to `0x0082C5F0`), so it walked the game's globals as an ED: it rewrote
   `ED.HeadP` (= the list head) with the "next TD" word and stamped
   completion codes into the PMT0 node. On the console contiguous memory
   never overlaps the image's physical pages. Fix:
   `RECOMP_CONTIG_AVOID_IMAGE=1` (toolkit switch; keeps contiguous blocks
   out of the image's physical range), on by default in `main.c`.

- Typing the profile name: Gunslinger Profiles (page vtable 0x003C7948)
  has a letter grid while +0x44E is set: "ABCDEFGHIJKLMNOPQRSTUVWXYZ<>"
  then End (0x00403FA6, '\n') at 0x00403F88, +0x468 cells, [0x0040405C]
  per row, cursor row/column +0x46C/+0x470. A on a cell is
  sub_00033EF0(ch) (thiscall, ret 4): '<' deletes, '>' is a space,
  anything else appends to the name at +0x45C (7 at most). Typed keys go
  through it; Enter moves the cursor to End, refreshes the hint
  (sub_000336B0, plain ret: it needs a return address pushed) and makes
  one A press.
- Unlockables: manager 0x006585C0: +0x14 the table from Unlockables.csv
  (221 entries: 48 characters, 16 stages, 147 journal pages, 10 cheats;
  u16 count at +4), +0 the saved data shared by all profiles (0x006534A8),
  whose +0x24 holds a bit per entry. sub_00057C90(i) tests (that bit, or
  the session bits at manager+0x18), sub_00057D30 sets. The retail binary
  still reads a command-line switch "unlockall" (sub_000833D0) that
  implies unlockmulti and unlockjournal (read at 0x0002A050, 0x00036F05
  and others); unlockcheats, unlockdiffs and lvlaccess are set by it but
  read nowhere.
- Profiles: 0x00653508 + slot * 0xB18 (slot [0x00657798]); two mode
  records of 0x3D4 (story, bounty) at +0 and +0x3D4, the one in use by
  +0x7A8; in a record +0 money (float), +0x23C the current level's name
  and +0x27C its part (the chapter book's "continue" page), +0x292 the
  chosen difficulty; the name at +0x7BC, the cheat word at +0x800.
- Diagonal movement with the gun up: keyboard diagonals send full throw
  on both axes while the left trigger's key is held (0.707 each, the
  stick's circle, otherwise).

- Fatal errors: the retail game keeps its 333 fatal checks but prints
  nothing; sub_000175B0(fmt, ...) is just exit(1), and the CRT's exit ends
  in KeBugCheck(0) (0x00189714). The toolkit's KeBugCheck used to return,
  so the game spun in thousands of bug checks with a frozen window. It now
  logs the guest stack (return addresses and any text the stack points
  at: the fatal error's format string is among them) and ends the process.
- Out of sound voices: one such fatal error is sndControl::AllocateVoice
  (sub_002C8020, 60 voices at 0x006841E8, 0xC0 each; +0x14 the sound, -1
  free; +0x16 state; +0x1C eight DirectSound buffers). It only fails when
  every voice's buffers still report playing (IDirectSoundBuffer_GetStatus
  through sub_002C7F60). An entry hook (rdr_hooks.c) steals the voice held
  longest the way the game's stop command does (0x002C87ED) and logs it.
  The [CS] lock traffic in the user's log before the bug check is DSound's
  lock 0x003A3208 taken from sub_002C7F60/sub_002C8000.
- Entry hooks: `void sub_XXXXXXXX_enter(void)` in src/rdr_hooks.c is called
  at the top of the generated sub_XXXXXXXX (the generator's
  --exclude-manual; tools/regen.* pass the file, build.ps1 regenerates when
  the set of hooked functions changes). Used for directly-called code that
  rdr_lookup_override (indirect calls only) cannot reach.
- Camera matrices: the camera's world matrix [0x00678828] (rows right, up,
  back, position) and the view matrix [0x00678720]; sub_00181870 /
  sub_001818A0 / sub_001818F0 set both, call sub_00180870 (view-projection
  and culling planes), then D3D SetTransform (sub_00337810(0, view)). The
  free camera writes both at sub_00180870's entry.
- Characters: player vtable 0x003D9FA8 (Update at +8 = sub_0014A0B0); the
  sibling classes 0x003D9C30 (+8 sub_0013AE50), 0x003CF7B0 (sub_000921C0)
  and 0x003CE070 (sub_00076F80) are everyone else; Freeze NPCs skips their
  +8. Found with RDR_ICALL_PROFILE=1 (count every indirect-call target;
  "icalls" on the test flag prints the busiest).
- Players: sub_00138090 makes a level's players from descriptors at
  0x00653338 (0x5C each: +0 player number or -1, +4 character, +0x18 a
  second character, +0x4C/+0x50 control, +0x54 camera, 0x006795C8 +
  n*0xB0 the cameras), one in the story and four in Showdown
  ([0x00652E30]); each is a character in [0x00675930 + n*4] (count
  [0x0067592C]) with player slot 0x00675968 + n*0x148, whose number picks
  the controller (sub_00136D80). The level code also reads command-line
  switches "friendlyfire" and "sharehealth" (the latter only with more
  than one player, [0x00652E54]): traces of co-op in development.

- Frame-counted fatal errors: "Audio didn't halt after 300 frames" (six
  places), "SwapLevels(): ... 300 frames", "Sound bank took longer than
  300 frames to load" give up after a number of frames written for 60 a
  second; at 500 the harness hit one at boot. rdr_hooks.c wraps
  sub_000175B0 (the generator's sub_X_gen wrap) and returns for those and
  for the out-of-voices error, logging them; every other fatal error is
  logged with its caller and still ends the game.
- Play as: player 1's descriptor type (+4) is replaced at sub_00138090's
  entry. The first chapter's is "mrd"; Showdown's are the 48 "m???" types
  (mpcharbounty.csv), each a .swt in swat.dat. Verified with mjck (Jack
  Swift) and mwyn (Governor Griffon) in the first chapter. The names are
  the line-up's own: the front end's swat.strtbl (front.dat) keys them by
  the code without its "m" ("wyn" -> "Governor\nGriffon"). A .strtbl:
  u32 languages, u32 offset per language, the key names (u32 length,
  text, 0), then per language u32 count and records {u32 ELF hash of the
  key, u16, u32 + font name, u32 + UTF-16 text, f32 x2, u16}
  (tools/mods/strtbl.py).

- Story co-op, option A (one game, two players): RDR_COOP_TEST=1 runs
  sub_00138090 again right after a story level makes its players
  (sub_001EA7D0 called from 0x00062BC0), with player 1's descriptor
  renumbered 1 and player slot 2 copied from slot 1, then puts the new
  character in the set-up's arrays ([esp+0x78], [esp+0x88] of its frame)
  and counts two local players ([0x00652E54]), so the set-up gives it a HUD
  ([player+0x890]) and a view of its own. Then, at the player's first
  update (override of the player vtable's +8, sub_0014A0B0):
  - controller: [player+0x894] is its pad object, one of four at
    0x006795C8 (0xB0 each, one per port; sub_001A8390 updates them,
    sub_001A8490 picks the active one). The spawn gave player 2 port 1's;
    set to 0x006795C8+0xB0 it walks on the second pad, on its own.
  - views: 0x00652B2C holds the views drawn each frame ([0x00652B48] of
    them; sub_00051770 adds one, sub_00051790 removes one; sub_00051970
    draws them all, sub_000524F0 sets one's rectangle). A view: +0 its
    camera, +4 aspect, +8..+0x14 the rectangle now and +0x1C..+0x28 the one
    it eases to (x, y, width, height in fractions of the screen; the aspect
    is derived from them in sub_000524F0),
    +0x34 the clear colour, +0x38/+0x40 a fade colour now/target. Two
    full-screen views drew the whole scene twice (half the frame rate) and
    showed black. One screen (default): the count goes back to 1, both
    players on player 1's camera: verified, picture and speed normal.
    Split (F1 > Cheats > Story co-op, or RDR_COOP_SPLIT=1 top/bottom,
    2 left/right; 3-4 players: quarters) keeps the views with their
    rectangles set. It was black: sub_000524F0, making a view current,
    clears it (colour + Z, [view+0x34]) unless [0x00652B24] is set, and the
    story draws a frame in several passes (sub_000517F0 per layer range),
    each making every view current again: each pass wiped the last.
    Showdown sets [0x00652B24] (one clear for the whole screen at the start
    of sub_00051970); co-op sets it too. While a cutscene plays
    ([0x00652B50], the letterboxed cinema mode) only player 1's view is
    drawn, full screen; the views are arranged at the entry of
    sub_00051970 (rdr_hooks.c), which runs every frame, cutscenes too:
    the players' own updates do not run during one.
- Story co-op, option B (two games over the network, the partner drawn as
  a puppet, the recvx-enhanced-port way): needs a puppet character placed
  and posed every frame from the other game's position and animation.
  Placing is the same spawn as option A; posing needs the animation
  system (crAnimFrame / acMixer) worked out. Not started.
- PS2 disc (SLUS-20500): same level archives and tables as the Xbox one;
  its SWAT.DAT has ~1,090 extra entries, all development leftovers (older
  level layouts, unused textures of a bank, a mine, a maze, a training
  ground, passenger cars). Models are the same characters in PS2 formats
  (.mod/.tex for .xmod/.xtex); the Xbox disc already has every one.

- White lines on the front end's books (render scale 2 and up): the page
  text is drawn into a 512x256 swizzled surface (0x01B9EE80) and the book's
  page mesh samples it, clamped. That surface's outermost row is grey (the
  paper texture drawn into it wraps there), and at a higher scale the page
  mesh's edge pixels landed on it: a grey line along every page edge.
  Surfaces sampled in place at a scale above 1 now keep their samples 1.5
  guest texels inside a clamped edge (nv2a_gl.c, uTexClamp). Found with
  RECOMP_PIXEL_PROBE / RECOMP_PIXEL_PROBE_FLAG (a file holding "x y" in
  scaled pixels: the draws that brighten that column are logged).
- The keyboard and mouse follow the port the game reads: its menus take
  one controller (the menu input map [0x0067991C], +4 its port: the one
  that pressed START), a lone story player its own pad object
  ([player+0x894]); with the keyboard on player 1 and a pad on player 2
  the menus were deaf to the keyboard and mouse (rdr_keyboard_port).
- Mouse look with several local players: only the camera of the player
  whose pad object is the keyboard player's port (sub_0013EC00, the player
  in edi at the GetChannel call).
- KeBugCheck(0xA) from XAPI's thread exit (fs:[0x24] >= DISPATCH_LEVEL in
  the one shared processor block) now continues, as before the bug-check
  exit: seen skipping an intro movie.
- The pause after the Rockstar logo: SDL_InitSubSystem (HID enumeration,
  10 s with a Bluetooth DualSense) ran at the game's first pad read on the
  USB thread; it now runs on its own thread from start-up.

## Known issues

- Mouse look is calibrated on the normal camera; other CamMach cameras
  that read channels 14/15 with their own speeds (sneak, wagon) turn at
  their own rate per count.
- Frame rates above 60: play is clock-timed and the refresh-period
  steppers are fixed; anything else frame-counted that turns up needs the
  same treatment.
- The SDL2 controller path is built and syntax-checked for Windows but was
  not run on a Windows machine before delivery. Until this build it was
  never started there (nothing called xbox_InputInit on Windows), which is
  why PlayStation pads needed DS4Windows; it now starts on the pad
  thread's first read and logs what it finds.
- Story chapters still open as they are reached: Unlock Everything covers
  the Unlockables table only. A profile's progress is its mode record's
  current level (+0x23C/+0x27C); the chapter book's page limit was not
  found yet.
- Diagonal strafing with the gun up (keys at full throw on both axes while
  the left trigger's key is held) could not be checked in the test
  harness: its first level does not let the gun be drawn.
- Story co-op: not attempted. Showdown has up to four players
  ([0x00675930 + i*4], [0x00652E30] the multiplayer flag, split-screen
  levels in lvls_m2/lvls_m4), but the story levels, their scripts and
  cutscenes assume player 0 alone, and the camera, HUD and results screen
  are single-player. A second human would need a second player character
  spawned into a story level (or an AI partner switched to pad input), a
  split screen, and every script that looks for "the player" taught
  about two.

- Xbox Live itself cannot work: there is no account on the virtual drive
  and no network stack behind XNet. The Live screens open and back out
  cleanly; signing in is not possible.

- Legal screen: on the GPU path the text appeared a few characters at a
  time over ~15 s during loading on the 2-core test machine. Check on a
  real PC.

- `wbinvd` reached at `0x0033E494` (D3D) and `0x0017C210` (game, before each
  Bink frame upload). Lifted as a no-op, which is right for a host with
  coherent caches.
- Linux renders through the toolkit's OpenGL path at ~12 fps headless under
  Xvfb with software GL on 2 cores. Expect full speed on a real GPU.
- `\\.\D:\` (raw volume open) fails before each movie; the game carries on.
