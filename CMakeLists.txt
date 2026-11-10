cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Red Dead Revolver (Xbox) - static recompilation
#
#   cmake -S . -B build -DXBOXRECOMP_DIR=<path to xboxrecomp clone>
#   cmake --build build --config Release
#
# The generated C (src/recomp/gen/) is produced from YOUR copy of default.xbe
# by tools/regen.sh / tools/regen.ps1. Never commit or publish it.
# ============================================================================

project(rdr_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")
# Windows: SDL2 for controllers that are not XInput devices (build.ps1
# fetches the official package and passes SDL2_DIR). Found here, before the
# toolkit, so its target is visible to both.
if(WIN32)
    find_package(SDL2 CONFIG QUIET)
endif()

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR "src/recomp/gen/ is empty - run tools/regen.sh (or regen.ps1) first")
endif()

# The launcher and the in-game menu: Dear ImGui (third_party/imgui, MIT)
# with two SIL OFL fonts embedded at configure time.
include(cmake/embed.cmake)
set(RDR_EMBED_DIR ${CMAKE_BINARY_DIR}/embedded)
embed_file(${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Rye-Regular.ttf ${RDR_EMBED_DIR}/font_rye.c rdr_font_rye)
embed_file(${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/IMFeENrm28P.ttf ${RDR_EMBED_DIR}/font_fell.c rdr_font_fell)
embed_file(${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/IMFeENit28P.ttf ${RDR_EMBED_DIR}/font_fell_italic.c rdr_font_fell_italic)
# Berylium (Ray Larabie, free: assets/fonts/Berylium-LICENSE.txt): the
# outlines of the game's menu font, for sharp letters (rdr_gameart.c)
embed_file(${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Berylium.ttf ${RDR_EMBED_DIR}/font_berylium.c rdr_font_berylium)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    assets/fonts/Rye-Regular.ttf assets/fonts/IMFeENrm28P.ttf assets/fonts/IMFeENit28P.ttf
    assets/fonts/Berylium.ttf)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(UI_SOURCES
    src/ui/rdr_settings.c
    src/ui/rdr_cheats.c
    src/ui/rdr_modchars.c
    src/ui/rdr_preview_setup.c
    src/ui/rdr_mods.c
    src/ui/rdr_freecam.c
    src/ui/rdr_input.c
    src/ui/rdr_gl.c
    src/ui/rdr_gameart.c
    src/ui/rdr_sharptext.c
    src/ui/rdr_ui_game.cpp
    src/ui/rdr_hud2002.cpp
    src/ui/rdr_ui_theme.cpp
    src/ui/rdr_ui_pages.cpp
    src/ui/rdr_overlay.cpp
    src/ui/rdr_launcher.cpp
    src/ui/rdr_update.c
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${RDR_EMBED_DIR}/font_rye.c
    ${RDR_EMBED_DIR}/font_fell.c
    ${RDR_EMBED_DIR}/font_fell_italic.c
    ${RDR_EMBED_DIR}/font_berylium.c
)

set(GAME_SOURCES
    src/main.c
    src/iso_install.c
    src/recomp_manual.c
    src/rdr_overrides.c
    src/rdr_hooks.c
    src/rdr_lockstep.c
    src/rdr_npc.c
    src/rdr_backup.c
    src/net/rdr_net.c
    src/net/rdr_net_host.c
    src/net/rdr_lobby.c
    src/net/rdr_viewer.c
    ${UI_SOURCES}
)

if(WIN32)
    # (with its icon: assets/icon, made by tools/make_icon.py)
    add_executable(${PROJECT_NAME} WIN32 ${GAME_SOURCES} ${RECOMP_GEN_SOURCES}
                   ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/rdr_recomp.rc)
    # the application manifest (assets/icon/rdr.manifest): Visual Studio
    # merges a .manifest source into its own; MinGW takes it from the .rc
    if(MSVC)
        target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/rdr.manifest)
    else()
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/rdr_recomp.rc
                                    PROPERTIES COMPILE_DEFINITIONS RDR_RC_MANIFEST=1)
    endif()
else()
    add_executable(${PROJECT_NAME} ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)
# The version (VERSION, raised with every release): the logs, the title
# screen, the launcher and its update check.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RDR_VERSION LIMIT_COUNT 1)
string(STRIP "${RDR_VERSION}" RDR_VERSION)
target_compile_definitions(${PROJECT_NAME} PRIVATE RDR_VERSION="${RDR_VERSION}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src
    ${IMGUI_DIR}
)
# Dear ImGui and the UI at full optimisation, whatever the generated code gets.
set_source_files_properties(${UI_SOURCES} PROPERTIES COMPILE_DEFINITIONS "IMGUI_DISABLE_OBSOLETE_FUNCTIONS")

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11 dxgi dxguid
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>
        winmm dbghelp opengl32 shell32 ws2_32 comdlg32 winhttp)
endif()

if(WIN32 AND TARGET SDL2::SDL2)
    # The launcher reads PlayStation and other pads through SDL too.
    target_compile_definitions(${PROJECT_NAME} PRIVATE RDR_HAVE_SDL2)
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMENT "SDL2.dll beside the game")
endif()

# The generated code: -O1 (/O1) by default, the build-time / speed sweet
# spot; RDR_GEN_O2=ON optimises it fully (-O2, /O2): the guest's code runs
# faster and takes longer to compile. Either keeps the same arithmetic
# (no fast-math, no contraction into FMA on x86-64's baseline), so builds
# of both kinds play online together (checked: docs/dev/BRINGUP.md 217).
option(RDR_GEN_O2 "Optimise the generated code fully (-O2 / /O2)" OFF)
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /MP)   # /MP: files in parallel
    if(RDR_GEN_O2)
        set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/O2")
    else()
        set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/O1")
    endif()
else()
    # The generated code is millions of lines; the -fno-* flags keep GCC from
    # spending minutes per file on passes that buy nothing on lifted code
    # (Clang has neither).
    if(RDR_GEN_O2)
        set(RDR_GEN_OPTS -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
    else()
        set(RDR_GEN_OPTS -O1 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        list(APPEND RDR_GEN_OPTS -fno-tree-pre -fno-gcse)
    endif()
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "${RDR_GEN_OPTS}")
    if(MINGW)
        # LLVM-MinGW (build.bat -Compiler clang): the exe where Visual
        # Studio's goes (build\Release, which package.ps1 copies from), with
        # the compiler's C++ runtime DLLs beside it.
        set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Release)
        get_filename_component(RDR_MINGW_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
        foreach(dll libc++.dll libunwind.dll libwinpthread-1.dll)
            set(src ${RDR_MINGW_BIN}/../x86_64-w64-mingw32/bin/${dll})
            if(EXISTS ${src})
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${src} $<TARGET_FILE_DIR:${PROJECT_NAME}>)
            endif()
        endforeach()
    endif()
    if(NOT WIN32)
        target_link_libraries(${PROJECT_NAME} PRIVATE m ${CMAKE_DL_LIBS})
        # The launcher's window and GL context come from SDL2, as the toolkit's do.
        find_package(SDL2 REQUIRED)
        target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2)
        # backtrace_symbols names generated functions in crash reports
        target_link_options(${PROJECT_NAME} PRIVATE -rdynamic)
    endif()
endif()
