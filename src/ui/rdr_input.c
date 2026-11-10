/*
 * rdr_input.c - see rdr_input.h.
 *
 * Mouse look: where the game's camera reads its aim inputs (the player's
 * camera channels 14 and 15, rdr_overrides.c), the mouse is added there
 * as an angle each frame -- no deadzone, curve or top speed in the way.
 * Elsewhere it falls back to the stick:
 *
 * Mouse to stick: the movement since the last poll becomes a stick
 * deflection proportional to the mouse's speed, smoothed over a few
 * milliseconds and lifted past the game's own stick deadzone, so the
 * smallest movement turns the camera and a fast flick turns it at full
 * rate. Raw input (WM_INPUT), registered on the game window once, without
 * the input-sink flag: only while the window has the focus. The pointer is
 * hidden and kept inside the window while playing; F9 frees it, the menu
 * (F1) frees it while open.
 */
#include "rdr_input.h"
#include "rdr_settings.h"
#include "rdr_cheats.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

extern int xbox_FramebufferKeyDown(int vk);
extern uint64_t xbox_HostNowNs(void);

static const char *const s_control_names[RDR_KEY_COUNT] = {
    "Move forward", "Move back", "Move left", "Move right",
    "A button", "B button", "X button", "Y button", "Black (special move)", "White (next weapon)",
    "Left trigger", "Right trigger", "Start", "Back",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "Left stick click", "Right stick click",
    "Rifle zoom",
    "Previous weapon", "Next weapon",
    "Dead Eye", "Dead Eye (second key)",
};

const char *rdr_control_name(int c)
{
    return c >= 0 && c < RDR_KEY_COUNT ? s_control_names[c] : "?";
}

const char *rdr_key_name(int vk)
{
    static char buf[32];
    switch (vk) {
    case 0: return "(none)";
    case 0x01: return "Left mouse";
    case 0x02: return "Right mouse";
    case 0x04: return "Middle mouse";
    case 0x05: return "Mouse 4";
    case 0x06: return "Mouse 5";
    case RDR_VK_WHEEL_UP: return "Wheel up";
    case RDR_VK_WHEEL_DOWN: return "Wheel down";
    case 0x08: return "Backspace";
    case 0x09: return "Tab";
    case 0x0D: return "Enter";
    case 0x10: return "Shift";
    case 0x11: return "Ctrl";
    case 0x12: return "Alt";
    case 0x14: return "Caps Lock";
    case 0x1B: return "Esc";
    case 0x20: return "Space";
    case 0x25: return "Left arrow";
    case 0x26: return "Up arrow";
    case 0x27: return "Right arrow";
    case 0x28: return "Down arrow";
    case 0xA0: return "Left Shift";
    case 0xA1: return "Right Shift";
    case 0xA2: return "Left Ctrl";
    case 0xA3: return "Right Ctrl";
    case 0xC0: return "`";
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        snprintf(buf, sizeof buf, "%c", vk);
        return buf;
    }
    if (vk >= 0x60 && vk <= 0x69) {
        snprintf(buf, sizeof buf, "Keypad %d", vk - 0x60);
        return buf;
    }
    if (vk >= 0x70 && vk <= 0x87) {
        snprintf(buf, sizeof buf, "F%d", vk - 0x6F);
        return buf;
    }
    snprintf(buf, sizeof buf, "Key %d", vk);
    return buf;
}

static volatile int s_capture_wait, s_captured;

/* ---- A second keyboard and mouse (local co-op) ----------------------
 *
 * Windows' raw input names the device each key press and mouse movement
 * comes from. With the second keyboard and mouse given to a player
 * (kbm2_player) and the two picked (settings.ini keeps their device
 * names: kb2_device, mouse2_device), what they do drives that player's
 * port (rdr_input_merge2), and the first keyboard-and-mouse player's
 * controls stop seeing them: its keys and buttons then come from raw
 * input as well (every other keyboard and mouse), not from the window's
 * messages, which mix every device together. Off (the default, nothing
 * picked, or an online game), the first player's input is as before. */
extern int rdr_lockstep_mode(void);                  /* rdr_lockstep.c: 4 online */
static volatile unsigned char s_raw_key[2][256];     /* [0] every other device, [1] the second set */
static volatile uint64_t s_raw_tap_ns[2][8];         /* a mouse button's press: a click lasts 80 ms */
static volatile long s_dx2, s_dy2;                   /* the second mouse's counts */
static volatile long s_wheel2[2];                    /* its wheel's notches: up, down */
static volatile int s_kb2_pick;                      /* 1: the next key picks the keyboard, 2: the next click the mouse */
static volatile int s_kb2_changed;                   /* picked: the UI takes the names */
static char s_kb2_new_kbd[256], s_kb2_new_mouse[256];
static volatile unsigned s_kb2_gen;                  /* the devices changed: their names looked up again */
static volatile uint64_t s_test_kb2_until[256];      /* tests (any host): a second-set key held */
static int s_kb2_test = -1;                          /* RDR_TEST_KB2: the second set on with no devices */

/* The second set's player's port (0-3), or -1: not in use. */
int rdr_kbm2_port(void)
{
    const RdrSettings *s = &g_rdr_settings;
    int p = s->kbm2_player - 1;
    if (s_kb2_test < 0)
        s_kb2_test = getenv("RDR_TEST_KB2") != NULL;
    if (p < 0 || p > 3 || p == s->kbm_player - 1 || !s->mouse_aim || rdr_lockstep_mode() == 4)
        return -1;
    if (!s->kb2_device[0] && !s->mouse2_device[0] && !s_kb2_test)
        return -1;
    return p;
}

/* The first player's keys from raw input instead of the window's (which
 * has the second keyboard's too): only with a second set really picked. */
static int kb2_split(void)
{
#if defined(_WIN32)
    return rdr_kbm2_port() >= 0 && (g_rdr_settings.kb2_device[0] || g_rdr_settings.mouse2_device[0]);
#else
    return 0;
#endif
}

/* A key or mouse button down on one set; 0x10-0x12 (Shift, Ctrl, Alt)
 * are either side's. A click counts for 80 ms however short it was. */
static int raw_down(int set, int vk)
{
    if (vk <= 0 || vk > 255)
        return 0;
    if (vk == 0x10) return s_raw_key[set][0xA0] || s_raw_key[set][0xA1];
    if (vk == 0x11) return s_raw_key[set][0xA2] || s_raw_key[set][0xA3];
    if (vk == 0x12) return s_raw_key[set][0xA4] || s_raw_key[set][0xA5];
    if (s_raw_key[set][vk])
        return 1;
    if (vk <= 6 && vk != 3 && s_raw_tap_ns[set][vk] && xbox_HostNowNs() - s_raw_tap_ns[set][vk] < 80000000ull)
        return 1;
    return set == 1 && s_test_kb2_until[vk] && xbox_HostNowNs() < s_test_kb2_until[vk];
}

static void raw_clear(void)
{
    memset((void *)s_raw_key, 0, sizeof s_raw_key);
}

void rdr_input_kb2_pick(void)
{
    s_kb2_new_kbd[0] = s_kb2_new_mouse[0] = 0;
    s_kb2_pick = 1;
}

int rdr_input_kb2_picking(void)
{
    return s_kb2_pick;
}

void rdr_input_kb2_cancel(void)
{
    s_kb2_pick = 0;
}

/* After a pick: the two names (either may be "", none picked), once. */
int rdr_input_kb2_take(char *kbd, char *mouse, int n)
{
    if (!s_kb2_changed)
        return 0;
    snprintf(kbd, (size_t)n, "%s", s_kb2_new_kbd);
    snprintf(mouse, (size_t)n, "%s", s_kb2_new_mouse);
    s_kb2_changed = 0;
    return 1;
}

/* The settings' names changed (picked or forgotten): look them up again. */
void rdr_input_kb2_changed(void)
{
    s_kb2_gen++;
    memset((void *)s_raw_key[1], 0, sizeof s_raw_key[1]);
}

void rdr_input_test_kb2_key(int vk, int ms)
{
    if (vk > 0 && vk < 256)
        s_test_kb2_until[vk] = xbox_HostNowNs() + (uint64_t)ms * 1000000ull;
}

void rdr_input_test_kb2_mouse(long dx, long dy)
{
#if defined(_WIN32)
    InterlockedAdd((volatile LONG *)&s_dx2, dx);
    InterlockedAdd((volatile LONG *)&s_dy2, dy);
#else
    __sync_fetch_and_add(&s_dx2, dx);
    __sync_fetch_and_add(&s_dy2, dy);
#endif
}

/* The pointer over menu pages (see "Pointing at menu items" below). */
static volatile int s_ptr_cx = -1, s_ptr_cy = -1;   /* client pixels */
static volatile unsigned s_ptr_gen;                 /* bumped on movement */
static volatile float s_test_sx = -1, s_test_sy = -1;
static volatile float s_test_u = -99, s_test_v = -99;
static volatile int s_view_w, s_view_h;
static volatile uint64_t s_ptr_page_ns;
static volatile int s_ptr_hover;

/* Typing a name on the Gunslinger Profiles sheet: while the game's letter
 * grid is up (the hook in rdr_overrides.c says so every frame), letters,
 * Space, Backspace, Enter and Esc go to it instead of the bindings. The
 * window thread queues them, the game's thread takes them. */
static volatile uint64_t s_text_ns;
static volatile int s_text_q[64];
static volatile unsigned s_text_head, s_text_tail;
static volatile int s_focus_lost;            /* the window lost the focus (rdr_input_msg) */

/* Whether the window lost the focus since the last call. */
/* (only a loss in the last second counts: one during a load or a
 * cutscene, seen when play comes back, is too old to pause for -- user:
 * "when alt tabbing while the game is playing a cutscene / loading, the
 * game will auto-pause after tabbing back in") */
static volatile uint64_t s_focus_lost_ns;
int rdr_input_focus_lost_take(void)
{
    uint64_t now = xbox_HostNowNs();
    if (!s_focus_lost)
        return 0;
    s_focus_lost = 0;
    return now - s_focus_lost_ns < 1000000000ull;
}

static volatile int s_text_press;           /* 1 A, 2 B, 3 Start, 4/5 left/right, 6/7 up/down: one press */
static int s_second_press;                  /* the desk's far item: a second step, 4/5 */
static uint64_t s_second_at;

static volatile uint64_t s_test_stick_until;
static volatile int s_test_stick_x, s_test_stick_y, s_test_stick_lt;

void rdr_input_test_stick(float x, float y, int lt, int ms)
{
    s_test_stick_x = (int)(x * 32767.0f);
    s_test_stick_y = (int)(y * 32767.0f);
    s_test_stick_lt = lt;
    s_test_stick_until = xbox_HostNowNs() + (uint64_t)ms * 1000000ull;
}

static volatile uint64_t s_test2_until;
static volatile int s_test2_x, s_test2_y;

void rdr_input_test_stick2(float x, float y, int ms)
{
    s_test2_x = (int)(x * 32767.0f);
    s_test2_y = (int)(y * 32767.0f);
    s_test2_until = xbox_HostNowNs() + (uint64_t)ms * 1000000ull;
}

void rdr_input_test_port2(XBOX_INPUT_STATE *st)
{
    if (s_test2_until && xbox_HostNowNs() < s_test2_until) {
        st->Gamepad.sThumbLX = (SHORT)s_test2_x;
        st->Gamepad.sThumbLY = (SHORT)s_test2_y;
        st->dwPacketNumber++;
    }
}

void rdr_input_text_mode(void)
{
    s_text_ns = xbox_HostNowNs();
}

static int text_mode(void)
{
    return s_text_ns && xbox_HostNowNs() - s_text_ns < 250000000ull;
}

void rdr_input_text_push(int c)
{
    unsigned t = s_text_tail;
    if (t - s_text_head >= 64)
        return;
    s_text_q[t % 64] = c;
    s_text_tail = t + 1;
}

int rdr_input_text_take(void)
{
    unsigned h = s_text_head;
    int c;
    if (h == s_text_tail)
        return 0;
    c = s_text_q[h % 64];
    s_text_head = h + 1;
    return c;
}

void rdr_input_text_press(int button)
{
    s_text_press = button;
}


void rdr_input_capture_next(void)
{
    s_captured = 0;
    s_capture_wait = 1;
}

int rdr_input_captured(void)
{
    return s_capture_wait ? 0 : s_captured;
}

static void capture_key(int vk)
{
    if (!s_capture_wait || vk == 0x70 /* F1 */ || vk == 0x71 /* F2 */)
        return;
    /* Esc: leave it as it was; Delete: no key at all (the control unbound). */
    s_captured = vk == 0x1B ? -1 : vk == 0x2E ? -2 : vk;
    s_capture_wait = 0;
}

int rdr_input_capturing(void)
{
    return s_capture_wait;
}

#if defined(_WIN32)

static volatile LONG s_dx, s_dy;
static volatile LONG s_wheel[2];          /* notches not yet pressed: up, down */
static volatile LONG s_free = 0;          /* F9: pointer released */
static HWND s_registered;

static int pointer_page(void);
static int steps_asked(void);

static int playing(int menu_open)
{
    /* Over a page the pointer works, the pointer is the window's own. */
    return g_rdr_settings.mouse_aim && !menu_open && !s_free && !pointer_page();
}

/* Inside Windows' own move or size loop (the title bar or a border held).
 * Confining the pointer to the client area then pulls it down out of the
 * title bar, the loop moves the window after it, the client area moves
 * down, and so on: the window ran off the bottom of the screen. */
static volatile LONG s_moving, s_nc_down;   /* the loop; a title bar press */

static void clip(HWND h, int on)
{
    if (on && !s_moving && !s_nc_down) {
        RECT r;
        POINT a, b;
        if (GetForegroundWindow() != h || !GetClientRect(h, &r))
            return;
        a.x = r.left; a.y = r.top; b.x = r.right; b.y = r.bottom;
        ClientToScreen(h, &a);
        ClientToScreen(h, &b);
        r.left = a.x; r.top = a.y; r.right = b.x; r.bottom = b.y;
        ClipCursor(&r);
    } else {
        ClipCursor(NULL);
    }
}

/* ---- the second keyboard and mouse: raw input by device ---- */
static int s_reg_kbd;                       /* raw keyboard input registered */
static HANDLE s_pick_kbd;                   /* the keyboard just picked */

static void dev_name(HANDLE h, char *out, UINT n)
{
    UINT sz = n;
    out[0] = 0;
    if (!h || GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, out, &sz) == (UINT)-1)
        out[0] = 0;
    out[n - 1] = 0;
}

/* Which set a device is: 1 the second (its name is the one picked), 0
 * any other. Input a program made up (remote desktops, Parsec, on-screen
 * keyboards: no device) is the first player's. */
static int dev_set(HANDLE h, int mouse)
{
    static struct { HANDLE h; int set; } seen[2][16];
    static int nseen[2];
    static unsigned gen;
    char name[256];
    const char *want;
    int i, set;
    if (!h)
        return 0;
    if (gen != s_kb2_gen) {
        nseen[0] = nseen[1] = 0;
        gen = s_kb2_gen;
    }
    for (i = 0; i < nseen[mouse]; i++)
        if (seen[mouse][i].h == h)
            return seen[mouse][i].set;
    want = mouse ? g_rdr_settings.mouse2_device : g_rdr_settings.kb2_device;
    dev_name(h, name, sizeof name);
    set = want[0] && name[0] && !_stricmp(name, want);
    if (nseen[mouse] == 16)
        nseen[mouse] = 0;                   /* (devices come and go: start the list again) */
    seen[mouse][nseen[mouse]].h = h;
    seen[mouse][nseen[mouse]].set = set;
    nseen[mouse]++;
    return set;
}

static void raw_key_event(int set, const RAWKEYBOARD *k)
{
    UINT vk = k->VKey;
    if (!vk || vk >= 255)
        return;                             /* (a key made up by the keyboard's driver) */
    if (vk == VK_SHIFT) vk = k->MakeCode == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    else if (vk == VK_CONTROL) vk = (k->Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) vk = (k->Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
    s_raw_key[set][vk] = (unsigned char)!(k->Flags & RI_KEY_BREAK);
}

static void raw_mouse_buttons(int set, USHORT f)
{
    static const USHORT dn[5] = { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN,
                                  RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
    static const USHORT up[5] = { RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP,
                                  RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP };
    static const int vk[5] = { 0x01, 0x02, 0x04, 0x05, 0x06 };
    int i;
    for (i = 0; i < 5; i++) {
        if (f & dn[i]) {
            s_raw_key[set][vk[i]] = 1;
            s_raw_tap_ns[set][vk[i]] = xbox_HostNowNs();
        }
        if (f & up[i])
            s_raw_key[set][vk[i]] = 0;
    }
}

static void wheel_notches(volatile LONG *w2, SHORT d, int *rest)
{
    int n;
    *rest += d;
    n = *rest / 120;
    *rest -= n * 120;
    if (n > 0) InterlockedAdd(&w2[0], n);
    if (n < 0) InterlockedAdd(&w2[1], -n);
}

/* Raw input before the first player's mouse code: the second set's, or a
 * pick, is taken here (returns 1); the rest goes on as before (0). */
static int kb2_raw(const RAWINPUT *ri, int play, int menu_open)
{
    if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        int press = !(k->Flags & RI_KEY_BREAK);
        if (s_kb2_pick && press && k->VKey == VK_ESCAPE
            && (s_kb2_pick == 1 || ri->header.hDevice != s_pick_kbd)) {
            s_kb2_pick = 0;                 /* Esc: cancelled */
            return 1;
        }
        if (s_kb2_pick == 1 && press && ri->header.hDevice) {
            dev_name(ri->header.hDevice, s_kb2_new_kbd, sizeof s_kb2_new_kbd);
            if (s_kb2_new_kbd[0]) {
                s_pick_kbd = ri->header.hDevice;
                s_kb2_pick = 2;             /* now its mouse */
            }
            return 1;
        }
        if (s_kb2_pick == 2 && press && k->VKey == VK_ESCAPE) {
            s_kb2_new_mouse[0] = 0;         /* Esc on the new keyboard: no mouse of its own */
            s_kb2_pick = 0;
            s_kb2_changed = 1;
            return 1;
        }
        raw_key_event(dev_set(ri->header.hDevice, 0), k);
        return 1;                           /* (the window's key messages still come) */
    }
    if (ri->header.dwType != RIM_TYPEMOUSE)
        return 1;
    {
        const RAWMOUSE *m = &ri->data.mouse;
        int set;
        static int rest1, rest2;
        if (s_kb2_pick == 2 && ri->header.hDevice
            && (m->usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN))) {
            dev_name(ri->header.hDevice, s_kb2_new_mouse, sizeof s_kb2_new_mouse);
            s_kb2_pick = 0;
            s_kb2_changed = 1;
            return 1;
        }
        if (s_kb2_pick)
            return 0;
        set = dev_set(ri->header.hDevice, 1);
        raw_mouse_buttons(set, m->usButtonFlags);
        if (set == 1) {
            if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
                InterlockedAdd((volatile LONG *)&s_dx2, m->lLastX);
                InterlockedAdd((volatile LONG *)&s_dy2, m->lLastY);
            }
            if (m->usButtonFlags & RI_MOUSE_WHEEL)
                wheel_notches((volatile LONG *)s_wheel2, (SHORT)m->usButtonData, &rest2);
            return 1;
        }
        /* The first player's wheel comes from here while the second set is
         * on: the window's WM_MOUSEWHEEL is every mouse's. */
        if (kb2_split() && (m->usButtonFlags & RI_MOUSE_WHEEL) && (play || (!menu_open && rdr_menu_mode())))
            wheel_notches(s_wheel, (SHORT)m->usButtonData, &rest1);
        return 0;
    }
}

int rdr_input_msg(void *hwnd, unsigned msg, uintptr_t w, intptr_t l, int menu_open)
{
    HWND h = (HWND)hwnd;
    int play = playing(menu_open);

    if (msg == WM_ACTIVATEAPP && !w)
    {
        s_focus_lost_ns = xbox_HostNowNs();
        s_focus_lost = 1;                   /* another program in front */
        raw_clear();                        /* (raw input stops: no key's release would come) */
    }

    if (s_capture_wait) {
        int vk = 0;
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) vk = (int)w;
        else if (msg == WM_LBUTTONDOWN) vk = 0x01;
        else if (msg == WM_RBUTTONDOWN) vk = 0x02;
        else if (msg == WM_MBUTTONDOWN) vk = 0x04;
        else if (msg == WM_XBUTTONDOWN) vk = HIWORD(w) == 1 ? 0x05 : 0x06;
        else if (msg == WM_MOUSEWHEEL) vk = (short)HIWORD(w) > 0 ? RDR_VK_WHEEL_UP : RDR_VK_WHEEL_DOWN;
        if (vk && vk != 0x70 && vk != 0x71) {
            capture_key(vk);
            return 1;                      /* the binding's, not the menu's */
        }
    }

    /* The name grid: keys type (with or without mouse and keyboard). Key
     * releases still pass, so nothing held from before sticks. */
    if (!menu_open && text_mode() && (msg == WM_KEYDOWN || msg == WM_CHAR)) {
        int c = 0, repeat = (l & (1 << 30)) != 0;
        if (msg == WM_CHAR)
            return w != 27 && w >= 8;       /* the game never reads them */
        if (w >= 'A' && w <= 'Z') c = (int)w;
        else if (w == VK_SPACE) c = '>';    /* the grid's space */
        else if (w == VK_BACK) c = '<';     /* the grid's delete */
        else if (w == VK_RETURN && !repeat) c = RDR_TEXT_END;
        else if (w == VK_ESCAPE && !repeat) c = RDR_TEXT_CANCEL;
        if (c) {
            rdr_input_text_push(c);
            return 1;
        }
        if (w == VK_RETURN || w == VK_ESCAPE)
            return 1;
    }

    if (!g_rdr_settings.mouse_aim || menu_open == 2)      /* 2: the launcher */
        return 0;
    {
        /* The keyboards too while a second set is given to a player (to
         * tell them apart, and to pick one). */
        int want_kbd = g_rdr_settings.kbm2_player > 0;
        if (s_registered != h || s_reg_kbd != want_kbd) {
            RAWINPUTDEVICE rid[2];
            UINT n = 1;
            rid[0].usUsagePage = 0x01;
            rid[0].usUsage = 0x02;          /* mouse */
            rid[0].dwFlags = 0;             /* only while the window has focus */
            rid[0].hwndTarget = h;
            if (want_kbd || s_reg_kbd) {
                rid[1].usUsagePage = 0x01;
                rid[1].usUsage = 0x06;      /* keyboard */
                rid[1].dwFlags = want_kbd ? 0 : RIDEV_REMOVE;
                rid[1].hwndTarget = want_kbd ? h : NULL;
                n = 2;
            }
            if (RegisterRawInputDevices(rid, n, sizeof rid[0])) {
                s_registered = h;
                s_reg_kbd = want_kbd;
                raw_clear();
            }
        }
    }
    {
        /* The pointer page came or went since the last message: give the
         * pointer back, or take it again. */
        static int was_play = -1;
        if (play != was_play) {
            was_play = play;
            clip(h, play);
        }
    }
    switch (msg) {
    case WM_ENTERSIZEMOVE:
        InterlockedExchange(&s_moving, 1);
        ClipCursor(NULL);
        return 0;
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
        InterlockedExchange(&s_nc_down, 1);
        ClipCursor(NULL);
        return 0;
    case WM_EXITSIZEMOVE:
        InterlockedExchange(&s_moving, 0);
        InterlockedExchange(&s_nc_down, 0);
        clip(h, play);
        return 0;
    case WM_MOUSEMOVE:
        InterlockedExchange(&s_nc_down, 0); /* back in the client area */
        s_ptr_cx = (short)LOWORD(l);
        s_ptr_cy = (short)HIWORD(l);
        s_ptr_gen++;
        return 0;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
            return 0;
        if (s_reg_kbd && kb2_raw(&ri, play, menu_open))
            return 0;                      /* the second set's (or a pick) */
        if (ri.header.dwType == RIM_TYPEMOUSE && (play || (!menu_open && steps_asked()))) {
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                InterlockedAdd(&s_dx, ri.data.mouse.lLastX);
                InterlockedAdd(&s_dy, ri.data.mouse.lLastY);
            } else {
                /* Some laptops' touchpads (and remote desktops, Parsec,
                 * pen tablets) report where the finger is, 0..65535 across
                 * the screen or the desktop, not how far it moved: those
                 * reports were dropped, and the trackpad did nothing (user
                 * report). The move is the difference from the last report,
                 * in screen pixels; a jump of more than an eighth of the
                 * range is a finger put down again, not a move. */
                static LONG lx, ly;
                static int have;
                static DWORD last_ms;
                int vd = (ri.data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                int sw = GetSystemMetrics(vd ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
                int sh = GetSystemMetrics(vd ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
                LONG x = ri.data.mouse.lLastX, y = ri.data.mouse.lLastY;
                DWORD now = GetTickCount();
                if (have && now - last_ms < 250 && labs(x - lx) < 8192 && labs(y - ly) < 8192) {
                    static double fx, fy;              /* the parts of a pixel left over */
                    LONG ix, iy;
                    fx += (double)(x - lx) * (sw > 0 ? sw : 1920) / 65536.0;
                    fy += (double)(y - ly) * (sh > 0 ? sh : 1080) / 65536.0;
                    ix = (LONG)fx; iy = (LONG)fy;
                    fx -= ix; fy -= iy;
                    InterlockedAdd(&s_dx, ix);
                    InterlockedAdd(&s_dy, iy);
                }
                lx = x; ly = y; have = 1; last_ms = now;
            }
            if (play)
                clip(h, 1);
        }
        return 0;                          /* DefWindowProc cleans up */
    }
    case WM_MOUSEWHEEL:
        /* In play the wheel's binding; in the game's menus, steps (also
         * where the pointer shows, rdr_input_merge). */
        if (play || (!menu_open && rdr_menu_mode())) {
            /* One notch (120) is one press of whatever the wheel is bound to. */
            static int rest;
            int n;
            if (kb2_split())
                return 1;                   /* (from raw input then: kb2_raw) */
            rest += (short)HIWORD(w);
            n = rest / 120;
            rest -= n * 120;
            if (n > 0) InterlockedAdd(&s_wheel[0], n);
            if (n < 0) InterlockedAdd(&s_wheel[1], -n);
            return 1;
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_F9 && !(l & (1 << 30))) {
            InterlockedExchange(&s_free, !s_free);
            clip(h, !s_free && !menu_open);
            return 1;
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && play) {
            SetCursor(NULL);
            return 1;
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_ACTIVATE:
    case WM_SIZE:
    case WM_MOVE:
        clip(h, play);
        return 0;
    case WM_KILLFOCUS:
        clip(h, 0);
        raw_clear();
        return 0;
    }
    return 0;
}

/* Called when the menu opens or closes. */
void rdr_input_menu(void *hwnd, int open)
{
    if (hwnd)
        clip((HWND)hwnd, playing(open));
}

#else

int rdr_input_msg(void *hwnd, unsigned msg, uintptr_t w, intptr_t l, int menu_open)
{
    (void)hwnd; (void)msg; (void)w; (void)l; (void)menu_open;
    return 0;
}
void rdr_input_menu(void *hwnd, int open) { (void)hwnd; (void)open; }
static volatile long s_dx, s_dy;
static volatile long s_wheel[2];
#define InterlockedExchange(p, v) __sync_lock_test_and_set((p), (v))

#endif

/* The wheel as buttons: each notch held for 3 polls (the game samples the
 * pad every frame and must see it down), then up for 2 before the next, so
 * fast scrolling still counts every notch -- Mercenaries Recompiled turns
 * the wheel into presses the same way. */
/* A press made up for the game (a wheel notch, a menu step): down long
 * enough for the game to see it on a frame of its own, then up as long, so
 * the next one counts as a new press -- at least 60 ms, and two of the
 * game's frames when it runs slower than that. */
static uint64_t pulse_ns(void)
{
    double f = rdr_frame_seconds();
    uint64_t ns = 60000000ull;
    if (f > 0.0 && f < 0.5 && (uint64_t)(f * 2e9) > ns)
        ns = (uint64_t)(f * 2e9);
    return ns;
}

typedef struct { uint64_t until; int down; } Pulse;

/* One step of a pulse train: returns whether the button is down now. */
static int pulse(Pulse *p, int *queued, uint64_t now)
{
    if (p->until > now)
        return p->down;
    if (p->down) {                          /* the press is over: let go */
        p->down = 0;
        p->until = now + pulse_ns();
        return 0;
    }
    if (*queued > 0) {
        (*queued)--;
        p->down = 1;
        p->until = now + pulse_ns();
        return 1;
    }
    return 0;
}

static int s_wheel_down[2];
static Pulse s_wheel_pulse[2];

static void wheel_step(uint64_t now)
{
    int i;
    for (i = 0; i < 2; i++) {
        int q = (int)s_wheel[i], before = q;
        s_wheel_down[i] = pulse(&s_wheel_pulse[i], &q, now);
        if (q != before) {
#if defined(_WIN32)
            InterlockedDecrement(&s_wheel[i]);
#else
            __sync_fetch_and_sub(&s_wheel[i], 1);
#endif
        }
    }
}

static int key_down(int vk)
{
    if (vk == RDR_VK_WHEEL_UP) return s_wheel_down[0];
    if (vk == RDR_VK_WHEEL_DOWN) return s_wheel_down[1];
    if (kb2_split())
        return raw_down(0, vk);             /* every keyboard and mouse but the second set */
    return vk && xbox_FramebufferKeyDown(vk);
}

/* Mouse look straight into the camera (rdr_overrides.c, on the game's own
 * thread, once a frame): it takes the counts; while it does, the mouse no
 * longer drives the right stick. */
static volatile uint64_t s_direct_ns;

int rdr_input_take_look(long *dx, long *dy)
{
    if (!g_rdr_settings.mouse_aim || rdr_menu_mode())
        return 0;                           /* menus: the counts are theirs */
    *dx = (long)InterlockedExchange(&s_dx, 0);
    *dy = (long)InterlockedExchange(&s_dy, 0);
    s_direct_ns = xbox_HostNowNs();
    return 1;
}

/* The camera hook took counts it is not sure the camera turns with
 * (rdr_overrides.c, mouse_look): the right stick gets them too. */
static volatile long s_share_dx, s_share_dy;

void rdr_input_look_share(long dx, long dy)
{
#if defined(_WIN32)
    InterlockedAdd(&s_share_dx, dx);
    InterlockedAdd(&s_share_dy, dy);
#else
    __sync_fetch_and_add(&s_share_dx, dx);
    __sync_fetch_and_add(&s_share_dy, dy);
#endif
}

/* A duel's aim: the mouse moves the cursor itself (rdr_overrides.c,
 * duel_mouse_aim, on the game's thread, while the aim is on screen). */
static volatile uint64_t s_duel_aim_ns;
static volatile long s_duel_dx, s_duel_dy;

void rdr_input_duel_aim_seen(void)
{
    s_duel_aim_ns = xbox_HostNowNs();
}

int rdr_input_take_duel(long *dx, long *dy)
{
    *dx = (long)InterlockedExchange(&s_duel_dx, 0);
    *dy = (long)InterlockedExchange(&s_duel_dy, 0);
    return 1;
}

/* Tests: the mouse moving dx, dy counts a poll for ms milliseconds. */
static volatile uint64_t s_test_mouse_until;
static volatile long s_test_mdx, s_test_mdy;

void rdr_input_test_mouse_hold(long dx, long dy, int ms)
{
    s_test_mdx = dx;
    s_test_mdy = dy;
    s_test_mouse_until = xbox_HostNowNs() + (uint64_t)ms * 1000000ull;
}

void rdr_input_test_mouse(long dx, long dy)
{
#if defined(_WIN32)
    InterlockedAdd(&s_dx, dx);
    InterlockedAdd(&s_dy, dy);
#else
    __sync_fetch_and_add(&s_dx, dx);
    __sync_fetch_and_add(&s_dy, dy);
#endif
}

/* ---- The mouse in the game's menus (pause menu, front end) ----------
 *
 * The menus are driven by the pad, so the mouse is turned into pad
 * presses: moving it steps the highlight (one step per ~120 counts at 100 %
 * sensitivity, along whichever way it moved most), the wheel steps up and
 * down, left click is A, right click is B. Controls bound to mouse buttons
 * or the wheel are left out while a menu is up, so a click does not also
 * pull a trigger. */
enum { MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_DIRS };
static int s_menu_queue[MENU_DIRS], s_menu_down[MENU_DIRS];
static Pulse s_menu_pulse[MENU_DIRS];
static long s_menu_ax, s_menu_ay;

static void menu_feed(long dx, long dy, int sens)
{
    long step = 12000 / (sens > 0 ? sens : 100);
    s_menu_ax += dx;
    s_menu_ay += dy;
    while (labs(s_menu_ay) >= step && labs(s_menu_ay) >= labs(s_menu_ax)) {
        s_menu_queue[s_menu_ay < 0 ? MENU_UP : MENU_DOWN]++;
        s_menu_ay += s_menu_ay < 0 ? step : -step;
        s_menu_ax = 0;
    }
    while (labs(s_menu_ax) >= step && labs(s_menu_ax) > labs(s_menu_ay)) {
        s_menu_queue[s_menu_ax < 0 ? MENU_LEFT : MENU_RIGHT]++;
        s_menu_ax += s_menu_ax < 0 ? step : -step;
        s_menu_ay = 0;
    }
}

static void menu_step(uint64_t now)
{
    int i;
    for (i = 0; i < MENU_DIRS; i++) {
        /* A long flick queues many steps; a menu has a handful of items. */
        if (s_menu_queue[i] > 8)
            s_menu_queue[i] = 8;
        s_menu_down[i] = pulse(&s_menu_pulse[i], &s_menu_queue[i], now);
    }
}

/* One d-pad step left or right (RDR_HUD_LEFT / RIGHT), unless one is
 * still under way: a slider dragged with the mouse (rdr_overrides.c). */
/* One press held long enough for a Flash menu to see it (the shop, the
 * weapon select: they read the pad at their own pace, and a 60 ms step or
 * a quick click went unseen), short of their 250 ms repeat. */
static volatile int s_swf_press;
static volatile uint64_t s_swf_until;

void rdr_input_swf_press(int code)
{
    if (s_swf_until > xbox_HostNowNs())
        return;
    s_swf_press = code;
    s_swf_until = xbox_HostNowNs() + 180000000ull;
}

/* The menu that took the press has gone (the shop's "Done" opened the
 * weapon select): let go, or the next one takes it too. */
void rdr_input_swf_cancel(void)
{
    s_swf_until = 0;
}

int rdr_input_swf_busy(void)
{
    return s_swf_until + 120000000ull > xbox_HostNowNs();
}

void rdr_input_step(int code)
{
    int d = code == RDR_HUD_LEFT ? MENU_LEFT : code == RDR_HUD_RIGHT ? MENU_RIGHT
          : code == RDR_HUD_UP ? MENU_UP : code == RDR_HUD_DOWN ? MENU_DOWN : -1;
    if (d < 0 || s_menu_queue[d] || s_menu_down[d] || s_menu_pulse[d].until > xbox_HostNowNs())
        return;
    s_menu_queue[d] = 1;
}

static void menu_reset(void)
{
    int i;
    for (i = 0; i < MENU_DIRS; i++) {
        s_menu_queue[i] = s_menu_down[i] = 0;
        s_menu_pulse[i].until = 0;
        s_menu_pulse[i].down = 0;
    }
    s_menu_ax = s_menu_ay = 0;
}

/* ---- Pointing at menu items (the swui pages: pause menu, options) -----
 *
 * The window's pointer is shown over these pages; its position, in the
 * menus' 640x448 space, is read by the hook in rdr_overrides.c, which
 * moves the game's highlight to the item under it. That hook says each
 * frame whether such a page is up and whether the pointer is over an item:
 * then a left click is A only over an item, and the mouse no longer steps
 * the highlight. */

void rdr_input_set_view(int w, int h)
{
    s_view_w = w;
    s_view_h = h;
}

/* The game image's rectangle in the window, as the presenter places it
 * (nv2a_gl.c: centred at 4:3 or 16:9, or stretched). */
extern int xbox_VideoWidescreen(void);
static void image_rect(float *x, float *y, float *w, float *h)
{
    float ww = (float)s_view_w, wh = (float)s_view_h, want, ar;
    *x = 0; *y = 0; *w = ww; *h = wh;
    if (ww <= 0 || wh <= 0 || g_rdr_settings.stretch)
        return;
    want = xbox_VideoWidescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
    ar = ww / wh;
    if (ar > want) {
        *w = wh * want;
        *x = (ww - *w) * 0.5f;
    } else {
        *h = ww / want;
        *y = (wh - *h) * 0.5f;
    }
}

int rdr_input_pointer(float *sx, float *sy, float *u, float *v, unsigned *gen)
{
    float x, y, w, h;
    if (!g_rdr_settings.mouse_aim)
        return 0;
    *gen = s_ptr_gen;
    if (s_test_sx >= 0 || s_test_u > -50) { /* tests */
        *sx = s_test_sx;
        *sy = s_test_sy;
        *u = s_test_u;
        *v = s_test_v;
        return 1;
    }
    if (s_ptr_cx < 0)
        return 0;
    image_rect(&x, &y, &w, &h);
    if (w <= 0 || h <= 0)
        return 0;
    /* The flat pages: the menus' 640x448 space over the whole image. */
    *sx = ((float)s_ptr_cx - x) / w * 640.0f;
    *sy = ((float)s_ptr_cy - y) / h * 448.0f;
    /* The 3D ones (the front end): across from the centre and down from
     * the top, in image heights -- the same in 4:3 and 16:9, since the
     * game widens its view, not narrows it. */
    *u = ((float)s_ptr_cx - (x + w * 0.5f)) / h;
    *v = ((float)s_ptr_cy - y) / h;
    return 1;
}

void rdr_input_pointer_page(int hovering)
{
    s_ptr_page_ns = xbox_HostNowNs();
    s_ptr_hover = hovering;
}

/* A page of items to point at (rdr_overrides.c, menu_pointer). */
static int items_page(void)
{
    return s_ptr_page_ns && xbox_HostNowNs() - s_ptr_page_ns < 250000000ull;
}

/* Button prompts on screen ("A Enter", "B Back", the arrows), clickable:
 * rdr_overrides.c says each frame that some are up, and which one is under
 * the pointer (RDR_HUD_*), if any. */
static volatile uint64_t s_hud_ns, s_hud_hit_ns;
static volatile int s_hud_code;

void rdr_input_hud(int code)
{
    uint64_t now = xbox_HostNowNs();
    s_hud_ns = now;
    if (code) {
        s_hud_code = code;
        s_hud_hit_ns = now;
    }
}

/* The front end's desk: its prompts go while the view turns from one item
 * to the next (a second or so), and with them the pointer page -- the
 * mouse then stepped the d-pad again, and a click made while it moved
 * turned the view on past where it was clicked (user video). On the desk
 * the mouse never steps: a few seconds after it was last seen. */
static volatile uint64_t s_desk_ns;

void rdr_input_desk(void)
{
    s_desk_ns = xbox_HostNowNs();
}

static int desk_on(void)
{
    return s_desk_ns && xbox_HostNowNs() - s_desk_ns < 3000000000ull;
}

static int hud_on(void)
{
    return s_hud_ns && xbox_HostNowNs() - s_hud_ns < 250000000ull;
}

/* The prompt under the pointer, as of the last frame or two. */
static int hud_hit(void)
{
    double f = rdr_frame_seconds();
    uint64_t win = 150000000ull;
    if (f > 0.0 && f < 0.5 && (uint64_t)(f * 3e9) > win)
        win = (uint64_t)(f * 3e9);
    if (!s_hud_hit_ns || xbox_HostNowNs() - s_hud_hit_ns > win)
        return 0;
    return s_hud_code;
}

static volatile uint64_t s_steps_ns;

void rdr_input_steps(void)
{
    s_steps_ns = xbox_HostNowNs();
}

/* The pointer is shown, and the mouse no longer steps the highlight
 * (except where a page asks for the steps, rdr_input_steps). */
static int pointer_page(void)
{
    return items_page() || hud_on();
}

static int steps_asked(void)
{
    return s_steps_ns && xbox_HostNowNs() - s_steps_ns < 250000000ull;
}

static int mouse_steps(void)
{
    return steps_asked() || (!pointer_page() && !desk_on());
}

void rdr_input_test_pointer(float sx, float sy)
{
    s_test_sx = sx;
    s_test_sy = sy;
    s_test_u = -99;
    s_ptr_gen++;
}

void rdr_input_test_pointer_px(float px, float py)
{
    /* A point of a 640x360 (16:9) frame, as the test screenshots are. */
    s_test_u = (px - 320.0f) / 360.0f;
    s_test_v = py / 360.0f;
    s_test_sx = px;
    s_test_sy = py / 360.0f * 448.0f;
    s_ptr_gen++;
}

static int is_mouse_key(int vk)
{
    return vk == 0x01 || vk == 0x02 || vk == 0x04 || vk == 0x05 || vk == 0x06
        || vk == RDR_VK_WHEEL_UP || vk == RDR_VK_WHEEL_DOWN;
}

/* A control's key, except mouse buttons and the wheel while a menu is up. */
static int bound_down(int vk, int menu)
{
    if (menu && is_mouse_key(vk))
        return 0;
    return key_down(vk);
}

/* Whether a control is bound to this key. */
static int key_bound(int vk)
{
    int i;
    for (i = 0; i < RDR_KEY_COUNT; i++)
        if (g_rdr_settings.keys[i] == vk)
            return 1;
    return 0;
}

static int s_menu_enter;                    /* Enter is A in a menu now */

static short clamp16(double v)
{
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (short)v;
}

extern int rdr_duel_active(void);          /* rdr_overrides.c */

/* The controls held as buttons: control, the pad's button (both players). */
static const int s_bind_analog[][2] = {
    { RDR_KEY_A, XBOX_BUTTON_A }, { RDR_KEY_B, XBOX_BUTTON_B },
    { RDR_KEY_X, XBOX_BUTTON_X }, { RDR_KEY_Y, XBOX_BUTTON_Y },
    { RDR_KEY_BLACK, XBOX_BUTTON_BLACK }, { RDR_KEY_WHITE, XBOX_BUTTON_WHITE },
    { RDR_KEY_LT, XBOX_BUTTON_LTRIGGER }, { RDR_KEY_RT, XBOX_BUTTON_RTRIGGER },
    { RDR_KEY_ZOOM, XBOX_BUTTON_BLACK },    /* the game zooms rifles on black */
};
static const int s_bind_digital[][2] = {
    { RDR_KEY_START, XBOX_GAMEPAD_START }, { RDR_KEY_BACK, XBOX_GAMEPAD_BACK },
    { RDR_KEY_DPAD_UP, XBOX_GAMEPAD_DPAD_UP }, { RDR_KEY_DPAD_DOWN, XBOX_GAMEPAD_DPAD_DOWN },
    { RDR_KEY_DPAD_LEFT, XBOX_GAMEPAD_DPAD_LEFT }, { RDR_KEY_DPAD_RIGHT, XBOX_GAMEPAD_DPAD_RIGHT },
    { RDR_KEY_L3, XBOX_GAMEPAD_LEFT_THUMB }, { RDR_KEY_R3, XBOX_GAMEPAD_RIGHT_THUMB },
    /* "To change weapons, press left or right on the D-pad." */
    { RDR_KEY_PREV_WEAPON, XBOX_GAMEPAD_DPAD_LEFT }, { RDR_KEY_NEXT_WEAPON, XBOX_GAMEPAD_DPAD_RIGHT },
    /* "Click the right thumbstick to start deadeye." */
    { RDR_KEY_DEADEYE, XBOX_GAMEPAD_RIGHT_THUMB }, { RDR_KEY_DEADEYE2, XBOX_GAMEPAD_RIGHT_THUMB },
};
#define N_BIND_ANALOG ((int)(sizeof s_bind_analog / sizeof s_bind_analog[0]))
#define N_BIND_DIGITAL ((int)(sizeof s_bind_digital / sizeof s_bind_digital[0]))

void rdr_input_merge(XBOX_INPUT_STATE *st)
{
    static uint64_t last;
    static double sx, sy;                  /* smoothed stick, -1..1 */
    static double duel_qx, duel_qy;        /* a duel's mouse counts not yet paid out */
    int duel;
    const RdrSettings *s = &g_rdr_settings;
    XBOX_GAMEPAD *g = &st->Gamepad;
    uint64_t now;
    double dt, tx = 0, ty = 0, k;
    long dx, dy;
    int i, any = 0;

    int menu, last_menu_logged;
    static int was_menu = -1;

    /* A press asked for: the name grid (Enter: A on End; Esc: B), the
     * level warp (Start, A in the pause menu). */
    {
        static Pulse p;
        static int button;
        int q = s_text_press ? 1 : 0;
        uint64_t t = xbox_HostNowNs();
        if (q && !p.down && p.until <= t)
            button = s_text_press;
        if (pulse(&p, &q, t)) {
            if (button == 3)
                g->wButtons |= XBOX_GAMEPAD_START;
            else if (button == 4)
                g->wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
            else if (button == 5)
                g->wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;
            else if (button == 6)
                g->wButtons |= XBOX_GAMEPAD_DPAD_UP;
            else if (button == 7)
                g->wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
            else
                g->bAnalogButtons[button == 2 ? XBOX_BUTTON_B : XBOX_BUTTON_A] = 255;
            st->dwPacketNumber++;
        }
        if (!q)
            s_text_press = 0;
    }

    /* Tests: the left stick held at a value for a while. */
    if (s_test_stick_until && xbox_HostNowNs() < s_test_stick_until) {
        g->sThumbLX = (SHORT)s_test_stick_x;
        g->sThumbLY = (SHORT)s_test_stick_y;
        if (s_test_stick_lt)
            g->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
        st->dwPacketNumber++;
    }

    if (!s->mouse_aim)
        return;
    now = xbox_HostNowNs();
    if (s_test_mouse_until && now < s_test_mouse_until)
        rdr_input_test_mouse(s_test_mdx, s_test_mdy);
    /* A menu of the game's own, or one drawn over play that the pointer
     * works (a store in town: rdr_swf_menu). */
    menu = rdr_menu_mode() || pointer_page();
    last_menu_logged = was_menu;
    if (menu != was_menu) {
        menu_reset();
        was_menu = menu;
        if (last_menu_logged >= 0) {
            fprintf(stderr, "[INPUT] mouse: %s\n", menu ? "menus" : "play");
            fflush(stderr);
        }
    }
    dt = last ? (double)(now - last) / 1e9 : 0.008;
    last = now;
    if (dt <= 0.0005) dt = 0.0005;
    if (dt > 0.1) dt = 0.1;

    /* Mouse: counts per second -> stick, where the camera does not take
     * the mouse directly (gatling gun, duels, ...). 1600 counts a second at
     * 100 % is a full throw; lifted past a 0.2 deadzone. */
    if (menu) {
        dx = (long)InterlockedExchange(&s_dx, 0);
        dy = (long)InterlockedExchange(&s_dy, 0);
        if (mouse_steps())                  /* pointing does it elsewhere */
            menu_feed(dx, dy, s->mouse_sens);
        while (s_wheel[0] > 0) { s_menu_queue[MENU_UP]++; s_wheel[0]--; }
        while (s_wheel[1] > 0) { s_menu_queue[MENU_DOWN]++; s_wheel[1]--; }
        dx = dy = 0;                      /* no camera in a menu */
    } else if (s_direct_ns && now - s_direct_ns < 150000000ull) {
        dx = (long)InterlockedExchange(&s_share_dx, 0);   /* the camera has them, */
        dy = (long)InterlockedExchange(&s_share_dy, 0);   /* bar what it shares */
    } else {
        dx = (long)InterlockedExchange(&s_dx, 0);
        dy = (long)InterlockedExchange(&s_dy, 0);
    }
    duel = !menu && rdr_duel_active();
    if (duel && s_duel_aim_ns && now - s_duel_aim_ns < 200000000ull) {
        /* The aim: the counts are the cursor's, not the stick's. */
#if defined(_WIN32)
        InterlockedAdd(&s_duel_dx, dx);
        InterlockedAdd(&s_duel_dy, dy);
#else
        __sync_fetch_and_add(&s_duel_dx, dx);
        __sync_fetch_and_add(&s_duel_dy, dy);
#endif
        duel_qx = duel_qy = 0;
        sx = sy = 0;
    } else if (duel) {
        /* A duel's aim moves at the stick's throw (a rate), and slowly: in
         * slow motion. A mouse turned into a rate the usual way saturates
         * at a full throw, and a quick flick lost most of its movement.
         * Here every count is kept: the movement goes into a queue that
         * the stick pays out, at up to a full throw, within ~60 ms, so the
         * cursor goes as far as the mouse did, however fast it moved. */
        double full = 1600.0 / (s->mouse_sens / 100.0) / (s->mouse_duel_sens / 100.0), r;
        duel_qx += dx * (s->mouse_invert_x ? -1 : 1);
        duel_qy += -dy * (s->mouse_invert_y ? -1 : 1);
        tx = duel_qx / (full * 0.06);
        ty = duel_qy / (full * 0.06);
        r = sqrt(tx * tx + ty * ty);
        if (r > 1.0) { tx /= r; ty /= r; }
        duel_qx -= tx * full * dt;
        duel_qy -= ty * full * dt;
        if (fabs(duel_qx) < 0.5) duel_qx = 0;
        if (fabs(duel_qy) < 0.5) duel_qy = 0;
        sx = tx; sy = ty;                   /* no smoothing: it is paced */
    } else {
        duel_qx = duel_qy = 0;
        if (dx || dy) {
            double full = 1600.0 / (s->mouse_sens / 100.0);
            tx = dx / dt / full * (s->mouse_invert_x ? -1 : 1);
            ty = -dy / dt / full * (s->mouse_invert_y ? -1 : 1);
        }
        /* Smooth over ~15 ms, so the poll rate does not jitter the camera. */
        k = 1.0 - exp(-dt / 0.015);
        sx += (tx - sx) * k;
        sy += (ty - sy) * k;
    }
    if (fabs(sx) < 0.002) sx = 0;
    if (fabs(sy) < 0.002) sy = 0;
    if (sx || sy) {
        double r = sqrt(sx * sx + sy * sy), lift, ox, oy;
        lift = (0.2 + 0.8 * (r > 1 ? 1 : r)) / r;
        ox = sx * lift; oy = sy * lift;
        g->sThumbRX = clamp16(ox * 32767.0);
        g->sThumbRY = clamp16(oy * 32767.0);
        any = 1;
        {
            static int dbg = -1;
            static uint64_t last_log;
            if (dbg < 0) dbg = getenv("RDR_INPUT_DEBUG") != NULL;
            if (dbg && now - last_log > 100000000ull) {
                last_log = now;
                fprintf(stderr, "[INPUT] mouse as right stick %d %d\n", g->sThumbRX, g->sThumbRY);
                fflush(stderr);
            }
        }
    }

    /* Menus: the mouse as d-pad steps, A and B. */
    if (menu) {
        static const unsigned short dir_bit[MENU_DIRS] = {
            XBOX_GAMEPAD_DPAD_UP, XBOX_GAMEPAD_DPAD_DOWN,
            XBOX_GAMEPAD_DPAD_LEFT, XBOX_GAMEPAD_DPAD_RIGHT,
        };
        menu_step(now);
        for (i = 0; i < MENU_DIRS; i++)
            if (s_menu_down[i]) {
                g->wButtons |= dir_bit[i];
                any = 1;
            }
        /* Left click: the button of the prompt under the pointer ("B
         * Back", an arrow); else A -- over an item, on a page of items.
         * Chosen as the button goes down and kept while it is held. */
        {
            static int act;                 /* RDR_HUD_*, 0 none, -1 nothing */
            if (!(kb2_split() ? raw_down(0, 0x01) : xbox_FramebufferKeyDown(0x01)))
                act = 0;
            else if (!act) {
                act = hud_hit();
                if (!act)
                    act = !items_page() || s_ptr_hover ? RDR_HUD_A : -1;
            }
            switch (act) {
            case RDR_HUD_A: g->bAnalogButtons[XBOX_BUTTON_A] = 255; any = 1; break;
            case RDR_HUD_B: g->bAnalogButtons[XBOX_BUTTON_B] = 255; any = 1; break;
            case RDR_HUD_X: g->bAnalogButtons[XBOX_BUTTON_X] = 255; any = 1; break;
            case RDR_HUD_Y: g->bAnalogButtons[XBOX_BUTTON_Y] = 255; any = 1; break;
            case RDR_HUD_LEFT: g->wButtons |= XBOX_GAMEPAD_DPAD_LEFT; any = 1; break;
            case RDR_HUD_RIGHT: g->wButtons |= XBOX_GAMEPAD_DPAD_RIGHT; any = 1; break;
            case RDR_HUD_START: g->wButtons |= XBOX_GAMEPAD_START; any = 1; break;
            case RDR_HUD_DOWN: g->wButtons |= XBOX_GAMEPAD_DPAD_DOWN; any = 1; break;
            case RDR_HUD_UP: g->wButtons |= XBOX_GAMEPAD_DPAD_UP; any = 1; break;
            case RDR_HUD_LEFT2: case RDR_HUD_RIGHT2:
                /* the first step now, the second once the view has turned */
                g->wButtons |= act == RDR_HUD_LEFT2 ? XBOX_GAMEPAD_DPAD_LEFT : XBOX_GAMEPAD_DPAD_RIGHT;
                any = 1;
                s_second_press = act == RDR_HUD_LEFT2 ? 4 : 5;
                s_second_at = now + 900000000ull;
                break;
            }
            if (s_second_press && !act && now >= s_second_at) {
                rdr_input_text_press(s_second_press);
                s_second_press = 0;
            }
        }
        if (kb2_split() ? raw_down(0, 0x02) : xbox_FramebufferKeyDown(0x02)) {
            g->bAnalogButtons[XBOX_BUTTON_B] = 255;
            any = 1;
        }
        if (s_swf_until > now) {
            switch (s_swf_press) {
            case RDR_HUD_A: g->bAnalogButtons[XBOX_BUTTON_A] = 255; break;
            case RDR_HUD_Y: g->bAnalogButtons[XBOX_BUTTON_Y] = 255; break;
            case RDR_HUD_LEFT: g->wButtons |= XBOX_GAMEPAD_DPAD_LEFT; break;
            case RDR_HUD_RIGHT: g->wButtons |= XBOX_GAMEPAD_DPAD_RIGHT; break;
            case RDR_HUD_UP: g->wButtons |= XBOX_GAMEPAD_DPAD_UP; break;
            case RDR_HUD_DOWN: g->wButtons |= XBOX_GAMEPAD_DPAD_DOWN; break;
            }
            any = 1;
        }
    }

    /* Menus from the keyboard, as in a PC game: Enter picks (A), Esc and
     * Backspace go back (B); in play Esc pauses (Start). A key still held
     * when the menus come or go waits for its release, so the Esc or Enter
     * that opened the pause menu does not also act in it. Enter, Start's
     * key, is A instead while a menu is up (see the digital keys below). */
    {
        static const int vks[3] = { 0x0D, 0x1B, 0x08 };
        static int armed[3], last_mode = -1;
        int k, down[3];
        for (k = 0; k < 3; k++) {
            down[k] = kb2_split() ? raw_down(0, vks[k]) : xbox_FramebufferKeyDown(vks[k]);
            if (menu != last_mode || !down[k])
                armed[k] = !down[k];
        }
        last_mode = menu;
        if (menu) {
            if (armed[0] && down[0]) { g->bAnalogButtons[XBOX_BUTTON_A] = 255; any = 1; }
            if ((armed[1] && down[1]) || (armed[2] && down[2])) {
                g->bAnalogButtons[XBOX_BUTTON_B] = 255;
                any = 1;
            }
        } else if (armed[1] && down[1] && !key_bound(0x1B)) {
            g->wButtons |= XBOX_GAMEPAD_START;
            any = 1;
        }
        /* Enter is A in the menus; in the front end Start as well, which
         * its screens ask for ("Press START" on the poster and the match's
         * loading screen); not in the pause menu, which Start closes. */
        if (menu && armed[0] && down[0] && !rdr_pause_menu()) {
            g->wButtons |= XBOX_GAMEPAD_START;
            any = 1;
        }
        s_menu_enter = menu && down[0];
    }

    /* A key the game has seen held for 10 s straight is logged (every 10 s
     * while it lasts): a stuck key overrides the controller's stick. */
    {
        static uint64_t held_since[RDR_KEY_COUNT], logged_at[RDR_KEY_COUNT];
        for (i = 0; i < RDR_KEY_COUNT; i++) {
            if (!bound_down(s->keys[i], menu)) {
                held_since[i] = 0;
                continue;
            }
            if (!held_since[i])
                held_since[i] = now;
            else if (now - held_since[i] > 10000000000ull && now - logged_at[i] > 10000000000ull) {
                logged_at[i] = now;
                fprintf(stderr, "[INPUT] %s (%s) held for %.0f s\n", rdr_control_name(i),
                        rdr_key_name(s->keys[i]), (double)(now - held_since[i]) * 1e-9);
                fflush(stderr);
            }
        }
    }

    /* Keys. */
    wheel_step(now);
    {
        int mx = 0, my = 0;
        if (bound_down(s->keys[RDR_KEY_MOVE_UP], menu)) my++;
        if (bound_down(s->keys[RDR_KEY_MOVE_DOWN], menu)) my--;
        if (bound_down(s->keys[RDR_KEY_MOVE_LEFT], menu)) mx--;
        if (bound_down(s->keys[RDR_KEY_MOVE_RIGHT], menu)) mx++;
        if (mx || my) {
            /* A diagonal: 0.707 on each axis (the stick's circle) when
             * running, which the game handles. With the gun up (the left
             * trigger's key held) it walked instead of strafing on the
             * diagonal at 0.707 each; there both go to full throw, as a
             * pad's stick pushed into its corner. */
            int aiming = bound_down(s->keys[RDR_KEY_LT], menu);
            double n = (mx && my && !aiming) ? 0.7071 : 1.0;
            g->sThumbLX = clamp16(mx * n * 32767.0);
            g->sThumbLY = clamp16(my * n * 32767.0);
            any = 1;
        }
    }
    for (i = 0; i < N_BIND_ANALOG; i++)
        if (bound_down(s->keys[s_bind_analog[i][0]], menu)) {
            g->bAnalogButtons[s_bind_analog[i][1]] = 255;
            any = 1;
        }
    for (i = 0; i < N_BIND_DIGITAL; i++)
        if (bound_down(s->keys[s_bind_digital[i][0]], menu)
            && !(s_menu_enter && s->keys[s_bind_digital[i][0]] == 0x0D)) {
            g->wButtons |= (unsigned short)s_bind_digital[i][1];
            any = 1;
        }
    if (any)
        st->dwPacketNumber++;
}

/* ---- The second keyboard and mouse's player (rdr_kbm2_port) ---------
 *
 * The same bindings as the first player's, from the second set's keys
 * and buttons. Its mouse turns that player's camera directly where the
 * camera takes it (rdr_overrides.c, mouse_look, the second mouse's turn:
 * rdr_input_take_look2), else it is the right stick, as the first
 * player's is. In the game's menus: Enter and a left click are A, Esc,
 * Backspace and a right click B. */
static volatile uint64_t s_direct2_ns;
static volatile long s_share2_dx, s_share2_dy;
static int s_wheel2_down[2];
static Pulse s_wheel2_pulse[2];

int rdr_input_take_look2(long *dx, long *dy)
{
    if (rdr_kbm2_port() < 0 || rdr_menu_mode())
        return 0;
    *dx = (long)InterlockedExchange(&s_dx2, 0);
    *dy = (long)InterlockedExchange(&s_dy2, 0);
    s_direct2_ns = xbox_HostNowNs();
    return 1;
}

void rdr_input_look_share2(long dx, long dy)
{
#if defined(_WIN32)
    InterlockedAdd((volatile LONG *)&s_share2_dx, dx);
    InterlockedAdd((volatile LONG *)&s_share2_dy, dy);
#else
    __sync_fetch_and_add(&s_share2_dx, dx);
    __sync_fetch_and_add(&s_share2_dy, dy);
#endif
}

static int key2(int vk, int menu)
{
    if (menu && is_mouse_key(vk))
        return 0;
    if (vk == RDR_VK_WHEEL_UP) return s_wheel2_down[0];
    if (vk == RDR_VK_WHEEL_DOWN) return s_wheel2_down[1];
    return raw_down(1, vk);
}

void rdr_input_merge2(XBOX_INPUT_STATE *st)
{
    static uint64_t last;
    static double sx, sy;
    static int armed[3], last_mode = -1;
    static const int vks[3] = { 0x0D, 0x1B, 0x08 };
    const RdrSettings *s = &g_rdr_settings;
    XBOX_GAMEPAD *g = &st->Gamepad;
    uint64_t now = xbox_HostNowNs();
    double dt, tx = 0, ty = 0, k;
    long dx, dy;
    int menu, any = 0, i, down[3], enter = 0;

    if (rdr_kbm2_port() < 0)
        return;
    menu = rdr_menu_mode();
    dt = last ? (double)(now - last) / 1e9 : 0.008;
    last = now;
    if (dt <= 0.0005) dt = 0.0005;
    if (dt > 0.1) dt = 0.1;

    /* the wheel: each notch one press of what it is bound to */
    for (i = 0; i < 2; i++) {
        int q = (int)s_wheel2[i], before = q;
        s_wheel2_down[i] = pulse(&s_wheel2_pulse[i], &q, now);
        if (q != before) {
#if defined(_WIN32)
            InterlockedDecrement((volatile LONG *)&s_wheel2[i]);
#else
            __sync_fetch_and_sub(&s_wheel2[i], 1);
#endif
        }
    }

    /* the mouse: the camera's directly, or the right stick */
    if (menu) {
        InterlockedExchange(&s_dx2, 0);
        InterlockedExchange(&s_dy2, 0);
        dx = dy = 0;
    } else if (s_direct2_ns && now - s_direct2_ns < 150000000ull) {
        dx = (long)InterlockedExchange(&s_share2_dx, 0);
        dy = (long)InterlockedExchange(&s_share2_dy, 0);
    } else {
        dx = (long)InterlockedExchange(&s_dx2, 0);
        dy = (long)InterlockedExchange(&s_dy2, 0);
    }
    if (dx || dy) {
        double full = 1600.0 / (s->mouse_sens / 100.0);
        tx = dx / dt / full * (s->mouse_invert_x ? -1 : 1);
        ty = -dy / dt / full * (s->mouse_invert_y ? -1 : 1);
    }
    k = 1.0 - exp(-dt / 0.015);
    sx += (tx - sx) * k;
    sy += (ty - sy) * k;
    if (fabs(sx) < 0.002) sx = 0;
    if (fabs(sy) < 0.002) sy = 0;
    if (sx || sy) {
        double r = sqrt(sx * sx + sy * sy), lift = (0.2 + 0.8 * (r > 1 ? 1 : r)) / r;
        g->sThumbRX = clamp16(sx * lift * 32767.0);
        g->sThumbRY = clamp16(sy * lift * 32767.0);
        any = 1;
    }

    /* menus: Enter A, Esc and Backspace B, the mouse's buttons A and B; in
     * play Esc is Start (unless a control has it). A key held as the menus
     * come or go waits for its release. */
    for (i = 0; i < 3; i++) {
        down[i] = raw_down(1, vks[i]);
        if (menu != last_mode || !down[i])
            armed[i] = !down[i];
    }
    last_mode = menu;
    if (menu) {
        if ((armed[0] && down[0]) || raw_down(1, 0x01)) { g->bAnalogButtons[XBOX_BUTTON_A] = 255; any = 1; }
        if ((armed[1] && down[1]) || (armed[2] && down[2]) || raw_down(1, 0x02)) {
            g->bAnalogButtons[XBOX_BUTTON_B] = 255;
            any = 1;
        }
        if (armed[0] && down[0] && !rdr_pause_menu()) { g->wButtons |= XBOX_GAMEPAD_START; any = 1; }
        enter = down[0];
    } else if (armed[1] && down[1] && !key_bound(0x1B)) {
        g->wButtons |= XBOX_GAMEPAD_START;
        any = 1;
    }

    /* the bindings */
    {
        int mx = 0, my = 0;
        if (key2(s->keys[RDR_KEY_MOVE_UP], menu)) my++;
        if (key2(s->keys[RDR_KEY_MOVE_DOWN], menu)) my--;
        if (key2(s->keys[RDR_KEY_MOVE_LEFT], menu)) mx--;
        if (key2(s->keys[RDR_KEY_MOVE_RIGHT], menu)) mx++;
        if (mx || my) {
            int aiming = key2(s->keys[RDR_KEY_LT], menu);
            double n = (mx && my && !aiming) ? 0.7071 : 1.0;
            g->sThumbLX = clamp16(mx * n * 32767.0);
            g->sThumbLY = clamp16(my * n * 32767.0);
            any = 1;
        }
    }
    for (i = 0; i < N_BIND_ANALOG; i++)
        if (key2(s->keys[s_bind_analog[i][0]], menu)) {
            g->bAnalogButtons[s_bind_analog[i][1]] = 255;
            any = 1;
        }
    for (i = 0; i < N_BIND_DIGITAL; i++)
        if (key2(s->keys[s_bind_digital[i][0]], menu) && !(enter && s->keys[s_bind_digital[i][0]] == 0x0D)) {
            g->wButtons |= (unsigned short)s_bind_digital[i][1];
            any = 1;
        }
    if (any)
        st->dwPacketNumber++;
}
