/*
 * rdr_overlay.cpp - the in-game menu (F1, or both sticks clicked) and the
 * frame counter (F2).
 *
 * Drawn by the GPU presenter after each frame, on its thread
 * (nvgl_set_overlay). Input arrives on two other threads -- window messages
 * from the window's thread (xbox_FramebufferWindowSetMsgHook), the
 * controller from the USB thread (xbox_InputSetFilter) -- and is queued for
 * the draw. While the menu is open the game gets neither: the keys and the
 * pad go to the menu.
 *
 * The same pad filter applies the Controls settings during play: stick
 * deadzone and inverted camera axes.
 */
#include <ctype.h>
#include "rdr_ui.h"
#include "rdr_ui_common.h"
#include "rdr_cheats.h"
#include "rdr_input.h"
#include "rdr_freecam.h"
#include "rdr_update.h"
#include "../net/rdr_net.h"
#include "rdr_gl.h"
#include "rdr_ui_game.h"
#include "imgui_internal.h"

extern "C" void rdr_timing_tick(void);
extern "C" char g_rdr_npc_note[200];
extern "C" void rdr_test_players_report(void);   /* rdr_overrides.c (tests) */
extern "C" int g_rdr_test_sound_busy;              /* rdr_hooks.c (tests) */
extern "C" int g_rdr_test_frame_us, g_rdr_test_coach_log, g_rdr_test_coach_old;          /* rdr_overrides.c: the Spawner's word */
extern "C" uint32_t g_rdr_hud_mask_test;  /* rdr_cheats.c (tests) */
extern "C" void rdr_hud2002_draw(ImDrawList *dl, int w, int h, float dt, int menu_open);   /* rdr_hud2002.cpp */
extern "C" int rdr_hud2002_wanted(void);
extern "C" int rdr_hud2002_title(ImDrawList *dl, int w, int h);
extern "C" void rdr_online_start_tick(void);   /* rdr_overrides.c */
extern "C" int  rdr_ls_status(char *buf, int n);   /* rdr_lockstep.c */
extern "C" void rdr_mouse_look_report(void);
extern "C" void rdr_menu_test_report(void);
extern "C" void rdr_coop_test_kill(int k);
extern "C" void rdr_test_tp(int k, float x, float y, float z);   /* rdr_overrides.c */
extern "C" void rdr_duel_report(void);
extern "C" void rdr_mem_dump(const char *what, uint32_t len, const char *path);
extern "C" void rdr_swf_test_set(const char *name, int value);
extern "C" void rdr_swf_test_get(const char *name);
extern "C" void rdr_warp_request(const char *name, int var);
extern "C" int  rdr_warp_pending(void);
extern "C" void rdr_input_test_mouse_hold(long dx, long dy, int ms);
extern "C" void rdr_timing_report(void);
extern "C" void rdr_count_report(void);
extern "C" void rdr_icall_report(void);
extern "C" void rdr_freeze_report(void);
extern "C" void rdr_coop_tick(void);
extern "C" int  rdr_pause_owner_port(void);                 /* rdr_overrides.c */
extern "C" int  rdr_port_view(int port, float r[4]);
extern "C" int  rdr_views_windowed(void);
extern "C" void xbox_PresentSetOverlayView(int k);          /* the toolkit */
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" {
#include "kernel/nv2a_gl.h"
#include "input/xinput_xbox.h"
void xbox_FramebufferWindowSetMsgHook(int (*fn)(void *, unsigned, uintptr_t, intptr_t));
uint64_t xbox_HostNowNs(void);
void xbox_InputKeySet(int vk, int down);
#if defined(_WIN32)
void mkb_set_paused(int paused);
#endif
}

using namespace rdrui;

namespace {

std::atomic<bool> s_open{false};

/* For the game's thread (rdr_overrides.c, the demo held off): a menu of
 * ours is open. */
extern "C" int rdr_overlay_menu_open(void) { return s_open ? 1 : 0; }
void *s_hwnd;                            /* the game window, once seen */
std::atomic<bool> s_dirty{false};      /* settings changed: save on close */
std::atomic<bool> s_game_look{false};  /* drawn in the game's art: Esc goes back a page */

/* ---- Input from other threads ---- */
struct Ev {
    enum Kind { MouseMove, MouseButton, Wheel, Key, Char, Focus } kind;
    float x, y;
    int a, b;
};
std::mutex s_mu;
std::vector<Ev> s_events;
/* Opened by the game's own menu with A: the menu does not see A until it
 * has been let go (else that same press would act on the first row). */
std::atomic<bool> s_hold_a{false};
XBOX_INPUT_STATE s_pad;                  /* latest, under s_mu */
bool s_pad_ok;

void push(const Ev &e)
{
    std::lock_guard<std::mutex> l(s_mu);
    if (s_events.size() < 512)
        s_events.push_back(e);
}

/* Closed with B or A still down: the game does not see those buttons
 * until they are let go (else the press that closed the menu would also
 * act in the game's menu under it -- B leaving the book). */
std::atomic<int> s_close_gen{0};
/* Opened: the menu starts with no key or button down. While it is shut its
 * key and button releases go to the game, not to it, so a menu closed with
 * Esc or a click still had that key or button down when it next opened --
 * and saw no new press of it: Esc and clicks did nothing until the window
 * lost the focus (user: "an issue leaving the new menus ... without
 * alt-tabbing"). */
std::atomic<bool> s_reset_input{false};
/* What the menu opens on (draw_game_menu): -1 as it was, 0 the pause
 * menu's PC Options list, 1 / 2 the book's Play As / Story Co-op, 3 the
 * Quick menu. */
std::atomic<int> s_show_req{-1};

void set_open(bool open);
/* Whose menu it is: the port that opened it (a controller's sticks, the
 * keyboard's F1 -- the keyboard player's port, the pause menu's PC Options
 * -- the player who paused), -1 everyone's. In split screen it opens over
 * that player's part of the screen, with Separate windows in that
 * player's window; only that player's controller works it, and the others
 * play on (user: "everyone has their own menu"). */
std::atomic<int> s_owner{-1};
/* F1, both sticks, a test: the Quick menu, whatever was open last (the
 * book's Play As page came back on F1 -- user report). Another player's
 * sticks while it is open do nothing: one menu at a time. */
void toggle_quick(int port = -1)
{
    if (!s_open) {
        s_show_req = 3;
        s_owner = port;
        set_open(true);
    } else if (port < 0 || s_owner < 0 || port == s_owner) {
        set_open(false);
    }
}

int kbm_port()
{
    int k = g_rdr_settings.kbm_player;
    return k >= 1 && k <= 4 ? k - 1 : 0;
}

void set_open(bool open)
{
    if (s_open != open)
        fprintf(stderr, "[UI] menu %s\n", open ? "opened" : "closed");
    if (s_open && !open)
        s_close_gen++;
    if (!s_open && open) {
        s_reset_input = true;
        std::lock_guard<std::mutex> l(s_mu);
        s_events.clear();               /* anything queued while shut is stale */
    }
    s_open = open;
    if (!open)
        s_owner = -1;
    {
        /* the keyboard and mouse go to the menu when it is theirs (or
         * everyone's); another player's menu leaves them playing */
        bool kb = open && (s_owner < 0 || s_owner == kbm_port());
        rdr_input_menu(s_hwnd, kb);
#if defined(_WIN32)
        mkb_set_paused(kb ? 1 : 0);
#endif
    }
    if (!open && s_dirty.exchange(false))
        rdr_settings_save(&g_rdr_settings);
}

#if defined(_WIN32)
ImGuiKey vk_key(uintptr_t vk)
{
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_BACK: return ImGuiKey_Backspace;
    default: return ImGuiKey_None;
    }
}

/* The window's thread. Nonzero: the game does not see this message. */
int msg_hook(void *hwnd, unsigned msg, uintptr_t w, intptr_t l)
{
    bool open = s_open && (s_owner < 0 || s_owner == kbm_port());   /* (another player's menu: the keys play) */
    s_hwnd = hwnd;
    /* Mouse and keyboard play first: raw mouse, the pointer, rebinding. */
    if (rdr_input_msg(hwnd, msg, w, l, open))
        return 1;
    if (msg == WM_KEYDOWN && !(l & (1 << 30))) {
        if (w == VK_F1) {
            toggle_quick(kbm_port());
            return 1;
        }
        if (w == VK_F6 && !rdr_online_session()) {
            g_rdr_freecam = !g_rdr_freecam;
            return 1;
        }
        if (w == VK_F2) {
            g_rdr_settings.show_fps = !g_rdr_settings.show_fps;
            rdr_settings_save(&g_rdr_settings);
            return 1;
        }
        if (open && w == VK_ESCAPE && !s_game_look) {
            set_open(false);
            return 1;
        }
    }
    if (!open)
        return 0;
    /* The game's key table (the toolkit's, filled by the window's own
     * handler) still hears every release while the menu has the keys: a
     * button or key that went down for the game and came up in the menu
     * stayed down for it. The click on the book's Play As, let go in the
     * menu, was a held A once the menu closed -- the book started the
     * chapter or opened the page again (user video). Presses made in the
     * menu stay the menu's. */
    switch (msg) {
    case WM_KEYUP: case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        if (w == VK_SHIFT) { xbox_InputKeySet(VK_LSHIFT, 0); xbox_InputKeySet(VK_RSHIFT, 0); }
        if (w == VK_CONTROL) { xbox_InputKeySet(VK_LCONTROL, 0); xbox_InputKeySet(VK_RCONTROL, 0); }
        if (w == VK_MENU) { xbox_InputKeySet(VK_LMENU, 0); xbox_InputKeySet(VK_RMENU, 0); }
        break;
    case WM_LBUTTONUP: xbox_InputKeySet(0x01, 0); break;
    case WM_RBUTTONUP: xbox_InputKeySet(0x02, 0); break;
    case WM_MBUTTONUP: xbox_InputKeySet(0x04, 0); break;
    case WM_XBUTTONUP: xbox_InputKeySet(HIWORD(w) == 1 ? 0x05 : 0x06, 0); break;
    }
    switch (msg) {
    case WM_MOUSEMOVE:
        push({Ev::MouseMove, (float)(short)LOWORD(l), (float)(short)HIWORD(l), 0, 0});
        return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        push({Ev::MouseButton, 0, 0, 0, msg == WM_LBUTTONDOWN});
        if (msg == WM_LBUTTONDOWN) SetCapture((HWND)hwnd); else ReleaseCapture();
        return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        push({Ev::MouseButton, 0, 0, 1, msg == WM_RBUTTONDOWN});
        return 1;
    case WM_MOUSEWHEEL:
        push({Ev::Wheel, 0, (float)GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA, 0, 0});
        return 1;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        ImGuiKey k = vk_key(w);
        if (k != ImGuiKey_None)
            push({Ev::Key, 0, 0, (int)k, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN});
        /* Alt+Enter and F11 still reach the window's own handler. */
        if (w == VK_F11 || (w == VK_RETURN && (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)))
            return 0;
        return 1;
    }
    case WM_CHAR:
        if (w >= 32 && w < 0x10000)
            push({Ev::Char, 0, 0, (int)w, 0});
        return 1;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return 1;
        }
        return 0;
    case WM_KILLFOCUS:
        push({Ev::Focus, 0, 0, 0, 0});
        return 0;
    }
    return 0;
}
#endif

/* ---- The controller (USB thread) ---- */

short deadzone(short v, int pct)
{
    if (pct <= 0) return v;
    int dz = 32767 * pct / 100, a = v < 0 ? -v : v;
    if (a <= dz) return 0;
    long out = (long)(a - dz) * 32767 / (32767 - dz);
    if (out > 32767) out = 32767;
    return (short)(v < 0 ? -out : out);
}

/* 1 while someone plays on the port: a controller connected there, or the
 * keyboard and mouse's own port (rdr_overrides: a co-op player with nobody
 * on their port is brought along beside player 1). */
static volatile int s_port_live[4] = { 1, 1, 1, 1 };
static volatile int s_port_ever[4];     /* someone has played there since the start */
extern "C" int rdr_net_pad_port_taken(int port);
extern "C" int rdr_port_live(int port)
{
    /* an online guest's port is as live as a controller there */
    return port >= 0 && port < 4 ? (s_port_live[port] || rdr_net_pad_port_taken(port)) : 0;
}

/* 1 when the port has had a controller (or the keyboard) at any time since
 * the game started: a wireless controller that has gone to sleep (user
 * log: "controller 1 disconnected (SDL)" while player 2 stood waiting)
 * still belongs to someone. */
/* 1 when the port has had a controller (not the keyboard) since the game
 * started. */
static volatile int s_pad_ever[4];
extern "C" int rdr_port_live_pad(int port)
{
    return port >= 0 && port < 4 ? s_pad_ever[port] : 0;
}

extern "C" int rdr_port_ever(int port)
{
    return port >= 0 && port < 4 ? (s_port_ever[port] || rdr_port_live(port)) : 0;
}

void pad_filter(DWORD port, XBOX_INPUT_STATE *st, int connected)
{
    static bool combo_was[4];
    static bool conn[4];
    if (port >= 4)
        return;
    /* The menu is worked by the first port with a controller -- player 1's,
     * or player 2's when player 1 is on the keyboard. */
    conn[port] = connected != 0;
    if (connected)
        s_pad_ever[port] = 1;
    {
        const RdrSettings &ss = g_rdr_settings;
        int kbm = ss.kbm_player >= 1 && ss.kbm_player <= 4 ? ss.kbm_player - 1 : 0;
        s_port_live[port] = connected != 0 || (int)port == kbm || (int)port == rdr_kbm2_port();
        if (s_port_live[port])
            s_port_ever[port] = 1;
    }
    DWORD nav = 0;
    while (nav < 3 && !conn[nav])
        nav++;
    if (!conn[nav])
        nav = 0;
    if (s_open && s_owner >= 0 && s_owner < 4)
        nav = (DWORD)s_owner;           /* its owner's controller works it */
    if (port == nav) {
        std::lock_guard<std::mutex> l(s_mu);
        s_pad = *st;
        s_pad_ok = connected != 0;
    }
    if (port == 1)
        rdr_input_test_port2(st);               /* tests: "stick2" */
    /* Both sticks clicked, on any controller: open or close the menu. */
    bool combo = (st->Gamepad.wButtons & (XBOX_GAMEPAD_LEFT_THUMB | XBOX_GAMEPAD_RIGHT_THUMB))
                 == (XBOX_GAMEPAD_LEFT_THUMB | XBOX_GAMEPAD_RIGHT_THUMB);
    if (combo && !combo_was[port])
        toggle_quick((int)port);
    combo_was[port] = combo;
    if ((s_open && (s_owner < 0 || (int)port == s_owner)) || combo) {
        /* The menu has the controllers. */
        memset(&st->Gamepad, 0, sizeof st->Gamepad);
        st->dwPacketNumber++;
        return;
    }
    {
        static int seen_gen[4];
        static bool mask[4];
        int gen = s_close_gen;
        if (seen_gen[port] != gen) {
            seen_gen[port] = gen;
            mask[port] = true;
        }
        if (mask[port]) {
            XBOX_GAMEPAD &gp = st->Gamepad;
            bool held = gp.bAnalogButtons[XBOX_BUTTON_A] > 64 || gp.bAnalogButtons[XBOX_BUTTON_B] > 64
                     || (gp.wButtons & XBOX_GAMEPAD_START);
            if (held) {
                gp.bAnalogButtons[XBOX_BUTTON_A] = gp.bAnalogButtons[XBOX_BUTTON_B] = 0;
                gp.wButtons &= (unsigned short)~XBOX_GAMEPAD_START;
            } else {
                mask[port] = false;
            }
        }
    }
    const RdrSettings &s = g_rdr_settings;
    if (s.deadzone) {
        st->Gamepad.sThumbLX = deadzone(st->Gamepad.sThumbLX, s.deadzone);
        st->Gamepad.sThumbLY = deadzone(st->Gamepad.sThumbLY, s.deadzone);
        st->Gamepad.sThumbRX = deadzone(st->Gamepad.sThumbRX, s.deadzone);
        st->Gamepad.sThumbRY = deadzone(st->Gamepad.sThumbRY, s.deadzone);
    }
    if (s.invert_cam_x)
        st->Gamepad.sThumbRX = (short)(st->Gamepad.sThumbRX == -32768 ? 32767 : -st->Gamepad.sThumbRX);
    if (s.invert_cam_y)
        st->Gamepad.sThumbRY = (short)(st->Gamepad.sThumbRY == -32768 ? 32767 : -st->Gamepad.sThumbRY);
    /* Keyboard and mouse: the port the game is reading for them -- the
     * menu's in a menu, a lone player's own, else their player's. Worked
     * out once per round of the four ports (at port 0). */
    {
        static int target;
        static int logged = -1;
        static bool seen[4];
        int kbm = s.kbm_player >= 1 && s.kbm_player <= 4 ? s.kbm_player - 1 : 0;
        seen[port] = true;
        if (port == 0) {
            target = rdr_keyboard_port(kbm);
            /* The menus' controller unplugged: they become the keyboard's. */
            if (target != kbm && target >= 0 && target < 4 && seen[target] && !conn[target]
                && rdr_menu_port_take(kbm)) {
                /* (the game hands them back to the START port now and then:
                 * logged once a minute) */
                static uint64_t said;
                uint64_t now = xbox_HostNowNs();
                if (!said || now - said > 60000000000ull) {
                    said = now;
                    fprintf(stderr, "[INPUT] menus: port %d has no controller; the keyboard's (port %d) now\n",
                            target + 1, kbm + 1);
                }
                target = kbm;
            }
        }
        if (port == 0 && target != logged) {
            logged = target;
            fprintf(stderr, "[INPUT] keyboard and mouse: port %d\n", target + 1);
            fflush(stderr);
        }
        if ((int)port == target) {
            if (g_rdr_freecam) {
                /* The free camera: the keyboard's own state, apart from the
                 * controller's -- each flies the camera or plays on. */
                XBOX_INPUT_STATE kb;
                memset(&kb, 0, sizeof kb);
                rdr_input_merge(&kb);
                rdr_freecam_pad(port, st, &kb);
                return;
            }
            rdr_input_merge(st);
        }
    }
    /* The second keyboard and mouse: their player's port (with or without a
     * controller of its own; not while the free camera flies). */
    if ((int)port == rdr_kbm2_port() && !g_rdr_freecam)
        rdr_input_merge2(st);
    /* The same after the keyboard and mouse are in: the Enter or the click
     * that closed the menu (Go to Chapter's second press, say) is still
     * down for a moment, and the keyboard's and mouse's own handling did
     * not see it go down (it does not run while the menu is open), so it
     * passed as a new A: on the desk's book, Begin on the first chapter,
     * before the warp had swapped it (user: Go to Chapter from the main
     * menu went to the first chapter; the log: "menu closed", then
     * lvl_perfnew at once). Held A, B and Start wait for their release. */
    {
        static int seen_gen2[4];
        static bool mask2[4];
        int gen = s_close_gen;
        if (seen_gen2[port] != gen) {
            seen_gen2[port] = gen;
            mask2[port] = true;
        }
        if (mask2[port]) {
            XBOX_GAMEPAD &gp = st->Gamepad;
            bool held = gp.bAnalogButtons[XBOX_BUTTON_A] > 64 || gp.bAnalogButtons[XBOX_BUTTON_B] > 64
                     || (gp.wButtons & XBOX_GAMEPAD_START);
            if (held) {
                gp.bAnalogButtons[XBOX_BUTTON_A] = gp.bAnalogButtons[XBOX_BUTTON_B] = 0;
                gp.wButtons &= (unsigned short)~XBOX_GAMEPAD_START;
            } else {
                mask2[port] = false;
            }
        }
    }
    /* Every port: the controllers set to fly it (Free Camera's choices).
     * The declaration had lost the port argument when the function gained
     * it, so the call passed the state as the port and the camera never
     * moved (user report). */
    rdr_freecam_pad(port, st, NULL);
}

/* ---- Drawing (GPU thread) ---- */

ImGuiContext *s_ctx;
float s_built_scale;
uint64_t s_last_ns;
int s_tab;

void feed_input()
{
    ImGuiIO &io = ImGui::GetIO();
    std::vector<Ev> evs;
    XBOX_INPUT_STATE pad;
    bool pad_ok;
    {
        std::lock_guard<std::mutex> l(s_mu);
        evs.swap(s_events);
        pad = s_pad;
        pad_ok = s_pad_ok;
    }
    for (const Ev &e : evs) {
        switch (e.kind) {
        case Ev::MouseMove: io.AddMousePosEvent(e.x, e.y); break;
        case Ev::MouseButton: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case Ev::Wheel: io.AddMouseWheelEvent(0, e.y); break;
        case Ev::Key: io.AddKeyEvent((ImGuiKey)e.a, e.b != 0); break;
        case Ev::Char: io.AddInputCharacter((unsigned)e.a); break;
        case Ev::Focus: io.AddFocusEvent(false); break;
        }
    }
    /* The pad as ImGui's gamepad (navigation). */
    if (!pad_ok)
        memset(&pad.Gamepad, 0, sizeof pad.Gamepad);
    const XBOX_GAMEPAD &g = pad.Gamepad;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (g.wButtons & XBOX_GAMEPAD_DPAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (g.wButtons & XBOX_GAMEPAD_DPAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (g.wButtons & XBOX_GAMEPAD_DPAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (g.wButtons & XBOX_GAMEPAD_DPAD_RIGHT) != 0);
    bool a_down = g.bAnalogButtons[XBOX_BUTTON_A] > 64;
    if (s_hold_a) {
        if (a_down) a_down = false;
        else s_hold_a = false;
    }
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, a_down);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, g.bAnalogButtons[XBOX_BUTTON_B] > 64);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, g.bAnalogButtons[XBOX_BUTTON_X] > 64);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, g.bAnalogButtons[XBOX_BUTTON_Y] > 64);
    io.AddKeyEvent(ImGuiKey_GamepadL1, g.bAnalogButtons[XBOX_BUTTON_LTRIGGER] > 64);
    io.AddKeyEvent(ImGuiKey_GamepadR1, g.bAnalogButtons[XBOX_BUTTON_RTRIGGER] > 64);
    io.AddKeyEvent(ImGuiKey_GamepadStart, (g.wButtons & XBOX_GAMEPAD_START) != 0);
    auto axis = [&](ImGuiKey neg, ImGuiKey pos, short v) {
        float f = v / 32767.0f;
        io.AddKeyAnalogEvent(neg, f < -0.35f, f < -0.35f ? -f : 0.0f);
        io.AddKeyAnalogEvent(pos, f > 0.35f, f > 0.35f ? f : 0.0f);
    };
    axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, g.sThumbLX);
    axis(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp, g.sThumbLY);
}

void rebuild_fonts(float scale)
{
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    rdrui::Init(scale);
    ImGui::GetIO().Fonts->Build();
    ImGui_ImplOpenGL3_CreateFontsTexture();
    s_built_scale = scale;
}

bool ensure_context(int h)
{
    /* The menu is 930 units tall: the largest eighth that fits the window
     * (fonts are rebuilt only when the step changes). */
    float scale = floorf((float)(h - 20) / 930.0f * 8.0f) / 8.0f;
    if (scale < 0.75f) scale = 0.75f;
    if (scale > 3.0f) scale = 3.0f;
    if (!s_ctx) {
        if (!rdr_gl_load())
            return false;
        IMGUI_CHECKVERSION();
        s_ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(s_ctx);
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.BackendPlatformName = "rdr_overlay";
        /* ImGui's own "conflicting ID" box is for us, not players (RDR_IMGUI_DEBUG=1) */
        io.ConfigDebugHighlightIdConflicts = getenv("RDR_IMGUI_DEBUG") != nullptr;
        rdrui::Init(scale);
        if (!ImGui_ImplOpenGL3_Init("#version 330 core"))
            return false;
        rdrui::CreateTextures();
        game::Upload();              /* the game's own art, when its files are there */
        s_built_scale = scale;
        s_last_ns = xbox_HostNowNs();
    }
    ImGui::SetCurrentContext(s_ctx);
    if (scale != s_built_scale)
        rebuild_fonts(scale);
    return true;
}

void draw_fps(ImDrawList *dl)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%.0f fps", nvgl_present_fps());
    float s = g_scale;
    ImVec2 ts = g_fonts.small->CalcTextSizeA(g_fonts.small->FontSize, FLT_MAX, 0, buf);
    ImVec2 a(14 * s, 12 * s), b(a.x + ts.x + 24 * s, a.y + ts.y + 12 * s);
    dl->AddRectFilled(ImVec2(a.x + 3 * s, a.y + 4 * s), ImVec2(b.x + 3 * s, b.y + 4 * s), IM_COL32(0, 0, 0, 90), 3 * s);
    dl->AddRectFilled(a, b, IM_COL32(222, 202, 160, 235), 3 * s);
    dl->AddRect(a, b, IM_COL32(60, 38, 24, 220), 3 * s, 0, 1.5f * s);
    dl->AddText(g_fonts.small, g_fonts.small->FontSize, ImVec2(a.x + 12 * s, a.y + 6 * s), kInk, buf);
}

/* The version, small in the bottom right corner of the front end (the
 * title screen, the desk, the menus) -- not over a level. */
static void draw_version(ImDrawList *dl, int w, int h)
{
    char buf[64];
    snprintf(buf, sizeof buf, "Red Dead Recompilation %s", rdr_version());
    float s = g_scale, fs = g_fonts.small->FontSize * 0.85f;
    ImVec2 ts = g_fonts.small->CalcTextSizeA(fs, FLT_MAX, 0, buf);
    ImVec2 at(w - ts.x - 14 * s, h - ts.y - 10 * s);
    dl->AddText(g_fonts.small, fs, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 160), buf);
    dl->AddText(g_fonts.small, fs, at, IM_COL32(230, 220, 190, 150), buf);
}

/* a line at the top of the picture: online lockstep's state (rdr_ls_status) */
static void draw_status(ImDrawList *dl, const char *buf, int w)
{
    float s = g_scale;
    ImVec2 ts = g_fonts.small->CalcTextSizeA(g_fonts.small->FontSize, FLT_MAX, 0, buf);
    float x = (w - ts.x - 24 * s) * 0.5f;
    ImVec2 a(x, 12 * s), b(a.x + ts.x + 24 * s, a.y + ts.y + 12 * s);
    dl->AddRectFilled(ImVec2(a.x + 3 * s, a.y + 4 * s), ImVec2(b.x + 3 * s, b.y + 4 * s), IM_COL32(0, 0, 0, 90), 3 * s);
    dl->AddRectFilled(a, b, IM_COL32(222, 202, 160, 235), 3 * s);
    dl->AddRect(a, b, IM_COL32(60, 38, 24, 220), 3 * s, 0, 1.5f * s);
    dl->AddText(g_fonts.small, g_fonts.small->FontSize, ImVec2(a.x + 12 * s, a.y + 6 * s), kInk, buf);
}

void fmt_money(char *buf, size_t n, int v)
{
    char t[32];
    int len, k, o = 0;
    snprintf(t, sizeof t, "%d", v < 0 ? 0 : v);
    len = (int)strlen(t);
    o = snprintf(buf, n, "$");
    for (k = 0; k < len && o + 2 < (int)n; k++) {
        buf[o++] = t[k];
        if ((len - k - 1) % 3 == 0 && k != len - 1)
            buf[o++] = ',';
    }
    buf[o] = 0;
}

/* Money: the amount the HUD shows; left/right on the row add or take the
 * step, the field below takes a typed amount (Enter to set). */
bool money_rows()
{
    static int step_idx = 1;
    static char typed[16] = "";
    static const char *steps[] = { "$100", "$1,000", "$10,000", "$100,000" };
    static const int step_v[] = { 100, 1000, 10000, 100000 };
    int money = rdr_money_get();
    char cur[48];
    if (money < 0) {
        Action("Money", "in a mission", "Money can be changed while playing a mission.");
        return false;
    }
    fmt_money(cur, sizeof cur, money);
    /* The amount as a Choice-style row: arrows step it. */
    {
        int dummy = 1;
        static const char *one[3];
        one[0] = cur; one[1] = cur; one[2] = cur;
        int before = dummy;
        if (Choice("Money", &dummy, one, 3,
                   "Left and right take away or add the step below; A adds it.")) {
            int d = dummy - before;           /* +1 right or A, -1 left */
            if (d == 2) d = -1;               /* wrapped from 0..2 */
            rdr_money_set(money + (d < 0 ? -step_v[step_idx] : step_v[step_idx]));
        }
    }
    Choice("Step", &step_idx, steps, 4, "How much each press adds or takes away.");
    /* Typed amount. */
    {
        float s = g_scale;
        bool gl = game::Ready();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float row_h = gl ? 48 * game::g_space.s : ImGui::GetFontSize() + 12 * s;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float avail = ImGui::GetContentRegionAvail().x;
        float fw = 200 * s, fx = p.x + avail - fw - 14 * s;
        if (gl) {
            /* the field where the value goes, between the arrows' places */
            fw = avail * 0.30f;
            fx = p.x + avail * 0.7525f - fw * 0.5f;
        }
        ImGui::SetCursorScreenPos(ImVec2(fx, p.y + (gl ? (row_h - ImGui::GetFrameHeight()) * 0.5f : 2 * s)));
        if (gl) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(28, 25, 22, 230));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(40, 36, 32, 240));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(50, 30, 24, 240));
            ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(147, 19, 0, 120));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(128, 128, 128, 160));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 228, 220, 255));
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(128, 128, 128, 255));
        } else {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.30f, 0.20f, 0.12f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.30f, 0.20f, 0.12f, 0.20f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.50f, 0.12f, 0.08f, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.55f, 0.12f, 0.08f, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kInkSoft));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kInk));
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(kInkSoft));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f * s);
        ImGui::SetNextItemWidth(fw);
        bool enter = ImGui::InputTextWithHint("##setmoney", "type an amount", typed, sizeof typed,
                                              ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar();
        if (gl)
            game::Text(dl, game::kText, p.x, p.y + row_h * 0.70f, "Set to", 1.5f, 1.2f, hot ? game::kWhite : game::kGrey);
        else
            dl->AddText(ImVec2(p.x, p.y + 6 * s), hot ? kRed : kInk, "Set to");
        if (enter && typed[0]) {
            rdr_money_set(atoi(typed));
            typed[0] = 0;
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
        ImGui::Dummy(ImVec2(1, 1));
    }
    return false;
}

/* Showdown's power-ups, worn while on (rdr_overrides.c gives them). */
static const char *powerup_hint(const char *code)
{
    /* From the manual, powerpac316's Multiplayer FAQ (GameFAQs, 2004) and
     * the game's own effect data (swat.dat's cr_*.arfx: duration, shots). */
    static const struct { const char *code, *hint; } h[] = {
        { "cr_speed_up",     "You run faster." },
        { "cr_invulnerable", "Nothing hurts you." },
        { "cr_ghost",        "You turn almost invisible, hard to see and to hit. In Showdown it ends after 30 seconds or three shots." },
        { "cr_regen_health", "Your health keeps filling back up." },
        { "cr_damagemult",   "Your shots do three times the damage." },
        { "cr_fastbullet",   "Your bullets fly faster, so they land before the target can move." },
        { "cr_fast_reload",  "Reloading is quicker." },
        { "cr_triplecash",   "Bounty money you earn counts three times." },
        { "cr_retrib",       "Retribution: a red-glowing power-up that lasts 30 seconds in Showdown." },
        { "cr_burningrage",  "You burst into flames without being hurt, and set alight anyone who touches you." },
        { "cr_forgedcard",   "Your dynamite can turn into fake power-up cards that explode on whoever picks them up, you included." },
        { "cr_stickybomb",   "Thrown dynamite sticks to whatever or whoever it hits; it cannot be shaken off." },
        { "cr_speed_down",   "You move slower: a Showdown curse." },
        { "cr_pistol_ignite",   "Pistol shots set the target on fire: helpless and burning, and anyone they touch catches fire too." },
        { "cr_pistol_poison",   "Pistol shots poison: damage over time, a shaking controller, and no running until it wears off." },
        { "cr_pistol_knockdown","Pistol shots knock the target off their feet, as if hit by a cannonball." },
        { "cr_dblgun_ignite",   "Twin-revolver shots set the target on fire, and the fire spreads to anyone they touch." },
        { "cr_dblgun_poison",   "Twin-revolver shots poison: damage over time, a shaking controller, no running." },
        { "cr_dblgun_knockdown","Twin-revolver shots knock the target off their feet." },
        { "cr_dblgun_cannon_call", "Like General Diego's special: a hit marks the target, and cannonballs fall on it." },
        { "cr_rifle_ice",       "Rifle shots freeze the target in place for a moment (wiggling the stick breaks out sooner)." },
        { "cr_rifle_piercing",  "Rifle shots throw the target through the air towards you; they land face down, open to a finishing shot." },
        { "cr_shotgun_ignite",  "Shotgun blasts set the target on fire, and the fire spreads to anyone they touch." },
        { "cr_shotgun_explode", "Shotgun blasts explode: huge damage, and the target is blown off their feet." },
        { "cr_arrow_ignite",    "Arrows set the target on fire." },
        { "cr_arrow_explode",   "Arrows explode on impact: a direct hit is almost certain death." },
        { "cr_arrow_piercing",  "Arrows throw the target through the air towards you, landing face down." },
        { "cr_arrow_poison",    "Arrows poison: damage over time, a shaking controller, no running." },
        { "cr_knife_boomerang", "Thrown knives come back to you." },
        { "cr_knife_pickpocket","Thrown knives steal money from whoever they hit." },
        { "cr_knife_southpaw",  "Thrown knives explode where they hit." },
        { "cr_knife_spirit",    "A special thrown knife (Spirit Knife)." },
        { "cr_knife_vampire",   "Thrown knives drain health from whoever they hit and give it to you." },
        { "cr_molotov_explode", "Throwables become nitroglycerine: they explode for huge damage instead of burning." },
        { "cr_molotov_hotcoffee","Bottles splash scalding coffee: whoever is hit hops about in place, helpless, taking damage." },
        { "cr_molotov_ice",     "Bottles freeze whoever they splash for a moment." },
        { "cr_dynamite_explode","Dynamite blows up harder: the Nitroglycerine effect for dynamite." },
        { "gb_drunk",        "Everyone else is drunk: their controls are reversed and their view blurs for a while." },
        { "gb_fast_time",    "The whole game runs faster." },
        { "gb_cannonstrike", "Cannonballs rain down on the arena for a while." },
    };
    for (size_t i = 0; i < sizeof h / sizeof h[0]; i++)
        if (!strcmp(h[i].code, code))
            return h[i].hint;
    return nullptr;
}

static void powerup_toggle(int i)
{
    const RdrPowerup &p = g_rdr_powerups[i];
    const char *what = powerup_hint(p.code);
    int on = (int)(g_rdr_powerup_mask >> i & 1);
    /* what it does under its name; the hint line says how it is worn */
    if (ToggleSub(p.name, &on, what ? what : "The Showdown power-up.",
                  "Worn as if picked up, for as long as it is switched on (a timed one starts again when it runs out).")) {
        if (on) g_rdr_powerup_mask |= 1ull << i;
        else    g_rdr_powerup_mask &= ~(1ull << i);
    }
}

static void powerup_rows()
{
    static bool weapons = false;
    int nweap = 0;
    Section("Showdown power-ups");
    for (int i = 0; i < g_rdr_powerup_count && i < 64; i++)
        if (g_rdr_powerups[i].group == 0)
            powerup_toggle(i);
    for (int i = 0; i < g_rdr_powerup_count && i < 64; i++)
        if (g_rdr_powerups[i].group == 2)
            powerup_toggle(i);
    for (int i = 0; i < g_rdr_powerup_count && i < 64; i++)
        if (g_rdr_powerups[i].group == 1 && (g_rdr_powerup_mask >> i & 1))
            nweap++;
    char v[48];
    if (weapons) snprintf(v, sizeof v, "Hide");
    else if (nweap) snprintf(v, sizeof v, "Show (%d on)", nweap);
    else snprintf(v, sizeof v, "Show");
    if (Action("Weapon power-ups", v,
               "Burning, venomous, explosive and other special ammunition for each weapon."))
        weapons = !weapons;
    if (weapons)
        for (int i = 0; i < g_rdr_powerup_count && i < 64; i++)
            if (g_rdr_powerups[i].group == 1)
                powerup_toggle(i);
    if (g_rdr_powerup_mask &&
        Action("All power-ups off", "Off", "Takes off every power-up switched on here."))
        g_rdr_powerup_mask = 0;
}

/* Go to chapter: the story's chapters, one a row; A twice starts one (the
 * first press asks, as leaving the level loses the mission in hand). */
/* In a game from the online lobby every machine has to stay the same:
 * what would change one machine's game alone (cheats, chapters, co-op,
 * the free camera) waits until the online game ends. */
bool online_locked_note()
{
    if (!rdr_online_session())
        return false;
    Note("You are in an online game: the host's options are used, and cheats, chapters and co-op "
         "can't be changed while you play (every player's game has to stay the same). "
         "Online Co-op > Leave the online game ends it.");
    return true;
}

bool page_chapter()
{
    if (online_locked_note())
        return false;
    if (!rdr_cheats_available()) {
        Note("Pick or make a player in the book first: the chapters live in the player's profile.");
        return false;
    }
    static RdrLevel lv[48];
    static int n = -1, armed = -1;
    static double armed_at;
    if (n < 0) {
        int all = rdr_level_list(1, lv, 48);
        n = 0;
        for (int i = 0; i < all; i++)
            if (strncmp(lv[i].label, "Unused", 6))
                lv[n++] = lv[i];
    }
    double now = ImGui::GetTime();
    if (armed >= 0 && now - armed_at > 5.0)
        armed = -1;
    for (int i = 0; i < n; i++) {
        ImGui::PushID(i);
        bool is_armed = armed == i;
        const char *v = rdr_warp_pending() ? "on the way..." : is_armed ? "Press again to start" : "";
        if (Action(lv[i].label, v,
                   is_armed ? "Press again to leave this level and start the chapter. Your progress is kept."
                            : "Starts this chapter straight away (the level you are in is left; your "
                              "progress and profile are kept).")
            && !rdr_warp_pending()) {
            if (is_armed) {
                armed = -1;
                rdr_warp_request(lv[i].name, lv[i].var);
                set_open(false);
            } else {
                armed = i;
                armed_at = now;
            }
        }
        ImGui::PopID();
    }
    return false;
}

/* Play As: player 1's character (from the next level). */
bool rows_play_as()
{
    {
        static const char *names[RDR_MAX_CHARACTERS];
        int n = g_rdr_character_count;
        for (int i = 0; i < n; i++) names[i] = g_rdr_characters[i].name;
        int pa = g_rdr_settings.play_as;
        if (Picker("Play as", &pa, names, n,
                   "Story levels start as this character: the story's own, a Showdown one, or one "
                   "a mod adds (the 2002 preview's: launcher > Extras). A opens the list to search. "
                   "From the next level or Restart Level.")) {
            g_rdr_settings.play_as = pa;
            g_rdr_settings.play_as_code[0] = 0;
            rdr_settings_save(&g_rdr_settings);
        }
    }
    return false;
}

/* Story co-op: who else plays, and as whom (from the next level). */
bool rows_coop()
{
    static const char *players[] = { "Off", "2 players", "3 players", "4 players" };
    static const char *screens[] = { "Shared", "Split: top/bottom", "Split: side by side", "Separate windows" };
    int np = g_rdr_settings.coop_players - 1, sp = g_rdr_settings.coop_split;
    if (np < 0 || np > 3) np = 0;
    if (Choice("Co-op players", &np, players, 4,
               "Friends join the story as more players, each on their own controller "
               "(Controls page: who holds which; a player with none takes the next free "
               "one). From the next level or Restart Level (pause menu); a third or fourth "
               "controller is plugged in at the next start.")) {
        g_rdr_settings.coop_players = np + 1;
        rdr_settings_save(&g_rdr_settings);
        ApplyLive(&g_rdr_settings);     /* players without a controller get one */
    }
    if (np > 0) {
        /* Who each joins as: the same list as Play as (player 1's). */
        static const char *names[RDR_MAX_CHARACTERS];
        int n = g_rdr_character_count;
        for (int i = 0; i < n; i++) names[i] = g_rdr_characters[i].name;
        names[0] = "Same as player 1";
        for (int k = 1; k <= np && k < 4; k++) {
            char label[24];
            snprintf(label, sizeof label, "Player %d as", k + 1);
            int c = g_rdr_settings.coop_char[k];
            if (c < 0 || c >= n) c = 0;
            ImGui::PushID(k);
            if (Picker(label, &c, names, n,
                       "This player's character: one of the story's own or a Showdown one "
                       "(player 1's is Play as, above). From the next level or Restart Level.")) {
                g_rdr_settings.coop_char[k] = c;
                g_rdr_settings.coop_code[k][0] = 0;
                rdr_settings_save(&g_rdr_settings);
            }
            ImGui::PopID();
        }
        names[0] = "Story default";
    }
    if (np > 0 && Choice("Screen", &sp, screens, 4,
                         "Shared: everyone on player 1's camera. Split: a view each "
                         "(three or four players: quarters). Separate windows: player 1 in the "
                         "game's window, each other player in a window of their own (drag it to "
                         "another monitor; F11 or a double click there fills that monitor). "
                         "Changes at once.")) {
        g_rdr_settings.coop_split = sp;
        rdr_settings_save(&g_rdr_settings);
    }
    static const char *loadouts[] = { "Shared", "Each player's own" };
    if (np > 0 && Choice("Weapons and money", &g_rdr_settings.coop_loadout, loadouts, 2,
                         "Shared: one purse. What anyone earns is player 1's to spend in the shop, and "
                         "player 1's weapons and loadout go to every player. Each player's own: players "
                         "2-4 keep their own money and weapons from chapter to chapter and buy on the Co-op "
                         "Shop page (PC Options, or the Quick menu)."))
        rdr_settings_save(&g_rdr_settings);
    static const char *duels[] = { "Player 1", "Every player" };
    if (np > 0 && Choice("Duels", &g_rdr_settings.coop_duels, duels, 2,
                         "Player 1: the story's duels are player 1's, as in a one-player game, and the "
                         "others wait. Every player: when player 1 duels, players 2-4 duel the same "
                         "opponent at the same time: draw and shoot together; the first good shot wins."))
        rdr_settings_save(&g_rdr_settings);
    if (np > 0 && Toggle("Friendly fire", &g_rdr_settings.coop_friendly_fire,
                         "Off: the players' bullets do not hurt each other (enemies "
                         "still get hit). On: as in Showdown, everyone can shoot everyone."))
        rdr_settings_save(&g_rdr_settings);
    if (rdr_coop_changed() && Action("Apply now", "Restart chapter",
                                     "Starts this chapter again with these players (as Go to Chapter "
                                     "does: progress in the chapter is lost). Restart Level does it too.")) {
        rdr_settings_save(&g_rdr_settings);
        if (rdr_coop_apply_now())
            set_open(false);
    }
    return false;
}

/* The F1 menu's page has both; the book has one of each. */
bool page_coop()
{
    if (online_locked_note())
        return false;
    rows_play_as();
    rows_coop();
    return false;
}

static bool freecam_rows();
bool page_cheats()
{
    if (online_locked_note())
        return false;
    if (!rdr_cheats_available()) {
        Note("Pick or make a player in the book first: the cheats live in the player's profile.");
        return false;
    }
    Section("The game's cheats");
    for (int i = 0; i < g_rdr_cheat_count; i++) {
        const RdrCheat &c = g_rdr_cheats[i];
        int on = rdr_cheat_get(c.bit);
        if (Toggle(c.name, &on, c.what))
            rdr_cheat_set(c.bit, on);
    }

    Section("Player");
    Toggle("Infinite Ammo", &g_rdr_infinite_ammo,
           "Clips and reserves stay full: no reloading, the count never drops.");
    Toggle("Always Win Duels", &g_rdr_duel_win,
           "In a duel the opponent draws and fires, but always misses: take your time and win. "
           "Story duels and Showdown duels against the computer.");
    Toggle("Always Excellent", &g_rdr_always_excellent,
           "Every mission's results screen rates you Excellent, with what that unlocks.");
    money_rows();

    Section("World");
    Toggle("Freeze NPCs", &g_rdr_freeze_npcs,
           "Everyone else stops where they stand: they do not move, shoot or notice you. "
           "Switch it off again for missions that need someone to move.");
    if (Toggle("Guns in Town", &g_rdr_settings.town_guns,
               "Draw and shoot where the story keeps your guns holstered: Brimstone's streets, "
               "shops and saloon. The townsfolk can be shot, as in the fight with Mr. Kelley. "
               "Kept with your settings; switch it off before a scene that needs them alive."))
        rdr_settings_save(&g_rdr_settings);
    if (g_rdr_settings.town_guns
        && Toggle("Townsfolk Come Back", &g_rdr_settings.town_respawn,
                  "With Guns in Town: Brimstone's people who are shot dead (the shopkeepers, the "
                  "paper boy and the rest) come back at their own places about a minute later."))
        rdr_settings_save(&g_rdr_settings);
    if (Toggle("Protect Allies", &g_rdr_settings.protect_friendly,
               "The people on your side or not in the fight take no damage: the barmaids in the "
               "saloon fight, the ones you escort or guard, the townsfolk. Enemies and the players "
               "are not changed. Story levels only."))
        rdr_settings_save(&g_rdr_settings);
    {
        int fc = g_rdr_freecam;
        if (Toggle("Free Camera (F6)", &fc,
                   "Fly the camera anywhere: left stick / WASD move, right stick / mouse look, "
                   "RT or Space up, LT or R down, left stick click / Shift faster. Below, while "
                   "it is on: the keyboard and each player's controller fly it or play on."))
            g_rdr_freecam = fc;
        if (freecam_rows())
            rdr_settings_save(&g_rdr_settings);
    }
    if (Slider("Enemy health", &g_rdr_settings.enemy_health, 100, 500, 25, "%d%%",
               "Story enemies take this much more to bring down (200%: twice the shooting): they "
               "take less damage, so their health bars and the bosses' tricks work as ever. "
               "Showdown is not changed."))
        rdr_settings_save(&g_rdr_settings);

    powerup_rows();

    Section("Unlocks");
    {
        /* Two clicks: the first arms it for five seconds. */
        static int done = -2;
        static double armed_at = -1;
        double now = ImGui::GetTime();
        bool armed = armed_at >= 0 && now - armed_at < 5.0;
        const char *v = armed ? "Click again to confirm"
                      : done == -2 ? "Unlock" : done < 0 ? "Not ready" : "Done";
        if (Action("Unlock Everything", v,
                   "Every Showdown character and stage, every journal page in the Extras and the "
                   "game's cheats, kept with your saves. Asks again before it acts; Undo below "
                   "locks exactly these again.")) {
            if (armed) {
                done = rdr_unlock_everything();
                armed_at = -1;
            } else {
                armed_at = now;
            }
        }
        if (rdr_unlock_can_undo()) {
            static double undo_armed = -1;
            bool uarmed = undo_armed >= 0 && now - undo_armed < 5.0;
            if (Action("Undo Unlock Everything", uarmed ? "Click again to confirm" : "Undo",
                       "Locks again what Unlock Everything unlocked; what you had before, or "
                       "earned since, stays.")) {
                if (uarmed) {
                    rdr_unlock_undo();
                    undo_armed = -1;
                    done = -2;
                } else {
                    undo_armed = now;
                }
            }
        }
    }
    ImGui::Dummy(ImVec2(1, 8 * g_scale));
    Note("The game's cheats are its own, as its pause menu switches them; they stay with the "
         "player until switched off.");
    return false;
}


/* A row with a text field (a name, a join code): the label at the left,
 * the field where a value goes. True when Enter was pressed in it. */
bool text_row(const char *id, const char *label, char *buf, size_t n, const char *empty_hint)
{
    float s = g_scale;
    bool gl = game::Ready();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float row_h = gl ? 48 * game::g_space.s : ImGui::GetFontSize() + 12 * s;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float avail = ImGui::GetContentRegionAvail().x;
    float fw = 260 * s, fx = p.x + avail - fw - 14 * s;
    if (gl) {
        fw = avail * 0.40f;
        fx = p.x + avail * 0.7525f - fw * 0.5f;
    }
    ImGui::SetCursorScreenPos(ImVec2(fx, p.y + (gl ? (row_h - ImGui::GetFrameHeight()) * 0.5f : 2 * s)));
    if (gl) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(28, 25, 22, 230));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(40, 36, 32, 240));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(50, 30, 24, 240));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(147, 19, 0, 120));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(128, 128, 128, 160));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 228, 220, 255));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(128, 128, 128, 255));
    } else {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.30f, 0.20f, 0.12f, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.30f, 0.20f, 0.12f, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.50f, 0.12f, 0.08f, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.55f, 0.12f, 0.08f, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(kInkSoft));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kInk));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(kInkSoft));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f * s);
    ImGui::SetNextItemWidth(fw);
    bool enter = ImGui::InputTextWithHint(id, empty_hint, buf, n, ImGuiInputTextFlags_EnterReturnsTrue);
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
    ImGui::PopStyleColor(7);
    ImGui::PopStyleVar();
    /* With a controller alone: Y on the field brings up letters to pick. */
    {
        char pop[64];
        snprintf(pop, sizeof pop, "##padkeys%s", id);
        if (focused && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)
                        || (ImGui::IsItemActivated() && ImGui::GetIO().NavVisible
                            && ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown)))) {
            ImGui::ClearActiveID();
            ImGui::OpenPopup(pop);
        }
        if (ImGui::BeginPopup(pop)) {
            static const char *rows[] = { "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "0123456789-._", "abcdefghijklm",
                                          "nopqrstuvwxyz" };
            size_t len = strlen(buf);
            ImGui::TextUnformatted(buf[0] ? buf : "(empty)");
            for (int r = 0; r < 5; r++) {
                for (int c = 0; rows[r][c]; c++) {
                    char key[2] = { rows[r][c], 0 };
                    if (c) ImGui::SameLine();
                    ImGui::PushID(r * 32 + c);
                    if (ImGui::Button(key, ImVec2(ImGui::GetFrameHeight() * 1.2f, 0)) && len + 1 < n) {
                        buf[len++] = rows[r][c];
                        buf[len] = 0;
                    }
                    ImGui::PopID();
                }
            }
            if (ImGui::Button("Space") && len + 1 < n) { buf[len++] = ' '; buf[len] = 0; }
            ImGui::SameLine();
            if (ImGui::Button("Delete") && len) buf[--len] = 0;
            ImGui::SameLine();
            if (ImGui::Button("Done") || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) {
                ImGui::CloseCurrentPopup();
                enter = buf[0] != 0;
            }
            ImGui::EndPopup();
        }
    }
    if (gl)
        game::Text(dl, game::kText, p.x, p.y + row_h * 0.70f, label, 1.5f, 1.2f, hot ? game::kWhite : game::kGrey);
    else
        dl->AddText(ImVec2(p.x, p.y + 6 * s), hot ? kRed : kInk, label);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
    ImGui::Dummy(ImVec2(1, 1));
    return enter;
}

/* Online Co-op: the lobby (src/net/rdr_lobby.c). Host or join; the host
 * gives out a join code; when everyone has the host's save, the host
 * starts and every machine starts the game again together. Opened from the
 * desk's "Online Co-op" (Y, where the Xbox had its Friends Menu) or here. */
bool page_online()
{
    static int mode = 0, players = 0;
    static char name[33] = "", code[64] = "";
    static const char *modes[] = { "Host a game", "Join a game" };
    static const char *counts[] = { "2 players", "3 players", "4 players" };
    RdrLobbyView v;
    if (rdr_online_session()) {
        Note("You are playing online. Everyone's game runs the same: the host's save and options, "
             "each player on their own controller. The line at the top says when someone falls behind.");
        if (Action("Leave the online game", "Leave",
                   "Ends the online game here and starts your own game again, with your own saves "
                   "and settings (the others' games stop too: they wait for you)."))
            rdr_online_leave_session();
        return false;
    }
    if (!name[0]) {
        const char *u = getenv("USERNAME");
        if (!u) u = getenv("USER");
        snprintf(name, sizeof name, "%s", u && *u ? u : "Player");
    }
    rdr_lobby_view(&v);
    if (v.state == 0 || v.state == 5) {
        Choice("Online co-op", &mode, modes, 2,
               "Play the story together over the internet or a LAN: every player runs the game, "
               "with the host's save and options; each is a player in Story Co-op. Experimental.");
        text_row("##lbname", "Your name", name, sizeof name, "your name");
        if (mode == 0) {
            Choice("Players", &players, counts, 3, "How many will play, you included.");
            if (Toggle("Internet address in the code", &g_rdr_settings.online_share_ip,
                       "On: the code also holds this PC's internet (public IP) address, for friends "
                       "joining over the internet with UDP port 27032 forwarded. Anyone with the "
                       "code can read that address, so give it only to people you trust. Off: only "
                       "home-network and VPN addresses (Tailscale, ZeroTier), which strangers cannot "
                       "reach or use to find you."))
                rdr_settings_save(&g_rdr_settings);
            if (Action("Host", "Open the game",
                       "Shows a join code to give the others. It works at home, over a VPN "
                       "(Tailscale, ZeroTier), and over the internet when your router forwards UDP "
                       "port 27032. Everyone plays on a copy of your save; their own saves are not touched."))
                rdr_lobby_host(players + 2, name);
        } else {
            bool go = text_row("##lbcode", "Join code", code, sizeof code, "the host's code (Y: with the controller)");
            if (Action("Join", "Join the game",
                       "The host's join code (they see it when they host), or their address. You "
                       "need the game, the same version of the port as the host.") || go)
                rdr_lobby_join(code, name);
        }
        if (v.status[0])
            Note(v.status);
        return false;
    }
    if (v.state == 1) {
        int pc = v.players - 2;
        if (Choice("Players", &pc, counts, 3, "How many will play, you included."))
            rdr_lobby_set_players(pc + 2);
        Section("Join code");
        if (v.ncodes == 0)
            Note("Finding this PC's addresses...");
        else {
            if (Action("Join code", v.codes[0],
                       "Give this code to your friends; A or a click copies it. It holds every "
                       "address this PC can be reached at: their game tries them all."))
                ImGui::SetClipboardText(v.codes[0]);
            if (v.reach[0])
                Note(v.reach);
        }
    }
    Section("Players");
    for (int k = 0; k < v.players && k < 4; k++) {
        char label[48], val[64];
        snprintf(label, sizeof label, "Player %d", k + 1);
        if (!v.present[k])
            snprintf(val, sizeof val, "waiting...");
        else
            snprintf(val, sizeof val, "%s%s", v.names[k],
                     k == 0 ? " (host)" : v.ready[k] == 1 ? " - ready"
                     : v.ready[k] == 2 ? " - needs the 2002 preview"
                     : v.ready[k] == 3 ? " - different mods" : " - getting the save");
        ImGui::PushID(100 + k);
        Action(label, val, k == v.me ? "You." : "");
        ImGui::PopID();
    }
    if (v.state == 1) {
        static const char *starts[] = { "The menus (by hand)", "Continue the story", "A new game" };
        int sm = rdr_lobby_get_start();
        if (Choice("Start in", &sm, starts, 3,
                   "Where everyone's game goes once it starts again. Continue and New go there by "
                   "themselves (past the title, the desk and the book, straight into loading) "
                   "with your save's profile (the one you played last); nobody needs to press "
                   "anything. The menus: you work them with your controller, as before."))
            rdr_lobby_set_start(sm);
        bool can = rdr_lobby_can_start() != 0;
        if (Action("Start", can ? "Start the game" : "Waiting for everyone",
                   "Everyone's game starts again, together, into Story Co-op (see Start in). Your "
                   "progress in this game now is left; the online game plays on a copy of your "
                   "save.") && can)
            rdr_lobby_start();
    }
    if (Action("Leave", v.state == 1 ? "Close the game" : "Leave the game", "Back to choosing."))
        rdr_lobby_leave();
    if (v.status[0])
        Note(v.status);
    return false;
}

/* Give Weapon (on the Spawner page): any of the game's weapons, to the
 * player chosen there (online: to you, with your input). The list is the
 * game's own (swat.guns), names tidied; the mounted gatling guns and the
 * level-only copies are left out. */
static void weapon_label(const char *raw, char *out, size_t n)
{
    size_t k = 0;
    int up = 0;
    size_t len = strlen(raw);
    if (len > 2 && !strcmp(raw + len - 2, "_u")) { up = 1; len -= 2; }
    for (size_t i = 0; i < len && k + 1 < n; i++)
        out[k++] = raw[i] == '_' ? ' ' : (i == 0 ? (char)toupper((unsigned char)raw[i]) : raw[i]);
    out[k] = 0;
    if (up && k + 12 < n) strcat(out, " (upgraded)");
}

static bool weapon_listed(const char *raw)
{
    return raw[0] && strncmp(raw, "gatlinggun", 10) && strcmp(raw, "carlos_rifle") && strcmp(raw, "dynamite_02")
           && strcmp(raw, "Rifle_Testudine");
}

void weapon_rows(int player)
{
    static char names[96][48];
    static const char *items[96];
    static int idx[96], n = -1, pick = 0, count_at = -1;
    int count = rdr_weapon_count();
    Section("Weapons");
    if (count <= 0) {
        Note("The weapons list is read from the level: start one first.");
        return;
    }
    if (count != count_at) {
        n = 0;
        for (int i = 0; i < count && n < 96; i++) {
            char raw[48];
            if (!rdr_weapon_name(i, raw, sizeof raw) || !weapon_listed(raw))
                continue;
            weapon_label(raw, names[n], sizeof names[n]);
            items[n] = names[n];
            idx[n] = i;
            n++;
        }
        count_at = count;
        if (pick >= n) pick = 0;
    }
    if (!n)
        return;
    Picker("Weapon", &pick, items, n, "Any of the game's weapons: A opens the list to search, left/right steps.");
    bool has = rdr_weapon_owned(player, idx[pick]);
    char hint[160];
    snprintf(hint, sizeof hint, "Adds %s to %s weapons, with its ammunition: change to it as to any other.",
             names[pick], player ? "that player's" : "your");
    if (Action("Give weapon", has ? "Already carried" : "Give", hint) && !has)
        rdr_give_weapon_ask(idx[pick], player);
    if (Action("Fill ammunition", "Fill", player ? "Fills the clips and reserves of every weapon that player carries."
                                                 : "Fills the clips and reserves of every weapon you carry.")) {
        rdr_fill_ammo_ask(player);
        rdr_ui_sound(RDR_SOUND_ACCEPT);
    }
}

/* The Co-op Shop: story co-op with each player's own weapons and money --
 * any player buys weapons at the game's prices with their own money. A
 * sale is a command (online: with your input, on every machine). */
extern "C" int rdr_ls_my_port(void);
bool page_coop_shop()
{
    static int who = 0;
    static const char *players[] = { "Player 1", "Player 2", "Player 3", "Player 4" };
    bool online = rdr_online_session() != 0;
    int np = rdr_coop_count();
    if (g_rdr_settings.coop_loadout != 1) {
        /* (an empty page read as broken -- user) */
        static const char *loadouts[] = { "Shared", "Each player's own" };
        if (!online && Choice("Weapons and money", &g_rdr_settings.coop_loadout, loadouts, 2,
                              "Shared: player 1 buys for everyone in the game's own stores. Each player's "
                              "own: every player buys here with their own money."))
            rdr_settings_save(&g_rdr_settings);
        Note(online ? "Nothing to buy here: weapons and money are shared in this game (the host's Story "
                      "Co-op settings), so player 1 buys for everyone in the game's own stores."
                    : "Nothing to buy here while weapons and money are shared: player 1 buys for everyone "
                      "in the game's own stores. Set \"Weapons and money\" above to \"Each player's own\" "
                      "and each player's weapons and prices show here.");
        return false;
    }
    if (!rdr_in_level() || np < 2) {
        Note("Here each player of a story co-op game buys weapons with their own money, once a chapter "
             "with two or more players is under way.");
        return false;
    }
    if (online) {
        who = rdr_ls_my_port();
        if (who < 0 || who > 3) who = 0;
    } else {
        if (who >= np) who = 0;
        Choice("Player", &who, players, np > 4 ? 4 : np, "Who is buying.");
    }
    char v[48];
    int money = rdr_coop_player_money(who);
    snprintf(v, sizeof v, "$%d", money);
    Action(online ? "Your money" : "Money", v, "What this player has to spend: their own, kept between chapters.");
    Section("Weapons");
    static char hints[96][96];
    int count = rdr_weapon_count(), shown = 0;
    for (int i = 0; i < count && i < 96; i++) {
        char raw[48], label[64];
        int price = rdr_weapon_price(i);
        if (price < 0 || !rdr_weapon_name(i, raw, sizeof raw) || !weapon_listed(raw))
            continue;
        weapon_label(raw, label, sizeof label);
        bool has = rdr_weapon_owned(who, i);
        snprintf(v, sizeof v, has ? "Carried" : "$%d", price);
        snprintf(hints[i], sizeof hints[i], has ? "Already carried." : money >= price ? "Buys it for $%d." :
                                                  "Costs $%d: not enough money yet.", price);
        ImGui::PushID(i);
        if (Action(label, v, hints[i]) && !has && money >= price) {
            rdr_coop_buy_ask(i, who);
            rdr_ui_sound(RDR_SOUND_ACCEPT);
        }
        ImGui::PopID();
        shown++;
    }
    if (!online && Action("Start players 2-4 over", "Clear",
                          "Players 2-4's money and weapons kept between chapters are cleared (as for a new "
                          "story). Player 1's are the profile's, as ever."))
        rdr_coop_loadouts_clear();
    return false;
}

/* New people (rdr_npc.c): anyone -- the story's and Showdown's characters,
 * the preview's and the mods' -- made on the spot, acting as one of this
 * level's people does (that person's AI), on the side chosen. */
static void new_people_rows_in(int player, const char *front);
/* Its rows share labels with the level's people above ("How many"): in an
 * ID scope of their own, or ImGui takes them for one item (user
 * screenshot: "conflicting ID" over How many). */
static void new_people_rows(int player, const char *front)
{
    ImGui::PushID("newpeople");
    new_people_rows_in(player, front);
    ImGui::PopID();
}

static void new_people_rows_in(int player, const char *front)
{
    static RdrNpcAct acts[64];
    static int na = 0, who = 0, act = 0, side = 0;
    static double at = -10.0;
    enum { BEASTS_MAX = 32 };
    static char names[64 + BEASTS_MAX + RDR_MAX_CHARACTERS][56];
    static const char *items[64 + BEASTS_MAX + RDR_MAX_CHARACTERS];
    static int codes[64 + BEASTS_MAX + RDR_MAX_CHARACTERS];
    enum { OTHERS_MAX = 256 };
    static const char *act_items[64 + OTHERS_MAX];
    static char act_names[64 + OTHERS_MAX][72];
    double now = ImGui::GetTime();
    if (now - at > 1.0) {
        na = rdr_npc_acts(acts, 64);
        at = now;
    }
    Section("New people (experimental)");
    int ni = 0, np = 0;
    for (int k = 0; k < na; k++) {
        snprintf(names[ni], sizeof names[ni], "%s (this level%s)", acts[k].name, acts[k].animal ? ", animal" : "");
        items[ni] = names[ni];
        codes[ni++] = 1000 + k;
        if (!acts[k].animal)
            np = k + 1;                 /* the people come first */
    }
    /* other chapters' animals */
    for (int k = 0; k < rdr_npc_beasts_count() && k < BEASTS_MAX; k++) {
        const char *where, *nm;
        if (!rdr_npc_beast_info(k, &where, &nm))
            continue;
        snprintf(names[ni], sizeof names[ni], "%s (%s, animal)", nm, where);
        items[ni] = names[ni];
        codes[ni++] = 2000 + k;
    }
    for (int k = 1; k < g_rdr_character_count && ni < 64 + BEASTS_MAX + RDR_MAX_CHARACTERS; k++) {
        if (!g_rdr_characters[k].code || !g_rdr_characters[k].code[0])
            continue;
        snprintf(names[ni], sizeof names[ni], "%s", g_rdr_characters[k].name);
        for (char *c = names[ni]; *c; c++)
            if (*c == '\n') *c = ' ';
        items[ni] = names[ni];
        codes[ni++] = k;
    }
    for (int k = 0; k < np; k++) {
        snprintf(act_names[k], sizeof act_names[k], "Like %s (%s)", acts[k].name,
                 acts[k].enemy ? "fights you" : "does not fight you");
        act_items[k] = act_names[k];
    }
    /* then the story's other levels' people (their ways come with them) */
    int nacts = np, no = rdr_npc_others_count();
    for (int k = 0; k < no && k < OTHERS_MAX; k++) {
        const char *where, *ty, *nm;
        int tm, ar;
        if (!rdr_npc_other_info(k, &where, &ty, &tm, &ar))
            continue;
        nm = rdr_type_name(ty);
        snprintf(act_names[nacts], sizeof act_names[nacts], "Like %s, %s (%s)", nm ? nm : ty, where,
                 tm == 1 ? "fights you" : ar ? "armed" : "does not fight you");
        for (char *c = act_names[nacts]; *c; c++)
            if (*c == '\n') *c = ' ';
        act_items[nacts] = act_names[nacts];
        nacts++;
    }
    if (who >= ni) who = 0;
    if (act >= nacts) act = 0;
    Picker("Character", &who, items, ni,
           "Who to make: one of this level's people or animals, or anyone from the story, Showdown, the "
           "preview or the mods. A to search the list.");
    bool beast = codes[who] >= 2000 || (codes[who] >= 1000 && acts[codes[who] - 1000].animal);
    static const char *sides[] = { "As that person", "Against you", "On your side" };
    if (!ni) {
        Note("Nobody to make.");
        return;
    }
    if (beast)
        Note(codes[who] >= 2000 ? "Animals act as they do in their own chapter." :
                                  "Animals act as this level's own do.");
    else if (!nacts)
        Note("This level has only animals to copy: pick one of them above.");
    else {
        Picker("Acts like", &act, act_items, nacts,
               "How they behave: as this person does (their walking about, fighting, talking). The list "
               "has this level's people first, then the other chapters'. Someone from another chapter may need "
               "things this level does not have: if they do nothing, try another. A to search.");
        Choice("Side", &side, sides, 3,
               "As that person: their own side. Against you: they fight the players. On your side: they "
               "fight the players' enemies.");
    }
    static int count_i = 0, place = 0, arms = 0;
    static const char *counts[] = { "1", "2", "3", "5", "8" };
    static const int count_v[] = { 1, 2, 3, 5, 8 };
    static const char *places[] = { "In front", "All around", "Further off" };
    static const char *armss[] = { "As their side", "None", "Armed" };
    Choice("How many", &count_i, counts, 5, "How many to make at a time.");
    Choice("Where", &place, places, 3,
           "In front: a few steps ahead, in rows of three. All around: in a ring around the player. "
           "Further off: some way ahead, to see them come.");
    static int boss = 0;
    static const char *offon[] = { "Off", "On" };
    if (!beast) {
        Choice("Guns", &arms, armss, 3,
               "As their side: armed when against you or on your side, unarmed otherwise. None: no guns. "
               "Armed: guns whatever their side.");
        Choice("Health bar", &boss, offon, 2,
               "On: each one is a boss, with their name and health bar on screen, as the level's bosses "
               "have.");
    }
    static char hint[260];
    int room = rdr_npc_room();
    snprintf(hint, sizeof hint, "Makes them, %s. New: if someone does nothing, try another \"Acts like\". "
             "Room now for %d more (a level holds only so many people and animals at once).", front, room);
    if (g_rdr_npc_note[0])
        Note(g_rdr_npc_note);
    if ((beast || nacts) && Action("Make", "Make", hint)) {
        g_rdr_npc_note[0] = 0;
        rdr_npc_opts_ask(count_v[count_i], place, arms | (boss && !beast ? 0x10 : 0), player);
        rdr_npc_other_ask(!beast && act >= np ? act - np : -1, player);
        rdr_npc_ask(codes[who], !beast && act >= np ? 0 : act, side, player);
        rdr_ui_sound(RDR_SOUND_ACCEPT);
    }
    if (Action("Remove the people made", "Remove",
               "Everyone made here goes away (the level's own people stay).")) {
        rdr_npc_remove_ask(player);
        rdr_ui_sound(RDR_SOUND_ACCEPT);
    }
}

/* Spawner: the level's own characters that are waiting to come in (the
 * game loads a level's people with it; those the story has not brought in
 * yet wait), brought in now in front of a player. In co-op any player's
 * menu can bring them in; online the request goes with that player's
 * input, so every machine brings in the same ones at the same moment. */
bool page_spawner()
{
    static int count_i = 0, who = 0;
    static const char *counts[] = { "1", "2", "3", "5", "All waiting" };
    static const int count_v[] = { 1, 2, 3, 5, 64 };
    static const char *players[] = { "Player 1", "Player 2", "Player 3", "Player 4" };
    static RdrSpawnKind kinds[48];
    static int n = 0;
    static double at = -10.0;
    if (!rdr_in_level()) {
        Note("Start a chapter (or a Showdown) first: the spawner brings in the people of the level "
             "you are playing.");
        return false;
    }
    double now = ImGui::GetTime();
    if (now - at > 0.5) {               /* the counts change as they come in */
        n = rdr_spawn_kinds(kinds, 48);
        at = now;
    }
    bool online = rdr_online_session() != 0;
    int np = rdr_coop_count();
    if (!online && np >= 2) {
        if (who >= np) who = 0;
        Choice("Player", &who, players, np > 4 ? 4 : np,
               "Who the people come in in front of, and who is given the weapon.");
    } else
        who = 0;
    weapon_rows(online ? 0 : who);
    Section("People");
    Choice("How many", &count_i, counts, 5, "How many to bring in with each press.");
    int total = 0;
    for (int i = 0; i < n; i++)
        total += kinds[i].waiting;
    if (!n) {
        Note("Nobody in this level is waiting to come in: everyone the level has is already here.");
        new_people_rows(online ? 0 : who, online ? "in front of you" : np >= 2 ? "in front of the player chosen above"
                                                                            : "in front of you");
        return false;
    }
    char v[48];
    snprintf(v, sizeof v, total ? "%d waiting" : "none waiting", total);
    const char *front = online ? "in front of you" : np >= 2 ? "in front of the player chosen above"
                                                             : "in front of you";
    /* (a row keeps its hint's pointer for the line under the list: one each) */
    static char hints[49][200];
    char *hint = hints[48];
    snprintf(hint, 200, "Brings in anyone waiting in this level, %s.", front);
    if (Action("Anyone", v, hint))
        rdr_spawn_ask(-1, count_v[count_i], online ? 0 : who);
    for (int i = 0; i < n; i++) {
        char label[64];
        ImGui::PushID(i);
        snprintf(label, sizeof label, "%s (%s)", kinds[i].name,
                 kinds[i].animal ? "animal" : kinds[i].enemy ? "enemy" : "friendly");
        snprintf(v, sizeof v, kinds[i].waiting ? "%d waiting" : "none waiting", kinds[i].waiting);
        snprintf(hints[i], 200, "%s %s. %s", kinds[i].animal ? "An animal (a horse can be ridden); comes in"
                                             : kinds[i].enemy ? "Fights you; comes in" : "Does not fight you; comes in",
                 front, kinds[i].waiting ? "" : "Everyone of this kind is here already.");
        if (Action(label, v, hints[i]) && kinds[i].waiting)
            rdr_spawn_ask(i, count_v[count_i], online ? 0 : who);
        ImGui::PopID();
    }
    Note("Only the people this level has loaded can come in: each level has its own, and once "
         "everyone is here there is nobody left to bring in.");
    new_people_rows(online ? 0 : who, front);
    return false;
}

/* ---- The menu in the game's art (rdr_ui_game) --------------------------
 *
 * F1 opens a list drawn as the game's pause menu (the picture dimmed, the
 * two scroll rules, berrylium centred); each entry opens a page drawn as
 * its options pages (the black page, the red title, rows with triangles and
 * the slider's X, the hint, the A/B prompts). B or Esc goes back a step. */

struct GamePage {
    const char *title;
    bool (*draw)();
};
bool page_display() { return PageDisplay(&g_rdr_settings, true); }
bool page_graphics() { return PageGraphics(&g_rdr_settings, true); }
bool page_controls() { return PageControls(&g_rdr_settings, true); }
bool page_extras() { return PageExtras(&g_rdr_settings, true); }
bool page_mods() { return PageMods(&g_rdr_settings, true); }
bool page_book_play_as() { return rows_play_as(); }
bool page_book_coop() { return rows_coop(); }
const GamePage kGamePages[] = {
    { "Go to Chapter", page_chapter },
    { "Cheats", page_cheats },
    { "Spawner", page_spawner },
    { "Co-op Shop", page_coop_shop },
    { "Story Co-op", page_coop },
    { "Online Co-op", page_online },
    { "Display", page_display },
    { "Graphics", page_graphics },
    { "Controls", page_controls },
    { "Mods", page_mods },
    { "Extras", page_extras },
    /* opened from the chapter book's own items, not in the list */
    { "Play As", page_book_play_as },
    { "Story Co-op", page_book_coop },
};
const int kGamePageCount = 11;  /* the list's; the book's follow */
int s_view = -1;               /* -1 the list, else the page shown */
int s_hub_sel = 0;             /* 0 Return to Game, 1.. the pages */
bool s_view_new = true;
bool s_direct;                 /* opened from the game's own menu: Back closes */
bool s_close_on_back;          /* the desk's Online Co-op: Back goes back to the desk, not the list */
bool s_from_pause;             /* from the pause menu's PC Options: the list is an options page */
const int kListPage = -2;      /* s_view: that list */

bool nav_up() { return ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)
                       || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp); }
bool nav_down() { return ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)
                         || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown); }
bool nav_accept() { return (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)
                           || ImGui::IsKeyPressed(ImGuiKey_Space, false)
                           || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)); }
bool nav_back() { return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)
                         || ImGui::IsMouseClicked(ImGuiMouseButton_Right); }

void open_view(int v)
{
    if (v != s_view)
        fprintf(stderr, "[UI] menu: %s\n", v == -1 ? "Quick menu" : v == -2 ? "PC Options" : kGamePages[v].title);
    s_view = v;
    s_view_new = true;
}

/* ---- F1: the Quick menu ------------------------------------------------
 *
 * The full settings are the pause menu's PC Options now; F1 (or both sticks
 * clicked) brings up a narrow panel at the left with the things changed in
 * the middle of a fight -- the game's Invincibility and Infinite Dead Eye,
 * Infinite Ammo, Freeze NPCs, the free camera, money, enemy health, the
 * frame counter -- and All PC Options for the rest. The game stays in view
 * beside it. Drawn as the game's: black, the pause menu's scroll rules, the
 * red title, berrylium rows a size smaller than an options page's. */
/* The free camera's choices, under its switch while it is on: the
 * keyboard and mouse, and each player's controller, fly the camera or play
 * on (for filming: the camera on one, the players on the others). */
static bool freecam_rows()
{
    static const char *const kb_items[2] = { "Flies the camera", "Plays" };
    static const char *const pad_items[2] = { "Flies the camera", "Plays" };
    bool changed = false;
    if (!g_rdr_freecam)
        return false;
    int k = g_rdr_settings.freecam_kbm;
    if (Choice("  Keyboard and mouse", &k, kb_items, 2,
               "Fly the free camera, or play on as their player while the camera is free.")) {
        g_rdr_settings.freecam_kbm = k;
        changed = true;
    }
    int shown = 0;
    for (int p = 0; p < 4; p++) {
        if (!rdr_port_live_pad(p) && p >= g_rdr_settings.coop_players)
            continue;
        char lab[48];
        snprintf(lab, sizeof lab, "  Player %d's controller", p + 1);
        int v = (g_rdr_settings.freecam_pads >> p) & 1;
        if (Choice(lab, &v, pad_items, 2,
                   "Fly the free camera, or keep playing: the player moves under the free camera.")) {
            g_rdr_settings.freecam_pads = (g_rdr_settings.freecam_pads & ~(1 << p)) | (v << p);
            changed = true;
        }
        shown++;
    }
    (void)shown;
    return changed;
}

void quick_money_row()
{
    int money = rdr_money_get();
    if (money < 0)
        return;
    char cur[48];
    fmt_money(cur, sizeof cur, money);
    static const char *one[3];
    one[0] = one[1] = one[2] = cur;
    int idx = 1, before = idx;
    if (Choice("Money", &idx, one, 3, "Left and right take away or add $1,000; A adds it.")) {
        int d = idx - before;           /* +1 right or A, -1 left */
        rdr_money_set(money + (d < 0 ? -1000 : 1000));
    }
}

/* Returns 1: All PC Options, 2: close. */
int quick_rows()
{
    int act = 0;
    if (rdr_online_session()) {
        Note("Online game: cheats and co-op are the host's, and stay as they are.");
    } else {
    if (rdr_cheats_available()) {
        int inv = rdr_cheat_get(6);
        if (Toggle("Invincibility", &inv, "Your health never drops (the game's own cheat)."))
            rdr_cheat_set(6, inv);
        int de = rdr_cheat_get(10);
        if (Toggle("Infinite Dead Eye", &de, "The Dead Eye meter never runs down (the game's own cheat)."))
            rdr_cheat_set(10, de);
    }
    Toggle("Infinite Ammo", &g_rdr_infinite_ammo, "Clips and reserves stay full: no reloading.");
    Toggle("Always Win Duels", &g_rdr_duel_win, "In a duel the opponent's shots miss: take your time.");
    Toggle("Always Excellent", &g_rdr_always_excellent,
           "Every mission's results screen rates you Excellent, with what that unlocks.");
    if (rdr_in_level()) {
        Toggle("Freeze NPCs", &g_rdr_freeze_npcs,
               "Everyone else stops where they stand. Switch it off again for missions that need someone to move.");
        if (Toggle("Guns in Town", &g_rdr_settings.town_guns,
                   "Draw and shoot in Brimstone, where the story keeps your guns holstered. The townsfolk can be hurt and killed."))
            s_dirty = true;
        if (g_rdr_settings.town_guns
            && Toggle("Townsfolk Come Back", &g_rdr_settings.town_respawn,
                      "Brimstone's people shot dead come back at their own places about a minute later."))
            s_dirty = true;
        if (Toggle("Protect Allies", &g_rdr_settings.protect_friendly,
                   "The people on your side or not in the fight (the barmaids, the ones you guard) take no damage."))
            s_dirty = true;
        int fc = g_rdr_freecam;
        if (Toggle("Free Camera (F6)", &fc, "Fly the camera anywhere. Below, while it is on: who flies it and who plays on."))
            g_rdr_freecam = fc;
        if (freecam_rows())
            s_dirty = true;
        quick_money_row();
        /* (a slider does not fit the narrow rows: the amount between arrows) */
        static char pct[17][8];
        static const char *pcts[17];
        for (int i = 0; i < 17; i++) {
            snprintf(pct[i], sizeof pct[i], "%d%%", 100 + 25 * i);
            pcts[i] = pct[i];
        }
        int eh = (g_rdr_settings.enemy_health - 100) / 25;
        if (eh < 0) eh = 0;
        if (eh > 16) eh = 16;
        if (Choice("Enemy health", &eh, pcts, 17,
                   "Story enemies take this much more to bring down: they take less damage from every hit.")) {
            g_rdr_settings.enemy_health = 100 + 25 * eh;
            s_dirty = true;
        }
    }
    if (rdr_level_kind() == 1) {
        /* Story co-op: the players now, restarting the chapter to change them */
        static const char *players[] = { "Off", "2 players", "3 players", "4 players" };
        int np = g_rdr_settings.coop_players - 1;
        if (np < 0 || np > 3) np = 0;
        if (Choice("Story co-op", &np, players, 4,
                   "Friends join as more players, each on a controller. Takes effect when the chapter "
                   "starts again: Restart Level, or Apply below.")) {
            g_rdr_settings.coop_players = np + 1;
            s_dirty = true;
            ApplyLive(&g_rdr_settings);
        }
        if (g_rdr_settings.coop_players > 1) {
            static const char *screens[] = { "Shared", "Top / bottom", "Side by side", "Separate windows" };
            int sp = g_rdr_settings.coop_split;
            if (Choice("Co-op screen", &sp, screens, 4,
                       "Shared: everyone on player 1's camera. Split: a view each. Separate windows: "
                       "each player in a window of their own. Changes at once."))  {
                g_rdr_settings.coop_split = sp;
                s_dirty = true;
            }
        }
        if (rdr_coop_changed() && Action("Apply co-op now", "Restart chapter",
                                         "Starts this chapter again with the players set above (as Go to "
                                         "Chapter does: progress in the chapter is lost).")) {
            rdr_settings_save(&g_rdr_settings);
            if (rdr_coop_apply_now())
                act = 2;
        }
    }
    }   /* not online */
    if (rdr_in_level() && rdr_coop_count() >= 2 && g_rdr_settings.coop_loadout == 1
        && Action("Co-op Shop", "", "Each player buys weapons with their own money."))
        act = 3;
    if (Toggle("Frame counter (F2)", &g_rdr_settings.show_fps, "The frame rate in the top left corner."))
        s_dirty = true;
    if (Action("All PC Options", "",
               "Every setting: Go to Chapter, Cheats and power-ups, Story Co-op, Display, Graphics, Controls."))
        act = 1;
    if (Action("Return to Game", "", "Closes this menu: F1, Esc or B too."))
        act = 2;
    return act;
}

void draw_quick(int w, int h, const game::Space &sp)
{
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    /* the panel, in the 1280 x 720 design space */
    const float x0 = 52, x1 = 588, y0 = 40, y1 = 692, cx = (x0 + x1) * 0.5f;
    bg->AddRectFilled(sp.P(x0, y0), sp.P(x1, y1), IM_COL32(0, 0, 0, 222));
    bg->AddRect(sp.P(x0, y0), sp.P(x1, y1), IM_COL32(120, 120, 120, 90), 0, 0, ImMax(1.0f, sp.s));
    game::ScrollRule(bg, sp.X(cx), sp.Y(76), 480 * sp.s, true);
    game::Text(bg, game::kTitle, sp.X(cx), sp.Y(138), "Quick Menu", 2.5f, 2.0f, game::kRed, 1, false, &sp);
    game::ScrollRule(bg, sp.X(cx), sp.Y(666), 480 * sp.s, false);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    if (s_view_new)
        ImGui::SetNextWindowFocus();
    ImGui::Begin("##rdr_quick", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    /* the rows a size smaller than an options page's */
    game::Space q = sp;
    q.s = sp.s * 0.76f;
    game::g_space = q;
    float rx = sp.X(x0 + 34), rw = (x1 - x0 - 68) * sp.s, top = sp.Y(158), bottom = sp.Y(564);
    {
        /* a whole number of rows, so no half a row shows above the hint */
        float pitch = 48 * q.s + ImGui::GetStyle().ItemSpacing.y;
        if (pitch > 1)
            bottom = top + floorf((bottom - top) / pitch) * pitch;
    }
    ImGui::SetCursorScreenPos(ImVec2(rx, top));
    if (s_view_new) {
        ImGui::SetNextWindowScroll(ImVec2(0, 0));
        FocusFirstRow();
    }
    ImGui::BeginChild("##qrows", ImVec2(rw + 8 * sp.s, bottom - top), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    BeginRows(rw);
    bool picker_was_open = PickerOpen();
    int act = quick_rows();
    const char *hint = CurrentHint();
    {
        /* the cursor moved to another row: the game's menu tick */
        static ImGuiID last_nav;
        ImGuiID nav = ImGui::GetCurrentContext()->NavId;
        if (nav != last_nav && last_nav && nav && !s_view_new && !PickerOpen())
            rdr_ui_sound(RDR_SOUND_MOVE);
        last_nav = nav;
    }
    game::ScrollBar(dl, sp.X(x1 - 14), sp.Y(160), sp.Y(560), sp);
    ImGui::EndChild();
    game::g_space = sp;
    s_view_new = false;
    if (hint)
        game::TextWrapped(dl, game::kText, sp.X(cx), sp.Y(588), 18 * sp.s, (x1 - x0 - 56) * sp.s, 2, hint, 0.86f,
                          0.78f, game::kHint, &sp);
    /* the prompts: A Change, B Close (both clickable) */
    float adv_a = 0, adv_b = 0, gap = 44 * sp.s;
    float wa = game::Width(game::kText, "\xba Change", 1.6f, &sp), wb = game::Width(game::kText, "\xbe Close", 1.6f, &sp);
    float x = sp.X(cx) - (wa + gap + wb) * 0.5f;
    game::Prompt(dl, x, sp.Y(645), 'A', "Change", 1.6f, 1.3f, sp, &adv_a);
    bool back = game::Prompt(dl, x + adv_a + gap, sp.Y(645), 'B', "Close", 1.6f, 1.3f, sp, &adv_b);
    ImGui::End();
    if (act == 1) {
        s_from_pause = false;
        s_hub_sel = 1;
        open_view(kListPage);
    } else if (act == 3) {
        s_from_pause = false;
        s_hub_sel = 4;
        open_view(3);                    /* the Co-op Shop */
    } else if (act == 2 || back || (!picker_was_open && !PickerOpen() && !rdr_input_capturing() && nav_back())) {
        set_open(false);
    }
}

/* The pause menu's PC Options: the list as the game's own Options page
 * draws its list (the black page, the red title, regulatorcondensed
 * entries centred, the one chosen white) -- it covers the pause menu, as
 * the game's Options page does. B goes back to the pause menu. */
void draw_list_page(int w, int h, const game::Space &sp)
{
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    game::OptionsBg(dl, sp, (float)w, (float)h);
    game::Text(dl, game::kTitle, sp.X(640), sp.Y(196), "PC Options", 4.12f, 3.2f, game::kRed, 1, false, &sp);
    const int n = kGamePageCount;
    if (s_hub_sel < 1 || s_hub_sel > n) s_hub_sel = 1;
    int sel0 = s_hub_sel;
    if (nav_up()) s_hub_sel = s_hub_sel > 1 ? s_hub_sel - 1 : n;
    if (nav_down()) s_hub_sel = s_hub_sel < n ? s_hub_sel + 1 : 1;
    bool go = nav_accept();
    ImVec2 m = ImGui::GetIO().MousePos;
    static ImVec2 last_mouse;
    bool moved = m.x != last_mouse.x || m.y != last_mouse.y;
    last_mouse = m;
    /* gameoptions.swui: regulatorcondensed 1.195, centred. The rows share
     * the room between the title and the prompts (y 256..552): up to six
     * at the game's own size, more a little smaller and closer (nine went
     * over the prompts and off the page -- user screenshot). */
    float step = n > 1 ? 296.0f / (float)(n - 1) : 58.0f;
    if (step > 58.0f) step = 58.0f;
    float k = step / 58.0f < 0.62f ? 0.62f : step / 58.0f;
    float sx = 2.0f * k, sy = 1.65f * k;
    for (int i = 1; i <= n; i++) {
        float base = sp.Y((k >= 1.0f ? 270.0f : 256.0f) + (float)(i - 1) * step);
        const char *t = kGamePages[i - 1].title;
        float tw = game::Width(game::kTitle, t, sx, &sp);
        ImVec2 a(sp.X(640) - tw * 0.5f - 20 * sp.s, base - (step - 10) * sp.s),
               b(sp.X(640) + tw * 0.5f + 20 * sp.s, base + 8 * sp.s);
        if (ImGui::IsMouseHoveringRect(a, b, false)) {
            if (moved) s_hub_sel = i;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { s_hub_sel = i; go = true; }
        }
        game::Text(dl, game::kTitle, sp.X(640), base, t, sx, sy, i == s_hub_sel ? game::kWhite : game::kGrey, 1, true, &sp);
    }
    float adv_a = 0, adv_b = 0, gap = 60 * sp.s;
    float wa = game::Width(game::kText, "\xba Select", 2.0f, &sp), wb = game::Width(game::kText, "\xbe Back", 2.0f, &sp);
    float x = sp.X(640) - (wa + gap + wb) * 0.5f;
    if (game::Prompt(dl, x, sp.Y(616), 'A', "Select", 2.0f, 1.6f, sp, &adv_a)) go = true;
    bool back = game::Prompt(dl, x + adv_a + gap, sp.Y(616), 'B', "Back", 2.0f, 1.6f, sp, &adv_b);
    if (go) {
        rdr_ui_sound(RDR_SOUND_ACCEPT);
        open_view(s_hub_sel - 1);
    } else if (back || nav_back()) {
        rdr_ui_sound(RDR_SOUND_BACK);
        if (s_from_pause) set_open(false);           /* back to the pause menu */
        else open_view(-1);                          /* back to the Quick menu */
    } else if (sel0 != s_hub_sel)
        rdr_ui_sound(RDR_SOUND_MOVE);
}

void draw_game_page(int w, int h, const game::Space &sp)
{
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    game::OptionsBg(bg, sp, (float)w, (float)h);
    const GamePage &pg = kGamePages[s_view];
    float tw = game::Text(bg, game::kTitle, sp.X(640), sp.Y(196), pg.title, 4.12f, 3.2f, game::kRed, 1, false, &sp);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
    if (s_view_new)
        ImGui::SetNextWindowFocus();
    ImGui::Begin("##rdr_game_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* the pages either side of this one: LB / RB, Page Up / Down, or the
     * red triangles beside the title */
    int step = 0;
    bool listed = s_view < kGamePageCount && !s_close_on_back;
    if (listed && (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_PageUp))) step = -1;
    if (listed && (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_PageDown))) step = 1;
    for (int side = 0; side < 2 && listed; side++) {
        ImVec2 c(sp.X(640) + (side ? 1 : -1) * (tw * 0.5f + 46 * sp.s), sp.Y(168));
        ImVec2 a(c.x - 22 * sp.s, c.y - 26 * sp.s), b(c.x + 22 * sp.s, c.y + 26 * sp.s);
        bool hot = ImGui::IsMouseHoveringRect(a, b, false);
        game::Tri(dl, c, side == 1, 34 * sp.s, hot ? IM_COL32(200, 40, 20, 255) : game::kRed);
        if (hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) step = side ? 1 : -1;
    }
    if (step) {
        open_view((s_view + kGamePageCount + step) % kGamePageCount);
        s_hub_sel = s_view + 1;
        ImGui::End();
        return;
    }

    /* the rows: the game's from x 160 to 1150, 48 apart, six in view */
    game::g_space = sp;
    float rx = sp.X(160), rw = 990 * sp.s, top = sp.Y(222), bottom = sp.Y(516);
    ImGui::SetCursorScreenPos(ImVec2(rx, top));
    if (s_view_new) {
        ImGui::SetNextWindowScroll(ImVec2(0, 0));
        FocusFirstRow();
    }
    ImGui::BeginChild("##rows", ImVec2(rw + 8 * sp.s, bottom - top), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    BeginRows(rw);
    bool picker_was_open = PickerOpen();
    bool book = s_direct && !listed;    /* the chapter book's Play As / Story Co-op */
    g_rows_accept_closes = book;
    g_rows_accepted = false;
    if (kGamePages[s_view].draw()) s_dirty = true;
    g_rows_accept_closes = false;
    const char *hint = CurrentHint();
    {
        /* the cursor moved to another row: the game's menu tick */
        static ImGuiID last_nav;
        ImGuiID nav = ImGui::GetCurrentContext()->NavId;
        if (nav != last_nav && last_nav && nav && !s_view_new && !PickerOpen())
            rdr_ui_sound(RDR_SOUND_MOVE);
        last_nav = nav;
    }
    game::ScrollBar(dl, sp.X(1205), sp.Y(226), sp.Y(512), sp);
    ImGui::EndChild();
    s_view_new = false;
    if (getenv("RDR_UI_DEBUG")) {
        static double last;
        ImGuiContext &g = *ImGui::GetCurrentContext();
        if (ImGui::GetTime() - last > 1.0) {
            last = ImGui::GetTime();
            fprintf(stderr, "[UI] nav id %08X window %s focused %s hint %s\n", g.NavId,
                    g.NavWindow ? g.NavWindow->Name : "-", g.NavWindow ? "yes" : "no", hint ? hint : "-");
        }
    }
    if (hint)
        game::TextWrapped(dl, game::kText, sp.X(640), sp.Y(534), 19 * sp.s, 1060 * sp.s, 3, hint, 0.98f, 0.86f,
                          game::kHint, &sp);

    /* the prompts, as the game's: A Accept, B Back (both clickable) */
    float adv_a = 0, adv_b = 0, gap = 60 * sp.s;
    float wa = game::Width(game::kText, "\xba Accept", 2.0f, &sp), wb = game::Width(game::kText, "\xbe Back", 2.0f, &sp);
    float x = sp.X(640) - (wa + gap + wb) * 0.5f;
    bool accept = game::Prompt(dl, x, sp.Y(616), 'A', "Accept", 2.0f, 1.6f, sp, &adv_a);
    bool back = game::Prompt(dl, x + adv_a + gap, sp.Y(616), 'B', "Back", 2.0f, 1.6f, sp, &adv_b);
    ImGui::End();
    if (book && (g_rows_accepted || (accept && !PickerOpen()))) {
        if (getenv("RDR_UI_DEBUG")) fprintf(stderr, "[UI] book page accepted\n");
        rdr_ui_sound(RDR_SOUND_ACCEPT);
        set_open(false);                 /* accepted: back to the book */
        g_rows_accepted = false;
        return;
    }
    if (accept && !book && !PickerOpen()) {
        /* "A Accept" clicked: the page's settings saved and the page left,
         * as the game's own option pages do (user: the Accept buttons
         * should "save and exit the current respective menu"). It took the
         * row with the cursor before (207), which only changed that row. A
         * and Enter on a row still change the row. */
        rdr_ui_sound(RDR_SOUND_ACCEPT);
        if (s_dirty.exchange(false))
            rdr_settings_save(&g_rdr_settings);
        if (s_direct && !listed) {
            set_open(false);             /* back to the book, or the desk */
        } else {
            s_hub_sel = s_view + 1;
            open_view(kListPage);        /* back to the list */
        }
        return;
    }
    if (back || (!picker_was_open && !PickerOpen() && !rdr_input_capturing() && nav_back())) {
        rdr_ui_sound(RDR_SOUND_BACK);
        if (s_direct && !listed) {
            set_open(false);             /* back to the book, or the desk */
        } else {
            s_hub_sel = s_view + 1;
            open_view(kListPage);        /* back to the list */
        }
    }
}

/* Where the menu goes: over its owner's part of the screen in split
 * screen; with Separate windows, the whole of its owner's window (the
 * toolkit draws the overlay there from the next frame). */
game::Space menu_space(int w, int h)
{
    float r[4];
    int k = s_owner >= 0 ? rdr_port_view(s_owner, r) : -1;
    if (k >= 0 && rdr_views_windowed()) {
        xbox_PresentSetOverlayView(k);
        return game::Fit((float)w, (float)h);
    }
    xbox_PresentSetOverlayView(0);
    if (k < 0)
        return game::Fit((float)w, (float)h);
    game::Space sp = game::Fit(r[2] * (float)w, r[3] * (float)h);
    sp.ox += r[0] * (float)w;
    sp.oy += r[1] * (float)h;
    return sp;
}

void draw_game_menu(int w, int h)
{
    game::Space sp = menu_space(w, h);
    game::g_space = sp;
    int req = s_show_req.exchange(-1);
    if (req == 3) {
        s_close_on_back = false;
        s_direct = false;
        s_from_pause = false;
        s_hub_sel = 0;
        open_view(-1);
    } else if (req >= 0) {
        s_close_on_back = req == 4;
        /* 0: the list (the pause menu's PC Options); 1, 2: the book's Play
         * As and Story Co-op pages */
        s_direct = true;
        s_from_pause = req == 0;
        s_hub_sel = req == 0 ? 1 : 0;   /* (4: the desk's Online Co-op) */
        open_view(req == 1 ? kGamePageCount : req == 2 ? kGamePageCount + 1 : req == 4 ? 5 : kListPage);
    }
    if (s_view == kListPage) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
        ImGui::Begin("##rdr_game_list", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
        draw_list_page(w, h, sp);
        ImGui::End();
    } else if (s_view < 0) {
        draw_quick(w, h, sp);
    } else {
        draw_game_page(w, h, sp);
    }
}

void draw_menu(int w, int h)
{
    const float s = g_scale;
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(8, 4, 2, 150));

    /* Tall enough for the whole Cheats page (the game's seven, ours, the
     * money rows) without scrolling. */
    float pw = 840 * s, ph = 930 * s;   /* see ensure_context */
    if (pw > w - 20) pw = (float)w - 20;
    if (ph > h - 20) ph = (float)h - 20;
    ImVec2 a((w - pw) * 0.5f, (h - ph) * 0.5f), b(a.x + pw, a.y + ph);
    DrawParchment(bg, a, b, 3);
    DrawFrame(bg, a, b);

    ImGui::SetNextWindowPos(a);
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##rdr_menu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    static const char *tabs[] = { "Cheats", "Display", "Graphics", "Controls" };
    DrawTitle(dl, g_fonts.title, g_fonts.title->FontSize, tabs[s_tab], ImVec2(a.x + pw * 0.5f, a.y + 92 * s), kRed);
    DrawFlourish(dl, ImVec2(a.x + pw * 0.5f, a.y + 138 * s), pw * 0.42f);

    /* Tabs: LB / RB or click. */
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_PageUp))
        s_tab = (s_tab + 3) % 4;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_PageDown))
        s_tab = (s_tab + 1) % 4;
    {
        ImGui::PushFont(g_fonts.heading);
        float total = 0, gap = 34 * s;
        for (int i = 0; i < 4; i++) total += ImGui::CalcTextSize(tabs[i]).x;
        total += gap * 3;
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(a.x + (pw - total) * 0.5f, a.y + 156 * s));
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine(0, gap);
            if (Tab(tabs[i], s_tab == i)) s_tab = i;
        }
    }

    float rx = a.x + 90 * s, rw = pw - 180 * s;
    ImGui::SetCursorScreenPos(ImVec2(rx, a.y + 214 * s));
    ImGui::BeginChild("##rows", ImVec2(rw + 12 * s, ph - 214 * s - 124 * s), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);
    BeginRows(rw);
    bool ch = false;
    switch (s_tab) {
    case 0:
        ch = page_cheats();
        Section("Story co-op and Play As");
        ch |= page_coop();
        Section("Go to chapter");
        ch |= page_chapter();
        break;
    case 1: ch = PageDisplay(&g_rdr_settings, true); break;
    case 2: ch = PageGraphics(&g_rdr_settings, true); break;
    case 3: ch = PageControls(&g_rdr_settings, true); break;
    }
    if (ch) s_dirty = true;
    const char *hint = CurrentHint();
    ImGui::EndChild();

    /* The hint for the row under the cursor, then the prompts. */
    if (hint) {
        ImGui::PushFont(g_fonts.italic);
        ImVec2 hp(a.x + 90 * s, b.y - 118 * s);
        ImGui::GetWindowDrawList()->AddText(g_fonts.italic, g_fonts.italic->FontSize, hp, kInkSoft, hint,
                                            nullptr, rw);
        ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + 90 * s, b.y - 74 * s));
    if (Prompt('A', "Select")) {}
    ImGui::SameLine(0, 30 * s);
    if (Prompt('B', "Close") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        set_open(false);
    ImGui::SameLine(0, 30 * s);
    ImGui::PushFont(g_fonts.small);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * s);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kInkSoft), "LB / RB: pages    F1 or both sticks: close");
    ImGui::PopFont();
    ImGui::End();
}

/* RDR_OVERLAY_FLAG=<file> (tests, for runs with no keyboard): each time
 * the file appears it is read and removed. Empty, it opens or closes the
 * menu; "tab <n>" opens it on that page; "money <n>" sets the money,
 * "pay <player> <n>" pays a player as a pickup does,
 * "excellent <0|1>" and "ammo <0|1>" switch those cheats; "drain" empties
 * the current gun as firing would (to check Infinite Ammo); "mouse dx dy"
 * moves the mouse; "pointer x y" puts the pointer at x,y of the menus'
 * 640x448 space; "look" logs the camera's bearing and tilt. */
} /* namespace */
extern "C" void rdr_overlay_show(int what);
extern "C" void rdr_coop_test_pay(int player, int amount);
extern "C" void rdr_test_npc_hurt(int damage);
extern "C" void rdr_test_player_hurt(int k, int damage);
extern "C" void rdr_test_ray(void);
extern "C" void rdr_spawn_request(const char *type, int count, int player);
extern "C" void rdr_enm_dump(void);
extern "C" void rdr_heaps_dump(void);
extern "C" void rdr_test_enm_kill(int n);
extern "C" void rdr_npc_request(const char *type, const char *table, const char *name, int team, int player);
namespace {
void test_flag(void)
{
    static const char *path = getenv("RDR_OVERLAY_FLAG");
    static unsigned n;
    char cmd[64] = {0};
    int v, w2;
    FILE *f;
    if (!path || ++n % 30)
        return;
    f = fopen(path, "rb");
    if (!f)
        return;
    if (fread(cmd, 1, sizeof cmd - 1, f) == 0)
        cmd[0] = 0;
    fclose(f);
    remove(path);
    if (sscanf(cmd, "money %d", &v) == 1) {
        int before = rdr_money_get(), ok = rdr_money_set(v);
        fprintf(stderr, "[TEST] money %d -> %d: %s (now %d)\n", before, v, ok ? "set" : "no player", rdr_money_get());
    }
    else if (sscanf(cmd, "pay %d %d", &v, &w2) == 2) {
        rdr_coop_test_pay(v - 1, w2);
        fprintf(stderr, "[TEST] pay player %d $%d\n", v, w2);
    }
    else if (sscanf(cmd, "lobbyhost %d", &v) == 1)
        rdr_lobby_host(v, "TestHost");
    else if (!strncmp(cmd, "lobbyjoin ", 10)) {
        char c[48] = "";
        sscanf(cmd + 10, "%47s", c);
        rdr_lobby_join(c, "TestGuest");
    } else if (!strncmp(cmd, "lobbystart", 10))
        rdr_lobby_start();
    else if (!strncmp(cmd, "lobbyleave", 10))
        rdr_lobby_leave();
    else if (!strncmp(cmd, "lobbyview", 9)) {
        RdrLobbyView lv;
        rdr_lobby_view(&lv);
        fprintf(stderr, "[TEST] lobby state %d players %d me %d status '%s' codes %d", lv.state, lv.players, lv.me,
                lv.status, lv.ncodes);
        for (int k = 0; k < lv.ncodes; k++) fprintf(stderr, " [%s %s]", lv.where[k], lv.codes[k]);
        for (int k = 0; k < 4; k++) fprintf(stderr, " p%d:%d%d'%s'", k + 1, lv.present[k], lv.ready[k], lv.names[k]);
        fprintf(stderr, "\n");
    }
    else if (!strncmp(cmd, "raytest", 7))
        rdr_test_ray();
    else if (sscanf(cmd, "phurt %d %d", &v, &w2) == 2) {
        rdr_test_player_hurt(v, w2);
    }
    else if (sscanf(cmd, "npchurt %d", &v) == 1)
        rdr_test_npc_hurt(v);
    else if (sscanf(cmd, "enemyhp %d", &v) == 1)
        g_rdr_settings.enemy_health = v;
    else if (sscanf(cmd, "coop %d %d", &v, &w2) == 2) {
        g_rdr_settings.coop_players = v;
        g_rdr_settings.coop_split = w2;
        fprintf(stderr, "[TEST] co-op players %d split %d\n", v, w2);
    }
    else if (sscanf(cmd, "cheat %d %d", &v, &w2) == 2) {
        rdr_cheat_set(v, w2);
        fprintf(stderr, "[TEST] cheat %d -> %d (now %d)\n", v, w2, rdr_cheat_get(v));
    }
    else if (!strncmp(cmd, "enmdump", 7))
        rdr_enm_dump();
    else if (!strncmp(cmd, "heaps", 5))
        rdr_heaps_dump();
    else if (sscanf(cmd, "enmkill %d", &v) == 1)
        rdr_test_enm_kill(v);
    else if (!strncmp(cmd, "npcask ", 7)) {
        int a1 = 0, a2 = 0, a3 = 0;
        if (sscanf(cmd + 7, "%d %d %d", &a1, &a2, &a3) == 3)
            rdr_npc_ask(a1, a2, a3, 0);
    }
    else if (!strncmp(cmd, "npcopts ", 8)) {
        int c = 1, p2 = 0, ar = 0;
        if (sscanf(cmd + 8, "%d %d %d", &c, &p2, &ar) == 3)
            rdr_npc_opts_ask(c, p2, ar, 0);
    }
    else if (!strncmp(cmd, "npcdel", 6))
        rdr_npc_remove_ask(0);
    else if (sscanf(cmd, "npcother %d", &v) == 1)
        rdr_npc_other_ask(v, 0);
    else if (!strncmp(cmd, "npcmake ", 8)) {
        char ty[16] = "", tb[48] = "";
        int team = 2;
        if (sscanf(cmd + 8, "%15s %47s %d", ty, tb, &team) >= 2)
            rdr_npc_request(ty, tb, ty, team, 0);
    }
    else if (sscanf(cmd, "letterbox %d", &v) == 1)
        g_rdr_settings.letterbox = v;
    else if (sscanf(cmd, "excellent %d", &v) == 1)
        g_rdr_always_excellent = v;
    else if (sscanf(cmd, "ammo %d", &v) == 1)
        g_rdr_infinite_ammo = v;
    else if (!strncmp(cmd, "unlock", 6))
        rdr_unlock_everything();
    else if (!strncmp(cmd, "drain", 5))
        rdr_ammo_test_drain();
    else if (!strncmp(cmd, "report", 6))
        rdr_ammo_test_report();
    else if (!strncmp(cmd, "mousehold ", 10)) {
        int ms = 0;
        if (sscanf(cmd + 10, "%d %d %d", &v, &w2, &ms) == 3)
            rdr_input_test_mouse_hold(v, w2, ms);
    }
    else if (sscanf(cmd, "mouse %d %d", &v, &w2) == 2)
        rdr_input_test_mouse(v, w2);
    else if (sscanf(cmd, "pointerpx %d %d", &v, &w2) == 2)
        rdr_input_test_pointer_px((float)v, (float)w2);
    else if (sscanf(cmd, "pointer %d %d", &v, &w2) == 2)
        rdr_input_test_pointer((float)v, (float)w2);
    else if (!strncmp(cmd, "look", 4))
        rdr_mouse_look_report();
    else if (!strncmp(cmd, "spawn ", 6)) {
        char ty[16] = "*";
        int cnt = 1;
        sscanf(cmd + 6, "%15s %d", ty, &cnt);
        rdr_spawn_request(ty, cnt, 0);
    }
    else if (!strncmp(cmd, "warp ", 5)) {
        char nm[64] = "";
        int var = 1;
        if (sscanf(cmd + 5, "%63s %d", nm, &var) >= 1)
            rdr_warp_request(nm, var);
    }
    else if (!strncmp(cmd, "swfget ", 7)) {
        char nm[64] = "";
        if (sscanf(cmd + 7, "%63s", nm) == 1)
            rdr_swf_test_get(nm);
    }
    else if (!strncmp(cmd, "swfset ", 7)) {
        char nm[32] = "";
        int val = 0;
        if (sscanf(cmd + 7, "%31s %d", nm, &val) == 2)
            rdr_swf_test_set(nm, val);
    }
    else if (!strncmp(cmd, "memdump ", 8)) {
        char what[32] = "", path[256] = "";
        unsigned len = 0;
        if (sscanf(cmd + 8, "%31s %x %255s", what, &len, path) == 3)
            rdr_mem_dump(what, len, path);
    }
    else if (!strncmp(cmd, "duelrep", 7))
        rdr_duel_report();
    else if (sscanf(cmd, "buy %d %d", &v, &w2) == 2)
        rdr_coop_buy_ask(v, w2);
    else if (sscanf(cmd, "loadout %d", &v) == 1)
        g_rdr_settings.coop_loadout = v;
    else if (!strncmp(cmd, "tp ", 3)) {
        float x, y, z;
        if (sscanf(cmd + 3, "%d %f %f %f", &v, &x, &y, &z) == 4)
            rdr_test_tp(v, x, y, z);
    }
    else if (sscanf(cmd, "coopduels %d", &v) == 1)
        g_rdr_settings.coop_duels = v ? 1 : 0;
    else if (sscanf(cmd, "giveweapon %d", &v) == 1)
        rdr_give_weapon_ask(v, 0);
    else if (sscanf(cmd, "kb2key %d %d", &v, &w2) == 2)
        rdr_input_test_kb2_key(v, w2);          /* the second keyboard: a key held w2 ms */
    else if (sscanf(cmd, "kb2mouse %d %d", &v, &w2) == 2)
        rdr_input_test_kb2_mouse(v, w2);        /* the second mouse moved */
    else if (!strncmp(cmd, "kb2report", 9))
        fprintf(stderr, "[TEST] kb2: port %d\n", rdr_kbm2_port() + 1);
    else if (!strncmp(cmd, "players", 7))
        rdr_test_players_report();
    else if (sscanf(cmd, "uisound %d", &v) == 1)
        rdr_ui_sound(v);
    else if (sscanf(cmd, "soundbusy %d", &v) == 1)
        g_rdr_test_sound_busy = v;
    else if (sscanf(cmd, "framems %d", &v) == 1)
        g_rdr_test_frame_us = v * 1000;         /* the clock as at 1000/v frames a second */
    else if (sscanf(cmd, "coachlog %d", &v) == 1)
        g_rdr_test_coach_log = v;
    else if (sscanf(cmd, "coachold %d", &v) == 1)
        g_rdr_test_coach_old = v;
    else if (!strncmp(cmd, "fillammo", 8))
        rdr_fill_ammo_ask(0);
    else if (sscanf(cmd, "duelwin %d", &v) == 1)
        g_rdr_duel_win = v;
    else if (sscanf(cmd, "protect %d", &v) == 1)
        g_rdr_settings.protect_friendly = v ? 1 : 0;
    else if (sscanf(cmd, "coopkill %d", &v) == 1)
        rdr_coop_test_kill(v);
    else if (!strncmp(cmd, "overlay ", 8))
        set_open(atoi(cmd + 8) != 0);          /* tests: the menu opened or closed */
    else if (!strncmp(cmd, "page", 4))
        rdr_menu_test_report();
    else if (!strncmp(cmd, "period", 6))
        rdr_timing_report();
    else if (!strncmp(cmd, "calls", 5))
        rdr_count_report();
    else if (!strncmp(cmd, "icalls", 6))
        rdr_icall_report();
    else if (sscanf(cmd, "freeze %d", &v) == 1) {
        g_rdr_freeze_npcs = v;
        rdr_freeze_report();
    } else if (!strncmp(cmd, "freezereport", 12))
        rdr_freeze_report();
    else if (!strncmp(cmd, "level ", 6)) {
        char nm[64] = "";
        int var = 0;
        if (sscanf(cmd + 6, "%63s %d", nm, &var) >= 1)
            fprintf(stderr, "[TEST] level %s %d -> %d\n", nm, var, rdr_level_go(nm, var));
    } else if (sscanf(cmd, "fcroles %d %d", &v, &w2) == 2) {
        g_rdr_settings.freecam_kbm = v;
        g_rdr_settings.freecam_pads = w2;
        fprintf(stderr, "[TEST] free camera: keyboard %s, controllers playing %x\n", v ? "plays" : "flies", w2);
    } else if (sscanf(cmd, "freecam %d", &v) == 1)
        g_rdr_freecam = v;
    else if (!strncmp(cmd, "stick2 ", 7)) {
        float x = 0, y = 0;
        int ms = 0;
        if (sscanf(cmd + 7, "%f %f %d", &x, &y, &ms) == 3)
            rdr_input_test_stick2(x, y, ms);
    } else if (!strncmp(cmd, "stick ", 6)) {
        float x = 0, y = 0;
        int lt = 0, ms = 0;
        if (sscanf(cmd + 6, "%f %f %d %d", &x, &y, &lt, &ms) == 4)
            rdr_input_test_stick(x, y, lt, ms);
    } else if (!strncmp(cmd, "imclick ", 8)) {
        /* the menu's own pointer: a click at x,y of the window */
        float x = 0, y = 0;
        if (sscanf(cmd + 8, "%f %f", &x, &y) == 2) {
            push({Ev::MouseMove, x, y, 0, 0});
            push({Ev::MouseButton, 0, 0, 0, 1});
            push({Ev::MouseButton, 0, 0, 0, 0});
        }
    } else if (!strncmp(cmd, "imkey ", 6)) {
        /* a key for the menu: up down left right enter esc pgup pgdn */
        static const struct { const char *n; ImGuiKey k; } keys[] = {
            { "up", ImGuiKey_UpArrow }, { "down", ImGuiKey_DownArrow }, { "left", ImGuiKey_LeftArrow },
            { "right", ImGuiKey_RightArrow }, { "enter", ImGuiKey_Enter }, { "esc", ImGuiKey_Escape },
            { "pgup", ImGuiKey_PageUp }, { "pgdn", ImGuiKey_PageDown } };
        for (const auto &k : keys)
            if (!strncmp(cmd + 6, k.n, strlen(k.n))) {
                push({Ev::Key, 0, 0, (int)k.k, 1});
                push({Ev::Key, 0, 0, (int)k.k, 0});
            }
    } else if (!strncmp(cmd, "show ", 5) && sscanf(cmd + 5, "%d", &v) == 1) {
        rdr_overlay_show(v);
    } else if (!strncmp(cmd, "imtype ", 7)) {
        for (const char *c = cmd + 7; *c && *c != '\n'; c++)
            push({Ev::Char, 0, 0, (unsigned char)*c, 0});
    } else if (!strncmp(cmd, "type ", 5)) {
        /* as if typed on the name grid: letters, '<' delete, '>' space,
         * '!' Enter, '~' Esc */
        for (const char *c = cmd + 5; *c && *c != '\n'; c++)
            rdr_input_text_push(*c == '!' ? RDR_TEXT_END : *c == '~' ? RDR_TEXT_CANCEL : *c);
    }
    else if (sscanf(cmd, "menuport %d", &v) == 1)
        toggle_quick(v);                        /* as that port's sticks would */
    else if (sscanf(cmd, "h2002 %d", &v) == 1)
        g_rdr_settings.look2002 = v;
    else if (!strncmp(cmd, "hudmask ", 8)) {
        g_rdr_hud_mask_test = (uint32_t)strtoul(cmd + 8, nullptr, 16);
        fprintf(stderr, "[TEST] HUD mask AND %08X\n", g_rdr_hud_mask_test);
        fflush(stderr);
    }
    else if (sscanf(cmd, "hang %d", &v) == 1) {
        /* the picture held for v seconds (the stall report's test) */
        fprintf(stderr, "[TEST] holding for %d s\n", v);
        fflush(stderr);
        std::this_thread::sleep_for(std::chrono::seconds(v));
    }
    else if (sscanf(cmd, "tab %d", &v) == 1) {
        s_tab = v % 4;
        open_view(v >= 0 && v < kGamePageCount ? v : -1);
        set_open(true);
    } else if (!strncmp(cmd, "hub", 3)) {
        open_view(-1);
        set_open(true);
    } else
        toggle_quick();
}

void overlay_draw(void *, int w, int h)
{
    test_flag();
    rdr_input_set_view(w, h);
    bool open = s_open;
    rdr_timing_tick();
    rdr_freecam_tick();
    rdr_coop_tick();
    rdr_online_start_tick();
    char ls_status[192];
    bool have_status = rdr_ls_status(ls_status, (int)sizeof ls_status) != 0;
    if (!have_status && !open)
        have_status = rdr_freecam_hint(ls_status, (int)sizeof ls_status) != 0;
    bool show_version = !open && game::Ready() && !rdr_in_level();
    bool hud2002 = rdr_hud2002_wanted() != 0;
    if (!open && !g_rdr_settings.show_fps && !have_status && !show_version && !hud2002)
        return;
    if (!ensure_context(h))
        return;
    ImGuiIO &io = ImGui::GetIO();
    uint64_t now = xbox_HostNowNs();
    io.DeltaTime = (float)((now - s_last_ns) / 1e9);
    if (io.DeltaTime <= 0 || io.DeltaTime > 0.25f) io.DeltaTime = 1.0f / 60.0f;
    s_last_ns = now;
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.MouseDrawCursor = false;
    if (s_reset_input.exchange(false)) {
        io.ClearInputKeys();
        io.ClearInputMouse();
#if defined(_WIN32)
        POINT pt;
        if (s_hwnd && GetCursorPos(&pt) && ScreenToClient((HWND)s_hwnd, &pt))
            io.AddMousePosEvent((float)pt.x, (float)pt.y);
#endif
    }
    feed_input();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    if (hud2002 && !rdr_hud2002_title(ImGui::GetBackgroundDrawList(), w, h))
        rdr_hud2002_draw(ImGui::GetBackgroundDrawList(), w, h, io.DeltaTime, open);
    if (g_rdr_settings.show_fps)
        draw_fps(ImGui::GetForegroundDrawList());
    if (have_status)
        draw_status(ImGui::GetForegroundDrawList(), ls_status, w);
    if (show_version)
        draw_version(ImGui::GetForegroundDrawList(), w, h);
    s_game_look = game::Ready();
    if (open) {
        if (game::Ready()) draw_game_menu(w, h);
        else draw_menu(w, h);
    } else {
        xbox_PresentSetOverlayView(0);
        s_view = -1;                 /* F1 opens on the list again */
        s_hub_sel = 0;
        s_direct = false;
        s_close_on_back = false;
        s_from_pause = false;
    }
    ImGui::Render();
    rdr_gl.Viewport(0, 0, w, h);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} /* namespace */

/* From the game's own menus (rdr_hooks.c): 0 the menu's list (the pause
 * menu's PC Options), 1 Play As, 2 Story Co-op (the chapter book). */
extern "C" void rdr_overlay_show(int what)
{
    if (!game::Ready() && what) {
        s_tab = 0;                   /* the parchment menu: the Cheats tab has them */
    }
    s_show_req = what;
    s_hold_a = true;
    s_owner = rdr_pause_owner_port();   /* the pause menu's PC Options: the player who paused */
    set_open(true);
}

extern "C" void rdr_overlay_install(void)
{
    xbox_InputSetFilter(pad_filter);
    xbox_InputSetRumbleScale(g_rdr_settings.vibration / 100.0f);
#if defined(_WIN32)
    xbox_FramebufferWindowSetMsgHook(msg_hook);
#endif
    nvgl_set_overlay(overlay_draw, nullptr);
    /* RDR_OVERLAY_OPEN=1 (tests): start with the menu open. */
    if (getenv("RDR_OVERLAY_OPEN")) {
        s_open = true;
        s_tab = atoi(getenv("RDR_OVERLAY_OPEN")) % 4;
        /* in the game's look: 0 the list, 1.. a page */
        int v = atoi(getenv("RDR_OVERLAY_OPEN"));
        s_view = v >= 1 && v <= kGamePageCount ? v - 1 : -1;
    }
}
