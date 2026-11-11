/*
 * rdr_freecam.c - a free camera.
 *
 * The game keeps the camera's world matrix at [0x00678828] -- rows right, up,
 * back (the camera looks down -back) and position, y up -- and the view
 * matrix, its inverse, at [0x00678720]; sub_00180870 builds the view-
 * projection and culling from them each time the camera is set (rdr_hooks.c
 * hooks its entry). While the free camera is on, its own pair goes there,
 * so everything the game draws is seen from here; the game's own camera
 * carries on underneath and is back the moment it is switched off.
 *
 * The controller (or the keys, which drive the same virtual pad) is taken
 * from the game while it is on: left stick moves, right stick looks, right
 * trigger / A rises, left trigger / X sinks, the left stick click (Shift)
 * goes four times as fast. With mouse and keyboard the mouse looks.
 *
 * Which of them fly it is a choice (settings freecam_kbm, freecam_pads):
 * the keyboard and mouse, and each player's controller, either fly the
 * camera or play on -- for filming, the keyboard on the camera and the
 * players moving under it (user: a trailer, two controllers in co-op).
 */
#include "rdr_freecam.h"
#include "rdr_settings.h"
#include "rdr_cheats.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <xbox/xboxrecomp.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern uint64_t xbox_HostNowNs(void);
extern int rdr_input_take_look(long *dx, long *dy);
extern int rdr_port_live_pad(int port);

#define RDR_CAMERA_WORLD  0x00678828u   /* the camera's world matrix, 4x4 */
#define RDR_CAMERA_VIEW   0x00678720u   /* its inverse, the view matrix */

volatile int g_rdr_freecam;

static volatile int s_ready;            /* pose taken from the game's camera */
static volatile float s_pos[3], s_yaw, s_pitch;
/* Per port: every controller (and the keys, merged into the port the game
 * reads them on) flies it -- it read port 0 alone, and with player 1's
 * controller on another port (Controls page) it never moved. */
static volatile short s_plx[5], s_ply[5], s_prx[5], s_pry[5];   /* [4]: the keyboard */
static volatile int s_pup[5], s_pdown[5], s_pfast[5];
static volatile uint64_t s_hint_until;

static float *gf(uint32_t va)
{
    return (float *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

static void basis(float r[3], float u[3], float b[3])
{
    float cp = cosf(s_pitch), sp = sinf(s_pitch), cy = cosf(s_yaw), sy = sinf(s_yaw);
    float f[3] = { cp * sy, sp, cp * cy };      /* forward */
    float n;
    b[0] = -f[0]; b[1] = -f[1]; b[2] = -f[2];
    r[0] = b[2]; r[1] = 0.0f; r[2] = -b[0];     /* up x back, up = y */
    n = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (n < 1e-6f) { r[0] = 1.0f; r[2] = 0.0f; n = 1.0f; }
    r[0] /= n; r[2] /= n;
    u[0] = b[1] * r[2] - b[2] * r[1];           /* back x right */
    u[1] = b[2] * r[0] - b[0] * r[2];
    u[2] = b[0] * r[1] - b[1] * r[0];
}

static void take_pose(void)
{
    const float *m = gf(RDR_CAMERA_WORLD);
    float f[3] = { -m[8], -m[9], -m[10] };
    if (!(m[15] > 0.5f && m[15] < 1.5f) || !isfinite(m[12]))
        return;
    s_pos[0] = m[12]; s_pos[1] = m[13]; s_pos[2] = m[14];
    if (f[1] > 1.0f) f[1] = 1.0f;
    if (f[1] < -1.0f) f[1] = -1.0f;
    s_pitch = asinf(f[1]);
    s_yaw = atan2f(f[0], f[2]);
    s_ready = 1;
}

int rdr_freecam_active(void)
{
    return g_rdr_freecam;
}

static void take(int slot, const XBOX_GAMEPAD *g)
{
    if (!g) {
        s_plx[slot] = s_ply[slot] = s_prx[slot] = s_pry[slot] = 0;
        s_pup[slot] = s_pdown[slot] = s_pfast[slot] = 0;
        return;
    }
    s_plx[slot] = g->sThumbLX; s_ply[slot] = g->sThumbLY;
    s_prx[slot] = g->sThumbRX; s_pry[slot] = g->sThumbRY;
    s_pup[slot] = g->bAnalogButtons[XBOX_BUTTON_RTRIGGER] > 40 || g->bAnalogButtons[XBOX_BUTTON_A] > 40;
    s_pdown[slot] = g->bAnalogButtons[XBOX_BUTTON_LTRIGGER] > 40 || g->bAnalogButtons[XBOX_BUTTON_X] > 40;
    s_pfast[slot] = (g->wButtons & XBOX_GAMEPAD_LEFT_THUMB) != 0;
}

static short bigger(short a, short b)
{
    return (b < 0 ? -b : b) > (a < 0 ? -a : a) ? b : a;
}

int rdr_freecam_takes_mouse(void)
{
    return g_rdr_freecam && !g_rdr_settings.freecam_kbm;
}

void rdr_freecam_pad(unsigned port, XBOX_INPUT_STATE *st, const XBOX_INPUT_STATE *kb)
{
    XBOX_GAMEPAD *g = &st->Gamepad;
    int pad_plays, i;
    if (!g_rdr_freecam || port >= 4)
        return;
    pad_plays = (g_rdr_settings.freecam_pads >> port) & 1;
    if (pad_plays)
        take((int)port, NULL);
    else {
        /* The game gets nothing of this controller but Start (its pause menu). */
        unsigned short keep = g->wButtons & XBOX_GAMEPAD_START;
        take((int)port, g);
        memset(g, 0, sizeof *g);
        g->wButtons = keep;
    }
    if (kb) {
        const XBOX_GAMEPAD *k = &kb->Gamepad;
        if (g_rdr_settings.freecam_kbm) {
            /* the keyboard plays: on top of what the controller gives */
            take(4, NULL);
            g->wButtons |= k->wButtons;
            for (i = 0; i < 8; i++)
                if (k->bAnalogButtons[i] > g->bAnalogButtons[i])
                    g->bAnalogButtons[i] = k->bAnalogButtons[i];
            g->sThumbLX = bigger(g->sThumbLX, k->sThumbLX);
            g->sThumbLY = bigger(g->sThumbLY, k->sThumbLY);
            g->sThumbRX = bigger(g->sThumbRX, k->sThumbRX);
            g->sThumbRY = bigger(g->sThumbRY, k->sThumbRY);
        } else {
            take(4, k);
            g->wButtons |= k->wButtons & XBOX_GAMEPAD_START;
        }
    }
    st->dwPacketNumber++;
}

int rdr_freecam_hint(char *out, int n)
{
    char fly[128] = "", play[128] = "";
    int p, kbp = g_rdr_settings.kbm_player;
    if (!s_hint_until || xbox_HostNowNs() > s_hint_until)
        return 0;
    if (!g_rdr_freecam) {
        snprintf(out, (size_t)n, "Free camera off");
        return 1;
    }
    if (g_rdr_settings.freecam_kbm)
        snprintf(play, sizeof play, "keyboard (player %d)", kbp);
    else
        snprintf(fly, sizeof fly, "keyboard and mouse");
    for (p = 0; p < 4; p++) {
        char *d;
        size_t len;
        if (!rdr_port_live_pad(p))
            continue;
        d = ((g_rdr_settings.freecam_pads >> p) & 1) ? play : fly;
        len = strlen(d);
        snprintf(d + len, sizeof fly - len, "%splayer %d's controller", len ? ", " : "", p + 1);
    }
    snprintf(out, (size_t)n, "Free camera. Flying it: %s.  Playing: %s.  (F1 to change)",
             fly[0] ? fly : "nobody", play[0] ? play : "nobody");
    return 1;
}

static float stick(short v)
{
    float f = v / 32767.0f;
    if (f > -0.2f && f < 0.2f)
        return 0.0f;
    return f > 0 ? (f - 0.2f) / 0.8f : (f + 0.2f) / 0.8f;
}

/* The HUD is models drawn just in front of the game's camera (the bullets,
 * the dynamite, the health bar): seen from the free camera they hung in the
 * air where the camera had been (user report). The game's No HUD cheat
 * (bit 14) hides them while the free camera is on, and No Distortion (bit
 * 15) the old-film blur, which kept the last frames over the picture as
 * the camera flew (ghosts: user video); both as they were after. (The
 * letterbox bars: rdr_hooks.c, sub_001D5F80.) */
static void freecam_hud(int on)
{
    static const int bits[2] = { 14, 15 };
    static int hidden, was[2];
    int i;
    if (on && !hidden && rdr_cheats_available()) {
        for (i = 0; i < 2; i++) {
            was[i] = rdr_cheat_get(bits[i]);
            if (!was[i])
                rdr_cheat_set(bits[i], 1);
        }
        hidden = 1;
    } else if (!on && hidden) {
        for (i = 0; i < 2; i++)
            if (!was[i] && rdr_cheats_available())
                rdr_cheat_set(bits[i], 0);
        hidden = 0;
    }
}

void rdr_freecam_tick(void)
{
    static uint64_t last;
    uint64_t now = xbox_HostNowNs();
    float dt = last ? (float)((now - last) * 1e-9) : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    freecam_hud(g_rdr_freecam != 0);
    {
        static int was;
        if (g_rdr_freecam != was) {
            was = g_rdr_freecam;
            s_hint_until = now + 5000000000ull;
        }
    }
    if (!g_rdr_freecam) {
        s_ready = 0;
        return;
    }
    if (!s_ready) {
        take_pose();
        return;
    }
    {
        float r[3], u[3], b[3];
        float speed, look = 2.0f * dt;
        float mx = 0, mz = 0, lx = 0, ly = 0;
        int up = 0, down = 0, fast = 0, p;
        for (p = 0; p < 5; p++) {
            mx += stick(s_plx[p]); mz += stick(s_ply[p]);
            lx += stick(s_prx[p]); ly += stick(s_pry[p]);
            up |= s_pup[p]; down |= s_pdown[p]; fast |= s_pfast[p];
        }
        mx = mx > 1 ? 1 : mx < -1 ? -1 : mx;
        mz = mz > 1 ? 1 : mz < -1 ? -1 : mz;
        lx = lx > 1 ? 1 : lx < -1 ? -1 : lx;
        ly = ly > 1 ? 1 : ly < -1 ? -1 : ly;
        float yaw, pitch;
        long dx = 0, dy = 0;
        int i;
        speed = (fast ? 24.0f : 6.0f) * dt;
        yaw = s_yaw - lx * look;
        pitch = s_pitch + ly * look;
        if (g_rdr_settings.mouse_aim && rdr_freecam_takes_mouse() && rdr_input_take_look(&dx, &dy)) {
            float k = 0.0022f * (float)g_rdr_settings.mouse_sens / 100.0f;
            yaw += (g_rdr_settings.mouse_invert_x ? dx : -dx) * k;
            pitch += (g_rdr_settings.mouse_invert_y ? dy : -dy) * k;
        }
        if (pitch > 1.5f) pitch = 1.5f;
        if (pitch < -1.5f) pitch = -1.5f;
        s_yaw = yaw;
        s_pitch = pitch;
        basis(r, u, b);
        for (i = 0; i < 3; i++)
            s_pos[i] += (r[i] * mx - b[i] * mz) * speed;
        s_pos[1] += ((up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f)) * speed;
    }
}

void rdr_freecam_apply(void)
{
    float r[3], u[3], b[3], *w, *v;
    int i;
    if (!g_rdr_freecam)
        return;
    if (!s_ready) {
        take_pose();                    /* the game's camera, as it stands */
        if (!s_ready)
            return;
    }
    w = gf(RDR_CAMERA_WORLD);
    v = gf(RDR_CAMERA_VIEW);
    basis(r, u, b);
    /* world: rows right, up, back, position */
    for (i = 0; i < 3; i++) {
        w[i] = r[i]; w[4 + i] = u[i]; w[8 + i] = b[i]; w[12 + i] = s_pos[i];
    }
    w[3] = w[7] = w[11] = 0.0f;
    w[15] = 1.0f;
    /* view = its inverse: the rotation transposed, then -position in it */
    for (i = 0; i < 3; i++) {
        v[i * 4 + 0] = r[i];
        v[i * 4 + 1] = u[i];
        v[i * 4 + 2] = b[i];
        v[i * 4 + 3] = 0.0f;
    }
    v[12] = -(s_pos[0] * r[0] + s_pos[1] * r[1] + s_pos[2] * r[2]);
    v[13] = -(s_pos[0] * u[0] + s_pos[1] * u[1] + s_pos[2] * u[2]);
    v[14] = -(s_pos[0] * b[0] + s_pos[1] * b[1] + s_pos[2] * b[2]);
    v[15] = 1.0f;
}
