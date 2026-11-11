# Building

The game is built on your own PC from your own copy of the disc: the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit translates the
Xbox game's x86 code (`default.xbe`) into C, and that C is compiled with this
project's code into a native program. The generated C is made from the
game, so it is never committed or shared (`src/recomp/gen/` is ignored).

- [Windows: the one-click build](#windows-the-one-click-build)
- [Windows: by hand](#windows-by-hand)
- [Linux](#linux)
- [Regenerating the C](#regenerating-the-c)
- [The toolkit and its patch](#the-toolkit-and-its-patch)
- [Project layout](#project-layout)

## Windows: the one-click build

Everything the build makes stays inside this folder:

```
rdr-recomp\                              this project
  Red Dead Recompilation\                the game, ready to play
    Game\                                the game's files (default.xbe, swat.dat, bik\, ...)
  deps\                                  the xboxrecomp toolkit and SDL2
  build\                                 the compile
```

An extracted disc beside the project (`..\Red Dead Revolver (USA)\`) is
used where it is (`Game\` is linked to it). Earlier builds put the play
folder, the toolkit and SDL2 beside the project: the play folder is moved
in (with its saves, settings and mods), and a toolkit clone already there
is used.

Disc extracted in a folder with another name, or somewhere else? `build.bat`
finds a folder beside the project that has the game (`default.xbe` and
`swat.dat`); otherwise drop that folder onto `build.bat`, pass it as
`-Game <folder>`, or pick its `default.xbe` in the window `build.bat` opens.
Only have a disc image? Drop the `.iso` onto `build.bat`, or put it next
to this folder, or just run `build.bat`: with no extracted disc it asks for
the image (or `default.xbe`) in a window. It unpacks it into
`Red Dead Recompilation\Game\` inside the project (`tools\extract_xiso.py`: an extract-xiso image or a full
Redump dump, checked to be Red Dead Revolver (USA), title 5454001A) and
goes on.

1. Double-click **`build.bat`**. It checks for Git, Python 3 and Visual
   Studio 2022's C++ tools and offers to install anything missing (through
   winget; Visual Studio is installed slim (the C++ compiler and the Windows SDK only, about 3-4 GB) and needs an admin prompt; CMake comes from pip when Visual Studio has none). Then it clones
   the xboxrecomp toolkit into `deps\`, applies the patch, regenerates
   the C code from your `default.xbe` (about 15 minutes) and compiles it
   (10-30 minutes). It also downloads SDL2 (a few MB, from the SDL
   project's GitHub releases) for controller support. Running it again
   only redoes what changed.
2. Play from the **Red Dead Recompilation** folder it makes inside the
   project: start **Red Dead Revolver.exe**. (`play.bat` in the project
   starts the same build with the project's own `settings.ini`, `saves\`,
   `mods\` and `logs\`.)

**Which compiler:** the first build asks: Visual Studio (the default) or
Clang. The answer is kept in `deps\compiler.txt`; `build.bat -Compiler
msvc` or `-Compiler clang` changes it. The Clang build uses
[LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw) (Clang, LLD and the
MinGW-w64 headers and libraries; the 20250826 UCRT x86_64 zip, about 175 MB,
unpacked into `deps\`), with CMake and Ninja from pip (or their own zips from GitHub when pip's cannot be found) and SDL2's MinGW
package (`deps\mingw\`). It builds into `build-clang\` and copies the
compiler's C++ runtime (`libc++.dll`, `libunwind.dll`) beside the exe. No
installer and no admin prompt. It is newer here than the Visual Studio
build: the same code; first built and played on Windows with 1.0.9.

**What it downloads is checked.** Every zip (LLVM-MinGW, CMake, Ninja,
both SDL2 packages) has its SHA-256 written in `tools/build.ps1`; a
download that differs is deleted and the build stops. The installers it
may run (Git, Python, Visual Studio Build Tools) must be signed by their
publisher. The Python packages have fixed versions (capstone 5.0.9, the
version the generated code is made with; CMake 3.31.6; Ninja 1.13.0), and
the toolkit is checked out at one commit. winget checks its own downloads.
`build.bat` also checks that `default.xbe` is the USA version 1 the port
was made for (`-AnyGame` skips that).

**Faster generated code:** the LLVM-MinGW build compiles the generated
code fully optimised (`-O2`); `build.bat -FastCode` does the same with
Visual Studio (`/O2`, a longer compile). Measured in the harness, standing
in chapter 1: 4-8 % less CPU per frame on the guest's busiest threads, 8 %
more frames. Builds with and without it play online together (a host at
`-O1` and a guest at `-O2`: 3,197 frames, every one the same). CMake:
`-DRDR_GEN_O2=ON`.

Options: `build.bat -Clean` rebuilds from scratch; `build.bat -Iso "D:\game.iso"`
unpacks that image; `build.bat -Game "D:\other\path"`
and `play.bat "D:\other\path"` point at a disc folder elsewhere.

The play folder is assembled by `tools/package.ps1` after each build; see
the [user guide](USER_GUIDE.md#the-play-folder).

## Windows: by hand

Prerequisites: Visual Studio 2022 (Desktop C/C++), Python 3.10+ with
`pip install capstone==5.0.9`, Git. From "Developer PowerShell for VS 2022":

```powershell
git clone -c core.autocrlf=false https://github.com/sp00nznet/xboxrecomp.git ..\xboxrecomp
git -C ..\xboxrecomp checkout f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b   # v0.16.0
git -C ..\xboxrecomp apply ..\rdr-recomp\patches\xboxrecomp-posix-x86_64-mmio.patch
powershell -ExecutionPolicy Bypass -File tools\regen.ps1 -XboxRecomp ..\xboxrecomp -Game "..\Red Dead Revolver (USA)"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DXBOXRECOMP_DIR=..\xboxrecomp
cmake --build build --config Release --parallel
build\Release\rdr_recomp.exe "..\Red Dead Revolver (USA)"
```

The patch is needed on Windows too: besides the Linux register traps (inert
on Windows) it makes new guest threads wait behind their creator, which
Bink's movie reader depends on.

## Linux

```bash
git clone https://github.com/sp00nznet/xboxrecomp.git ../xboxrecomp
git -C ../xboxrecomp checkout f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b
git -C ../xboxrecomp apply "$PWD/patches/xboxrecomp-posix-x86_64-mmio.patch"
bash ../xboxrecomp/tools/linux/install_deps.sh
tools/regen.sh ../xboxrecomp "../Red Dead Revolver (USA)"
cmake -S . -B build -G Ninja -DXBOXRECOMP_DIR=../xboxrecomp
cmake --build build
./build/rdr_recomp "../Red Dead Revolver (USA)"
```

The Linux renderer is the toolkit's OpenGL path, which is less complete than
D3D11.

## Regenerating the C

`tools/regen.ps1` (Windows) and `tools/regen.sh` (Linux) run the toolkit's
pipeline over `default.xbe` (the names in `names/*.json` applied first
(`tools/apply_names.py`)) and write `src/recomp/gen/`. Functions this
project replaces by hand (in `src/rdr_hooks.c`) are left out of the
generated code; when that list changes the C has to be generated again,
which `build.bat` notices and does by itself (about 15 minutes).

## The toolkit and its patch

The build uses xboxrecomp at a fixed commit
(`f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b`, v0.16.0) with
`patches/xboxrecomp-posix-x86_64-mmio.patch` applied. The patch carries
every change this port needs in the toolkit: trapped audio and USB
registers on x86-64 Linux/macOS hosts, new guest threads waiting behind
their creator (Bink's movie reader depends on it), the GPU renderer's
additions, the present and audio taps for online play, a title-supplied
kernel clock for lockstep, and more. [dev/xboxrecomp-PR.md](dev/xboxrecomp-PR.md)
describes the parts meant to go upstream.

## Project layout

| Path | What |
|---|---|
| `src/main.c` | host entry point (Windows + Linux/macOS) |
| `src/recomp_manual.c` | indirect-call hooks, from the toolkit template |
| `src/rdr_overrides.c` | Red Dead Revolver-specific patches, keyed by guest VA |
| `names/string_names.json` | 101 function names recovered from the game's own log strings |
| `build.bat`, `play.bat`, `play_debug.bat` | one-click build and launch on Windows |
| `make_preview.bat`, `play_preview.bat` | the 2002 preview's characters and stages in Showdown, and 59 of the story's own people (Brimstone's townsfolk and shopkeepers, Wilbur from the train, soldiers, braves) on the line-up's later pages (`mods_preview`, played on a copy of your saves; Bat Boy flies with Black while his gun is drawn, or the melee button while it is put away) |
| `tools/build.ps1` | what `build.bat` runs: prerequisites, toolkit, regen, compile |
| `tools/regen.sh`, `tools/regen.ps1` | run the xboxrecomp pipeline and regenerate `src/recomp/gen/` |
| `tools/apply_names.py` | applies `names/*.json` to the disassembly before lifting |
| `src/ui/` | the launcher, the in-game menu, cheats, mouse and keyboard; `rdr_gameart.c` reads the game's fonts and pictures from your disc for them |
| `assets/fonts/` | Berylium (Ray Larabie; the outlines of the game's menu font, licence beside it), Rye and IM Fell (SIL OFL; the menus' look without the game's files) |
| `tools/mods/` | `dave.py` (game archives), `xtex.py` (textures), `addlevel.py` (new Showdown stages), `beta_level.py` / `beta_mine.py` (the 2002 preview's levels as stages), `addchar.py` / `beta_char.py` (new Showdown characters, experimental), `beta_roster.py` / `beta_showdown.py` (the whole preview in Showdown), `story_npcs.py` (the story's people in Showdown); see [MODDING.md](MODDING.md) |
| `docs/` | the documents listed in the README; `docs/dev/` the development log (BRINGUP.md) and design notes |
| `patches/xboxrecomp-posix-x86_64-mmio.patch` | toolkit fix: trapped audio/USB registers on x86-64 Linux/macOS hosts |
| `src/rdr_hooks.c` | functions replaced by hand (wrappers around the generated ones) |
| `src/rdr_lockstep.c`, `src/net/` | online play: lockstep and host streaming ([ONLINE.md](ONLINE.md)) |
| `third_party/` | Dear ImGui, stb_image / stb_image_write |
| `tools/lockstep_compare.py` | compares two lockstep runs' fingerprints frame by frame |
