# Third-party notices

| Component | Where | License | Use |
|---|---|---|---|
| [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) | fetched by the build (`deps/xboxrecomp`), patched by `patches/` | MIT, except the files its own NOTICE lists as taken from [xemu](https://github.com/xemu-project/xemu) (the MCPX APU sources and the NV2A register headers): LGPL-2.1-or-later | the recompiler and the Xbox runtime |
| [Dear ImGui](https://github.com/ocornut/imgui) | `third_party/imgui/` | MIT (`third_party/imgui/LICENSE.txt`) | the launcher and in-game menus |
| [stb_image, stb_image_write](https://github.com/nothings/stb) | `third_party/stb/` | MIT or public domain (in each header) | pictures for the menus; online streaming |
| [SDL2](https://github.com/libsdl-org/SDL) | downloaded by the build, `SDL2.dll` | zlib | controllers; the online viewer |
| [Capstone](https://github.com/capstone-engine/capstone) | installed by the build (`pip install capstone`) | BSD-3-Clause | disassembly while generating the C (not shipped) |
| Berylium (Ray Larabie) | `assets/fonts/Berylium.ttf` | Larabie Fonts freeware EULA (`assets/fonts/Berylium-LICENSE.txt`) | the outlines of the game's menu font |
| Rye | `assets/fonts/Rye-Regular.ttf` | SIL Open Font License 1.1 (`assets/fonts/OFL-Rye.txt`) | the menus' look without the game's files |
| IM Fell English | `assets/fonts/IMFeENrm28P.ttf`, `IMFeENit28P.ttf` | SIL Open Font License 1.1 (`assets/fonts/OFL-IMFell.txt`) | the menus' look without the game's files |

**The patch** (`patches/xboxrecomp-posix-x86_64-mmio.patch`) changes two of
xboxrecomp's LGPL files, `src/apu/apu_core.c` and `src/apu/apu_vp.c`. Those
changes are under the same licence, LGPL-2.1-or-later (the text is in
`LICENSES/LGPL-2.1.txt`), and each of the two files says that it was
modified. The rest of the patch, like this project's own code, is under the
MIT licence (`LICENSE`).

Red Dead Revolver's code and assets are not included and are not covered by
anything here: they belong to Rockstar Games / Take-Two Interactive.
