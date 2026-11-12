/*
 * rdr_ui_pages.cpp - the settings pages, shared by the launcher and the
 * in-game overlay. In the overlay (live) a change reaches the running game
 * at once where it can; the rest say they wait for the next start.
 */
#include "rdr_ui_common.h"
#include <string.h>
#include "rdr_input.h"


#include <stdio.h>

extern "C" {
#include "kernel/nv2a_gl.h"
void xbox_FramebufferWindowToggleFullscreen(void);
int  xbox_FramebufferWindowIsFullscreen(void);
void xbox_InputSetRumbleScale(float scale);
void xbox_VblankSetHz(int hz);
int rdr_frame_rate_held(void);
}

namespace rdrui {

static const char *kNextStart = "Takes effect the next time the game starts.";

/* "This page's defaults": the page's own settings back to how they
 * started, the others as they are. */
static bool page_defaults(RdrSettings *s, int page)
{
    RdrSettings d;
    if (!Action("This page's defaults", "Restore", "Everything on this page back to how it started; "
                                                    "the other pages are left as they are."))
        return false;
    rdr_settings_defaults(&d);
    switch (page) {
    case 0:
        s->fullscreen = d.fullscreen; s->borderless = d.borderless; s->window_height = d.window_height;
        s->window_scale = d.window_scale; s->widescreen = d.widescreen; s->stretch = d.stretch;
        s->letterbox = d.letterbox; s->look2002 = d.look2002; s->look2002_bands = d.look2002_bands; s->vsync = d.vsync; s->fps_cap = d.fps_cap; s->show_fps = d.show_fps;
        break;
    case 1:
        s->render_scale = d.render_scale; s->fxaa = d.fxaa; s->aniso = d.aniso;
        s->sharp_filter = d.sharp_filter; s->sharp_text = d.sharp_text; s->dump_textures = d.dump_textures;
        break;
    case 2:
        s->vibration = d.vibration; s->deadzone = d.deadzone; s->invert_cam_x = d.invert_cam_x; s->buttons = d.buttons;
        s->invert_cam_y = d.invert_cam_y; s->kbm_player = d.kbm_player; s->kbm2_player = d.kbm2_player;
        memcpy(s->pad_player, d.pad_player, sizeof s->pad_player);
        s->mouse_aim = d.mouse_aim; s->mouse_sens = d.mouse_sens; s->mouse_duel_sens = d.mouse_duel_sens;
        s->mouse_invert_x = d.mouse_invert_x; s->mouse_invert_y = d.mouse_invert_y;
        s->pause_on_focus = d.pause_on_focus;
        memcpy(s->keys, d.keys, sizeof s->keys);
        break;
    }
    return true;
}

bool PageDisplay(RdrSettings *s, bool live)
{
    bool ch = false;
    static const char *fs_items[] = { "Window", "Borderless window", "Fullscreen" };
    static const int heights[] = { 720, 900, 1080, 1440, 2160 };
    static const char *size_items[] = { "1280 x 720 (720p)", "1600 x 900", "1920 x 1080 (1080p)",
                                        "2560 x 1440 (1440p)", "3840 x 2160 (4K)" };
    static const char *shape_items[] = { "As the game draws it", "Stretch to fill" };
    static const char *cap_items[] = { "30", "60 (original)", "120", "144", "165", "240", "Unlimited" };
    static const int cap_vals[] = { 30, 60, 120, 144, 165, 240, 500 };

    int fs = s->fullscreen ? 2 : s->borderless ? 1 : 0;
    if (Choice("Display mode", &fs, fs_items, 3,
               "Fullscreen is borderless, on the game's monitor; F11 or Alt+Enter switches while "
               "playing. A borderless window has no frame or title bar; at the screen's own size it "
               "covers the whole screen, taskbar too (from the next start).")) {
        s->fullscreen = fs == 2;
        s->borderless = fs == 1;
        ch = true;
    }
    int ws = 0;
    for (int i = 0; i < 5; i++)
        if (heights[i] == s->window_height) ws = i;
    if (Choice("Window size", &ws, size_items, 5,
               live ? kNextStart : "The window's size when not fullscreen. If it would not fit on the "
                                   "screen, the next size down. A 4:3 picture gets bars at the sides.")) {
        s->window_height = heights[ws];
        ch = true;
    }
    ch |= Toggle("Widescreen (16:9)", &s->widescreen,
                 live ? "The game's own widescreen mode: more of the world to either side. "
                        "Takes effect the next time the game starts."
                      : "The game's own widescreen mode: more of the world to either side, "
                        "nothing stretched.");
    ch |= Choice("Picture shape", &s->stretch, shape_items, 2,
                 "Keep the game's 4:3 or 16:9 picture with bars at the sides, or fill the window.");
    {
        static const char *bars_items[] = { "Off", "In cutscenes only", "Always (as the game)" };
        ch |= Choice("Black bars", &s->letterbox, bars_items, 3,
                     "The black bands the game draws at the top and bottom of the picture, for its "
                     "old-film look. Off shows the whole picture while playing and in cutscenes. "
                     "Takes effect at once.");
    }
    ch |= Toggle("2002 look", &s->look2002,
                 "The health bar, ammunition, Dead Eye, money and clock as the January 2002 preview "
                 "drew them. Takes effect at once.");
    if (s->look2002)
        ch |= Toggle("2002 blue bands", &s->look2002_bands,
                     "The 2002 look's dark blue curved bands at the top and bottom of the picture. "
                     "Takes effect at once.");
    ch |= Toggle("Vertical sync", &s->vsync,
                 "Wait for the monitor before showing each frame: no tearing.");
    int cap = 1;
    for (int i = 0; i < 7; i++)
        if (cap_vals[i] == (s->fps_cap ? s->fps_cap : 60)) cap = i;
    if (Choice("Frame rate", &cap, cap_items, 7,
               "The game times itself by the clock, so it can run faster than the console's 60. "
               "Above 60 is new: if anything misbehaves, go back to 60. Vertical sync caps it at "
               "your monitor's rate.")) {
        s->fps_cap = cap_vals[cap];
        ch = true;
    }
    if (rdr_frame_rate_held() && (s->fps_cap ? s->fps_cap : 60) > 60)
        Note("Held at 60 in this chapter (Sunset Canyon): above 60 the stagecoach chase breaks (the coach "
             "is thrown off its track). This is a known issue; your setting comes back when you leave the "
             "chapter.");
    ch |= Toggle("Frame counter", &s->show_fps,
                 "Frames per second in the corner. F2 shows or hides it while playing.");
    ImGui::Dummy(ImVec2(1, 6 * g_scale));
    ch |= page_defaults(s, 0);
    if (ch && live)
        ApplyLive(s);
    return ch;
}

bool PageGraphics(RdrSettings *s, bool live)
{
    bool ch = false;
    static char res[6][48];
    static const char *res_items[6];
    for (int i = 0; i < 6; i++) {
        int w = (s->widescreen ? 720 : 640) * (i + 1), h = 480 * (i + 1);
        snprintf(res[i], sizeof res[i], i == 0 ? "%d x %d (original)" : "%d x %d", w, h);
        res_items[i] = res[i];
    }
    static const char *aniso_items[] = { "Off", "2x", "4x", "8x", "16x" };
    static const int aniso_vals[] = { 1, 2, 4, 8, 16 };
    static const char *filter_items[] = { "Smooth", "Sharp" };

    int rs = s->render_scale - 1;
    if (Choice("Internal resolution", &rs, res_items, 6,
               "How finely the 3D world is drawn. 3x suits a 1440p screen, 4x-6x a 4K one; "
               "higher needs a stronger graphics card.")) {
        s->render_scale = rs + 1;
        ch = true;
    }
    ch |= Toggle("Anti-aliasing (FXAA)", &s->fxaa, "Smooths jagged edges, at almost no cost.");
    int an = 0;
    for (int i = 0; i < 5; i++)
        if (aniso_vals[i] <= s->aniso) an = i;
    if (Choice("Texture filtering", &an, aniso_items, 5,
               "Anisotropic filtering: keeps roads, floors and fences sharp into the distance.")) {
        s->aniso = aniso_vals[an];
        ch = true;
    }
    ch |= Choice("Upscaling", &s->sharp_filter, filter_items, 2,
                 "How the picture is scaled to the window: smooth, or sharp square pixels.");
    ch |= Toggle("Sharp game text", &s->sharp_text,
                 live ? "The game's own lettering (menus, subtitles, the money) redrawn four times "
                        "finer from its own fonts. Takes effect the next time the game starts."
                      : "The game's own lettering (menus, subtitles, the money) redrawn four times "
                        "finer from its own fonts, so it stays sharp on big screens.");
    ch |= Toggle("Dump textures", &s->dump_textures,
                 "For making texture packs: every texture the game loads is saved to texture_dump\\ "
                 "as a .tga named by its hash. Put edited ones in mods\\textures\\. "
                 "Takes effect the next time the game starts; slows loading while on.");
    ImGui::Dummy(ImVec2(1, 6 * g_scale));
    ch |= page_defaults(s, 1);
    if (ch && live)
        ApplyLive(s);
    return ch;
}

bool PageControls(RdrSettings *s, bool live)
{
    bool ch = false;
    ch |= Slider("Vibration", &s->vibration, 0, 100, 10, "%d%%",
                 "How hard the controller rumbles. 0 turns it off.");
    ch |= Slider("Stick deadzone", &s->deadzone, 0, 40, 2, "%d%%",
                 "Ignore small stick movements. Raise it if the camera drifts on its own.");
    ch |= Toggle("Invert camera up/down", &s->invert_cam_y,
                 "Right stick: push up to look down.");
    ch |= Toggle("Invert camera left/right", &s->invert_cam_x,
                 "Right stick: push left to look right.");
    {
        static const char *styles[] = { "As the controller", "Xbox", "PlayStation" };
        ch |= Choice("Button pictures", &s->buttons, styles, 3,
                     "The buttons the game shows in its prompts and menus. As the controller: "
                     "PlayStation's cross, circle, square and triangle with a PlayStation pad "
                     "(the PS2 version's own pictures, still on the disc), else Xbox's. In the "
                     "game's own menus from the next chapter or menu loaded; in these menus at once.");
    }
    {
        /* Who holds what: Showdown's split screen is up to four players. */
        /* Each controller as it is: "Controller 2: PS5 Controller". */
        static char padbuf[5][128];
        static const char *pads[5];
        pads[0] = "None";
        for (int k = 1; k <= 4; k++) {
            const char *n = xbox_InputPortName((DWORD)(k - 1));   /* input/xinput_xbox.h */
            snprintf(padbuf[k], sizeof padbuf[k], "Controller %d: %s", k, n ? n : "not connected");
            pads[k] = padbuf[k];
        }
        static const char *players[] = { "Player 1", "Player 2", "Player 3", "Player 4" };
        static const char *labels[] = { "Player 1 controller", "Player 2 controller",
                                        "Player 3 controller", "Player 4 controller" };
        ImGui::Dummy(ImVec2(1, 6 * g_scale));
        int kb = s->kbm_player - 1;
        if (kb < 0 || kb > 3) kb = 0;
        if (Choice("Keyboard and mouse are", &kb, players, 4,
                   "Which player the keyboard and mouse control. Put them on Player 1 and give "
                   "Player 1 no controller to play split screen against a friend on a pad.")) {
            s->kbm_player = kb + 1;
            ch = true;
        }
        for (int i = 0; i < 4; i++)
            ch |= Choice(labels[i], &s->pad_player[i], pads, 5,
                         "Which controller this player holds: controllers count in the order "
                         "Windows connected them. A player added or removed here (the console's "
                         "number of controllers) takes effect the next time the game starts.");
#if defined(_WIN32)
        {
            /* A second keyboard and mouse: another player's (rdr_input.c). */
            static const char *who[] = { "No one", "Player 1", "Player 2", "Player 3", "Player 4" };
            ImGui::Dummy(ImVec2(1, 6 * g_scale));
            ch |= Choice("Second keyboard and mouse are", &s->kbm2_player, who, 5,
                         "Two players on one PC, each with a keyboard and a mouse: which player the "
                         "second set controls, with the same keys as the first. Pick the two in the "
                         "game (F1, Controls). A player new to the game this way joins the next time "
                         "it starts.");
            if (s->kbm2_player > 0) {
                char kb[256], ms[256];
                int pick = rdr_input_kb2_picking();
                if (rdr_input_kb2_take(kb, ms, (int)sizeof kb)) {
                    snprintf(s->kb2_device, sizeof s->kb2_device, "%s", kb);
                    snprintf(s->mouse2_device, sizeof s->mouse2_device, "%s", ms);
                    rdr_input_kb2_changed();
                    ch = true;
                }
                const char *state = s->kb2_device[0]
                    ? (s->mouse2_device[0] ? "Keyboard and mouse" : "Keyboard, no mouse")
                    : s->mouse2_device[0] ? "Mouse, no keyboard" : "Not picked yet";
                if (s->kbm2_player == s->kbm_player)
                    Note("That player has the first keyboard and mouse already: give the second set another player.");
                if (!live) {
                    Action("Second set", state, "Picked in the game: F1, Controls.");
                } else if (pick == 1) {
                    if (Action("Press a key on the second keyboard", "Esc cancels", "Any key on the keyboard "
                               "for the second player."))
                        rdr_input_kb2_cancel();
                } else if (pick == 2) {
                    if (Action("Now click with the second mouse", "Esc: no mouse", "Esc on the second "
                               "keyboard if that player has no mouse of their own."))
                        rdr_input_kb2_cancel();
                } else {
                    if (Action("Pick the second keyboard and mouse", state,
                               "Press a key on the second keyboard, then click with the second mouse."))
                        rdr_input_kb2_pick();
                    if ((s->kb2_device[0] || s->mouse2_device[0])
                        && Action("Forget them", "", "Both keyboards and mice back to the first player.")) {
                        s->kb2_device[0] = s->mouse2_device[0] = 0;
                        rdr_input_kb2_changed();
                        ch = true;
                    }
                }
            }
        }
#endif
    }
#if defined(_WIN32)
    ImGui::Dummy(ImVec2(1, 6 * g_scale));
    ch |= Toggle("Mouse and keyboard", &s->mouse_aim,
                 live ? "Mouse to aim, keys for the rest (below). Takes effect the next time the game starts."
                      : "Mouse to aim and look, keys for the rest (below). F9 frees the mouse pointer.");
    ch |= Slider("Mouse sensitivity", &s->mouse_sens, 10, 400, 10, "%d%%",
                 "How far the camera turns for the mouse's movement (100%: about 0.07 degrees a count).");
    ch |= Slider("Duel aim sensitivity", &s->mouse_duel_sens, 25, 400, 5, "%d%%",
                 "How far a duel's aiming cursor goes for the mouse's movement, on top of the mouse sensitivity.");
    ch |= Toggle("Invert mouse up/down", &s->mouse_invert_y, "Move the mouse forward to look down.");
    ch |= Toggle("Invert mouse left/right", &s->mouse_invert_x, "Move the mouse right to turn left.");
    ch |= Toggle("Pause when in the background", &s->pause_on_focus,
                 "Switching to another program opens the game's pause menu (in play, not during cutscenes).");
    if (s->mouse_aim) {
        static int waiting = -1;
        int got = rdr_input_captured();
        if (waiting >= 0 && !rdr_input_capturing()) {
            if (got > 0) {
                s->keys[waiting] = got;
                ch = true;
            } else if (got == -2) {
                s->keys[waiting] = 0;          /* Delete: unbound */
                ch = true;
            }
            waiting = -1;
        }
        for (int c = 0; c < RDR_KEY_COUNT; c++) {
            const char *v = waiting == c ? "press a key (Del: none)..." : rdr_key_name(s->keys[c]);
            if (Action(rdr_control_name(c), v,
                       "Select, then press the key or mouse button for it. Esc keeps the old one; Delete leaves the control unbound.")) {
                waiting = c;
                rdr_input_capture_next();
            }
        }
    }
#endif
    ImGui::Dummy(ImVec2(1, 6 * g_scale));
    ch |= page_defaults(s, 2);
    if (ch && live)
        ApplyLive(s);
    return ch;
}

/* Mods: each mod (a folder in Mods) and texture pack (a folder in
 * Mods\textures) switched on or off, from the next start (rdr_mods.c). */
bool PageMods(RdrSettings *s, bool live)
{
    bool ch = false;
    static RdrModEntry e[128];
    int n = rdr_mods_list(s, e, 128), mods = 0, packs = 0;
    const char *next = live ? " Takes effect the next time the game starts." : "";
    static char hint[400], hint2[400];       /* (the row keeps the pointer) */
    for (int i = 0; i < n; i++)
        (e[i].kind < 2 ? mods : packs)++;
    Section("Mods");
    if (!mods)
        Note("No mods yet. Put each mod in a folder of its own inside Mods, with its files where the "
             "game has them (Mods\\My Mod\\swat.dat, Mods\\My Mod\\lvls_m2\\...).");
    snprintf(hint, sizeof hint, "A mod's files are used instead of the game's own. When two mods change "
             "the same file, the one higher in this list is used.%s", next);
    for (int i = 0; i < n; i++) {
        if (e[i].kind >= 2) continue;
        int on = e[i].on;
        ImGui::PushID(i);
        if (Toggle(e[i].name, &on, hint)) {
            rdr_mods_set(s, &e[i], on);
            ch = true;
        }
        ImGui::PopID();
    }
    if (Action("Open the Mods folder", "", "Add a mod: put its folder in here."))
        rdr_open_folder(rdr_mods_base());
    Section("Texture packs");
    if (!packs)
        Note("No texture packs yet. Put each pack in a folder of its own inside Mods\\textures "
             "(.tga files named by the texture's code; Graphics > Dump textures writes them out).");
    snprintf(hint2, sizeof hint2, "Replacement textures. When two packs replace the same texture, the "
             "one higher in this list is used.%s", next);
    for (int i = 0; i < n; i++) {
        if (e[i].kind < 2) continue;
        int on = e[i].on;
        ImGui::PushID(1000 + i);
        if (Toggle(e[i].name, &on, hint2)) {
            rdr_mods_set(s, &e[i], on);
            ch = true;
        }
        ImGui::PopID();
    }
    if (Action("Open the texture packs folder", "", "Add a texture pack: put its folder in here."))
        rdr_open_folder(rdr_texpacks_base());
    return ch;
}

/* Extras: the 2002 preview's characters and stages. Set up from the
 * preview's disc image in the launcher (live: in the game, only switched;
 * it takes effect at the next start). */
bool PageExtras(RdrSettings *s, bool live)
{
    bool ch = false;
    static bool confirm_remove;
    static bool people_job;                 /* the running or last job: the story's people alone */
    static int seen_state;
    static char image[1024];
    char text[256] = "";
    int pct = 0;
    int st = rdr_preview_setup_state(&pct, text, sizeof text);
    bool ready = rdr_preview_ready() != 0;
    if (st == 2 && seen_state != 2 && !people_job) {   /* just set up: on */
        s->preview = 1;
        ch = true;
    }
    seen_state = st;
    Section("2002 preview characters");
    if (ready) {
        ch |= Toggle("Preview characters", &s->preview,
                     live ? "The 2002 preview's characters and stages in Showdown and Play As. "
                            "The stages are ported as they are on that unfinished disc, and some may "
                            "not work properly as Showdown stages. "
                            "Takes effect the next time the game starts."
                          : "The 2002 preview's characters and stages, and the story's townsfolk, "
                            "in Showdown and Play As. The stages are ported as they are on that "
                            "unfinished disc, and some may not work properly as Showdown stages.");
    } else if (st != 1) {
        Note("Not set up yet. You need a disc image of the January 2002 preview "
             "(PlayStation 2, the 15 January 2002 build). Setting up takes 5 to 10 minutes "
             "and about 2 GB of space.");
    }
    if (live) {
        if (!ready)
            Note("Set them up in the launcher: hold Shift while the game starts, then Extras.");
        return ch;
    }
    if (st == 1) {
        char v[32];
        snprintf(v, sizeof v, "%d%%", pct);
        Action("Setting up", v, people_job ? "Making the story's people from the game's files. You can leave "
                                             "this window open; Play waits until it is done."
                                           : "Converting the preview's characters and stages. You can leave "
                                "this window open; Play waits until it is done.");
        Note(text);
        return ch;
    }
    if (ready && rdr_preview_people_old())
        Note("Made by an older version of the setup, which wrote files the game can crash on (taking "
             "off as Bat Boy, for one), and some of the story's people are missing. Set up again to "
             "bring them up to date.");
    if (Action(ready ? "Set up again" : "Set up the preview characters", "Choose the disc image",
               "Pick the preview's disc image (.iso). It is only read: the converted characters "
               "and stages go in Mods\\Preview.")) {
        if (rdr_preview_pick_image(image, sizeof image)) {
            people_job = false;
            rdr_preview_setup_start(image);
        }
    }
    if (st == 2 && !people_job)
        Note("Done: the preview characters are on. Play As lists them, and Showdown's line-up "
             "has them on its later pages.");
    else if (st == 3 && !people_job)
        Note(text);
    if (ready) {
        if (Action("Remove the preview characters", confirm_remove ? "Select again to remove" : "",
                   "Moves Mods\\Preview (about 1.5 GB) to the Recycle Bin. Your saves are not "
                   "touched. Setting them up again needs the disc image.")) {
            if (!confirm_remove)
                confirm_remove = true;
            else {
                confirm_remove = false;
                if (rdr_preview_remove()) {
                    s->preview = 0;
                    ch = true;
                }
            }
        }
    }
    Section("The story's people");
    if (rdr_preview_ready() && s->preview)
        Note("On, with the preview characters: the story's townsfolk, soldiers, Atlas and the rest "
             "are in Play As, story co-op and Showdown.");
    else if (rdr_people_ready())
        Note("On: the story's townsfolk, soldiers, Atlas and the rest are in Play As, story co-op "
             "and Showdown, with or without the preview characters.");
    else
        Note("The story's townsfolk, soldiers, Atlas and the rest, for Play As, story co-op and "
             "Showdown. Made from your copy of the game (no other disc needed) in a few "
             "minutes.");
    if (rdr_people_ready() && rdr_people_old() && !(rdr_preview_ready() && s->preview))
        Note("Made by an older version of the setup, which wrote files the game can crash on, and "
             "some of the story's people are missing. Set up again to bring them up to date.");
    else if (rdr_preview_ready() && s->preview)
        Note("With the preview characters on, the game uses the preview's own copy of these people: "
             "to change it, set up the preview characters again above.");
    if (Action(rdr_people_ready() ? "Set up the story's people again" : "Set up the story's people",
               "Start", "Makes them from the game's own files, into Mods\\Story People. "
               "Takes effect the next time the game starts.")) {
        people_job = true;
        rdr_preview_setup_start(NULL);
    }
    if (people_job && st == 2)
        Note("Done: the story's people are in Play As and story co-op from the next start.");
    else if (people_job && st == 3)
        Note(text);
    return ch;
}

void ApplyLive(const RdrSettings *s)
{
    NvGlOptions *o = nvgl_options();
    o->render_scale = s->render_scale;
    o->aniso = s->aniso;
    o->fxaa = s->fxaa;
    o->filter = s->sharp_filter;
    o->aspect = s->stretch ? 1 : 0;
    o->vsync = s->vsync;
    o->fps_cap = s->fps_cap == 30 ? 30 : 0;
    xbox_VblankSetHz(s->fps_cap == 30 || !s->fps_cap ? 60 : s->fps_cap);
    if (!!s->fullscreen != !!xbox_FramebufferWindowIsFullscreen())
        xbox_FramebufferWindowToggleFullscreen();
    xbox_InputSetRumbleScale(s->vibration / 100.0f);
    {
        int pm[4];
        rdr_effective_pad_map(s, pm);
        xbox_InputSetPortMap(pm);
    }
    xbox_InputSetKeyboardPort(s->kbm_player);
}

} /* namespace rdrui */
