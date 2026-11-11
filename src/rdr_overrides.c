/*
 * rdr_overrides.c - Red Dead Revolver specific runtime patches.
 *
 * Anything the generic runtime cannot do for this title goes here, keyed by
 * guest VA and documented with why. recomp_manual.c's recomp_lookup_manual()
 * consults rdr_lookup_override() first; main.c calls rdr_runtime_init()
 * before entering the game.
 */
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#  include <time.h>
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <xbox/xboxrecomp.h>   /* RECOMP_TLS */
#include "ui/rdr_cheats.h"
#include "ui/rdr_settings.h"
#include "ui/rdr_input.h"

typedef void (*recomp_func_t)(void);

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);

extern RECOMP_TLS uint32_t g_ecx, g_esp;
extern void sub_003BFAA2(void);
extern void sub_002730A0(void);
extern void sub_00019BD0(void);
extern RECOMP_TLS uint32_t g_eax, g_edx;
extern RECOMP_TLS uint32_t g_eax;

/* ── Controller report length guard ─────────────────────────────
 *
 * XAPI's gamepad driver (XID) takes each input report in its URB
 * completion routine (sub_003BF95C), which calls the device's
 * vtable+0x24 = sub_003BFAA2 with ecx = the device. That copies
 * (length - 2) bytes from the report (+0x34) into the device's state
 * (+0x14, room for 32), length being the URB's transfer length at +0x66
 * as the USB driver left it. Once, deep into a play session on the test
 * machine, it was 0xFFFFF106 -- a race between the emulated USB host
 * controller (a host thread) and the guest's USB driver -- and the copy ran
 * off the end of memory. Real hardware never reports more than the 20-byte
 * report, so a length outside 2..34 is dropped: the pad keeps its last
 * state for one poll instead of the game crashing. */
static void guarded_xid_report(void)
{
    uint32_t len = *(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + g_ecx + 0x66);
    if (len < 2 || len > 34) {
        static int logged;
        if (logged++ < 8) {
            fprintf(stderr, "[RDR] controller report with length %u (0x%08X) dropped\n", len, len);
            fflush(stderr);
        }
        g_esp += 4;                         /* ret */
        return;
    }
    sub_003BFAA2();
}

/* ── Refresh period at frame rates above 60 ─────────────────────
 *
 * [0x00409DAC] is the length of one screen refresh, set once when D3D
 * starts (sub_0017D3B0: 1/60 s, or 1/50 on PAL). The game's frame time
 * comes from the CPU clock (sub_00083690), so play is right at any rate,
 * but a few things step by this period once per frame instead: the
 * blocking fade loop of the level title cards and transitions
 * (sub_00105B40: t += swap interval * period, 0x001061AC), a progress
 * animation (0x0006FBA0: progress += period / length), and two more
 * (0x00066CAF, 0x001F4883). With the vblank at 240 Hz the frames come four
 * times as often, so they ran four times too fast.
 *
 * The first fix set the period to 1 / vblank rate. That is right only when
 * the frames really come at that rate: with the cap above what the monitor
 * shows under vsync (240 or Unlimited on a 144 Hz screen), or a machine that
 * cannot reach the cap, frames came slower and the same things ran slow
 * instead (a 240 Hz monitor under vsync with Unlimited: the vblank at
 * 500, the title cards flipping every second one, and every flip waiting
 * for the monitor too -- about half speed). So the period is now the
 * measured time between presents divided by the swap interval in force
 * ([0x004097B8], vblanks per flip: the title cards use 2), smoothed, and
 * kept between 1 / vblank rate and 1/30 s -- one vblank's worth of real
 * time, which is what the console's 1/60 was. */
extern int xbox_VblankHz(void);
extern void xbox_VblankSetHz(int hz);
#define RDR_REFRESH_PERIOD  0x00409DACu
#define RDR_SWAP_MODE       0x0040987Cu   /* as asked: 0-6 */
#define RDR_SWAP_INTERVAL   0x004097B8u   /* vblanks per flip: 0-3 */

static double host_seconds(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

double rdr_now_seconds(void) { return host_seconds(); }

static void host_sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

#define RDR_TRANSITION_ON   0x00663338u   /* byte: a screen fade's thread runs */

/* ── Sound voices in use (diagnostic) ──────────────────────────
 *
 * sndControl's 60 voices (0x006841E8, 0xC0 each: +0x14 the sound, -1
 * free; +0x16 its state). A user lost every gun sound part way into a
 * preview match while music and cries went on: if voices leak, this shows
 * it in the log -- every 20 s, when the count changed: how many are held,
 * by state, and the sound held longest. */
static void sound_census(void)
{
    static double last_t;
    static int last_busy = -1;
    static int16_t tag_seen[60];
    static double since[60];
    double now = host_seconds();
    int i, busy = 0, oldest = -1;
    int st_n[4] = {0, 0, 0, 0};
    uint16_t st_v[4] = {0, 0, 0, 0};
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    if (now - last_t < 20.0)
        return;
    last_t = now;
    for (i = 0; i < 60; i++) {
        uint32_t v = 0x006841E8u + (uint32_t)i * 0xC0u;
        int16_t tag = *(volatile int16_t *)(m + v + 0x14);
        uint16_t st = *(volatile uint16_t *)(m + v + 0x16);
        int k;
        if (tag != tag_seen[i] || since[i] == 0.0) {
            tag_seen[i] = tag;
            since[i] = now;
        }
        if (tag == -1)
            continue;
        busy++;
        for (k = 0; k < 4; k++)
            if (st_n[k] == 0 || st_v[k] == st) {
                st_v[k] = st;
                st_n[k]++;
                break;
            }
        if (oldest < 0 || since[i] < since[oldest])
            oldest = i;
    }
    if (busy == last_busy)
        return;
    last_busy = busy;
    fprintf(stderr, "[SND] voices held %d of 60 (state %x:%d %x:%d %x:%d %x:%d)",
            busy, st_v[0], st_n[0], st_v[1], st_n[1], st_v[2], st_n[2], st_v[3], st_n[3]);
    if (oldest >= 0)
        fprintf(stderr, "; longest: voice %d, sound %d, %.0f s", oldest,
                (int)tag_seen[oldest], now - since[oldest]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static double s_present_avg;
static int s_period_mode;               /* 0 not seen yet, 1 ours, 2 PAL */

void rdr_level_rate_tick(void);
void rdr_timing_tick(void)
{
    uintptr_t base = (uintptr_t)xbox_GetMemoryOffset();
    volatile float *p = (volatile float *)(base + RDR_REFRESH_PERIOD);
    static double last;
    double now = host_seconds(), dt, lo, hi;
    float cur = *p, want;
    int hz;
    rdr_level_rate_tick();
    hz = xbox_VblankHz();
    int32_t iv = *(volatile int32_t *)(base + RDR_SWAP_INTERVAL);

    /* A fade between screens (the one after the Rockstar logo, the level
     * transitions): sub_00105B40 on its own thread, [0x00663338] set while
     * it runs. Its fades are incremental -- each frame subtracts from the
     * picture (blend op reverse-subtract) the fade's growth since that
     * buffer was last drawn, rounded to a whole step of 255 -- so above
     * about 100 frames a second every step rounds to nothing and the
     * picture holds still until the next screen cuts in (the logo seemed
     * to hang for five seconds). The console flipped these at 60: they
     * are paced to 60 here, on the presenting thread (the guest waits for
     * its flips, and the loading the fade covers goes on meanwhile). */
    if (*(volatile uint8_t *)(base + RDR_TRANSITION_ON) && last > 0.0) {
        double until = last + 1.0 / 60.0;
        if (now < until && until - now < 0.05) {
            while ((now = host_seconds()) + 0.0015 < until)
                host_sleep_ms(1);
            while ((now = host_seconds()) < until)
                ;
        }
    }
    dt = last > 0.0 ? now - last : 0.0;
    last = now;
    sound_census();
    {
        /* lockstep (rdr_lockstep.c): the console's 1/60, not a measure */
        extern int rdr_lockstep_mode(void);
        if (rdr_lockstep_mode()) {
            if (cur > 0.001f && cur < 0.05f)
                *p = 1.0f / 60.0f;
            return;
        }
    }
    if (hz < 30 || hz > 500)
        return;
    if (!s_period_mode) {
        if (!(cur > 0.001f && cur < 0.05f))
            return;                         /* not set up yet */
        /* The game's own value, before any of ours: PAL 50 Hz is left be. */
        s_period_mode = cur > 0.0195f && cur < 0.0205f ? 2 : 1;
    }
    if (s_period_mode != 1)
        return;
    lo = 1.0 / hz;
    hi = 1.0 / 30.0;
    if (iv < 1 || iv > 3)
        iv = 1;
    {
        static int32_t last_iv;
        if (iv != last_iv)                  /* a new pace: start over */
            s_present_avg = 0.0;
        last_iv = iv;
    }
    if (dt > 0.0 && dt < 0.25) {
        dt /= iv;
        /* A frame held by loading counts for little; the average follows
         * the rate within a few dozen frames. */
        if (dt > hi)
            dt = hi;
        s_present_avg = s_present_avg > 0.0 ? s_present_avg + (dt - s_present_avg) * 0.1 : dt;
    }
    want = (float)(s_present_avg > 0.0 ? s_present_avg : lo);
    if (want < lo) want = (float)lo;
    if (want > hi) want = (float)hi;
    if (cur != want)
        *p = want;
}

/* Tests: the period now in use. */
void rdr_timing_report(void)
{
    fprintf(stderr, "[TEST] refresh period %.6f (avg %.6f, vblank %d Hz, mode %d interval %d)\n",
            *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + RDR_REFRESH_PERIOD),
            s_present_avg, xbox_VblankHz(),
            *(volatile int32_t *)((uintptr_t)xbox_GetMemoryOffset() + RDR_SWAP_MODE),
            *(volatile int32_t *)((uintptr_t)xbox_GetMemoryOffset() + RDR_SWAP_INTERVAL));
    fflush(stderr);
}

/* ── Mouse look ───────────────────────────────────────────────
 *
 * The camera is CamMach, a small dataflow program loaded from basecam.ccm
 * and evaluated every frame. It does not read the pad: the player code
 * fills its input channels (sub_0013EC00), among them
 *   channel 14  aim, horizontal = sub_0014CA40(right stick x)
 *   channel 15  aim, vertical   = sub_0014CB10(right stick y)
 * -- a deadzone (0.25), a logarithmic curve, a jump to a top rate at full
 * throw (2.3 across, 0.8 up and down), times the sensitivity slider. The
 * program takes them as turn rates: BaseAimRate (ZoomedAimRate zoomed) x
 * channel x frame time is added to the camera's bearing and tilt (groups
 * G18/G19 and their users in basecam.ccm). So the camera turns linearly
 * with these two numbers; only the stick's shaping is in the way.
 *
 * Mercenaries Recompiled adds raw mouse counts to its camera's angles. The
 * equivalent here: add counts x angle-per-count / (frame time x K) to the
 * two channels, K being the camera's turn rate per unit -- measured by
 * injecting known counts and reading the camera's bearing and tilt back
 * (channels 20 and 21): 1.25 rad/s across, 1.0 up and down, linear (500
 * counts and -1000 counts gave exactly 1:-2). The game's own stick value
 * stays in, so a pad works alongside.
 *
 * Where: the channels are reached through the container's GetChannel
 * (vtable +4, sub_002730A0: channels[i]), an indirect call, so it can be
 * wrapped here. Right after sub_0013EC00 stores channels 14 and 15 it asks
 * for channel 0x1E -- the call that returns to 0x0013F694. At that moment,
 * on the game's thread, both are written and not yet read: the mouse is
 * added to them then. Every other caller goes straight through. */
#define RDR_CHANNEL_VT      0x003F09D4u     /* a float channel's vtable */
#define RDR_FRAME_SECONDS   0x00406504u     /* the game clock's frame time */
#define RDR_LOOK_RET        0x0013F694u
#define RDR_LOOK_KX         1.25f           /* rad/s per unit, across */
#define RDR_LOOK_KY         1.0f            /* rad/s per unit, up and down */
#define RDR_LOOK_RAD        0.0012f         /* rad per count at 100 % */

extern RECOMP_TLS uint32_t g_edi;
static uint32_t guest_rd32(uint32_t va);
extern int rdr_input_take_look(long *dx, long *dy);
extern uint64_t xbox_HostNowNs(void);
extern int rdr_mouse_settings(int *sens, int *invert_x, int *invert_y);   /* rdr_settings.c */

static int g_look_frames;                   /* frames the mouse was offered */

static volatile float *look_channel(uint32_t cont, uint32_t i)
{
    uint32_t arr = guest_rd32(cont + 4), ch;
    if (arr < 0x80000000u || arr >= 0x84000000u)
        return NULL;
    ch = guest_rd32(arr + i * 4u);
    if (ch < 0x80000000u || ch >= 0x84000000u)
        return NULL;
    if (guest_rd32(ch) != RDR_CHANNEL_VT || guest_rd32(ch + 4) != 2)
        return NULL;                        /* not a float channel */
    return (volatile float *)((uintptr_t)xbox_GetMemoryOffset() + ch + 8);
}

#define RDR_FRAME_COUNT     0x0065AA98u     /* the game clock's frame number */

/* Per mouse: [0] the keyboard-and-mouse player's, [1] the second
 * keyboard and mouse's (rdr_input.c, rdr_kbm2_port). */
typedef struct {
    uint32_t frame;
    uint64_t dead_ns;                       /* the camera ignored the mouse */
    float ax, ay;                           /* this frame's additions */
    float last_b, last_t;
    int still;
} LookState;
static LookState s_look[2];
extern void rdr_input_look_share(long dx, long dy);
extern void rdr_input_look_share2(long dx, long dy);
extern int rdr_input_take_look2(long *dx, long *dy);
extern int rdr_kbm2_port(void);
static int g_look_calls;

extern volatile int g_rdr_freecam;
extern int rdr_freecam_takes_mouse(void);

/* A duel is on for the player the mouse belongs to: its duel object
 * ([character + 0x190], what the scripts' "duel_state" reads) has a state
 * other than 0 (0 none yet, 2 grab and draw, 4 aim and fire...). */
int rdr_duel_active(void)
{
    uint32_t idx = guest_rd32(0x00652024u), p, o;
    if (idx > 3)
        idx = 0;
    p = guest_rd32(0x00675930u + idx * 4u);
    if (p < 0x80000000u || p >= 0x84000000u)
        return 0;
    o = guest_rd32(p + 0x190);
    if (o < 0x80000000u || o >= 0x84000000u)
        return 0;
    return guest_rd32(o) != 0;
}

static void mouse_look(uint32_t cont, int slot)
{
    LookState *L = &s_look[slot & 1];
    uint32_t frame = guest_rd32(RDR_FRAME_COUNT);
    volatile float *ax, *ay;

    if (g_rdr_freecam && rdr_freecam_takes_mouse())
        return;                             /* the mouse is the free camera's */

    g_look_calls++;
    if (frame != L->frame) {
        /* A new frame: this frame's mouse movement, as rates for the
         * frame's length. Re-added on every call within the frame, since
         * the channels are written afresh each time. */
        long dx = 0, dy = 0;
        int sens = 100, inv_x = 0, inv_y = 0;
        float dt = *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + RDR_FRAME_SECONDS);
        float a;
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("RDR_LOOK_DEBUG") != NULL;
        if (dbg && (frame & 63) == 0) {
            fprintf(stderr, "[LOOK] frame %u: %d calls since the last, dt %.4f\n", frame, g_look_calls, dt);
            fflush(stderr);
        }
        g_look_calls = 0;
        L->frame = frame;
        /* In a duel the mouse is the right stick's, wholly: grab, draw and
         * the aim read the stick, and the duel's camera moves by itself,
         * which the test below could take for turning with the mouse
         * (then the mouse went nowhere in that direction). */
        if (rdr_duel_active()) {
            L->dead_ns = xbox_HostNowNs();
            L->ax = L->ay = 0;
            return;
        }
        /* A camera that reads these channels but does not turn with them
         * (a duel's, the gatling gun's, a scripted one): the mouse added
         * to them for 4 frames running with the view's bearing and tilt
         * (channels 20, 21) unchanged. The mouse then goes to the right
         * stick (rdr_input.c), which those cameras and the duel's grab,
         * draw and aim read, for half a second at a time; between those,
         * while it is tried again on the channels, its movement is given
         * to the stick as well, so nothing is lost while the duel lasts,
         * until the camera is seen turning with it again. */
        {
            volatile float *b = look_channel(cont, 20), *t = look_channel(cont, 21);
            uint64_t now = xbox_HostNowNs();
            if (b && t) {
                float db = *b - L->last_b;
                int moved = db * db + (*t - L->last_t) * (*t - L->last_t) > 1e-10f;
                /* turned the mouse's way (channel 14 positive: the bearing
                 * grows), not a camera moving on its own */
                float dtl = *t - L->last_t;
                int with = (db * L->ax > 0.0f && db > -3.0f && db < 3.0f)
                           || (L->ay && dtl * dtl > 1e-10f);
                /* (Up and down against the tilt's limit counts too: then
                 * the stick gets it, which does nothing there either.) */
                if (L->ax || L->ay)
                    L->still = with ? 0 : L->still + 1;
                else if (moved)
                    L->still = 0;
                if (with && L->dead_ns
                    && now - L->dead_ns >= 500000000ull) {
                    L->dead_ns = 0;     /* it turns with the mouse again */
                    if (dbg) {
                        fprintf(stderr, "[LOOK] the camera turns with the mouse again\n");
                        fflush(stderr);
                    }
                }
                L->last_b = *b;
                L->last_t = *t;
            }
            if (L->still >= 4) {
                L->still = 0;
                if (!L->dead_ns && dbg) {
                    fprintf(stderr, "[LOOK] the camera does not turn with the mouse: right stick\n");
                    fflush(stderr);
                }
                L->dead_ns = now;
            }
            if (L->dead_ns && now - L->dead_ns < 500000000ull) {
                L->ax = L->ay = 0;
                return;                     /* the stick's */
            }
            if (L->dead_ns && now - L->dead_ns > 10000000000ull)
                L->dead_ns = 0;             /* long unused: start afresh */
        }
        L->ax = L->ay = 0;
        if (!(slot ? rdr_input_take_look2(&dx, &dy) : rdr_input_take_look(&dx, &dy)))
            return;
        if (L->dead_ns) {                   /* being tried: the stick's too */
            if (slot) rdr_input_look_share2(dx, dy);
            else rdr_input_look_share(dx, dy);
        }
        g_look_frames++;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.5f) dt = 0.5f;
        if (dx || dy) {
            rdr_mouse_settings(&sens, &inv_x, &inv_y);
            a = RDR_LOOK_RAD * (float)sens / 100.0f / dt;
            /* Channel 14 positive turns the view left (the bearing grows);
             * mouse right (dx > 0) turns right. Channel 15 positive tilts
             * down; mouse up (dy < 0) looks up. */
            L->ax = (float)(inv_x ? dx : -dx) * a / RDR_LOOK_KX;
            L->ay = (float)(inv_y ? -dy : dy) * a / RDR_LOOK_KY;
            if (dbg) {
                fprintf(stderr, "[LOOK] frame %u: mouse %d: %ld %ld -> %.3f %.3f\n", frame, slot + 1, dx, dy, L->ax, L->ay);
                fflush(stderr);
            }
        }
    }
    if (!(L->ax || L->ay))
        return;
    ax = look_channel(cont, 14);
    ay = look_channel(cont, 15);
    if (!ax || !ay)
        return;
    *ax += L->ax;
    *ay += L->ay;
}

static uint32_t g_look_cont;

/* Tests (RDR_OVERLAY_FLAG "look"): the camera's bearing and tilt as the
 * player publishes them (channels 20 and 21, from the view direction). */
void rdr_mouse_look_report(void)
{
    volatile float *b = g_look_cont ? look_channel(g_look_cont, 20) : NULL;
    volatile float *t = g_look_cont ? look_channel(g_look_cont, 21) : NULL;
    fprintf(stderr, "[TEST] look: frames %d bearing %.4f tilt %.4f\n", g_look_frames,
            b ? *b : -99.0f, t ? *t : -99.0f);
    fflush(stderr);
}

/* The mouse is one player's: with several local players (Showdown's split
 * screen, co-op) each fills its own camera's channels here (sub_0013EC00,
 * the player in edi), and only the keyboard-and-mouse player's camera may
 * turn with it -- the one whose pad object ([player+0x894], one per port at
 * 0x006795C8) is that player's port; when none is, the first player. */
extern int rdr_kbm_port(void);                  /* rdr_settings.c: 0-3 */

static int mouse_is_for(uint32_t player)
{
    uint32_t n = guest_rd32(0x0067592Cu), pad, i;
    if (n <= 1 || n > 4)
        return 1;
    pad = 0x006795C8u + (uint32_t)rdr_kbm_port() * 0xB0u;
    if (guest_rd32(player + 0x894) == pad)
        return 1;
    for (i = 0; i < n; i++)
        if (guest_rd32(guest_rd32(0x00675930u + i * 4) + 0x894) == pad)
            return 0;                           /* someone else's camera */
    return player == guest_rd32(0x00675930u);
}

static uint32_t s_look_cont2;               /* the second mouse's camera (tests) */
static float guest_rdf(uint32_t va);
static int in_heap(uint32_t va);

/* Tests (RDR_OVERLAY_FLAG "players"): where each player is, the port its
 * pad object is, and each mouse's camera bearing. */
void rdr_test_players_report(void)
{
    uint32_t n = guest_rd32(0x0067592Cu), k;
    volatile float *b1 = g_look_cont ? look_channel(g_look_cont, 20) : NULL;
    volatile float *b2 = s_look_cont2 ? look_channel(s_look_cont2, 20) : NULL;
    for (k = 0; k < n && k < 4; k++) {
        uint32_t c = guest_rd32(0x00675930u + k * 4u);
        if (!in_heap(c))
            continue;
        fprintf(stderr, "[TEST] player %u at %.2f %.2f %.2f, pad of port %d\n", k + 1, guest_rdf(c + 0x4C),
                guest_rdf(c + 0x50), guest_rdf(c + 0x54), (int)((guest_rd32(c + 0x894) - 0x006795C8u) / 0xB0u) + 1);
    }
    fprintf(stderr, "[TEST] camera bearings: mouse 1 %.4f, mouse 2 %.4f\n", b1 ? *b1 : -99.0f, b2 ? *b2 : -99.0f);
    fflush(stderr);
}

/* The second keyboard and mouse's player's camera (its pad object is that
 * port's), with two or more players. */
static int mouse2_is_for(uint32_t player)
{
    uint32_t n = guest_rd32(0x0067592Cu);
    int p = rdr_kbm2_port();
    if (p < 0 || n <= 1 || n > 4)
        return 0;
    return guest_rd32(player + 0x894) == 0x006795C8u + (uint32_t)p * 0xB0u;
}

static void wrapped_get_channel(void)
{
    if (guest_rd32(g_esp) == RDR_LOOK_RET) {
        if (mouse2_is_for(g_edi)) {
            s_look_cont2 = g_ecx;
            mouse_look(g_ecx, 1);
        } else if (mouse_is_for(g_edi)) {
            g_look_cont = g_ecx;
            mouse_look(g_ecx, 0);
        }
    }
    sub_002730A0();
}

int rdr_mouse_look_frames(void)
{
    return g_look_frames;
}

/* ── Pointing at menu items ───────────────────────────────────
 *
 * The in-game menus (pause, options, the confirmation boxes) are swui
 * pages, loaded from .swui files: [0x00651FEC] is the page on screen.
 *   page (vtables 0x003C7270 pause, 0x003C7470 options, ...):
 *     +0x40   widget*[], +0x298 count, +0x31C focused widget index
 *     sub_00019BD0(index, 1) = SetFocus: unhighlights the old widget
 *       (vtable +0x14), highlights the new (+0x10), stores the index
 *     vtable +8 = Update, called every frame for each page on the stack
 *       by the manager (0x00651ED4, sub_00029E30; [mgr+0x118] = current
 *       page = [0x00651FEC]): sub_0001ABD0 for most, its own for some
 *   button (vtable 0x003C62E0, built by sub_0001AD30 from <BUTTON:>):
 *     +0x4C/+0x50 position in the 640x448 menu space, +0x58/+0x5C the
 *     page's layout offset (the pause page spaces its items itself),
 *     +0x114 the text shown, +0x190 the font settings: +4 byte 0 =
 *     CENTER (centred across the screen), +8/+0xC scale; +0xD8 and
 *     +0xE6 nonzero = can take the focus; +0xE4 = has it.
 * Text widths are estimated from the scale (14.8 units a character at
 * 1.0, 26 high -- measured against the pause and options screens).
 *
 * The wrapper runs on the game's thread just before the page's update:
 * when the pointer has moved, the button under it gets the focus through
 * the game's own SetFocus, so it lights up as with the d-pad; a click
 * then presses A (rdr_input.c). */
#define RDR_CUR_PAGE        0x00651FECu
#define RDR_BUTTON_VT       0x003C62E0u

extern int  rdr_input_pointer(float *sx, float *sy, float *u, float *v, unsigned *gen);
extern int  rdr_in_level(void);
extern void rdr_input_pointer_page(int hovering);

static unsigned g_ptr_gen_seen;

static float guest_rdf(uint32_t va)
{
    return *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

static uint8_t guest_rd8(uint32_t va)
{
    return *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

static void guest_wr32(uint32_t va, uint32_t v)
{
    *(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va) = v;
}

static void guest_wr16(uint32_t va, uint16_t v)
{
    *(volatile uint16_t *)((uintptr_t)xbox_GetMemoryOffset() + va) = v;
}

static void guest_wr8(uint32_t va, uint8_t v)
{
    *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + va) = v;
}

static int in_heap(uint32_t va)
{
    return (va >= 0x00010000u && va < 0x04000000u) || (va >= 0x80000000u && va < 0x84000000u);
}

/* ── The demo held off while our menus are open ─────────────────────
 *
 * Left alone on the desk, the front end plays its demo: the screen
 * manager's front-end screen ([0x0065A810]) keeps its transitions in a
 * list at +8 (nodes {transition, next}, sub_00064830), and
 * FrontEndToAttract's (vtable 0x003CBA90, made in sub_0005F340) condition,
 * sub_000644F0, takes the frame's time off a float at +0xC and starts the
 * demo when it runs out; a press on any pad sets it again. In our menus
 * (the online lobby above all: user, "the game's demo mode takes over and
 * starts playing") the pads are the menu's, so the game sees nobody and
 * the time ran out. While a menu of ours is open, or the lobby is
 * hosting or joining, each such timer is kept at what it was when they
 * opened. On the game's thread, at the pad reads (rdr_hooks.c). */
#define RDR_FRONTEND_SCREEN   0x0065A810u
#define RDR_IDLE_TRANSITION   0x003CBA90u
extern int rdr_overlay_menu_open(void);
extern int rdr_lobby_busy(void);

void rdr_attract_hold_tick(void)
{
    static uint32_t held_t[8];
    static float held_v[8];
    static int nheld;
    uint32_t scr, node;
    int n, k;
    if (!rdr_overlay_menu_open() && !rdr_lobby_busy()) {
        nheld = 0;
        return;
    }
    scr = guest_rd32(RDR_FRONTEND_SCREEN);
    if (!in_heap(scr))
        return;
    for (node = guest_rd32(scr + 8), n = 0; in_heap(node) && n < 64; node = guest_rd32(node + 4), n++) {
        uint32_t t = guest_rd32(node);
        volatile float *v;
        if (!in_heap(t) || guest_rd32(t) != RDR_IDLE_TRANSITION)
            continue;
        v = (volatile float *)((uintptr_t)xbox_GetMemoryOffset() + t + 0x0C);
        for (k = 0; k < nheld && held_t[k] != t; k++) {}
        if (k == nheld) {
            if (nheld == 8)
                continue;
            held_t[nheld] = t;
            held_v[nheld++] = *v > 1.0f ? *v : 1.0f;
            fprintf(stderr, "[MENU] the demo held off (its timer at %.1f)\n", (double)*v);
        } else if (*v < held_v[k]) {
            *v = held_v[k];
        }
    }
}

static void set_focus(uint32_t page, uint32_t index)
{
    uint32_t ecx = g_ecx;
    volatile uint32_t *m;
    g_esp -= 12;
    m = (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + g_esp);
    m[0] = 0x0001AA60u;                     /* a return address */
    m[1] = index;
    m[2] = 1;
    g_ecx = page;
    sub_00019BD0();                         /* ret 8: pops all three */
    g_ecx = ecx;
}

/* Front-end pages drawn onto 3D objects -- the book, the profile sheet --
 * measured from the game's own frames (16:9, 640x360): where a button at
 * page position (x0, y0) shows, how far a page unit goes across and down,
 * and the size of an item there, all in pixels of that frame. Screen
 * positions are taken relative to the image's height (u, v), which the
 * game keeps in 4:3 too. */
typedef struct {
    const char *page;
    float x0, px0, kx, y0, py0, ky;     /* page -> 640x360 frame */
    float w, h;                         /* an item, frame pixels */
    float kq, slant;                    /* the book leans away: rows spread
                                         * further down (frame px per unit^2)
                                         * and start further left (px/unit) */
} PageCal;

static const PageCal s_page_cal[] = {
    /* the book's menu: New / Change Player / Player Options */
    /* (re-measured with six rows, Continue to Story Co-op, from the user's
     * video: centres 185/204/221/240/262/289 for page y 110..210) */
    { "storyselect", 40.0f, 171.0f, 0.61f, 130.0f, 202.0f, 0.858f, 120.0f, 18.0f,
      0.00268f, 0.12f },
    /* Gunslinger Profiles: the six name boxes */
    { "profile",     53.0f,  77.0f, 0.856f, 193.0f, 164.0f, 0.807f, 136.0f, 24.0f, 0.0f, 0.0f },
};

static const PageCal *page_cal(uint32_t page)
{
    char name[24];
    unsigned i;
    for (i = 0; i < sizeof name - 1; i++) {
        name[i] = (char)guest_rd8(page + 0x1C + i);
        if (!name[i])
            break;
    }
    name[sizeof name - 1] = 0;
    for (i = 0; i < sizeof s_page_cal / sizeof s_page_cal[0]; i++)
        if (!strcmp(name, s_page_cal[i].page))
            return &s_page_cal[i];
    return NULL;
}

/* Front-end pages drawn flat over the whole picture, as the pause menu's
 * are (not onto the book or the poster): Player Options and the options
 * pages behind it (while current), and the pop-ups that are never the
 * manager's current page: Showdown Options, difficulty, warning, save. */
static int front_flat(uint32_t page)
{
    static const char *const flat[] = {
        "gameplayeropt", "gamesound", "gamepicture", "videooptions", "mainoptions",
        "mpmode", "difficultyselect", "warningfrontend", "savefrontend",
    };
    char name[24];
    unsigned i;
    for (i = 0; i < sizeof name - 1; i++)
        if (!(name[i] = (char)guest_rd8(page + 0x1C + i)))
            break;
    name[sizeof name - 1] = 0;
    for (i = 0; i < sizeof flat / sizeof flat[0]; i++)
        if (!strcmp(name, flat[i]))
            return i < 5 ? 1 : 2;           /* 2: shown on top, not "current" */
    return 0;
}

static void menu_pointer(uint32_t page, const PageCal *cal)
{
    uint32_t n = guest_rd32(page + 0x298), i;
    float px, py, u, v, best = 1e9f;
    unsigned gen;
    int hit = -1, buttons = 0;
    static int dbg = -1;

    if (n == 0 || n > 64)
        return;
    if (!rdr_input_pointer(&px, &py, &u, &v, &gen))
        return;
    if (cal) {                              /* in the 640x360 frame */
        px = 320.0f + u * 360.0f;
        py = v * 360.0f;
    }
    if (dbg < 0) dbg = getenv("RDR_MENU_DEBUG") ? atoi(getenv("RDR_MENU_DEBUG")) : 0;
    for (i = 0; i < n; i++) {
        uint32_t w = guest_rd32(page + 0x40 + i * 4), font;
        float x, y, sx = 1.0f, sy = 1.0f, wd, ht, left, top, cx, cy, d;
        int len = 0, center = 0;
        if (!in_heap(w) || guest_rd32(w) != RDR_BUTTON_VT)
            continue;
        if (!guest_rd8(w + 0xD8) || !guest_rd8(w + 0xE6))
            continue;
        buttons++;
        x = guest_rdf(w + 0x4C) + guest_rdf(w + 0x58);
        y = guest_rdf(w + 0x50) + guest_rdf(w + 0x5C);
        font = guest_rd32(w + 0x190);
        if (in_heap(font)) {
            center = guest_rd8(font + 4) != 0;
            sx = guest_rdf(font + 8);
            sy = guest_rdf(font + 0xC);
            if (!(sx > 0.05f && sx < 10.0f)) sx = 1.0f;
            if (!(sy > 0.05f && sy < 10.0f)) sy = 1.0f;
        }
        while (len < 40 && guest_rd8(w + 0x114 + len))
            len++;
        if (cal) {
            float dy = y - cal->y0;
            /* rows further down the leaning page are taller and further
             * apart: the item's height grows with the spacing there */
            wd = cal->w;
            ht = cal->h * (cal->ky + 2.0f * cal->kq * dy) / cal->ky;
            left = cal->px0 + (x - cal->x0) * cal->kx - cal->slant * dy - 6.0f;
            top = cal->py0 + dy * cal->ky + dy * dy * cal->kq - ht * 0.5f;
        } else {
            wd = (float)len * 14.8f * sx + 8.0f;
            ht = 26.0f * sy + 6.0f;
            left = center ? 320.0f - wd * 0.5f : x - 4.0f;
            top = y - 3.0f;
        }
        if (dbg > 1)
            fprintf(stderr, "[MENU]   item %u '%.20s' rect %.0f,%.0f %.0fx%.0f\n", i,
                    (const char *)((uintptr_t)xbox_GetMemoryOffset() + w + 0x114), left, top, wd, ht);
        if (px < left || px > left + wd || py < top || py > top + ht)
            continue;
        cx = left + wd * 0.5f;
        cy = top + ht * 0.5f;
        d = (px - cx) * (px - cx) + (py - cy) * (py - cy);
        if (d < best) {
            best = d;
            hit = (int)i;
        }
    }
    if (!buttons)
        return;                             /* nothing to point at here */
    rdr_input_pointer_page(hit >= 0);
    if (gen == g_ptr_gen_seen)
        return;                             /* not moved: leave the d-pad be */
    g_ptr_gen_seen = gen;
    if (dbg) {
        fprintf(stderr, "[MENU] pointer %.0f,%.0f over %d (focus %d)\n", px, py, hit,
                (int)guest_rd32(page + 0x31C));
        fflush(stderr);
    }
    if (hit >= 0 && (uint32_t)hit != guest_rd32(page + 0x31C))
        set_focus(page, (uint32_t)hit);
}

/* Page Update methods (vtable +8 of the 35 swui page classes; 0x0009E9A0
 * is an empty one): each wrapped, the pointer applied first. 0x000359C0
 * is the book's chapter page (vtable 0x003C7AF8: the level warp). */
#define PAGE_UPDATES(X) X(0001ABD0) X(0002FB80) X(0002F110) X(0002F240) \
    X(0002F1C0) X(00032430) X(00034FA0) X(0002D6E0) X(00037700) X(0002EDC0) \
    X(0002B1A0) X(00039840) X(0003F4B0) X(000359C0)

/* Tests (RDR_OVERLAY_FLAG "page"): the page on screen. */
void rdr_menu_test_report(void)
{
    uint32_t page = guest_rd32(RDR_CUR_PAGE);
    char name[24] = "-";
    unsigned i;
    if (in_heap(page))
        for (i = 0; i < sizeof name - 1; i++)
            if (!(name[i] = (char)guest_rd8(page + 0x1C + i)))
                break;
    name[sizeof name - 1] = 0;
    fprintf(stderr, "[TEST] page %08X %s focus %d shown %d\n", page, name,
            in_heap(page) ? (int)guest_rd32(page + 0x31C) : -9,
            in_heap(page) ? guest_rd8(page + 0x3D) : -1);
    if (in_heap(page)) {
        /* its buttons: page position and label, for calibrating */
        uint32_t n = guest_rd32(page + 0x298), k;
        for (k = 0; k < n && k < 64; k++) {
            uint32_t w = guest_rd32(page + 0x40 + k * 4);
            char label[41];
            unsigned j;
            if (!in_heap(w) || guest_rd32(w) != RDR_BUTTON_VT)
                continue;
            for (j = 0; j < 40 && (label[j] = (char)guest_rd8(w + 0x114 + j)); j++)
                ;
            label[j] = 0;
            fprintf(stderr, "[TEST]   button %u at %.1f,%.1f vis %d,%d \"%s\"\n", k,
                    guest_rdf(w + 0x4C) + guest_rdf(w + 0x58), guest_rdf(w + 0x50) + guest_rdf(w + 0x5C),
                    guest_rd8(w + 0xD8), guest_rd8(w + 0xE6), label);
        }
    }
    fflush(stderr);
}

/* ── Typing a name ─────────────────────────────────────────────
 *
 * Gunslinger Profiles (page vtable 0x003C7948) picks a name from a letter
 * grid. Its input handler (vtable +0x18, sub_00033FF0) while +0x44E is set:
 * the grid "ABCDEFGHIJKLMNOPQRSTUVWXYZ<>" then End ([0x00403FA6], '\n') at
 * 0x00403F88, +0x468 cells, [0x0040405C] to a row, the cursor's row +0x46C
 * and column +0x470; A on a cell is sub_00033EF0(ch) (thiscall, ret 4):
 * '<' deletes (sub_0003B550), '>' is a space, anything else is appended
 * to the name at +0x45C (+0x464 long, 7 at most); A on End checks and
 * keeps the name; B leaves the grid. Typed keys go through the same
 * sub_00033EF0, so the game's limit, sounds and drawing all apply; Enter
 * puts the cursor on End and presses A, Esc presses B. */
#define RDR_PROFILE_VT      0x003C7948u
#define RDR_GRID            0x00403F88u
#define RDR_GRID_END        0x00403FA6u
#define RDR_GRID_COLS       0x0040405Cu

extern void sub_00033EF0(void);
extern void sub_000336B0(void);
extern void rdr_input_text_mode(void);
extern int  rdr_input_text_take(void);
extern void rdr_input_text_press(int button);

static void grid_char(uint32_t page, int c)
{
    uint32_t ecx = g_ecx;
    volatile uint32_t *m;
    g_esp -= 8;
    m = (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + g_esp);
    m[0] = 0x00034326u;                     /* a return address */
    m[1] = (uint32_t)(uint8_t)c;
    g_ecx = page;
    sub_00033EF0();                         /* ret 4: pops both */
    g_ecx = ecx;
}

static void name_entry(uint32_t page)
{
    int c;
    if (guest_rd32(page) != RDR_PROFILE_VT || !guest_rd8(page + 0x44E))
        return;
    rdr_input_text_mode();
    while ((c = rdr_input_text_take()) != 0) {
        if (c == 1) {                       /* Enter: A on End */
            uint32_t n = guest_rd32(page + 0x468), cols = guest_rd32(RDR_GRID_COLS), i;
            uint8_t end = guest_rd8(RDR_GRID_END);
            for (i = 0; i < n && i < 64; i++)
                if (guest_rd8(RDR_GRID + i) == end)
                    break;
            if (i < n && cols) {
                guest_wr32(page + 0x46C, i / cols);
                guest_wr32(page + 0x470, i % cols);
                uint32_t ecx = g_ecx;
                g_esp -= 4;                 /* a return address: it ends in ret */
                *(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + g_esp) = 0x00034326u;
                g_ecx = page;
                sub_000336B0();             /* the hint under the grid */
                g_ecx = ecx;
            }
            rdr_input_text_press(1);
            break;
        }
        if (c == 2) {                       /* Esc: B */
            rdr_input_text_press(2);
            break;
        }
        grid_char(page, c);
    }
}

/* Tests (RDR_HUD_DUMP=1): every page that updates and its widgets --
 * name (+0x30), class, place, label, flags -- every 300th update of it. */
static void page_dump(uint32_t page)
{
    static int on = -1;
    static uint32_t seen[32], count[32];
    uint32_t n, k, slot;
    char nm[24];
    unsigned j;
    if (on < 0) on = getenv("RDR_HUD_DUMP") != NULL;
    if (!on)
        return;
    for (slot = 0; slot < 32 && seen[slot] && seen[slot] != page; slot++)
        ;
    if (slot == 32)
        return;
    seen[slot] = page;
    if (count[slot]++ % 100)
        return;
    for (j = 0; j < sizeof nm - 1 && (nm[j] = (char)guest_rd8(page + 0x1C + j)); j++)
        ;
    nm[j] = 0;
    if (getenv("RDR_PAGE_HEX") && !strcmp(getenv("RDR_PAGE_HEX"), nm)) {
        uint32_t o, bk = guest_rd32(page + 0x450);
        if (in_heap(bk))
            for (o = 0; o < 0x400; o += 16) {
                fprintf(stderr, "[BHEX] +%03X:", o);
                for (j = 0; j < 16; j += 4)
                    fprintf(stderr, " %08X", guest_rd32(bk + o + j));
                fputc('\n', stderr);
            }
        for (o = 0x2A0; o < 0x700; o += 16) {
            fprintf(stderr, "[PHEX] +%03X:", o);
            for (j = 0; j < 16; j += 4)
                fprintf(stderr, " %08X", guest_rd32(page + o + j));
            fputc('\n', stderr);
        }
    }
    n = guest_rd32(page + 0x298);
    fprintf(stderr, "[HUD] page %08X '%s' shown %d cur %d widgets %u\n", page, nm,
            guest_rd8(page + 0x3D), page == guest_rd32(RDR_CUR_PAGE), n);
    for (k = 0; k < n && k < 64; k++) {
        uint32_t w = guest_rd32(page + 0x40 + k * 4);
        char wn[32], label[24];
        if (!in_heap(w))
            continue;
        for (j = 0; j < sizeof wn - 1 && (wn[j] = (char)guest_rd8(w + 0x28 + j)); j++)
            ;
        wn[j] = 0;
        label[0] = 0;
        if (guest_rd32(w) == 0x003C6B30u) {
            for (j = 0; j < sizeof label - 1 && (label[j] = (char)guest_rd8(w + 0x121 + j)); j++)
                ;
            label[j] = 0;
        } else if (guest_rd32(w) == RDR_BUTTON_VT) {
            for (j = 0; j < sizeof label - 1 && (label[j] = (char)guest_rd8(w + 0x114 + j)); j++)
                ;
            label[j] = 0;
        }
        if (getenv("RDR_HUD_FIND")) {
            float want = (float)atof(getenv("RDR_HUD_FIND"));
            uint32_t o;
            for (o = 0; o < 0x2000; o += 4)
                if (guest_rdf(w + o) == want) {
                    fprintf(stderr, "[FIND] %2u %s +%03X\n", k, wn, o);
                }
        }
        if (getenv("RDR_HUD_HEX")) {
            char key[8];
            snprintf(key, sizeof key, ",%u,", k);
            if (strstr(getenv("RDR_HUD_HEX"), key)) {
                uint32_t o;
                for (o = 0; o < 0x200; o += 16) {
                    fprintf(stderr, "[HEX] %2u +%03X:", k, o);
                    for (j = 0; j < 16; j++)
                        fprintf(stderr, " %02X", guest_rd8(w + o + j));
                    fprintf(stderr, "  ");
                    for (j = 0; j < 16; j++) {
                        int ch = guest_rd8(w + o + j);
                        fputc(ch >= 32 && ch < 127 ? ch : '.', stderr);
                    }
                    fputc('\n', stderr);
                }
            }
        }
        fprintf(stderr, "[HUD]   %2u %08X vt %08X '%s' at %.0f,%.0f +%.0f,%.0f e4 %02X%02X%02X%02X d8 %02X%02X%02X%02X \"%s\"\n",
                k, w, guest_rd32(w), wn, guest_rdf(w + 0x4C), guest_rdf(w + 0x50),
                guest_rdf(w + 0x58), guest_rdf(w + 0x5C), guest_rd8(w + 0xE4), guest_rd8(w + 0xE5),
                guest_rd8(w + 0xE6), guest_rd8(w + 0xE7), guest_rd8(w + 0xD8), guest_rd8(w + 0xD9),
                guest_rd8(w + 0xDA), guest_rd8(w + 0xDB), label);
    }
    fflush(stderr);
}

/* ── Clickable button prompts ─────────────────────────────────────
 *
 * The prompts along the bottom of the front end ("A Enter", "X Options",
 * "B Back"; frontendbuttons.swui) and on the menu pages are two text
 * widgets (vtable 0x003C6B30: name +0x28, label +0x121, shown +0xE6): one
 * showing a button glyph by name (BUTTON_CONFIRM ...), the words just to
 * its right. The page arrows (BOOK_/POSTER_/JOURNAL_ARROW_LEFT/RIGHT) are
 * textures (0x003C6A50, 95x48). Each frame a menu is up, the one under the
 * pointer is reported (rdr_input_hud), and a left click there is that
 * button: B over "Back", X over "Options", left over the left arrow. */
#define RDR_TEXT_VT     0x003C6B30u
#define RDR_TEX_VT      0x003C6A50u

static int glyph_button(const char *s)
{
    static const struct { const char *name; int code; } g[] = {
        { "BUTTON_CONFIRM", RDR_HUD_A }, { "BUTTON_CROSS", RDR_HUD_A },
        { "BUTTON_BACK", RDR_HUD_B }, { "BUTTON_CIRCLE", RDR_HUD_B },
        { "BUTTON_ALT1_OPTIONS", RDR_HUD_X }, { "BUTTON_SQUARE", RDR_HUD_X },
        { "BUTTON_ALT2_MISC", RDR_HUD_Y }, { "BUTTON_TRIANGLE", RDR_HUD_Y },
    };
    unsigned i;
    for (i = 0; i < sizeof g / sizeof g[0]; i++)
        if (!strcmp(s, g[i].name))
            return g[i].code;
    return 0;
}

static void guest_str(uint32_t va, char *out, unsigned n)
{
    unsigned j;
    for (j = 0; j + 1 < n && (out[j] = (char)guest_rd8(va + j)); j++)
        ;
    out[j] = 0;
}

static float wx(uint32_t w) { return guest_rdf(w + 0x4C) + guest_rdf(w + 0x58); }
static float wy(uint32_t w) { return guest_rdf(w + 0x50) + guest_rdf(w + 0x5C); }

/* A rectangle of the page, in the pointer's space (cal: the 640x360 frame). */
static void hud_rect(const PageCal *cal, float x, float y, float w, float h,
                     float *l, float *t, float *r, float *b)
{
    if (cal) {
        *l = cal->px0 + (x - cal->x0) * cal->kx;
        *t = cal->py0 + (y - cal->y0) * cal->ky;
        *r = *l + w * cal->kx;
        *b = *t + h * cal->ky;
    } else {
        *l = x; *t = y; *r = x + w; *b = y + h;
    }
}

static int desk_coop_hit(float px, float py, int dbg);

static void hud_scan(uint32_t page, const PageCal *cal)
{
    uint32_t n = guest_rd32(page + 0x298), i, k;
    float px, py, u, v;
    unsigned gen;
    int any = 0, hit = 0, hit_arrow = 0, desk = 0;     /* desk: 1 book, 2 poster, 3 journal in front */
    static int dbg = -1;

    if (n == 0 || n > 96 || !rdr_menu_mode())
        return;
    if (!rdr_input_pointer(&px, &py, &u, &v, &gen))
        return;
    if (cal) {
        px = 320.0f + u * 360.0f;
        py = v * 360.0f;
    }
    if (dbg < 0) dbg = getenv("RDR_MENU_DEBUG") != NULL;
    for (i = 0; i < n; i++) {
        uint32_t w = guest_rd32(page + 0x40 + i * 4), vt;
        char name[40], label[40];
        float l, t, r, b, x, y, right;
        int code = 0;
        if (!in_heap(w) || !guest_rd8(w + 0xE6))
            continue;
        vt = guest_rd32(w);
        if (vt == RDR_TEX_VT) {
            size_t len;
            guest_str(w + 0x28, name, sizeof name);
            len = strlen(name);
            if (!strcmp(name, "BOOK_ARROW_LEFT")) desk = 1;
            else if (!strcmp(name, "POSTER_ARROW_LEFT")) desk = 2;
            else if (!strcmp(name, "JOURNAL_ARROW_LEFT")) desk = 3;
            if (len > 11 && !strcmp(name + len - 11, "_ARROW_LEFT"))
                code = RDR_HUD_LEFT;
            else if (len > 12 && !strcmp(name + len - 12, "_ARROW_RIGHT"))
                code = RDR_HUD_RIGHT;
            /* The desk's outer arrows lead nowhere on the pad (the poster
             * is the left end, the journal the right): clicked, they go
             * back to the book, as d-pad down does. */
            if (!strcmp(name, "POSTER_ARROW_LEFT") || !strcmp(name, "JOURNAL_ARROW_RIGHT"))
                code = RDR_HUD_DOWN;
            if (!code)
                continue;
            hud_rect(cal, wx(w), wy(w), 95.0f, 48.0f, &l, &t, &r, &b);
        } else if (vt == RDR_TEXT_VT) {
            guest_str(w + 0x121, label, sizeof label);
            if (!(code = glyph_button(label)))
                continue;
            x = wx(w);
            y = wy(w);
            right = x + 30.0f;
            /* the words: a shown text on the same line, just to the right */
            for (k = 0; k < n; k++) {
                uint32_t p = guest_rd32(page + 0x40 + k * 4);
                float pxx, pyy;
                if (p == w || !in_heap(p) || guest_rd32(p) != RDR_TEXT_VT || !guest_rd8(p + 0xE6))
                    continue;
                pxx = wx(p);
                pyy = wy(p);
                if (pyy - y > 4.0f || y - pyy > 4.0f || pxx <= x + 4.0f || pxx > x + 70.0f)
                    continue;
                guest_str(p + 0x121, label, sizeof label);
                if (!label[0] || glyph_button(label))
                    continue;
                right = pxx + (float)strlen(label) * 11.5f;
                break;
            }
            /* the desk's "Online Co-op" (Y): its words are a readout, not a
             * text -- the click takes them in too */
            if (code == RDR_HUD_Y && right < x + 170.0f)
                right = x + 170.0f;
            hud_rect(cal, x - 6.0f, y - 4.0f, right - x + 12.0f, 30.0f, &l, &t, &r, &b);
        } else
            continue;
        any = 1;
        if (px >= l && px <= r && py >= t && py <= b) {
            hit = code;
            hit_arrow = vt == RDR_TEX_VT;
        }
    }
    if (!any)
        return;
    if (desk) {
        rdr_input_desk();
        /* The "Y Online Co-op" prompt top right is on another page (the
         * desk's roominstr, which is not a page of places): its readout,
         * the glyph just left of it. */
        if (!hit && !cal && desk_coop_hit(px, py, dbg))
            hit = RDR_HUD_Y;
    }
    if (dbg) {
        static int last_desk = -1;
        if (desk != last_desk) {
            last_desk = desk;
            fprintf(stderr, "[MENU] desk front %d\n", desk);
            fflush(stderr);
        }
    }
    /* The desk (the front end's room): the book, the Showdown poster and
     * the journal themselves are clickable, not only the arrows -- the one
     * in front is entered (A), another is turned to as the d-pad does it:
     * from the book left to the poster and right to the journal, between
     * those two left and right, and from either down to the book. Where each
     * shows, in the menus' 640x448 space, for each one in front (user's
     * screenshots, 16:9). The arrows come first; the button prompts along
     * the bottom ("A Enter", "X Options") lie over the book as the journal
     * shows it, and there the book wins (clicking the book goes to it). */
    if (!hit_arrow && hit != RDR_HUD_Y && desk && !cal) {   /* (the Y prompt over the journal wins) */
        static const struct { int front; float l, t, r, b; int code; } spots[] = {
            { 1,  92,  18, 272, 125, RDR_HUD_LEFT },     /* book: the poster, up left */
            { 1, 371,  18, 570, 117, RDR_HUD_RIGHT },    /*       the journal, up right */
            { 1, 288, 233, 512, 372, RDR_HUD_A },        /*       the book itself */
            { 2, 435, 228, 640, 401, RDR_HUD_RIGHT },    /* poster: the journal, down right */
            { 2, 198,  76, 365, 372, RDR_HUD_A },        /*         the poster itself */
            { 3,   4,  13, 202, 242, RDR_HUD_LEFT },     /* journal: the poster, left */
            { 3, 216, 322, 448, 426, RDR_HUD_DOWN },     /*          the book, down */
            { 3, 288,  90, 480, 237, RDR_HUD_A },        /*          the journal itself */
        };
        for (i = 0; i < sizeof spots / sizeof spots[0]; i++)
            if (spots[i].front == desk && px >= spots[i].l && px <= spots[i].r
                && py >= spots[i].t && py <= spots[i].b) {
                hit = spots[i].code;
                break;
            }
    }
    rdr_input_hud(hit);
    if (dbg && hit) {
        static unsigned last_gen;
        if (gen != last_gen) {
            last_gen = gen;
            fprintf(stderr, "[MENU] pointer %.0f,%.0f over prompt %d\n", px, py, hit);
            fflush(stderr);
        }
    }
}

/* Pages that are flat over the picture in the front end and hold prompts. */
static int front_hud(uint32_t page)
{
    char name[24];
    guest_str(page + 0x1C, name, sizeof name);
    return !strcmp(name, "frontendbuttons") || !strcmp(name, "roominstr");
}

/* ── Option rows: value choosers and sliders ──────────────────────
 *
 * The option pages (Player/Sound/Picture Options, Showdown Options, the
 * pause menu's) hold COMBO widgets (vtable 0x003C6BA0: label at +0x4C/
 * +0x50, its value box at +0x127C x, +0x1280 y, +0x1284 w, +0x1288 h, the
 * arrows at the box's ends) and SLIDERs (0x003C6EF8: value 0..1 at
 * +0x144, step +0x148 -- written directly it shows but is not kept;
 * drawn as a groove texture, 1 high, a marker and two arrows; the n-th
 * slider on the n-th groove).
 * Under a moved pointer a row takes the focus (SetFocus, as the d-pad
 * would); a click on a chooser's arrow or value steps it (d-pad left or
 * right, rdr_input_hud), anywhere else on the row nothing (A would accept
 * the page); a slider is stepped (d-pad) towards where the pointer
 * presses or drags along it, its arrows step it once. */
#define RDR_COMBO_VT    0x003C6BA0u
#define RDR_SLIDER_VT   0x003C6EF8u

extern int xbox_FramebufferKeyDown(int vk);

static void option_rows(uint32_t page)
{
    uint32_t n = guest_rd32(page + 0x298), i, grooves[8], sliders[8];
    int ng = 0, ns = 0, hover = -1, code = 0, rows = 0;
    float px, py, u, v;
    unsigned gen;
    static unsigned seen_gen;
    static uint32_t drag;                    /* the slider being dragged */
    static int was_down;
    int down = xbox_FramebufferKeyDown(0x01);

    if (n == 0 || n > 96 || !rdr_input_pointer(&px, &py, &u, &v, &gen))
        return;
    for (i = 0; i < n; i++) {
        uint32_t w = guest_rd32(page + 0x40 + i * 4), vt;
        if (!in_heap(w) || !guest_rd8(w + 0xE6))
            continue;
        vt = guest_rd32(w);
        if (vt == RDR_SLIDER_VT && ns < 8)
            sliders[ns++] = i;
        else if (vt == RDR_TEX_VT) {
            char tn[16];
            guest_str(w + 0x28, tn, sizeof tn);
            if (!strncmp(tn, "groove", 6) && ng < 8)
                grooves[ng++] = w;
        } else if (vt == RDR_COMBO_VT) {
            float lx = wx(w), ly = wy(w), bx = guest_rdf(w + 0x127C), bw = guest_rdf(w + 0x1284);
            rows++;
            if (!(bw > 10.0f && bw < 640.0f))
                continue;
            if (px >= lx - 6.0f && px <= bx + bw + 24.0f && py >= ly - 4.0f && py <= ly + 24.0f) {
                hover = (int)i;
                code = px < bx + 36.0f && px >= bx - 24.0f ? RDR_HUD_LEFT : RDR_HUD_RIGHT;
                if (px < bx - 24.0f)
                    code = RDR_HUD_NOTHING;   /* the label: focus only */
            }
        }
    }
    /* Sliders: the k-th on the k-th groove. */
    for (i = 0; i < (uint32_t)ns && i < (uint32_t)ng; i++) {
        uint32_t s = guest_rd32(page + 0x40 + sliders[i] * 4), g = grooves[i];
        float gx = wx(g), gy = wy(g), gw = guest_rdf(g + 0x118), mw = 53.0f, val;
        rows++;
        if (!(gw > 20.0f && gw < 640.0f))
            continue;
        if (px >= 60.0f && px <= gx + gw + 40.0f && py >= gy - 18.0f && py <= gy + 14.0f) {
            hover = (int)sliders[i];
            code = px < gx - 2.0f ? RDR_HUD_LEFT : px > gx + gw + 2.0f ? RDR_HUD_RIGHT : RDR_HUD_NOTHING;
            if (down && !was_down && code == RDR_HUD_NOTHING)
                drag = s;
        }
        if (drag == s && down) {
            /* Stepped there with the d-pad, as the game would be: what it
             * keeps is what its own steps set. */
            float step = guest_rdf(s + 0x148), cur = guest_rdf(s + 0x144);
            val = (px - gx - mw * 0.5f) / (gw - mw);
            if (val < 0.0f) val = 0.0f;
            if (val > 1.0f) val = 1.0f;
            if (!(step > 0.001f && step < 1.0f))
                step = 0.1f;
            if (val > cur + step * 0.5f)
                rdr_input_step(RDR_HUD_RIGHT);
            else if (val < cur - step * 0.5f)
                rdr_input_step(RDR_HUD_LEFT);
        }
    }
    if (!down)
        drag = 0;
    was_down = down;
    if (!rows)
        return;
    rdr_input_pointer_page(hover >= 0);
    if (hover >= 0)
        rdr_input_hud(code);
    if (gen != seen_gen) {
        seen_gen = gen;
        if (hover >= 0 && (uint32_t)hover != guest_rd32(page + 0x31C))
            set_focus(page, (uint32_t)hover);
    }
}

extern void rdr_lineup_pointer(uint32_t page);

/* ── Level warp (F1 menu) ───────────────────────────────────────────
 *
 * A story chapter is started the way the book's New starts one, so only
 * from the front end (warp_step). From a level
 * the game is first quit to the front end through its own menus: Start
 * opens the pause menu, "Quit" is focused and pressed, the warning's "Yes"
 * likewise; once the desk's prompts are up the chapter starts. */
extern uint64_t xbox_HostNowNs(void);

static volatile int s_warp_stage;           /* 0 none, 1 asked, 2 quitting the level */
static int s_warp_left;                     /* the front end reached */
static uint32_t prof_va;                    /* the profile, kept over New */
static uint8_t prof[0xB18];
static uint32_t s_warp_cell;                /* the contents cell lent, and */
static char s_warp_cell_old[32];            /* what it held */
static char s_warp_name[32];
static int s_warp_var;
static uint64_t s_warp_ns, s_warp_press_ns;

void rdr_warp_request(const char *name, int var)
{
    snprintf(s_warp_name, sizeof s_warp_name, "%s", name);
    s_warp_var = var;
    s_warp_ns = xbox_HostNowNs();
    s_warp_press_ns = 0;
    s_warp_left = 0;
    /* The profile as it is now, to put back over New's reset. */
    prof_va = 0;
    {
        uint32_t idx = guest_rd32(0x00657798u), j;
        if (idx < 8) {
            prof_va = 0x00653508u + idx * 0xB18u;
            for (j = 0; j < sizeof prof; j++)
                prof[j] = guest_rd8(prof_va + j);
        } else {
            fprintf(stderr, "[LEVEL] warp: no profile (index %u) to keep\n", idx);
        }
    }
    s_warp_stage = 1;
    fprintf(stderr, "[LEVEL] warp to %s %d asked\n", name, var);
    fflush(stderr);
}

int rdr_warp_pending(void)
{
    return s_warp_stage;
}

/* The page's selectable button whose label starts with one of these. */
static int find_button(uint32_t page, const char *const *labels, int nl)
{
    uint32_t n = guest_rd32(page + 0x298), k;
    for (k = 0; k < n && k < 64; k++) {
        uint32_t w = guest_rd32(page + 0x40 + k * 4);
        char label[24];
        int j;
        if (!in_heap(w) || guest_rd32(w) != RDR_BUTTON_VT || !guest_rd8(w + 0xD8))
            continue;
        guest_str(w + 0x114, label, sizeof label);
        for (j = 0; j < nl; j++)
            if (!strncmp(label, labels[j], strlen(labels[j])))
                return (int)k;
    }
    return -1;
}

extern void sub_00058A70(void);
static uint32_t gcall(void (*fn)(void), uint32_t ecx, int n, uint32_t a0, uint32_t a1);
static void warp_gcall(void (*fn)(void), uint32_t ecx)
{
    gcall(fn, ecx, 0, 0, 0);
}

/* Each frame from the game's thread (rdr_warp_tick, the pause and front
 * end page updates). */
static void warp_step(uint32_t page)
{
    uint64_t now = xbox_HostNowNs();
    uint32_t cur;
    char name[24];
    int front;
    {
        static int dbg = -1, left;
        if (dbg < 0) dbg = getenv("RDR_WARP_DEBUG") != NULL;
        if (dbg && s_warp_stage) left = 400;
        if (dbg && left > 0) {
            char a[24], b[24];
            static char la[24], lb[24];
            static uint32_t l30;
            left--;
            guest_str(0x00652E70u, a, sizeof a);
            guest_str(0x00652678u, b, sizeof b);
            if (strcmp(a, la) || strcmp(b, lb) || guest_rd32(0x00652E30u) != l30) {
                fprintf(stderr, "[WARP] 652E70 '%s' 652678 '%s' 652E30 %u 652E50 %u\n", a, b,
                        guest_rd32(0x00652E30u), guest_rd8(0x00652E50u));
                fflush(stderr);
                strcpy(la, a); strcpy(lb, b); l30 = guest_rd32(0x00652E30u);
            }
        }
    }
    if (!s_warp_stage)
        return;
    if (s_warp_stage != 3 && now - s_warp_ns > 120000000000ull) {    /* two minutes: give up */
        s_warp_stage = 0;
        fprintf(stderr, "[LEVEL] warp gave up\n");
        fflush(stderr);
        return;
    }
    cur = guest_rd32(RDR_CUR_PAGE);
    guest_str(page + 0x1C, name, sizeof name);
    /* The front end: the level loaded is lvl_front (after quitting a level
     * the player list, so rdr_in_level, can still name the old player). */
    {
        char lv[16];
        guest_str(0x00652E70u, lv, sizeof lv);
        front = !strcmp(lv, "lvl_front");
        {
            static uint64_t last;
            if (getenv("RDR_WARP_DEBUG") && now - last > 2000000000ull) {
                last = now;
                fprintf(stderr, "[WARP] stage %d level '%s' page '%s' shown %d cur %d\n", s_warp_stage, lv,
                        name, guest_rd8(page + 0x3D), page == cur);
                fflush(stderr);
            }
        }
    }
    if (getenv("RDR_WARP_DEBUG")) {
        static char seen[16][24];
        static int ns;
        int j;
        for (j = 0; j < ns && strcmp(seen[j], name); j++)
            ;
        if (j == ns && ns < 16) {
            snprintf(seen[ns++], 24, "%s", name);
            fprintf(stderr, "[WARP] page '%s' (%08X) shown %d cur %08X\n", name, page,
                    guest_rd8(page + 0x3D), cur);
            fflush(stderr);
        }
    }
    if (front)
        s_warp_left = 1;
    if (s_warp_stage == 3)
        return;                         /* begun: the chapter loads (its own menus) */
    if (front) {
        /* The story is started the way the book starts it: the book
         * opened (A on the desk's book), New, then Begin on the chapter
         * page with the chapter swapped in (below). New takes the story
         * back to its start, so the player's profile is copied here and
         * put back once the level runs. */
        static int book_open;
        if (!strcmp(name, "storyselect"))
            book_open = guest_rd8(page + 0x3D);
        if (!strcmp(name, "storyselect") && guest_rd8(page + 0x3D)
            && now - s_warp_press_ns > 1200000000ull) {
            static const char *const nw[] = { "story_newgame" };
            int b = find_button(page, nw, 1);
            if (b < 0) {
                static int told;
                if (!told++) {
                    uint32_t n = guest_rd32(page + 0x298), k;
                    for (k = 0; k < n && k < 32; k++) {
                        uint32_t w = guest_rd32(page + 0x40 + k * 4);
                        char lb[24];
                        if (!in_heap(w))
                            continue;
                        guest_str(w + 0x114, lb, sizeof lb);
                        fprintf(stderr, "[LEVEL] book item %u vt %08X d8 %d '%s'\n", k, guest_rd32(w),
                                guest_rd8(w + 0xD8), lb);
                    }
                    fflush(stderr);
                }
            }
            if (b >= 0) {
                if ((uint32_t)b != guest_rd32(page + 0x31C))
                    set_focus(page, (uint32_t)b);
                rdr_input_text_press(1);
                s_warp_press_ns = now;
            }
        }
        if ((!strcmp(name, "difficultyselect") || !strcmp(name, "warningfrontend"))
            && guest_rd8(page + 0x3D) && now - s_warp_press_ns > 2000000000ull) {
            /* New on a finished story asks the difficulty (kept as it is);
             * a warning that progress goes: yes, it is put back. */
            if (!strcmp(name, "warningfrontend")) {
                static const char *const yes[] = { "btnYes", "Yes", "YES" };
                int b = find_button(page, yes, 3);
                if (b >= 0 && (uint32_t)b != guest_rd32(page + 0x31C))
                    set_focus(page, (uint32_t)b);
            }
            rdr_input_text_press(1);
            s_warp_press_ns = now;
            return;
        }
        if (!strcmp(name, "bookchapterview") && guest_rd8(page + 0x3D)
                   && now - s_warp_press_ns > 1500000000ull) {
            /* The chapter page New opens (Begin: sub_00035910). Its level
             * comes from the book's contents table, [page+0x45C] indexed
             * by (the book's page [[page+0x44C]+0x100] - [page+0x464]) / 2,
             * strings like "lvl_perfnew_1" in the LOOKUP's 32-byte cells:
             * the shown chapter's cell holds the warp's level for the
             * moment Begin reads it (put back once the level runs). */
            uint32_t book = guest_rd32(page + 0x44C), tab = guest_rd32(page + 0x45C), cell;
            int32_t d = in_heap(book) ? (int32_t)(guest_rd32(book + 0x100) - guest_rd32(page + 0x464)) : -1;
            {
                static int told;
                if (!told++) {
                    fprintf(stderr, "[LEVEL] chapter page %08X vt %08X book %08X page %d first %d table %08X\n",
                            page, guest_rd32(page), book, in_heap(book) ? (int)guest_rd32(book + 0x100) : -1,
                            (int)guest_rd32(page + 0x464), tab);
                    fflush(stderr);
                }
            }
            if (book && tab && d >= 0 && d < 200 && in_heap(cell = guest_rd32(tab + (uint32_t)(d / 2) * 4))) {
                char want[40];
                int j;
                snprintf(want, sizeof want, "%s_%d", s_warp_name, s_warp_var);
                if (!s_warp_cell) {
                    s_warp_cell = cell;
                    guest_str(cell, s_warp_cell_old, sizeof s_warp_cell_old);
                }
                for (j = 0; j < 31 && want[j]; j++)
                    *(volatile char *)((uintptr_t)xbox_GetMemoryOffset() + cell + (uint32_t)j) = want[j];
                *(volatile char *)((uintptr_t)xbox_GetMemoryOffset() + cell + (uint32_t)j) = 0;
                fprintf(stderr, "[LEVEL] chapter page: '%s' for '%s', Begin\n", want, s_warp_cell_old);
                fflush(stderr);
                rdr_input_text_press(1);
                s_warp_press_ns = now;
                s_warp_stage = 3;           /* begun: hands off until a level runs */
            }
        } else if (!strcmp(name, "frontendbuttons") && guest_rd8(page + 0x3D) && !book_open
                   && now - s_warp_press_ns > 2500000000ull) {
            /* The desk: on the book (its arrows shown), A opens it; on the
             * poster, right; on the journal, left. */
            uint32_t n = guest_rd32(page + 0x298), k;
            int press = 0;
            for (k = 0; k < n && k < 64; k++) {
                uint32_t w = guest_rd32(page + 0x40 + k * 4);
                char wn[24];
                if (!in_heap(w) || !guest_rd8(w + 0xE6))
                    continue;
                guest_str(w + 0x28, wn, sizeof wn);
                if (!strcmp(wn, "BOOK_ARROW_LEFT")) press = 1;
                else if (!strcmp(wn, "POSTER_ARROW_LEFT")) press = 5;
                else if (!strcmp(wn, "JOURNAL_ARROW_LEFT")) press = 4;
            }
            if (press) {
                rdr_input_text_press(press);
                s_warp_press_ns = now;
            }
        }
        return;
    }
    if (!s_warp_left && s_warp_stage == 1 && !strcmp(name, "pause") && page == cur && guest_rd8(page + 0x3D)
        && !getenv("RDR_WARP_VIA_MENUS")) {
        /* Quit and its warning's Yes, without either: what the Yes does
         * (sub_00032C40 on the pause page: quitting flag +0x46D, +0x3B0
         * cleared, the level's tallies reset in a story), with the pause
         * page's two-second fade (+0x470 counting to 2.0, sub_0002FB80)
         * already run out -- its next update sets the quit request, and
         * the front-end screen's enter goes straight to the chapter
         * (rdr_warp_direct). The pause menu shows for a frame or two. */
        volatile uint8_t *mm = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
        float two = 2.0f;
        mm[page + 0x46D] = 1;
        memcpy((void *)(uintptr_t)(mm + page + 0x470), &two, 4);
        *(volatile uint32_t *)(mm + page + 0x3B0) = 0;
        if (guest_rd32(0x00652E30u) == 0)
            warp_gcall(sub_00058A70, 0x006585F8u);
        s_warp_press_ns = now;
        s_warp_stage = 2;
        fprintf(stderr, "[LEVEL] warp: quit from the pause menu at once\n");
        fflush(stderr);
        return;
    }
    if (s_warp_stage == 2 && page == cur && now - s_warp_press_ns > 5000000000ull
        && (!strcmp(name, "warning") || !strcmp(name, "pause")))
        s_warp_stage = 1;                       /* the Yes was not taken: again */
    if (s_warp_left || s_warp_stage == 2 || page != cur || now - s_warp_press_ns < 2000000000ull)
        return;
    if (!strcmp(name, "pause")) {
        /* Quit focused first, pressed a moment later: focused and pressed
         * in one frame, the press went to the item the menu still had
         * (Return to Game) -- the pause menu closed, Start opened it
         * again, and so on (user: stuck on the pause menu). */
        static const char *const quit[] = { "Quit" };
        int b = find_button(page, quit, 1);
        if (b >= 0) {
            if ((uint32_t)b != guest_rd32(page + 0x31C)) {
                set_focus(page, (uint32_t)b);
                s_warp_press_ns = now - 1500000000ull;   /* A in half a second */
                return;
            }
            rdr_input_text_press(1);
            s_warp_press_ns = now;
            fprintf(stderr, "[LEVEL] warp: Quit (item %d)\n", b);
            fflush(stderr);
        }
    } else if (!strcmp(name, "warning")) {
        /* As by hand: left onto YES, then A, a moment after it shows. */
        static const char *const yes[] = { "btnYes", "Yes", "YES" };
        static uint64_t seen;
        int b = find_button(page, yes, 3);
        if (now - seen > 3000000000ull)
            seen = now;                         /* first seen now */
        if (b < 0 || now - seen < 2000000000ull)
            return;
        if ((uint32_t)b != guest_rd32(page + 0x31C)) {
            rdr_input_text_press(4);
            s_warp_press_ns = now;
        } else {
            rdr_input_text_press(1);
            s_warp_press_ns = now;
            s_warp_stage = getenv("RDR_WARP_STOP_AT_YES") ? 0 : 2;  /* leaving: nothing more here */
            fprintf(stderr, "[LEVEL] warp: Yes (item %d)\n", b);
            fflush(stderr);
        }
        seen = now - 2000000000ull;             /* still showing */
    }
}

/* ── Straight to the chapter (no front end) ─────────────────────────
 *
 * The warp quits the level through the pause menu. Quitting enters the
 * front-end screen (sub_0006BAA0, rdr_hooks.c: unload the level, load
 * lvl_front, the desk and book); after Begin its transition to the game
 * screen fires (condition sub_0006BA40: the game screen told the level,
 * sub_00064980 on [0x0065A6D4] with 0x00652E70) and the game screen's
 * enter (sub_0006B190) loads that level (sub_00060B90).
 *
 * A screen's enter may return a transition, which the screen manager
 * (sub_00064760) follows at once into its target's enter. So from a story
 * level the front-end screen's enter does only what Begin and that
 * condition do -- the level named (sub_00056550), the game screen told --
 * and returns its own transition to the game screen: the game screen
 * unloads this level and loads the chapter as after Begin, with neither
 * the front end nor its loading screen.
 * RDR_WARP_VIA_BOOK=1: the old way (front end, book, Begin). */
extern void sub_00056550(void), sub_00064980(void), sub_00064280(void), sub_00064350(void),
    sub_0003FB80(void), sub_0003FBF0(void), sub_00056DA0(void), sub_0005DF40(void);

/* A guest function called from here: args on the guest stack under a
 * return address; the stack put back afterwards (cdecl or callee-pops). */
static uint32_t gcall(void (*fn)(void), uint32_t ecx, int n, uint32_t a0, uint32_t a1)
{
    uint32_t esp0 = g_esp, ecx0 = g_ecx, r;
    volatile uint32_t *m;
    g_esp -= 4u * (uint32_t)(n + 1);
    m = (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + g_esp);
    m[0] = 0x0006BA7Au;                     /* a return address (sub_0006BA40's) */
    if (n > 0) m[1] = a0;
    if (n > 1) m[2] = a1;
    g_ecx = ecx;
    fn();
    r = g_eax;
    g_esp = esp0;
    g_ecx = ecx0;
    return r;
}

/* The front-end screen's enter: 0 to run it, else the transition to follow. */
uint32_t rdr_warp_direct(uint32_t screen)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t node, tr = 0, mgr = guest_rd32(0x0065A6D4u);
    int j;
    if (s_warp_stage != 2 || getenv("RDR_WARP_VIA_BOOK") || guest_rd32(0x00652E30u) != 0
        || (!in_heap(mgr) && !(mgr >= 0x00010000u && mgr < 0x00800000u)))
        return 0;                           /* not a warp, or not from a story level */
    /* The transition to the game screen: its condition is sub_0006BA40. */
    node = guest_rd32(screen + 8);
    for (j = 0; node && j < 16; j++, node = guest_rd32(node + 4)) {
        uint32_t t = guest_rd32(node);
        if (guest_rd32(guest_rd32(t) + 0xC) == 0x0006BA40u) {
            tr = t;
            break;
        }
    }
    if (!tr) {
        fprintf(stderr, "[LEVEL] warp: no transition to the game screen; through the front end\n");
        fflush(stderr);
        return 0;
    }
    /* The quit request the pause menu's Quit set (bit 0 of 0x0065A828: the
     * in-level screen's way to the front end, condition sub_0005DF20),
     * cleared as the front end's enter clears it (sub_0005DF40(0, 0)):
     * left set, the chapter went to the main menu once it started. */
    gcall(sub_0005DF40, 0, 2, 0, 0);
    /* Begin (sub_00035910), its page's parts left out: the chapter is the
     * warp's instead of the contents table's (sub_00035880). */
    gcall(sub_00064280, gcall(sub_00064350, 0, 0, 0, 0), 0, 0, 0);
    gcall(sub_0003FB80, 0x00652138u, 0, 0, 0);
    *(volatile uint32_t *)(m + 0x00652148u) = guest_rd32(0x0065338Cu);
    *(volatile uint32_t *)(m + 0x0065268Cu) = 1;
    *(volatile uint32_t *)(m + 0x00652690u) = 1;
    m[0x0065214Cu] = 1;
    for (j = 0; j < 31 && s_warp_name[j]; j++)
        m[0x00652678u + (uint32_t)j] = (uint8_t)s_warp_name[j];
    m[0x00652678u + (uint32_t)j] = 0;
    {
        uint32_t t = gcall(sub_00056DA0, 0, 2, 0x00652678u, (uint32_t)s_warp_var);
        if (t >= 0x00010000u && (t < 0x00800000u || in_heap(t))) {
            for (j = 0; j < 31 && m[t + (uint32_t)j]; j++)
                m[0x00652139u + (uint32_t)j] = m[t + (uint32_t)j];
            m[0x00652139u + (uint32_t)j] = 0;
        }
    }
    m[0x00652138u] = 1;
    *(volatile uint32_t *)(m + 0x00652E30u) = 0;
    m[0x00652E50u] = 1;
    gcall(sub_00056550, 0, 2, 0x00652678u, (uint32_t)s_warp_var);
    gcall(sub_0003FBF0, 0x00652138u, 0, 0, 0);
    /* What the transition's condition (sub_0006BA40) does for a story. */
    gcall(sub_00064980, mgr, 1, 0x00652E70u, 0);
    if (getenv("RDR_WARP_DEBUG")) {
        uint32_t scr[3] = { guest_rd32(tr + 8), 0x00579B60u, 0x004239E0u };
        int k, q;
        for (q = 0; q < 3; q++) {
            uint32_t nd = guest_rd32(scr[q] + 8);
            for (k = 0; nd && k < 16; k++, nd = guest_rd32(nd + 4)) {
                uint32_t t = guest_rd32(nd), tg = guest_rd32(t + 8);
                fprintf(stderr, "[LEVEL]   screen %08X (vt %08X) transition %08X cond %08X to %08X (vt %08X)\n",
                        scr[q], guest_rd32(scr[q]), t, guest_rd32(guest_rd32(t) + 0xC), tg, guest_rd32(tg));
            }
        }
    }
    s_warp_left = 1;
    s_warp_stage = 3;                       /* the level that runs now is the warp's */
    prof_va = 0;                            /* no New here: the profile stays as it is */
    {
        char lv[32];
        guest_str(0x00652E70u, lv, sizeof lv);
        fprintf(stderr, "[LEVEL] warp: straight to '%s' variant %u (no front end; game screen %08X)\n", lv,
                guest_rd32(0x00652E60u), guest_rd32(tr + 8));
        fflush(stderr);
    }
    return tr;
}

/* From the game's thread each frame (rdr_overrides' player update): in a
 * level with no menu up, Start to open the pause menu. */
static void warp_profile_back(void)
{
    /* The profile as it was before New (New takes the story back to its
     * start), so the warp costs no progress -- put back as soon as the
     * chapter's level is named, before its shop (put back later, the
     * shop's purchases went with it). */
    uint32_t j, diff = 0, first = 0;
    for (j = 0; prof_va && j < sizeof prof; j++)
        if (guest_rd8(prof_va + j) != prof[j]) {
            if (!diff++)
                first = j;
            if (!getenv("RDR_WARP_NO_RESTORE"))
                *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + prof_va + j) = prof[j];
        }
    if (prof_va) {
        fprintf(stderr, "[LEVEL] warp: profile %u bytes put back (first +0x%X)\n", diff, first);
        fflush(stderr);
    }
    prof_va = 0;
}

extern int rdr_pause_on_focus(void);

static void warp_tick_level(void)
{
    uint64_t now = xbox_HostNowNs();
    char lv[32];
    guest_str(0x00652E70u, lv, sizeof lv);
    /* Another program brought to the front: the game's own pause menu, as
     * a console's game pauses (settings: pause_on_focus). Only in play --
     * not in a menu, not in a cutscene (Start there can skip it), not
     * while a chapter warp drives the menus. */
    extern int rdr_lockstep_mode(void);
    /* (online, lockstep: one machine's pause alone would part the games) */
    if (rdr_input_focus_lost_take() && rdr_pause_on_focus() && rdr_lockstep_mode() != 4 && !s_warp_stage && rdr_in_level()
        && strcmp(lv, "lvl_front") && !rdr_menu_mode() && !guest_rd8(0x00652B50u)) {
        rdr_input_text_press(3);
        fprintf(stderr, "[RDR] window lost the focus: paused\n");
        fflush(stderr);
    }
    if (getenv("RDR_WARP_DEBUG")) {
        static uint32_t last[4];
        uint32_t g = guest_rd32(0x0065A6D4u), v[4];
        if (g) {
            v[0] = guest_rd32(g + 0x90); v[1] = guest_rd32(g + 0x94); v[2] = guest_rd32(g + 0x98);
            v[3] = guest_rd32(g + 0x9C);
            if (memcmp(v, last, sizeof v)) {
                fprintf(stderr, "[WARP] game %08X +90 %u +94 %08X +98 %08X +9C %08X +A0 %04X level '%s' stage %d\n",
                        g, v[0], v[1], v[2], v[3], guest_rd32(g + 0xA0) & 0xFFFF, lv, s_warp_stage);
                fflush(stderr);
                memcpy(last, v, sizeof v);
            }
        }
    }
    if (s_warp_stage == 3 && prof_va && strcmp(lv, "lvl_front"))
        warp_profile_back();
    /* Done once a level runs after Begin -- whichever: a chapter can open
     * on another level than its name (lvl_traincar_1 runs lvl_train after
     * its own menus), and waiting for the name left the warp pressing
     * buttons in those menus (user: back to the main menu). */
    if (s_warp_stage && s_warp_left && rdr_in_level() && strcmp(lv, "lvl_front")
        && (s_warp_stage == 3 || !strcmp(lv, s_warp_name))) {
        warp_profile_back();
        fprintf(stderr, "[LEVEL] warp done: %s %d\n", s_warp_name, s_warp_var);
        fflush(stderr);
        s_warp_stage = 0;
        if (s_warp_cell) {
            int k;
            for (k = 0; k < 31 && s_warp_cell_old[k]; k++)
                *(volatile char *)((uintptr_t)xbox_GetMemoryOffset() + s_warp_cell + (uint32_t)k) = s_warp_cell_old[k];
            *(volatile char *)((uintptr_t)xbox_GetMemoryOffset() + s_warp_cell + (uint32_t)k) = 0;
            s_warp_cell = 0;
        }
        return;
    }
    /* From the main menu, a press that reached the game before the warp
     * had swapped the book's chapter started another level (user: Go to
     * Chapter from the main menu went to the first chapter, lvl_perfnew,
     * at once after the menu closed). Then the warp goes on from that
     * level, as from any level: Start, the pause menu, straight to the
     * chapter. */
    if (s_warp_stage == 1 && s_warp_left && rdr_in_level() && strcmp(lv, "lvl_front")
        && strcmp(lv, s_warp_name)) {
        s_warp_left = 0;
        s_warp_ns = now;
        s_warp_press_ns = now;              /* Start in 1.5 s, once it runs */
        fprintf(stderr, "[LEVEL] warp: '%s' started instead of the chapter; going on from it\n", lv);
        fflush(stderr);
    }
    if (s_warp_stage && s_warp_stage != 3 && now - s_warp_ns > 120000000000ull) {
        s_warp_stage = 0;                   /* two minutes: give up */
        fprintf(stderr, "[LEVEL] warp gave up\n");
        fflush(stderr);
        return;
    }
    if (s_warp_stage == 1 && getenv("RDR_WARP_DEBUG")) {
        static uint64_t last;
        if (now - last > 3000000000ull) {
            last = now;
            fprintf(stderr, "[WARP] waiting: left %d in level %d menu %d level '%s'\n", s_warp_left,
                    rdr_in_level(), rdr_menu_mode(), lv);
            fflush(stderr);
        }
    }
    if (s_warp_stage == 1 && !s_warp_left && rdr_in_level() && strcmp(lv, "lvl_front") && !rdr_menu_mode()
        && now - s_warp_press_ns > 1500000000ull) {
        rdr_input_text_press(3);
        s_warp_press_ns = now;
        fprintf(stderr, "[LEVEL] warp: Start\n");
        fflush(stderr);
    }
}

/* ── Online: straight into the story (the host's "Start in") ────────
 *
 * Online, every game starts again from its boot and stays in step only
 * through the controllers. The host's lobby can send them on by
 * themselves (RDR_ONLINE_START, the host's game only: 1 the book's
 * Continue, 2 its New): the host's own controller is pressed for it --
 * Start through the logos and the title, Start on the desk (the book),
 * on the book the d-pad to Continue or New and A, then A on New's
 * difficulty, Yes on its warning, Begin on the chapter page -- and the
 * presses go to every game with the host's pad, so all of them go the
 * same way, frame for frame. Nothing here writes to the game. It stops
 * once a level other than the front end is named (the chapter loading),
 * or after five minutes. */
extern int rdr_lockstep_mode(void);
static int s_os_mode = -1, s_os_done;

/* The game's own time (its frame count, 60 a second): the waits below
 * are the game's -- a slow machine's desk takes as many frames to come
 * up as a fast one's (the harness, two games at 3 frames a second, pressed
 * all its tries before the desk was ready). */
static uint64_t os_now(void)
{
    return (uint64_t)guest_rd32(RDR_FRAME_COUNT) * 16666667ull;
}
static uint64_t s_os_t0, s_os_press, s_os_front, s_os_page, s_os_desk;
static int s_os_book, s_os_desk_presses;

static int online_start_mode(void)
{
    if (s_os_mode < 0) {
        const char *e = getenv("RDR_ONLINE_START"), *h = getenv("RDR_LS_HOST");
        s_os_mode = e && ((h && *h && *h != '0') || getenv("RDR_ONLINE_START_SOLO")) ? atoi(e) : 0;
        if (s_os_mode < 0 || s_os_mode > 2) s_os_mode = 0;
        if (s_os_mode) {
            fprintf(stderr, "[ONLINE] start: %s, by itself\n", s_os_mode == 1 ? "continue the story" : "a new game");
            fflush(stderr);
        }
    }
    return s_os_done ? 0 : s_os_mode;
}

/* The press, put into the host's own pad report as it goes out
 * (rdr_lockstep.c, once per lockstep frame): down 4 frames, then up 4.
 * (Through the keyboard's filter it went to the keyboard's port -- with
 * the keyboard on another player, nobody's.) */
static volatile int s_os_btn, s_os_frames;

void rdr_online_start_merge(uint8_t *g)
{
    int b = s_os_btn;
    if (!b)
        return;
    if (s_os_frames == 0) {
        fprintf(stderr, "[ONLINE] start: button %d into the host's pad (A %u, buttons %02X%02X)\n", b, g[2], g[1], g[0]);
        fflush(stderr);
    }
    if (s_os_frames < 4) {
        uint16_t w;
        memcpy(&w, g, 2);
        switch (b) {
        case 1: g[2 + 0] = 255; break;                       /* A */
        case 2: g[2 + 1] = 255; break;                       /* B */
        case 3: w |= 0x0010; break;                          /* Start */
        case 4: w |= 0x0004; break;                          /* left */
        case 5: w |= 0x0008; break;                          /* right */
        case 6: w |= 0x0001; break;                          /* up */
        case 7: w |= 0x0002; break;                          /* down */
        }
        memcpy(g, &w, 2);
    }
    if (++s_os_frames >= 8) {
        s_os_btn = 0;
        s_os_frames = 0;
    }
}

static void os_press(int b, uint64_t now, const char *what)
{
    char lv[32];
    if (rdr_lockstep_mode() == 4) {
        if (s_os_btn)
            return;                         /* the last one still going out */
        s_os_frames = 0;
        s_os_btn = b;
    } else
        rdr_input_text_press(b);
    s_os_press = now;
    guest_str(0x00652E70u, lv, sizeof lv);
    fprintf(stderr, "[ONLINE] start: %s (level '%s')\n", what, lv);
    fflush(stderr);
}

/* From the overlay each frame: the boot (no page of the front end yet)
 * and the desk. */
void rdr_online_start_tick(void)
{
    uint64_t now = os_now();
    char lv[32];
    if (!online_start_mode() || (rdr_lockstep_mode() != 4 && !getenv("RDR_ONLINE_START_SOLO")))
        return;
    {
        /* (by the clock on the wall: a slow machine's loads take long) */
        uint64_t wall = xbox_HostNowNs();
        if (!s_os_t0)
            s_os_t0 = wall;
        if (wall - s_os_t0 > 900000000000ull) {
            s_os_done = 1;
            fprintf(stderr, "[ONLINE] start: gave up after fifteen minutes (the menus are yours)\n");
            fflush(stderr);
            return;
        }
    }
    guest_str(0x00652E70u, lv, sizeof lv);
    if (!strcmp(lv, "lvl_front")) {
        if (!s_os_front)
            s_os_front = now;
    } else if (s_os_front && lv[0]) {
        s_os_done = 1;
        fprintf(stderr, "[ONLINE] start: %s is loading\n", lv);
        fflush(stderr);
        return;
    }
    if (now - s_os_press < 1500000000ull || now - s_os_page < 2000000000ull)
        return;                             /* a page below is being worked */
    if (!s_os_front)
        os_press(3, now, "Start (the logos, the title)");
    else if (now - s_os_front > 2500000000ull && now - s_os_desk < 500000000ull && !s_os_book
             && now - s_os_press > 3000000000ull && s_os_desk_presses < 60) {
        /* the desk itself, the book not open yet: A ("Enter") opens it
         * (a few tries, slowly: the book takes a moment to come up; Start
         * did it alone, but not online) */
        s_os_desk_presses++;
        os_press(1, now, "A (the desk: the book)");
    }
}

/* From each front-end page's update (the game's thread). */
static void online_start_page(uint32_t page)
{
    uint64_t now = os_now();
    char name[24];
    int b;
    if (!online_start_mode() || (rdr_lockstep_mode() != 4 && !getenv("RDR_ONLINE_START_SOLO")))
        return;
    guest_str(page + 0x1C, name, sizeof name);
    if (getenv("RDR_ONLINE_START_SOLO")) {
        static char last[24];
        if (strcmp(last, name)) {
            snprintf(last, sizeof last, "%s", name);
            fprintf(stderr, "[ONLINE] page '%s' shown %d cur %d\n", name, guest_rd8(page + 0x3D),
                    page == guest_rd32(RDR_CUR_PAGE));
        }
    }
    if (!guest_rd8(page + 0x3D))
        return;
    if (!strcmp(name, "frontendbuttons") && page == guest_rd32(RDR_CUR_PAGE)) {
        /* the desk is up and listening: its "Enter" prompt (PROMPT1, a
         * text widget: label +0x121, shown +0xE6) shows -- not while the
         * front end still loads (the page is there then too; the presses
         * went nowhere: harness, online, two games at 3 frames a second) */
        uint32_t n = guest_rd32(page + 0x298), k;
        for (k = 0; k < n && k < 64; k++) {
            uint32_t w = guest_rd32(page + 0x40 + k * 4);
            char wn[16], lb[16];
            if (!in_heap(w))
                continue;
            guest_str(w + 0x28, wn, sizeof wn);
            if (strcmp(wn, "PROMPT1"))
                continue;
            guest_str(w + 0x121, lb, sizeof lb);
            if (guest_rd8(w + 0xE6) && !strcmp(lb, "Enter"))
                s_os_desk = now;            /* (the tick opens the book) */
            break;
        }
    }
    if (strcmp(name, "storyselect") && strcmp(name, "difficultyselect") && strcmp(name, "warningfrontend")
        && strcmp(name, "bookchapterview"))
        return;
    s_os_page = now;
    s_os_book = 1;                          /* past the desk: no more Start there */
    if (now - s_os_press < 1200000000ull)
        return;
    if (!strcmp(name, "storyselect")) {
        static const char *const cont[] = { "story_loadgame" }, *const nw[] = { "story_newgame" };
        uint32_t focus = guest_rd32(page + 0x31C);
        b = find_button(page, s_os_mode == 1 ? cont : nw, 1);
        if (b < 0)
            return;
        if (focus != (uint32_t)b)
            os_press(focus > (uint32_t)b ? 6 : 7, now, "the book: up/down");
        else
            os_press(1, now, s_os_mode == 1 ? "the book: Continue" : "the book: New");
    } else if (!strcmp(name, "warningfrontend")) {
        static const char *const yes[] = { "btnYes", "Yes", "YES" };
        b = find_button(page, yes, 3);
        if (b >= 0 && guest_rd32(page + 0x31C) != (uint32_t)b)
            os_press(4, now, "the warning: left to Yes");
        else
            os_press(1, now, "the warning: Yes");
    } else
        os_press(1, now, !strcmp(name, "bookchapterview") ? "the chapter: Begin" : "the difficulty: A");
}

static void page_pointer(uint32_t page)
{
    page_dump(page);
    /* The line-up's small option menus have no places to point at here:
     * there the mouse steps the highlight as on the desk, with the prompts
     * still clickable. (The journal's pages are Flash movies, pointed at in
     * rdr_swf_menu: a sideways movement stepping them turned the pages.) */
    {
        char nm[24];
        guest_str(page + 0x1C, nm, sizeof nm);
        if (guest_rd8(page + 0x3D) && !strncmp(nm, "multioption_", 12))
            rdr_input_steps();
    }
    warp_step(page);
    online_start_page(page);
    rdr_lineup_pointer(page);
    name_entry(page);
    if (guest_rd8(page + 0x3D)) {
        if (rdr_in_level())
            hud_scan(page, NULL);
        else if (page_cal(page))
            hud_scan(page, page_cal(page));
        else if (front_hud(page) || front_flat(page))
            hud_scan(page, NULL);
    }
    /* In a level, the page on screen is flat over the picture. In the
     * front end the pages are drawn onto the book and the profile sheet:
     * those measured above, while shown (+0x3D). */
    if (rdr_in_level()) {
        if (page == guest_rd32(RDR_CUR_PAGE)) {
            menu_pointer(page, NULL);
            option_rows(page);
        }
    } else if (guest_rd8(page + 0x3D)) {
        const PageCal *cal = page_cal(page);
        if (cal)
            menu_pointer(page, cal);
        else if (front_flat(page) == 2 || (front_flat(page) && page == guest_rd32(RDR_CUR_PAGE))) {
            menu_pointer(page, NULL);
            option_rows(page);
        }
    }
}

#define PAGE_WRAP(a) \
    extern void sub_##a(void); \
    static void wrapped_page_##a(void) \
    { \
        uint32_t page = g_ecx; \
        page_pointer(page); \
        g_ecx = page; \
        sub_##a(); \
    }
PAGE_UPDATES(PAGE_WRAP)

/* Tests (RDR_COUNT_CALLS=1): how often a few virtual methods run. */
#define COUNTED(X) X(0014A0B0) X(0013AE50) X(000921C0)
#define COUNT_WRAP(a) \
    extern void sub_##a(void); \
    static volatile uint32_t s_count_##a; \
    static void counted_##a(void) { s_count_##a++; sub_##a(); }
COUNTED(COUNT_WRAP)

void rdr_count_report(void)
{
#define COUNT_PRINT(a) fprintf(stderr, " %s=%u", #a, s_count_##a);
    fprintf(stderr, "[TEST] calls:");
    COUNTED(COUNT_PRINT)
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Tests (RDR_ICALL_PROFILE=1, "icalls" on the test flag): how often each
 * indirect-call target ran since the last report -- what a level's
 * per-frame virtual calls are, to find the ones worth hooking. */
#define ICALL_SLOTS 8192
static volatile uint32_t s_ic_va[ICALL_SLOTS], s_ic_n[ICALL_SLOTS];
static int s_ic_on = -1;

static void icall_count(uint32_t va)
{
    uint32_t h = (va * 2654435761u) >> 19, k;
    for (k = 0; k < 64; k++, h = (h + 1) & (ICALL_SLOTS - 1)) {
        if (s_ic_va[h] == va) { s_ic_n[h]++; return; }
        if (!s_ic_va[h]) { s_ic_va[h] = va; s_ic_n[h] = 1; return; }
    }
}

void rdr_icall_report(void)
{
    uint32_t i, k, frames = 0;
    static uint32_t last_frame;
    static uint32_t top_va[3000], top_n[3000];
    frames = guest_rd32(RDR_FRAME_COUNT) - last_frame;
    last_frame = guest_rd32(RDR_FRAME_COUNT);
    for (i = 0; i < ICALL_SLOTS; i++) {
        uint32_t n = s_ic_n[i], va = s_ic_va[i];
        if (!va) continue;
        for (k = 0; k < 3000; k++)
            if (n > top_n[k]) {
                memmove(&top_n[k + 1], &top_n[k], (2999 - k) * sizeof top_n[0]);
                memmove(&top_va[k + 1], &top_va[k], (2999 - k) * sizeof top_va[0]);
                top_n[k] = n; top_va[k] = va;
                break;
            }
        s_ic_n[i] = 0;
    }
    fprintf(stderr, "[TEST] icalls over %u frames:", frames);
    for (k = 0; k < 3000 && top_n[k]; k++)
        fprintf(stderr, "%s %08X=%u", k % 8 ? "" : "\n ", top_va[k], top_n[k]);
    fprintf(stderr, "\n");
    memset(top_va, 0, sizeof top_va);
    memset(top_n, 0, sizeof top_n);
    fflush(stderr);
}

/* ── Freeze NPCs ───────────────────────────────────────────────
 *
 * Every character is updated once a frame through its vtable +8. The
 * player's class (vtable 0x003D9FA8) has its own (sub_0014A0B0); the other
 * character classes that share the layout -- 0x003D9C30 (sub_0013AE50),
 * 0x003CF7B0 (sub_000921C0) and 0x003CE070 (sub_00076F80), the people and
 * animals the levels place -- are skipped while the cheat is on: they stand
 * where they are, mid-stride, and neither move, shoot nor notice anyone.
 * Found by counting indirect calls (RDR_ICALL_PROFILE) per frame against
 * the player's vtable and its siblings'. */
int g_rdr_freeze_npcs;
static volatile uint32_t s_frozen_calls;

#define NPC_UPDATES(X) X(0013AE50) X(000921C0) X(00076F80)
/* Tests ("npchurt"): the characters these updates were for, last first. */
static uint32_t s_npc_seen[16];
static void npc_seen(uint32_t c)
{
    int k;
    for (k = 0; k < 16 && s_npc_seen[k] != c; k++)
        ;
    if (k == 16) k = 15;
    for (; k > 0; k--)
        s_npc_seen[k] = s_npc_seen[k - 1];
    s_npc_seen[0] = c;
}
#define NPC_WRAP(a) \
    extern void sub_##a(void); \
    static void frozen_##a(void) \
    { \
        npc_seen(g_ecx); \
        if (g_rdr_freeze_npcs) { \
            s_frozen_calls++; \
            g_esp += 4;                     /* ret: the update is skipped */ \
            return; \
        } \
        sub_##a(); \
    }
NPC_UPDATES(NPC_WRAP)

/* The characters' controllers (vtable 0x003CF54C, +8 sub_0008AE60, from
 * the enemy manager's update through sub_000871B0) move them too: a
 * gunman walking a path (the second chapter's, lvl_hng) kept walking with
 * his own update skipped (user report; gdb, a watchpoint on his position:
 * sub_0008AE60 -> sub_0008A150 -> sub_000D1D60). Skipped as well -- not
 * for the players, whose controllers are the same class: the controller's
 * character is [ctrl+8]. */
extern void sub_0008AE60(void);
static int coop_index(uint32_t c);
static void frozen_0008AE60(void)
{
    if (g_rdr_freeze_npcs) {
        uint32_t ch = guest_rd32(g_ecx + 8);
        if (in_heap(ch) && guest_rd32(ch) != 0x003D9FA8u && coop_index(ch) < 0) {
            s_frozen_calls++;
            g_esp += 4;                 /* ret */
            return;
        }
    }
    sub_0008AE60();
}

/* Guns in town. Brimstone keeps the player's guns holstered with a zone:
 * the level's zones ([0x0065AA48]: a list, [+0] -> {items, u16 count},
 * the type of each at +0x58) are tested against the player every update
 * (sub_00145FE0, from the player's update): inside one of type 3, bit 0 of
 * the player's +0xA04 bit set; inside one of type 4, bit 1 -- and with bit
 * 1 the left trigger reads as up (0x0014A691: no draw), and the fire and
 * right-stick buttons too (0x001461BF). Brimstone's streets, shops and
 * saloon are one type-4 zone; the Battle Royale's last day (the Mr. Kelley
 * fight, lvl_royfinal) has none, which is why the townsfolk can be shot
 * there. With the option on, the type-4 zones are given another type
 * (TOWN_ZONE_OFF, which nothing tests for) and given 4 back when it is
 * switched off; a level loads its zones fresh. */
#define TOWN_ZONE_OFF 0x80000004u
/* Who may hurt whom: a 6 x 6 byte table at 0x004065A8, the shooter's team
 * ([char+0xB4]) a row, the one hit's a column; bit 1 (2) lets the bullet do
 * its damage (sub_0008F9E0, from the hit, sub_00088050 at 0x0008855D via
 * vtable +0xB8). Players are team 0. Brimstone's townsfolk are team 3, the
 * player's own side (row 0, column 3: 0) -- a bullet that met one did
 * nothing at all, no blood, no flinch (user report); in the Mr. Kelley
 * fight the same people are team 4 (6: hurt). With Guns in Town on, in a
 * level that has the no-gun zone, team 3 is hurt by the players' teams as
 * team 4 is; the table is put back when the option is off or the level has
 * no such zone. Harness: a bullet on a townsman stopped at 0x00088707 (that
 * check) before; the ray itself always met him (raytest). */
#define TEAM_TABLE 0x004065A8u
static uint8_t s_team_saved[6];
static int s_team_set;
static void town_guns_teams(int on)
{
    uint32_t cnt = guest_rd32(0x0067592Cu), k;
    if (on && !s_team_set) {
        for (k = 0; k < 6; k++)
            s_team_saved[k] = guest_rd8(TEAM_TABLE + k * 6u + 3u);
        s_team_set = 1;
    }
    if (!s_team_set)
        return;
    for (k = 0; k < 6; k++) {
        uint8_t want = s_team_saved[k];
        if (on) {
            uint32_t j;
            int player_team = k == 0;
            for (j = 0; j < cnt && j < 8; j++) {
                uint32_t c = guest_rd32(0x00675930u + j * 4u);
                if (in_heap(c) && guest_rd32(c + 0xB4) == k)
                    player_team = 1;
            }
            if (player_team)
                want = (uint8_t)(s_team_saved[k] | guest_rd8(TEAM_TABLE + k * 6u + 4u));
        }
        if (guest_rd8(TEAM_TABLE + k * 6u + 3u) != want)
            *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + TEAM_TABLE + k * 6u + 3u) = want;
    }
    if (!on)
        s_team_set = 0;
}

extern int rdr_town_guns(void);
static void town_guns_zones(void)
{
    uint32_t zm = guest_rd32(0x0065AA48u), arr, items, n, i, want = rdr_town_guns() != 0;
    static int logged = -1;
    int town = 0;
    if (!in_heap(zm) || !in_heap(arr = guest_rd32(zm))) {
        town_guns_teams(0);
        return;
    }
    items = guest_rd32(arr);
    n = guest_rd32(arr + 4) & 0xFFFFu;
    if (!in_heap(items) || n > 4096) {
        town_guns_teams(0);
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t z = guest_rd32(items + i * 4), t;
        if (!in_heap(z))
            continue;
        t = guest_rd32(z + 0x58);
        if (t == 4 || t == TOWN_ZONE_OFF)
            town = 1;
        if (want && t == 4)
            guest_wr32(z + 0x58, TOWN_ZONE_OFF);
        else if (!want && t == TOWN_ZONE_OFF)
            guest_wr32(z + 0x58, 4);
        else
            continue;
        if (logged != (int)want) {
            logged = (int)want;
            fprintf(stderr, "[RDR] guns in town %s (no-gun zone %u of %u)\n", want ? "on" : "off", i, n);
            fflush(stderr);
        }
    }
    town_guns_teams(want && town);
}

void rdr_freeze_report(void)
{
    fprintf(stderr, "[TEST] freeze %d: %u updates skipped\n", g_rdr_freeze_npcs, s_frozen_calls);
    fflush(stderr);
}

/* ── Story co-op ───────────────────────────────────────────────
 *
 * Levels make their players in sub_00138090 from descriptors at 0x00653338
 * (0x5C each: +0 the player number, -1 none; +4 the character; +0x18 a
 * second character for the same player; +0x54 the camera), one in the
 * story, four in Showdown ([0x00652E30]). Each becomes a character object
 * in [0x00675930 + n*4] (count [0x0067592C]) driven by player slot n
 * (0x00675968 + n*0x148), whose number picks the controller.
 *
 * The experiment: as a story level is made (rdr_hooks.c, right after its
 * players are), run it once more
 * with a copy of player 1's descriptor numbered 1 -- a second Red, on the
 * second controller, sharing the camera -- and put back what it sets for
 * "the" player (0x00652E38, 0x00652E4C, 0x00652E28). */
#define RDR_PLAYER_DESC     0x00653338u
#define RDR_PLAYER_COUNT    0x0067592Cu
extern void sub_00138090(void);
extern void sub_001EB490(void), sub_001EB4C0(void), sub_00219D60(void);
extern void sub_00272010(void), sub_0005AFC0(void);
static int s_coop_copy_slot = 1;
volatile int s_coop_restore_count;

void rdr_coop_tick(void) { }

/* Once the level runs -- the players' first update, after the set-up has
 * given every player a HUD and a view:
 *  - player k reads controller port k: [character+0x894] is its pad
 *    object, one of four at 0x006795C8 (0xB0 each, one per port), and the
 *    spawn gave it player 1's;
 *  - one screen: one local player and one view again ([0x00652E54], and
 *    [0x00652B48], the count of views at 0x00652B2C that each draw the
 *    whole scene); the others share player 1's camera;
 *  - split screen: every view stays, in its own part of the screen (two:
 *    top and bottom, or left and right; three or four: quarters). A view
 *    is {+4 aspect, +8..+0x14 its rectangle now, +0x1C..+0x28 the one it
 *    eases to}, each x, y, width, height in fractions of the screen.
 * Settings (F1, Cheats: Story co-op) or RDR_COOP_TEST=<players> and
 * RDR_COOP_SPLIT=0/1/2 for tests. */
#define RDR_VIEWS      0x00652B2Cu
#define RDR_VIEW_COUNT 0x00652B48u
#define RDR_PADS       0x006795C8u
static uint32_t s_coop_p[4];
static int s_coop_n;
/* Each extra player's brain as its spawn made it (the empty
 * playerbrain_multipartner_tester), and whether the level has gone on into
 * another of the same chapter's levels (lvl_roy*: coop_brain_keep). */
static uint32_t s_coop_sm[4];
static int s_coop_kept;
static int s_coop_place;                    /* bit k: player k+1 to place */
static int s_coop_place_near;               /* ... even when near (a chapter's other level) */
static int s_coop_cleared;              /* we set [0x00652B24] */
/* What co-op changed outside the level, put back when it ends: the
 * players' slots 2-4 (0x00675968 + k*0x148, copied from slot 1) and the
 * count of local players [0x00652E54]. Left as co-op had them, the front
 * end went on as if four players were signed in. */
static uint8_t s_coop_slot_saved[4][0x148];
/* The level co-op was set up in ([0x0065AA0C], set by sub_0007A250 as a
 * level loads): the players' list is not emptied when one ends, so this is
 * how the end is seen. */
#define RDR_LEVEL 0x0065AA0Cu
static uint32_t s_coop_level;
static uint32_t s_coop_local_saved;
static int s_coop_saved_n;

static void coop_put_back(volatile uint8_t *m)
{
    int k;
    uint32_t i;
    if (!s_coop_saved_n)
        return;
    for (k = 1; k < s_coop_saved_n; k++)
        for (i = 0; i < 0x148; i++)
            m[0x00675968u + 0x148u * (uint32_t)k + i] = s_coop_slot_saved[k][i];
    *(volatile uint32_t *)(m + 0x00652E54u) = s_coop_local_saved;
    fprintf(stderr, "[COOP] level over: player slots and local count (%u) put back\n", s_coop_local_saved);
    fflush(stderr);
    s_coop_saved_n = 0;
}
static uint32_t s_coop_views[4];        /* the split's views, in order */
#define RDR_CINEMA 0x00652B50u
#define RDR_CLEAR_ONCE 0x00652B24u

extern int rdr_coop_players(void);      /* rdr_settings.c: 1 = off, 2-4 */
extern int rdr_coop_split(void);        /* 0 one screen, 1 top/bottom, 2 side by side, 3 windows */

static int coop_players(void)
{
    const char *e = getenv("RDR_COOP_TEST");
    int n = e && *e ? atoi(e) : rdr_coop_players();
    return n < 1 ? 1 : n > 4 ? 4 : n;
}

extern int rdr_lockstep_mode(void);
extern int rdr_ls_my_port(void);

static int coop_split(void)
{
    const char *e = getenv("RDR_COOP_SPLIT");
    int v = e && *e ? atoi(e) : rdr_coop_split();
    /* online: the quarters on every machine, each showing its own (below,
     * online_view) -- the same views everywhere, so the same game */
    if (rdr_lockstep_mode() == 4 && v)
        return 3;
    return v < 0 || v > 3 ? 0 : v;
}

static void view_rect(volatile uint8_t *m, uint32_t v, float x0, float y0, float x1, float y1)
{
    /* Stored as x, y, width, height; sub_000524F0 derives the aspect. */
    float r[4] = { x0, y0, x1 - x0, y1 - y0 };
    int i;
    for (i = 0; i < 4; i++) {
        *(volatile float *)(m + v + 0x08 + i * 4) = r[i];
        *(volatile float *)(m + v + 0x1C + i * 4) = r[i];
    }
}

/* Every frame, before the views are drawn (rdr_hooks.c, the entry of
 * sub_00051970, which draws them all) -- also while a cutscene keeps the
 * players' own updates from running. */
/* A level is being set up (rdr_hooks.c, sub_0007A250): if it is not the
 * one co-op was made in, co-op is over -- put back what it changed before
 * the new level (the front end, after Quit) reads it: with two local
 * players and the old level's views it drew garbage in two halves. */
/* Levels in one archive (the .dat files in lvls_sng), what a chapter switches among. */
static int chapter_group(const char *n)
{
    static const char *const g[][8] = {
        { "lvl_tomb", "lvl_bank", "lvl_bar", "lvl_closhop", "lvl_genstore", "lvl_gunshop", "lvl_tombsher", 0 },
        { "lvl_roydusk", "lvl_royale", "lvl_barnight", "lvl_royfinal", 0 },
        { "lvl_barfight", "lvl_theater", 0 },
        { "lvl_core", "lvl_jailnew", 0 },
        { "lvl_mansinta", "lvl_mansintb", 0 },
        { "lvl_woods", "lvl_waterfall", 0 },
    };
    unsigned i, j, c;
    for (i = 0; i < sizeof g / sizeof g[0]; i++)
        for (j = 0; g[i][j]; j++) {
            for (c = 0; n[c] && (n[c] | 0x20) == (g[i][j][c] | 0x20); c++)
                ;
            if (!n[c] && !g[i][j][c])
                return (int)i + 1;
        }
    return 0;
}

static int same_chapter(const char *a, const char *b)
{
    int ga = chapter_group(a);
    return ga && ga == chapter_group(b);
}

void rdr_coop_level_switch(uint32_t level)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    if (!s_coop_saved_n && s_coop_n < 2)
        return;
    if (level == s_coop_level)
        return;
    {
        char a[32] = "", b[32] = "";
        if (in_heap(level)) guest_str(level + 0x14, a, sizeof a);
        if (in_heap(s_coop_level)) guest_str(s_coop_level + 0x14, b, sizeof b);
        fprintf(stderr, "[COOP] level %08X '%s' made current (co-op's %08X '%s'), player 1 %08X (co-op's %08X)\n",
                level, a, s_coop_level, b, guest_rd32(0x00675930u), s_coop_p[0]);
        fflush(stderr);
        /* A chapter's other levels, in its own archive and made current
         * with the same players and nothing loaded: Brimstone's stores, bank,
         * saloon and Sheriff's Office (lvl_tomb's lvl_genstore ...
         * lvl_tombsher), the Battle Royale's arena (lvl_roydusk 6 and
         * lvl_royfinal 7 go on into lvl_royale) and the rest below. Co-op
         * stays: it ended there, and the picture went black -- the split
         * screen's views with one player in them in the arena; entering a
         * store, and the Sheriff's Office cutscene at a chapter's end (user
         * video). */
        if (s_coop_n >= 2 && same_chapter(a, b) && guest_rd32(0x00675930u) == s_coop_p[0]) {
            s_coop_level = level;
            /* the others where they stood in the other level: inside a
             * store's walls, back in town (harness) -- beside player 1 at
             * the first update of play */
            s_coop_place = ((1 << s_coop_n) - 1) & ~1;
            s_coop_place_near = 1;
            s_coop_kept = 1;
            fprintf(stderr, "[COOP] the same chapter goes on: co-op kept\n");
            fflush(stderr);
            return;
        }
    }
    s_coop_n = 0; s_coop_kept = 0;
    memset(s_coop_views, 0, sizeof s_coop_views);
    if (s_coop_cleared) {
        *(volatile uint8_t *)(m + RDR_CLEAR_ONCE) = 0;
        s_coop_cleared = 0;
    }
    coop_put_back(m);
}

static uint32_t s_coop_updated[4];          /* the frame each player last ran */

/* 1 while split screen shows the players' views (not a cutscene): the
 * letterbox bars are not drawn then (rdr_hooks.c, sub_001D5F80). */
/* Players in this co-op level (0 or 1: none). */
int rdr_coop_count(void)
{
    return s_coop_n;
}

/* 1 while a cutscene plays (the game's cinema flag). */
int rdr_cinema_playing(void)
{
    return guest_rd8(RDR_CINEMA) != 0;
}

int rdr_coop_split_shown(void)
{
    return coop_split() && s_coop_n >= 2 && s_coop_views[0] && !guest_rd8(RDR_CINEMA);
}

/* Player k's part of the screen in split screen, as fractions from the
 * top left (x, y, width, height): online play sends a guest only its own
 * view. 0 when the screen is not split. */
int rdr_coop_view_frac(int k, float r[4])
{
    int i;
    if (!rdr_coop_split_shown() || k < 0 || k >= s_coop_n || k >= 4 || !in_heap(s_coop_views[k]))
        return 0;
    for (i = 0; i < 4; i++)
        r[i] = guest_rdf(s_coop_views[k] + 8u + 4u * (uint32_t)i);
    return r[2] > 0.05f && r[3] > 0.05f && r[0] >= 0.0f && r[1] >= 0.0f && r[0] + r[2] <= 1.01f && r[1] + r[3] <= 1.01f;
}

/* ── Co-op changes on Restart Level ──────────────────────────────
 *
 * Restart Level (the pause menu, then YES) restarts the level in place:
 * sub_0005EA50 respawns the players already made (sub_00137650) and the
 * level set-up that makes them (sub_00138090, then rdr_coop_level_start)
 * does not run again. So a change to Story co-op (how many players, who
 * they are, Play as) waited for the next level (user: "restart a level to
 * adjust the amount of players"). When the restart's flag [0x00652014]
 * goes up and those settings are not what the level was made with, the
 * flag is taken down again and the chapter is started afresh instead,
 * through the front end as Go to Chapter does (rdr_warp_request): its
 * set-up makes the players as the settings say now. (Loading the level
 * again in place -- the loaded level's name changed so sub_0005EA50 takes
 * its reload path -- was tried: the name is put back before the restart
 * reads it, and a reload keeps the players it has.) The Quick menu and the
 * Story Co-op page do the same at once (rdr_coop_apply_now). */
#define RDR_RESTART_FLAG 0x00652014u
static char s_coop_made_sig[200];

static void coop_signature(char *buf, size_t n)
{
    const char *pa = rdr_play_as();
    int k, o;
    o = snprintf(buf, n, "%d|%s", coop_players(), pa ? pa : "");
    for (k = 1; k < 4 && o > 0 && (size_t)o < n; k++) {
        const char *c = rdr_coop_char(k);
        o += snprintf(buf + o, n - (size_t)o, "|%s", c ? c : "");
    }
}

/* 1 in a story level whose players are not what Story co-op says now. */
int rdr_coop_changed(void)
{
    char sig[200];
    if (!s_coop_made_sig[0] || guest_rd32(0x00652E30u) != 0 || !rdr_in_level() || rdr_warp_pending())
        return 0;
    coop_signature(sig, sizeof sig);
    return strcmp(sig, s_coop_made_sig) != 0;
}

/* The chapter started afresh, so its players are made as the settings say. */
int rdr_coop_apply_now(void)
{
    char name[64];
    int var = 1;
    if (!rdr_level_current(name, sizeof name, &var) || rdr_warp_pending())
        return 0;
    fprintf(stderr, "[COOP] co-op changed: %s %d started again\n", name, var);
    fflush(stderr);
    rdr_warp_request(name, var);
    return 1;
}

static void coop_restart_check(volatile uint8_t *m)
{
    static int seen;                        /* this raising of the flag looked at */
    if (!guest_rd8(RDR_RESTART_FLAG)) {
        seen = 0;
        return;
    }
    if (seen)
        return;
    seen = 1;
    if (!rdr_coop_changed())
        return;
    if (rdr_coop_apply_now())
        m[RDR_RESTART_FLAG] = 0;            /* not in place: afresh */
}

static void coop_brain_keep(uint32_t c);
static void coop_duels_tick(void);
static int coop_duels_split(void);
/* The film letterbox of the 3D picture. Besides the two black quads
 * (rdr_hooks.c, sub_001D5F80), while playing the game draws the world in a
 * band 1.2 times wider than the screen: [0x00652B51] set, the view's aspect
 * is multiplied by 1.2 ([0x00404500], sub_000524F0) and its viewport cut to
 * match (sub_000520A0) -- 400 of 480 lines, black above and below. Set at a
 * level's start (0x000641B2), clear with split screen (0x00051AB7). With
 * Display: "Black bars" off it is kept clear -- the whole screen, at the
 * screen's own shape -- and the game's own value put back when on again
 * (in cutscenes, with "in cutscenes only"). */
#define RDR_FILM_BAND 0x00652B51u
/* Cutscenes have bands of their own: the view's film object (sub_001D98D0,
 * the per-view drawing that also calls sub_001D5F80 with it) has at +0x1C
 * an object whose +0x10..+0x1C are the bands' heights (as fractions of the
 * view: top, bottom, each now and where it is going); sub_001DDC70 draws
 * them (sub_001DD140, at least 20 lines high on a 16:9 screen) unless all
 * four are 0. Found with RDR_PRIM_LOG in Brimstone's first cutscene: two
 * quads a frame from 0x001DD213. With the bars off they are kept at 0
 * outside the band object's own update (hud_update). */
static uint32_t s_lb_film, s_lb_film_frame;
static uint32_t s_coop_frame;               /* frames drawn (rdr_coop_views) */

void rdr_letterbox_film(uint32_t film)
{
    s_lb_film = film;
    s_lb_film_frame = s_coop_frame;
}

/* The HUD's switch ([0x0040C730], cleared by sub_0006DDE0 and set by
 * sub_001DB7E0 / sub_001DA3A0 / sub_0006DFD0), the cutscene flag and the
 * film band flag, logged when they change (user video, Chapter 8: after a
 * duel the HUD did not come back; not seen in the harness yet). */
static void hud_state_log(void)
{
    {
        /* player 1's "cannot be hurt" bit ([[p+0xC]+0x98] bit 4) */
        static int was_inv = -1;
        uint32_t p = guest_rd32(0x00675930u), c = in_heap(p) ? guest_rd32(p + 0xC) : 0;
        int inv = in_heap(c) ? (int)((guest_rd32(c + 0x98) >> 4) & 1) : -1;
        if (inv != was_inv && inv >= 0) {
            fprintf(stderr, "[HEALTH] player 1 %s (flags %08X, cutscene %d)\n", inv ? "cannot be hurt" : "can be hurt",
                    guest_rd32(c + 0x98), guest_rd8(RDR_CINEMA));
            fflush(stderr);
        }
        was_inv = inv;
    }
    static int was_hud = -1, was_cin = -1, was_film = -1;
    int hud = guest_rd8(0x0040C730u), cin = guest_rd8(RDR_CINEMA), film = guest_rd8(RDR_FILM_BAND);
    if (hud == was_hud && cin == was_cin && film == was_film)
        return;
    {
        uint32_t lvl = guest_rd32(RDR_LEVEL);
        char nm[32] = "";
        if (in_heap(lvl))
            guest_str(lvl + 0x14, nm, sizeof nm);
        fprintf(stderr, "[HUD] shown %d, cutscene %d, film band %d (bars setting %d) in %s\n", hud, cin, film,
                g_rdr_settings.letterbox, nm[0] ? nm : "-");
        fflush(stderr);
    }
    was_hud = hud; was_cin = cin; was_film = film;
}

/* Known issue: the stagecoach chase (lvl_coach, Chapter 14 "Sunset
 * Canyon") above 60 frames a second -- the coach is flung into the sky
 * and around the map (user video at fps_cap 500), and the rail-vehicle
 * fix (219) did not cure it (user: "The Stagecoach bug is still there").
 * Until it is found, the frame rate is held at 60 in that level (the
 * console's: there it plays as it should -- user), whatever the setting,
 * and put back on the way out. The Frame rate option says so while held
 * (rdr_frame_rate_held). */
static volatile int s_rate_held;
int rdr_frame_rate_held(void) { return s_rate_held; }

void rdr_level_rate_tick(void)
{
    static uint32_t last_lvl;
    static int coach;
    uint32_t lvl = guest_rd32(RDR_LEVEL);
    int set_hz = g_rdr_settings.fps_cap == 30 || !g_rdr_settings.fps_cap ? 60 : g_rdr_settings.fps_cap;
    if (lvl != last_lvl) {
        char nm[32] = "";
        last_lvl = lvl;
        if (in_heap(lvl))
            guest_str(lvl + 0x14, nm, sizeof nm);
        coach = !strcmp(nm, "lvl_coach");
    }
    if (coach && set_hz > 60) {
        if (xbox_VblankHz() != 60)
            xbox_VblankSetHz(60);
        if (!s_rate_held) {
            s_rate_held = 1;
            fprintf(stderr, "[RATE] the stagecoach chapter: frame rate held at 60 (set to %d; known issue)\n",
                    set_hz);
            fflush(stderr);
        }
    } else if (s_rate_held) {
        s_rate_held = 0;
        xbox_VblankSetHz(set_hz);
        fprintf(stderr, "[RATE] frame rate back to %d\n", set_hz);
        fflush(stderr);
    }
}

static void letterbox_tick(void)
{
    static int game_val = -1, written = -1;
    int cur = guest_rd8(RDR_FILM_BAND), lb = g_rdr_settings.letterbox, want;
    if (cur != written || game_val < 0)
        game_val = cur;                     /* the game's own write */
    /* the 2002 look has its own bands while playing (rdr_hud2002.cpp) */
    if (lb == 2 && g_rdr_settings.look2002 && rdr_hud2002_hide_mask())
        lb = 1;
    want = lb == 0 || (lb == 1 && !guest_rd8(RDR_CINEMA)) ? 0 : game_val;
    if (cur != want)
        guest_wr8(RDR_FILM_BAND, (uint8_t)want);
    written = want;
    hud_state_log();
    (void)s_lb_film_frame;          /* (the cutscene bands: hud_update below) */
}

/* Guns in Town: Brimstone's people come back (settings: town_respawn).
 * Someone killed lies for the body's time ([inst+0x298], 10 s) and is then
 * taken out by the enemy manager's update (sub_00074D10, vtable +0x14:
 * [inst+0xC] the controller back to the pool, [inst+8] cleared) -- and,
 * dead, is never brought in again: that update only brings in the living
 * (harness: Jody, "Kid" Cougar and a townsman killed with "enmkill" stayed
 * out, health 0, still wanted). The bring-in itself revives (sub_00072CA0,
 * the spawner's way in: health back, a spawn point of the level's picked).
 * So, with Guns in Town and this on, in Brimstone and its buildings (the
 * shopkeepers are their stores' people), one of the level's people who is
 * not against the players and has been dead and out for 60 s is brought
 * in again where the level puts them. One still lying there after 90 s
 * (a body the game keeps) is taken out first, as the update does. */
static int spawn_usable(uint32_t inst);
static void guest_push(uint32_t v);
extern void sub_00072CA0(void);
#define TOWN_BACK_S 60.0f

/* The level's controllers: a fixed pool ([0x0065A960], [0x0065A980] of
 * them; 15 on the ranch) shared by everyone in at once, people and
 * animals. The bring-in (sub_00072CA0) takes a free one (sub_00072C50: its
 * object's user [obj+8] is 0) and does not check that there was one: with
 * none left it went on with nothing and crashed (harness: the eighth
 * animal made on the ranch, sub_00076FD0). How many are free. */
int rdr_npc_room(void)
{
    uint32_t n = guest_rd32(0x0065A980u), arr = guest_rd32(0x0065A960u), k;
    int free_n = 0;
    if (!in_heap(arr) || n > 256)
        return 0;
    for (k = 0; k < n; k++) {
        uint32_t c = guest_rd32(arr + 4 * k), o = in_heap(c) ? guest_rd32(c + 0x1C) : 0;
        if (in_heap(o) && !guest_rd32(o + 8))
            free_n++;
    }
    return free_n;
}

static void town_respawn_tick(void)
{
    static struct { uint32_t inst; float t; } dead[48];
    static uint32_t mgr_was;
    uint32_t mgr = guest_rd32(0x0065A998u), lvl = guest_rd32(RDR_LEVEL), i;
    float dt = guest_rdf(0x00406504u);
    char nm[32] = "";
    int guard = 0, k;
    if (!g_rdr_settings.town_guns || !g_rdr_settings.town_respawn || !in_heap(mgr) || !in_heap(lvl)
        || guest_rd8(RDR_CINEMA) || !(dt > 0.0f && dt < 0.5f))
        return;
    guest_str(lvl + 0x14, nm, sizeof nm);
    if (chapter_group(nm) != 1)
        return;
    if (mgr != mgr_was) {                   /* another of the town's levels: its own people */
        memset(dead, 0, sizeof dead);
        mgr_was = mgr;
    }
    for (i = guest_rd32(mgr + 0x24); in_heap(i) && guard < 512; i = guest_rd32(i + 0x1B8), guard++) {
        uint32_t h = guest_rd32(i + 0x18), tm = guest_rd32(i + 0xB4), slot = 48;
        int is_dead = in_heap(h) && !(guest_rdf(h + 0x10) > 0.0f);
        for (k = 0; k < 48; k++)
            if (dead[k].inst == i) {
                slot = (uint32_t)k;
                break;
            }
        if (!is_dead || !spawn_usable(i) || (tm > 0 && tm < 6 && (guest_rd8(0x004065A8u + tm) & 1))) {
            if (slot < 48)
                dead[slot].inst = 0;
            continue;
        }
        if (slot == 48) {
            for (k = 0; k < 48 && dead[k].inst; k++)
                ;
            if (k == 48)
                continue;
            slot = (uint32_t)k;
            dead[slot].inst = i;
            dead[slot].t = 0.0f;
        }
        dead[slot].t += dt;
        if (guest_rd32(i + 0xC)) {
            /* still lying there: the game's own take-out, if it keeps the body */
            if (dead[slot].t >= TOWN_BACK_S + 30.0f) {
                uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, fn = guest_rd32(guest_rd32(i) + 0x14);
                extern recomp_func_t recomp_lookup(uint32_t xbox_va);
                recomp_func_t f = recomp_lookup(fn);
                if (f) {
                    guest_push(0x00074F31u);    /* as from the manager's update */
                    g_ecx = i;
                    f();
                }
                g_esp = esp0; g_ecx = ecx0; g_eax = eax0;
                guest_wr8(i + 8, 0);
                dead[slot].t = TOWN_BACK_S;     /* in again next time round */
            }
            continue;
        }
        if (dead[slot].t < TOWN_BACK_S)
            continue;
        {
            uint32_t tb = guest_rd32(i + 0x158), esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax;
            char t[16] = "";
            if (in_heap(tb))
                guest_wr8(tb + 0x40, 1);        /* wanted in (the spawner's note) */
            if (!rdr_npc_room())
                continue;                   /* (no controller free: later) */
            guest_push(i);
            guest_push(0x00074EA8u);
            g_ecx = mgr;
            sub_00072CA0();
            g_esp = esp0; g_ecx = ecx0; g_eax = eax0;
            if (in_heap(guest_rd32(i + 0x20)))
                guest_str(guest_rd32(i + 0x20) + 0x18, t, sizeof t);
            fprintf(stderr, "[RDR] townsfolk: %08X (%s) %s\n", i, t,
                    guest_rd32(i + 0xC) ? "back" : "could not come back yet (no free controller)");
            fflush(stderr);
            if (guest_rd32(i + 0xC))
                dead[slot].inst = 0;
            else
                dead[slot].t = TOWN_BACK_S - 5.0f;  /* again in 5 s */
        }
    }
}

/* Story co-op, Screen "Separate windows" (user: "separate game windows
 * for each co-op player ... one player on one monitor, and another player
 * on the other"). The players' views are laid out as quarters of the
 * frame -- each the screen's own shape, so nothing is stretched -- and the
 * toolkit shows each in a window: player 1's in the game's window, the
 * others' in windows of their own (xbox_PresentSetViews). A cutscene,
 * drawn full screen, goes to every window. */
extern void xbox_PresentSetViews(int n, const float *rects);
extern void xbox_PresentSetViewTitle(const char *fmt);
/* Online (lockstep), with more than one view -- Showdown's split screen,
 * or story co-op's -- each machine shows only its own player's view, over
 * the whole window. Every machine draws the same views in the same places
 * (what a view sees, and so what the game does, must not depend on whose
 * machine it is), and the toolkit presents just the one. Player k has view k; its port is its pad
 * object's ([player+0x894], one of four at 0x006795C8, 0xB0 each). Raise
 * Graphics > Resolution scale for a sharper picture: a part of the screen
 * is drawn at that part's size. */
static int online_view(float out[4])
{
    uint32_t n = guest_rd32(RDR_VIEW_COUNT), np = guest_rd32(0x0067592Cu), k, i, me;
    if (rdr_lockstep_mode() != 4 || n < 2 || n > 4 || np < 2 || guest_rd8(RDR_CINEMA)
        || guest_rd8(0x00651FF4u) || !guest_rd8(0x0040C730u))
        return 0;                       /* a cutscene, the pause menu: the whole screen */
    me = (uint32_t)rdr_ls_my_port();
    for (k = 0; k < n && k < np; k++) {
        uint32_t p = guest_rd32(0x00675930u + 4u * k), pad = in_heap(p) ? guest_rd32(p + 0x894) : 0;
        if (pad >= 0x006795C8u && (pad - 0x006795C8u) / 0xB0u == me)
            break;
    }
    if (k >= n || k >= np)
        return 0;
    {
        /* the view's own place (x, y, width, height): Showdown's own split
         * -- two players a strip each, about 4:1, three or four quarters --
         * with its HUD laid out for it (forcing quarters for two left the
         * HUD laid out for a strip: harness), story co-op's quarters */
        uint32_t v = guest_rd32(RDR_VIEWS + 4u * k);
        float r[4];
        if (!in_heap(v))
            return 0;
        for (i = 0; i < 4; i++)
            r[i] = guest_rdf(v + 8u + 4u * i);
        if (!(r[2] > 0.05f && r[3] > 0.05f && r[0] >= 0.0f && r[1] >= 0.0f && r[0] + r[2] <= 1.01f
              && r[1] + r[3] <= 1.01f))
            return 0;
        out[0] = r[0];
        out[1] = r[1];
        out[2] = r[0] + r[2];
        out[3] = r[1] + r[3];
    }
    return 1;
}

/* The player on a port, in split screen: its view (0-3) and that view's
 * place (x, y, width, height, fractions of the screen); -1 with one view,
 * in a cutscene, or online (each machine shows its own view whole). The
 * in-game menus open over the part of the player who opened them. */
static int s_views_windowed;
int rdr_port_view(int port, float r[4])
{
    uint32_t n = guest_rd32(RDR_VIEW_COUNT), np = guest_rd32(0x0067592Cu), k, i;
    if (port < 0 || port > 3 || n < 2 || n > 4 || np < 2 || guest_rd8(RDR_CINEMA) || rdr_lockstep_mode() == 4)
        return -1;
    for (k = 0; k < n && k < np; k++) {
        uint32_t p = guest_rd32(0x00675930u + 4u * k), pad = in_heap(p) ? guest_rd32(p + 0x894) : 0;
        if (pad >= 0x006795C8u && (pad - 0x006795C8u) / 0xB0u == (uint32_t)port)
            break;
    }
    if (k >= n || k >= np)
        return -1;
    {
        uint32_t v = guest_rd32(RDR_VIEWS + 4u * k);
        if (!in_heap(v))
            return -1;
        for (i = 0; i < 4; i++)
            r[i] = guest_rdf(v + 8u + 4u * i);
    }
    if (!(r[2] > 0.05f && r[3] > 0.05f && r[0] >= 0.0f && r[1] >= 0.0f && r[0] + r[2] <= 1.01f && r[1] + r[3] <= 1.01f))
        return -1;
    return (int)k;
}

/* The port of the player who opened the pause menu ([0x00652020], the
 * game's "Player N" on it), -1 outside a level or with one player. */
int rdr_pause_owner_port(void)
{
    uint32_t np = guest_rd32(0x0067592Cu), k = guest_rd32(0x00652020u), p, pad;
    if (np < 2 || np > 4 || k >= np)
        return -1;
    p = guest_rd32(0x00675930u + 4u * k);
    pad = in_heap(p) ? guest_rd32(p + 0x894) : 0;
    if (pad < 0x006795C8u || (pad - 0x006795C8u) / 0xB0u > 3)
        return -1;
    return (int)((pad - 0x006795C8u) / 0xB0u);
}

/* 1 while the views are in windows of their own (Separate windows). */
int rdr_views_windowed(void)
{
    return s_views_windowed;
}

static void coop_windows_tick(void)
{
    static int was, was_online;
    float r[4][4];
    int k, n = 0;
    {
        int on = online_view(r[0]);
        if (on != was_online) {
            fprintf(stderr, "[LS] own view only: %s\n", on ? "on" : "off");
            fflush(stderr);
            was_online = on;
        }
        if (on) {
            xbox_PresentSetViews(1, &r[0][0]);
            s_views_windowed = 0;
            was = 1;
            return;
        }
    }
    if (coop_split() == 3 && rdr_lockstep_mode() != 4 && s_coop_n >= 2 && s_coop_views[0]) {
        /* the whole picture in every window: a cutscene (the cinema flag;
         * and the talks that hide the HUD without it, [0x0040C730] 0 --
         * user screenshot, a soldier's close-up cropped to player 2's
         * quarter), the pause menu ([0x00651FF4], drawn over the whole
         * screen: each window showed its corner of it), or one view */
        int cut = (guest_rd8(RDR_CINEMA) != 0 && !coop_duels_split()) || !guest_rd8(0x0040C730u)
                  || guest_rd32(RDR_VIEW_COUNT) < 2;
        /* the pause menu: in the window of the player who opened it
         * ([0x00652020], the game's "Player N" on it) and in the game's
         * window; the others keep their picture (user: "player 2-4
         * unaffected by the menu opening") */
        int paused = guest_rd8(0x00651FF4u) != 0, owner = (int)guest_rd32(0x00652020u);
        for (k = 0; k < s_coop_n && k < 4; k++) {
            float f[4];
            if (paused && !cut) {
                if (k == 0 || k == owner) {
                    r[k][0] = 0.0f; r[k][1] = 0.0f; r[k][2] = 1.0f; r[k][3] = 1.0f;
                } else {
                    r[k][0] = 1.0f; r[k][1] = 0.0f; r[k][2] = 0.0f; r[k][3] = 1.0f;   /* held */
                }
                continue;
            }
            if (!cut && rdr_coop_view_frac(k, f)) {
                r[k][0] = f[0]; r[k][1] = f[1]; r[k][2] = f[0] + f[2]; r[k][3] = f[1] + f[3];
            } else {
                r[k][0] = 0.0f; r[k][1] = 0.0f; r[k][2] = 1.0f; r[k][3] = 1.0f;
            }
        }
        n = k;
    }
    if (n != was) {
        if (n) {
            xbox_PresentSetViewTitle("Red Dead Recompilation - Player %d");
            fprintf(stderr, "[COOP] separate windows: %d\n", n);
        } else
            fprintf(stderr, "[COOP] separate windows: off\n");
        fflush(stderr);
        was = n;
    }
    s_views_windowed = n >= 2;
    xbox_PresentSetViews(n, n ? &r[0][0] : NULL);
}

void rdr_coop_views(void)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    int split = coop_split(), k;
    letterbox_tick();
    coop_windows_tick();
    town_respawn_tick();
    warp_tick_level();
    coop_restart_check(m);
    s_coop_frame++;
    if (s_coop_n >= 2 && (guest_rd32(0x00675930u) != s_coop_p[0]
                          || guest_rd32(RDR_LEVEL) != s_coop_level)) {
        /* The level is gone (a restart, the front end, Showdown): hands off
         * until the next story level makes its players again. */
        fprintf(stderr, "[COOP] level gone: player 1 %08X (was %08X), level %08X (was %08X)\n",
                guest_rd32(0x00675930u), s_coop_p[0], guest_rd32(RDR_LEVEL), s_coop_level);
        fflush(stderr);
        s_coop_n = 0; s_coop_kept = 0;
        memset(s_coop_views, 0, sizeof s_coop_views);
        coop_put_back(m);
        if (s_coop_cleared) {
            *(volatile uint8_t *)(m + RDR_CLEAR_ONCE) = 0;
            s_coop_cleared = 0;
        }
        return;
    }
    for (k = 1; s_coop_kept && k < s_coop_n && k < 4; k++)
        coop_brain_keep(s_coop_p[k]);       /* also while a cutscene stops the updates */
    coop_duels_tick();
    if (s_coop_n >= 2) {
        /* The players' views, kept the first time the list holds as many
         * (co-op made one for each local player). A view the game puts in
         * the list that is none of them -- a store's, a cutscene's of its
         * own (the Sheriff's Office at a chapter's end) -- is left alone with
         * the whole list: the players' views put back over it had the
         * screen black for as long as it lasted (user video: the General
         * Store in Brimstone, and the Sheriff's Office cutscene, in split
         * screen). */
        uint32_t n = guest_rd32(RDR_VIEW_COUNT), i;
        int foreign = 0, j;
        static uint32_t last_list[6];
        uint32_t list[6] = { 0 };
        if (n >= (uint32_t)s_coop_n && n <= 8 && !s_coop_views[0])
            for (k = 0; k < s_coop_n; k++)
                s_coop_views[k] = guest_rd32(RDR_VIEWS + 4u * (uint32_t)k);
        list[0] = n;
        for (i = 0; i < n && i < 8; i++) {
            uint32_t v = guest_rd32(RDR_VIEWS + 4u * i);
            int ours = 0;
            for (j = 0; j < s_coop_n && j < 4; j++)
                if (v == s_coop_views[j])
                    ours = 1;
            if (!ours && s_coop_views[0])
                foreign = 1;
            if (i < 5)
                list[i + 1] = v;
        }
        if (memcmp(list, last_list, sizeof list)) {
            static int said;
            memcpy(last_list, list, sizeof list);
            if (said++ < 200) {
                fprintf(stderr, "[COOP] views: %u [%08X %08X %08X %08X]%s\n", n, list[1], list[2], list[3],
                        list[4], foreign ? " -- the game's own, left as they are" : "");
                fflush(stderr);
            }
        }
        if (foreign)
            return;
    }
    if (!split && s_coop_n >= 2) {
        /* One screen: one view, player 1's, also before the players'
         * first update (an opening cutscene) -- the whole screen (it kept
         * the top half after split screen was switched off). */
        if (guest_rd32(RDR_VIEW_COUNT) > 1)
            *(volatile uint32_t *)(m + RDR_VIEW_COUNT) = 1;
        if (s_coop_views[0] && guest_rd32(RDR_VIEWS) == s_coop_views[0])
            view_rect(m, s_coop_views[0], 0.0f, 0.0f, 1.0f, 1.0f);
        return;
    }
    if (split && s_coop_n >= 2) {
        /* The views, kept: a cutscene shows player 1's alone, full screen
         * ([0x00652B50] is set while one plays -- the letterboxed cinema
         * mode, sub_00052440), and the split comes back after it. */
        uint32_t n = guest_rd32(RDR_VIEW_COUNT);
        int cut = guest_rd8(RDR_CINEMA) != 0;
        static int last_cut = -1;
        if (cut != last_cut) {
            fprintf(stderr, "[COOP] cinema %s (views %u)\n", cut ? "on" : "off", n);
            fflush(stderr);
            last_cut = cut;
        }
        if (n >= (uint32_t)s_coop_n && !s_coop_views[0])
            for (k = 0; k < s_coop_n; k++)
                s_coop_views[k] = guest_rd32(RDR_VIEWS + 4u * (uint32_t)k);
        if (s_coop_views[0]) {
            if (cut && !coop_duels_split()) {
                if (n != 1) {
                    *(volatile uint32_t *)(m + RDR_VIEW_COUNT) = 1;
                    *(volatile uint32_t *)(m + RDR_VIEWS) = s_coop_views[0];
                }
                view_rect(m, s_coop_views[0], 0.0f, 0.0f, 1.0f, 1.0f);
            } else {
                if (n != (uint32_t)s_coop_n) {
                    for (k = 0; k < s_coop_n; k++)
                        *(volatile uint32_t *)(m + RDR_VIEWS + 4u * (uint32_t)k) = s_coop_views[k];
                    *(volatile uint32_t *)(m + RDR_VIEW_COUNT) = (uint32_t)s_coop_n;
                }
                for (k = 0; k < s_coop_n; k++) {
                    uint32_t v = s_coop_views[k];
                    if (!v)
                        continue;
                    if (s_coop_n > 2 || split == 3)
                        view_rect(m, v, (k & 1) * 0.5f, (k >> 1) * 0.5f, (k & 1) * 0.5f + 0.5f, (k >> 1) * 0.5f + 0.5f);
                    else if (split == 2)
                        view_rect(m, v, k * 0.5f, 0.0f, k * 0.5f + 0.5f, 1.0f);
                    else
                        view_rect(m, v, 0.0f, k * 0.5f, 1.0f, k * 0.5f + 0.5f);
                }
            }
            /* [0x00652B24]: clear the screen once a frame, not each view
             * each time it is made current (sub_000524F0 clears it when
             * this is 0). Showdown sets it; without it the story's later
             * drawing passes re-cleared every view to black. */
            *(volatile uint8_t *)(m + RDR_CLEAR_ONCE) = 1;
            s_coop_cleared = 1;
        }
    }
}

/* ── The money sparkle in split screen ─────────────────────────
 *
 * Money paid to a player (the HUD's sub_001D9DA0: a kill's combo money,
 * a pickup's bonus) starts a sparkle at the money box -- particle effect
 * 0x41, [hud+0x5478] / [hud+0x547C] -- at [0x0067A16C], [0x0067A170]:
 * screen pixels that the HUD's layout (sub_001D9420, from its draw, HUD
 * vtable +0x18 = sub_001D98D0) leaves there for the view it lays out. That
 * is the last view drawn. The story has one view, and Showdown, the only
 * game with several, makes no sparkle ([0x00652E30] set), so it never
 * mattered: in split-screen co-op the sparkle went to the last player's
 * money box whoever was paid (two players, top and bottom: the bottom-left
 * corner, under player 2's money; four: the lower right quarter's). So each
 * HUD's spot is kept as its view is drawn, and put in place while that
 * player's money changes -- the player's AddMoney (vtable +0x16C,
 * sub_0013E2F0) and its update (sub_0014A0B0, which pays the combo). */
#define RDR_HUD_MONEY_X  0x0067A16Cu
#define RDR_HUD_MONEY_Y  0x0067A170u
static uint64_t s_hud_drawn_ns;     /* the game's HUD last drawn (host clock; hud_draw) */
static int s_title_up;              /* the game's title menu ('logo01X') has shown */
#define RDR_VIEW_NOW     0x00652B04u      /* the view being drawn */
#define RDR_HUD_SHOWN    0x0040C730u
extern void sub_001D98D0(void), sub_0013E2F0(void), sub_001D90E0(void);
static struct { uint32_t hud; float x, y; } s_money_spot[4];

/* A HUD piece at the bottom right (the HUD's [hud+0x30], drawn by
 * sub_001E6D10 at 680, 454 of the 720x480 screen: a little white square,
 * red at times) sits under the bottom bar, which hides it in the game as
 * it was. With the bars off it showed in the corner of cutscenes and duels
 * (user: "this little white square in the bottom right corner"). While
 * the bars are not drawn it is not either: sub_001E6D10 draws nothing when
 * the piece's first word's first word is 0, so that word is 0 for the
 * HUD's drawing and put back after. */
extern int rdr_coop_split_shown(void);

/* ── The 2002 preview's HUD (Display: "2002 look") ───────────────────
 *
 * The HUD's parts mask [0x0067A1BC] (0x3FF in a level; the scripts'
 * HUD command, sub_000E62C0, sets and clears its bits): bit 0 the health
 * bar, 1 the Dead Eye meter (both drawn by the health object [hud+0x28],
 * sub_001E3030), 4 the bosses' bars, 7 the ammunition ([hud+0x2C],
 * sub_001E4230) -- found by clearing each in turn (harness, the ranch).
 * Money: sub_001D6400, drawn only after a change, whatever the mask.
 *
 * The health object: +0 the most, +4 the health shown (it follows the
 * real one), +0x1C the Dead Eye tanks, +0x20 how full they are (in
 * tanks). The player's health: [player+0x18] +0x10, the most +0x14. */
#define RDR_HUD_PARTS 0x0067A1BCu
#define HUD2002_HIDE  0x83u            /* health, Dead Eye, ammunition */

static uint32_t hud2002_player(void)
{
    uint32_t p = guest_rd32(0x00675930u), lvl = guest_rd32(RDR_LEVEL);
    char nm[16] = "";
    if (!in_heap(p) || !in_heap(lvl) || guest_rd32(0x00652024u) != 0)
        return 0;
    guest_str(lvl + 0x14, nm, sizeof nm);
    if (!strncmp(nm, "lvl_front", 9))
        return 0;
    return p;
}

/* One view on the screen, the story or a lone Showdown player. */
static int hud2002_one_view(void)
{
    return s_coop_n < 2 && guest_rd32(RDR_VIEW_COUNT) <= 1u;
}

uint32_t rdr_hud2002_hide_mask(void)
{
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("RDR_HUD2002_HIDE");
        env = e ? (int)strtol(e, NULL, 16) : -1;
    }
    if (!g_rdr_settings.look2002 || !hud2002_one_view() || !hud2002_player())
        return 0;
    return env >= 0 ? (uint32_t)env : HUD2002_HIDE;
}

/* The game's title, before the desk (the 2002 look's title picture,
 * rdr_hud2002.cpp): 1 from the end of the logos (a level named: the
 * title's attract level, lvl_sher70) until the front end (lvl_front) is
 * first named; 2 once Start was pressed there (loading the desk).
 * 0 before and after. */
int rdr_title_phase(void)
{
    static int front_seen, pressed;
    char lv[32];
    uint32_t k;
    if (front_seen)
        return 0;
    guest_str(0x00652E70u, lv, sizeof lv);
    if (!strcmp(lv, "lvl_front")) {
        front_seen = 1;
        return 0;
    }
    if (!lv[0] || hud2002_player())
        return 0;
    /* not over the intro: from the game's own title menu on (user: the
     * picture and its "Press Start" came up while the intro still played
     * behind it, before Start would do anything) */
    if (!s_title_up)
        return 0;
    for (k = 0; k < 4 && !pressed; k++) {
        uint32_t r = 0x004041A0u + k * 0x38u;
        if (guest_rd8(r + 0x24) & 0x10u)
            pressed = 1;                    /* Start (the pad record's digital buttons) */
    }
    return pressed ? 2 : 1;
}

int rdr_hud2002_read(RdrHud2002 *o)
{
    uint32_t p = hud2002_player(), hud, hv, h, ch, wl, tab, arr, valid, n, cur, item, gun, pool;
    uint32_t mask;
    memset(o, 0, sizeof *o);
    if (!p || !hud2002_one_view())
        return 0;
    hud = guest_rd32(p + 0x890);
    if (!in_heap(hud))
        return 0;
    mask = guest_rd32(RDR_HUD_PARTS);
    o->level = guest_rd32(RDR_LEVEL);
    /* shown while the game draws its own HUD: not over a chapter's loading
     * screen and the fade after it, where the new level's player is
     * already made and the HUD flag still set (user video) */
    o->hud_on = guest_rd8(RDR_HUD_SHOWN) && !guest_rd8(RDR_CINEMA)
                && s_hud_drawn_ns && xbox_HostNowNs() - s_hud_drawn_ns < 250000000ull;
    o->health_on = (mask & 1u) != 0;
    o->valor_on = (mask & 2u) != 0;
    o->ammo_on = (mask & 0x80u) != 0;
    hv = guest_rd32(hud + 0x28);
    h = guest_rd32(p + 0x18);
    if (in_heap(h)) {
        o->health = guest_rdf(h + 0x10);
        o->health_max = guest_rdf(h + 0x14);
    }
    if (in_heap(hv)) {
        o->health_shown = guest_rdf(hv + 4);
        if (o->health_max <= 0.0f)
            o->health_max = guest_rdf(hv);
        o->valor_max = guest_rdf(hv + 0x1C);
        o->valor = guest_rdf(hv + 0x20);
    }
    o->money = -1;
    if (!guest_rd32(0x00652E30u))
        o->money = (int)guest_rd32(p + 0x9FC);
    /* the gun in hand: [character+0x80] the weapons, [+0x48] the one
     * held, its item in the table [+0x20] (a gun per hand) */
    o->weapon = -1;
    ch = guest_rd32(p + 0xC);
    wl = in_heap(ch) ? guest_rd32(ch + 0x80) : 0;
    tab = in_heap(wl) ? guest_rd32(wl + 0x20) : 0;
    if (in_heap(tab)) {
        arr = guest_rd32(tab);
        n = guest_rd32(tab + 4) & 0xFFFFu;
        valid = guest_rd32(tab + 8);
        cur = guest_rd32(wl + 0x48);
        if (in_heap(arr) && in_heap(valid) && cur < n && n <= 256 && guest_rd8(valid + cur)
            && in_heap(item = guest_rd32(arr + cur * 4))) {
            int k;
            o->weapon = (int)cur;
            rdr_weapon_name((int)cur, o->weapon_name, (int)sizeof o->weapon_name);
            for (k = 0; k < 2; k++) {
                gun = guest_rd32(item + 4u * (uint32_t)k);
                if (!in_heap(gun))
                    continue;
                o->hands++;
                if (o->hands == 1) {
                    o->clip = (int)guest_rd32(gun + 0x20);
                    o->loaded = (int)guest_rd32(gun + 0x24);
                    pool = guest_rd32(gun + 0x28);
                    o->left = in_heap(pool) ? (int)guest_rd32(pool + 8) : -1;
                } else {
                    o->clip2 = (int)guest_rd32(gun + 0x20);
                    o->loaded2 = (int)guest_rd32(gun + 0x24);
                }
            }
        }
    }
    return 1;
}

static void hud_draw(void)
{
    uint32_t hud = g_ecx, piece = 0, keep = 0;
    int k, lb = g_rdr_settings.letterbox;
    if (lb == 2 && g_rdr_settings.look2002 && rdr_hud2002_hide_mask())
        lb = 1;                             /* (as letterbox_tick) */
    if (in_heap(hud) && (lb == 0 || (lb == 1 && !rdr_cinema_playing()) || rdr_coop_split_shown())) {
        uint32_t o = guest_rd32(hud + 0x30);
        piece = in_heap(o) ? guest_rd32(o) : 0;
        if (in_heap(piece)) {
            keep = guest_rd32(piece);
            guest_wr32(piece, 0);
        } else
            piece = 0;
    }
    {
        /* The 2002 look draws its own health, Dead Eye, ammunition and
         * money (rdr_hud2002.cpp): the game's are left out of this drawing
         * only -- the parts mask stays the game's (scripts and cutscenes
         * keep switching parts on and off; the 2002 HUD follows it). */
        uint32_t mask = guest_rd32(RDR_HUD_PARTS), hide = rdr_hud2002_hide_mask();
        s_hud_drawn_ns = xbox_HostNowNs();  /* (the 2002 HUD shows while the game's does) */
        if (hide)
            guest_wr32(RDR_HUD_PARTS, mask & ~hide);
        sub_001D98D0();
        if (hide)
            guest_wr32(RDR_HUD_PARTS, mask);
    }
    if (piece)
        guest_wr32(piece, keep);
    if (s_coop_n < 2 || !in_heap(hud) || guest_rd32(RDR_VIEW_NOW) != guest_rd32(hud + 0x53CC)
        || !guest_rd8(RDR_HUD_SHOWN))
        return;                             /* not laid out for its view now */
    for (k = 0; k < s_coop_n && k < 4; k++)
        if (s_coop_p[k] && guest_rd32(s_coop_p[k] + 0x890) == hud) {
            s_money_spot[k].hud = hud;
            s_money_spot[k].x = guest_rdf(RDR_HUD_MONEY_X);
            s_money_spot[k].y = guest_rdf(RDR_HUD_MONEY_Y);
            break;
        }
}

/* The cutscene bands with the bars off (Display: "Black bars"). The bands'
 * heights are the HUD's band object's ([hud+0x1C]) +0x10..+0x1C: what
 * the view is cut to and what sub_001DDC70 draws. They were cleared every
 * frame, and the band object's own update (sub_001DD530, from the HUD's
 * update, vtable +4 = sub_001D90E0) moves them towards its targets and
 * decides from them when the bands are in and out -- cleared, its
 * cutscene or duel never finished going out, and the HUD stayed hidden
 * after it until the bars were switched on again (user video and
 * report, Chapter 8 and others: "after a cutscene I did not have a ui
 * until I enabled the black bars again"). Now the update sees its own
 * heights: they are put back for it and cleared again after it, so the
 * view and the drawing see none. */
static struct { uint32_t o; uint32_t real[4]; int held; } s_band[4];

static void hud_update(void)
{
    uint32_t hud = g_ecx, o = in_heap(hud) ? guest_rd32(hud + 0x1C) : 0;
    int k, slot = -1, off = g_rdr_settings.letterbox == 0;
    if (in_heap(o)) {
        for (k = 0; k < 4; k++)
            if (s_band[k].o == o)
                slot = k;
        if (slot < 0)
            for (k = 0; k < 4 && slot < 0; k++)
                if (!s_band[k].held)
                    slot = k;
        if (slot >= 0 && s_band[slot].o == o && s_band[slot].held) {
            for (k = 0; k < 4; k++)
                guest_wr32(o + 0x10 + 4 * (uint32_t)k, s_band[slot].real[k]);
            s_band[slot].held = 0;
        }
    }
    sub_001D90E0();
    if (off && slot >= 0 && in_heap(o)) {
        static int said;
        uint32_t any = 0;
        s_band[slot].o = o;
        for (k = 0; k < 4; k++) {
            s_band[slot].real[k] = guest_rd32(o + 0x10 + 4 * (uint32_t)k);
            any |= s_band[slot].real[k];
            guest_wr32(o + 0x10 + 4 * (uint32_t)k, 0);
        }
        s_band[slot].held = 1;
        if (any && !said++) {
            float f[4];
            memcpy(f, s_band[slot].real, sizeof f);
            fprintf(stderr, "[RDR] black bars off: cutscene bands %.3f %.3f %.3f %.3f kept out of the picture\n",
                    f[0], f[1], f[2], f[3]);
            fflush(stderr);
        }
    }
}

/* The player's money spot in place (1, with the old one in keep), or 0. */
static int money_spot_take(uint32_t player, float keep[2])
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t hud;
    int k;
    if (!rdr_coop_split_shown() || !in_heap(player))
        return 0;
    hud = guest_rd32(player + 0x890);
    for (k = 0; k < s_coop_n && k < 4; k++)
        if (s_coop_p[k] == player && hud && s_money_spot[k].hud == hud) {
            keep[0] = guest_rdf(RDR_HUD_MONEY_X);
            keep[1] = guest_rdf(RDR_HUD_MONEY_Y);
            *(volatile float *)(m + RDR_HUD_MONEY_X) = s_money_spot[k].x;
            *(volatile float *)(m + RDR_HUD_MONEY_Y) = s_money_spot[k].y;
            return 1;
        }
    return 0;
}

static void money_spot_give_back(const float keep[2])
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    *(volatile float *)(m + RDR_HUD_MONEY_X) = keep[0];
    *(volatile float *)(m + RDR_HUD_MONEY_Y) = keep[1];
}

static uint32_t coop_money_to(uint32_t c);
static void player_add_money(void)
{
    float keep[2];
    int took;
    g_ecx = coop_money_to(g_ecx);       /* co-op, shared: player 1's purse */
    took = money_spot_take(g_ecx, keep);
    static int say = -1;
    if (say < 0)
        say = getenv("RDR_COOP_LOG") != NULL;
    if (say) {
        fprintf(stderr, "[COOP] money %d to %08X (spot %s %.0f,%.0f)\n", (int)guest_rd32(g_esp + 4), g_ecx,
                took ? "its own" : "as drawn", guest_rdf(RDR_HUD_MONEY_X), guest_rdf(RDR_HUD_MONEY_Y));
        fflush(stderr);
    }
    sub_0013E2F0();
    if (took)
        money_spot_give_back(keep);
}

/* ── The tutorial's controller in split screen ─────────────────
 *
 * The tutorial's controller picture (the controllerXbox_* models; one
 * object, [0x0067A9F4], vtable 0x003EB3D0, drawn by sub_001DF140) is drawn
 * with a camera of its own ([obj+0x4C]) whose viewport (+0x164: x, y,
 * width, height, min z, max z) is the whole screen. Its layer is drawn once
 * for each view (sub_00051970), so in split screen the picture sat where it
 * sits on a full screen, whichever view that fell in (two players: player
 * 2's half; four: across the right-hand quarters), while the words it goes
 * with are in player 1's. Now it is drawn once, in player 1's view: the
 * whole screen's shape scaled to fit that view, at the view's right, where
 * the picture sits on a full screen. */
#define RDR_CAM_VIEWPORT 0x164u
#define RDR_CAM_DIRTY    0x128u           /* bit 0: the viewport to set again */
extern void sub_001DF140(void);
static void pad_model_draw(void)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t obj = g_ecx, cam, cam1, keep[4], i;
    float s, sw, sh, w, h, x, y;
    if (!rdr_coop_split_shown() || !in_heap(obj) || !s_coop_views[0]) {
        sub_001DF140();
        return;
    }
    {
        static int said = -1;
        if (said < 0)
            said = getenv("RDR_COOP_LOG") ? 0 : 99;
        if (said < 8) {
            uint32_t now = guest_rd32(RDR_VIEW_NOW);
            int k = 0;
            while (k < 4 && s_coop_views[k] != now)
                k++;
            fprintf(stderr, "[COOP] controller picture drawn in view %d (frame %u)\n", k < 4 ? k + 1 : 0, s_coop_frame);
            fflush(stderr);
            said++;
        }
    }
    if (guest_rd32(RDR_VIEW_NOW) != s_coop_views[0]) {
        g_esp += 4;                         /* ret: another player's view */
        return;
    }
    cam = guest_rd32(obj + 0x4C);
    cam1 = guest_rd32(s_coop_views[0]);     /* player 1's view's camera */
    if (!in_heap(cam) || !in_heap(cam1)) {
        sub_001DF140();
        return;
    }
    for (i = 0; i < 4; i++)
        keep[i] = guest_rd32(cam + RDR_CAM_VIEWPORT + 4 * i);
    sw = (float)keep[2];
    sh = (float)keep[3];
    w = (float)guest_rd32(cam1 + RDR_CAM_VIEWPORT + 8);
    h = (float)guest_rd32(cam1 + RDR_CAM_VIEWPORT + 12);
    if (sw < 1.0f || sh < 1.0f || w < 1.0f || h < 1.0f) {
        sub_001DF140();
        return;
    }
    s = w / sw < h / sh ? w / sw : h / sh;
    x = (float)guest_rd32(cam1 + RDR_CAM_VIEWPORT) + (w - sw * s);
    y = (float)guest_rd32(cam1 + RDR_CAM_VIEWPORT + 4) + (h - sh * s) * 0.5f;
    *(volatile uint32_t *)(m + cam + RDR_CAM_VIEWPORT) = (uint32_t)(x + 0.5f);
    *(volatile uint32_t *)(m + cam + RDR_CAM_VIEWPORT + 4) = (uint32_t)(y + 0.5f);
    *(volatile uint32_t *)(m + cam + RDR_CAM_VIEWPORT + 8) = (uint32_t)(sw * s + 0.5f);
    *(volatile uint32_t *)(m + cam + RDR_CAM_VIEWPORT + 12) = (uint32_t)(sh * s + 0.5f);
    *(volatile uint32_t *)(m + cam + RDR_CAM_DIRTY) |= 1u;
    sub_001DF140();
    for (i = 0; i < 4; i++)
        *(volatile uint32_t *)(m + cam + RDR_CAM_VIEWPORT + 4 * i) = keep[i];
    *(volatile uint32_t *)(m + cam + RDR_CAM_DIRTY) |= 1u;
}

/* Tests (RDR_OVERLAY_FLAG "npchurt <damage>"): the most recently updated
 * living character that is not a player takes that much damage through its
 * TakeDamage (vtable +0x9C), at player 1's next update. */
static volatile int s_npc_hurt;
void rdr_test_npc_hurt(int damage) { s_npc_hurt = damage; }
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
static void guest_push(uint32_t v);
/* Tests ("phurt <player 1-4> <damage>"): that player takes the damage
 * through its TakeDamage, at its next update. */
static volatile int s_player_hurt, s_player_hurt_k;
void rdr_test_player_hurt(int k, int damage) { s_player_hurt_k = k; s_player_hurt = damage; }
static void player_hurt_now(uint32_t c)
{
    extern recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
    uint32_t h = guest_rd32(c + 0x18), a[7], db, esp0 = g_esp, ecx0 = g_ecx, fn = guest_rd32(guest_rd32(c) + 0x9C);
    float d = (float)s_player_hurt, before = in_heap(h) ? guest_rdf(h + 0x10) : -1.0f;
    recomp_func_t f = recomp_lookup_manual(fn);
    int j;
    s_player_hurt = 0;
    if (!in_heap(h))
        return;
    if (!f)
        f = recomp_lookup(fn);
    memcpy(&db, &d, 4);
    a[0] = db; a[1] = 1; a[2] = 0; a[3] = 0; a[4] = 0; a[5] = 0; a[6] = 0;
    if (f) {
        for (j = 6; j >= 0; j--)
            guest_push(a[j]);
        guest_push(0x000891ADu);
        g_ecx = c;
        f();
    }
    g_esp = esp0; g_ecx = ecx0;
    fprintf(stderr, "[TEST] phurt: %08X health %.1f -> %.1f\n", c, before, guest_rdf(h + 0x10));
    fflush(stderr);
}

/* Tests ("enmkill <n>"): the n-th of the level's people (enmdump's order)
 * takes the damage instead. */
static volatile uint32_t s_npc_hurt_target;
void rdr_test_enm_kill(int n)
{
    uint32_t mgr = guest_rd32(0x0065A998u), i;
    int k = 0;
    if (!in_heap(mgr))
        return;
    for (i = guest_rd32(mgr + 0x24); in_heap(i) && k < 512; i = guest_rd32(i + 0x1B8), k++)
        if (k == n) {
            s_npc_hurt_target = i;
            s_npc_hurt = 1000;
            return;
        }
}

static void npc_hurt_now(uint32_t player)
{
    int k, j;
    for (k = 0; k < 16; k++) {
        uint32_t c = s_npc_hurt_target ? (k ? 0 : s_npc_hurt_target) : s_npc_seen[k], h, cnt = guest_rd32(0x0067592Cu);
        int is_player = 0;
        if (!in_heap(c)) continue;
        for (j = 0; j < 8 && (uint32_t)j < cnt; j++)
            if (guest_rd32(0x00675930u + (uint32_t)j * 4u) == c) is_player = 1;
        h = guest_rd32(c + 0x18);
        if (is_player || !in_heap(h) || !(guest_rdf(h + 0x10) > 0.0f)) continue;
        {
            float d = (float)s_npc_hurt, before = guest_rdf(h + 0x10);
            uint32_t a[7], db;
            memcpy(&db, &d, 4);
            a[0] = db; a[1] = 1; a[2] = 0; a[3] = 0; a[4] = 0; a[5] = 0; a[6] = player;
            fprintf(stderr, "[TEST] npchurt: %08X (vtable %08X, +0x9C %08X) health %.1f/%.1f, %d damage\n", c,
                    guest_rd32(c), guest_rd32(guest_rd32(c) + 0x9C), before, guest_rdf(h + 0x14), s_npc_hurt);
            {
                /* as the game's indirect calls do: the overrides first */
                extern recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
                uint32_t esp0 = g_esp, ecx0 = g_ecx, fn = guest_rd32(guest_rd32(c) + 0x9C);
                recomp_func_t f = recomp_lookup_manual(fn);
                if (!f)
                    f = recomp_lookup(fn);
                if (f) {
                    for (j = 6; j >= 0; j--)
                        guest_push(a[j]);
                    guest_push(0x000891ADu);
                    g_ecx = c;
                    f();
                }
                g_esp = esp0; g_ecx = ecx0;
            }
            fprintf(stderr, "[TEST] npchurt: health now %.1f/%.1f\n", guest_rdf(h + 0x10), guest_rdf(h + 0x14));
            fflush(stderr);
        }
        break;
    }
    s_npc_hurt = 0;
    s_npc_hurt_target = 0;
}

/* Tests (RDR_OVERLAY_FLAG "pay <player> <amount>"): pays a player as a
 * pickup does, on the game's thread at that player's next update. */
static volatile int s_pay_player = -1, s_pay_amount;
void rdr_coop_test_pay(int player, int amount)
{
    s_pay_amount = amount;
    s_pay_player = player;
}

/* ── Showdown power-ups ────────────────────────────────────────
 *
 * The game's power-ups ("rewards": cr_* on a character, gb_* on everyone)
 * are made by name by the reward manager [0x006766AC]: sub_0015E470 takes
 * "name[:level][,name...]" and returns a reward set holding one of each.
 * A character wears one set at a time in [[character+0xC]+0x264]; giving
 * it one is what the pick-up message does (sub_0013AAD0, 0x0013AC55):
 *   set->+0x14(0, character, [character+0xC]+0x268)   sub_0015E070
 *   set->+0x0C(0, 0)                                  sub_0015E040, "start"
 *   [[character+0xC]+0x264] = set
 * and taking it off is sub_0015DFB0 (manager, set), which stops each reward
 * (message 6) and returns them to the pool. The cheat wears the power-ups
 * switched on in g_rdr_powerup_mask whenever the slot is empty, so a timed
 * one starts again when it runs out, and a power-up picked up in play is
 * left alone until it ends. */
#define RDR_REWARD_MGR   0x006766ACu
#define RDR_REWARD_SET_VT 0x003DC978u
extern void sub_0015E470(void), sub_0015E070(void), sub_0015E040(void), sub_0015DFB0(void);
extern RECOMP_TLS uint32_t g_eax;

static uint32_t s_pu_player[4], s_pu_set[4];

/* Tests (RDR_OVERLAY_FLAG "raytest"): the bullets' ray (the collision
 * world's ray test, as sub_0014E000 asks it: mask 0x00408B0F) from player 1
 * to each recently updated character: which collider it meets, against
 * the character's own ([char+0x20]+0x60, what the hit test compares). */
static volatile int s_ray_test;
void rdr_test_ray(void) { s_ray_test = 1; }
static uint32_t ray_once(uint32_t from_xyz[3], uint32_t to_xyz[3], uint32_t mask, uint32_t *hit)
{
    extern recomp_func_t recomp_lookup(uint32_t xbox_va);
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, ray, out, k, world = guest_rd32(0x00661D44u);
    recomp_func_t f = recomp_lookup(guest_rd32(guest_rd32(world) + 0xDC));
    uint32_t r = 0;
    g_esp = (g_esp - 0x400u) & ~15u;
    ray = g_esp; out = g_esp + 0x40;
    for (k = 0; k < 0x30; k++)
        guest_wr32(ray + k * 4, 0);
    for (k = 0; k < 3; k++) {
        guest_wr32(ray + 4 + k * 4, from_xyz[k]);
        guest_wr32(ray + 0x10 + k * 4, to_xyz[k]);
    }
    g_esp -= 0x40;
    if (f) {
        guest_push(0xF); guest_push(0xFFFFFFFFu); guest_push(mask); guest_push(0); guest_push(out); guest_push(ray);
        guest_push(0x0014E04Cu);
        g_ecx = world;
        f();
        r = g_eax & 0xFF;
        *hit = guest_rd32(out);
    }
    g_esp = esp0; g_ecx = ecx0; g_eax = eax0;
    return r;
}
static void ray_test_now(uint32_t player)
{
    int k;
    for (k = 0; k < 16; k++) {
        uint32_t c = s_npc_seen[k], a[3], b[3], hit1 = 0, hit2 = 0, r1, r2, own = 0, j;
        float v;
        if (!in_heap(c) || c == player) continue;
        for (j = 0; j < 3; j++) {
            v = guest_rdf(player + 0x4C + j * 4) + (j == 1 ? 1.2f : 0.0f); memcpy(&a[j], &v, 4);
            v = guest_rdf(c + 0x4C + j * 4) + (j == 1 ? 1.0f : 0.0f); memcpy(&b[j], &v, 4);
        }
        if (in_heap(guest_rd32(c + 0x20)))
            own = guest_rd32(c + 0x20) + 0x60;
        r1 = ray_once(a, b, 0x00408B0Fu, &hit1);
        r2 = ray_once(a, b, 0xFFFFFFFFu, &hit2);
        fprintf(stderr, "[TEST] ray to %08X (vt %08X, collider %08X +80 %08X): bullets' mask %u hit %08X; all %u hit %08X (vt %08X)\n",
                c, guest_rd32(c), own, own ? guest_rd32(own + 0x80) : 0, r1, hit1, r2, hit2, in_heap(hit2) ? guest_rd32(hit2) : 0);
    }
    fflush(stderr);
    s_ray_test = 0;
}
static unsigned long long s_pu_worn[4];
static int s_pu_log = -1;

static void guest_push(uint32_t v)
{
    g_esp -= 4;
    *(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + g_esp) = v;
}

static void powerup_take_off(uint32_t sub, uint32_t set)
{
    uint32_t ecx = g_ecx;
    guest_push(set);
    guest_push(0x0013903Au);
    g_ecx = guest_rd32(RDR_REWARD_MGR);
    sub_0015DFB0();                         /* ret 4 */
    g_ecx = ecx;
    guest_wr32(sub + 0x264, 0);
}

static uint32_t powerup_wear(uint32_t player, uint32_t sub, const char *names)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t esp = g_esp, ecx = g_ecx, eax = g_eax, buf, set;
    size_t n = strlen(names), i;
    g_esp = (g_esp - (uint32_t)n - 16u) & ~15u;
    buf = g_esp;
    for (i = 0; i <= n; i++)
        m[buf + i] = (uint8_t)names[i];
    guest_push(buf);
    guest_push(0x0013907Eu);
    g_ecx = guest_rd32(RDR_REWARD_MGR);
    sub_0015E470();                         /* ret 4 */
    set = g_eax;
    if (in_heap(set) && guest_rd32(set) == RDR_REWARD_SET_VT && guest_rd32(set + 4)) {
        guest_push(sub + 0x268);
        guest_push(player);
        guest_push(0);
        guest_push(0x00139091u);
        g_ecx = set;
        sub_0015E070();                     /* ret 0xC */
        guest_push(0);
        guest_push(0);
        guest_push(0x0013909Cu);
        g_ecx = set;
        sub_0015E040();                     /* ret 8 */
        guest_wr32(sub + 0x264, set);
    } else if (in_heap(set) && guest_rd32(set) == RDR_REWARD_SET_VT) {
        guest_push(set);                    /* none of the names known: give it back */
        guest_push(0x0013903Au);
        g_ecx = guest_rd32(RDR_REWARD_MGR);
        sub_0015DFB0();
        set = 0;
    } else {
        set = 0;
    }
    g_esp = esp;
    g_ecx = ecx;
    g_eax = eax;
    return set;
}

static void powerups_apply(uint32_t player)
{
    char names[1024];
    unsigned long long want = g_rdr_powerup_mask;
    uint32_t n = guest_rd32(RDR_PLAYER_COUNT), sub, cur;
    int k, i;
    if (s_pu_log < 0) {
        const char *e = getenv("RDR_POWERUP_LOG");
        s_pu_log = e && *e == '1';
        rdr_powerups_test_init();
        want = g_rdr_powerup_mask;
    }
    if (n > 4 || !guest_rd32(RDR_REWARD_MGR))
        return;
    for (k = 0; k < (int)n; k++)
        if (guest_rd32(0x00675930u + 4u * (uint32_t)k) == player)
            break;
    if (k >= (int)n)
        return;
    /* Only characters someone plays: their pad object is one of ours. */
    {
        uint32_t pad = guest_rd32(player + 0x894);
        if (pad < RDR_PADS || pad >= RDR_PADS + 4u * 0xB0u)
            return;
    }
    sub = guest_rd32(player + 0xC);
    if (!in_heap(sub))
        return;
    if (s_pu_player[k] != player) {         /* a new level, a new character */
        s_pu_player[k] = player;
        s_pu_set[k] = 0;
        s_pu_worn[k] = 0;
    }
    cur = guest_rd32(sub + 0x264);
    if (cur && cur == s_pu_set[k] && want != s_pu_worn[k]) {
        powerup_take_off(sub, cur);
        if (s_pu_log)
            fprintf(stderr, "[POWERUP] player %d: off %08X\n", k + 1, cur);
        cur = 0;
    }
    if (cur != s_pu_set[k])
        s_pu_set[k] = 0;                    /* it ran out, or the game swapped it */
    if (cur || !want)
        return;
    names[0] = 0;
    for (i = 0; i < g_rdr_powerup_count && i < 64; i++)
        if (want >> i & 1) {
            size_t l = strlen(names);
            if (l + strlen(g_rdr_powerups[i].code) + 2 >= sizeof(names))
                break;
            if (l)
                names[l++] = ',';
            strcpy(names + l, g_rdr_powerups[i].code);
        }
    s_pu_set[k] = powerup_wear(player, sub, names);
    s_pu_worn[k] = want;
    if (s_pu_log) {
        fprintf(stderr, "[POWERUP] player %d (%08X, %08X): %s -> %08X\n", k + 1, player, sub,
                names, s_pu_set[k]);
        fflush(stderr);
    }
}

/* Tests (RDR_ZONE_LOG): a level's zone (scenery) node loading its model --
 * zone vtable 0x003CEBF0 +0x1C (resource file or 0, flag), called per zone
 * by the level's resolve (sub_0007AF30). */
extern void sub_000801C0(void);
static void zone_load_logged(void)
{
    uint32_t zone = g_ecx, rsc = guest_rd32(g_esp + 4), lod = guest_rd32(zone + 0x5C);
    char name[48];
    unsigned i;
    for (i = 0; i < sizeof name - 1; i++)
        if (!(name[i] = lod ? (char)guest_rd8(guest_rd32(lod + 0x1C) + i) : 0))
            break;
    name[sizeof name - 1] = 0;
    sub_000801C0();
    fprintf(stderr, "[ZONE] %08X %s rsc %08X index %u -> lod %08X %08X %08X %08X\n", zone, name, rsc,
            guest_rd32(zone + 0xA0), guest_rd32(lod + 0x10), guest_rd32(lod + 0x14),
            guest_rd32(lod + 0x18), guest_rd32(lod + 0x1C));
    fflush(stderr);
}

extern void sub_000803F0(void);
/* RDR_ZONE_LOG: each zone's name the first time its update (vtable,
 * 0x000803F0) runs -- the last one named before a crash is the culprit */
static void zone_update_logged(void)
{
    static uint32_t seen[512];
    static unsigned nseen;
    uint32_t zone = g_ecx, lod = guest_rd32(zone + 0x5C);
    unsigned i;
    for (i = 0; i < nseen && seen[i] != zone; i++)
        ;
    if (i == nseen && nseen < 512) {
        char name[48];
        seen[nseen++] = zone;
        for (i = 0; i < sizeof name - 1; i++)
            if (!(name[i] = lod ? (char)guest_rd8(guest_rd32(lod + 0x1C) + i) : 0))
                break;
        name[sizeof name - 1] = 0;
        fprintf(stderr, "[ZONE] update %08X %s\n", zone, name);
        fflush(stderr);
    }
    sub_000803F0();
}

extern void sub_001BE240(void);
static void lod_set_logged(void)
{
    uint32_t lod = g_ecx, a1 = guest_rd32(g_esp + 4), a2 = guest_rd32(g_esp + 8),
             a3 = guest_rd32(g_esp + 12), ret = guest_rd32(g_esp);
    fprintf(stderr, "[ZONE] lod %08X set %08X %08X %08X (from %08X)\n", lod, a1, a2, a3, ret);
    fflush(stderr);
    sub_001BE240();
}

extern void sub_0014A0B0(void);
/* Story co-op helpers (player_update). */
static float coop_dist2(uint32_t p, uint32_t p1)
{
    float dx = guest_rdf(p + 0x4C) - guest_rdf(p1 + 0x4C);
    float dz = guest_rdf(p + 0x54) - guest_rdf(p1 + 0x54);
    return dx * dx + dz * dz;
}

/* A character's health: [char+0x18]+0x10 (what Showdown's respawn,
 * sub_0006D000, tests against 0). */
static float coop_health(uint32_t p)
{
    uint32_t h = guest_rd32(p + 0x18);
    return h ? guest_rdf(h + 0x10) : 1.0f;
}

/* Friendly fire (Cheats > Story co-op): with it off, a bullet one co-op
 * player fires at another does nothing (rdr_hooks.c, sub_00088050). */
extern int rdr_coop_friendly_fire(void);
extern int rdr_port_live(int port);
extern int rdr_port_ever(int port);
static int coop_index(uint32_t c)
{
    int k;
    for (k = 0; k < s_coop_n && k < 4; k++)
        if (c && s_coop_p[k] == c)
            return k;
    return -1;
}

/* Enemy health (settings: enemy_health, Cheats > World, Quick menu). A
 * story enemy takes its damage divided by the setting (200%: half), at the
 * character's TakeDamage (vtable +0x9C, sub_001394B0: armour, then the
 * health object's sub_00272090 takes it off, tells its listeners, and
 * death at zero). Health and maximum stay the game's own, so health bars,
 * the scripts' healthpercent and every reaction decided before the damage
 * (the bullet hit, sub_00088050: stagger, a dropped hostage) are as the
 * game made them -- only the health goes down slower. (Before: health and
 * maximum multiplied at the first bullet. Boss bars then sat still, and
 * Ugly Chris no longer stumbled when shot with the Sheriff on his
 * shoulder: user report.) Players, co-op players and Showdown untouched. */
extern int rdr_enemy_health_pct(void);
extern void sub_001394B0(void), sub_00091500(void);
/* Protect Friendly People (settings: protect_friendly, Cheats > World):
 * one of the level's people (0x003CE070) whose team does not fight the
 * players takes no damage -- the teams' table at 0x004065A8 (6x6 bytes,
 * a team's row against each team), the players' row 0: bit 0 for the
 * teams against them (the bar fight's brawlers, team 2: 7; its barmaids,
 * teams 0 and 4: 0 and 6; the shopkeeper, 3: 0). Checked at each damage,
 * so one who turns on the players can be hurt again. Story only; never a
 * player or a co-op player. */
extern int rdr_protect_friendly(void);
static int friendly_protected(uint32_t victim)
{
    uint32_t team, cnt;
    int k;
    if (!rdr_protect_friendly() || !in_heap(victim) || guest_rd32(0x00652E30u) != 0
        || guest_rd32(victim) != 0x003CE070u)
        return 0;
    team = guest_rd32(victim + 0xB4);
    if (team >= 6 || (guest_rd8(0x004065A8u + team) & 1))
        return 0;
    cnt = guest_rd32(0x0067592Cu);
    for (k = 0; k < 8 && (uint32_t)k < cnt; k++)
        if (guest_rd32(0x00675930u + (uint32_t)k * 4u) == victim)
            return 0;
    return coop_index(victim) < 0;
}

/* The damage on the guest stack ([esp+4], a float) cut to the setting's
 * share when the one taking it (ecx) is a story enemy. */
static void enemy_damage_scale(const char *via)
{
    uint32_t victim = g_ecx, cnt;
    int pct = rdr_enemy_health_pct(), k, enemy = 1;
    {
        static int lg = -1;
        if (lg < 0)
            lg = getenv("RDR_DMG_LOG") != NULL;
        if (lg && in_heap(victim)) {
            uint32_t h = guest_rd32(victim + 0x18);
            fprintf(stderr, "[DMG] %s %08X: %.1f (health %.1f, type %u, caller %08X)\n", via, victim,
                    guest_rdf(g_esp + 4), in_heap(h) ? guest_rdf(h + 0x10) : -1.0f, guest_rd32(g_esp + 8),
                    guest_rd32(g_esp));
            fflush(stderr);
        }
    }
    if (friendly_protected(victim)) {
        volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
        float z = 0.0f, d;
        static unsigned logged;
        memcpy(&d, (const void *)(uintptr_t)(m + g_esp + 4), 4);
        if (d > 0.0f && d < 1.0e7f) {
            memcpy((void *)(uintptr_t)(m + g_esp + 4), &z, 4);
            if (logged++ < 6) {
                fprintf(stderr, "[RDR] friendly %08X (team %u) protected from %.1f damage\n", victim,
                        guest_rd32(victim + 0xB4), d);
                fflush(stderr);
            }
        }
        return;
    }
    if (pct <= 100 || pct > 500 || !in_heap(victim) || guest_rd32(0x00652E30u) != 0)
        enemy = 0;
    if (enemy) {
        cnt = guest_rd32(0x0067592Cu);
        for (k = 0; k < 8 && (uint32_t)k < cnt; k++)
            if (guest_rd32(0x00675930u + (uint32_t)k * 4u) == victim)
                enemy = 0;                      /* a player */
        if (coop_index(victim) >= 0)
            enemy = 0;
    }
    if (enemy) {
        volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
        float d;
        memcpy(&d, (const void *)(uintptr_t)(m + g_esp + 4), 4);
        if (d > 0.0f && d < 1.0e7f) {
            float nd = d * 100.0f / (float)pct;
            static unsigned logged;
            memcpy((void *)(uintptr_t)(m + g_esp + 4), &nd, 4);
            if (logged++ < 6 || getenv("RDR_ENEMY_LOG")) {
                uint32_t h = guest_rd32(victim + 0x18);
                fprintf(stderr, "[RDR] enemy %08X takes %.1f of %.1f damage (health %.0f/%.0f, %s)\n", victim, nd,
                        d, in_heap(h) ? guest_rdf(h + 0x10) : -1.0f, in_heap(h) ? guest_rdf(h + 0x14) : -1.0f, via);
                fflush(stderr);
            }
        }
    }
}

/* The character classes' TakeDamage (vtable +0x9C), each damage scaled
 * once: the people the levels place (0x003CE070: sub_00076560, the
 * townsfolk and the gunmen; and 0x003D9C30: sub_001394B0) both call
 * sub_00091500 directly, which is itself +0x9C of 0x003CF7B0. */
extern void sub_00076560(void);
static void char_take_damage(void)
{
    enemy_damage_scale("TakeDamage");
    sub_001394B0();
}
static void char_take_damage3(void)
{
    enemy_damage_scale("TakeDamage");
    sub_00076560();
}
static void char_take_damage2(void)
{
    enemy_damage_scale("Damage");
    sub_00091500();
}

/* The talk partner in co-op (rdr_hooks.c, sub_0013DFE0). The level's
 * scripts talk with "the player" -- player 1 ([0x00652E38] and
 * [0x00652E4C]; their playertalkingtome is partner == me, sub_000F82E0) --
 * so the partner is player 1's look alone, as in a one-player game: another
 * player's look is undone (partner and its 1.5 s hold, [0x0065A9A4], as
 * they were before it). Before, whoever found someone kept them (114):
 * player 2 standing by the paper boy or a shopkeeper kept them the talk
 * partner after player 1 had finished -- the talk's camera stayed on the
 * paper boy, and the store's menu opened again and again until player 1
 * left the store (user videos, co-op only). */
void rdr_coop_talk_scan(uint32_t player, uint32_t before, uint32_t cool_before)
{
    uint32_t after = guest_rd32(0x0065A9A0u);
    if (s_coop_n < 2 || player == s_coop_p[0])
        return;
    if (after != before || guest_rd32(0x0065A9A4u) != cool_before) {
        guest_wr32(0x0065A9A0u, before);
        guest_wr32(0x0065A9A4u, cool_before);
        {
            static unsigned logged;
            if (after && (logged++ < 4 || getenv("RDR_COOP_LOG"))) {
                fprintf(stderr, "[COOP] talk: player %d's find %08X left to player 1 (partner stays %08X)\n",
                        coop_index(player) + 1, after, before);
                fflush(stderr);
            }
        }
    }
}

/* Showdown's cheats. The game keeps each Showdown player's cheats in its
 * own word ([[0x00652EE8 + n*0x114]]+0x44, sub_000580E0; the story's is
 * the profile's), and the menus set player 1's -- but nothing in a
 * Showdown match asks for Invincibility (user: "cheats such as infinite
 * health do not work on showdown"). The players' TakeDamage (vtable
 * 0x003D9FA8 +0x9C, sub_0013C330): in Showdown, a player whose word has
 * Invincibility (bit 6) takes no damage. */
extern void sub_0013C330(void);
/* Always Win Duels (Cheats): the opponent's shot misses. A bullet's hit
 * on a player in a duel (sub_00088050) decides the duel there and then
 * (sub_000961B0 -> sub_000951F0: the player's duel object [char+0x190]
 * to state 6, lost) before any damage, so the hit itself is skipped
 * (rdr_hooks.c, as co-op's "no friendly fire" does): the duel goes on
 * until the player's own shot lands. Found with watchpoints on the duel
 * object's state and the player's health in Chapter 23's first duel. */
int g_rdr_duel_win;
int rdr_duel_win_hit(uint32_t hit, uint32_t shot)
{
    uint32_t victim, cnt, o, shooter;
    int k;
    if (!g_rdr_duel_win || !in_heap(hit))
        return 0;
    /* a computer's shot: between players (Showdown's High Noon) the duel
     * stays fair */
    shooter = in_heap(shot) ? guest_rd32(shot + 0x1C) : 0;
    cnt = guest_rd32(0x0067592Cu);
    for (k = 0; k < 8 && (uint32_t)k < cnt; k++)
        if (shooter && guest_rd32(0x00675930u + 4u * (uint32_t)k) == shooter)
            return 0;
    victim = guest_rd32(hit + 8);
    o = in_heap(victim) ? guest_rd32(victim + 0x190) : 0;
    if (!in_heap(o))
        return 0;
    for (k = 0; k < 8 && (uint32_t)k < cnt; k++)
        if (guest_rd32(0x00675930u + 4u * (uint32_t)k) == victim) {
            static unsigned logged;
            if (logged++ < 4) {
                fprintf(stderr, "[DUEL] the opponent's shot at player %d misses (duel state %u)\n", k + 1,
                        guest_rd32(o));
                fflush(stderr);
            }
            return 1;
        }
    return 0;
}

static void player_take_damage(void)
{
    uint32_t victim = g_ecx, cnt = guest_rd32(0x0067592Cu);
    int k;
    if (guest_rd32(0x00652E30u) == 1 && !getenv("RDR_SD_CHEATS_OFF"))
        for (k = 0; k < 4 && (uint32_t)k < cnt; k++) {
            uint32_t w;
            if (guest_rd32(0x00675930u + 4u * (uint32_t)k) != victim)
                continue;
            w = guest_rd32(0x00652EE8u + (uint32_t)k * 0x114u);
            if (w >= 0x10000u && (w < 0x04000000u || in_heap(w)) && (guest_rd32(w + 0x44) & 0x40u)) {
                float z = 0.0f;
                memcpy((void *)((uintptr_t)xbox_GetMemoryOffset() + g_esp + 4), &z, 4);
            }
            break;
        }
    {
        /* (the log: a player hit while the game's "cannot be hurt" bit
         * is set -- [[player+0xC]+0x98] bit 4, set for cutscenes; user:
         * no harm taken in Chapter 8, cheats off) */
        uint32_t c = guest_rd32(victim + 0xC), h = guest_rd32(victim + 0x18);
        float d = guest_rdf(g_esp + 4), before = in_heap(h) ? guest_rdf(h + 0x10) : -1.0f;
        static unsigned logged;
        sub_0013C330();
        if (in_heap(c) && in_heap(h) && d > 0.0f && logged < 40) {
            float after = guest_rdf(h + 0x10);
            if (!(after < before)) {
                logged++;
                fprintf(stderr, "[HEALTH] player %08X hit for %.1f, no harm: health %.1f, flags %08X, cutscene %d\n",
                        victim, d, after, guest_rd32(c + 0x98), guest_rd8(RDR_CINEMA));
                fflush(stderr);
            }
        }
        return;
    }
}

int rdr_coop_friendly_hit(uint32_t hit, uint32_t shot)
{
    uint32_t victim, shooter;
    int a, b;
    if (s_coop_n < 2 || rdr_coop_friendly_fire() || getenv("RDR_COOP_FRIENDLY_FIRE"))
        return 0;
    if (!in_heap(hit) || !in_heap(shot))
        return 0;
    victim = guest_rd32(hit + 8);
    shooter = guest_rd32(shot + 0x1C);
    a = coop_index(victim);
    b = coop_index(shooter);
    if (a < 0 || b < 0 || a == b)
        return 0;
    {
        static unsigned logged;
        if (logged++ < 8 || getenv("RDR_COOP_LOG")) {
            fprintf(stderr, "[COOP] player %d's shot at player %d: no harm (friendly fire off)\n", b + 1, a + 1);
            fflush(stderr);
        }
    }
    return 1;
}

/* Where player 1 has stood (once its feet were on the ground and it was
 * alive): a revived or fallen player goes back to one of these, a few
 * metres behind player 1. Put 1.5 m to the side instead (always the same
 * way, whatever was there), a revived player could land outside the level
 * -- over a cliff or a wall -- and fall, the camera with it (user report). */
#define COOP_TRAIL 64
static float s_trail[COOP_TRAIL][3];
static int s_trail_n, s_trail_at;

static void coop_trail_note(uint32_t p1)
{
    float x = guest_rdf(p1 + 0x4C), y = guest_rdf(p1 + 0x50), z = guest_rdf(p1 + 0x54);
    float *l;
    if (s_trail_n) {
        l = s_trail[(s_trail_at + COOP_TRAIL - 1) % COOP_TRAIL];
        if ((x - l[0]) * (x - l[0]) + (z - l[2]) * (z - l[2]) < 0.75f * 0.75f && fabsf(y - l[1]) < 0.5f)
            return;
        /* a jump of more than 6 m (a script moved player 1): start again */
        if ((x - l[0]) * (x - l[0]) + (y - l[1]) * (y - l[1]) + (z - l[2]) * (z - l[2]) > 36.0f)
            s_trail_n = 0;
    }
    l = s_trail[s_trail_at];
    l[0] = x; l[1] = y; l[2] = z;
    s_trail_at = (s_trail_at + 1) % COOP_TRAIL;
    if (s_trail_n < COOP_TRAIL)
        s_trail_n++;
}

/* The spot for player k+1: the trail's newest point at least 1.2 m * k
 * from player 1 (each player its own), else player 1's own place. */
static void coop_spot(int k, uint32_t p1, float out[3])
{
    float x = guest_rdf(p1 + 0x4C), y = guest_rdf(p1 + 0x50), z = guest_rdf(p1 + 0x54);
    float want = 1.2f * (float)k;
    int i;
    out[0] = x; out[1] = y; out[2] = z;
    for (i = 1; i <= s_trail_n; i++) {
        float *l = s_trail[(s_trail_at + COOP_TRAIL - i) % COOP_TRAIL];
        float d2 = (l[0] - x) * (l[0] - x) + (l[2] - z) * (l[2] - z);
        if (fabsf(l[1] - y) > 3.0f)
            break;                      /* not on player 1's level any more */
        if (d2 >= want * want) {
            out[0] = l[0]; out[1] = l[1]; out[2] = l[2];
            if (d2 > (want + 2.0f) * (want + 2.0f))
                break;
            return;
        }
    }
}

/* Moved the way the game places a character at its start (sub_00137370):
 * the position at +0x5A0 handed to sub_001EB490, the heading (+0x5AC) to
 * sub_001EB4C0, then its physics told (sub_00219D60 on [[p+0xC]+0x14]).
 * Moving the position by hand left the collision behind (fell through). */
static void coop_place(uint32_t p, uint32_t p1, const float nw[3])
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, q;
    *(volatile float *)(m + p + 0x5A0) = nw[0];
    *(volatile float *)(m + p + 0x5A4) = nw[1];
    *(volatile float *)(m + p + 0x5A8) = nw[2];
    guest_push(p + 0x5A0); guest_push(0x00137430u);
    g_ecx = p; sub_001EB490(); g_esp = esp0;
    guest_push(guest_rd32(p1 + 0x5AC)); guest_push(0x00137444u);
    g_ecx = p; sub_001EB4C0(); g_esp = esp0;
    *(volatile uint32_t *)(m + p + 0x5AC) = guest_rd32(p1 + 0x5AC);
    q = guest_rd32(p + 0xC);
    if (q && guest_rd32(q + 0x14)) {
        guest_push(0x00137463u);
        g_ecx = guest_rd32(q + 0x14); sub_00219D60(); g_esp = esp0;
    }
    g_ecx = ecx0; g_eax = eax0;
}

/* Test (overlay flag "tp <player 1-4> <x> <y> <z>"): that player there. */
static volatile int s_tp_player;
static float s_tp_at[3];
void rdr_test_tp(int k, float x, float y, float z)
{
    s_tp_at[0] = x; s_tp_at[1] = y; s_tp_at[2] = z;
    s_tp_player = k;
}

static void coop_put_beside(int k, uint32_t p, uint32_t p1, const char *why)
{
    float nw[3];
    coop_spot(k, p1, nw);
    coop_place(p, p1, nw);
    fprintf(stderr, "[COOP] player %d %s beside player 1 (%.2f %.2f %.2f), now %.2f %.2f %.2f\n",
            k + 1, why, nw[0], nw[1], nw[2], guest_rdf(p + 0x4C), guest_rdf(p + 0x50), guest_rdf(p + 0x54));
    fflush(stderr);
}

/* Showdown's respawn (sub_0006D000, after its timer and the trigger):
 * sub_00272010 on [char+0x18] (health back), the character's +4 (reset),
 * its body's [[char+0xC]]+8, its +0x94 object's +0x4C8 sub_0005AFC0(1.0),
 * then if +0x28 says so, +0x184. */
extern recomp_func_t recomp_lookup(uint32_t xbox_va);

static uint32_t coop_vcall(uint32_t obj, uint32_t slot, int nargs, const uint32_t *a, uint32_t ret)
{
    uint32_t esp0 = g_esp, ecx0 = g_ecx, r, fn = guest_rd32(guest_rd32(obj) + slot);
    recomp_func_t f = recomp_lookup(fn);
    int i;
    if (!f)
        return 0;
    for (i = nargs - 1; i >= 0; i--)
        guest_push(a[i]);
    guest_push(ret);
    g_ecx = obj;
    f();
    r = g_eax;
    g_esp = esp0; g_ecx = ecx0;
    return r;
}

static void coop_revive(uint32_t p)
{
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, o, a[1];
    guest_push(0x0006D0B8u);
    g_ecx = guest_rd32(p + 0x18); sub_00272010(); g_esp = esp0;
    coop_vcall(p, 4, 0, NULL, 0x0006D0BFu);
    if (guest_rd32(p + 0xC))
        coop_vcall(guest_rd32(p + 0xC), 8, 0, NULL, 0x0006D0C9u);
    o = coop_vcall(p, 0x94, 0, NULL, 0x0006D0D3u);
    if (o) {
        guest_push(0x3F800000u); guest_push(0x0006D0F1u);
        g_ecx = o + 0x4C8; sub_0005AFC0(); g_esp = esp0;
    }
    if (coop_vcall(p, 0x28, 0, NULL, 0x0006D0F8u) & 0xFF)
        coop_vcall(p, 0x184, 0, NULL, 0x0006D10Au);
    (void)a;
    g_ecx = ecx0; g_eax = eax0;
    fprintf(stderr, "[COOP] revived %08X, health %.2f\n", p, coop_health(p));
}

/* Tests (RDR_OVERLAY_FLAG "coopkill <k>"): co-op player k's health to 0. */
/* Test: the duel object of each player ([player + 0x190], duel_state is
 * its first word), 0x400 bytes as words and floats (overlay flag "duelrep") */
void rdr_duel_report(void)
{
    int k;
    for (k = 0; k < 4; k++) {
        uint32_t p = guest_rd32(0x00675930u + k * 4u), o, i;
        if (p < 0x10000u || p >= 0x84000000u)
            continue;
        o = guest_rd32(p + 0x190);
        fprintf(stderr, "[DUEL] player %d %08X duel %08X\n", k, p, o);
        if (o < 0x10000u || o >= 0x84000000u)
            continue;
        for (i = 0; i < 0x400; i += 16) {
            uint32_t w0 = guest_rd32(o + i), w1 = guest_rd32(o + i + 4),
                     w2 = guest_rd32(o + i + 8), w3 = guest_rd32(o + i + 12);
            float f[4];
            memcpy(&f[0], &w0, 4); memcpy(&f[1], &w1, 4); memcpy(&f[2], &w2, 4); memcpy(&f[3], &w3, 4);
            fprintf(stderr, "[DUEL] +%03X %08X %08X %08X %08X | %g %g %g %g\n", i, w0, w1, w2, w3,
                    f[0], f[1], f[2], f[3]);
        }
    }
    fflush(stderr);
}

/* Test: guest memory to a file (overlay flag "memdump <va hex> <len hex> <path>");
 * va "p0" / "p1": that player's character */
void rdr_mem_dump(const char *what, uint32_t len, const char *path)
{
    uint32_t va;
    FILE *f;
    if (what[0] == 'p' && what[1] >= '0' && what[1] <= '3')
        va = guest_rd32(0x00675930u + (uint32_t)(what[1] - '0') * 4u);
    else
        va = (uint32_t)strtoul(what, NULL, 16);
    if (va < 0x10000u || len == 0 || len > 0x1000000u)
        return;
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite((const void *)((uintptr_t)xbox_GetMemoryOffset() + va), 1, len, f);
    fclose(f);
    fprintf(stderr, "[TEST] memdump %08X +%X -> %s\n", va, len, path);
    fflush(stderr);
}

void rdr_coop_test_kill(int k)
{
    uint32_t h;
    if (k < 0 || k >= s_coop_n || k >= 4 || !s_coop_p[k] || !(h = guest_rd32(s_coop_p[k] + 0x18)))
        return;
    *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + h + 0x10) = 0.0f;
    fprintf(stderr, "[TEST] co-op player %d health 0\n", k + 1);
    fflush(stderr);
}

/* ── A duel's aim by mouse ─────────────────────────────────────────
 *
 * The duel's aiming cursor (sub_00146530, run from the character's update)
 * is a point on the screen, [character + 0x67C] across and + 0x680 down,
 * moved by a velocity (+0x87C, +0x880) that the right stick pushes at the
 * level's CursorSpeed (a .duel file: 0.6 for the player) times the frame's
 * time -- slow motion's time -- and friction (CursorFriction) slows. Fed by
 * a stick, the mouse drove that velocity: slow (a full throw crosses a
 * third of the screen in about two seconds of slow motion) and lagging,
 * with the cursor drifting on after the mouse stopped. Here the mouse moves
 * the point itself, before the character's update runs, and stops the
 * velocity it moved: 1600 counts across half the screen at 100 % (Mouse
 * sensitivity x Duel aim sensitivity). In the draw and the aim (the duel
 * object's states 3 and 4); in the grab (1, 2) the mouse is still the
 * right stick, for the draw's down-and-up.
 */
extern int rdr_input_take_duel(long *dx, long *dy);
extern void rdr_input_duel_aim_seen(void);
extern int rdr_duel_mouse_scale(void);      /* rdr_settings.c: % */

/* The draw (state 3) too: it runs in the slow motion, the cursor is up
 * from its start and the player aims already -- through the stick, the
 * mouse was the drifting, lagging cursor of before there ("floaty" until
 * the slow motion ended). RDR_DUEL_AIM3=0: the stick there, as before. */
static int s_duel_aim3 = 1;

static void duel_mouse_aim(uint32_t c)
{
    uint32_t idx = guest_rd32(0x00652024u), p, o;
    long dx, dy;
    int sens, ix, iy;
    if (idx > 3)
        idx = 0;
    p = guest_rd32(0x00675930u + idx * 4u);
    if (c != p || p < 0x80000000u || p >= 0x84000000u)
        return;
    o = guest_rd32(p + 0x190);
    if (o < 0x80000000u || o >= 0x84000000u)
        return;
    {
        static int tr = -1;
        static uint32_t last_st = 0xFFFFFFFFu;
        if (tr < 0) tr = getenv("RDR_DUEL_TRACE") != NULL;
        if (tr && guest_rd32(o) != last_st) {
            last_st = guest_rd32(o);
            fprintf(stderr, "[DUEL] state %u cursor %.3f %.3f dt %.4f frame %.4f\n", last_st,
                    *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + p + 0x67C),
                    *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + p + 0x680),
                    *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + 0x0040652Cu),
                    *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + 0x00406504u));
        }
    }
    {
        static int once;
        if (!once) {
            const char *e = getenv("RDR_DUEL_AIM3");
            once = 1;
            if (e && *e == '0')
                s_duel_aim3 = 0;
        }
    }
    if (guest_rd32(o) != 4 && (guest_rd32(o) != 3 || !s_duel_aim3)) {
        static int tr3 = -1;
        static double lt3;
        if (tr3 < 0) tr3 = getenv("RDR_DUEL_TRACE") != NULL;
        if (tr3 && (guest_rd32(o) == 2 || guest_rd32(o) == 3)) {
            volatile uint8_t *mm = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
            double nt = host_seconds();
            fprintf(stderr, "[DUEL] s%u cursor %.3f %.3f vel %.3f %.3f dt %.4f real %.4f\n", guest_rd32(o),
                    *(volatile float *)(mm + p + 0x67C), *(volatile float *)(mm + p + 0x680),
                    *(volatile float *)(mm + p + 0x87C), *(volatile float *)(mm + p + 0x880),
                    *(volatile float *)(mm + 0x0040652Cu), nt - lt3);
            lt3 = nt;
        }
        return;
    }
    if (!rdr_mouse_settings(&sens, &ix, &iy))
        return;
    rdr_input_duel_aim_seen();
    {
        static int dbg2 = -1;
        static unsigned n2;
        if (dbg2 < 0) dbg2 = getenv("RDR_DUEL_TRACE") != NULL;
        if (dbg2 && (n2++ % 2) == 0) {
            volatile uint8_t *mm = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
            static double lt;
            double nt = host_seconds();
            fprintf(stderr, "[DUEL] s%u cursor %.3f %.3f vel %.3f %.3f dt %.4f frame %.4f real %.4f\n", guest_rd32(o),
                    *(volatile float *)(mm + p + 0x67C),
                    *(volatile float *)(mm + p + 0x680), *(volatile float *)(mm + p + 0x87C),
                    *(volatile float *)(mm + p + 0x880), *(volatile float *)(mm + 0x0040652Cu),
                    *(volatile float *)(mm + 0x00406504u), (nt - lt) / 2);
            lt = nt;
        }
    }
    if (!rdr_input_take_duel(&dx, &dy))
        return;
    {
        /* Test: RDR_TEST_DUEL_MOUSE="dx,dy" moves the mouse that much on
         * the 3rd update of each aim */
        static const char *t = (const char *)1;
        static uint32_t last_o, calls;
        if (t == (const char *)1)
            t = getenv("RDR_TEST_DUEL_MOUSE");
        if (o != last_o) { last_o = o; calls = 0; }
        if (t && ++calls == 12) {
            long a = 0, b = 0;
            if (sscanf(t, "%ld,%ld", &a, &b) == 2) { dx += a; dy += b; }
        }
        /* RDR_DUEL_DIFF=1 (tests): the character's floats that change in
         * the updates around the test move -- what follows the cursor */
        {
            static int df = -1;
            static float snap[0x500];
            if (df < 0) df = getenv("RDR_DUEL_DIFF") != NULL;
            if (df && t && calls >= 10 && calls <= 30) {
                volatile float *cf = (volatile float *)((uintptr_t)xbox_GetMemoryOffset() + p);
                int i, n = 0;
                char line[2048];
                int len = snprintf(line, sizeof line, "[DIFF] %u fr %.4f:", calls,
                                   *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + 0x00406504u));
                for (i = 0; i < 0x500; i++) {
                    float v = cf[i];
                    if (calls > 10 && v != snap[i] && v == v && v > -50.0f && v < 50.0f && n < 40 && len < 1900) {
                        len += snprintf(line + len, sizeof line - len, " %X=%.3f", i * 4, v);
                        n++;
                    }
                    snap[i] = v;
                }
                fprintf(stderr, "%s\n", line);
            }
        }
    }
    if (!dx && !dy)
        return;
    {
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("RDR_INPUT_DEBUG") != NULL;
        if (dbg) {
            fprintf(stderr, "[INPUT] duel aim: mouse %ld %ld, cursor %.3f %.3f\n", dx, dy,
                    *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + p + 0x67C),
                    *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + p + 0x680));
            fflush(stderr);
        }
    }
    {
        volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
        volatile float *x = (volatile float *)(m + p + 0x67C), *y = (volatile float *)(m + p + 0x680);
        float k = (float)sens / 100.0f * (float)rdr_duel_mouse_scale() / 100.0f / 1600.0f;
        float nx = *x + (float)(ix ? -dx : dx) * k, ny = *y + (float)(iy ? dy : -dy) * k;   /* y is up */
        if (nx > 1.2f) nx = 1.2f;
        if (nx < -1.2f) nx = -1.2f;
        if (ny > 1.2f) ny = 1.2f;
        if (ny < -1.2f) ny = -1.2f;
        *x = nx;
        *y = ny;
        *(volatile float *)(m + p + 0x87C) = 0.0f;
        *(volatile float *)(m + p + 0x880) = 0.0f;
    }
}

/* Fliers -- a character whose .swt has bit 0 of SpecialMovesets (the
 * condor's flight move set; tools/mods/beta_roster.py gives it to the 2002
 * preview's Bat Boy). The game's own way into the air is the melee button
 * with the gun put away (B, or Right Trigger: sub_00143660 starts the
 * MeleeSpecialMove, his spmove_glideattack, only while the controller's
 * +0xA8 is 0 -- no gun drawn). The swat_xbx.strtbl hint says "Press the
 * BLACK button with your gun drawn to activate your special move": nothing
 * in the retail game does that for a person's character, so Black does it
 * here, the same steps as the game's glide case (0x00143A13): on the
 * ground, CanDoSpecial (+0x198), pay for it from the meter (+0x19C) and
 * take off (sub_0025A980 on the flight state, [[char+0xC]+0x90]+0x28); in
 * the air, dive at the target (+0xF0's +0x28, sub_0025AC90). */
extern void sub_0025A980(void), sub_0025AC90(void);
static int special_index(const char *name)
{
    int i, k;
    for (i = 1; i < 0x28; i++) {
        uint32_t a = guest_rd32(0x004045CCu + (uint32_t)i * 8u);
        for (k = 0; k < 40; k++) {
            char ch = (char)guest_rd8(a + (uint32_t)k);
            if (ch != name[k])
                break;
            if (!ch)
                return i;
        }
    }
    return 0;
}
/* A dive needs someone to dive at (sub_0025AC90 keeps the target and
 * reads its position as it goes). A person seldom has one locked when he
 * presses the button in the air, so: the character the game would aim at
 * (+0xF0's +0x28), else the nearest living one -- the other players
 * (Showdown's opponents among them) and the people the level updated last
 * -- within 60 m, in front of him rather than behind. */
static uint32_t flier_target(uint32_t c)
{
    uint32_t o = coop_vcall(c, 0xF0, 0, NULL, 0x00143A33u), t = o ? guest_rd32(o + 0x28) : 0, best = 0;
    float cx = guest_rdf(c + 0x4C), cy = guest_rdf(c + 0x50), cz = guest_rdf(c + 0x54), bd = 3600.0f;
    /* facing: minus the third row of the rotation at +0x28 (characters
     * face -z; checked against a walk forward) */
    float fx = -guest_rdf(c + 0x40), fz = -guest_rdf(c + 0x48);
    uint32_t cand[24], cnt = guest_rd32(0x0067592Cu);
    int n = 0, k;
    if (t && in_heap(t))
        return t;
    for (k = 0; k < 8 && (uint32_t)k < cnt; k++)
        cand[n++] = guest_rd32(0x00675930u + (uint32_t)k * 4u);
    for (k = 0; k < 16; k++)
        cand[n++] = s_npc_seen[k];
    for (k = 0; k < n; k++) {
        uint32_t e = cand[k], h;
        float dx, dy, dz, d;
        if (e == c || !in_heap(e) || !in_heap(h = guest_rd32(e + 0x18)) || !(guest_rdf(h + 0x10) > 0.0f))
            continue;
        dx = guest_rdf(e + 0x4C) - cx; dy = guest_rdf(e + 0x50) - cy; dz = guest_rdf(e + 0x54) - cz;
        d = dx * dx + dy * dy + dz * dz;
        if (dx * fx + dz * fz < 0.0f)
            d *= 4.0f;                                              /* behind him: only if nothing ahead */
        if (d < bd) {
            bd = d;
            best = e;
        }
    }
    return best;
}

static void flier_black(uint32_t c)
{
    static uint8_t was[4], was_b[4], dive_wanted[4];
    uint32_t b = guest_rd32(c + 0xC), w, f, pad, port, ms, down, a[1];
    char nm[40];
    int i, idx;
    if (!b || !(w = guest_rd32(b + 0x90)) || guest_rd8(w + 0x7C))
        return;
    if (!guest_rd32(w + 4) || !(guest_rd8(guest_rd32(w + 4) + 0x1DC) & 1) || !(f = guest_rd32(w + 0x28)))
        return;
    if (!(pad = guest_rd32(c + 0x894)) || (port = guest_rd32(pad + 4)) > 3)
        return;
    {
        /* the melee button in the air dives too (the game's own way in,
         * glideattack, finds no target unless one is locked) */
        uint32_t b_down = guest_rd32(c + 0x608) == 2;
        if (b_down && !was_b[port] && guest_rd32(f + 0x19C) != 0)
            dive_wanted[port] = 1;
        was_b[port] = (uint8_t)b_down;
    }
    if (dive_wanted[port]) {
        /* pressed while still climbing: dives once it glides */
        uint32_t st = guest_rd32(f + 0x19C);
        if (st == 0 || st > 3)
            dive_wanted[port] = 0;
        else if (st == 3) {
            uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, t = flier_target(c);
            dive_wanted[port] = 0;
            if (t) {
                guest_push(t); guest_push(0x00143A51u);
                g_ecx = f; sub_0025AC90();
                if (getenv("RDR_SPECIAL_DUMP"))
                    fprintf(stderr, "[SPECIAL] %08X dives at %08X (%s)\n", c, t,
                            (g_eax & 0xFF) ? "diving" : "refused");
            }
            g_esp = esp0; g_ecx = ecx0; g_eax = eax0;
        }
    }
    down = guest_rd32(0x004041C4u + port * 0x38u) & 0x2u;      /* Black */
    if (!down || was[port]) {
        was[port] = (uint8_t)(down != 0);
        return;
    }
    was[port] = 1;
    if (guest_rd32(c + 0x620) != 0xFFFFFFFFu)                   /* a special under way */
        return;
    ms = guest_rd32(c + 0x20);
    for (i = 0; i < 39 && (nm[i] = (char)guest_rd8(ms + 0x2B4 + (uint32_t)i)); i++)
        ;
    nm[i] = 0;
    idx = special_index(nm);                                    /* NormalSpecialMove */
    if (idx != 7 && idx != 8) {
        for (i = 0; i < 39 && (nm[i] = (char)guest_rd8(ms + 0x274 + (uint32_t)i)); i++)
            ;
        nm[i] = 0;
        idx = special_index(nm);                                /* MeleeSpecialMove */
    }
    if (idx != 7 && idx != 8)
        idx = 7;
    if (guest_rd32(f + 0x19C) == 0) {
        uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax;
        a[0] = (uint32_t)idx;
        if (!(coop_vcall(c, 0x198, 1, a, 0x0014393Fu) & 0xFF))
            return;
        coop_vcall(c, 0x19C, 1, a, 0x00143A5Au);
        guest_push(0); guest_push(0x00143A65u);
        g_ecx = f; sub_0025A980();
        g_esp = esp0; g_ecx = ecx0; g_eax = eax0;
        if (getenv("RDR_SPECIAL_DUMP"))
            fprintf(stderr, "[SPECIAL] Black: %08X takes off (%s)\n", c, nm);
    } else {
        dive_wanted[port] = 1;                                  /* at the next update */
    }
}

/* The camera on a flier (user: it does not keep up with Bat Boy). The
 * game's follow camera rises with him but keeps looking where it looked:
 * on the launch (11 m in about a second) he went off the top of the
 * picture and came back only once he glided. Each time the camera is set
 * (sub_00180870, rdr_hooks.c), if a flier is in the air (flight state 1-6)
 * within 20 m of the camera, the camera is turned up (or down) just enough
 * to keep his middle within about a sixth of the picture's height of its
 * centre: the game's camera as it is otherwise, nothing carried from one
 * frame to the next. World matrix rows: right, up, back, position (the
 * camera looks along -back); the view matrix is its inverse. */
void rdr_flier_camera(void)
{
    volatile float *w = (volatile float *)((uintptr_t)xbox_GetMemoryOffset() + 0x00678828u);
    volatile float *v = (volatile float *)((uintptr_t)xbox_GetMemoryOffset() + 0x00678720u);
    uint32_t cnt = guest_rd32(0x0067592Cu), best = 0;
    float bd = 400.0f, r[3], u[3], b[3], f[3], pos[3], d[3], x, y, z, t, tmax = 0.30f, a, ca, sa;
    int k, i;
    if (cnt > 8)
        return;
    for (i = 0; i < 3; i++) {
        r[i] = w[i]; u[i] = w[4 + i]; b[i] = w[8 + i]; pos[i] = w[12 + i];
    }
    for (k = 0; k < (int)cnt; k++) {
        uint32_t c = guest_rd32(0x00675930u + (uint32_t)k * 4u), o, fw, fs, st;
        float dx, dz, dd;
        if (!in_heap(c) || !(o = guest_rd32(c + 0xC)) || !in_heap(o) || !(fw = guest_rd32(o + 0x90))
            || !in_heap(fw) || guest_rd8(fw + 0x7C))
            continue;
        if (!in_heap(guest_rd32(fw + 4)) || !(guest_rd8(guest_rd32(fw + 4) + 0x1DC) & 1)
            || !in_heap(fs = guest_rd32(fw + 0x28)))
            continue;
        st = guest_rd32(fs + 0x19C);
        if (st < 1 || st > 6)
            continue;
        dx = guest_rdf(c + 0x4C) - pos[0];
        dz = guest_rdf(c + 0x54) - pos[2];
        dd = dx * dx + dz * dz;
        if (dd < bd) {
            bd = dd;
            best = c;
        }
    }
    if (!best)
        return;
    d[0] = guest_rdf(best + 0x4C) - pos[0];
    d[1] = guest_rdf(best + 0x50) + 0.9f - pos[1];
    d[2] = guest_rdf(best + 0x54) - pos[2];
    x = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
    y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    z = -(d[0] * b[0] + d[1] * b[1] + d[2] * b[2]);
    (void)x;
    if (z < 0.5f)
        return;
    t = y / z;
    if (t > tmax)
        a = atanf(t) - atanf(tmax);
    else if (t < -tmax)
        a = atanf(t) + atanf(tmax);
    else
        return;
    ca = cosf(a);
    sa = sinf(a);
    for (i = 0; i < 3; i++) {
        float fi = -b[i], ui = u[i];
        f[i] = fi * ca + ui * sa;
        u[i] = ui * ca - fi * sa;
        b[i] = -f[i];
    }
    for (i = 0; i < 3; i++) {
        w[4 + i] = u[i];
        w[8 + i] = b[i];
        v[i * 4 + 0] = r[i];
        v[i * 4 + 1] = u[i];
        v[i * 4 + 2] = b[i];
    }
    v[12] = -(pos[0] * r[0] + pos[1] * r[1] + pos[2] * r[2]);
    v[13] = -(pos[0] * u[0] + pos[1] * u[1] + pos[2] * u[2]);
    v[14] = -(pos[0] * b[0] + pos[1] * b[1] + pos[2] * b[2]);
    {
        static unsigned n;
        if (getenv("RDR_SPECIAL_DUMP") && (n++ % 10) == 0)
            fprintf(stderr, "[SPECIAL] flier camera: %08X at %.2f of the height, turned %.1f deg\n",
                    best, t, a * 57.29578f);
    }
}

/* ── The spawner: the level's own characters, brought in now ────────────
 *
 * A level's people are all made when it loads (the .enm file: enmManager
 * [0x0065A998]), each an instance of vtable 0x003CE070, linked from the
 * manager's +0x24 through +0x1B8. One the story has not brought in yet has
 * no controller ([inst+0xC] = 0) and waits at its spawn point. The
 * manager's update brings one in (sub_00074D10 -> sub_00072CA0, thiscall
 * on the manager with the instance: a spawn point picked and marked, the
 * instance's Spawn (vtable +0x10, sub_00076FD0: its controller and brain
 * made), registered with the AI) when its AI table says it is wanted in
 * (byte [[inst+0x158]+0x40], sub_00076900 -- the level's script sets it),
 * and takes it out again when not: so the spawner sets it too. Found by watching a waiting instance's
 * +0xC in the bar fight (gdb, docs/dev/BRINGUP.md 110). The spawner calls
 * the same for a waiting instance and puts it in front of the player who
 * asked, the way co-op places a player (coop_put_beside). Per instance:
 * its type [[inst+0x20]+0x18] ("ncb"), its .enm entry [inst+0x194]
 * (Name at +0x9C), its team [inst+0xB4] (0 the players' side).
 * Requests come from the menus' thread and run here, on the game's. */
#define RDR_ENM_MGR 0x0065A998u
#define RDR_ENEMY_VT 0x003CE070u
#define RDR_ANIMAL_VT 0x003CE398u       /* the levels' animals (horses, cattle, buffalo), the same manager */
#define RDR_ANIMAL3_VT 0x003CDDA0u      /* the other animal class (a type's kind 3, sub_000749F0) */
static int is_animal_vt(uint32_t v) { return v == RDR_ANIMAL_VT || v == RDR_ANIMAL3_VT; }
static int is_npc_vt(uint32_t v) { return v == RDR_ENEMY_VT || is_animal_vt(v); }
extern void sub_00072CA0(void);
extern void sub_0009B400(void);
static volatile int s_spawn_req, s_spawn_player;
static char s_spawn_type[16];
int rdr_spawn_list(char types[][16], int *waiting, int *team, int max);

/* ── Players' commands (the spawner, Give Weapon) ──────────────────────────
 *
 * One u32: the command (low byte), a count or small argument (next byte),
 * an index (high half). Played alone or in local co-op it runs on the
 * game's thread at its next pad read; online it rides with this player's
 * input (rdr_lockstep.c) and every machine runs it at the same frame, so
 * the games stay the same. */
enum { RDR_CMD_SPAWN = 1, RDR_CMD_WEAPON = 2, RDR_CMD_BUY = 3, RDR_CMD_AMMO = 4, RDR_CMD_MAKE = 5,
       RDR_CMD_NPCOPT = 6, RDR_CMD_NPCDEL = 7, RDR_CMD_NPCOTHER = 8 };
static volatile uint32_t s_cmd_local[8];
static volatile int s_cmd_local_player[8], s_cmd_local_n;
extern int rdr_ls_send_cmd(uint32_t cmd);
static void spawn_run(void);
void rdr_give_weapon_run(int player, int weapon);

void rdr_cmd_run(int player, uint32_t cmd)
{
    switch (cmd & 0xFF) {
    case RDR_CMD_SPAWN: {
        char types[64][16];
        int w[64], t[64], n = rdr_spawn_list(types, w, t, 64), idx = (int)(cmd >> 16) - 1;
        snprintf(s_spawn_type, sizeof s_spawn_type, "%s", idx >= 0 && idx < n ? types[idx] : "");
        s_spawn_player = player;
        s_spawn_req = (int)((cmd >> 8) & 0xFF);
        if (idx >= n) {
            s_spawn_req = 0;
            return;
        }
        spawn_run();
        break;
    }
    case RDR_CMD_BUY: {
        extern void rdr_coop_buy_run(int player, int weapon);
        rdr_coop_buy_run(player, (int)(cmd >> 16));
        break;
    }
    case RDR_CMD_WEAPON:
        rdr_give_weapon_run(player, (int)(cmd >> 16));
        break;
    case RDR_CMD_MAKE: {
        extern void rdr_npc_make_run(int player, int who, int act, int side);
        rdr_npc_make_run(player, (int)(cmd >> 16), (int)((cmd >> 10) & 0x3F), (int)((cmd >> 8) & 3));
        break;
    }
    case RDR_CMD_AMMO: {
        extern void rdr_fill_ammo_run(int player);
        rdr_fill_ammo_run(player);
        break;
    }
    case RDR_CMD_NPCOPT: {
        extern void rdr_npc_opts_run(int count, int place, int arms);
        rdr_npc_opts_run((int)((cmd >> 8) & 0xFF), (int)((cmd >> 16) & 0xFF), (int)((cmd >> 24) & 0xFF));
        break;
    }
    case RDR_CMD_NPCDEL: {
        extern void rdr_npc_remove_run(void);
        rdr_npc_remove_run();
        break;
    }
    case RDR_CMD_NPCOTHER: {
        extern void rdr_npc_other_run(int idx);
        rdr_npc_other_run((int)(cmd >> 16) - 1);
        break;
    }
    }
}

static void cmd_send(uint32_t cmd, int player)
{
    if (rdr_ls_send_cmd(cmd))
        return;                         /* online: with the next input */
    if (s_cmd_local_n < 8) {
        s_cmd_local[s_cmd_local_n] = cmd;
        s_cmd_local_player[s_cmd_local_n] = player;
        s_cmd_local_n++;
    }
}

/* (the game's thread, at a pad read) */
static void cmd_local_run(void)
{
    while (s_cmd_local_n > 0) {
        uint32_t c = s_cmd_local[0];
        int pl = s_cmd_local_player[0], k;
        for (k = 1; k < s_cmd_local_n; k++) {
            s_cmd_local[k - 1] = s_cmd_local[k];
            s_cmd_local_player[k - 1] = s_cmd_local_player[k];
        }
        s_cmd_local_n--;
        rdr_cmd_run(pl, c);
    }
}

/* The menu: count of type_index (rdr_spawn_list's order; -1 any) in front
 * of player (0-3). */
void rdr_spawn_ask(int type_index, int count, int player)
{
    if (count < 1) count = 1;
    if (count > 64) count = 64;
    cmd_send(RDR_CMD_SPAWN | (uint32_t)count << 8 | (uint32_t)(type_index + 1) << 16, player);
}

void rdr_coop_buy_ask(int weapon, int player)
{
    cmd_send(RDR_CMD_BUY | (uint32_t)weapon << 16, player);
}

void rdr_give_weapon_ask(int weapon, int player)
{
    cmd_send(RDR_CMD_WEAPON | (uint32_t)weapon << 16, player);
}

void rdr_fill_ammo_ask(int player)
{
    cmd_send(RDR_CMD_AMMO, player);
}

/* The new people's options -- how many, where, their guns -- go as a
 * command too, so every machine online makes the same ones. */
void rdr_npc_opts_ask(int count, int place, int arms, int player)
{
    /* arms: 0..2, and 0x10 for "a health bar on screen" (a boss) */
    if (count < 1 || count > 16 || place < 0 || place > 2 || (arms & 0x0F) > 2 || (arms & ~0x1F))
        return;
    cmd_send(RDR_CMD_NPCOPT | (uint32_t)count << 8 | (uint32_t)place << 16 | (uint32_t)arms << 24, player);
}

/* "Acts like" someone from another level (the index into the list below;
 * -1: one of this level's own): a command before the make, as the
 * options are. */
void rdr_npc_other_ask(int idx, int player)
{
    if (idx < -1 || idx > 0xFFF0)
        return;
    cmd_send(RDR_CMD_NPCOTHER | (uint32_t)(idx + 1) << 16, player);
}

void rdr_npc_remove_ask(int player)
{
    cmd_send(RDR_CMD_NPCDEL, player);
}

void rdr_npc_ask(int who, int act, int side, int player)
{
    if (who < 0 || who > 0xFFFF || act < 0 || act > 0x3F || side < 0 || side > 2)
        return;
    cmd_send(RDR_CMD_MAKE | (uint32_t)side << 8 | (uint32_t)act << 10 | (uint32_t)who << 16, player);
}

/* tests: by type code */
void rdr_spawn_request(const char *type, int count, int player)
{
    char types[64][16];
    int w[64], t[64], n = rdr_spawn_list(types, w, t, 64), k, idx = -1;
    for (k = 0; k < n && type && strcmp(type, "*"); k++)
        if (!strcmp(types[k], type))
            idx = k;
    rdr_spawn_ask(idx, count, player);
}

static void spawn_type(uint32_t inst, char *out, size_t n)
{
    uint32_t t = guest_rd32(inst + 0x20);
    out[0] = 0;
    if (in_heap(t))
        guest_str(t + 0x18, out, (unsigned)n);
}

/* One the spawner offers: a character of the level's, not a cutscene's
 * (AI table "ACTOR", name at [inst+0x158]+0x42: brought in, those only
 * stood there -- Dan in the bar fight, harness). */
static int spawn_usable(uint32_t inst)
{
    uint32_t tb;
    char ai[8];
    if (!is_npc_vt(guest_rd32(inst)))
        return 0;
    tb = guest_rd32(inst + 0x158);
    if (!in_heap(tb))
        return 0;
    guest_str(tb + 0x42, ai, sizeof ai);
    return strcmp(ai, "ACTOR") != 0;
}

/* The level's people: those waiting (and what they are), for the menu. */
int rdr_spawn_list(char types[][16], int *waiting, int *team, int max)
{
    uint32_t mgr = guest_rd32(RDR_ENM_MGR), i;
    int n = 0, guard = 0, k;
    if (!in_heap(mgr))
        return 0;
    for (i = guest_rd32(mgr + 0x24); in_heap(i) && guard < 512; i = guest_rd32(i + 0x1B8), guard++) {
        char t[16];
        if (!spawn_usable(i))
            continue;
        spawn_type(i, t, sizeof t);
        for (k = 0; k < n && strcmp(types[k], t); k++)
            ;
        if (k == n) {
            if (n >= max)
                continue;
            snprintf(types[n], 16, "%s", t);
            waiting[n] = 0;
            team[n] = (int)guest_rd32(i + 0xB4);
            n++;
        }
        if (!guest_rd32(i + 0xC))
            waiting[k]++;
    }
    return n;
}

static int spawn_type_is_animal(const char *type)
{
    uint32_t mgr = guest_rd32(RDR_ENM_MGR), i;
    int guard = 0;
    if (!in_heap(mgr))
        return 0;
    for (i = guest_rd32(mgr + 0x24); in_heap(i) && guard < 512; i = guest_rd32(i + 0x1B8), guard++) {
        char t[16];
        spawn_type(i, t, sizeof t);
        if (!strcmp(t, type))
            return is_animal_vt(guest_rd32(i));
    }
    return 0;
}

/* For the menu: each kind with its name, how many wait, and whether it is
 * against the players. The teams' table (0x004065A8, 6x6 bytes, row a
 * team, column the team it looks at): the players' row (0) has bit 0 for
 * the teams that fight them (the bar fight's 2: 7; its barmaids' 4: 6,
 * the shopkeeper's 3: 0). */
int rdr_spawn_kinds(RdrSpawnKind *out, int max)
{
    char types[64][16];
    int w[64], t[64], n = rdr_spawn_list(types, w, t, 64), k;
    if (n > max)
        n = max;
    int unk[2] = { 0, 0 };
    for (k = 0; k < n; k++) {
        int tm = t[k];
        const char *nm = rdr_type_name(types[k]);
        snprintf(out[k].type, sizeof out[k].type, "%s", types[k]);
        out[k].waiting = w[k];
        out[k].enemy = tm > 0 && tm < 6 && (guest_rd8(0x004065A8u + (uint32_t)tm) & 1);
        out[k].animal = spawn_type_is_animal(types[k]);
        if (out[k].animal)
            out[k].enemy = 0;
        if (nm)
            snprintf(out[k].name, sizeof out[k].name, "%s", nm);
        else                            /* one with no name of its own */
            snprintf(out[k].name, sizeof out[k].name, "%s %d", out[k].enemy ? "Gunman" : "Townsperson",
                     ++unk[out[k].enemy]);
    }
    return n;
}

/* Tests ("enmdump"): the level's people, with what decides whether they
 * come back. */
void rdr_enm_dump(void)
{
    uint32_t mgr = guest_rd32(RDR_ENM_MGR), i;
    int guard = 0;
    if (!in_heap(mgr))
        return;
    for (i = guest_rd32(mgr + 0x24); in_heap(i) && guard < 512; i = guest_rd32(i + 0x1B8), guard++) {
        char t[16];
        uint32_t h = guest_rd32(i + 0x18), tb = guest_rd32(i + 0x158);
        const char *nm;
        spawn_type(i, t, sizeof t);
        nm = rdr_type_name(t);
        fprintf(stderr, "[ENM] %08X vt %08X %-6s %-18s in %08X hp %.0f/%.0f 1A5 %u 298 %.2f want %d +8 %u team %d usable %d at %.1f %.1f %.1f\n",
                i, guest_rd32(i), t, nm ? nm : "-", guest_rd32(i + 0xC),
                in_heap(h) ? guest_rdf(h + 0x10) : -1.0f, in_heap(h) ? guest_rdf(h + 0x14) : -1.0f,
                guest_rd8(i + 0x1A5), guest_rdf(i + 0x298), in_heap(tb) ? guest_rd8(tb + 0x40) : -1,
                guest_rd8(i + 8), (int)guest_rd32(i + 0xB4), spawn_usable(i),
                guest_rdf(i + 0x4C), guest_rdf(i + 0x50), guest_rdf(i + 0x54));
    }
    fflush(stderr);
}

static void spawn_place(uint32_t p, uint32_t pl, int nth)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, q, hd;
    float h, x, z, d = 3.0f + 0.9f * (float)(nth / 3), side = ((nth % 3) - 1) * 1.2f;
    hd = guest_rd32(pl + 0x5AC);
    memcpy(&h, &hd, 4);
    /* in front of the player, a row of three. A character faces -z of its
     * rotation, which is minus (sin, cos) of its heading [+0x5AC] (player
     * 1 heading -1.47: rotation's third row -0.995, 0.103 -- facing +x).
     * Before, the heading's own (sin, cos) was taken for ahead, and the
     * people came in behind the player, out of sight. */
    x = guest_rdf(pl + 0x4C) - sinf(h) * d + cosf(h) * side;
    z = guest_rdf(pl + 0x54) - cosf(h) * d - sinf(h) * side;
    /* The point goes to the setter from the guest's stack. It was written
     * at [p+0x5A0] (a player's own copy of its position), but p here is a
     * level person (enmInstance, 0x2A4 bytes): those 16 bytes went past its
     * end, into whatever the heap had next -- for a made person, its own
     * ammunition slots ([[inst+0x80]+0x14] 4..7 read -21.7, 0.8, 10.0,
     * 1.7), and the game crashed when it was reset after dying (user log,
     * Billy Cougar; harness). The heading the same: the setter keeps it. */
    {
        uint32_t v = (esp0 - 0x40u) & ~0xFu;
        float y = guest_rdf(pl + 0x50);
        memcpy((void *)(m + v), &x, 4);
        memcpy((void *)(m + v + 4), &y, 4);
        memcpy((void *)(m + v + 8), &z, 4);
        memset((void *)(m + v + 12), 0, 4);
        g_esp = v - 0x10u;
        guest_push(v); guest_push(0x00137430u);
        g_ecx = p; sub_001EB490(); g_esp = esp0;
    }
    {
        float back = h + 3.14159265f;           /* facing the player */
        uint32_t b;
        memcpy(&b, &back, 4);
        guest_push(b); guest_push(0x00137444u);
        g_ecx = p; sub_001EB4C0(); g_esp = esp0;
    }
    q = guest_rd32(p + 0xC);
    if (q && guest_rd32(q + 0x14)) {
        guest_push(0x00137463u);
        g_ecx = guest_rd32(q + 0x14); sub_00219D60(); g_esp = esp0;
    }
    g_ecx = ecx0; g_eax = eax0;
}

/* New people (rdr_npc.c): made on the game's thread at a player's update,
 * then brought in in front of that player as the spawner does. */
static volatile int s_npc_req, s_npc_player, s_npc_team;
/* New people: how many at a time, where (0 in front, 1 around the player,
 * 2 further off ahead), their guns (0 as their side has it, 1 none, 2
 * armed); and which of a batch this one is. */
static int s_npc_count = 1, s_npc_place, s_npc_arms, s_npc_nth, s_npc_animal, s_npc_boss;
static char s_npc_type[16], s_npc_table[48], s_npc_name[32];
extern uint32_t rdr_npc_make(const char *type, const char *table, const char *name, int team, int armed,
                             float x, float y, float z, float angle, char *why, size_t why_n);
void rdr_npc_request(const char *type, const char *table, const char *name, int team, int player)
{
    snprintf(s_npc_type, sizeof s_npc_type, "%s", type);
    snprintf(s_npc_table, sizeof s_npc_table, "%s", table);
    snprintf(s_npc_name, sizeof s_npc_name, "%s", name && *name ? name : type);
    s_npc_team = team;
    s_npc_animal = 0;
    s_npc_player = player;
    s_npc_req = 1;
}

/* The people made here stay wanted in: a Showdown bot's table (default_bot)
 * clears the byte the manager asks ([table+0x40]) in its first update, and
 * the manager took it out again at once (harness: in, wanted 1 -> 0, out
 * within 0.1 s). Kept at 1 for as long as they live. */
static uint32_t s_npc_made[32], s_npc_made_mgr;
/* People made and then sent away (rdr_npc_remove_run): kept out -- their
 * table's script may want them in again (Billy's, BRIM_ELT, does). */
static uint32_t s_npc_gone[64];
/* A made person put at x, y, z facing face (radians): the position and
 * heading through the character's setters (sub_001EB490, sub_001EB4C0),
 * then its physics told (sub_00219D60 on [[inst+0xC]+0x14]). */
static void npc_put(uint32_t inst, float x, float y, float z, float face)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, v = (esp0 - 0x40u) & ~0xFu, b, q;
    memcpy((void *)(m + v), &x, 4);
    memcpy((void *)(m + v + 4), &y, 4);
    memcpy((void *)(m + v + 8), &z, 4);
    memset((void *)(m + v + 12), 0, 4);
    g_esp = v - 0x10u;
    guest_push(v); guest_push(0x00137430u);
    g_ecx = inst; sub_001EB490(); g_esp = esp0;
    memcpy(&b, &face, 4);
    guest_push(b); guest_push(0x00137444u);
    g_ecx = inst; sub_001EB4C0(); g_esp = esp0;
    q = guest_rd32(inst + 0xC);
    if (q && guest_rd32(q + 0x14)) {
        guest_push(0x00137463u);
        g_ecx = guest_rd32(q + 0x14); sub_00219D60(); g_esp = esp0;
    }
    g_ecx = ecx0; g_eax = eax0;
}
static struct { uint32_t inst; float x, y, z, face; int ticks, moved; } s_npc_hold[8];
char g_rdr_npc_note[200];           /* a word for the Spawner page (rdr_overlay.cpp) */

static void npc_take_out(uint32_t i)
{
    uint32_t tb = guest_rd32(i + 0x158);
    if (in_heap(tb))
        *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + tb + 0x40) = 0;
    if (guest_rd32(i + 0xC)) {
        uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, fn = guest_rd32(guest_rd32(i) + 0x14);
        extern recomp_func_t recomp_lookup(uint32_t xbox_va);
        recomp_func_t f = recomp_lookup(fn);
        if (f) {
            guest_push(0x00074F31u);            /* as from the manager's update */
            g_ecx = i;
            f();
        }
        g_esp = esp0; g_ecx = ecx0; g_eax = eax0;
        guest_wr8(i + 8, 0);
    }
}
static void npc_keep_tick(void)
{
    int k;
    if (guest_rd32(RDR_ENM_MGR) != s_npc_made_mgr) {
        /* another level (or the same one again): those people went with it */
        memset(s_npc_made, 0, sizeof s_npc_made);
        memset(s_npc_gone, 0, sizeof s_npc_gone);
        memset(s_npc_hold, 0, sizeof s_npc_hold);
        s_npc_made_mgr = guest_rd32(RDR_ENM_MGR);
    }
    for (k = 0; k < 8; k++) {
        /* the made people held where they were put, their first moments */
        uint32_t i = s_npc_hold[k].inst;
        float dx, dz;
        if (!i)
            continue;
        /* One its table puts back at its own place every update (Chapter
         * 1's practice gunmen, PERFNEW_ENEMY1/2, waiting at the start of
         * their paths by the targets: put back in front five times, found
         * there again each next update -- harness) cannot be had in front:
         * it is sent away, and the Spawner page says so. */
        if (s_npc_hold[k].moved >= 5 && in_heap(i) && guest_rd32(i + 0xC)) {
            int m;
            char tb[48] = "";
            uint32_t t = guest_rd32(i + 0x158);
            if (in_heap(t))
                guest_str(t + 0x42, tb, sizeof tb);
            fprintf(stderr, "[NPC] %08X: its table (%s) keeps it at its own place; sent away\n", i, tb);
            snprintf(g_rdr_npc_note, sizeof g_rdr_npc_note,
                     "Acting like that one (%s), they keep to a path of their own in this level, so they "
                     "were sent away: pick another way of acting.", tb[0] ? tb : "?");
            npc_take_out(i);
            for (m = 0; m < 32; m++)
                if (s_npc_made[m] == i)
                    s_npc_made[m] = 0;
            s_npc_hold[k].inst = 0;
            continue;
        }
        if (--s_npc_hold[k].ticks <= 0 || !in_heap(i) || !is_npc_vt(guest_rd32(i)) || !guest_rd32(i + 0xC)) {
            if (s_npc_hold[k].moved)
                fprintf(stderr, "[NPC] %08X held in place %d time(s)\n", i, s_npc_hold[k].moved);
            s_npc_hold[k].inst = 0;
            continue;
        }
        dx = guest_rdf(i + 0x4C) - s_npc_hold[k].x;
        dz = guest_rdf(i + 0x54) - s_npc_hold[k].z;
        if (dx * dx + dz * dz > 6.0f * 6.0f) {
            if (s_npc_hold[k].moved < 3)
                fprintf(stderr, "[NPC] %08X found at %.1f %.1f %.1f (%d updates in): put back\n", i,
                        guest_rdf(i + 0x4C), guest_rdf(i + 0x50), guest_rdf(i + 0x54), 180 - s_npc_hold[k].ticks);
            npc_put(i, s_npc_hold[k].x, s_npc_hold[k].y, s_npc_hold[k].z, s_npc_hold[k].face);
            s_npc_hold[k].moved++;
        }
    }
    for (k = 0; k < 64; k++) {
        uint32_t i = s_npc_gone[k];
        if (!i)
            continue;
        if (!in_heap(i) || !is_npc_vt(guest_rd32(i))) {
            s_npc_gone[k] = 0;
            continue;
        }
        {
            uint32_t tb = guest_rd32(i + 0x158);
            if (guest_rd32(i + 0xC) || (in_heap(tb) && guest_rd8(tb + 0x40)))
                npc_take_out(i);
        }
    }
    for (k = 0; k < 32; k++) {
        uint32_t i = s_npc_made[k], h, tb;
        if (!i)
            continue;
        h = guest_rd32(i + 0x18);
        if (!in_heap(i) || !is_npc_vt(guest_rd32(i)) || !in_heap(h) || !(guest_rdf(h + 0x10) > 0.0f)) {
            s_npc_made[k] = 0;
            continue;
        }
        tb = guest_rd32(i + 0x158);
        if (in_heap(tb) && !guest_rd8(tb + 0x40))
            *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + tb + 0x40) = 1;
    }
}

/* While one is being made and brought in: an AI table from another
 * chapter that names something this level lacks (a cannon, one of that
 * chapter's zones) is noted rather than ending the game (rdr_hooks.c,
 * sub_000175B0), and the one made is not brought in, or is sent away. */
int g_rdr_npc_making, g_rdr_npc_bad;
static uint32_t s_npc_last;
static void npc_run_one(void);
static void npc_take_out(uint32_t inst);

static void npc_run(void)
{
    s_npc_last = 0;
    g_rdr_npc_bad = 0;
    g_rdr_npc_making = 1;
    npc_run_one();
    g_rdr_npc_making = 0;
    if (g_rdr_npc_bad && s_npc_last) {
        int k;
        for (k = 0; k < 32; k++)
            if (s_npc_made[k] == s_npc_last)
                s_npc_made[k] = 0;
        npc_take_out(s_npc_last);
        fprintf(stderr, "[NPC] %08X sent away: its AI table needs what this level does not have\n", s_npc_last);
        fflush(stderr);
    }
}

static void npc_run_one(void)
{
    uint32_t mgr = guest_rd32(RDR_ENM_MGR), pl, inst;
    char why[96];
    float h, d, a, x, z;
    int armed;
    s_npc_req = 0;
    pl = guest_rd32(0x00675930u + 4u * (uint32_t)(s_npc_player > 0 && s_npc_player < 4 ? s_npc_player : 0));
    if (!in_heap(mgr) || !in_heap(pl)) {
        fprintf(stderr, "[NPC] no %s now\n", in_heap(mgr) ? "player" : "level");
        fflush(stderr);
        return;
    }
    if (!rdr_npc_room()) {
        fprintf(stderr, "[NPC] no room: everyone the level can hold at once is in\n");
        fflush(stderr);
        return;
    }
    h = guest_rdf(pl + 0x5AC);
    /* where: ahead is -(sin, cos) of the heading (see spawn_place) */
    if (s_npc_place == 1) {
        int n = s_npc_count > 1 ? s_npc_count : 1;
        a = h + 6.2831853f * (float)s_npc_nth / (float)n;
        d = 5.0f;
        x = guest_rdf(pl + 0x4C) - sinf(a) * d;
        z = guest_rdf(pl + 0x54) - cosf(a) * d;
    } else {
        float side = ((s_npc_nth % 3) - 1) * 1.6f;
        d = (s_npc_place == 2 ? 15.0f : 4.0f) + 1.8f * (float)(s_npc_nth / 3);
        x = guest_rdf(pl + 0x4C) - sinf(h) * d + cosf(h) * side;
        z = guest_rdf(pl + 0x54) - cosf(h) * d - sinf(h) * side;
        a = h;
    }
    armed = s_npc_animal ? 0 : s_npc_arms == 2 ? 1 : s_npc_arms == 1 ? 0 : (s_npc_team != 0 && s_npc_team != 3);
    {
        /* facing the player */
        float px = guest_rdf(pl + 0x4C) - x, pz = guest_rdf(pl + 0x54) - z;
        float face = atan2f(-px, -pz) + 3.14159265f;
        inst = rdr_npc_make(s_npc_type, s_npc_table, s_npc_name, s_npc_team,
                            armed | (s_npc_animal ? 2 : 0) | (s_npc_boss && !s_npc_animal ? 4 : 0), x, guest_rdf(pl + 0x50), z,
                            face * 57.29578f, why, sizeof why);
    }
    (void)a;
    s_npc_last = inst;
    if (inst && g_rdr_npc_bad) {
        fprintf(stderr, "[NPC] %08X not brought in: its AI table needs what this level does not have\n", inst);
        fflush(stderr);
        s_npc_last = 0;                 /* (made, on the level's list, never in) */
        return;
    }
    if (!inst) {
        if (why[0]) {
            fprintf(stderr, "[NPC] %s (%s): %s\n", s_npc_type, s_npc_table, why);
            fflush(stderr);
        }
        return;
    }
    {
        uint32_t tb = guest_rd32(inst + 0x158), esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax;
        if (in_heap(tb))
            *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + tb + 0x40) = 1;
        guest_push(inst);
        guest_push(0x00074EA8u);
        g_ecx = mgr;
        sub_00072CA0();
        g_esp = esp0; g_ecx = ecx0; g_eax = eax0;
    }
    if (guest_rd32(inst + 0xC)) {
        /* Put where asked, facing the player, through the setters as the
         * spawner does -- for everyone now, not only animals (whose
         * controller never took the spawn point: a cow at 0, 0): a person
         * acting as one of the level's own could start where that one's
         * table starts it (user: in the first chapter, people made "in
         * front" came in by the tutorial's targets). And held there for
         * the first moments, in case the table moves them on its first
         * steps (npc_keep_tick). */
        float y = guest_rdf(pl + 0x50), px = guest_rdf(pl + 0x4C) - x, pz = guest_rdf(pl + 0x54) - z;
        float face = atan2f(-px, -pz) + 3.14159265f;
        int k;
        npc_put(inst, x, y, z, face);
        for (k = 0; k < 8 && s_npc_hold[k].inst; k++)
            ;
        if (k == 8)
            k = 0;
        s_npc_hold[k].inst = inst;
        s_npc_hold[k].x = x;
        s_npc_hold[k].y = y;
        s_npc_hold[k].z = z;
        s_npc_hold[k].face = face;
        s_npc_hold[k].ticks = 180;
        s_npc_hold[k].moved = 0;
    }
    if (!guest_rd32(inst + 0xC)) {
        fprintf(stderr, "[NPC] %08X made but would not come in (no free controller?)\n", inst);
        fflush(stderr);
        return;
    }
    {
        int k;
        for (k = 0; k < 32 && s_npc_made[k]; k++)
            ;
        s_npc_made[k < 32 ? k : 31] = inst;
    }
    fprintf(stderr, "[NPC] %08X %s in at %.1f %.1f (player %d, team %d, %s)\n", inst, s_npc_type, x, z,
            s_npc_player + 1, (int)guest_rd32(inst + 0xB4), armed ? "armed" : "unarmed");
    fflush(stderr);
}

void rdr_npc_opts_run(int count, int place, int arms)
{
    s_npc_count = count;
    s_npc_place = place;
    s_npc_arms = arms & 0x0F;
    s_npc_boss = (arms & 0x10) != 0;
}

/* The people made here, sent away: not wanted in any more, so the enemy
 * manager takes each out at its next update (sub_00074D10, as it does
 * anyone the level no longer wants); dead ones lying there go too. They
 * stay on the level's list (its scripts may hold them), out. */
void rdr_npc_remove_run(void)
{
    int k, n = 0, g;
    for (k = 0; k < 32; k++) {
        uint32_t i = s_npc_made[k];
        if (!i)
            continue;
        s_npc_made[k] = 0;
        if (!in_heap(i) || !is_npc_vt(guest_rd32(i)))
            continue;
        npc_take_out(i);
        for (g = 0; g < 64 && s_npc_gone[g]; g++)
            ;
        if (g < 64)
            s_npc_gone[g] = i;
        n++;
    }
    fprintf(stderr, "[NPC] %d made people sent away\n", n);
    fflush(stderr);
}

/* The level's ways of acting: one per AI table its people use (named at
 * [table+0x42]), not a cutscene's ("ACTOR"); the people's first, then the
 * animals' (horses, cattle ...: made only as themselves, .animal set). */
int rdr_npc_acts(RdrNpcAct *out, int max)
{
    uint32_t mgr = guest_rd32(RDR_ENM_MGR), i;
    int n = 0, guard, k, pass;
    if (!in_heap(mgr))
        return 0;
    for (pass = 0; pass < 2; pass++) {
        guard = 0;
        for (i = guest_rd32(mgr + 0x24); in_heap(i) && guard < 512 && n < max; i = guest_rd32(i + 0x1B8), guard++) {
            uint32_t tb = guest_rd32(i + 0x158), tm = guest_rd32(i + 0xB4), vt = guest_rd32(i);
            char t[48], ty[16];
            const char *nm;
            if (!in_heap(tb) || (pass == 0 ? vt != RDR_ENEMY_VT : !is_animal_vt(vt)))
                continue;
            guest_str(tb + 0x42, t, sizeof t);
            if (!t[0] || !strcmp(t, "ACTOR"))
                continue;
            for (k = 0; k < n && strcmp(out[k].table, t); k++)
                ;
            if (k < n)
                continue;
            spawn_type(i, ty, sizeof ty);
            snprintf(out[n].table, sizeof out[n].table, "%s", t);
            snprintf(out[n].type, sizeof out[n].type, "%s", ty);
            nm = rdr_type_name(ty);
            snprintf(out[n].name, sizeof out[n].name, "%s", nm ? nm : ty);
            out[n].team = (int)tm;
            out[n].animal = pass;
            out[n].enemy = !pass && tm > 0 && tm < 6 && (guest_rd8(0x004065A8u + tm) & 1);
            n++;
        }
    }
    return n;
}

/* the name the HUD shows (a boss's bar, Health bar on): the character's
 * own, not its code ("elt": harness) */
static void npc_name_from(const char *nm, const char *type)
{
    char *c, *o;
    snprintf(s_npc_name, sizeof s_npc_name, "%s", nm && *nm ? nm : type);
    for (c = o = s_npc_name; *c; c++) {
        if (*c == '"')
            continue;                       /* ("Kid" Cougar) */
        *o++ = (*c == '\n' || *c == '\r' || *c == '\t') ? ' ' : *c;
    }
    *o = 0;
}

/* The story levels' people (tools/mods/npc_others.py): someone made here
 * can act as one of them does -- their AI table, loaded from swat.dat on
 * the spot. A level's heaps hold what it loads and little more; what a
 * table from elsewhere needs comes from the overflow heap (rdr_hooks.c,
 * sub_0005D650). */
static const struct { const char *where, *table, *type; int team, armed; } s_others[] = {
#include "rdr_npc_others.inc"
};
#define N_OTHERS ((int)(sizeof s_others / sizeof s_others[0]))
/* and their animals (Character list codes 2000 + i): an animal acts as its
 * own kind does in its own chapter */
static const struct { const char *where, *table, *type, *name; int animal; } s_beasts[] = {
#include "rdr_npc_animals.inc"
};
#define N_BEASTS ((int)(sizeof s_beasts / sizeof s_beasts[0]))

int rdr_npc_beasts_count(void) { return N_BEASTS; }

int rdr_npc_beast_info(int i, const char **where, const char **name)
{
    if (i < 0 || i >= N_BEASTS)
        return 0;
    *where = s_beasts[i].where;
    *name = s_beasts[i].name;
    return 1;
}
static int s_npc_other = -1;

int rdr_npc_others_count(void) { return N_OTHERS; }

int rdr_npc_other_info(int i, const char **where, const char **type, int *team, int *armed)
{
    if (i < 0 || i >= N_OTHERS)
        return 0;
    *where = s_others[i].where;
    *type = s_others[i].type;
    *team = s_others[i].team;
    *armed = s_others[i].armed;
    return 1;
}

void rdr_npc_other_run(int idx)
{
    s_npc_other = idx >= 0 && idx < N_OTHERS ? idx : -1;
}

/* (a command: the game's thread, the same on every machine online) */
void rdr_npc_make_run(int player, int who, int act, int side)
{
    RdrNpcAct acts[64];
    int n = rdr_npc_acts(acts, 64), team, other = s_npc_other;
    const char *type;
    s_npc_other = -1;                   /* (each make says again) */
    fprintf(stderr, "[NPC] make: player %d, character %d, acting %d of %d%s%s, side %d\n", player + 1, who,
            act, n, other >= 0 ? ", like " : "", other >= 0 ? s_others[other].table : "", side);
    fflush(stderr);
    if (who >= 2000 && who - 2000 < N_BEASTS) {
        /* another chapter's animal: its kind, as it acts there, no side */
        int b = who - 2000;
        snprintf(s_npc_type, sizeof s_npc_type, "%s", s_beasts[b].type);
        snprintf(s_npc_table, sizeof s_npc_table, "%s", s_beasts[b].table);
        npc_name_from(s_beasts[b].name, s_beasts[b].type);
        s_npc_team = 2;
        s_npc_animal = s_beasts[b].animal;
        s_npc_player = player;
        for (s_npc_nth = 0; s_npc_nth < (s_npc_count > 0 ? s_npc_count : 1); s_npc_nth++)
            npc_run();
        s_npc_nth = 0;
        return;
    }
    if (who >= 1000 && who - 1000 < n && acts[who - 1000].animal) {
        /* an animal: as the level's own (its table, its side, no guns) */
        act = who - 1000;
        side = 0;
        other = -1;
    }
    if (other >= 0) {
        if (who >= 1000) {
            if (who - 1000 >= n)
                return;
            type = acts[who - 1000].type;
        } else if (who >= 1 && who < g_rdr_character_count && g_rdr_characters[who].code[0]) {
            type = g_rdr_characters[who].code;
        } else {
            fprintf(stderr, "[NPC] no character %d (%d)\n", who, g_rdr_character_count);
            fflush(stderr);
            return;
        }
        snprintf(s_npc_type, sizeof s_npc_type, "%s", type);
        snprintf(s_npc_table, sizeof s_npc_table, "%s", s_others[other].table);
        npc_name_from(who >= 1000 ? acts[who - 1000].name : g_rdr_characters[who].name, type);
        /* its side in its own level means little here: "As that person"
         * is against the players if it fought them there, else on no side */
        s_npc_team = side == 1 ? 1 : side == 2 ? 0 : s_others[other].team == 1 ? 1 : 3;
        s_npc_animal = 0;
        s_npc_player = player;
        for (s_npc_nth = 0; s_npc_nth < (s_npc_count > 0 ? s_npc_count : 1); s_npc_nth++)
            npc_run();
        s_npc_nth = 0;
        return;
    }
    if (act < 0 || act >= n) {
        fprintf(stderr, "[NPC] no way of acting %d here (%d)\n", act, n);
        fflush(stderr);
        return;
    }
    if (who >= 1000) {
        if (who - 1000 >= n)
            return;
        type = acts[who - 1000].type;
    } else if (who >= 1 && who < g_rdr_character_count && g_rdr_characters[who].code[0]) {
        type = g_rdr_characters[who].code;
    } else {
        fprintf(stderr, "[NPC] no character %d (%d)\n", who, g_rdr_character_count);
        fflush(stderr);
        return;
    }
    team = side == 1 ? 1 : side == 2 ? 0 : acts[act].team;
    snprintf(s_npc_type, sizeof s_npc_type, "%s", type);
    snprintf(s_npc_table, sizeof s_npc_table, "%s", acts[act].table);
    npc_name_from(who >= 1000 ? acts[who - 1000].name : g_rdr_characters[who].name, type);
    s_npc_team = team;
    s_npc_animal = acts[act].animal;
    s_npc_player = player;
    for (s_npc_nth = 0; s_npc_nth < (s_npc_count > 0 ? s_npc_count : 1); s_npc_nth++)
        npc_run();
    s_npc_nth = 0;
}

static void spawn_run(void)
{
    uint32_t mgr = guest_rd32(RDR_ENM_MGR), i, pl;
    int want = s_spawn_req, done = 0, guard = 0;
    s_spawn_req = 0;
    pl = guest_rd32(0x00675930u + 4u * (uint32_t)(s_spawn_player > 0 && s_spawn_player < 4 ? s_spawn_player : 0));
    if (!in_heap(mgr) || !in_heap(pl))
        return;
    for (i = guest_rd32(mgr + 0x24); in_heap(i) && guard < 512 && done < want; i = guest_rd32(i + 0x1B8), guard++) {
        char t[16];
        uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax;
        if (!spawn_usable(i) || guest_rd32(i + 0xC) || !in_heap(guest_rd32(i + 0x18)))
            continue;                   /* in already (one killed and taken out comes back: the
                                         * bring-in revives it, sub_00072CA0's first step) */
        spawn_type(i, t, sizeof t);
        if (s_spawn_type[0] && strcmp(t, s_spawn_type))
            continue;
        /* "wanted in": the byte the manager's update asks (sub_00076900:
         * [[inst+0x158]+0x40], the instance's AI table, set by the level's
         * script) -- without it the next update took it out again */
        {
            uint32_t tb = guest_rd32(i + 0x158);
            if (in_heap(tb))
                *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + tb + 0x40) = 1;
        }
        if (!rdr_npc_room()) {
            fprintf(stderr, "[SPAWN] no room: everyone the level can hold at once is in\n");
            break;
        }
        guest_push(i);
        guest_push(0x00074EA8u);        /* as from the manager's update */
        g_ecx = mgr;
        sub_00072CA0();
        g_esp = esp0; g_ecx = ecx0; g_eax = eax0;
        if (!guest_rd32(i + 0xC)) {
            fprintf(stderr, "[SPAWN] %08X (%s) would not come in\n", i, t);
            continue;
        }
        spawn_place(i, pl, done);
        fprintf(stderr, "[SPAWN] %08X %s (team %d) in front of player %d: %.2f %.2f %.2f\n", i, t,
                (int)guest_rd32(i + 0xB4), s_spawn_player + 1, guest_rdf(i + 0x4C), guest_rdf(i + 0x50),
                guest_rdf(i + 0x54));
        done++;
    }
    if (!done)
        fprintf(stderr, "[SPAWN] nobody left to bring in%s%s\n", s_spawn_type[0] ? " of type " : "", s_spawn_type);
    fflush(stderr);
}

/* ── Co-op: weapons and money (settings: coop_loadout) ──────────────────
 *
 * Each player object has its own money ([player+0x9FC]; GetMoney vtable
 * +0x164 adds the HUD's counting-up, SetMoney +0x168 = sub_0013E2B0 sets
 * both) and weapons ([char+0x80]+8, AddWeapon +0x104), but only player
 * 1's are the profile's: kept between chapters, spent in the shop, set by
 * the weapon select. Two ways for the others:
 *  - Shared (0): money anyone earns goes to player 1 (AddMoney redirected,
 *    player_add_money), every player's HUD shows that one purse, and the
 *    others start each chapter with player 1's weapons as well as their
 *    own -- player 1 shops and picks the loadout for everyone.
 *  - Each player's own (1): players 2-4 keep their own money and weapons
 *    from chapter to chapter (what they carried at the end of the last
 *    one, given back two seconds into the next), and buy weapons on the
 *    Co-op Shop page (the game's own prices, invmasterlist.csv) with their
 *    own money: a command, so online every machine makes the same sale.
 *    Kept in Saves\coop_loadouts.txt (not online: there every machine
 *    starts the session with the same empty purses). */
extern int rdr_coop_loadout(void);
extern int rdr_online_session(void);
extern void sub_0013E2B0(void);
#define LD_WEAPONS 96
static int32_t s_ld_money[4];
static uint8_t s_ld_owned[4][LD_WEAPONS];
static int s_ld_have[4], s_ld_dirty, s_ld_loaded;
static uint32_t s_ld_applied[4];
static uint64_t s_ld_seen_ns[4], s_ld_saved_ns;

static int32_t player_money(uint32_t c) { return (int32_t)guest_rd32(c + 0x9FC); }

/* A player's money and its HUD counter ([player+0x890]).
 *
 * Not the game's SetMoney (sub_0013E2B0 -> the HUD's sub_001D4E10): that
 * blanks the counter's digits, and the HUD (sub_001D7760) then counts the
 * whole sum up from nothing, its counting sound (the sound player's 0xE,
 * until 0xF at the end) playing all the while -- every time player 1's
 * money changed, player 2's counter, mirrored that way, counted its whole
 * $2,000-odd again after player 1's had added the $8 (user: "the noise for
 * money being added plays for way too long").
 *
 * The counter: +0x53F8 the money, +0x5420 what the digits are rolling to,
 * +0x5424 the digits' targets and +0x5436 the digits shown (9 shorts each,
 * 0xFFFF blank), +0x5448 each digit's step timer (1.0 at rest), +0x546C up
 * or down, +0x5470 the gleam after, +0x5474 counting (0xF at its end).
 *   at_once: the digits set, no counting (a level's start);
 *   else: counted from what is shown, as a payment is (the Co-op Shop). */
static void player_set_money_hud(uint32_t c, int32_t v, int at_once)
{
    uint32_t h = guest_rd32(c + 0x890);
    if (v < 0) v = 0;
    if (v > 9999999) v = 9999999;
    guest_wr32(c + 0x9FC, (uint32_t)v);
    if (!in_heap(h))
        return;
    guest_wr32(h + 0x53F8, (uint32_t)v);
    if (at_once) {
        int i, seen = 0, p = 1000000;
        for (i = 0; i < 7; i++, p /= 10) {
            int d = v / p % 10;
            uint16_t s = (d || seen || i == 6) ? (uint16_t)d : 0xFFFFu;
            if (d) seen = 1;
            guest_wr16(h + 0x5424 + 2u * (uint32_t)i, s);
            guest_wr16(h + 0x5436 + 2u * (uint32_t)i, s);
        }
        for (i = 0; i < 9; i++)
            guest_wr32(h + 0x5448 + 4u * (uint32_t)i, 0x3F800000u);
        guest_wr32(h + 0x5420, (uint32_t)v);
        guest_wr8(h + 0x5474, 0);
    }
}

/* Shared purse: player 2-4's counter shows player 1's, rolling with it --
 * its own never counts (or sounds). */
static void coop_money_mirror(uint32_t c, uint32_t p1)
{
    uint32_t h = guest_rd32(c + 0x890), h1 = guest_rd32(p1 + 0x890), o;
    guest_wr32(c + 0x9FC, guest_rd32(p1 + 0x9FC));
    if (!in_heap(h) || !in_heap(h1) || h == h1)
        return;
    guest_wr32(h + 0x53F8, guest_rd32(h1 + 0x53F8));
    guest_wr32(h + 0x5420, guest_rd32(h1 + 0x5420));
    for (o = 0x5424; o < 0x546C; o += 4)
        guest_wr32(h + o, guest_rd32(h1 + o));
    guest_wr8(h + 0x546C, guest_rd8(h1 + 0x546C));
    guest_wr32(h + 0x5470, guest_rd32(h1 + 0x5470));
    guest_wr8(h + 0x5474, 0);
}

static int player_has_weapon(uint32_t c, int i)
{
    uint32_t l = guest_rd32(c + 0x80), v = in_heap(l) ? guest_rd32(l + 8) : 0;
    return in_heap(v) && i >= 0 && i < LD_WEAPONS && guest_rd8(v + (uint32_t)i);
}

extern void sub_00139E50(void);
static void player_add_weapon(uint32_t c, int i)
{
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, edx0 = g_edx;
    if (player_has_weapon(c, i))
        return;
    guest_push(0); guest_push(0); guest_push(0); guest_push((uint32_t)i);
    guest_push(0x00092909u);
    g_ecx = c;
    sub_00139E50();                     /* AddWeapon (vtable +0x104), ret 16 */
    g_esp = esp0; g_ecx = ecx0; g_eax = eax0; g_edx = edx0;
}

static void ld_path(char *out, size_t n)
{
    const char *d = getenv("RECOMP_SAVE_DIR");
    snprintf(out, n, "%s/coop_loadouts.txt", d && *d ? d : ".");
}

static void ld_load(void)
{
    char path[1100], line[600];
    FILE *f;
    s_ld_loaded = 1;
    if (rdr_online_session())
        return;
    ld_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        int k, money, pos = 0, w;
        if (sscanf(line, "player%d %d%n", &k, &money, &pos) != 2 || k < 2 || k > 4)
            continue;
        k--;
        s_ld_money[k] = money;
        memset(s_ld_owned[k], 0, LD_WEAPONS);
        for (char *p = line + pos; *p;) {
            if (sscanf(p, " %d", &w) == 1 && w >= 0 && w < LD_WEAPONS)
                s_ld_owned[k][w] = 1;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != '\n') p++;
            if (*p == '\n') break;
        }
        s_ld_have[k] = 1;
    }
    fclose(f);
}

static void ld_save(void)
{
    char path[1100];
    FILE *f;
    int k, i;
    if (rdr_online_session())
        return;
    ld_path(path, sizeof path);
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# Story co-op, each player's own weapons and money: player money weapon...\n");
    for (k = 1; k < 4; k++) {
        if (!s_ld_have[k])
            continue;
        fprintf(f, "player%d %d", k + 1, (int)s_ld_money[k]);
        for (i = 0; i < LD_WEAPONS; i++)
            if (s_ld_owned[k][i])
                fprintf(f, " %d", i);
        fprintf(f, "\n");
    }
    fclose(f);
}

void rdr_coop_loadouts_clear(void)
{
    memset(s_ld_have, 0, sizeof s_ld_have);
    memset(s_ld_money, 0, sizeof s_ld_money);
    memset(s_ld_owned, 0, sizeof s_ld_owned);
    s_ld_dirty = 1;
    ld_save();
}

/* each co-op player's update (not player 1's) */
static void coop_loadout_tick(uint32_t c)
{
    int k = coop_index(c), mode = rdr_coop_loadout(), i, n = rdr_weapon_count();
    uint32_t p1 = s_coop_p[0];
    uint64_t now = xbox_HostNowNs();
    if (k < 1 || k > 3 || s_coop_n < 2 || !in_heap(p1) || guest_rd32(0x00652E30u) != 0)
        return;
    if (n > LD_WEAPONS) n = LD_WEAPONS;
    if (!s_ld_loaded)
        ld_load();
    if (s_ld_applied[k] != c) {
        /* a new level's player: two seconds in, as the level's own
         * start-up has given its weapons */
        if (s_ld_applied[k] != 0xFFFFFFFFu) {
            s_ld_seen_ns[k] = now;
            s_ld_applied[k] = 0xFFFFFFFFu;
        }
        if (s_ld_applied[k] == 0xFFFFFFFFu && now - s_ld_seen_ns[k] < 2000000000ull)
            return;
        if (mode == 1) {
            if (s_ld_have[k]) {
                player_set_money_hud(c, s_ld_money[k], 1);
                for (i = 0; i < n; i++)
                    if (s_ld_owned[k][i])
                        player_add_weapon(c, i);
            }
        } else {
            for (i = 0; i < n; i++)
                if (player_has_weapon(p1, i))
                    player_add_weapon(c, i);
        }
        fprintf(stderr, "[COOP] player %d: %s weapons and money (%d)\n", k + 1,
                mode == 1 ? (s_ld_have[k] ? "its own" : "new, its own from now") : "player 1's", player_money(c));
        fflush(stderr);
        s_ld_applied[k] = c;
        s_ld_seen_ns[k] = 0;
        return;
    }
    if (mode == 1) {
        int32_t mo = player_money(c);
        if (mo != s_ld_money[k] || !s_ld_have[k]) { s_ld_money[k] = mo; s_ld_dirty = 1; }
        for (i = 0; i < n; i++) {
            uint8_t h = (uint8_t)player_has_weapon(c, i);
            if (h != s_ld_owned[k][i]) { s_ld_owned[k][i] = h; s_ld_dirty = 1; }
        }
        s_ld_have[k] = 1;
        if (s_ld_dirty && now - s_ld_saved_ns > 3000000000ull) {
            s_ld_saved_ns = now;
            s_ld_dirty = 0;
            ld_save();
        }
    } else {
        coop_money_mirror(c, p1);                   /* the one purse, on every HUD */
    }
}

/* Shared: money a co-op player is paid goes to player 1 */
static uint32_t coop_money_to(uint32_t c)
{
    if (rdr_coop_loadout() == 0 && s_coop_n >= 2 && coop_index(c) >= 1 && in_heap(s_coop_p[0])
        && guest_rd32(0x00652E30u) == 0)
        return s_coop_p[0];
    return c;
}

/* ── The Co-op Shop: the game's prices ── */
typedef struct { char name[32]; int cost; } ShopPrice;
static ShopPrice s_prices[128];
static int s_nprices = -1;
extern int rdr_art_open(const char *game_dir);
extern void rdr_art_close(void);
extern unsigned char *rdr_art_read(const char *name, size_t *len);
extern const char *rdr_ui_game_dir(void);
static void prices_load(void)
{
    size_t len = 0;
    unsigned char *d;
    char *p, *e;
    s_nprices = 0;
    if (!rdr_art_open(rdr_ui_game_dir()))
        return;
    d = rdr_art_read("invmasterlist.csv", &len);
    rdr_art_close();
    if (!d)
        return;
    p = (char *)d;
    e = p + len;
    while (p < e && s_nprices < 128) {
        char line[256], type[32];
        size_t n = 0;
        while (p < e && *p != '\n' && n < sizeof line - 1) line[n++] = *p++;
        line[n] = 0;
        while (p < e && *p != '\n') p++;
        if (p < e) p++;
        {
            char *c1 = strchr(line, ','), *c2, *c3, *c4;
            if (!c1) continue;
            *c1 = 0;
            c2 = strchr(c1 + 1, ',');
            if (!c2) continue;
            *c2 = 0;
            snprintf(type, sizeof type, "%s", c1 + 1);
            c3 = strchr(c2 + 1, ',');
            if (!c3) continue;
            c4 = c3 + 1;
            if (strcmp(type, "weapon")) continue;
            snprintf(s_prices[s_nprices].name, sizeof s_prices[0].name, "%s", line);
            s_prices[s_nprices].cost = atoi(c4);
            s_nprices++;
        }
    }
    free(d);
}

/* the game's price of weapon i, or -1 */
int rdr_weapon_price(int i)
{
    char nm[40];
    int k;
    if (s_nprices < 0)
        prices_load();
    if (!rdr_weapon_name(i, nm, sizeof nm))
        return -1;
    for (k = 0; k < s_nprices; k++)
        if (!strcmp(s_prices[k].name, nm))
            return s_prices[k].cost;
    return -1;
}

int rdr_coop_player_money(int player)
{
    uint32_t c = guest_rd32(0x00675930u + 4u * (uint32_t)(player & 3));
    return in_heap(c) ? (int)player_money(c) : 0;
}

/* a sale (a command: every machine makes it at the same moment) */
void rdr_coop_buy_run(int player, int weapon)
{
    uint32_t c = guest_rd32(0x00675930u + 4u * (uint32_t)(player & 3));
    int price = rdr_weapon_price(weapon);
    char nm[40];
    rdr_weapon_name(weapon, nm, sizeof nm);
    if (!in_heap(c) || price < 0 || player_has_weapon(c, weapon) || player_money(c) < price) {
        fprintf(stderr, "[SHOP] player %d could not buy %s (price %d, money %d)\n", player + 1, nm, price,
                in_heap(c) ? (int)player_money(c) : -1);
        fflush(stderr);
        return;
    }
    player_set_money_hud(c, player_money(c) - price, 0);
    player_add_weapon(c, weapon);
    fprintf(stderr, "[SHOP] player %d bought %s for $%d: $%d left\n", player + 1, nm, price, (int)player_money(c));
    fflush(stderr);
}

/* The Battle Royale arena (lvl_royale, made current by lvl_roydusk 6 and
 * lvl_royfinal 7) gives every player's character its own player brain
 * (sub_00137790 -> sub_001393F0: [char+0x158] = the character's brain for
 * the current level's enemy file) -- playerbrain_royale_6, which plays the
 * duels. Both players ran it, and the first through its intro took the
 * duel: player 2 dueled, player 1 waited for good. Extra players keep the
 * empty brain their spawn gave them, so the story's duels are player 1's
 * as in one-player games. */
static void coop_brain_keep(uint32_t c)
{
    int k;
    uint32_t sm;
    if (!s_coop_kept || s_coop_n < 2)
        return;
    for (k = 1; k < s_coop_n && k < 4; k++)
        if (s_coop_p[k] == c)
            break;
    if (k >= s_coop_n || k >= 4 || !in_heap(s_coop_sm[k]))
        return;
    sm = guest_rd32(c + 0x158);
    if (sm != s_coop_sm[k] && guest_rd32(s_coop_sm[k] + 4) == c) {
        guest_wr32(c + 0x158, s_coop_sm[k]);
        fprintf(stderr, "[COOP] player %d: the level's brain %08X put back to its own %08X\n", k + 1, sm,
                s_coop_sm[k]);
        fflush(stderr);
    }
}

/* Every player duels (F1 > Story Co-op > Duels): the duel manager
 * [0x0065B904] plays every duel object it has at once (Showdown's, and the
 * story's against several men). A story duel gives player 1 one (the
 * opponent's brain, then player 1's: sub_00095440 takes one from the
 * manager, sub_00090340 puts it on the character) and starts them all
 * (sub_00095560: the manager's +0x80 set, sub_000950A0 on each, which
 * points a player's at the level's duel opponent, [enemy file + 0x24]).
 * Players 2-4 get theirs as the manager starts, the same kind as player
 * 1's ([duel+0x14]), started the same way; taken off them when the duels
 * are over. */
extern void sub_00095440(void);
extern void sub_00090340(void);
extern void sub_000950A0(void);
extern int rdr_coop_duels(void);
static int s_duels_on;
static uint32_t s_coop_duel[4];
static uint32_t s_duel_p1;              /* player 1's duel, while others duel with it */

/* A duel's letterboxed camera shows player 1's view alone, full screen,
 * as a cutscene does; while other players duel too the split stays, so
 * each sees their own. */
static int coop_duels_split(void)
{
    int k;
    if (!s_duels_on)
        return 0;
    for (k = 1; k < s_coop_n && k < 4; k++)
        if (s_coop_duel[k] && in_heap(s_coop_p[k]) && guest_rd32(s_coop_p[k] + 0x190) == s_coop_duel[k])
            return 1;
    return 0;
}

static void coop_duel_set(uint32_t c, uint32_t obj)
{
    guest_push(obj);
    guest_push(0x000E7BB1u);            /* as sub_000E62C0's own call */
    g_ecx = c;
    sub_00090340();                     /* ret 4 */
}

static void coop_duels_end(void)
{
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, edx0 = g_edx;
    int k;
    s_duel_p1 = 0;
    for (k = 1; k < s_coop_n && k < 4; k++) {
        if (s_coop_duel[k] && in_heap(s_coop_p[k]) && guest_rd32(s_coop_p[k] + 0x190) == s_coop_duel[k]) {
            coop_duel_set(s_coop_p[k], 0);
            fprintf(stderr, "[COOP] duels over: player %d's duel %08X taken off\n", k + 1, s_coop_duel[k]);
            fflush(stderr);
        }
        s_coop_duel[k] = 0;
    }
    g_esp = esp0; g_ecx = ecx0; g_eax = eax0; g_edx = edx0;
}

/* s_duels_on: 0 no duel, 1 duels on (players 2-4 in them), 2 player 1's
 * decided and theirs taken off, the manager not yet done. */
static void coop_duels_tick(void)
{
    uint32_t mgr = guest_rd32(0x0065B904u), d1, opp, n, i;
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, edx0 = g_edx;
    int k, mine;
    if (s_coop_n < 2 || !in_heap(mgr)) {
        s_duels_on = 0;
        s_duel_p1 = 0;
        memset(s_coop_duel, 0, sizeof s_coop_duel);
        return;
    }
    if (!guest_rd8(mgr + 0x80)) {
        if (s_duels_on)
            coop_duels_end();
        s_duels_on = 0;
        return;
    }
    if (s_duels_on) {
        uint32_t d = in_heap(s_coop_p[0]) ? guest_rd32(s_coop_p[0] + 0x190) : 0;
        if (s_duels_on == 1 && s_duel_p1 && (d != s_duel_p1 || guest_rd32(d) == 6)) {
            /* Player 1's duel decided or gone: the others' end with it,
             * whoever shot first -- the story goes on from player 1's. */
            coop_duels_end();
            s_duels_on = 2;
        } else if (s_duels_on == 2 && in_heap(d) && guest_rd32(d) == 7) {
            s_duels_on = 0;                 /* the next duel starting */
        }
        if (s_duels_on)
            return;
    }
    s_duels_on = 1;
    d1 = in_heap(s_coop_p[0]) ? guest_rd32(s_coop_p[0] + 0x190) : 0;
    n = guest_rd32(mgr + 0x2C);
    for (mine = 0, i = 0; i < n && i < 16; i++)
        if (guest_rd32(mgr + 0x3C + 4 * i) == d1)
            mine = 1;
    fprintf(stderr, "[COOP] duels start: %u, player 1's %08X (%s), every player %s\n", n, d1,
            mine ? "playing" : "none", rdr_coop_duels() ? "on" : "off");
    fflush(stderr);
    if (!rdr_coop_duels() || !in_heap(d1) || !mine)
        return;
    opp = guest_rd32(d1 + 0x34);
    for (k = 1; k < s_coop_n && k < 4; k++) {
        uint32_t c = s_coop_p[k], obj;
        if (!in_heap(c) || guest_rd32(c) != 0x003D9FA8u || guest_rd32(c + 0x190))
            continue;
        guest_push(guest_rd32(d1 + 0x14));
        guest_push(0x000E7BA2u);
        g_ecx = mgr;
        sub_00095440();                 /* a duel from the manager, ret 4 */
        obj = g_eax;
        if (!in_heap(obj))
            break;
        coop_duel_set(c, obj);
        g_ecx = obj;
        guest_push(0x000955FEu);
        sub_000950A0();                 /* start it (plain ret) */
        if (!guest_rd32(obj + 0x34))
            guest_wr32(obj + 0x34, opp);
        guest_wr32(mgr + 0x84, guest_rd32(mgr + 0x2C));
        guest_wr32(mgr + 0x30, guest_rd32(mgr + 0x30) + 1);     /* the players' duels */
        s_coop_duel[k] = obj;
        s_duel_p1 = d1;
        fprintf(stderr, "[COOP] player %d duels too: %08X against %08X (player 1's against %08X)\n", k + 1,
                obj, guest_rd32(obj + 0x34), opp);
        fflush(stderr);
    }
    g_esp = esp0; g_ecx = ecx0; g_eax = eax0; g_edx = edx0;
}

static void player_update(void)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    int split = coop_split(), k;
    /* With a second player made, player 1's update ran twice a frame (the
     * second spawn puts the level's players in the update list again):
     * twice the speed, measured -- 2:1 updates against player 2. Once a
     * frame each. */
    if (s_coop_n >= 2) {
        for (k = 0; k < s_coop_n && k < 4; k++)
            if (s_coop_p[k] == g_ecx)
                break;
        if (k < s_coop_n && k < 4) {
            if (s_coop_updated[k] == s_coop_frame) {
                g_esp += 4;                     /* ret: already ran this frame */
                return;
            }
            s_coop_updated[k] = s_coop_frame;
        }
    }
    duel_mouse_aim(g_ecx);
    flier_black(g_ecx);
    town_guns_zones();
    coop_loadout_tick(g_ecx);
    {
        /* RDR_SPECIAL_DUMP=1: the special-move fields of the character being
         * updated, twice a second (finding what a special move waits for) */
        static int dump = -1;
        if (dump < 0)
            dump = getenv("RDR_SPECIAL_DUMP") != NULL;
        if (dump) {
            /* on any change: the button edges (+0x600..+0x610), the zone
             * bits that bar a special (+0xA04), the pad's buttons */
            static uint32_t last[10];
            uint32_t c = g_ecx, pad = guest_rd32(c + 0x894), v[10];
            v[0] = guest_rd32(c + 0x600); v[1] = guest_rd32(c + 0x608); v[2] = guest_rd32(c + 0x60C);
            v[3] = guest_rd32(c + 0x610); v[4] = guest_rd32(c + 0x620);
            v[5] = guest_rd32(c + 0xA04) ? guest_rd32(guest_rd32(c + 0xA04)) : 0xFFFFFFFFu;
            v[6] = pad ? guest_rd32(0x004041C4u + guest_rd32(pad + 4) * 0x38) : 0;
            v[7] = pad ? guest_rd32(pad + 0x70) : 0;
            v[8] = guest_rd32(c + 0xC) ? guest_rd32(guest_rd32(guest_rd32(c + 0xC) + 0x80) + 0xA8) : 0xFFFFFFFFu;
            {
                uint32_t w = guest_rd32(c + 0xC) ? guest_rd32(guest_rd32(c + 0xC) + 0x90) : 0;
                uint32_t f = w && !guest_rd8(w + 0x7C) ? guest_rd32(w + 0x28) : 0;
                v[9] = f ? guest_rd32(f + 0x19C) : 0xFFFFFFFFu;
            }
            {
                static uint32_t fn;
                if (v[9] >= 1 && v[9] <= 8 && (++fn % 4) == 0) {
                    uint32_t w = guest_rd32(guest_rd32(c + 0xC) + 0x90), f = guest_rd32(w + 0x28);
                    fprintf(stderr, "[SPECIAL] flying %d y=%.2f alt=%.2f target=%.2f rise=%.3f len=%.3f phase=%.2f\n",
                            (int)v[9], guest_rdf(c + 0x50), guest_rdf(f + 0x1EC), guest_rdf(f + 0x1B0),
                            guest_rdf(f + 0x1F4), guest_rdf(f + 0x1B8), guest_rdf(f + 0x1A0));
                }
            }
            if (memcmp(v, last, sizeof v)) {
                memcpy(last, v, sizeof v);
                fprintf(stderr, "[SPECIAL] %08X 600=%d 608=%d 60C=%d 610=%d 620=%d A04=%X pad=%08X mask=%08X A8=%d fly=%d y=%.2f\n",
                        c, (int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4], v[5], v[6], v[7], (int)v[8], (int)v[9],
                        guest_rdf(c + 0x50));
                if (v[1] == 2 && guest_rd32(c + 0xC)) {
                    /* the gates of sub_00143660 before CanDoSpecial */
                    uint32_t b = guest_rd32(c + 0xC), ctrl = guest_rd32(b + 0x80), brain = guest_rd32(b + 0x7C);
                    uint32_t w = guest_rd32(b + 0x90), ms = guest_rd32(c + 0x20);
                    char nm[40] = "";
                    for (int i = 0; i < 39; i++) {
                        nm[i] = (char)guest_rd8(ms + 0x274 + i);
                        if (!nm[i]) break;
                    }
                    fprintf(stderr, "[SPECIAL]   ctrl %08X A8=%d [40]+18=%08X brain %08X 20=%08X 14=%08X "
                            "mode %d W %08X 7C=%d 1DC=%X 28=%08X normal '%s'\n",
                            ctrl, (int)guest_rd32(ctrl + 0xA8),
                            guest_rd32(ctrl + 0x40) ? guest_rd32(guest_rd32(ctrl + 0x40) + 0x18) : 0xFFFFFFFFu,
                            brain, brain ? guest_rd32(brain + 0x20) : 0, brain ? guest_rd32(brain + 0x14) : 0,
                            (!guest_rd8(w + 0x7C) && guest_rd32(w + 0x48)) ? (int)guest_rd32(guest_rd32(w + 0x48) + 8) : -1,
                            w, guest_rd8(w + 0x7C), guest_rd32(w + 4) ? guest_rd32(guest_rd32(w + 4) + 0x1DC) : 0,
                            guest_rd32(w + 0x28), nm);
                }
            }
        }
        if (0) {
            uint32_t c = g_ecx;
#define SPM32(a) (*(volatile uint32_t *)(m + (a)))
#define SPM8(a) (*(volatile uint8_t *)(m + (a)))
            fprintf(stderr, "[SPECIAL] %08X 600=%d 608=%d 60C=%d 610=%d 620=%d C=%08X 648=%d 700=%d 6A8=%d\n",
                    c, (int)SPM32(c + 0x600), (int)SPM32(c + 0x608), (int)SPM32(c + 0x60C),
                    (int)SPM32(c + 0x610), (int)SPM32(c + 0x620), SPM32(c + 0xC), (int)SPM32(c + 0x648),
                    SPM8(c + 0x700), SPM8(c + 0x6A8));
        }
    }
    if (s_coop_n >= 2) {
        /* Extra players are put beside player 1 (coop_put_beside):
         *  - after the opening: a level's script can move player 1 away from
         *    its start point, where sub_00137370 (rdr_hooks.c) put player 2.
         *    Checked once, at the first update of play (no cutscene) after
         *    the level starts or a script moves player 1 -- a jump of more
         *    than 6 units in one update during a cutscene or within 3
         *    seconds (180 updates) of one. (Checked once at the start only,
         *    it missed on the user's PC: the check came between two parts
         *    of chapter 1's opening, before the script moved player 1.
         *    Checked for 600 updates after any jump, it pulled player 2
         *    back whenever it walked 8 m off in the first 10 seconds of a
         *    level, and on the train -- where a character's place is its
         *    car's, and changes by a car's length as it steps across --
         *    every time player 1 crossed to another car: "rubber banding",
         *    user video, Brimstone and chapter 3's train.);
         *  - when one has been dead for 4 seconds: revived (the Showdown
         *    respawn's steps, sub_0006D000) and put beside player 1. Player
         *    1's death is the story's (game over, restart). */
        static uint32_t grace, lvl, since_cut = 100000;
        static float last[3];
        static float dead_t[4], fall_t[4], idle_t[4];
        uint32_t p1 = s_coop_p[0];
        if (lvl != s_coop_level) {
            lvl = s_coop_level;
            grace = 0;
            memset(dead_t, 0, sizeof dead_t);
            memset(fall_t, 0, sizeof fall_t);
            memset(idle_t, 0, sizeof idle_t);
            s_trail_n = 0;
            last[0] = guest_rdf(p1 + 0x4C); last[1] = guest_rdf(p1 + 0x50); last[2] = guest_rdf(p1 + 0x54);
        }
        if (g_ecx == p1 && coop_health(p1) > 0.0f && !guest_rd8(RDR_CINEMA))
            coop_trail_note(p1);
        if (g_ecx == p1) {
            float x = guest_rdf(p1 + 0x4C), y = guest_rdf(p1 + 0x50), z = guest_rdf(p1 + 0x54);
            float dx = x - last[0], dy = y - last[1], dz = z - last[2];
            if (guest_rd8(RDR_CINEMA))
                since_cut = 0;
            else if (since_cut < 100000)
                since_cut++;
            if (dx * dx + dy * dy + dz * dz > 6.0f * 6.0f && since_cut < 180) {
                s_coop_place = ((1 << s_coop_n) - 1) & ~1;
                grace = 0;
            }
            last[0] = x; last[1] = y; last[2] = z;
            if (!guest_rd8(RDR_CINEMA) && grace < 100000)
                grace++;
        }
        for (k = 1; k < s_coop_n && k < 4; k++) {
            uint32_t p = s_coop_p[k];
            if (p != g_ecx)
                continue;
            if (!guest_rd8(RDR_CINEMA) && grace > 30 && (s_coop_place >> k & 1)) {
                if (coop_dist2(p, p1) > 8.0f * 8.0f || s_coop_place_near)
                    coop_put_beside(k, p, p1, "moved");
                s_coop_place &= ~(1 << k);      /* once */
                if (!s_coop_place)
                    s_coop_place_near = 0;
            }
            /* Nobody on this player's port (no controller there, or it was
             * unplugged): the character would stand where it was left, and
             * enemies took it for a target there -- they stood about near
             * player 1, neither moving nor shooting (user video, three
             * players with one controller). Brought along behind player 1
             * when it is left 15 m behind. Not when a controller was there
             * and has gone to sleep or been unplugged: that player stays
             * where they were left (user: "Brimstone teleporting the
             * second player back" -- the log had the DualSense switch
             * itself off while player 2 stood waiting). */
            if (!rdr_port_ever(k) && coop_health(p) > 0.0f && !guest_rd8(RDR_CINEMA)
                && coop_dist2(p, p1) > 15.0f * 15.0f) {
                idle_t[k] += guest_rdf(0x00406504u);
                if (idle_t[k] >= 2.0f) {
                    coop_put_beside(k, p, p1, "has nobody on its port; brought along");
                    idle_t[k] = 0.0f;
                }
            } else {
                idle_t[k] = 0.0f;
            }
            /* Fallen out of the level (far below player 1 for a second):
             * back beside it. */
            if (coop_health(p) > 0.0f && !guest_rd8(RDR_CINEMA)
                && guest_rdf(p + 0x50) < guest_rdf(p1 + 0x50) - 12.0f) {
                fall_t[k] += guest_rdf(0x00406504u);
                if (fall_t[k] >= 1.0f) {
                    coop_put_beside(k, p, p1, "fell; put back");
                    fall_t[k] = 0.0f;
                }
            } else {
                fall_t[k] = 0.0f;
            }
            if (coop_health(p) <= 0.0f && coop_health(p1) > 0.0f && !guest_rd8(RDR_CINEMA)) {
                dead_t[k] += guest_rdf(0x00406504u);
                if (dead_t[k] >= 4.0f) {
                    coop_revive(p);
                    coop_put_beside(k, p, p1, "revived");
                    dead_t[k] = 0.0f;
                }
            } else {
                dead_t[k] = 0.0f;
            }
        }
    }
    powerups_apply(g_ecx);
    {
        /* Tests (RDR_COOP_LOG=1): updates per player per second. */
        static int on = -1;
        static uint32_t who[4], cnt[4];
        static float lastp[4][3];
        static double dist[4];
        static double t0;
        if (on < 0) {
            const char *e = getenv("RDR_COOP_LOG");
            on = e && *e == '1';
        }
        if (on) {
            double t = host_seconds();
            for (k = 0; k < 4; k++)
                if (who[k] == g_ecx || !who[k]) {
                    float x = guest_rdf(g_ecx + 0x4C), y = guest_rdf(g_ecx + 0x50), z = guest_rdf(g_ecx + 0x54);
                    if (who[k] == g_ecx) {
                        double dx = x - lastp[k][0], dz = z - lastp[k][2];
                        dist[k] += sqrt(dx * dx + dz * dz);
                    }
                    lastp[k][0] = x; lastp[k][1] = y; lastp[k][2] = z;
                    who[k] = g_ecx;
                    cnt[k]++;
                    break;
                }
            if (t0 == 0.0)
                t0 = t;
            if (t - t0 >= 5.0) {
                fprintf(stderr, "[COOP] updates in %.1f s: %08X %u (moved %.2f), %08X %u (moved %.2f) (frame time %.4f, views %u)\n",
                        t - t0, who[0], cnt[0], dist[0], who[1], cnt[1], dist[1],
                        guest_rdf(0x00406504u), guest_rd32(RDR_VIEW_COUNT));
                memset(dist, 0, sizeof dist);
                fflush(stderr);
                memset(who, 0, sizeof who);
                memset(cnt, 0, sizeof cnt);
                t0 = t;
            }
        }
    }
    if (s_coop_n >= 2) {
        /* Player k reads controller port k, player 1 included. The story
         * gives player 1 the port that pressed START at the title screen
         * (the menus' controller); when that was player 2's controller,
         * both characters read it and one controller moved both players
         * (user report, "occasionally": whenever the pad started the
         * game). Checked every update, so a restart or anything else that
         * re-assigns pads is put right too. */
        static int said;
        for (k = 0; k < s_coop_n && k < 4; k++)
            if (s_coop_p[k] && guest_rd32(s_coop_p[k] + 0x894) != RDR_PADS + 0xB0u * (uint32_t)k) {
                if (said++ < 8)
                    fprintf(stderr, "[COOP] player %d read pad %08X; now port %d's\n", k + 1,
                            guest_rd32(s_coop_p[k] + 0x894), k + 1);
                *(volatile uint32_t *)(m + s_coop_p[k] + 0x894) = RDR_PADS + 0xB0u * (uint32_t)k;
            }
    }
    if (s_coop_restore_count) {
        s_coop_restore_count = 0;
        if (!split) {
            *(volatile uint32_t *)(m + 0x00652E54u) = 1;
            *(volatile uint32_t *)(m + RDR_VIEW_COUNT) = 1;
            fprintf(stderr, "[COOP] %d players, one screen\n", s_coop_n);
        } else {
            fprintf(stderr, "[COOP] %d players, split screen (%s), %u views\n", s_coop_n,
                    s_coop_n > 2 ? "quarters" : split == 2 ? "left/right" : "top/bottom",
                    guest_rd32(RDR_VIEW_COUNT));
        }
        fflush(stderr);
    }
    {
        /* The combo's money is paid in here: the sparkle at this player's
         * money box (money_spot_take). */
        uint32_t player = g_ecx;
        float keep[2];
        int took, who = s_pay_player;
        if (who >= 0 && who < 4 && (s_coop_n >= 2 ? s_coop_p[who] == player : who == 0)) {
            s_pay_player = -1;
            guest_push(0);                  /* AddMoney(amount, showdown, quiet) */
            guest_push(0);
            guest_push((uint32_t)s_pay_amount);
            guest_push(0x0014A7BEu);
            player_add_money();             /* ret 0xC */
            g_ecx = player;
        }
        if (s_npc_hurt && (s_coop_n < 2 || s_coop_p[0] == player))
            npc_hurt_now(player);
        if (s_coop_n < 2 || s_coop_p[0] == player) {
            if (s_npc_req)
                npc_run();
            npc_keep_tick();
        }
        if (s_player_hurt && s_player_hurt_k >= 1 && s_player_hurt_k <= 4
            && guest_rd32(0x00675930u + 4u * (uint32_t)(s_player_hurt_k - 1)) == player)
            player_hurt_now(player);
        if (s_ray_test && (s_coop_n < 2 || s_coop_p[0] == player))
            ray_test_now(player);
        coop_brain_keep(player);
        if (s_tp_player >= 1 && s_tp_player <= 4
            && guest_rd32(0x00675930u + 4u * (uint32_t)(s_tp_player - 1)) == player) {
            float at[3] = { s_tp_at[0], s_tp_at[1], s_tp_at[2] };
            s_tp_player = 0;
            coop_place(player, player, at);
            fprintf(stderr, "[TEST] tp: %08X now %.2f %.2f %.2f\n", player, guest_rdf(player + 0x4C),
                    guest_rdf(player + 0x50), guest_rdf(player + 0x54));
            fflush(stderr);
        }
        took = money_spot_take(player, keep);
        sub_0014A0B0();
        if (took)
            money_spot_give_back(keep);
    }
}

void rdr_coop_level_start(void)
{
    uint8_t saved[0x5C];
    uint32_t main1, main2, main3, out, i, frame;
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    int n = coop_players(), k;
    if (guest_rd32(0x00652E30u) == 0)
        coop_signature(s_coop_made_sig, sizeof s_coop_made_sig);
    if (getenv("RDR_COOP_LOG")) {
        fprintf(stderr, "[COOP] level start: %d wanted, characters %u, local %u, co-op %s\n", n,
                guest_rd32(RDR_PLAYER_COUNT), guest_rd32(0x00652E54u), s_coop_made_sig);
        fflush(stderr);
    }
    memset(s_coop_p, 0, sizeof s_coop_p);
    memset(s_coop_views, 0, sizeof s_coop_views);
    s_coop_n = 0; s_coop_kept = 0;
    if (s_coop_cleared) {
        *(volatile uint8_t *)(m + RDR_CLEAR_ONCE) = 0;   /* as the story had it */
        s_coop_cleared = 0;
    }
    if (n < 2 || guest_rd32(0x00652E30u) != 0 || guest_rd32(RDR_PLAYER_COUNT) != 1)
        return;
    for (i = 0; i < 0x5C; i++)
        saved[i] = m[RDR_PLAYER_DESC + i];
    main1 = guest_rd32(0x00652E38u);
    main2 = guest_rd32(0x00652E4Cu);
    main3 = guest_rd32(0x00652E28u);
    frame = g_esp + 16;     /* the level set-up's frame (see below) */
    s_coop_p[0] = guest_rd32(0x00675930u);      /* player 1's character */
    s_coop_level = guest_rd32(RDR_LEVEL);
    for (k = 1; k < n; k++) {
        /* Player slot k has never been set up in the story: start it as a
         * copy of slot 1 (the spawn gives it its number). */
        if (k == 1 && !s_coop_saved_n) {
            int j;
            for (j = 1; j < n && j < 4; j++)
                for (i = 0; i < 0x148; i++)
                    s_coop_slot_saved[j][i] = m[0x00675968u + 0x148u * (uint32_t)j + i];
            s_coop_local_saved = guest_rd32(0x00652E54u);
            s_coop_saved_n = n < 4 ? n : 4;
        }
        if (s_coop_copy_slot) {
            for (i = 0; i < 0x148; i++)
                m[0x00675968u + 0x148u * (uint32_t)k + i] = m[0x00675968u + i];
            /* But not slot 1's characters (+0x118, the list the spawn adds
             * to, and its counts): copied, player 2's slot drove player 1
             * as well as its own -- player 1 ran and animated twice as
             * fast (user report; the slot read [p1, p2]). */
            for (i = 0x118; i < 0x140; i++)
                m[0x00675968u + 0x148u * (uint32_t)k + i] = 0;
        }
        *(volatile uint32_t *)(m + RDR_PLAYER_DESC) = (uint32_t)k;
        /* Each player its own character (F1 > Cheats > Story co-op): the
         * type name put back to player 1's, then the spawn's Play-as hook
         * (rdr_hooks.c) writes this player's choice over it. */
        for (i = 4; i < 0x18; i++)
            m[RDR_PLAYER_DESC + i] = saved[i];
        *(volatile uint8_t *)(m + RDR_PLAYER_DESC + 0x18) = 0;    /* no second character */
        {
            const char *want = rdr_coop_char(k);
            rdr_play_as_spawn(want && *want ? want : NULL);
        }
        /* Room on the guest stack for the two arrays it fills. */
        g_esp -= 0x40;
        out = g_esp;
        for (i = 0; i < 0x40; i += 4)
            *(volatile uint32_t *)(m + out + i) = 0;
        g_esp -= 4; *(volatile uint32_t *)(m + g_esp) = out + 0x20;   /* second characters */
        g_esp -= 4; *(volatile uint32_t *)(m + g_esp) = out;          /* players */
        g_esp -= 4; *(volatile uint32_t *)(m + g_esp) = 0x00062BB9u;  /* as from its caller */
        {
            /* The level's player brain -- its .enm's PlayerBrain, kept in
             * the parsed enemy file [0x0065A998] at +0x30 (64 chars) --
             * runs the story's script: the opening cutscene among it. Made
             * with it, player 2 played the opening again after player 1's
             * (user report). Extra players get
             * playerbrain_multipartner_tester (swat.dat: one state, no
             * conditions, no actions), so the pad drives them and no script
             * runs; the level's is put back right after. (hero_playerAI is
             * as empty but its state sets "nofire", and with it player 2
             * did not move; Multiplayer_Player_Brain runs Showdown's
             * last-man-standing and death-camera states.)
             * Not BR_playerAI, used before: that is Brimstone's (the town
             * hub's) player brain, and its start state plays the town's
             * cutscenes by level variables -- in every co-op level player
             * 2 played Sheriff Bartlett's "I got your telegram" scene
             * (lvl_tomb_pupshow_01) right after the level's own opening
             * (user report; RDR_PUP_LOG names each cutscene loaded). */
            uint32_t enm = guest_rd32(0x0065A998u);
            uint8_t brain[0x40];
            const char *br = getenv("RDR_COOP_BRAIN");   /* tests */
            int have = in_heap(enm) && !getenv("RDR_COOP_SAME_BRAIN");
            if (!br || !*br || strlen(br) > 0x3F)
                br = "playerbrain_multipartner_tester";
            if (have) {
                for (i = 0; i < 0x40; i++) brain[i] = m[enm + 0x30 + i];
                for (i = 0; i <= strlen(br); i++) m[enm + 0x30 + i] = (uint8_t)br[i];
                fprintf(stderr, "[COOP] player %d: brain %.63s -> %s\n", k + 1, (const char *)brain, br);
            }
            sub_00138090();                     /* plain ret: the caller pops */
            rdr_play_as_spawn(NULL);
            if (have)
                for (i = 0; i < 0x40; i++) m[enm + 0x30 + i] = brain[i];
        }
        g_esp += 8 + 0x40;
        {
            /* The spawn puts the new character second in the slot's list
             * (+0x11C) -- after the one the slot already had, which in a
             * copy of slot 1 was player 1 (both players then followed both
             * controllers). Emptied first, the list read [0, p2]: player
             * 2's controller still reached player 1 through the empty
             * first entry, the players mirrored each other (user report).
             * So the new character goes first, alone. */
            uint32_t sl = 0x00675968u + 0x148u * (uint32_t)k;
            uint32_t c0 = guest_rd32(sl + 0x118), c1 = guest_rd32(sl + 0x11C);
            if (!c0 && c1) {
                guest_wr32(sl + 0x118, c1);
                guest_wr32(sl + 0x11C, 0);
            }
            fprintf(stderr, "[COOP] slot %d after spawn: chars %08X %08X (were %08X %08X), %u %u\n", k + 1,
                    guest_rd32(sl + 0x118), guest_rd32(sl + 0x11C), c0, c1,
                    guest_rd32(sl + 0x138), guest_rd32(sl + 0x13C));
        }
        if (!guest_rd32(out))
            break;
        /* The level's set-up (sub_00062xxx, our caller's caller) goes on to
         * give each of its [0x00652E54] local players a HUD and a view from
         * the arrays sub_00138090 filled -- [esp+0x78] and [esp+0x88] in
         * its frame, 16 bytes above the return address this hook was
         * entered with. */
        *(volatile uint32_t *)(m + frame + 0x78 + 4u * (uint32_t)k) = guest_rd32(out);
        *(volatile uint32_t *)(m + frame + 0x88 + 4u * (uint32_t)k) = guest_rd32(out + 0x20);
        s_coop_p[k] = guest_rd32(out);
        s_coop_sm[k] = guest_rd32(s_coop_p[k] + 0x158);
        s_coop_n = k + 1;
    }
    for (i = 0; i < 0x5C; i++)
        m[RDR_PLAYER_DESC + i] = saved[i];
    *(volatile uint32_t *)(m + 0x00652E38u) = main1;
    *(volatile uint32_t *)(m + 0x00652E4Cu) = main2;
    *(volatile uint32_t *)(m + 0x00652E28u) = main3;
    if (s_coop_n < 2) {
        s_coop_n = 0;
        return;
    }
    *(volatile uint32_t *)(m + 0x00652E54u) = (uint32_t)s_coop_n;
    s_coop_restore_count = 1;
    /* Once the opening is over, extra players more than 8 units from
     * player 1 are moved beside it (player_update): a level's script can
     * take player 1 away from its start point, where sub_00137370 (the
     * hook in rdr_hooks.c) put player 2. RDR_COOP_NO_PLACE=1: not. */
    s_coop_place = getenv("RDR_COOP_NO_PLACE") ? 0 : ((1 << s_coop_n) - 1) & ~1;
    fprintf(stderr, "[COOP] %d players: %08X %08X %08X %08X (characters now %u)\n", s_coop_n,
            s_coop_p[0], s_coop_p[1], s_coop_p[2], s_coop_p[3], guest_rd32(RDR_PLAYER_COUNT));
    fflush(stderr);
}


/* RDR_SPECIAL_DUMP=1: the special-move path logged (what Black waits for) */
extern void sub_00139780(void);
extern void sub_0013E8A0(void);
extern void sub_002324F0(void);
extern void sub_00143370(void);
static void logged_00139780(void)
{
    uint32_t c = g_ecx, cont = guest_rd32(c + 0xC) ? guest_rd32(guest_rd32(c + 0xC) + 0x80) : 0;
    static uint32_t n;
    if ((++n % 30) == 0)
        fprintf(stderr, "[SPECIAL] try %08X cont %08X A8=%d 48=%d\n", c, cont,
                cont ? (int)guest_rd32(cont + 0xA8) : -1, cont ? (int)guest_rd32(cont + 0x48) : -1);
    sub_00139780();
}
static void logged_0013E8A0(void)
{
    uint32_t idx = guest_rd32(g_esp + 4);
    sub_0013E8A0();
    fprintf(stderr, "[SPECIAL] can %d -> %d\n", (int)idx, (int)(g_eax & 0xFF));
}
static void logged_002324F0(void)
{
    uint32_t a = guest_rd32(g_esp + 4), b = guest_rd32(g_esp + 8);
    sub_002324F0();
    if (g_eax)
        fprintf(stderr, "[SPECIAL] button %d,%d -> %d\n", (int)a, (int)b, (int)g_eax);
}
static void logged_00143370(void)
{
    fprintf(stderr, "[SPECIAL] do %d\n", (int)guest_rd32(g_esp + 4));
    sub_00143370();
}


/* ── The desk's Online Co-op (where the Xbox had its Friends Menu) ──────
 *
 * The desk's prompt page (roominstr.swui: a class whose update,
 * sub_00039840, is vtable 0x003C91D0 +8) shows "Y Friends Menu" top right
 * (the text at 0x003C9224, its readout at [page+0x454]); Y opened an empty
 * Xbox Live page. The words become "Online Co-op" (as long as the old, in
 * place), and Y there opens the F1 menu's Online Co-op page (the lobby,
 * src/net/rdr_lobby.c) instead -- the game does not see that Y. Not in a
 * game started from the lobby: there the pads are everyone's. */
extern void sub_00039840(void);
static volatile uint64_t s_room_ns;
static volatile uint32_t s_room_page;
extern int rdr_lockstep_mode(void);
extern uint64_t xbox_HostNowNs(void);
/* (at start: the page's readout takes its words once, when the desk is
 * first built -- renamed in its update, a new profile's desk kept "Friends
 * Menu") */
static void room_rename(void)
{
    static const char t[] = "Online Co-op";            /* 12, as "Friends Menu" */
    uint32_t k;
    char old[13];
    guest_str(0x003C9224u, old, sizeof old);
    if (!strcmp(old, "Friends Menu"))
        for (k = 0; k < sizeof t; k++)
            *(volatile char *)((uintptr_t)xbox_GetMemoryOffset() + 0x003C9224u + k) = t[k];
}

static void room_update(void)
{
    s_room_page = g_ecx;
    s_room_ns = xbox_HostNowNs();
    sub_00039840();
}

/* The pointer over the prompt (menus' 640x448 space): the readout's words
 * (about 150 wide) and the Y glyph left of them. */
static int desk_coop_hit(float px, float py, int dbg)
{
    uint32_t page = s_room_page, txt;
    float x, y;
    if (rdr_lockstep_mode() || !s_room_ns || xbox_HostNowNs() - s_room_ns > 200000000ull)
        return 0;
    txt = in_heap(page) ? guest_rd32(page + 0x454) : 0;
    if (!in_heap(txt) || !guest_rd8(txt + 0xE6))
        return 0;
    x = wx(txt);
    y = wy(txt);
    if (dbg) {
        static int once;
        if (!once++) {
            fprintf(stderr, "[MENU] desk Online Co-op prompt at %.0f,%.0f\n", x, y);
            fflush(stderr);
        }
    }
    return px >= x - 45.0f && px <= x + 165.0f && py >= y - 8.0f && py <= y + 32.0f;
}

/* The game's menu sounds: sub_0009B400 (thiscall on the sound player
 * [0x65B944], an index into the menus' sound table at 0x003D1150) is what
 * every swui page's Move / Accept / Back slots call -- the front end's 2,
 * 1 and 3 (sub_000196B0/C0/D0), the pause page's own 5 and 4. Found by
 * breaking on the voice allocation as the pause menu's cursor moved. */
static volatile int s_ui_sound;
void rdr_ui_sound(int which)
{
    s_ui_sound = which;
}

static void ui_sound_run(void)
{
    uint32_t sp = guest_rd32(0x0065B944u), esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, edx0 = g_edx;
    int k = s_ui_sound;
    s_ui_sound = 0;
    if (getenv("RDR_UI_DEBUG"))
        fprintf(stderr, "[UI] sound %d (player %08X)\n", k, sp);
    if (sp < 0x10000u || (sp >= 0x04000000u && !in_heap(sp)) || k <= 0 || k > 15)
        return;                         /* (the player is a static: 0x00578960) */
    guest_push((uint32_t)k);
    guest_push(0x000196BDu);            /* as from the front end's own */
    g_ecx = sp;
    sub_0009B400();
    g_esp = esp0; g_ecx = ecx0; g_eax = eax0; g_edx = edx0;
}

void rdr_desk_pad(uint32_t st)
{
    static int down;
    if (s_ui_sound)
        ui_sound_run();
    if (s_cmd_local_n > 0 && rdr_in_level())
        cmd_local_run();                /* (the game's thread, as it reads the pads) */
    extern int rdr_lockstep_mode(void);
    extern void rdr_overlay_show(int what);
    uint32_t page = s_room_page, txt;
    uint8_t y;
    if (rdr_lockstep_mode() || !s_room_ns || xbox_HostNowNs() - s_room_ns > 200000000ull)
        return;
    txt = in_heap(page) ? guest_rd32(page + 0x454) : 0;
    if (!in_heap(txt) || !guest_rd8(txt + 0xE6))
        return;                                 /* the prompt is not showing */
    y = guest_rd8(st + 9);                      /* XINPUT_STATE: the analog Y */
    if (y > 30) {
        static uint64_t opened;                 /* (read for each pad: once a press) */
        uint64_t now = xbox_HostNowNs();
        if (!down && now - opened > 600000000ull) {
            opened = now;
            fprintf(stderr, "[MENU] desk: Online Co-op\n");
            fflush(stderr);
            rdr_overlay_show(4);
        }
        down = 1;
        *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + st + 9) = 0;
    } else {
        down = 0;
    }
}

/* Tests (RDR_RAY_LOG): the collision world's ray test (vtable +0xDC of
 * [0x00661D44], sub_000C90A0) as the bullets' sub_0014DEF0 and its kin call
 * it: the arguments (the masks) and the answer. */
extern void sub_000C90A0(void);
static void logged_ray(void)
{
    uint32_t ret = guest_rd32(g_esp), a[6], k;
    for (k = 0; k < 6; k++)
        a[k] = guest_rd32(g_esp + 4 + k * 4);
    sub_000C90A0();
    if (ret >= 0x0014D000u && ret < 0x00150000u) {
        uint32_t res = a[0], hit = in_heap(res) ? guest_rd32(res) : 0;
        fprintf(stderr, "[RAY] from %08X args %08X %08X %08X %08X %08X %08X -> %u, hit %08X (vt %08X)\n", ret,
                a[0], a[1], a[2], a[3], a[4], a[5], g_eax & 0xFF, hit, in_heap(hit) ? guest_rd32(hit) : 0);
        if (g_eax & 0xFF) {
            for (k = 0; k < 2; k++) {
                uint32_t j;
                fprintf(stderr, "   a%u:", k);
                for (j = 0; j < 12; j++)
                    fprintf(stderr, " %08X", guest_rd32(a[k] + j * 4));
                fprintf(stderr, "\n");
            }
            {
                uint32_t i0 = guest_rd32(a[1]), j;
                if (in_heap(i0)) {
                    fprintf(stderr, "   inst %08X:", i0);
                    for (j = 0; j < 16; j++)
                        fprintf(stderr, " %08X", guest_rd32(i0 + j * 4));
                    fprintf(stderr, "\n");
                }
            }
        }
        fflush(stderr);
    }
}

/* Tests (RDR_REL_LOG=1): the characters' relation questions (vtable
 * +0x38, +0x3C, +0x40: sub_0008F9E0, sub_0008FAD0, sub_0008F9A0; the
 * teams' table bits 1, 2 and 0) asked about two players: who asks, and
 * the answer. */
extern void sub_0008F9E0(void);
extern void sub_0008FAD0(void);
extern void sub_0008F9A0(void);
static void rel_log(int which, uint32_t self, uint32_t other, uint32_t ret)
{
    static uint32_t seen[64];
    static int n;
    uint32_t key = ret ^ (uint32_t)which << 28;
    int k;
    if (coop_index(self) < 0 || coop_index(other) < 0)
        return;
    for (k = 0; k < n; k++)
        if (seen[k] == key)
            return;
    if (n < 64)
        seen[n++] = key;
    fprintf(stderr, "[REL] +0x%X asked from %08X: player %d about player %d -> %u\n", 0x38 + 4 * which, ret,
            coop_index(self) + 1, coop_index(other) + 1, g_eax & 0xFF);
    fflush(stderr);
}
#define REL_WRAP(name, idx, fn) static void name(void) \
    { uint32_t self = g_ecx, other = guest_rd32(g_esp + 4), ret = guest_rd32(g_esp); \
      fn(); rel_log(idx, self, other, ret); }
REL_WRAP(rel_hurt, 0, sub_0008F9E0)
REL_WRAP(rel_aim, 1, sub_0008FAD0)
REL_WRAP(rel_enemy, 2, sub_0008F9A0)

/* Story co-op: the players are not each other's targets (user: "the game
 * still treats the co-op player as a target and will lightly lock onto
 * them"). The characters' "may aim at" question (vtable +0x3C,
 * sub_0008FAD0: teams' table bit 2; two players answer yes when their
 * sub_0008B580 sides differ, as in Showdown) is asked both ways for each
 * character by the aim's candidate search (sub_001B0A80, 0x001B0B75 and
 * 0x001B0B81 -- RDR_REL_LOG). Two co-op players now answer no, friendly
 * fire on or off: a bullet still meets the other player (and hurts with
 * friendly fire on), only the aim no longer seeks them. Showdown, with no
 * story co-op, is as the game has it. */
static void coop_may_aim(void)
{
    uint32_t self = g_ecx, other = guest_rd32(g_esp + 4);
    static int old = -1;
    sub_0008FAD0();
    if (old < 0)
        old = getenv("RDR_COOP_AIM_OLD") != NULL;
    if (!old && self != other && coop_index(self) >= 0 && coop_index(other) >= 0)
        g_eax = 0;
}

recomp_func_t rdr_lookup_override(uint32_t xbox_va)
{
    if (xbox_va == 0x0008FAD0u && s_coop_n >= 2)
        return coop_may_aim;
    if (xbox_va >= 0x000D3FE0u && xbox_va <= 0x000D5020u) {
        extern recomp_func_t rdr_npc_override(uint32_t va);
        recomp_func_t f = rdr_npc_override(xbox_va);
        if (f)
            return f;
    }
    {
        static int rl = -1;
        if (rl < 0)
            rl = getenv("RDR_REL_LOG") != NULL;
        if (rl) {
            if (xbox_va == 0x0008F9E0u) return rel_hurt;
            if (xbox_va == 0x0008FAD0u) return rel_aim;
            if (xbox_va == 0x0008F9A0u) return rel_enemy;
        }
    }
    if (s_ic_on) {
        if (s_ic_on < 0) {
            const char *e = getenv("RDR_ICALL_PROFILE");
            s_ic_on = e && *e == '1';
        }
        if (s_ic_on)
            icall_count(xbox_va);
    }
    static int counting = -1;
    if (counting < 0) {
        const char *e = getenv("RDR_COUNT_CALLS");
        counting = e && *e == '1';
    }
    if (counting) {
#define COUNT_CASE(a) if (xbox_va == 0x##a##u) return counted_##a;
        COUNTED(COUNT_CASE)
    }
    if (xbox_va == 0x00039840u)
        return room_update;
    if (xbox_va == 0x000C90A0u && getenv("RDR_RAY_LOG"))
        return logged_ray;
    if (xbox_va == 0x002730A0u)
        return wrapped_get_channel;
    if (xbox_va == 0x0014A0B0u)
        return player_update;
    if (xbox_va == 0x001D98D0u)
        return hud_draw;
    if (xbox_va == 0x001D90E0u)
        return hud_update;
    if (xbox_va == 0x0013E2F0u)
        return player_add_money;
    if (xbox_va == 0x0013C330u)
        return player_take_damage;
    if (xbox_va == 0x001394B0u)
        return char_take_damage;
    if (xbox_va == 0x00091500u)
        return char_take_damage2;
    if (xbox_va == 0x00076560u)
        return char_take_damage3;
    if (xbox_va == 0x001DF140u)
        return pad_model_draw;
    {
        static int sp = -1;
        if (sp < 0)
            sp = getenv("RDR_SPECIAL_DUMP") != NULL;
        if (sp) {
            if (xbox_va == 0x00139780u) return logged_00139780;
            if (xbox_va == 0x0013E8A0u) return logged_0013E8A0;
            if (xbox_va == 0x002324F0u) return logged_002324F0;
            if (xbox_va == 0x00143370u) return logged_00143370;
        }
    }
    if (xbox_va == 0x000801C0u && getenv("RDR_ZONE_LOG"))
        return zone_load_logged;
    if (xbox_va == 0x001BE240u && getenv("RDR_ZONE_LOG"))
        return lod_set_logged;
    if (xbox_va == 0x000803F0u && getenv("RDR_ZONE_LOG"))
        return zone_update_logged;
    if (xbox_va == 0x0008AE60u)
        return frozen_0008AE60;
#define NPC_CASE(a) if (xbox_va == 0x##a##u) return frozen_##a;
    NPC_UPDATES(NPC_CASE)
#define PAGE_CASE(a) if (xbox_va == 0x##a##u) return wrapped_page_##a;
    PAGE_UPDATES(PAGE_CASE)
    if (xbox_va == 0x003BFAA2u)
        return guarded_xid_report;
    return NULL;
}

/* ── D3D fence semaphore ─────────────────────────────────────────
 *
 * XDK 5849 D3D keeps its device in .data: [0x00347208] -> 0x00347210.
 * device+0x2C is the submit counter, device+0x30 points at a 4 KB
 * contiguous block (allocated in D3D init, 0x80001000 in practice) that the
 * GPU writes its completed count into via BACK_END_WRITE_SEMAPHORE_RELEASE
 * (0x1D70). D3D's two-frames-ahead throttle (sub_0033CCE0 ->
 * sub_0033C9D0, the loop at 0x0033CB07) spins on that word.
 *
 * The pushbuffer executor writes 0x1D70's value only to the address given to
 * nv2a_pb_set_semaphore_target(). Without it the word never moves and the
 * game freezes in the throttle on the first frame Bink submits (the Rockstar
 * logo). The block is allocated at run time, so read it from the device once
 * D3D has filled it in rather than hard-coding the address.
 */
#define RDR_D3D_DEVICE_PTR   0x00347208u
#define RDR_D3D_FENCE_FIELD  0x30u

static uint32_t guest_rd32(uint32_t va)
{
    return *(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static int bind_fence_once(void)
{
    uint32_t dev = guest_rd32(RDR_D3D_DEVICE_PTR);
    uint32_t fence;

    if (dev < 0x00010000u || dev >= 0x04000000u)
        return 0;
    fence = guest_rd32(dev + RDR_D3D_FENCE_FIELD);
    if (fence < 0x80000000u || fence >= 0x84000000u)   /* contiguous window */
        return 0;
    nv2a_pb_set_semaphore_target(fence);
    fprintf(stderr, "[RDR] D3D fence semaphore bound: device 0x%08X, fence word 0x%08X\n",
            dev, fence);
    return 1;
}

#if defined(_WIN32)
static DWORD WINAPI fence_binder(LPVOID unused)
#else
static void *fence_binder(void *unused)
#endif
{
    int i;
    (void)unused;
    /* D3D init runs within the first seconds; give up after a minute. */
    for (i = 0; i < 6000; i++) {
        if (bind_fence_once())
            break;
        sleep_ms(10);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* ── crash diagnostics ───────────────────────────────────────────
 *
 * datResourceHeader's chunk translators are a singly linked list at
 * [0x0065ABB8]: node { u32 tag ('MIP0'...), fn, ?, next @ +0x0C }.
 * sub_00083F50 walks it for each chunk of a loaded resource. Printed from
 * the crash handler so a bad link shows where it is. */
#define RDR_RSC_HANDLERS 0x0065ABB8u

static int guest_ok(uint32_t va)
{
    return (va >= 0x00010000u && va < 0x04000000u)
        || (va >= 0x80000000u && va < 0x84000000u);
}

void rdr_crash_diagnostics(void)
{
    uint32_t node = guest_rd32(RDR_RSC_HANDLERS);
    int i;

    fprintf(stderr, "  [RDR] resource translators at [0x%08X]:\n", RDR_RSC_HANDLERS);
    for (i = 0; i < 64 && node; i++) {
        uint32_t tag, fn, next;
        if (!guest_ok(node)) {
            fprintf(stderr, "    #%d node 0x%08X  <-- not a valid guest address\n", i, node);
            return;
        }
        tag = guest_rd32(node);
        fn = guest_rd32(node + 4);
        next = guest_rd32(node + 12);
        fprintf(stderr, "    #%d node 0x%08X tag %c%c%c%c fn 0x%08X next 0x%08X\n", i, node,
                (int)(tag & 0xFF), (int)((tag >> 8) & 0xFF),
                (int)((tag >> 16) & 0xFF), (int)(tag >> 24), fn, next);
        node = next;
    }
}

/* ── D3D software methods ────────────────────────────────────
 *
 * When D3D finds the GPU far behind the CPU (sub_0033C9D0, reached from
 * Present and the ring-space checks), it writes NO_OPERATION(5) into the
 * pushbuffer, kicks off, and sleeps on the KEVENT at 0x00348FDC until the
 * GPU's interrupt for that method has been handled
 * (KeWaitForSingleObject at 0x0033CB2D, looping until it succeeds). On the
 * console the GPU traps the method, D3D's ISR (0x0033F500) queues its DPC
 * (0x0033FEB0), and the PGRAPH handler (0x0033FC70) dispatches the value
 * (0x0033FA00): case 5 is KeSetEvent(context + 0x1A4), context being D3D's
 * interrupt block at 0x00348E38.
 *
 * The runtime raises no PGRAPH interrupt, so the event was never set and the
 * main thread slept for good -- the freeze on the legal screen, whose
 * 1,500-odd text batches are the first thing that puts the GPU that far
 * behind. The executor now calls this hook when its walk reaches a software
 * method, which is when the GPU would have trapped it. Only 5 is handled;
 * the others are counted so a run shows whether the title sends any. */
#define RDR_D3D_SWM_EVENT 0x00348FDCu   /* 0x00348E38 + 0x1A4 */

extern void nv2a_pb_set_sw_method_hook(void (*fn)(uint32_t, uint32_t, uint32_t));
extern int  xbox_KeSetEventFromHost(uint32_t guest_va);

static void rdr_sw_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static unsigned seen[32];
    unsigned code = param & 0x1Fu;

    (void)subch; (void)method;
    if (seen[code]++ == 0)
        fprintf(stderr, "[RDR] D3D software method %u (value 0x%08X)%s\n", code, param,
                code == 5 ? ": setting D3D's GPU event" : ": not handled");
    if (code == 5 && !xbox_KeSetEventFromHost(RDR_D3D_SWM_EVENT)) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[RDR] D3D GPU event 0x%08X is not a KEVENT; "
                            "is RECOMP_TITLE_KEVENTS=1?\n", RDR_D3D_SWM_EVENT);
    }
}

/* ── RDR_MEMSNAP=<prefix>: guest RAM snapshots, for finding things ──
 *
 * Each time <prefix>.flag appears, guest RAM from 0x10000 to 64 MB, then
 * the 64 MB contiguous window at 0x80000000, go to <prefix>NN.bin (file
 * offset 0 = guest 0x10000, 0x03FF0000 = 0x80000000); the flag is removed. A test aid (camera
 * angles, money, the level table were found by diffing these); costs one
 * getenv when unset. */
static void memsnap_poll(void)
{
    static const char *prefix = (const char *)-1;
    static int seq;
    char flag[600], out[600];
    FILE *f;
    if (prefix == (const char *)-1)
        prefix = getenv("RDR_MEMSNAP");
    if (!prefix)
        return;
    snprintf(flag, sizeof flag, "%s.flag", prefix);
    f = fopen(flag, "rb");
    if (!f)
        return;
    fclose(f);
    remove(flag);
    snprintf(out, sizeof out, "%s%02d.bin", prefix, seq++);
    f = fopen(out, "wb");
    if (f) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        fwrite(mem + 0x10000, 1, 0x04000000 - 0x10000, f);
        /* Then the contiguous window (0x80000000, 64 MB): objects the game
         * allocates there, the player among them. */
        fwrite(mem + 0x80000000u, 1, 0x04000000, f);
        fclose(f);
        fprintf(stderr, "[RDR] memory snapshot %s\n", out);
    }
}

#if defined(_WIN32)
static DWORD WINAPI memsnap_thread(LPVOID unused)
{
    (void)unused;
    for (;;) { memsnap_poll(); Sleep(200); }
    return 0;
}
#else
static void *memsnap_thread(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec ts = { 0, 200000000L };
        memsnap_poll();
        nanosleep(&ts, NULL);
    }
    return NULL;
}
#endif

void rdr_runtime_init(void)
{
    room_rename();
    if (getenv("RDR_MEMSNAP")) {
#if defined(_WIN32)
        HANDLE m = CreateThread(NULL, 0, memsnap_thread, NULL, 0, NULL);
        if (m) CloseHandle(m);
#else
        pthread_t m;
        if (pthread_create(&m, NULL, memsnap_thread, NULL) == 0)
            pthread_detach(m);
#endif
    }
    nv2a_pb_set_sw_method_hook(rdr_sw_method);
#if defined(_WIN32)
    HANDLE h = CreateThread(NULL, 0, fence_binder, NULL, 0, NULL);
    if (h) CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, fence_binder, NULL) == 0)
        pthread_detach(t);
#endif
}

/* ── Flash menus: the shop and the weapon select ─────────────────────
 *
 * The shop before a mission (and the stores in town) and the weapon select
 * after it are Flash movies, not swui pages: a menu object (built by
 * sub_000188C0: its name at +0, the movie's player at +0x24) whose frame
 * is sub_00018490 (thiscall, the frame time; ret 4), with the movie's
 * variables read by sub_00018600 (thiscall: name, &value; ret 8). The
 * prelevel loop (sub_00069B20) runs "shop_<level>" then
 * "weaponSelection". Their layouts are fixed (shots, 640x448 space):
 *
 *   shop:   arrows at the top corners (previous / next item), "A Done"
 *           and "Y Buy This Item" along the bottom
 *   weapon: three rows (Side Arms, Long Arms, Thrown; the movie's
 *           currentSelection 0-2) with arrows around the chosen row's
 *           weapon, "A Done" bottom left
 *
 * The hook on the frame (rdr_hooks.c) reports the prompt under the pointer
 * (a click there is that button, rdr_input.c), and handles a click on a
 * row itself: the d-pad to that row, then the arrow if one was clicked. */
extern void sub_00018600(void);

static int swf_get_int(uint32_t menu, const char *name, int32_t *out)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t esp = g_esp, ecx = g_ecx, eax = g_eax, buf, val;
    size_t n = strlen(name), i;
    int ok;
    g_esp = (g_esp - 64u - (uint32_t)n) & ~15u;
    buf = g_esp + 16;
    val = g_esp + 8;
    for (i = 0; i <= n; i++)
        m[buf + i] = (uint8_t)name[i];
    guest_wr32(val, 0x7FFFFFF1u);
    guest_push(val);
    guest_push(buf);
    guest_push(0x00069CEFu);                /* a return address */
    g_ecx = menu;
    sub_00018600();                         /* ret 8 */
    ok = (g_eax & 0xFF) != 0;
    *out = (int32_t)guest_rd32(val);
    g_esp = esp;
    g_ecx = ecx;
    g_eax = eax;
    return ok && *out != 0x7FFFFFF1;
}

extern void sub_00018620(void);
extern void sub_00018610(void);

/* A variable as text (thiscall: name, buffer, size; ret 0xC): an array
 * comes as "a,b,c". */
static int swf_get_str(uint32_t menu, const char *name, char *out, unsigned size)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t esp = g_esp, ecx = g_ecx, eax = g_eax, nbuf, obuf;
    size_t n = strlen(name), i;
    int ok;
    if (size > 512) size = 512;
    g_esp = (g_esp - 600u - (uint32_t)n) & ~15u;
    obuf = g_esp + 16;
    nbuf = obuf + 520;
    for (i = 0; i <= n; i++)
        m[nbuf + i] = (uint8_t)name[i];
    m[obuf] = 0;
    guest_push(size);
    guest_push(obuf);
    guest_push(nbuf);
    guest_push(0x00069E84u);                /* a return address */
    g_ecx = menu;
    sub_00018610();                         /* ret 0xC */
    ok = (g_eax & 0xFF) != 0;
    for (i = 0; i + 1 < size && m[obuf + i]; i++)
        out[i] = (char)m[obuf + i];
    out[i] = 0;
    g_esp = esp;
    g_ecx = ecx;
    g_eax = eax;
    return ok;
}

static void swf_set_int(uint32_t menu, const char *name, int32_t value)
{
    volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)xbox_GetMemoryOffset();
    uint32_t esp = g_esp, ecx = g_ecx, eax = g_eax, buf;
    size_t n = strlen(name), i;
    g_esp = (g_esp - 64u - (uint32_t)n) & ~15u;
    buf = g_esp + 16;
    for (i = 0; i <= n; i++)
        m[buf + i] = (uint8_t)name[i];
    guest_push((uint32_t)value);
    guest_push(buf);
    guest_push(0x00069CFDu);                /* a return address */
    g_ecx = menu;
    sub_00018620();                         /* ret 8 */
    g_esp = esp;
    g_ecx = ecx;
    g_eax = eax;
}

/* Tests (overlay flag "swfset <name> <value>"): set a variable of the next
 * Flash menu to run a frame. */
static char s_swf_set_name[32];
static volatile int s_swf_set_val, s_swf_set_on;

static char s_swf_get_name[64];
static volatile int s_swf_get_on;

void rdr_swf_test_get(const char *name)
{
    snprintf(s_swf_get_name, sizeof s_swf_get_name, "%s", name);
    s_swf_get_on = 1;
}

void rdr_swf_test_set(const char *name, int value)
{
    snprintf(s_swf_set_name, sizeof s_swf_set_name, "%s", name);
    s_swf_set_val = value;
    s_swf_set_on = 1;
}

/* The sheriff's journal (swuiBookJournalView): each of its pages is a
 * Flash movie ("j01_01_mainMenu" the contents, "j02_01_heros", ...) whose
 * hand points at entry curMenSel: the left column 0..columnSwitch, the
 * right one on from there; an entry is open when its bit is set in
 * mainUnlock (the contents) or unlock (the others) -- the d-pad skips the
 * rest. The mouse no longer steps it (a sideways movement turned the
 * pages); the open entry under the pointer takes the hand, and a click
 * opens it (A). The book is 3D: places in the 640x360 frame of the test
 * shots, the journal not zoomed. The contents' entries are measured; on
 * the other pages an entry's place in the movie is selectionStartV +
 * k * menuLineOffset (the right column columnOffsetV higher), about
 * 219 + 0.519 v in the frame (a few pixels' play as the pages stack up). */
typedef struct { float x0, x1, y; int idx; } JEntry;

static const JEntry s_j_contents[] = {
    { 155, 312, 143, 0 }, { 155, 312, 159, 1 }, { 155, 312, 175, 2 }, { 155, 312, 190, 3 },
    { 155, 312, 207, 4 }, { 155, 312, 223, 5 }, { 155, 312, 240, 6 }, { 155, 312, 256, 7 },
    { 155, 312, 273, 8 },
    { 325, 490,  77, 9 }, { 325, 490,  97, 10 }, { 325, 490, 113, 11 }, { 325, 490, 129, 12 },
    { 325, 490, 145, 13 }, { 325, 490, 161, 14 }, { 325, 490, 179, 15 }, { 325, 490, 196, 16 },
};

static int journal_entry(uint32_t menu, const char *name, float fx, float fy)
{
    int32_t open = -1, sv, off, cs = 8, cov = 110;
    int idx = -1;
    unsigned i;
    if (!strcmp(name, "j01_01_mainMenu")) {
        for (i = 0; i < sizeof s_j_contents / sizeof s_j_contents[0]; i++)
            if (fx >= s_j_contents[i].x0 && fx <= s_j_contents[i].x1
                && fy >= s_j_contents[i].y - 8.0f && fy < s_j_contents[i].y + 8.0f)
                idx = s_j_contents[i].idx;
        swf_get_int(menu, "mainUnlock", &open);
    } else {
        float v = (fy - 219.0f) / 0.519f, k;
        if (!swf_get_int(menu, "selectionStartV", &sv) || !swf_get_int(menu, "menuLineOffset", &off)
            || off < 10 || off > 100)
            return -1;
        swf_get_int(menu, "columnSwitch", &cs);
        swf_get_int(menu, "columnOffsetV", &cov);
        if (cs < 0 || cs > 30)
            cs = 8;
        if (fx >= 150 && fx <= 316) {
            k = (v - (float)sv) / (float)off;
            if (k > -0.5f && k < (float)cs + 0.5f)
                idx = (int)(k + 0.5f);
        } else if (fx >= 324 && fx <= 495) {
            k = (v - (float)(sv - cov)) / (float)off;
            if (k > -0.5f && k < 12.5f)
                idx = cs + 1 + (int)(k + 0.5f);
        }
        if (!swf_get_int(menu, "unlock", &open))
            open = -1;
    }
    if (idx < 0 || idx > 30 || !((uint32_t)open & (1u << idx)))
        return -1;
    return idx;
}

static void journal_swf(uint32_t menu, const char *name, float u, float v, unsigned gen, int click)
{
    /* Two of the journal's movies run at once (the page and a generic
     * one behind it): the page with a hand (curMenSel) is the one. */
    static unsigned seen_gen;
    static uint32_t seen_menu;
    static uint64_t list_ns;
    static int over = -1;                   /* the open entry under the pointer */
    float fx = 320.0f + u * 360.0f, fy = v * 360.0f;
    int hit = RDR_HUD_NOTHING;
    int32_t cur = -1;
    uint64_t now = xbox_HostNowNs();
    int list = swf_get_int(menu, "curMenSel", &cur);

    rdr_input_pointer_page(0);              /* the mouse no longer turns pages */
    if (list) {
        list_ns = now;
        if (gen != seen_gen || click || menu != seen_menu) {
            over = journal_entry(menu, name, fx, fy);
            if (over >= 0 && cur != over)
                swf_set_int(menu, "curMenSel", over);
        }
        seen_gen = gen;
        seen_menu = menu;
    } else if (now - list_ns < 300000000ull) {
        return;                             /* the page with the hand reports */
    } else {
        over = -1;
    }
    if (over >= 0)
        hit = RDR_HUD_A;                    /* a click opens it */
    /* The prompts under the book ("X Contents", "A OK", "B Leave"). */
    else if (fy >= 308 && fy <= 336 && fx >= 125 && fx <= 250) hit = RDR_HUD_X;
    else if (fy >= 308 && fy <= 336 && fx >= 285 && fx <= 375) hit = RDR_HUD_A;
    else if (fy >= 308 && fy <= 336 && fx >= 415 && fx <= 520) hit = RDR_HUD_B;
    rdr_input_hud(hit);
}

static char s_last_swf_set[32];   /* the Flash menu last seen (rdr_heap_check) */

void rdr_swf_menu(uint32_t menu)
{
    static uint32_t seen[16];
    static unsigned nseen;
    static int was_down, target = -1, after;
    char name[32];
    unsigned i;
    int kind, hit = RDR_HUD_NOTHING, down, row = -1;
    float px, py, u, v;
    unsigned gen;

    if (!in_heap(menu))
        return;
    guest_str(menu, name, sizeof name);
    for (i = 0; i < nseen && seen[i] != menu; i++)
        ;
    if (i == nseen && nseen < 16) {
        seen[nseen++] = menu;
        fprintf(stderr, "[SWF] menu %08X '%s'\n", menu, name);
        fflush(stderr);
    }
    {
        /* Tests: RDR_SWF_VARS="a,b": those variables, twice a second. */
        static const char *vars = (const char *)1;
        static uint64_t last;
        if (vars == (const char *)1) vars = getenv("RDR_SWF_VARS");
        if (vars && xbox_HostNowNs() - last > 500000000ull) {
            char list[256], *t, *comma;
            last = xbox_HostNowNs();
            snprintf(list, sizeof list, "%s", vars);
            fprintf(stderr, "[SWF] %s:", name);
            for (t = list; t && *t; t = comma ? comma + 1 : NULL) {
                int32_t x;
                comma = strchr(t, ',');
                if (comma)
                    *comma = 0;
                if (swf_get_int(menu, t, &x)) fprintf(stderr, " %s=%d", t, (int)x);
                else fprintf(stderr, " %s=?", t);
            }
            fputc('\n', stderr);
            fflush(stderr);
        }
    }
    if (s_swf_get_on) {
        int32_t x = 0;
        int ok;
        s_swf_get_on = 0;
        char str[256];
        ok = swf_get_int(menu, s_swf_get_name, &x);
        fprintf(stderr, "[SWF] %s: get %s -> %s %d", name, s_swf_get_name, ok ? "found" : "not found", (int)x);
        ok = swf_get_str(menu, s_swf_get_name, str, sizeof str);
        fprintf(stderr, " text %s \"%s\"\n", ok ? "found" : "not found", str);
        fflush(stderr);
    }
    if (s_swf_set_on) {
        s_swf_set_on = 0;
        swf_set_int(menu, s_swf_set_name, s_swf_set_val);
        fprintf(stderr, "[SWF] %s: %s set to %d\n", name, s_swf_set_name, s_swf_set_val);
        fflush(stderr);
    }
    if (!strcmp(name, "logo01X"))
        s_title_up = 1;
    kind = !strcmp(name, "weaponSelection") ? 2 : !strncmp(name, "shop", 4) ? 1
         : !strcmp(name, "startChpMenu") ? 4 : 0;
    {
        extern void rdr_heap_check(const char *where);
        static char last_name[32];
        if (strcmp(last_name, name)) {
            snprintf(last_name, sizeof last_name, "%s", name);
            snprintf(s_last_swf_set, sizeof s_last_swf_set, "%s", name);
        }
        rdr_heap_check(name);
    }
    if (name[0] == 'j' && name[1] >= '0' && name[1] <= '9' && name[2] >= '0' && name[2] <= '9' && name[3] == '_')
        kind = 3;                           /* the sheriff's journal */
    {
        /* Online, the menus before a mission (weapon select, shop, "Start
         * / Weapon Selection / Shop") run while the level loads: their
         * frames are played in step (rdr_lockstep.c) */
        extern void rdr_ls_prelevel_menu(void);
        if (kind == 1 || kind == 2 || kind == 4)
            rdr_ls_prelevel_menu();
    }
    if (!kind || !rdr_input_pointer(&px, &py, &u, &v, &gen))
        return;
    down = xbox_FramebufferKeyDown(0x01);
    {
        /* A press is the clicked menu's own. */
        static uint32_t press_menu;
        if (kind != 3 && press_menu && press_menu != menu && rdr_input_swf_busy())
            rdr_input_swf_cancel();
        if (kind != 3 && down && !was_down)
            press_menu = menu;
    }
    if (kind == 4) {
        /* Before a mission: Start / Weapon Selection / Shop (curMenSel
         * 0-2) and "A OK". */
        static unsigned seen_gen;
        int over = -1;
        for (i = 0; i < 3; i++)
            if (px >= 130 && px <= 440 && py >= 159.0f + 33.6f * (float)i - 16.0f
                && py < 159.0f + 33.6f * (float)i + 16.0f)
                over = (int)i;
        if (over >= 0 && (gen != seen_gen || (down && !was_down))) {
            int32_t cur = -1;
            if (swf_get_int(menu, "curMenSel", &cur) && cur != over)
                swf_set_int(menu, "curMenSel", over);
        }
        seen_gen = gen;
        if (down && !was_down && (over >= 0 || (py >= 365 && py <= 405 && px >= 50 && px <= 160)))
            rdr_input_swf_press(RDR_HUD_A);
        was_down = down;
        rdr_input_pointer_page(0);
        rdr_input_hud(RDR_HUD_NOTHING);
        return;
    }
    if (kind == 3) {
        journal_swf(menu, name, u, v, gen, down && !was_down);
        was_down = down;
        return;
    }
    /* A click is one press the movie sees (rdr_input_swf_press): it reads
     * the pad at its own pace and missed a short one. */
    if (kind == 1) {
        if (py >= 20 && py <= 85 && px >= 35 && px <= 115) hit = RDR_HUD_LEFT;
        else if (py >= 20 && py <= 85 && px >= 525 && px <= 612) hit = RDR_HUD_RIGHT;
        else if (py >= 370 && py <= 420 && px >= 45 && px <= 205) hit = RDR_HUD_A;
        else if (py >= 370 && py <= 420 && px >= 385 && px <= 600) hit = RDR_HUD_Y;
        if (down && !was_down && hit != RDR_HUD_NOTHING)
            rdr_input_swf_press(hit);
    } else {
        static const float row_y[3] = { 294.0f, 320.0f, 347.0f };
        int32_t cur = -1;
        swf_get_int(menu, "currentSelection", &cur);
        if (py >= 370 && py <= 420 && px >= 45 && px <= 175)
            hit = RDR_HUD_A;
        for (i = 0; i < 3; i++)
            if (px >= 90 && px <= 605 && py >= row_y[i] - 13.0f && py < row_y[i] + 13.0f)
                row = (int)i;
        if (down && !was_down && hit == RDR_HUD_A)
            rdr_input_swf_press(RDR_HUD_A);
        if (row >= 0 && down && !was_down) {
            int arrow = px >= 240 && px <= 292 ? RDR_HUD_LEFT
                      : px >= 535 && px <= 605 ? RDR_HUD_RIGHT : 0;
            if (cur == row) {
                if (arrow)
                    rdr_input_swf_press(arrow);
            } else if (cur >= 0 && cur <= 2) {
                target = row;
                after = arrow;
            }
        }
        if (target >= 0 && !rdr_input_swf_busy()) {
            if (cur == target) {
                if (after)
                    rdr_input_swf_press(after);
                target = -1;
                after = 0;
            } else if (cur < 0 || cur > 2) {
                target = -1;
            } else {
                rdr_input_swf_press(cur < target ? RDR_HUD_DOWN : RDR_HUD_UP);
            }
        }
    }
    was_down = down;
    rdr_input_pointer_page(0);              /* a click elsewhere: nothing */
    rdr_input_hud(RDR_HUD_NOTHING);         /* clicks are pressed above */
}

/* ── Heap check (user crash: "Free(%p) - Prev node no longer points back
 * to us" unloading Brimstone after buying in its stores) ───────────────
 *
 * The game's heaps (memMemoryAllocator: a list at [0x0065A5F8], +4 next,
 * +8 base, +0xC size) are runs of blocks, each after a 16-byte header:
 * +0 the block before it (low 3 bits flags), +4 its size; the next block
 * starts 16 + the size rounded up to 16 further on. The free that failed
 * found a block whose "block before" no longer matched: something wrote
 * past the end of the block in front of it. Walked every half second or
 * so from the game's thread; the first break is logged once, with the
 * bytes on either side of it and the Flash menu last seen, to name what
 * wrote there. Tests only (RDR_HEAP_CHECK=1): a heap in the game's own
 * image (0x005AC0A0) does not follow these rules everywhere. The free's
 * own failure is dumped too (rdr_heap_bad_free). */

void rdr_heap_check(const char *where)
{
    static int on = -1, reported;
    static unsigned calls;
    static uint32_t suspect;
    uint32_t h, base, size, node, prev, next, sz, p, bad = 0, bad_prev = 0;
    unsigned nh = 0, k;

    if (on < 0) {
        const char *e = getenv("RDR_HEAP_CHECK");
        on = e && *e == '1';                /* tests only: the walk is not exact yet */
    }
    if (!on || reported || (calls++ % 30))
        return;
    for (h = guest_rd32(0x0065A5F8u); in_heap(h) && nh < 16 && !bad; h = guest_rd32(h + 4), nh++) {
        base = guest_rd32(h + 8);
        size = guest_rd32(h + 0xC);
        if (!in_heap(base) || size < 64 || size > 0x04000000u || !in_heap(base + size - 1))
            continue;
        node = base;
        prev = 0;
        for (k = 0; k < 500000 && node + 16 <= base + size; k++) {
            p = guest_rd32(node) & ~7u;
            sz = guest_rd32(node + 4);
            if (!guest_rd32(node) && !sz)
                break;                      /* past the last block: never used */
            /* a run's first block points at the heap itself */
            if ((prev && p != prev && p != h) || sz > size) {
                bad = node;
                bad_prev = prev;
                break;
            }
            next = node + 16 + ((sz + 15) & ~15u);
            if (next <= node)
                break;
            prev = node;
            node = next;
        }
    }
    if (!bad) {
        suspect = 0;
        return;
    }
    if (suspect != bad) {                   /* seen twice in a row: not a race */
        suspect = bad;
        return;
    }
    reported = 1;
    fprintf(stderr, "[HEAP] broken at block %08X (header %08X %08X), the block before %08X "
            "(header %08X %08X); seen in %s, last Flash menu '%s'\n",
            bad, guest_rd32(bad), guest_rd32(bad + 4), bad_prev,
            guest_rd32(bad_prev), guest_rd32(bad_prev + 4), where, s_last_swf_set);
    for (k = 0; k < 0xC0; k += 16) {
        uint32_t a = bad - 0x80 + k, j;
        char asc[17];
        fprintf(stderr, "[HEAP]   %08X:", a);
        for (j = 0; j < 16; j++) {
            uint8_t c = guest_rd8(a + j);
            fprintf(stderr, " %02X", c);
            asc[j] = c >= 32 && c < 127 ? (char)c : '.';
        }
        asc[16] = 0;
        fprintf(stderr, "  %s\n", asc);
    }
    fflush(stderr);
}

/* The free that found the break ("Free(%p) - Prev/Next node ..."): the
 * block, its header, the block its header names, and the bytes before and
 * after the header -- what overran into it. */
void rdr_heap_bad_free(uint32_t ptr)
{
    uint32_t hdr = ptr - 16, prev = guest_rd32(hdr) & ~7u, k;
    fprintf(stderr, "[HEAP] bad free of %08X: header %08X %08X; it names %08X (header %08X %08X, "
            "which ends at %08X); last Flash menu '%s'\n", ptr, guest_rd32(hdr), guest_rd32(hdr + 4),
            prev, in_heap(prev) ? guest_rd32(prev) : 0, in_heap(prev) ? guest_rd32(prev + 4) : 0,
            in_heap(prev) ? prev + 16 + ((guest_rd32(prev + 4) + 15) & ~15u) : 0, s_last_swf_set);
    for (k = 0; k < 0x100; k += 16) {
        uint32_t a = hdr - 0xC0 + k, j;
        char asc[17];
        if (!in_heap(a))
            continue;
        fprintf(stderr, "[HEAP]   %08X:", a);
        for (j = 0; j < 16; j++) {
            uint8_t c = guest_rd8(a + j);
            fprintf(stderr, " %02X", c);
            asc[j] = c >= 32 && c < 127 ? (char)c : '.';
        }
        asc[16] = 0;
        fprintf(stderr, "  %s\n", asc);
    }
    fflush(stderr);
}

/* Whether freeing this block is sound (rdr_hooks.c, sub_0005D8F0): it is
 * marked in use (bit 0 of its header's first word; the free clears it),
 * the block its header names ends where it starts, and the block after it
 * names it back -- the game's own checks, made before anything is
 * changed. The heap: +8 its base, +0x10 its size. */
/* The game's code addresses on the guest stack from esp up: who called. */
static void heap_trace(uint32_t esp)
{
    unsigned k, n = 0;
    fprintf(stderr, "[HEAP]   callers:");
    for (k = 0; k < 96 && n < 10; k++) {
        uint32_t v = guest_rd32(esp + k * 4);
        if (v >= 0x00011000u && v < 0x00340000u) {
            fprintf(stderr, " %08X", v);
            n++;
        }
    }
    fputc('\n', stderr);
}

int rdr_heap_free_ok(uint32_t heap, uint32_t ptr, uint32_t caller, uint32_t esp)
{
    static unsigned logged, checked, traced;
    uint32_t hdr, base, end, w0, prev, nx, blk = ptr;
    const char *why = NULL;
    unsigned i;
    /* The block that has been freed twice in the user's runs (always at
     * this address, a name-map node freed again at Brimstone's unload):
     * every free of it, with who asked, to find the first. */
    if (ptr == 0x81DE5C1Cu) {
        static unsigned watched;
        if (watched++ < 40) {
            fprintf(stderr, "[HEAP] watch: free of %08X (from 0x%08X), header %08X %08X, words %08X %08X %08X\n",
                    ptr, caller, guest_rd32(ptr - 16), guest_rd32(ptr - 12), guest_rd32(ptr),
                    guest_rd32(ptr + 4), guest_rd32(ptr + 8));
            heap_trace(esp);
            fflush(stderr);
        }
    }
    if (!heap) {
        /* FindHeap found none (its fatal error is passed over): a pointer
         * that is not the heaps' at all -- leave it. */
        if (logged++ < 20) {
            fprintf(stderr, "[HEAP] free of %08X (from 0x%08X) left out: in no heap; last Flash menu '%s'\n",
                    ptr, caller, s_last_swf_set);
            heap_trace(esp);
            fflush(stderr);
        }
        return 0;
    }
    if (!in_heap(heap))
        return 1;
    /* As the free goes (sub_0005D8F0): a block of one of the six
     * small-block pools (+0xE8, 8 apart: +0 the first chunk -- +0 the
     * next, items from +0x40 -- +6 the items' span) is the pool's; a heap
     * without +0x21 and +0x20 set checks and frees nothing here; with
     * +0x22 the block starts 16 before the pointer (a guard word). Then
     * the same two checks the game makes, on the same words. */
    for (i = 0; i < 6; i++) {
        uint32_t pool = heap + 0xE8u + i * 8u, chunk = guest_rd32(pool), span = guest_rd32(pool + 4) >> 16, n;
        for (n = 0; in_heap(chunk) && n < 100000; n++, chunk = guest_rd32(chunk)) {
            if (ptr < chunk + 0x40u)
                continue;
            if (ptr < chunk + 0x40u + span)
                return 1;
        }
    }
    if (!guest_rd8(heap + 0x21) || !guest_rd8(heap + 0x20))
        return 1;
    if (guest_rd8(heap + 0x22))
        blk = ptr - 16;
    hdr = blk - 16;
    base = guest_rd32(heap + 8);
    end = base + guest_rd32(heap + 0x10);
    {
        /* Tests: RDR_HEAP_TEST=N breaks the N-th checked block's back link
         * the way the user's log showed it, to see the free left out. */
        static int test = -1;
        if (test < 0) test = getenv("RDR_HEAP_TEST") ? atoi(getenv("RDR_HEAP_TEST")) : 0;
        if (test > 0 && (int)checked + 1 == test && hdr != base) {
            guest_wr32(hdr, 0);
            fprintf(stderr, "[HEAP] test: header of %08X cleared\n", ptr);
        }
    }
    if (hdr < base || hdr >= end || (hdr & 15u))
        why = "its header is not on its heap's blocks";
    w0 = why ? 0 : guest_rd32(hdr);
    if (!why && hdr != base) {
        /* (the link is checked against the heap before it is followed: a
         * damaged one pointed off the guest's memory -- harness, after a
         * level that is not on the disc: 0xFF950040) */
        prev = w0 & ~7u;
        if (prev < base || prev >= hdr || (prev & 15u)
            || prev + 16u + ((guest_rd32(prev + 4) + 15u) & ~15u) != hdr)
            why = "the block before it does not end at it";
    }
    if (!why) {
        nx = hdr + 16u + ((guest_rd32(hdr + 4) + 15u) & ~15u);
        if (nx <= hdr || nx > end)
            why = "its size runs past its heap";
        else if (nx != end && (guest_rd32(nx) & ~7u) != hdr)
            why = "the block after it does not point back to it";
    }
    if (!why && !(w0 & 1u))
        why = "not in use (freed already)";
    if (!why) {
        /* A free neighbour is joined to this block, and taken off its free
         * list first (sub_0005CF20): its +0x10 / +0x14 are the list's
         * links, followed and written. Links that point outside the heap
         * (user crash loading lvl_tomb 5 after lvl_trainend: a read at
         * 0x8D538000, a size of 0x0B0B0C10 -- something had written over
         * the heap) would take the game with them: the free is left out
         * instead, and the words logged. */
        uint32_t nb[2], k;
        nb[0] = hdr != base ? (w0 & ~7u) : 0;
        nb[1] = hdr + 16u + ((guest_rd32(hdr + 4) + 15u) & ~15u);
        if (nb[1] >= end)
            nb[1] = 0;
        for (k = 0; k < 2 && !why; k++) {
            uint32_t b = nb[k], l0, l1;
            if (!b || (guest_rd32(b) & 1u))
                continue;                       /* none, or in use: not joined */
            l0 = guest_rd32(b + 0x10);
            l1 = guest_rd32(b + 0x14);
            if ((l0 && (l0 < base || l0 >= end || (l0 & 15u))) || (l1 && (l1 < base || l1 >= end || (l1 & 15u)))) {
                why = k ? "the free block after it has broken links" : "the free block before it has broken links";
                if (logged < 20)
                    fprintf(stderr, "[HEAP]   that block %08X: %08X %08X %08X %08X | %08X %08X %08X %08X\n", b,
                            guest_rd32(b), guest_rd32(b + 4), guest_rd32(b + 8), guest_rd32(b + 12),
                            l0, l1, guest_rd32(b + 0x18), guest_rd32(b + 0x1C));
            }
        }
    }
    checked++;
    if (getenv("RDR_HEAP_LOG") && (checked == 2000 || (!(w0 & 1u) && traced++ < 10))) {
        fprintf(stderr, "[HEAP] %u frees checked, %u left out (now %08X, header %08X)\n",
                checked, logged, ptr, w0);
        fflush(stderr);
    }
    if (!why)
        return 1;
    if (logged++ < 20) {
        fprintf(stderr, "[HEAP] free of %08X (from 0x%08X) left out: %s; heap %08X; header %08X %08X %08X %08X; "
                "last Flash menu '%s'\n", ptr, caller, why, heap, guest_rd32(hdr), guest_rd32(hdr + 4),
                guest_rd32(hdr + 8), guest_rd32(hdr + 12), s_last_swf_set);
        heap_trace(esp);
        fflush(stderr);
    }
    return 0;
}

/* ── Give Weapon (a cheat: any of the game's weapons) ───────────────────
 *
 * The weapons are swat.guns' table ([0x00676210], [0x0067621C] of them,
 * 0x410 bytes each, the name first; sub_0014D7C0 finds one by name). A
 * character's weapons: [char+0x80], +8 a byte per weapon, owned or not
 * (sub_000920E0, the characters' vtable +0x144's first step). The player
 * class's +0x144 (sub_0013E980: owned? no -- and not Showdown -- then
 * sub_00139700) gives one: what a level's start does with each weapon its
 * character starts with (sub_001B13B0). Story only, as the game's own;
 * in Showdown the weapon pick-ups are its own. */
extern void sub_00139E50(void);
int rdr_weapon_count(void)
{
    uint32_t n = guest_rd32(0x0067621Cu), b = guest_rd32(0x00676210u);
    return in_heap(b) && n < 256 ? (int)n : 0;
}

int rdr_weapon_name(int i, char *out, int n)
{
    uint32_t b = guest_rd32(0x00676210u);
    out[0] = 0;
    if (i < 0 || i >= rdr_weapon_count() || n <= 0)
        return 0;
    guest_str(b + (uint32_t)i * 0x410u, out, (unsigned)n);
    return out[0] != 0;
}

/* owned by player (0-3)? */
int rdr_weapon_owned(int player, int i)
{
    uint32_t c = guest_rd32(0x00675930u + 4u * (uint32_t)(player & 3)), l, v;
    if (!in_heap(c) || i < 0 || i >= rdr_weapon_count())
        return 0;
    l = guest_rd32(c + 0x80);
    v = in_heap(l) ? guest_rd32(l + 8) : 0;
    return in_heap(v) && guest_rd8(v + (uint32_t)i);
}

/* Fill Ammunition (the Spawner page): every gun the player carries gets a
 * full clip and a full reserve. The player's character [player+0xC] has its
 * weapon list at +0x80, whose table [+0x20] holds an item per weapon (array
 * +0, u16 count +4, a "held" byte each at [+8]); an item is a gun per hand;
 * a gun's +0x20 is its clip's size and +0x24 what is loaded, and its ammo
 * pool [+0x28] has the most it carries at +4 and what is left at +8 (the
 * revolver: 6 of 6, 150 of 150 -- harness). A weapon given with Give
 * Weapon came with what the game gives it; this fills it up. */
void rdr_fill_ammo_run(int player)
{
    uint32_t p = guest_rd32(0x00675930u + 4u * (uint32_t)(player & 3)), ch, wl, tab, arr, valid, n, i, k;
    int guns = 0;
    if (!in_heap(p) || !in_heap(ch = guest_rd32(p + 0xC)) || !in_heap(wl = guest_rd32(ch + 0x80))
        || !in_heap(tab = guest_rd32(wl + 0x20)))
        return;
    arr = guest_rd32(tab);
    n = guest_rd32(tab + 4) & 0xFFFFu;
    valid = guest_rd32(tab + 8);
    if (!in_heap(arr) || !in_heap(valid) || n > 256)
        return;
    for (i = 0; i < n; i++) {
        uint32_t item;
        if (!guest_rd8(valid + i) || !in_heap(item = guest_rd32(arr + i * 4)))
            continue;
        for (k = 0; k < 2; k++) {
            uint32_t gun = guest_rd32(item + k * 4), pool;
            int32_t clip;
            if (!in_heap(gun))
                continue;
            clip = (int32_t)guest_rd32(gun + 0x20);
            if (clip > 0 && clip < 1000 && (int32_t)guest_rd32(gun + 0x24) >= 0)
                guest_wr32(gun + 0x24, (uint32_t)clip);
            pool = guest_rd32(gun + 0x28);
            if (in_heap(pool)) {
                int32_t most = (int32_t)guest_rd32(pool + 4);
                /* the most the weapon's own entry allows (swat.guns +0x38:
                 * the revolver 150, upgraded 200; -1 none): a pool below it
                 * is raised to it. A weapon given with Give Weapon brings a
                 * pool the level never stocked -- the Lightning's at 0 of 0
                 * (harness), so Fill filled nothing (user) -- and one with
                 * no figure of its own gets a pistol's 200. */
                uint32_t wt = guest_rd32(0x00676210u);
                int32_t cap = (in_heap(wt) && i < (uint32_t)rdr_weapon_count())
                                  ? (int32_t)guest_rd32(wt + i * 0x410u + 0x38u) : -1;
                if (cap <= 0 || cap >= 100000)
                    cap = most > 0 ? most : (clip > 1 && clip < 1000 ? 200 : 0);
                if (cap > most && cap < 100000) {
                    fprintf(stderr, "[WEAPON] weapon %u: its ammunition's most %d -> %d\n", i, most, cap);
                    guest_wr32(pool + 4, (uint32_t)cap);
                    most = cap;
                }
                if (most > 0 && most < 100000 && (int32_t)guest_rd32(pool + 8) >= 0)
                    guest_wr32(pool + 8, (uint32_t)most);
            }
            guns++;
        }
    }
    fprintf(stderr, "[WEAPON] player %d: ammunition filled (%d guns)\n", player + 1, guns);
    fflush(stderr);
}

void rdr_give_weapon_run(int player, int weapon)
{
    uint32_t c = guest_rd32(0x00675930u + 4u * (uint32_t)(player & 3));
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, edx0 = g_edx;
    char nm[40];
    if (!in_heap(c) || guest_rd32(c) != 0x003D9FA8u || weapon < 0 || weapon >= rdr_weapon_count()) {
        fprintf(stderr, "[WEAPON] player %d: no weapon %d to give\n", player + 1, weapon);
        fflush(stderr);
        return;
    }
    rdr_weapon_name(weapon, nm, sizeof nm);
    if (rdr_weapon_owned(player, weapon)) {
        fprintf(stderr, "[WEAPON] player %d has %s already\n", player + 1, nm);
        fflush(stderr);
        return;
    }
    {
        guest_push(0);
        guest_push(0);
        guest_push(0);
        guest_push((uint32_t)weapon);
        guest_push(0x00092909u);        /* as sub_00092880's own call does */
        g_ecx = c;
        sub_00139E50();                 /* the player's AddWeapon (vtable +0x104), ret 16 */
    }
    g_esp = esp0; g_ecx = ecx0; g_eax = eax0; g_edx = edx0;
    fprintf(stderr, "[WEAPON] player %d given %s (%d): %s\n", player + 1, nm, weapon,
            rdr_weapon_owned(player, weapon) ? "owned now" : "not taken");
    fflush(stderr);
}
