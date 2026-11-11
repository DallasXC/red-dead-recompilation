/*
 * rdr_launcher.cpp - the settings window before the game starts.
 *
 * Opens on the first start, when Shift is held while the game starts, with
 * --launcher, or every time if the player asks for that. A plain window of
 * its own (Win32 + WGL on Windows, SDL elsewhere) drawing the same parchment
 * pages as the in-game menu; it closes before the game opens its window.
 *
 * Keyboard and controller work as well as the mouse: arrows or the d-pad
 * move, Enter or A changes a row, left/right steps a value, Tab or LB/RB
 * changes page, Start or Enter on "Play" starts the game.
 *
 * Tests: RDR_LAUNCHER_SHOT=<file.bmp> draws a few frames and saves one;
 * RDR_LAUNCHER_PAGE=<0-5> picks the page; RDR_LAUNCHER_SETUP=<image> sets
 * the preview characters up from that disc image first, RDR_LAUNCHER_INSTALL
 * =<image> installs the game from it when it is missing (the shot waits),
 * RDR_LAUNCHER_XBE=<default.xbe> uses that extracted copy instead.
 */
#include "rdr_ui.h"
#include "rdr_ui_common.h"
#include "rdr_ui_game.h"
#include "rdr_gl.h"
#include "rdr_input.h"
#include "rdr_update.h"
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <xinput.h>
#ifdef RDR_HAVE_SDL2
#define SDL_MAIN_HANDLED
#include <SDL.h>
#endif
#else
#include <SDL.h>
#endif

using namespace rdrui;

namespace {

int s_page;
int s_result = -1;               /* -1 running, 0 quit, 1 play */
bool s_sizing;                  /* (Windows) an edge or corner of the window is being dragged */
void (*s_paint)(void);          /* (Windows) draws one frame, from the resize loop */
bool s_first;                    /* the first start: the setup page first */
bool s_dirty;

const char *kPages[] = { "Display", "Graphics", "Controls", "Mods", "Extras", "About" };
const int kNumPages = 6;

/* Play: the game starts -- not while the preview characters are being
 * set up (the page says so). */
void play()
{
    if (rdr_game_missing() || rdr_preview_setup_state(NULL, NULL, 0) == 1) {
        if (!rdr_game_missing()) s_page = 4;   /* Extras: the setup */
        return;                         /* (the page says why) */
    }
    s_result = 1;
}

void open_folder(const char *path)
{
#if defined(_WIN32)
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '\\');
    if (!slash) slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    ShellExecuteA(NULL, "open", dir, NULL, NULL, SW_SHOWNORMAL);
#else
    (void)path;
#endif
}

/* The play folder with no game yet: install it from the Xbox disc image,
 * here, before anything else (main.c opens the launcher for it). */
static char s_inst_msg[200];

bool page_install()
{
    static char image[1024];
    char text[200] = "", v[32];
    int pct = 0;
    int st = rdr_game_install_state(&pct, text, sizeof text);
    Section("Install the game");
    if (st == 1) {
        snprintf(v, sizeof v, "%d%%", pct);
        Action("Installing", v, "Unpacking the disc image into the Game folder. This happens once.");
        Note("Unpacking the game's files into the Game folder (about 2 GB, a few minutes).");
        return false;
    }
    Note("Red Dead Revolver is not installed in this folder yet. Choose your Red Dead Revolver (USA) "
         "Xbox disc image (.iso): its files are unpacked into the Game folder, once (about 2 GB, a "
         "few minutes). Or, if the disc is already unpacked somewhere, choose its default.xbe.");
    if (Action("Install the game", "Choose the disc image", "Pick your Red Dead Revolver (USA) Xbox disc image (.iso)."))
        if (rdr_game_install_pick(image, sizeof image))
            rdr_game_install_start(image);
    if (Action("Already unpacked?", "Choose its default.xbe",
               "The game's files are already in a folder (default.xbe, swat.dat, bik...): pick its "
               "default.xbe. That folder is used as it is; nothing is copied.")) {
        static char xbe[1024];
        if (rdr_game_pick_xbe(xbe, sizeof xbe)) {
            int r = rdr_game_use_folder(xbe);
            snprintf(s_inst_msg, sizeof s_inst_msg, "%s",
                     r == -2 ? "That default.xbe is another game. Choose Red Dead Revolver (USA)'s."
                     : r == 0 ? "That folder does not have the whole game (default.xbe with swat.dat beside it)."
                     : "");
        }
    }
    if (s_inst_msg[0])
        Note(s_inst_msg);
    else if (st == 3)
        Note(text);
    return false;
}

/* After the install: the game's own art for the launcher, from its files. */
extern "C" const char *rdr_ui_game_dir(void);
void install_done_check()
{
    static bool retried;
    if (!retried && !rdr_game_missing() && rdr_game_install_state(NULL, NULL, 0) == 2) {
        retried = true;
        rdrui::game::Retry();
    }
    /* The parchment look until the game's files are there: look again
     * every two seconds (a laptop showed the parchment launcher with the
     * game installed -- the look had failed once and was never retried). */
    static double last;
    double now = ImGui::GetTime();
    static int tries;
    if (!rdrui::game::Ready() && tries < 5 && now - last > 2.0 && rdr_game_install_state(NULL, NULL, 0) != 1) {
        last = now;
        char p[1100];
        snprintf(p, sizeof p, "%s/swat.dat", rdr_ui_game_dir());
        FILE *f = fopen(p, "rb");
        if (f) {
            fclose(f);
            tries++;
            rdrui::game::Retry();
        }
    }
}

/* The first start: the few things worth choosing before playing, on one
 * page -- the screen, the black bars, the controls -- then the usual pages
 * (everything here is on them too). */
bool page_welcome(RdrSettings *s)
{
    bool ch = false;
    static const char *fs_items[] = { "Window", "Borderless window", "Fullscreen" };
    static const char *bars_items[] = { "Off", "In cutscenes only", "Always (as the game)" };
    static const char *play_items[] = { "A controller", "Keyboard and mouse" };
    static const char *res_items[] = { "Original (480 lines)", "2x", "3x (1440p screens)", "4x (4K screens)" };
    Note("Welcome. A few choices before the first game -- all of them can be changed later, "
         "here or in the game's PC Options.");
    int fs = s->fullscreen ? 2 : s->borderless ? 1 : 0;
    if (Choice("Display mode", &fs, fs_items, 3, "Fullscreen is borderless, on the game's monitor; "
               "F11 or Alt+Enter switches while playing.")) {
        s->fullscreen = fs == 2;
        s->borderless = fs == 1;
        ch = true;
    }
    int rs = s->render_scale - 1;
    if (rs < 0) rs = 0;
    if (rs > 3) rs = 3;
    if (Choice("Sharpness", &rs, res_items, 4, "How finely the 3D world is drawn: higher is sharper and "
               "needs a stronger graphics card (Graphics page: up to 6x).")) {
        s->render_scale = rs + 1;
        ch = true;
    }
    ch |= Choice("Black bars", &s->letterbox, bars_items, 3,
                 "The black bands the game draws at the top and bottom of the picture for its old-film "
                 "look. Off shows the whole widescreen picture.");
    int kb = s->mouse_aim ? 1 : 0;
    if (Choice("I play with", &kb, play_items, 2,
               "A controller: Xbox, PlayStation, Switch and most others. Keyboard and mouse: the mouse "
               "aims; the keys are on the Controls page.")) {
        s->mouse_aim = kb;
        if (kb)
            s->kbm_player = 1;
        ch = true;
    }
    if (Action("Done", "The other pages", "Go on to every setting (Play starts the game at any time)."))
        s_first = false;
    return ch;
}

/* The version, and what the update check found. */
static void about_version(RdrSettings *s, bool *ch)
{
    const char *tag = "", *url = "";
    int st = rdr_update_state(&tag, &url);
    char v[96];
    if (st == RDR_UPDATE_NEWER)
        snprintf(v, sizeof v, "%s (%s is out)", rdr_version(), tag);
    else if (st == RDR_UPDATE_CURRENT)
        snprintf(v, sizeof v, "%s (the latest)", rdr_version());
    else if (st == RDR_UPDATE_CHECKING)
        snprintf(v, sizeof v, "%s (asking...)", rdr_version());
    else
        snprintf(v, sizeof v, "%s", rdr_version());
    if (Action("Version", v, st == RDR_UPDATE_NEWER ? "A newer release is out: this opens its page to download it."
                                                     : "This build of Red Dead Recompilation.")
        && st == RDR_UPDATE_NEWER)
        rdr_update_open_page();
    if (Toggle("Check for updates", &s->update_check,
               "When this window opens, ask GitHub whether a newer release is out. Nothing is "
               "downloaded or installed by itself."))
    {
        *ch = true;
        if (s->update_check)
            rdr_update_start();
    }
}

bool page_about(RdrSettings *s)
{
    bool ch = false;
    about_version(s, &ch);
    ch |= Toggle("Show this window at every start", &s->show_launcher,
                 "Otherwise it opens only when Shift is held as the game starts.");
    if (Action("Settings file", "Open folder", rdr_settings_path()))
        open_folder(rdr_settings_path());
    if (Action("Restore the defaults", "", "Every setting back to how it started.")) {
        int keep = s->show_launcher;
        rdr_settings_defaults(s);
        s->show_launcher = keep;
        ch = true;
    }
    if (game::Ready()) {
        /* in the game's art: the keys as rows, the credits as a note */
        Section("While playing");
        Action("F1, or both sticks", "Cheats and settings", "The menu, over the game.");
        Action("F2", "Frame counter", "Frames a second, top left.");
        Action("F11, or Alt+Enter", "Fullscreen", "Window or full screen.");
        ImGui::Dummy(ImVec2(1, 6 * game::g_space.s));
        Note("The menus are drawn with the game's own fonts and pictures, read from your copy. "
             "Interface: Dear ImGui (MIT licence).");
        return ch;
    }
    ImGui::Dummy(ImVec2(1, 10 * g_scale));
    ImGui::PushFont(g_fonts.heading);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kRed), "While playing");
    ImGui::PopFont();
    ImGui::PushFont(g_fonts.small);
    {
        static const char *keys[][2] = {
            { "F1, or both sticks", "the menu: cheats and settings" },
            { "F2", "frame counter" },
            { "F11, or Alt+Enter", "fullscreen" },
        };
        float x0 = ImGui::GetCursorPosX();
        for (auto &k : keys) {
            ImGui::SetCursorPosX(x0);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kRed), "%s", k[0]);
            ImGui::SameLine(x0 + 230 * g_scale);
            ImGui::TextUnformatted(k[1]);
        }
    }
    ImGui::Dummy(ImVec2(1, 8 * g_scale));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 16 * g_scale);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kInkSoft),
        "Fonts: Rye and IM Fell English (SIL Open Font License). Interface: Dear ImGui (MIT licence).");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    return ch;
}

/* One frame of the launcher into a w x h window.
 *
 * Laid out after the game's 2004 website: the title and a menu in cream
 * woodtype down the left of dark planks, between glowing scrollwork; the
 * page on a sheet with a ragged black edge to the right. */
void frame(int w, int h)
{
    const float s = g_scale;
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    DrawDarkWood(bg, ImVec2(0, 0), ImVec2((float)w, (float)h));

    /* Everything is placed on a 1280 x 820 board, centred. */
    float ox = floorf((w - 1280 * s) * 0.5f), oy = floorf((h - 820 * s) * 0.5f);
    auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* The title, in two lines. */
    const float cx = 220;
    {
        ImFont *f = g_fonts.title;
        float s1 = 66 * s, s2 = 52 * s;
        ImVec2 t1 = f->CalcTextSizeA(s1, FLT_MAX, 0, "RED DEAD");
        ImVec2 t2 = f->CalcTextSizeA(s2, FLT_MAX, 0, "REVOLVER");
        DrawOutlined(dl, f, s1, ImVec2(P(cx, 0).x - t1.x * 0.5f, P(0, 92).y), kCream, "RED DEAD", 2.0f);
        DrawOutlined(dl, f, s2, ImVec2(P(cx, 0).x - t2.x * 0.5f, P(0, 156).y), kCream, "REVOLVER", 2.0f);
    }
    DrawGlowFlourish(dl, P(cx, 246), 200 * s);

    /* The pages, then playing and quitting. */
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1))
        s_page = (s_page + kNumPages - 1) % kNumPages, s_first = false;
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1))
        s_page = (s_page + 1) % kNumPages, s_first = false;
    static const char *upper[] = { "DISPLAY", "GRAPHICS", "CONTROLS", "MODS", "EXTRAS", "ABOUT" };
    for (int i = 0; i < kNumPages; i++) {
        ImGui::SetCursorScreenPos(P(cx, 262 + i * 34));
        if (SideItem(upper[i], s_page == i && !s_first, 34 * s, P(cx, 0).x)) {
            s_page = i;
            s_first = false;
        }
    }
    ImGui::SetCursorScreenPos(P(cx, 476));
    if (SideItem("PLAY", false, 42 * s, P(cx, 0).x) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        play();
    ImGui::SetCursorScreenPos(P(cx, 532));
    if (SideItem("QUIT", false, 30 * s, P(cx, 0).x) || ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false)
        || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        s_result = 0;
    DrawGlowFlourish(dl, P(cx, 592), 200 * s);
    {
        const char *tag = "";
        if (rdr_update_state(&tag, nullptr) == RDR_UPDATE_NEWER) {
            char t[96];
            snprintf(t, sizeof t, "%s is out -- About", tag);
            ImVec2 ts = g_fonts.small->CalcTextSizeA(g_fonts.small->FontSize, FLT_MAX, 0, t);
            ImVec2 at(P(cx, 0).x - ts.x * 0.5f, P(0, 650).y);
            dl->AddText(g_fonts.small, g_fonts.small->FontSize, at, IM_COL32(240, 200, 90, 255), t);
            if (ImGui::IsMouseHoveringRect(at, ImVec2(at.x + ts.x, at.y + ts.y)) && ImGui::IsMouseClicked(0))
                s_page = 5;
        }
    }
    {
        const char *help = "Start: play    Back: quit    LB/RB: pages";
        ImVec2 ts = g_fonts.small->CalcTextSizeA(g_fonts.small->FontSize * 0.8f, FLT_MAX, 0, help);
        dl->AddText(g_fonts.small, g_fonts.small->FontSize * 0.8f, ImVec2(P(cx, 0).x - ts.x * 0.5f, P(0, 618).y),
                    IM_COL32(200, 190, 140, 170), help);
    }

    /* The page, on its sheet. */
    ImVec2 a = P(410, 74), b = P(1232, 748);
    DrawTornPanel(bg, a, b, 5);
    DrawOutlined(dl, g_fonts.title, 50 * s, ImVec2(a.x + 58 * s, a.y + 46 * s), IM_COL32(250, 246, 232, 255),
                 rdr_game_missing() ? "INSTALL" : s_first ? "WELCOME" : upper[s_page], 2.0f);
    float rx = a.x + 70 * s, rw = (b.x - a.x) - 140 * s;
    dl->AddLine(ImVec2(rx - 10 * s, a.y + 116 * s), ImVec2(b.x - 60 * s, a.y + 116 * s), IM_COL32(52, 34, 22, 160),
                1.5f * s);
    ImGui::SetCursorScreenPos(ImVec2(rx, a.y + 134 * s));
    ImGui::BeginChild("##rows", ImVec2(rw + 14 * s, (b.y - a.y) - 134 * s - 92 * s), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);
    BeginRows(rw);
    bool ch = false;
    if (rdr_game_missing())
        page_install();
    else if (s_first)
        ch = page_welcome(&g_rdr_settings);
    else switch (s_page) {
    case 0: ch = PageDisplay(&g_rdr_settings, false); break;
    case 1: ch = PageGraphics(&g_rdr_settings, false); break;
    case 2: ch = PageControls(&g_rdr_settings, false); break;
    case 3: ch = PageMods(&g_rdr_settings, false); break;
    case 4: ch = PageExtras(&g_rdr_settings, false); break;
    case 5: ch = page_about(&g_rdr_settings); break;
    }
    if (ch) s_dirty = true;
    const char *hint = CurrentHint();
    ImGui::EndChild();
    if (hint)
        dl->AddText(g_fonts.italic, g_fonts.italic->FontSize, ImVec2(rx, b.y - 82 * s), kInkSoft, hint, nullptr, rw);
    ImGui::End();
}

/* The launcher in the game's art (design C1): the options page's black
 * background and scroll rules; RED <> DEAD in the logo's own typeface over a
 * red band with "Recompilation" cut out of it, as the game's legal screen
 * sets its title; the chapter book's portrait of Red as an old photograph;
 * the pages as the options list across the top, their rows as the game's;
 * Play and Quit under the portrait. */
void frame_game(int w, int h)
{
    namespace G = rdrui::game;
    using G::Space; using G::Width; using G::Text; using G::OptionsBg; using G::ScrollRule;
    using G::Image; using G::Prompt; using G::TextWrapped; using G::g_space;
    using G::kLogo; using G::kTitle; using G::kText; using G::kWhite; using G::kGrey; using G::kHint;
    /* The board is 1280 x 820, scaled to fit the window (it can be dragged
     * to any size), and grows into the room left over. Taller: the title
     * and the pages' names stay at the top, the prompts and the bottom
     * rule go to the bottom, the rows take the room between (more of them
     * show), the portrait and Play / Quit stay in the middle of it. Wider:
     * the rows get up to 400 more (longer names and values fit), the rules
     * reach across; past that the board is centred on the page's black. */
    Space sp;
    sp.s = w / 1280.0f < h / 820.0f ? w / 1280.0f : h / 820.0f;
    float ey = h / sp.s - 820.0f, ex = w / sp.s - 1280.0f;   /* the room left over, in the board's units */
    if (ey < 1.0f) ey = 0;
    if (ex < 1.0f) ex = 0;
    if (ex > 400.0f) ex = 400.0f;
    float ep = ey * 0.5f, cx = 640 + ex * 0.5f;   /* the portrait's share; the middle */
    sp.ox = (w - (1280 + ex) * sp.s) * 0.5f;
    sp.oy = ey > 0 ? 0 : (h - 820 * sp.s) * 0.5f;
    g_space = sp;
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(0, 0, 0, 255));
    {
        /* the options page's picture, its scroll rules and all: its top
         * half at the top and its bottom half at the bottom; across, its
         * left end, its middle (the ornament) and its right end, the rules
         * between drawn on by stretching a thin strip of them */
        const float xs[4] = { 0, 430, 850, 1280 };      /* the picture's: ends, middle */
        const float gap = ex * 0.5f;
        for (int half = 0; half < 2; half++) {
            float y0 = half ? 410 + ey : 0, y1 = half ? 820 + ey : 410, v0 = half ? 0.5f : 0.0f, v1 = v0 + 0.5f;
            for (int k = 0; k < 3; k++) {
                float off = k * gap;
                G::OptionsBgPart(bg, sp.P(xs[k] + off, y0), sp.P(xs[k + 1] + off, y1),
                                 ImVec2(xs[k] / 1280, v0), ImVec2(xs[k + 1] / 1280, v1));
                if (k < 2 && gap > 0) {
                    float u = (xs[k + 1] - 6) / 1280;
                    G::OptionsBgPart(bg, sp.P(xs[k + 1] + off, y0), sp.P(xs[k + 1] + off + gap, y1),
                                     ImVec2(u, v0), ImVec2(u + 2.0f / 1280, v1));
                }
            }
        }
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* the title */
    {
        const ImU32 red = IM_COL32(128, 28, 22, 255);
        float sx = 1.5f, sy = 1.35f, base = sp.Y(178);
        float wr = Width(kLogo, "RED", sx, &sp), wd = Width(kLogo, "DEAD", sx, &sp), gap = 64 * sp.s;
        float x = sp.X(cx) - (wr + gap + wd) * 0.5f, full = wr + gap + wd;
        Text(dl, kLogo, x, base, "RED", sx, sy, red, 0, false, &sp);
        Text(dl, kLogo, x + wr + gap, base, "DEAD", sx, sy, red, 0, false, &sp);
        float dx = x + wr + gap * 0.5f, dy = base - 24 * sy * sp.s, r = 12 * sy * sp.s;
        dl->AddQuadFilled(ImVec2(dx, dy - r), ImVec2(dx + r, dy), ImVec2(dx, dy + r), ImVec2(dx - r, dy), red);
        /* the band, with the word in the page's black, as tall as it allows */
        float by = base + 8 * sp.s, bh = 50 * sp.s;
        dl->AddRectFilled(ImVec2(x, by), ImVec2(x + full, by + bh), red);
        float s2 = 1.0f;
        while (s2 > 0.2f && Width(kLogo, "RECOMPILATION", s2, &sp) > full * 0.94f) s2 -= 0.01f;
        /* the logo's letters reach 7 texels below their baseline */
        Text(dl, kLogo, sp.X(cx), by + bh - 12 * sp.s, "RECOMPILATION", s2, 0.8f, IM_COL32(14, 12, 11, 255), 1,
             false, &sp);
    }

    /* the portrait: the book's cover, printed on a sepia plate */
    {
        int iw = 0, ih = 0;
        ImTextureID t = Image("book_red", &iw, &ih);
        ImVec2 a = sp.P(78, 262 + ep), b = sp.P(378, 562 + ep);
        dl->AddRectFilled(a, b, IM_COL32(176, 160, 132, 255));
        if (t)
            dl->AddImage(t, a, b, ImVec2(0, 1), ImVec2(1, 0), IM_COL32(34, 24, 18, 230));   /* stored upside down */
        dl->AddRect(a, b, IM_COL32(110, 98, 80, 255), 0, 0, 2 * sp.s);
    }

    /* the pages across the top: the options list's style */
    static const char *tabs[] = { "Display", "Graphics", "Controls", "Mods", "Extras", "About" };
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_PageUp))
        s_page = (s_page + kNumPages - 1) % kNumPages, s_first = false;
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_PageDown))
        s_page = (s_page + 1) % kNumPages, s_first = false;
    {
        float x = sp.X(450);
        for (int i = 0; i < kNumPages; i++) {
            float tw = Width(kTitle, tabs[i], 1.15f, &sp);
            ImVec2 a(x - 6 * sp.s, sp.Y(262)), b(x + tw + 6 * sp.s, sp.Y(300));
            bool hot = ImGui::IsMouseHoveringRect(a, b);
            if (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { s_page = i; s_first = false; }
            Text(dl, kTitle, x, sp.Y(292), tabs[i], 1.15f, 1.3f,
                 i == s_page && !s_first ? kWhite : hot ? IM_COL32(190, 190, 190, 255) : kGrey, 0, true, &sp);
            if (i == s_page && !s_first)
                dl->AddLine(ImVec2(x, sp.Y(302)), ImVec2(x + tw, sp.Y(302)), G::kRed, 2 * sp.s);
            x += tw + 28 * sp.s;
        }
    }

    /* the rows */
    float rx = sp.X(450), rw = (752 + ex) * sp.s, top = sp.Y(318), bottom = sp.Y(612 + ey);
    ImGui::SetCursorScreenPos(ImVec2(rx, top));
    ImGui::BeginChild("##rows", ImVec2(rw + 8 * sp.s, bottom - top), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    BeginRows(rw);
    bool ch = false;
    if (rdr_game_missing())
        page_install();
    else if (s_first)
        ch = page_welcome(&g_rdr_settings);
    else switch (s_page) {
    case 0: ch = PageDisplay(&g_rdr_settings, false); break;
    case 1: ch = PageGraphics(&g_rdr_settings, false); break;
    case 2: ch = PageControls(&g_rdr_settings, false); break;
    case 3: ch = PageMods(&g_rdr_settings, false); break;
    case 4: ch = PageExtras(&g_rdr_settings, false); break;
    case 5: ch = page_about(&g_rdr_settings); break;
    }
    if (ch) s_dirty = true;
    const char *hint = CurrentHint();
    G::ScrollBar(dl, sp.X(1222 + ex), sp.Y(324), sp.Y(606 + ey), sp);
    ImGui::EndChild();
    if (hint)
        TextWrapped(dl, kText, sp.X(835 + ex * 0.5f), sp.Y(640 + ey), 22 * sp.s, (770 + ex) * sp.s, 2, hint, 1.1f, 0.95f, kHint, &sp);

    /* Play and Quit, under the portrait */
    {
        struct { const char *t; float y, sx, sy; int result; } items[] = {
            { "Play", 652, 1.9f, 1.8f, 1 }, { "Quit", 700, 1.45f, 1.4f, 0 } };
        for (auto &it : items) {
            float tw = Width(kTitle, it.t, it.sx, &sp);
            ImVec2 a(sp.X(228) - tw * 0.5f - 10 * sp.s, sp.Y(it.y - 34 + ep)), b(sp.X(228) + tw * 0.5f + 10 * sp.s, sp.Y(it.y + 6 + ep));
            bool hot = ImGui::IsMouseHoveringRect(a, b);
            if (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { if (it.result) play(); else s_result = 0; }
            ImU32 c = it.result ? (hot ? IM_COL32(255, 255, 255, 255) : kWhite) : (hot ? kWhite : kGrey);
            Text(dl, kTitle, sp.X(228), sp.Y(it.y + ep), it.t, it.sx, it.sy, it.result && hot ? IM_COL32(200, 40, 20, 255) : c,
                 1, true, &sp);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) play();
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) s_result = 0;
    }
    {
        /* a newer release: under Quit, a click away from About */
        const char *tag = "";
        if (rdr_update_state(&tag, nullptr) == RDR_UPDATE_NEWER) {
            char t[96];
            snprintf(t, sizeof t, "%s is out -- About", tag);
            float tw = Width(kText, t, 1.0f, &sp);
            ImVec2 a(sp.X(228) - tw * 0.5f, sp.Y(722 + ep)), b(sp.X(228) + tw * 0.5f, sp.Y(748 + ep));
            if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(0))
                s_page = 5;
            Text(dl, kText, sp.X(228), sp.Y(744 + ep), t, 1.0f, 1.0f, IM_COL32(240, 200, 90, 255), 1, false, &sp);
        }
    }
    /* the prompts */
    {
        float adv = 0, x = sp.X(470), base = sp.Y(712 + ey);
        if (Prompt(dl, x, base, 'A', "Accept", 1.6f, 1.35f, sp, &adv)) {
            /* clicked: as A or Enter on the row with the cursor */
            AcceptCursorRow();
        }
        x += adv + 40 * sp.s;
        if (Prompt(dl, x, base, 'B', "Quit", 1.6f, 1.35f, sp, &adv)) s_result = 0;
        x += adv + 40 * sp.s;
        Text(dl, kText, x, base, "LB/RB: pages", 1.25f, 1.1f, kGrey, 0, true, &sp);
    }
    ImGui::End();
}

void apply_pad(ImGuiIO &io, unsigned buttons, int a, int b, int x, int y, int lb, int rb,
               short lx, short ly)
{
    (void)x; (void)y;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (buttons & 0x0001) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (buttons & 0x0002) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (buttons & 0x0004) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (buttons & 0x0008) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadStart, (buttons & 0x0010) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadBack, (buttons & 0x0020) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, a != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, b != 0);
    io.AddKeyEvent(ImGuiKey_GamepadL1, lb != 0);
    io.AddKeyEvent(ImGuiKey_GamepadR1, rb != 0);
    auto axis = [&](ImGuiKey neg, ImGuiKey pos, short v) {
        float f = v / 32767.0f;
        io.AddKeyAnalogEvent(neg, f < -0.35f, f < -0.35f ? -f : 0.0f);
        io.AddKeyAnalogEvent(pos, f > 0.35f, f > 0.35f ? f : 0.0f);
    };
    axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, lx);
    axis(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, ly);
}

bool setup_imgui(float scale)
{
    if (!rdr_gl_load())
        return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "rdr_launcher";
    /* ImGui's own "conflicting ID" box is for us, not players (RDR_IMGUI_DEBUG=1) */
    io.ConfigDebugHighlightIdConflicts = getenv("RDR_IMGUI_DEBUG") != nullptr;
    rdrui::Init(scale);
    if (!ImGui_ImplOpenGL3_Init("#version 330 core"))
        return false;
    rdrui::CreateTextures();
    rdrui::game::Upload();               /* the game's own art, when its files are there */
    return true;
}

void teardown_imgui()
{
    rdrui::game::Release();
    rdrui::DestroyTextures();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
}

void save_bmp(const char *path, int w, int h)
{
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
    if (!px) return;
    rdr_gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    rdr_gl.ReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        unsigned row = (unsigned)w * 3, pad = (4 - (row & 3)) & 3, sz = 54 + (row + pad) * (unsigned)h;
        unsigned char hdr[54] = {0}, z[3] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &sz, 4); hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
            fwrite(z, 1, pad, f);
        }
        fclose(f);
    }
    free(px);
}

/* The window can be any size: the board is fitted to it (frame_game,
 * frame). The game's art scales with it; ImGui's own fonts (the search
 * lists, the old look) are made for one size, so while the window is
 * being dragged they are stretched (FontGlobalScale) and once it stops
 * they are made again at the new size. */
float fit_scale(int w, int h)
{
    float s = w / 1280.0f < h / 820.0f ? w / 1280.0f : h / 820.0f;
    return s < 0.35f ? 0.35f : s > 4.0f ? 4.0f : s;
}

void fonts_follow(int w, int h, bool settled)
{
    ImGuiIO &io = ImGui::GetIO();
    float want = fit_scale(w, h);
    if (settled && fabsf(want - rdrui::g_scale) > 0.01f) {
        rdrui::Init(want);
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
    }
    io.FontGlobalScale = want / rdrui::g_scale;
}

#if defined(_WIN32)
/* ======================= Windows: Win32 + WGL ========================= */

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (!ImGui::GetCurrentContext())
        return DefWindowProcA(h, m, w, l);
    /* Rebinding a control: the key goes to the binding, not the page. */
    if (rdr_input_capturing() && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_LBUTTONDOWN
                                  || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_XBUTTONDOWN || m == WM_MOUSEWHEEL)) {
        rdr_input_msg(h, m, w, l, 2);
        return m == WM_XBUTTONDOWN ? TRUE : 0;
    }
    ImGuiIO &io = ImGui::GetIO();
    switch (m) {
    case WM_CLOSE:
        s_result = 0;
        return 0;
    case WM_GETMINMAXINFO: {
        /* no smaller than half the board */
        RECT r = { 0, 0, 640, 410 };
        AdjustWindowRect(&r, (DWORD)GetWindowLongA(h, GWL_STYLE), FALSE);
        ((MINMAXINFO *)l)->ptMinTrackSize.x = r.right - r.left;
        ((MINMAXINFO *)l)->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        s_sizing = true;
        return 0;
    case WM_EXITSIZEMOVE:
        s_sizing = false;
        return 0;
    case WM_SIZE:
        /* while an edge is dragged Windows runs its own loop and ours
         * waits: draw from here so the page follows the mouse */
        if (s_sizing && s_paint)
            s_paint();
        return 0;
    case WM_MOUSEMOVE:
        io.AddMousePosEvent((float)(short)LOWORD(l), (float)(short)HIWORD(l));
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        io.AddMouseButtonEvent(0, m == WM_LBUTTONDOWN);
        if (m == WM_LBUTTONDOWN) SetCapture(h); else ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        io.AddMouseButtonEvent(1, m == WM_RBUTTONDOWN);
        return 0;
    case WM_MOUSEWHEEL:
        io.AddMouseWheelEvent(0, (float)GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA);
        return 0;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        bool down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN;
        ImGuiKey k = ImGuiKey_None;
        switch (w) {
        case VK_TAB: k = ImGuiKey_Tab; break;
        case VK_LEFT: k = ImGuiKey_LeftArrow; break;
        case VK_RIGHT: k = ImGuiKey_RightArrow; break;
        case VK_UP: k = ImGuiKey_UpArrow; break;
        case VK_DOWN: k = ImGuiKey_DownArrow; break;
        case VK_RETURN: k = ImGuiKey_Enter; break;
        case VK_ESCAPE: k = ImGuiKey_Escape; break;
        case VK_SPACE: k = ImGuiKey_Space; break;
        case VK_SHIFT: k = ImGuiMod_Shift; break;
        }
        if (k != ImGuiKey_None)
            io.AddKeyEvent(k, down);
        if (m == WM_SYSKEYDOWN || m == WM_SYSKEYUP)
            return DefWindowProcA(h, m, w, l);
        return 0;
    }
    case WM_KILLFOCUS:
        io.AddFocusEvent(false);
        return 0;
    case WM_SETFOCUS:
        io.AddFocusEvent(true);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

HWND s_hwnd;
HDC s_dc;
LARGE_INTEGER s_freq, s_last;

/* One frame of the page at the window's size now (not swapped). */
void paint_frame(int *pw, int *ph)
{
    ImGuiIO &io = ImGui::GetIO();
    RECT cr;
    LARGE_INTEGER now;
    GetClientRect(s_hwnd, &cr);
    int w = cr.right - cr.left, h = cr.bottom - cr.top;
    if (w < 1 || h < 1)
        return;
    fonts_follow(w, h, !s_sizing);
    io.DisplaySize = ImVec2((float)w, (float)h);
    QueryPerformanceCounter(&now);
    io.DeltaTime = (float)(now.QuadPart - s_last.QuadPart) / (float)s_freq.QuadPart;
    if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60.0f;
    s_last = now;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    if (rdrui::game::Ready()) frame_game(w, h); else frame(w, h);
    ImGui::Render();
    rdr_gl.Viewport(0, 0, w, h);
    rdr_gl.ClearColor(0, 0, 0, 1);
    rdr_gl.Clear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (pw) *pw = w;
    if (ph) *ph = h;
}

int run_launcher()
{
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    /* the exe's icon (resource 1, assets/icon): the corner and the taskbar */
    wc.hIcon = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = "RdrLauncher";
    RegisterClassExA(&wc);

    RECT work;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = work.top = 0; work.right = 1920; work.bottom = 1040;
    }
    int ww = work.right - work.left, wh = work.bottom - work.top;
    /* 1280 x 820 at 720p scale, larger on a big desktop, smaller if needed. */
    float scale = 1.0f;
    while (scale > 0.6f && (1280 * scale > ww - 40 || 820 * scale > wh - 60)) scale -= 0.05f;
    while (scale < 2.0f && 1280 * (scale + 0.25f) <= ww * 0.7f && 820 * (scale + 0.25f) <= wh * 0.8f) scale += 0.25f;
    RECT r = { 0, 0, (LONG)(1280 * scale), (LONG)(820 * scale) };
    DWORD style = WS_OVERLAPPEDWINDOW;  /* (any size, maximizable: the page follows) */
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExA(0, "RdrLauncher", "Red Dead Revolver", style,
                                work.left + (ww - (r.right - r.left)) / 2, work.top + (wh - (r.bottom - r.top)) / 2,
                                r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
        return 1;                       /* no window: just play */
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    int pf = ChoosePixelFormat(dc, &pfd);
    HGLRC rc = NULL;
    if (!pf || !SetPixelFormat(dc, pf, &pfd) || !(rc = wglCreateContext(dc)) || !wglMakeCurrent(dc, rc)) {
        if (rc) wglDeleteContext(rc);
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return 1;
    }
    typedef BOOL (WINAPI *SwapInterval)(int);
    SwapInterval si = (SwapInterval)wglGetProcAddress("wglSwapIntervalEXT");
    if (si) si(1);
    if (!setup_imgui(scale)) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(rc);
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);

    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_last);
    s_hwnd = hwnd;
    s_dc = dc;
    s_paint = [] { paint_frame(NULL, NULL); SwapBuffers(s_dc); };
    const char *shot = getenv("RDR_LAUNCHER_SHOT");
    int frames = 0;
#ifdef RDR_HAVE_SDL2
    /* The game starts SDL's controllers again on its own thread after this:
     * they are shut down here when the launcher closes. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    bool sdl_pads = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
    SDL_GameController *sdl_pad = NULL;
#endif
    s_result = -1;
    while (s_result < 0) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        ImGuiIO &io = ImGui::GetIO();
        bool pad = false;
#ifdef RDR_HAVE_SDL2
        /* PlayStation, Switch and other pads through SDL (Xbox pads too). */
        if (sdl_pads) {
            SDL_GameControllerUpdate();
            if (sdl_pad && !SDL_GameControllerGetAttached(sdl_pad)) {
                SDL_GameControllerClose(sdl_pad);
                sdl_pad = NULL;
            }
            if (!sdl_pad)
                for (int i = 0; i < SDL_NumJoysticks() && !sdl_pad; i++)
                    if (SDL_IsGameController(i))
                        sdl_pad = SDL_GameControllerOpen(i);
            if (sdl_pad) {
                SDL_GameController *c = sdl_pad;
                auto bt = [c](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(c, b) != 0; };
                unsigned w = (bt(SDL_CONTROLLER_BUTTON_DPAD_UP) ? 0x01 : 0) | (bt(SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? 0x02 : 0)
                           | (bt(SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? 0x04 : 0) | (bt(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 0x08 : 0)
                           | (bt(SDL_CONTROLLER_BUTTON_START) ? 0x10 : 0) | (bt(SDL_CONTROLLER_BUTTON_BACK) ? 0x20 : 0);
                apply_pad(io, w, bt(SDL_CONTROLLER_BUTTON_A), bt(SDL_CONTROLLER_BUTTON_B),
                          bt(SDL_CONTROLLER_BUTTON_X), bt(SDL_CONTROLLER_BUTTON_Y),
                          bt(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), bt(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER),
                          SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX),
                          (short)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY)));
                pad = true;
            }
        }
#endif
        for (DWORD i = 0; i < 4 && !pad; i++) {
            XINPUT_STATE xs;
            if (XInputGetState(i, &xs) == ERROR_SUCCESS) {
                const XINPUT_GAMEPAD &g = xs.Gamepad;
                apply_pad(io, g.wButtons, g.wButtons & XINPUT_GAMEPAD_A, g.wButtons & XINPUT_GAMEPAD_B,
                          g.wButtons & XINPUT_GAMEPAD_X, g.wButtons & XINPUT_GAMEPAD_Y,
                          g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER, g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER,
                          g.sThumbLX, g.sThumbLY);
                break;
            }
        }
        if (IsIconic(hwnd)) {
            Sleep(30);
            continue;
        }
        install_done_check();
        int w = 0, h = 0;
        paint_frame(&w, &h);
        if (shot && rdr_preview_setup_state(NULL, NULL, 0) != 1 && rdr_game_install_state(NULL, NULL, 0) != 1
            && ++frames == 4) {
            save_bmp(shot, w, h);
            s_result = 0;
        }
        SwapBuffers(dc);
    }
    s_paint = NULL;
#ifdef RDR_HAVE_SDL2
    if (sdl_pad)
        SDL_GameControllerClose(sdl_pad);
    if (sdl_pads)
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
#endif
    teardown_imgui();
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    UnregisterClassA("RdrLauncher", wc.hInstance);
    return s_result;
}

#else
/* ========================= Elsewhere: SDL2 ============================= */

int run_launcher()
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    float scale = 1.0f;
    const char *e = getenv("RDR_LAUNCHER_SCALE");
    if (e) scale = (float)atof(e);
    if (scale < 0.5f || scale > 3.0f) scale = 1.0f;
    SDL_Window *win = SDL_CreateWindow("Red Dead Revolver", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       (int)(1280 * scale), (int)(820 * scale),
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_SetWindowMinimumSize(win, 640, 410);
    if (!win)
        return 1;
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        SDL_DestroyWindow(win);
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);
    if (!setup_imgui(scale)) {
        SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(win);
        return 1;
    }
    const char *shot = getenv("RDR_LAUNCHER_SHOT");
    int frames = 0;
    Uint64 last = SDL_GetPerformanceCounter();
    s_result = -1;
    while (s_result < 0) {
        SDL_Event ev;
        ImGuiIO &io = ImGui::GetIO();
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: s_result = 0; break;
            case SDL_MOUSEMOTION: io.AddMousePosEvent((float)ev.motion.x, (float)ev.motion.y); break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                io.AddMouseButtonEvent(ev.button.button == SDL_BUTTON_LEFT ? 0 : 1, ev.type == SDL_MOUSEBUTTONDOWN);
                break;
            case SDL_MOUSEWHEEL: io.AddMouseWheelEvent(0, (float)ev.wheel.y); break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                ImGuiKey k = ImGuiKey_None;
                switch (ev.key.keysym.sym) {
                case SDLK_TAB: k = ImGuiKey_Tab; break;
                case SDLK_LEFT: k = ImGuiKey_LeftArrow; break;
                case SDLK_RIGHT: k = ImGuiKey_RightArrow; break;
                case SDLK_UP: k = ImGuiKey_UpArrow; break;
                case SDLK_DOWN: k = ImGuiKey_DownArrow; break;
                case SDLK_RETURN: k = ImGuiKey_Enter; break;
                case SDLK_ESCAPE: k = ImGuiKey_Escape; break;
                case SDLK_SPACE: k = ImGuiKey_Space; break;
                }
                if (k != ImGuiKey_None) io.AddKeyEvent(k, ev.type == SDL_KEYDOWN);
                break;
            }
            }
        }
        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        if (getenv("RDR_LAUNCHER_SIZE") && frames == 0) {
            /* (tests: another window size, "WxH") */
            int tw, th;
            if (sscanf(getenv("RDR_LAUNCHER_SIZE"), "%dx%d", &tw, &th) == 2) {
                SDL_SetWindowSize(win, tw, th);
                SDL_GL_GetDrawableSize(win, &w, &h);
            }
        }
        {
            /* the fonts made again once the size has stayed put a moment */
            static int lw, lh;
            static Uint64 changed;
            Uint64 t = SDL_GetPerformanceCounter();
            if (w != lw || h != lh) { lw = w; lh = h; changed = t; }
            fonts_follow(w, h, shot || (double)(t - changed) / (double)SDL_GetPerformanceFrequency() > 0.25);
        }
        io.DisplaySize = ImVec2((float)w, (float)h);
        Uint64 now = SDL_GetPerformanceCounter();
        io.DeltaTime = (float)(now - last) / (float)SDL_GetPerformanceFrequency();
        if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60.0f;
        last = now;
        if (shot) {
            /* A test frame: hover the second row so the highlight shows. */
            io.AddMousePosEvent(w * 0.6f, (232 + 46 + 20) * scale + (h - 740 * scale) * 0.5f);
        }
        install_done_check();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        if (rdrui::game::Ready()) frame_game(w, h); else frame(w, h);
        ImGui::Render();
        rdr_gl.Viewport(0, 0, w, h);
        rdr_gl.ClearColor(0, 0, 0, 1);
        rdr_gl.Clear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shot && rdr_preview_setup_state(NULL, NULL, 0) != 1 && rdr_game_install_state(NULL, NULL, 0) != 1
            && ++frames == 4) {
            save_bmp(shot, w, h);
            s_result = 0;
        }
        SDL_GL_SwapWindow(win);
    }
    teardown_imgui();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    return s_result;
}
#endif

} /* namespace */

extern "C" int rdr_launcher_wanted(int argc, char **argv, int settings_existed)
{
    s_first = !settings_existed || getenv("RDR_LAUNCHER_FIRST") != NULL;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--launcher") || !strcmp(argv[i], "-launcher"))
            return 1;
    if (getenv("RDR_LAUNCHER_SHOT"))
        return 1;
    if (getenv("RDR_NO_LAUNCHER") || getenv("RECOMP_HEADLESS"))
        return 0;
    if (!settings_existed || g_rdr_settings.show_launcher)
        return 1;
#if defined(_WIN32)
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        return 1;
#endif
    return 0;
}

extern "C" int rdr_launcher_run(void)
{
    const char *p = getenv("RDR_LAUNCHER_PAGE");
    s_page = p ? atoi(p) % kNumPages : 0;
    s_dirty = false;
    if (g_rdr_settings.update_check)
        rdr_update_start();
    if (getenv("RDR_LAUNCHER_SETUP"))
        rdr_preview_setup_start(getenv("RDR_LAUNCHER_SETUP"));
    if (getenv("RDR_LAUNCHER_INSTALL") && rdr_game_missing())
        rdr_game_install_start(getenv("RDR_LAUNCHER_INSTALL"));
    if (getenv("RDR_LAUNCHER_XBE") && rdr_game_missing())
        rdr_game_use_folder(getenv("RDR_LAUNCHER_XBE"));
    int r = run_launcher();
    /* Saved either way: a first start leaves the file behind, so the
     * launcher does not come back unasked. */
    rdr_settings_save(&g_rdr_settings);
    return r;
}
