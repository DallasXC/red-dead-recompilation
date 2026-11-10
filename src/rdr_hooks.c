/*
 * rdr_hooks.c - entry hooks into directly-called game functions.
 *
 * rdr_overrides.c can only stand in for functions the game reaches through
 * a pointer (a virtual method, a callback). Functions it calls directly are
 * hooked here instead: the code generator (tools/regen.*, --exclude-manual
 * on this file) puts a call to sub_XXXXXXXX_enter() at the top of each
 * sub_XXXXXXXX this file defines one for, before the function's own first
 * instruction. g_esp then points at the return address; the arguments
 * follow it.
 *
 * Changing the set of hooks here (adding or removing a *_enter function)
 * needs the C to be generated again: build.ps1 notices and does it (about 15
 * minutes). Changing what a hook does needs only a rebuild.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xbox/xboxrecomp.h>   /* RECOMP_TLS */
#include "ui/rdr_settings.h"
#include "ui/rdr_cheats.h"
#include "ui/rdr_input.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern RECOMP_TLS uint32_t g_esp, g_ecx, g_edi, g_edx, g_esi, g_eax;

/* rdr_freecam.c */
void rdr_freecam_apply(void);

/* The camera: the game's three ways to set it (sub_00181870, sub_001818A0,
 * sub_001818F0) copy the camera's world matrix to [0x00678828] and its
 * inverse, the view matrix, to [0x00678720], then call sub_00180870 to
 * rebuild the view-projection and the culling planes from those two, then
 * hand [0x00678720] to D3D. sub_00180870 is where the free camera puts its
 * own pair in: everything from there on -- culling, drawing -- uses it,
 * while the game's own camera state (the matrices it passed in) is left
 * alone, so switching it off returns to the game's camera at once. */
void rdr_heap_check(const char *where);   /* rdr_overrides.c */

void rdr_flier_camera(void);    /* rdr_overrides.c */
extern volatile int g_rdr_freecam;

void sub_00180870_enter(void)
{
    rdr_freecam_apply();
    if (!g_rdr_freecam)
        rdr_flier_camera();
    rdr_heap_check("play");
}

/* ── Out of sound voices ─────────────────────────────────────────
 *
 * sndControl keeps 60 voices at 0x006841E8 (0xC0 each: +0x14 the sound
 * on it, -1 free; +0x16 its state; +0x1C eight DirectSound buffers, 0x14
 * apart). AllocateVoice (sub_002C8020) takes a free one; with none free it
 * asks each voice's buffers whether they still play (sub_002C7F60, through
 * IDirectSoundBuffer_GetStatus) and frees those that stopped -- and when
 * every one still plays, it ends the game (sub_000175B0, "sndControl::
 * AllocateVoice - no free voice available": exit(), KeBugCheck(0)). That
 * is the crash seen after talking to someone deep into a session: voices
 * that never report stopping pile up until a new line of speech finds none.
 *
 * Before it gets there: with no voice free, the one that has held its
 * sound longest is stopped the way the game's own stop command does
 * (0x002C87ED: state 0x17, IDirectSoundBuffer_StopEx(buffer, 0, 1) on each
 * buffer, then free). Voices in states 0x18/0x19 (the game keeps those) are
 * left alone. Logged, so a leak shows in last_run.log. */
#define SND_VOICES      0x006841E8u
#define SND_VOICE_SIZE  0xC0u
#define SND_VOICE_COUNT 60

extern void sub_0039539E(void);         /* IDirectSoundBuffer_StopEx, stdcall */
extern uint64_t xbox_HostNowNs(void);

static volatile uint8_t *gm(uint32_t va)
{
    return (volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

static void guest_push(uint32_t v)
{
    g_esp -= 4;
    *(volatile uint32_t *)gm(g_esp) = v;
}

void sub_002C8020_enter(void)
{
    static int16_t seen_tag[SND_VOICE_COUNT];
    static uint64_t since[SND_VOICE_COUNT];
    static unsigned stolen;
    uint64_t now = xbox_HostNowNs();
    int i, victim = -1, free_n = 0;
    uint64_t oldest = ~0ull;

    for (i = 0; i < SND_VOICE_COUNT; i++) {
        uint32_t v = SND_VOICES + (uint32_t)i * SND_VOICE_SIZE;
        int16_t tag = *(volatile int16_t *)gm(v + 0x14);
        uint16_t state = *(volatile uint16_t *)gm(v + 0x16);
        if (tag != seen_tag[i] || !since[i]) {
            seen_tag[i] = tag;
            since[i] = now;
        }
        if (tag == -1) {
            free_n++;
            continue;
        }
        if (state == 0x18 || state == 0x19)
            continue;
        if (since[i] < oldest) {
            oldest = since[i];
            victim = i;
        }
    }
    if (free_n || victim < 0)
        return;
    {
        uint32_t v = SND_VOICES + (uint32_t)victim * SND_VOICE_SIZE;
        int k;
        if (stolen++ < 20) {
            fprintf(stderr, "[RDR] sound: all %d voices busy; stopping voice %d (sound %d, held %.1f s)\n",
                    SND_VOICE_COUNT, victim, *(volatile int16_t *)gm(v + 0x14),
                    (double)(now - oldest) * 1e-9);
            fflush(stderr);
        }
        *(volatile uint16_t *)gm(v + 0x16) = 0x17;
        for (k = 0; k < 8; k++) {
            uint32_t buf = *(volatile uint32_t *)gm(v + 0x1C + (uint32_t)k * 0x14);
            if (!buf)
                continue;
            guest_push(1);              /* flags */
            guest_push(0);              /* timestamp, high */
            guest_push(0);              /* timestamp, low */
            guest_push(buf);
            guest_push(0x002C8823u);    /* return address (as in the stop command) */
            sub_0039539E();             /* ret 0x10: pops all of it */
        }
        *(volatile int16_t *)gm(v + 0x14) = -1;
        seen_tag[victim] = -1;
    }
}

/* ── Fatal errors that need not be ─────────────────────────────────
 *
 * sub_000175B0(fmt, ...) is the game's fatal error: in the retail build it
 * prints nothing and calls exit(1) (which ends in KeBugCheck). A few of its
 * 333 callers are timeouts counted in frames -- "Audio didn't halt after
 * 300 frames", "Sound bank took longer than 300 frames to load" -- written
 * for a console at 60 frames a second, so at 240 or more frames they give
 * up four times sooner; and the out-of-voices one, whose caller copes with
 * a failed allocation anyway (it returns -1 and the sound is skipped).
 * Those return to their caller instead, logged; the loops around the audio
 * ones simply keep waiting. Every other fatal error still ends the game. */
extern void sub_000175B0_gen(void);

static const uint32_t s_soft_fatal[] = {
    0x003C602Cu,    /* Audio didn't halt after 300 frames? (1) */
    0x003CCA84u,    /* ... (2) */
    0x003CCAACu,    /* ... (3) */
    0x003CD8D8u,    /* Audio didn't halt after 600 frames? (3) */
    0x003CD6B8u,    /* SwapLevels(): ... audio didn't halt after 300 frames */
    0x003D1E68u,    /* Sound bank took longer than 300 frames to load! */
    0x003FE2F8u,    /* sndControl::AllocateVoice - no free voice available */
    0x003CB96Cu,    /* memMemoryAllocator::FindHeap failed on ptr %x: it returns 0,
                       and the free that asked leaves it alone (sub_0005D8F0 below) */
};

/* An AI table from another chapter (the spawner's "Acts like", rdr_npc.c)
 * can name one of that chapter's crack zones (property 0x25: where a
 * herd grazes, where a gunman takes cover), found by key in the level's
 * zones ([0x0065AA0C]: +0x4C the zones, u16 count +0x50; sub_0007AD70) --
 * a level without it ended the game ("aiEntity::aiEntity: No crack zone
 * whose name matches key %d in file %s", 0x000E2E6B; harness: Chapter
 * 11's cow made in Chapter 2). The property's zone ([edi+8]) is given the
 * level's first zone instead and the table goes on: the one made goes
 * there, or stands. */
#define FMT_NO_CRACK_ZONE 0x003D4690u
/* the same for a cannon (property 0x24, the level's cannons from
 * [0x0067B9EC] by +0xD88; the bridge's gunners): none to give, so the one
 * being made is not brought in (rdr_overrides.c npc_run) */
#define FMT_NO_CANNON     0x003D46D8u
/* and a state's command that names one ("aiActState::SetCommand - error in
 * %s: no cannon whose name matches key %d", 0x000FF117; the train's
 * cannon gunner) */
#define FMT_NO_CANNON_CMD 0x003D5108u
extern int g_rdr_npc_making, g_rdr_npc_bad;
extern double rdr_now_seconds(void);

/* "Audio didn't halt": the loops around these wait, a frame at a time,
 * until the sound manager ([0x0065B944]) has nothing playing --
 * sub_000992B0: its 4 channels ([+4] + 0x2C i: +0x20 its state, 0 idle)
 * and, through sub_00098B80, its 48 streams ([+8] + 0x34 i: +4 the sound
 * it holds, a pointer). Twice (user logs, after Start on the title) one
 * sound never finished -- its voice held in state 0x15 -- and the game
 * waited for good. Once the wait has gone on 5 s past the game's own
 * limit, the manager's check answers "quiet" while the wait lasts
 * (sub_000992B0 below) and the game goes on. Nothing in the sound manager
 * is changed: an earlier way (1.0.6) set the channels idle and the
 * streams' sound pointers to 0, which left the streams' users holding
 * nothing (review M6). */
static volatile double s_quiet_until;

static void audio_halt_wait(void)
{
    static double first, last;
    static int said;
    double now = rdr_now_seconds();
    if (now - last > 1.0) {
        first = now;
        said = 0;
    }
    last = now;
    if (now - first < 5.0)
        return;
    s_quiet_until = now + 0.5;          /* (renewed while the game still waits) */
    if (!said) {
        said = 1;
        fprintf(stderr, "[SND] the wait for quiet: gone on %.0f s; not waited for any longer\n", now - first);
        fflush(stderr);
    }
}

/* ── Rail vehicles at high frame rates (the stagecoach) ───────────────
 *
 * The stagecoach (lvl_coach; a "RailVehicle": axles on a track, horses
 * pulling) moves in sub_00207790 (this = the level's vehicles, arg = the
 * vehicle's number; called once a frame from sub_00208AC0). Its physics
 * steps in slices of [0x0040D034] = 0.016 s (times game / real time):
 * one slice for every whole slice in the frame's time [0x00406504], the
 * rest thrown away -- and none at all when the frame is no longer than a
 * slice. Above 62.5 frames a second no frame is, so the coach's physics
 * never ran while its horses went on; the odd slow frame then ran one
 * slice that pulled it back all at once, and it flew (user video: the
 * stagecoach flung into the sky and around the map; the level could not
 * be finished). Now, when a frame is shorter than a slice, the vehicle is
 * left as it is until the frames add up to more than a slice, and then
 * moved once by all of that time: as the console moved it at 60 frames a
 * second. At 60 and below (and online, at 1/60) the game's own way. */
extern void sub_00207790_gen(void);
static uint32_t g32v(uint32_t va);
int g_rdr_test_coach_log;               /* tests ("coachlog 1"): where the vehicles are */
int g_rdr_test_coach_old;               /* tests ("coachold 1"): the game's own way, for comparing */
void sub_00207790(void)
{
    static struct { uint32_t mgr, idx; float dt, real; } acc[8];
    volatile float *pdt = (volatile float *)gm(0x00406504u);
    volatile float *preal = (volatile float *)gm(0x0040652Cu);
    float dt = *pdt, real = *preal, slice = *(volatile float *)gm(0x0040D034u), step;
    uint32_t mgr = g_ecx, idx = g32v(g_esp + 4);
    int k, f = -1;
    step = real > 0.0f ? dt / real * slice : slice;
    for (k = 0; k < 8; k++)
        if (acc[k].mgr == mgr && acc[k].idx == idx) { f = k; break; }
    if (g_rdr_test_coach_log) {
        static unsigned n;
        uint32_t veh = g32v(mgr + 0xD4) + idx * 0x35Cu, m = g32v(veh + 8);
        if (m >= 0x10000u && (n++ % 64) == 0)
            fprintf(stderr, "[COACH] vehicle %u at %.1f %.1f %.1f (frame %.4f s, slice %.4f)\n", idx,
                    *(volatile float *)gm(m + 0x24), *(volatile float *)gm(m + 0x28),
                    *(volatile float *)gm(m + 0x2C), (double)dt, (double)step);
    }
    if (g_rdr_test_coach_old || !(step > 0.0f) || dt > step) {
        if (f >= 0)
            acc[f].dt = acc[f].real = 0.0f;
        sub_00207790_gen();             /* the game's own: a slice or more this frame */
        return;
    }
    if (f < 0) {
        for (k = 0; k < 8 && acc[k].mgr; k++) {}
        if (k == 8)
            k = (int)(idx & 7u);        /* (more vehicles than ever seen: reuse) */
        acc[k].mgr = mgr;
        acc[k].idx = idx;
        acc[k].dt = acc[k].real = 0.0f;
        f = k;
    }
    acc[f].dt += dt;
    acc[f].real += real;
    {
        static uint32_t said_mgr;
        if (said_mgr != mgr) {
            said_mgr = mgr;
            fprintf(stderr, "[COACH] rail vehicles moved in slices of %.3f s: frames of %.4f s added up\n",
                    (double)step, (double)dt);
            fflush(stderr);
        }
    }
    if (acc[f].dt <= step) {
        g_esp += 8;                     /* ret 4: not moved this frame */
        return;
    }
    if (g_rdr_test_coach_log) {
        static unsigned n;
        uint32_t veh = g32v(mgr + 0xD4) + idx * 0x35Cu, m = g32v(veh + 8);
        if (m >= 0x10000u && (n++ % 8) == 0)
            fprintf(stderr, "[COACH] vehicle %u at %.1f %.1f %.1f (moved by %.4f s)\n", idx,
                    *(volatile float *)gm(m + 0x24), *(volatile float *)gm(m + 0x28),
                    *(volatile float *)gm(m + 0x2C), (double)acc[f].dt);
    }
    *pdt = acc[f].dt;                   /* the time since it last moved */
    *preal = acc[f].real;
    acc[f].dt = acc[f].real = 0.0f;
    sub_00207790_gen();
    *pdt = dt;
    *preal = real;
}

/* The sound manager: is anything still playing? (this, flag; ret 4).
 * "No" while a wait for quiet is given up (audio_halt_wait), else the
 * game's own answer. */
extern void sub_000992B0_gen(void);
extern int rdr_lockstep_mode(void);
extern int xbox_GuestCpuRelease(int kind, uint32_t where);
extern void xbox_GuestCpuAcquire(void);
extern void rdr_net_sleep_ms(int ms);
int g_rdr_test_sound_busy;              /* tests (RDR_OVERLAY_FLAG "soundbusy 1"): always "playing" */
void sub_000992B0(void)
{
    if (s_quiet_until > 0.0 && rdr_now_seconds() < s_quiet_until) {
        g_eax &= ~0xFFu;                /* al = 0: quiet */
        g_esp += 8;                     /* ret 4 */
        return;
    }
    if (g_rdr_test_sound_busy) {
        g_eax = (g_eax & ~0xFFu) | 1u;  /* al = 1: still playing */
        g_esp += 8;
        return;
    }
    if (rdr_lockstep_mode()) {
        /* Online (and the lockstep tests): the sound plays by the clock on
         * the wall, so a wait for quiet a frame at a time ended on a frame
         * of each machine's own -- the games part (a slower machine more
         * so). Here the wait is done inside the frame instead: asked
         * again, with the CPU handed to the other threads between, until
         * quiet or 0.3 s, and then "quiet" -- on the first ask, on every
         * machine alike. */
        uint32_t ecx = g_ecx, ret = *(volatile uint32_t *)gm(g_esp), arg = *(volatile uint32_t *)gm(g_esp + 4);
        double t0 = rdr_now_seconds();
        for (;;) {
            sub_000992B0_gen();         /* pops the return address and the argument */
            if (!(g_eax & 0xFFu))
                return;
            if (rdr_now_seconds() - t0 > 0.3) {
                static unsigned said;
                if (said++ < 5) {
                    fprintf(stderr, "[SND] online: the sound still playing after 0.3 s; taken as quiet\n");
                    fflush(stderr);
                }
                g_eax &= ~0xFFu;
                return;
            }
            g_esp -= 8;
            *(volatile uint32_t *)gm(g_esp) = ret;
            *(volatile uint32_t *)gm(g_esp + 4) = arg;
            g_ecx = ecx;
            {
                int rel = xbox_GuestCpuRelease(0, 238);
                rdr_net_sleep_ms(2);
                if (rel)
                    xbox_GuestCpuAcquire();
            }
        }
    }
    sub_000992B0_gen();
}

void sub_000175B0(void)
{
    uint32_t fmt = *(volatile uint32_t *)gm(g_esp + 4);
    unsigned i;
    if (g_rdr_npc_making && ((fmt == FMT_NO_CANNON && *(volatile uint32_t *)gm(g_esp) == 0x000E2E0Cu)
                             || (fmt == FMT_NO_CANNON_CMD && *(volatile uint32_t *)gm(g_esp) == 0x000FF12Du))) {
        fprintf(stderr, "[NPC] the AI table names a cannon not in this level (%u)\n",
                *(volatile uint32_t *)gm(g_esp + 12));
        fflush(stderr);
        g_rdr_npc_bad = 1;
        g_esp += 4;
        return;
    }
    if (g_rdr_npc_making && fmt == FMT_NO_CRACK_ZONE && *(volatile uint32_t *)gm(g_esp) == 0x000E2E7Au) {
        uint32_t mgr = *(volatile uint32_t *)gm(0x0065AA0Cu), z = 0;
        static unsigned logged;
        if (mgr >= 0x10000u && (*(volatile uint16_t *)gm(mgr + 0x50)) > 0) {
            uint32_t arr = *(volatile uint32_t *)gm(mgr + 0x4C);
            if (arr >= 0x10000u)
                z = *(volatile uint32_t *)gm(arr);
        }
        if (z && g_edi >= 0x10000u) {
            *(volatile uint32_t *)gm(g_edi + 8) = z;
            if (logged++ < 20) {
                fprintf(stderr, "[NPC] the AI table names crack zone %u, not in this level: given zone %08X\n",
                        *(volatile uint32_t *)gm(g_esp + 8), z);
                fflush(stderr);
            }
            g_esp += 4;
            return;
        }
        g_rdr_npc_bad = 1;              /* (no zones at all) */
        g_esp += 4;
        return;
    }
    if (fmt == 0x003C602Cu || fmt == 0x003CCA84u || fmt == 0x003CCAACu || fmt == 0x003CD8D8u
        || fmt == 0x003CD6B8u)
        audio_halt_wait();
    for (i = 0; i < sizeof s_soft_fatal / sizeof s_soft_fatal[0]; i++)
        if (fmt == s_soft_fatal[i]) {
            static unsigned logged;
            if (logged++ < 20) {
                fprintf(stderr, "[RDR] fatal error skipped (from 0x%08X): %s\n",
                        *(volatile uint32_t *)gm(g_esp), (const char *)gm(fmt));
                fflush(stderr);
            }
            g_esp += 4;                 /* ret: the caller pops the arguments */
            return;
        }
    fprintf(stderr, "[RDR] fatal error (from 0x%08X): %s\n",
            *(volatile uint32_t *)gm(g_esp), (const char *)gm(fmt));
    fflush(stderr);
    if (fmt == 0x003CBA18u || fmt == 0x003CB9E4u) {     /* Free(%p) - Prev / Next node ... */
        extern void rdr_heap_bad_free(uint32_t ptr);
        rdr_heap_bad_free(*(volatile uint32_t *)gm(g_esp + 8));
    }
    sub_000175B0_gen();
}

/* ── Play as anyone ────────────────────────────────────────────────
 *
 * sub_00138090 makes a level's players from descriptors at 0x00653338
 * (0x5C each: +0 the player number or -1, +4 the character type -- a
 * .swt file in swat.dat: "srd" is the story's Red, "mjck" Showdown's Jack
 * Swift -- +0x18 a second character for the same player). In the story,
 * "Play as" (rdr_cheats.c) writes its choice over player 1's type on the
 * way in, so the level makes that character instead: its model, animations,
 * and what its type file gives it. */
extern const char *rdr_play_as(void);

void sub_00138090_enter(void)
{
    volatile char *name = (volatile char *)gm(0x00653338u + 4);
    const char *want = rdr_play_as();
    char was[20];
    int i;
    if (*(volatile uint32_t *)gm(0x00652E30u) != 0) {
        /* Showdown picks its own -- but a test (RDR_SHOWDOWN_AS=mqls)
         * makes every player that character; a list (wod,wsa,wia) gives
         * the players one each, in turn */
        const char *e = getenv("RDR_SHOWDOWN_AS");
        int k, n = 0;
        if (e && *e)
            for (k = 0; k < 4; k++) {
                volatile char *t = (volatile char *)gm(0x00653338u + (uint32_t)k * 0x5Cu + 4);
                const char *w = e;
                int j;
                if (!t[0])
                    continue;
                for (j = n++; j > 0; j--) {
                    const char *c = strchr(w, ',');
                    w = c ? c + 1 : e;
                }
                for (i = 0; i < 19 && w[i] && w[i] != ','; i++)
                    t[i] = w[i];
                t[i] = 0;
                fprintf(stderr, "[RDR] Showdown player %d (%d): as %s (test)\n", k,
                        *(volatile int32_t *)gm(0x00653338u + (uint32_t)k * 0x5Cu), (const char *)t);
            }
        return;
    }
    for (i = 0; i < 19 && name[i]; i++)
        was[i] = name[i];
    was[i] = 0;
    if (!want || !*want) {
        fprintf(stderr, "[RDR] level player: %s\n", was);
        fflush(stderr);
        return;
    }
    for (i = 0; i < 19 && want[i]; i++)
        name[i] = want[i];
    name[i] = 0;
    fprintf(stderr, "[RDR] level player: %s, playing as %s\n", was, want);
    fflush(stderr);
}

/* Story co-op experiment (RDR_COOP_TEST=1, rdr_overrides.c): right after a
 * level has made its players -- sub_001EA7D0 called from 0x00062BC0, just
 * after sub_00138090 -- make a second one. */
extern void rdr_coop_level_start(void);

void sub_001EA7D0_enter(void)
{
    static int say = -1;
    if (say < 0)
        say = getenv("RDR_COOP_LOG") != NULL;
    if (say) {
        fprintf(stderr, "[COOP] players made (from %08X)\n", *(volatile uint32_t *)gm(g_esp));
        fflush(stderr);
    }
    if (*(volatile uint32_t *)gm(g_esp) == 0x00062BC5u)
        rdr_coop_level_start();
}

/* sub_00051970 draws every view of the frame ([0x00652B48] of them at
 * 0x00652B2C): co-op arranges them first (rdr_overrides.c). */
extern void rdr_coop_views(void);
void sub_00051970_enter(void)
{
    rdr_coop_views();
}

/* RDR_FILE_LOG=1: every name looked up in a Dave archive (sub_000D49C0,
 * thiscall: the archive, then the name), for working out what a level
 * loads. A substring in RDR_FILE_LOG_ONLY narrows it. */
void sub_000D49C0_enter(void)
{
    static int on = -1;
    static const char *only;
    uint32_t name;
    char buf[96];
    unsigned i;
    if (on < 0) {
        const char *e = getenv("RDR_FILE_LOG");
        on = e && *e == '1';
        only = getenv("RDR_FILE_LOG_ONLY");
    }
    if (!on)
        return;
    name = *(volatile uint32_t *)gm(g_esp + 4);
    if (!name || name >= 0x84000000u)
        return;
    for (i = 0; i < sizeof buf - 1; i++)
        if (!(buf[i] = *(volatile char *)gm(name + i)))
            break;
    buf[i] = 0;
    if (only && *only && !strstr(buf, only))
        return;
    fprintf(stderr, "[ARCHIVE] %08X: %s\n", g_ecx, buf);
    fflush(stderr);
}

/* Tests (RDR_PRIM_LOG=<file>): while <file> exists, the game's immediate
 * drawing (sub_001825D0: begin a primitive of `count` vertices of `type`)
 * is logged with its caller, 400 calls at most -- to find which code draws
 * a given shape. */
void sub_001825D0_enter(void)
{
    static const char *flag, *pts;
    static int checked, n;
    static unsigned tick, pts_tick, pts_caller;
    if (!checked) {
        flag = getenv("RDR_PRIM_LOG");
        pts = getenv("RDR_PRIM_POINTS");
        checked = 1;
    }
    /* (tests: the caller named in the file RDR_PRIM_POINTS draws points
     * instead -- to see which code draws a given shape) */
    if (pts) {
        if ((pts_tick++ & 1023) == 0) {
            FILE *f = fopen(pts, "rb");
            pts_caller = 0;
            if (f) {
                if (fscanf(f, "%x", &pts_caller) != 1)
                    pts_caller = 0;
                fclose(f);
            }
        }
        if (pts_caller && *(volatile uint32_t *)gm(g_esp) == pts_caller)
            *(volatile uint32_t *)gm(g_esp + 4) = 1;
    }
    if (!flag || n >= 400)
        return;
    if ((tick++ & 255) == 0 || n > 0) {
        FILE *f = n > 0 ? NULL : fopen(flag, "rb");
        if (n == 0) {
            if (!f)
                return;
            fclose(f);
        }
        n++;
        fprintf(stderr, "[PRIM] type %u count %u from %08X\n",
                *(volatile uint32_t *)gm(g_esp + 4), *(volatile uint32_t *)gm(g_esp + 8),
                *(volatile uint32_t *)gm(g_esp));
        if (n >= 400)
            fflush(stderr);
    }
}

/* Story co-op: a level is being made current (sub_0007A250, the level
 * object in the argument) -- the end of a co-op level (rdr_overrides.c). */
extern void rdr_coop_level_switch(uint32_t level);

void sub_0007A250_enter(void)
{
    rdr_coop_level_switch(*(volatile uint32_t *)gm(g_esp + 4));
}

/* ── Replacement cutscenes ──────────────────────────────────────
 *
 * The game's movies are Bink files (bik\<name>.bik), played by
 * sub_000705A0(names, count, ...): it builds "%sbik\%s.bik" for each
 * (0x003CDB40), sets the music and the screen up for them and opens them
 * (sub_001F7500); 0 back when they could not be opened, and its callers go
 * on as after the movie. A movie with a replacement in the play folder's
 * Mods\videos\ (<name>.mp4 or .wmv, any size and frame rate, with its
 * own sound) is played by the toolkit instead (xbox_MoviePlay, Windows'
 * own decoder, full window), and then the game's own call is made with no
 * movies in it: the same set-up, and "nothing to play". RDR_TEST_MOVIES=1
 * (tests): every movie taken as replaced, and none played. */
static uint32_t g32v(uint32_t va);
extern void sub_000705A0_gen(void);
extern int xbox_MoviePlay(const char *const *paths, int n);

static int movie_replacement(const char *name, char *out, size_t n)
{
    static const char *const ext[] = { ".mp4", ".wmv", ".m4v" };
    const char *m = getenv("RECOMP_MODS_DIR");
    unsigned i;
    if (!m || !*m)
        return 0;
    for (i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        FILE *f;
        snprintf(out, n, "%s%svideos/%s%s", m, (m[strlen(m) - 1] == '\\' || m[strlen(m) - 1] == '/') ? "" : "/",
                 name, ext[i]);
        f = fopen(out, "rb");
        if (f) {
            fclose(f);
            return 1;
        }
    }
    return 0;
}

void sub_000705A0(void)
{
    uint32_t names = g32v(g_esp + 4), count = g32v(g_esp + 8);
    static int test = -1;
    char path[4][600], name[64];
    const char *pp[4];
    uint32_t k;
    int all = count >= 1 && count <= 4;
    if (test < 0)
        test = getenv("RDR_TEST_MOVIES") != NULL;
    {
        /* Online (lockstep): no movies. A Bink movie plays by the clock on
         * the wall while the main loop goes on counting frames, so each
         * game came out of it at a frame of its own (users, Chapter 2's
         * bridge01.bik: the host waited 100 s for the guest, then the
         * games had parted). Every game skips it the same way. */
        extern int rdr_lockstep_mode(void);
        if (rdr_lockstep_mode() == 4 && count >= 1 && count <= 4) {
            uint32_t slot = g_esp + 8;
            fprintf(stderr, "[MOVIE] online: %u movie(s) left out (every game the same)\n", count);
            fflush(stderr);
            *(volatile uint32_t *)gm(slot) = 0;
            sub_000705A0_gen();
            *(volatile uint32_t *)gm(slot) = count;
            return;
        }
    }
    for (k = 0; all && k < count; k++) {
        uint32_t a = g32v(names + 4 * k);
        unsigned i;
        for (i = 0; i + 1 < sizeof name && a && *(volatile char *)gm(a + i); i++)
            name[i] = *(volatile char *)gm(a + i);
        name[i] = 0;
        pp[k] = path[k];
        if (!name[0] || (!movie_replacement(name, path[k], sizeof path[k]) && !test))
            all = 0;
        else if (test)
            snprintf(path[k], sizeof path[k], "(test) %s", name);
    }
    if (all) {
        int played = test ? (int)count : xbox_MoviePlay(pp, (int)count);
        fprintf(stderr, "[MOVIE] %u movie(s), %s: %s%s\n", count, path[0],
                played > 0 ? "played the replacement" : "the replacement did not play; the game's own",
                count > 1 ? " (and the rest)" : "");
        fflush(stderr);
        if (played > 0) {
            uint32_t slot = g_esp + 8;
            *(volatile uint32_t *)gm(slot) = 0;     /* the game's call, with none in it */
            sub_000705A0_gen();
            *(volatile uint32_t *)gm(slot) = count;
            return;
        }
    }
    sub_000705A0_gen();
}

/* ── Split screen without the letterbox bars ──────────────────────
 *
 * The story's letterbox is two black quads, 31 lines at the top and the
 * same at the bottom of each view (sub_001D5F80, from the film system's
 * per-view drawing at 0x001D9ADC; found by logging the callers of the
 * immediate-mode begin, sub_001825D0, while a pixel probe watched the
 * black band). With two views stacked that drew a band across the middle
 * of the screen and covered the objective text at the top. While split
 * screen shows the players' views (rdr_overrides.c) they are not drawn; a
 * cutscene, full screen, keeps them. */
extern void sub_001D5F80_gen(void);
extern void sub_00181530(void);
extern int rdr_coop_split_shown(void);
extern volatile int g_rdr_freecam;

/* (Nor with the free camera on: the quads sit in front of the game's
 * camera, and seen from elsewhere they cut black wedges across the
 * picture -- user video.) */
/* (Nor when the player switched them off, Display: "Black bars" -- user:
 * "an option to disable these bars so we can have a full widescreen". The
 * picture under them is drawn in full either way; they only cover it.) */
extern int rdr_cinema_playing(void);

static int letterbox_off(void)
{
    int lb = g_rdr_settings.letterbox;
    return lb == 0 || (lb == 1 && !rdr_cinema_playing());
}

extern void rdr_letterbox_film(uint32_t film);

void sub_001D5F80(void)
{
    rdr_letterbox_film(g_ecx);          /* (the cutscene bands: rdr_overrides.c) */
    if (rdr_coop_split_shown() || g_rdr_freecam || letterbox_off()) {
        /* Not drawn; but the drawing ends by setting the colour that
         * tints what is drawn next back to white (sub_00181530(-1), after
         * the HUD's own fade for the bars) and the HUD is drawn after it:
         * left as a cutscene or duel had set it, the HUD stayed
         * see-through after it with the bars off (user). */
        if (*(volatile uint32_t *)gm(0x0067A138u)) {
            uint32_t esp0 = g_esp;
            g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = 0xFFFFFFFFu;
            g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = 0x001D6340u;
            sub_00181530();
            g_esp = esp0;
        }
        g_esp += 4;                     /* ret */
        return;
    }
    sub_001D5F80_gen();
}

/* ── A pose blend with no pose (user: the preview's Bat Boy, taking off in
 * a Showdown of preview characters, crashed the game) ───────────────
 *
 * sub_000DA930(dst; a, b, ...) blends two poses ({short count; float
 * *channels}) into this one. The user's crash (lvl_hng2, the flight's
 * climb: sub_0025B200 -> sub_000D8E60 -> sub_000D8B40 -> here) had a pose
 * whose channels pointer was text ('clot', 0x746F6C63): an animation that
 * is not there. Not seen in the harness (the same Bat Boy, four of him or
 * against other preview characters). A blend with a pose outside the
 * game's memory is skipped (the pose keeps its last frame) and logged once,
 * with who called, instead of ending the game. */
extern void sub_000DA930_gen(void);

static int pose_addr(uint32_t p)
{
    return (p >= 0x00010000u && p < 0x04000000u) || (p >= 0x80000000u && p < 0x84000000u);
}

static int pose_ok(uint32_t p)
{
    return pose_addr(p) && pose_addr(*(volatile uint32_t *)gm(p + 4));
}

void sub_000DA930(void)
{
    /* (thiscall, five arguments: the poses are the second and third) */
    uint32_t a = *(volatile uint32_t *)gm(g_esp + 8), b = *(volatile uint32_t *)gm(g_esp + 12);
    if (!pose_ok(g_ecx) || !pose_ok(a) || !pose_ok(b)) {
        static int said;
        if (said++ < 4) {
            fprintf(stderr, "[ANIM] pose blend skipped: dst %08X a %08X b %08X (channels %08X %08X), from %08X\n",
                    g_ecx, a, b, pose_addr(a) ? *(volatile uint32_t *)gm(a + 4) : 0,
                    pose_addr(b) ? *(volatile uint32_t *)gm(b + 4) : 0, *(volatile uint32_t *)gm(g_esp));
            fflush(stderr);
        }
        g_esp += 4 + 0x14;                  /* ret 0x14 */
        return;
    }
    sub_000DA930_gen();
}

/* ── Character models from .xmod files ────────────────────────────
 *
 * A character's models come from its resource pack only: the loaders for
 * its LODs ("%s_lod_%02d", 0x00273F58), hat (0x00273FC0, 0x0027402C,
 * 0x00274062) and face (0x001ED01B) ask sub_00084140(name, 'MOD0'), which
 * searches the loaded name tables (<name>_lods.xrnt / .xrsc) and nothing
 * else. Levels have the fall-back characters lack: sub_00184730 tries the
 * same and then loads "<name>.xmod" from the archives (sub_000846B0 ->
 * sub_00184450, quiet when the file is not there). So a character added
 * with its own models as plain .xmod files (tools/mods/addchar.py: the
 * 2002 preview's Red) came out invisible -- only the gun he held was
 * drawn. A miss from those character loaders now loads "<name>.xmod" when
 * the archives have it (below). */
extern void sub_00084140_gen(void);
extern void sub_001846B0(void);
extern RECOMP_TLS uint32_t g_eax;
static int guest_file_exists(const char *name, const char *ext);

/* This used to ask sub_00184870 -- the level path, which goes through the
 * game's table of loaded models (0x00678998) -- on every miss. Retail
 * characters do miss: many have no second LOD or no hat (own02_lod_02,
 * hat_mom_lod_01, ...; harness logs), and each miss left an entry in that
 * table, "<name>" -> nothing, in a block taken from whichever heap was in
 * use while the character loaded. The level path only ever fills it while
 * a level's own models load; a character's heap is let go with the
 * character, and the entry stayed in the table: the next lookup in its
 * bucket followed it into whatever had taken that memory (user's crash in
 * lvl_bank: sub_000865E0 reading 0xBF800000, a -1.0f, from the table,
 * called from the LOD loader at 0x00273F5D), and a removal could write
 * into it (heap damage). Now only a model whose "<name>.xmod" is in the
 * archives is loaded, by the loader itself (sub_001846B0, "model"), with no
 * entry in the table: a model the table does not hold is released by
 * sub_001847B0 all the same (it looks for it by value and goes on). A
 * retail miss stays a miss, as on the console. */
void sub_00084140(void)
{
    uint32_t ret  = *(volatile uint32_t *)gm(g_esp);
    uint32_t name = *(volatile uint32_t *)gm(g_esp + 4);
    uint32_t type = *(volatile uint32_t *)gm(g_esp + 8);
    sub_00084140_gen();                         /* pops the return address */
    if (g_eax == 0 && type == 0x30444F4Du
        && (ret == 0x00273F5Du || ret == 0x00273FC5u || ret == 0x00274031u
            || ret == 0x00274067u || ret == 0x001ED020u)) {
        char buf[64];
        unsigned i;
        for (i = 0; i < sizeof buf - 1; i++)
            if (!(buf[i] = *(volatile char *)gm(name + i)))
                break;
        buf[i] = 0;
        if (buf[0] && i < sizeof buf - 1 && guest_file_exists(buf, "xmod")) {
            uint32_t esp = g_esp, ecx = g_ecx;
            g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = 0;
            g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = 0;
            g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = name;
            g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = 0x003DF080u;  /* "model" */
            g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = ret;   /* as if from the loader */
            sub_001846B0();                     /* cdecl: pops the return address */
            g_esp = esp;
            g_ecx = ecx;
            {
                static unsigned logged;
                if (logged++ < 16) {
                    fprintf(stderr, "[RDR] character model %s: no pack, %s.xmod -> %08X\n",
                            buf, buf, g_eax);
                    fflush(stderr);
                }
            }
        }
    }
}

/* ...and the loader that would ask: once a character's resource pack
 * is in (sub_00274400, from sub_00274550 and the pack's completion at
 * 0x00073CC5), its LODs, hat and the rest are asked for (sub_00273F20)
 * -- but with no pack at all (no "<name>_lods.xrsc") it reports done and
 * asks for nothing: the character is invisible. When there is no pack and
 * "<name>_lod_01.xmod" is in the archives, the models are now asked for
 * anyway (pack 0), and the misses above load them from the files.
 *
 * Arguments (cdecl): a1 the name, a2 the LOD list, a3 its count, a5 the
 * pack, a6 its name table, a7 "still to load" (a byte), a8, a9, a10 the
 * loader's further lists; a4[count] gets the end marker 0x4B3C61BD. */
extern void sub_00274400_gen(void);
extern void sub_00273F20(void);
extern void sub_00083CC0(void);

static int guest_file_exists(const char *name, const char *ext)
{
    /* sub_00083CC0(this = the archives [0x0065ABB0], name, ext): thiscall,
     * ret 8. The strings go on the guest stack, below everything live. */
    uint32_t esp = g_esp, ecx = g_ecx, eax = g_eax, sn, se;
    size_t ln = strlen(name) + 1, le = strlen(ext) + 1, i;
    g_esp -= 0x80;
    sn = g_esp; se = g_esp + 0x40;
    if (ln > 0x40 || le > 0x40) { g_esp = esp; return 0; }
    for (i = 0; i < ln; i++) *(volatile char *)gm(sn + (uint32_t)i) = name[i];
    for (i = 0; i < le; i++) *(volatile char *)gm(se + (uint32_t)i) = ext[i];
    g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = se;
    g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = sn;
    g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = 0x002744A9u;       /* ret */
    g_ecx = *(volatile uint32_t *)gm(0x0065ABB0u);
    sub_00083CC0();
    {
        int r = (g_eax & 0xFF) != 0;
        g_esp = esp; g_ecx = ecx; g_eax = eax;
        return r;
    }
}

void sub_00274400(void)
{
    uint32_t a[12];
    unsigned i;
    for (i = 1; i <= 11; i++)
        a[i] = *(volatile uint32_t *)gm(g_esp + 4 * i);
    sub_00274400_gen();                         /* pops the return address */
    if (a[5] == 0 && a[1] && a[7] && *(volatile uint8_t *)gm(a[7])) {
        char nm[64], lod[80];
        uint32_t esp = g_esp, eax = g_eax;
        for (i = 0; i < sizeof nm - 1; i++)
            if (!(nm[i] = *(volatile char *)gm(a[1] + i)))
                break;
        nm[i] = 0;
        snprintf(lod, sizeof lod, "%s_lod_01", nm);
        if (nm[0] && guest_file_exists(lod, "xmod")) {
            static const unsigned order[7] = { 2, 3, 1, 8, 9, 10, 6 };
            int k;
            fprintf(stderr, "[RDR] character %s: no resource pack, models from .xmod files\n", nm);
            fflush(stderr);
            for (k = 6; k >= 0; k--) {
                g_esp -= 4;
                *(volatile uint32_t *)gm(g_esp) = order[k] == 6 ? 0 : a[order[k]];
            }
            g_esp -= 4; *(volatile uint32_t *)gm(g_esp) = 0x00274446u;   /* ret */
            sub_00273F20();
            g_esp = esp;
            *(volatile uint32_t *)gm(a[4] + 4 * *(volatile uint32_t *)gm(a[3])) = 0x4B3C61BDu;
            *(volatile uint8_t *)gm(a[7]) = 0;
        }
        g_eax = eax;
    }
}

/* ── A swui page holds 150 elements: more for the line-up ──────────────
 *
 * A swui page keeps its elements in a list of 150 inside itself (+0x40,
 * the count at +0x298); sub_00019EF0 adds one with no check, and past 150
 * wrote over the page (the line-up poster with 158 characters crashed:
 * BRINGUP 92), so the line-up held 144 characters (user: "If there is a
 * 144 character limit, can we extend it?"). The 151st element on and are
 * kept in a list of our own for that page, and the lookup by name
 * (sub_00019D60, behind sub_00019E00 and the rest) finds them there too.
 * They are only the line-up's markers (the last in characterlineup.swui):
 * points on the photo, found by name, never drawn -- the page's own loops
 * over its list do not need them. */
extern void sub_00019EF0_gen(void);
/* ── An overflow heap ────────────────────────────────────────────
 * The game's memory is memMemoryAllocator heaps (the list at
 * [0x0065A5F8]; sub_0005D510 makes one and links it in, sub_0005CD10 gives
 * it its memory): a bump region [+8, +8 + +0xC), +0x10 used, with free
 * lists and small-block pools (+0xE8, pages of up to 0x7800 bytes taken
 * from the heap itself). sub_0005D650 (thiscall: size, three more; ret
 * 0x10) allocates; a heap that runs out ends the game ("Heap overrun (%d
 * bytes requested)", 0x0005D83C). A free finds its heap by address
 * (memMemoryAllocator::FindHeap over the list), so a block can come from
 * any heap in the list.
 *
 * The game sizes its heaps to what its levels load: the master heap
 * (0x004257E0, 46 MB from 0x81000000) holds the level's heaps and is left
 * with about 1 KB; the spawner bringing in another level's character or
 * AI table needs more (harness, the ranch: "Heap overrun (1152 bytes
 * requested)", from bankrobbery_gunman). So a heap of our own, made the
 * first time it is needed, from what the console's RAM has left
 * (xbox_HeapAlloc, 8 MB or less; the low RAM, apart from the contiguous
 * window the game's heaps are in), takes a request a big heap (1 MB or
 * more) has no room for -- only one that would have ended the game, as its
 * allocator would look (heap_has_room). A small request goes whole, pool
 * page and all, so a pool's blocks and their page are in the same heap.
 * Logged, the first times and when ours fills. */
extern void sub_0005D650_gen(void);
extern void sub_0005D510(void), sub_0005CD10(void);
extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
#define HEAP_PAGE_MAX   0x7800u
static uint32_t s_over_heap;
static int s_over_tried;
static unsigned s_over_n;

static uint32_t hp32(uint32_t va) { return *(volatile uint32_t *)gm(va); }

static void over_heap_make(void)
{
    static const uint32_t sizes[] = { 8u << 20, 4u << 20, 2u << 20, 1u << 20 };
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, edx0 = g_edx, obj = 0, size = 0;
    unsigned i;
    s_over_tried = 1;
    for (i = 0; i < sizeof sizes / sizeof sizes[0] && !obj; i++) {
        obj = xbox_HeapAlloc(sizes[i] + 0x200, 16);
        size = sizes[i];
    }
    if (!obj) {
        fprintf(stderr, "[HEAP] no memory left for an overflow heap\n");
        fflush(stderr);
        return;
    }
    memset((void *)gm(obj), 0, 0x200);
    g_esp -= 64;
    guest_push(0x0005DCECu);            /* (a return address in the allocator) */
    g_ecx = obj;
    sub_0005D510();                     /* its fields; linked into the list */
    g_esp = esp0 - 64;
    guest_push(0);                      /* as the game's own: (base, size, 1, 0) */
    guest_push(1);
    guest_push(size);
    guest_push(obj + 0x200);
    guest_push(0x0005DCECu);
    g_ecx = obj;
    sub_0005CD10();
    g_esp = esp0; g_ecx = ecx0; g_eax = eax0; g_edx = edx0;
    s_over_heap = obj;
    fprintf(stderr, "[HEAP] overflow heap %08X: %u KB at %08X\n", obj, size >> 10, obj + 0x200);
    fflush(stderr);
}

/* Whether heap could give `size` bytes without running out: what its
 * allocator would do -- a block on its free lists (+0x24, 31 lists by
 * size: [block+4] the size, [block+0x14] the next; one the same size or
 * more than 16 bytes bigger) or the rest of its bump region. A small
 * request (64 bytes or less) is a small-block pool's, and a pool may need a
 * page (up to 0x7800) for it: asked for as such. */
static int heap_has_room(uint32_t heap, uint32_t size)
{
    uint32_t need, left, k, b;
    int guard = 0;
    if (size <= 0x40 && !*gm(heap + 0x23))
        size = HEAP_PAGE_MAX;
    need = (size + (*gm(heap + 0x22) ? 0x14u : 0u) + 0xFu) & ~0xFu;
    left = hp32(heap + 0xC) - hp32(heap + 0x10);
    if (hp32(heap + 0x10) <= hp32(heap + 0xC) && left >= need + 0x40)
        return 1;
    if (!*gm(heap + 0x20))
        return 0;                       /* (no free lists: the bump only) */
    for (k = 0; k < 31; k++)
        for (b = hp32(heap + 0x24 + 4 * k); b && guard < 8192; b = hp32(b + 0x14), guard++) {
            uint32_t bs = hp32(b + 4);
            if (bs == need || bs > need + 0x10)
                return 1;
        }
    return 0;
}

void sub_0005D650(void)
{
    static RECOMP_TLS int depth;
    static int told_full;
    uint32_t heap = g_ecx, esp0 = g_esp, size = hp32(g_esp + 4);
    /* (a call from inside one -- a pool's page -- stays with its heap: a
     * pool's blocks and their page are in the same heap) */
    if (!depth && heap && heap != s_over_heap && size && size < (16u << 20)
            && hp32(heap + 0xC) >= (1u << 20) && !heap_has_room(heap, size)) {
        if (!s_over_heap && !s_over_tried)
            over_heap_make();
        if (s_over_heap && heap_has_room(s_over_heap, size)) {
            uint32_t args[5], k;
            for (k = 0; k < 5; k++)
                args[k] = hp32(esp0 + 4 * k);
            g_ecx = s_over_heap;
            depth++;
            sub_0005D650_gen();
            depth--;
            if (g_eax) {
                if (s_over_n++ < 8)
                    fprintf(stderr, "[HEAP] %u bytes from the overflow heap: heap %08X has %u left\n", size,
                            heap, hp32(heap + 0xC) - hp32(heap + 0x10));
                return;
            }
            g_ecx = heap;               /* (it popped its arguments) */
            g_esp = esp0;
            for (k = 0; k < 5; k++)
                *(volatile uint32_t *)gm(esp0 + 4 * k) = args[k];
        } else if (s_over_heap && !told_full) {
            told_full = 1;
            fprintf(stderr, "[HEAP] the overflow heap is full too (%u bytes asked of heap %08X)\n", size, heap);
            fflush(stderr);
        }
    }
    depth++;
    sub_0005D650_gen();
    depth--;
}

/* ── Guns in the stores ──────────────────────────────────────────
 * Inside a store (a level zone of type 3) the player's update keeps the
 * guns holstered and the fire buttons dead, as Brimstone's streets do with
 * type 4 (rdr_overrides.c town_guns_zones) -- and the same bit gives the
 * store its fixed camera. sub_00145FE0 (thiscall on the player, from its
 * update) tests the zones: sub_0007EE20(point, 3, 0) for a store
 * (returning at 0x0014617A) sets bit 0 of the player's bit set
 * ([player+0xA04]); the draw check (0x0014A691), the fire check
 * (0x001461C4) and the camera (sub_00149970, from 0x0014AA23, and what it
 * calls: sub_00141D00, sub_00148E30, sub_00149550) read it. With Guns in
 * Town on, the store test answers "no" -- guns and fire as in the street
 * -- and the bit is set again for the camera's call alone, so the store
 * keeps its camera. Clearing the zone's type instead let Red draw, but the
 * street's camera looked in through the walls (harness, 143). */
extern void sub_00145FE0_gen(void);
extern void sub_0007EE20_gen(void);
extern void sub_00149970_gen(void);
extern int rdr_town_guns(void);
#define MAX_STORE_PL 8
static uint32_t s_store_pl[MAX_STORE_PL];   /* players found in a store this update */
static uint32_t s_zone_player;

static void store_mark(uint32_t pl, int in)
{
    int i, free_i = -1;
    for (i = 0; i < MAX_STORE_PL; i++) {
        if (s_store_pl[i] == pl) {
            if (!in)
                s_store_pl[i] = 0;
            return;
        }
        if (!s_store_pl[i] && free_i < 0)
            free_i = i;
    }
    if (in && free_i >= 0)
        s_store_pl[free_i] = pl;
}

static int store_in(uint32_t pl)
{
    int i;
    for (i = 0; pl && i < MAX_STORE_PL; i++)
        if (s_store_pl[i] == pl)
            return 1;
    return 0;
}

void sub_00145FE0(void)
{
    uint32_t prev = s_zone_player;
    s_zone_player = g_ecx;
    sub_00145FE0_gen();
    s_zone_player = prev;
}

void sub_0007EE20(void)
{
    uint32_t ret = hp32(g_esp), type = hp32(g_esp + 8), pl = s_zone_player;
    static int told;
    sub_0007EE20_gen();
    if (ret != 0x0014617Au || type != 3 || !pl)
        return;
    store_mark(pl, g_eax != 0 && rdr_town_guns());
    if (g_eax && rdr_town_guns()) {
        if (!told) {
            told = 1;
            fprintf(stderr, "[RDR] guns in town: in a store, guns free and its camera kept\n");
            fflush(stderr);
        }
        g_eax = 0;
    }
}

void sub_00149970(void)
{
    uint32_t pl = g_ecx, bits = 0;
    if (store_in(pl)) {
        bits = hp32(pl + 0xA04);
        if (bits >= 0x10000u)
            *(volatile uint8_t *)gm(bits) |= 1;
        else
            bits = 0;
    }
    sub_00149970_gen();
    if (bits)
        *(volatile uint8_t *)gm(bits) &= (uint8_t)~1u;
}

extern void sub_00019D60_gen(void);

#define SW_OVER_MAX 512
static uint32_t s_sw_over_page[4], s_sw_over[4][SW_OVER_MAX];
static int s_sw_over_n[4];

static int sw_over_slot(uint32_t page, int make)
{
    static int next;
    int k;
    for (k = 0; k < 4; k++)
        if (s_sw_over_page[k] == page)
            return k;
    if (!make)
        return -1;
    k = next++ & 3;
    s_sw_over_page[k] = page;
    s_sw_over_n[k] = 0;
    return k;
}

void sub_00019EF0(void)
{
    uint32_t page = g_ecx, el = *(volatile uint32_t *)gm(g_esp + 4);
    uint32_t n = *(volatile uint32_t *)gm(page + 0x298), keep;
    int k;
    if (n == 0 && (k = sw_over_slot(page, 0)) >= 0)
        s_sw_over_n[k] = 0;                 /* a new page where an old one was */
    if (n < 150) {
        sub_00019EF0_gen();
        return;
    }
    /* the game's own add (it ties the element to the page) into the last
     * slot, then that slot's element put back and this one kept aside */
    keep = *(volatile uint32_t *)gm(page + 0x40 + 149 * 4);
    *(volatile uint32_t *)gm(page + 0x298) = 149;
    sub_00019EF0_gen();
    *(volatile uint32_t *)gm(page + 0x40 + 149 * 4) = keep;
    *(volatile uint32_t *)gm(page + 0x298) = n;
    k = sw_over_slot(page, 1);
    if (s_sw_over_n[k] < SW_OVER_MAX)
        s_sw_over[k][s_sw_over_n[k]++] = el;
    if (s_sw_over_n[k] == 1) {
        fprintf(stderr, "[UI] page %08X: more than 150 elements, the rest kept aside\n", page);
        fflush(stderr);
    }
}

void sub_00019D60(void)
{
    uint32_t page = g_ecx, esp = g_esp, name = *(volatile uint32_t *)gm(esp + 4),
             type = *(volatile uint32_t *)gm(esp + 8), ret = *(volatile uint32_t *)gm(esp);
    int k, i;
    sub_00019D60_gen();
    if (g_eax || (k = sw_over_slot(page, 0)) < 0 || !s_sw_over_n[k])
        return;
    for (i = 0; i < s_sw_over_n[k]; i++) {
        uint32_t el = s_sw_over[k][i];
        const volatile char *a = (const volatile char *)gm(name), *b = (const volatile char *)gm(el + 0x28);
        int j;
        if (*(volatile uint32_t *)gm(el + 0x48) != type)
            continue;
        for (j = 0; j < 64; j++) {
            char x = a[j], y = b[j];
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
            if (x != y)
                break;
            if (!x) {
                g_eax = el;
                return;
            }
        }
    }
    (void)ret;
}

/* ── Showdown line-up: more than one page ───────────────────────────
 *
 * The line-up is one photo, 1024 x 256 (two 512 x 256 halves, photoleft /
 * photoright), built when the poster loads (sub_00043CD0): a street scene
 * (ui_characterlineupl/r24b) with each unlocked character's cut-out
 * (ui_mphoto_<code>, 128 x 256) drawn at its marker in characterlineup.swui
 * (<POSMARKER:x,0,0:w,h:hotspot_<code>>; x 0..1023, w,h the point the hand
 * aims at). The 48 retail characters fill it. The hand moves freely over the
 * photo (sub_0002E250, clamped to it) and picks the nearest marker
 * (sub_0002D430, through each marker's place on screen, sub_0002A090).
 *
 * A marker at x >= 1024 now puts its character on a further page: page p
 * holds x in [1024p, 1024(p+1)). With more than one page:
 *   - each page's photo is built once, at poster load, by running the
 *     game's own builder with that page's markers moved into 0..1023 and the
 *     others out of reach; the halves of each page are kept and the photo
 *     elements switched between them;
 *   - the markers of the shown page sit at x - 1024p, the others where that
 *     puts them (off the photo), so the hand's aim, the tokens and the name
 *     follow; for the nearest-marker search the other pages' markers are
 *     moved far away, so only the shown page can be picked;
 *   - holding the stick against the right edge of the photo turns to the
 *     next page (the hand comes in at the left), against the left edge to
 *     the one before;
 *   - the hand placed on a character (entering the line-up: the character
 *     picked last) turns to that character's page.
 * With one page (the retail roster) none of this runs.
 * The extra pages' photos are released with the poster (sub_00043BA0). */
typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern void sub_00043CD0_gen(void);
extern void sub_0002D430_gen(void);
extern void sub_0002ACD0_gen(void);
extern void sub_00043BA0_gen(void);

#define LU_MAXHS   512
#define LU_MAXPG   8
#define LU_PAGE_W  1024.0f
#define LU_FAR     (-100000.0f)
#define LU_LOGF(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

static struct { uint32_t el; float x0; int page; } s_lu_hs[LU_MAXHS];
static int s_lu_n, s_lu_pages = 1, s_lu_page, s_lu_log = -1;
static uint32_t s_lu_poster, s_lu_photo[4], s_lu_tex[LU_MAXPG][2];
static struct { float lastx; int stuck, cool; } s_lu_pl[4];

static float gf(uint32_t va) { return *(volatile float *)gm(va); }
static void sf(uint32_t va, float v) { *(volatile float *)gm(va) = v; }
static uint32_t g32v(uint32_t va) { return *(volatile uint32_t *)gm(va); }

static int lu_log(void)
{
    if (s_lu_log < 0) {
        const char *e = getenv("RDR_LINEUP_LOG");
        s_lu_log = e && *e == '1';
    }
    return s_lu_log;
}

/* A guest call: thiscall/stdcall/cdecl alike -- args pushed right to left,
 * a return address, ecx; the guest stack is put back afterwards whatever
 * the callee popped. Strings go below with lu_str. Returns eax. */
static uint32_t lu_call(uint32_t va, uint32_t ecx, int n, const uint32_t *a, uint32_t ret)
{
    uint32_t esp = g_esp, ecx0 = g_ecx, eax0 = g_eax, r;
    recomp_func_t f = recomp_lookup(va);
    int i;
    if (!f)
        return 0;
    for (i = n - 1; i >= 0; i--)
        guest_push(a[i]);
    guest_push(ret);
    g_ecx = ecx;
    f();
    r = g_eax;
    g_esp = esp; g_ecx = ecx0; g_eax = eax0;
    return r;
}

/* A string on the guest stack, below everything live (call before the
 * pushes of lu_call, with g_esp lowered; the caller restores g_esp). */
static uint32_t lu_str(const char *s)
{
    size_t n = strlen(s) + 1, i;
    g_esp -= (uint32_t)((n + 0x13) & ~0xFu);
    for (i = 0; i < n; i++)
        *(volatile char *)gm(g_esp + (uint32_t)i) = s[i];
    return g_esp;
}

/* sub_00019E00 / sub_00019D00 (thiscall on a swui page, ret 4): the
 * element named so, or 0. */
static uint32_t lu_find(uint32_t fn, uint32_t page, const char *name)
{
    uint32_t esp = g_esp, a[1], r;
    if (!page)
        return 0;
    a[0] = lu_str(name);
    r = lu_call(fn, page, 1, a, 0x00043E63u);
    g_esp = esp;
    return r;
}

static int lu_find_hs(uint32_t el)
{
    int i;
    for (i = 0; i < s_lu_n; i++)
        if (s_lu_hs[i].el == el)
            return i;
    return -1;
}

static void lu_add_hs(uint32_t el)
{
    float x;
    int p;
    if (!el || s_lu_n >= LU_MAXHS || lu_find_hs(el) >= 0)
        return;
    x = gf(el + 0x4C);
    p = (int)((x + gf(el + 0x58)) / LU_PAGE_W);
    if (p < 0) p = 0;
    if (p >= LU_MAXPG) p = LU_MAXPG - 1;
    s_lu_hs[s_lu_n].el = el;
    s_lu_hs[s_lu_n].x0 = x;
    s_lu_hs[s_lu_n].page = p;
    s_lu_n++;
    if (p + 1 > s_lu_pages)
        s_lu_pages = p + 1;
}

/* Pages with nobody on them are skipped: the story's people stand on the
 * third page when the preview's characters are there for the second
 * (story_npcs.py after beta_roster.py, Mods\Preview); with the preview's
 * switched off and Mods\Story People made before they went to the second
 * page, the line-up had an empty page between (user). Each marker of a
 * later page moves up by the empty pages before it. */
static void lu_close_gaps(void)
{
    int used[LU_MAXPG] = { 0 }, map[LU_MAXPG], i, p, np = 0;
    used[0] = 1;
    for (i = 0; i < s_lu_n; i++)
        used[s_lu_hs[i].page] = 1;
    for (p = 0; p < LU_MAXPG; p++)
        map[p] = used[p] ? np++ : -1;
    if (np == s_lu_pages)
        return;
    for (i = 0; i < s_lu_n; i++) {
        int d = s_lu_hs[i].page - map[s_lu_hs[i].page];
        s_lu_hs[i].x0 -= LU_PAGE_W * (float)d;
        s_lu_hs[i].page = map[s_lu_hs[i].page];
        sf(s_lu_hs[i].el + 0x4C, s_lu_hs[i].x0);
    }
    if (lu_log())
        LU_LOGF("[LINEUP] %d page(s) with nobody on them skipped\n", s_lu_pages - np);
    s_lu_pages = np;
}

/* Markers for page p: its own moved into the photo; the others by the same
 * shift (away: out of reach). Not "far": windows.h defines that away. */
static void lu_place(int p, int away)
{
    int i;
    for (i = 0; i < s_lu_n; i++)
        sf(s_lu_hs[i].el + 0x4C, (away && s_lu_hs[i].page != p)
                                     ? LU_FAR : s_lu_hs[i].x0 - LU_PAGE_W * (float)p);
}

/* A picked character's ring stands on the page its character is on: on
 * another page it is put out of sight, and back when its page shows again
 * or the pick is let go (user: player 1's ring round Red, page 1, stayed
 * over the story's people's page; then, player 1 picking for all four
 * places, every pick's ring over every page). Each place has a hand and
 * its ring (lu + 0x4F4 + 0x4C place, +0x30 the group); a player's pointer
 * moves one at a time (a player's entry's +0x1C), and on a pick the game
 * leaves that one on the character picked, the hand itself hidden
 * (sub_0002AEA0, finding the character by name through sub_0002ACD0):
 * each so left is remembered with its character's page, until a player's
 * pointer takes it up again. */
#define LU_RINGS 8
static struct { uint32_t ring, owner; int kind, page, hidden; float at[3]; } s_lu_ring[LU_RINGS];

static void lu_tok_put(uint32_t el, float x, float y, float z)
{
    uint32_t esp = g_esp, v, a[1];
    g_esp -= 16;
    v = g_esp;
    sf(v, x);
    sf(v + 4, y);
    sf(v + 8, z);
    a[0] = v;
    lu_call(g32v(g32v(el) + 0x40), el, 1, a, 0x0002E786u);
    g_esp = esp;
}

static void lu_ring_drop(int k)
{
    if (s_lu_ring[k].hidden && s_lu_ring[k].ring)
        lu_tok_put(s_lu_ring[k].ring, s_lu_ring[k].at[0], s_lu_ring[k].at[1], s_lu_ring[k].at[2]);
    memset(&s_lu_ring[k], 0, sizeof s_lu_ring[k]);
}

/* the game left ring (a place's hand group, owner its record; kind 1:
 * sub_0002AEA0) on a character of page pg */
static void lu_ring_note(uint32_t owner, uint32_t ring, int kind, int pg)
{
    int k, slot = -1;
    if (!ring || ring < 0x10000u)
        return;
    for (k = 0; k < LU_RINGS; k++)
        if (s_lu_ring[k].ring == ring) {
            slot = k;
            break;
        }
    if (slot < 0)
        for (k = 0; k < LU_RINGS; k++)
            if (!s_lu_ring[k].ring) {
                slot = k;
                break;
            }
    if (slot < 0)
        return;
    if (s_lu_ring[slot].ring == ring && s_lu_ring[slot].page == pg)
        return;
    memset(&s_lu_ring[slot], 0, sizeof s_lu_ring[slot]);
    s_lu_ring[slot].ring = ring;
    s_lu_ring[slot].owner = owner;
    s_lu_ring[slot].kind = kind;
    s_lu_ring[slot].page = pg;
    if (lu_log())
        LU_LOGF("[LINEUP] ring %08X (of %08X, %d) on a character of page %d\n", ring, owner, kind, pg + 1);
}

static void lu_tokens(void)
{
    int k;
    if (s_lu_pages <= 1)
        return;
    uint32_t lu = s_lu_poster ? g32v(s_lu_poster + 0x1C) : 0, driven[4], i;
    int nd = 0;
    if (!lu)
        return;
    /* the hands a player moves now (a player's entry, lu + 0x624 + 0x40 i
     * while +4 is set: +0x14 a player's, +0x1C the place it picks for):
     * those go with the pointer, on the page shown */
    for (i = 0; i < 4 && g32v(lu + 0x628 + i * 0x40); i++) {
        uint32_t st = lu + 0x624 + i * 0x40, h = g32v(st + 0x1C);
        if (*(volatile uint8_t *)gm(st + 0x14) && h < 4)
            driven[nd++] = g32v(lu + 0x4F4 + h * 0x4C + 0x30);
    }
    for (k = 0; k < LU_RINGS; k++) {
        uint32_t ring = s_lu_ring[k].ring;
        float x;
        int d;
        if (!ring)
            continue;
        for (d = 0; d < nd && driven[d] != ring; d++)
            ;
        if (s_lu_ring[k].kind == 0 && !*(volatile uint8_t *)gm(s_lu_ring[k].owner + 0x24)) {
            if (lu_log())
                LU_LOGF("[LINEUP] ring %08X let go\n", ring);
            lu_ring_drop(k);            /* back with the hand */
            continue;
        }
        if (s_lu_ring[k].kind == 1 && d < nd) {
            /* being moved by a player: shown (kept in mind, as the
             * pointer passes over a hand just left on its pick) */
            if (s_lu_ring[k].hidden) {
                lu_tok_put(ring, s_lu_ring[k].at[0], s_lu_ring[k].at[1], s_lu_ring[k].at[2]);
                s_lu_ring[k].hidden = 0;
            }
            continue;
        }
        x = gf(ring + 0x4C) + gf(ring + 0x58);
        if (s_lu_ring[k].page != s_lu_page) {
            if (!s_lu_ring[k].hidden) {
                s_lu_ring[k].at[0] = x;
                s_lu_ring[k].at[1] = gf(ring + 0x50) + gf(ring + 0x5C);
                s_lu_ring[k].at[2] = gf(ring + 0x54) + gf(ring + 0x60);
                s_lu_ring[k].hidden = 1;
            }
            if (x > LU_FAR * 0.5f)              /* (again, if the game put it back) */
                lu_tok_put(ring, LU_FAR, s_lu_ring[k].at[1], s_lu_ring[k].at[2]);
        } else if (s_lu_ring[k].hidden) {
            lu_tok_put(ring, s_lu_ring[k].at[0], s_lu_ring[k].at[1], s_lu_ring[k].at[2]);
            s_lu_ring[k].hidden = 0;
        }
    }
}

static void lu_show(int p)
{
    int k;
    if (p < 0 || p >= s_lu_pages || !s_lu_tex[p][0])
        return;
    lu_tokens();                    /* a ring picked on the page now leaving keeps it */
    for (k = 0; k < 4; k++)
        if (s_lu_photo[k])
            *(volatile uint32_t *)gm(s_lu_photo[k] + 0x114) = s_lu_tex[p][k & 1];
    s_lu_page = p;
    lu_place(p, 0);
    lu_tokens();
    if (lu_log())
        LU_LOGF("[LINEUP] page %d of %d\n", p + 1, s_lu_pages);
}

void sub_00043CD0(void)
{
    uint32_t poster = g_ecx, ret = g32v(g_esp), roster, list, n, i, sw = g32v(poster + 0x50), lu, n0;
    int p, k;

    s_lu_poster = poster;
    s_lu_n = 0;
    s_lu_pages = 1;
    s_lu_page = 0;
    memset(s_lu_tex, 0, sizeof s_lu_tex);
    memset(s_lu_pl, 0, sizeof s_lu_pl);
    memset(s_lu_ring, 0, sizeof s_lu_ring);
    /* Every roster character's marker (as the builder finds them). */
    roster = g32v(poster + 0x5C);
    list = roster ? g32v(roster + 0x130) : 0;
    n = roster ? *(volatile uint16_t *)gm(roster + 0x134) : 0;
    for (i = 0; i < n && list; i++) {
        uint32_t e = g32v(list + 4 * i);
        char nm[48] = "hotspot_";
        size_t j;
        if (!e)
            continue;
        for (j = 8; j < sizeof nm - 1; j++)
            if (!(nm[j] = *(volatile char *)gm(e + 1 + (uint32_t)(j - 8))))
                break;
        nm[j] = 0;
        lu_add_hs(lu_find(0x00019E00u, sw, nm));
    }
    lu_close_gaps();
    if (lu_log())
        LU_LOGF("[LINEUP] poster %08X: %u in the roster, %d markers, %d page(s)\n",
                poster, n, s_lu_n, s_lu_pages);
    if (s_lu_pages <= 1) {
        sub_00043CD0_gen();
        return;
    }
    s_lu_photo[0] = lu_find(0x00019D00u, g32v(poster + 0x1C), "photoleft");
    s_lu_photo[1] = lu_find(0x00019D00u, g32v(poster + 0x1C), "photoright");
    s_lu_photo[2] = lu_find(0x00019D00u, sw, "photoleft");
    s_lu_photo[3] = lu_find(0x00019D00u, sw, "photoright");
    /* The builder also appends each unlocked character's marker to the
     * line-up's list (sub_0002C780 on [poster+0x1C], +0x470/+0x474): keep
     * the list as one build leaves it. */
    lu = g32v(poster + 0x1C);
    n0 = lu ? *(volatile uint16_t *)gm(lu + 0x474) : 0;
    for (p = s_lu_pages - 1; p >= 0; p--) {
        lu_place(p, 1);
        if (lu)
            *(volatile uint16_t *)gm(lu + 0x474) = (uint16_t)n0;
        if (p)
            guest_push(ret);        /* the builder pops a return address */
        g_ecx = poster;
        sub_00043CD0_gen();
        for (k = 0; k < 2; k++)
            s_lu_tex[p][k] = s_lu_photo[k] ? g32v(s_lu_photo[k] + 0x114) : 0;
        if (lu_log())
            LU_LOGF("[LINEUP] page %d built: %08X %08X\n", p + 1,
                    s_lu_tex[p][0], s_lu_tex[p][1]);
    }
    lu_show(0);
}

/* The hand moved (flag 0) or stopped (1): pick the nearest marker. */
void sub_0002D430(void)
{
    uint32_t lu = g_ecx, st = g32v(g_esp + 4), flag = g32v(g_esp + 8);
    if (lu_log()) {
        static uint32_t dumped;
        if (dumped != lu) {
            uint32_t list = g32v(lu + 0x470), n = *(volatile uint16_t *)gm(lu + 0x474), i, j;
            dumped = lu;
            LU_LOGF("[LINEUP] lineup %08X: %u markers in its list (poster's sw %08X)\n", lu, n,
                    s_lu_poster ? g32v(s_lu_poster + 0x50) : 0);
            for (i = 0; i < n; i++) {
                uint32_t el = g32v(list + 4 * i);
                char nm[40];
                for (j = 0; j < 39 && (nm[j] = *(volatile char *)gm(el + 0x30 + j)); j++)
                    ;
                nm[j] = 0;
                LU_LOGF("[LINEUP]   %u %08X '%s' x %.1f+%.1f known %d\n", i, el, nm,
                        gf(el + 0x4C), gf(el + 0x58), lu_find_hs(el));
            }
        }
    }
    if (s_lu_pages > 1 && s_lu_n) {
        uint32_t pl = g32v(st + 0x1C), blk = lu + 0x4F4 + pl * 0x4C, hand = g32v(blk + 0x30);
        float sx = gf(st + 0xC), sy = gf(st + 0x10);
        if (pl < 4 && hand) {
            float hx = gf(hand + 0x4C) + gf(hand + 0x58);
            int dir = 0;
            if (s_lu_pl[pl].cool > 0)
                s_lu_pl[pl].cool--;
            if (flag == 0 && (sx > 0.4f || sx < -0.4f) && ((sx > sy && sx > -sy) || (sx < sy && sx < -sy))
                && hx - s_lu_pl[pl].lastx < 0.01f && s_lu_pl[pl].lastx - hx < 0.01f)
                s_lu_pl[pl].stuck++;
            else
                s_lu_pl[pl].stuck = 0;
            s_lu_pl[pl].lastx = hx;
            if (s_lu_pl[pl].stuck >= 6 && !s_lu_pl[pl].cool)
                dir = sx > 0 ? 1 : -1;
            if (lu_log())
                LU_LOGF("[LINEUP] p%u flag %u stick %.2f %.2f hand %.1f stuck %d sel %d\n",
                        pl, flag, sx, sy, hx, s_lu_pl[pl].stuck,
                        (int)g32v(g32v(lu + 0x46C) + g32v(blk + 8) * 4 + 0x148));
            if (dir && s_lu_page + dir >= 0 && s_lu_page + dir < s_lu_pages) {
                /* The hand to the other edge: the photo's width, less a
                 * little, the other way. */
                uint32_t ph = g32v(lu + 0x4B4), pl0 = g32v(lu + 0x4B0);
                float w = ph ? gf(ph + 0x118) : 0.0f, l0 = pl0 ? gf(pl0 + 0x4C) + gf(pl0 + 0x58) : 0.0f,
                      r0 = ph ? gf(ph + 0x4C) + gf(ph + 0x58) + w : 0.0f, dx = (r0 - l0) - 24.0f;
                uint32_t els[2], k;
                lu_show(s_lu_page + dir);
                els[0] = hand;
                els[1] = g32v(st + 0x2C);
                if (dx > 0.0f)
                    for (k = 0; k < 2; k++) {
                        uint32_t el = els[k], esp = g_esp, v, a[1];
                        if (!el)
                            continue;
                        g_esp -= 16;
                        v = g_esp;
                        sf(v, gf(el + 0x4C) + gf(el + 0x58) - (float)dir * dx);
                        sf(v + 4, gf(el + 0x50) + gf(el + 0x5C));
                        sf(v + 8, gf(el + 0x54) + gf(el + 0x60));
                        a[0] = v;
                        lu_call(g32v(g32v(el) + 0x40), el, 1, a, 0x0002E786u);
                        g_esp = esp;
                    }
                s_lu_pl[pl].lastx = gf(hand + 0x4C) + gf(hand + 0x58);
                s_lu_pl[pl].stuck = 0;
                s_lu_pl[pl].cool = 20;
                if (lu_log())
                    LU_LOGF("[LINEUP] p%u turned the page (photo %.1f..%.1f, hand now %.1f)\n",
                            pl, l0, r0, s_lu_pl[pl].lastx);
            }
        }
        g_ecx = lu;
        lu_place(s_lu_page, 1);
        sub_0002D430_gen();
        lu_place(s_lu_page, 0);
        return;
    }
    sub_0002D430_gen();
}

/* ── The mouse in the line-up ──────────────────────────────────────
 *
 * The line-up's update (sub_0002E250) walks its players' entries (lu +
 * 0x624 + 0x40 i while +4 is set: pad object +8, player +0x1C, token
 * element +0x2C) and moves a player's hand ([lu+0x4F4+0x4C p+0x30]) and
 * token by the stick through their SetPosition (vtable +0x40), keeping the
 * hand's tip -- its position plus the offset of [block+0x34] -- on the
 * photo (photoleft [lu+0x4B0] to photoright [lu+0x4B4] + its size), then
 * picks the marker nearest (sub_0002D430, flag 0 while moving, 1 when the
 * stick lets go). Here, called each frame from the update of the poster's
 * page (postermultiview; the line-up's own page holds only the markers):
 * when the pointer has moved, the hand of the player on the keyboard's
 * port is put with its tip under it, the same way, and the game picks the
 * marker nearest it (flag 0). The photo's place on screen was measured from the game's
 * frames (16:9 640x360: x 133..505, y 52.5..166). */
void rdr_lineup_pointer(uint32_t page)
{
    static unsigned seen_gen;
    static int moving;
    uint32_t lu, pl0, ph, i, port;
    float sx, sy, u, v, fx, fy, L, T, R, B, tx, ty;
    unsigned gen;

    if (!s_lu_poster || !rdr_menu_mode())
        return;
    {
        static const char want[] = "postermultiview";
        unsigned j;
        for (j = 0; j < sizeof want; j++)
            if (*(volatile char *)gm(page + 0x1C + j) != want[j])
                return;
    }
    lu_tokens();                            /* a ring picked or let go */
    lu = g32v(s_lu_poster + 0x1C);
    if (!lu || !rdr_input_pointer(&sx, &sy, &u, &v, &gen))
        return;
    pl0 = g32v(lu + 0x4B0);
    ph = g32v(lu + 0x4B4);
    if (!pl0 || !ph)
        return;
    rdr_input_hud(0);                       /* the pointer shows here */
    {
        /* A movement of a pixel or two (a hand resting on the mouse) is
         * not one: the stick and the keys keep the hand. */
        static float last_u = -9, last_v = -9;
        float du = (u - last_u) * 360.0f, dv = (v - last_v) * 360.0f;
        if (gen != seen_gen && du * du + dv * dv < 9.0f)
            gen = seen_gen;
        else if (gen != seen_gen) {
            last_u = u;
            last_v = v;
        }
    }
    if (gen == seen_gen && !moving)
        return;
    if (gen == seen_gen) {                  /* at rest */
        moving = 0;
        return;
    }
    port = (uint32_t)rdr_keyboard_port(g_rdr_settings.kbm_player >= 1 && g_rdr_settings.kbm_player <= 4
                                       ? g_rdr_settings.kbm_player - 1 : 0);
    L = gf(pl0 + 0x4C) + gf(pl0 + 0x58);
    T = gf(pl0 + 0x50) + gf(pl0 + 0x5C);
    R = gf(ph + 0x4C) + gf(ph + 0x58) + gf(ph + 0x118);
    B = gf(ph + 0x50) + gf(ph + 0x5C) + gf(ph + 0x11C);
    fx = 320.0f + u * 360.0f;
    fy = v * 360.0f;
    tx = L + (fx - 133.0f) / (505.0f - 133.0f) * (R - L);
    ty = T + (fy - 52.5f) / (166.0f - 52.5f) * (B - T);
    for (i = 0; i < 4 && g32v(lu + 0x628 + i * 0x40); i++) {
        uint32_t st = lu + 0x624 + i * 0x40, pad = g32v(st + 8), pl = g32v(st + 0x1C), blk, hand, tip, a[2];
        if (pad != 0x006795C8u + 0xB0u * port || pl >= 4)
            continue;
        blk = lu + 0x4F4 + pl * 0x4C;
        hand = g32v(blk + 0x30);
        tip = g32v(blk + 0x34);
        if (!hand || !tip)
            continue;
        /* Only while the hand is the stick's (sub_0002E250's conditions):
         * the line-up choosing (+0x458 == 2, +0x490 clear), this player in
         * (+0x14) and not yet picked (block +0x40). */
        if (*(volatile uint8_t *)gm(lu + 0x490) || g32v(lu + 0x458) != 2
            || !*(volatile uint8_t *)gm(st + 0x14) || !*(volatile uint8_t *)gm(st + 0x24)
            || *(volatile uint8_t *)gm(st + 0x3C) || *(volatile uint8_t *)gm(blk + 0x40)) {
            moving = 0;
            continue;
        }
        if (gen != seen_gen) {
            float ox = gf(tip + 0x4C), oy = gf(tip + 0x50), hx, hy, dx, dy, x = tx, y = ty;
            uint32_t els[2], k;
            /* The pointer past the photo's side turns the page (the stick
             * does it pushing the hand against the edge, sub_0002D430
             * above; the mouse's hand is held to the photo, so with the
             * mouse the preview's and the story's pages could not be
             * reached -- user). Once a time: the pointer comes back over
             * the photo before it turns another. */
            static int armed = 1;
            float edge = (R - L) * 0.02f;
            if (tx >= L && tx <= R)
                armed = 1;
            else if (armed && s_lu_pages > 1) {
                int dir = tx > R + edge ? 1 : tx < L - edge ? -1 : 0;
                if (dir && s_lu_page + dir >= 0 && s_lu_page + dir < s_lu_pages) {
                    lu_show(s_lu_page + dir);
                    armed = 0;
                    if (lu_log())
                        LU_LOGF("[LINEUP] mouse: p%u turned the page (pointer %.1f, photo %.1f..%.1f)\n",
                                pl, tx, L, R);
                }
            }
            if (x < L) x = L;
            if (x > R) x = R;
            if (y < T) y = T;
            if (y > B) y = B;
            hx = gf(hand + 0x4C) + gf(hand + 0x58);
            hy = gf(hand + 0x50) + gf(hand + 0x5C);
            dx = x - ox - hx;
            dy = y - oy - hy;
            els[0] = hand;
            els[1] = g32v(st + 0x2C);
            for (k = 0; k < 2; k++) {
                uint32_t el = els[k], esp = g_esp, vec;
                if (!el)
                    continue;
                g_esp -= 16;
                vec = g_esp;
                sf(vec, gf(el + 0x4C) + gf(el + 0x58) + dx);
                sf(vec + 4, gf(el + 0x50) + gf(el + 0x5C) + dy);
                sf(vec + 8, gf(el + 0x54) + gf(el + 0x60));
                a[0] = vec;
                lu_call(g32v(g32v(el) + 0x40), el, 1, a, 0x0002E786u);
                g_esp = esp;
            }
            a[0] = st;
            a[1] = 0;
            lu_call(0x0002D430u, lu, 2, a, 0x0002E7A0u);
            moving = 1;
            if (lu_log())
                LU_LOGF("[LINEUP] mouse: p%u hand tip to %.1f,%.1f (458 %u 490 %u st14 %u st24 %u st3c %u blk40 %u)\n",
                        pl, x, y, g32v(lu + 0x458), *(volatile uint8_t *)gm(lu + 0x490),
                        *(volatile uint8_t *)gm(st + 0x14), *(volatile uint8_t *)gm(st + 0x24),
                        *(volatile uint8_t *)gm(st + 0x3C), *(volatile uint8_t *)gm(blk + 0x40));
        } else {
            /* At rest: left where it is. The game's "let go" pick
             * (sub_0002D430 flag 1) pulls the hand onto the marker, and the
             * next small movement of the mouse took it back -- the hand
             * shook between the two. The pick made while moving already
             * chose the character under it. */
            moving = 0;
        }
    }
    seen_gen = gen;
}

/* The page a named character's marker stands on, or -1. */
static int lu_name_page(uint32_t lu, uint32_t name)
{
    uint32_t list = g32v(lu + 0x470), n = *(volatile uint16_t *)gm(lu + 0x474), i;
    char want[40], have[48];
    size_t j;
    if (!list || !name)
        return -1;
    for (j = 0; j < sizeof want - 1 && (want[j] = *(volatile char *)gm(name + (uint32_t)j)); j++)
        ;
    want[j] = 0;
    for (i = 0; i < n; i++) {
        uint32_t el = g32v(list + 4 * i);
        int h;
        for (j = 0; j < sizeof have - 1 && (have[j] = *(volatile char *)gm(el + 0x30 + (uint32_t)j)); j++)
            ;
        have[j] = 0;
        if (strcmp(have, want) && !(strncmp(have, "hotspot_", 8) == 0 && !strcmp(have + 8, want)))
            continue;
        h = lu_find_hs(el);
        return h >= 0 ? s_lu_hs[h].page : -1;
    }
    return -1;
}

/* Where a named character's marker is on screen (sub_0002ACD0: out
 * vector, name). From sub_0002C7D0 (0x0002C829) it puts the hand on a
 * player's character: turn to its page first. From sub_0002AF20 and
 * sub_0002AEA0 it puts a picked character's ring on the one picked, every
 * frame: a character on another page has its ring put out of sight there,
 * so each ring shows only over the page its character stands on (user
 * video: player 1 picking for all four places, the rings of the picks
 * made on other pages stood over the page shown). */
void sub_0002ACD0(void)
{
    uint32_t ret = g32v(g_esp), out = g32v(g_esp + 4), name = g32v(g_esp + 8), owner = g_esi;
    uint32_t lu = s_lu_poster ? g32v(s_lu_poster + 0x1C) : 0;
    int pg = -2;
    if (s_lu_pages > 1 && lu_log()) {
        char w[16];
        uint32_t j;
        for (j = 0; j < 15 && (w[j] = *(volatile char *)gm(name + j)); j++)
            ;
        w[j] = 0;
        LU_LOGF("[LINEUP] where is '%s' (from %08X), page %d\n", w, ret, s_lu_page + 1);
    }
    if (s_lu_pages > 1 && ret == 0x0002C829u) {
        pg = lu_name_page(g_ecx, name);
        if (pg >= 0 && pg != s_lu_page)
            lu_show(pg);
    } else if (s_lu_pages > 1 && lu && (ret == 0x0002AF67u || ret == 0x0002AEC0u || ret == 0x0002AF03u))
        pg = lu_name_page(lu, name);
    sub_0002ACD0_gen();
    (void)out;
    if (pg >= 0 && ret == 0x0002AF67u)
        lu_ring_note(owner, g32v(owner + 0x2C), 0, pg);
    else if (pg >= 0 && ret != 0x0002C829u)
        lu_ring_note(owner, g32v(owner + 0x30), 1, pg);
}

/* The poster goes: the extra pages' photos with it. */
void sub_00043BA0(void)
{
    uint32_t poster = g_ecx;
    if (poster == s_lu_poster && s_lu_pages > 1) {
        int p, k;
        uint32_t tex0[2];
        tex0[0] = s_lu_tex[0][0];
        tex0[1] = s_lu_tex[0][1];
        for (k = 0; k < 4; k++)
            if (s_lu_photo[k] && tex0[k & 1])
                *(volatile uint32_t *)gm(s_lu_photo[k] + 0x114) = tex0[k & 1];
        lu_place(0, 0);
        for (p = 1; p < s_lu_pages; p++)
            for (k = 0; k < 2; k++) {
                uint32_t t = s_lu_tex[p][k];
                if (!t)
                    continue;
                /* Released as the game releases its textures (0x00043CA0):
                 * the count at +0x1C, then sub_0017F850 (the D3D texture,
                 * the name, off the list at 0x0067867C) and the memory. */
                if (--*(volatile uint32_t *)gm(t + 0x1C) == 0) {
                    lu_call(0x0017F850u, t, 0, NULL, 0x00043CAEu);
                    lu_call(0x0005DCF0u, 0, 1, &t, 0x00043CB4u);
                }
            }
        if (lu_log())
            LU_LOGF("[LINEUP] poster %08X gone: %d extra page(s) freed\n", poster, s_lu_pages - 1);
    }
    if (poster == s_lu_poster) {
        s_lu_poster = 0;
        s_lu_n = 0;
        s_lu_pages = 1;
        s_lu_page = 0;
    }
    sub_00043BA0_gen();
}

/* Tests (RDR_PUP_LOG=1): each puppetshow (cutscene) loaded, by name, and
 * who asked (sub_001B26C0(this, name): the loader). */
void sub_001B26C0_enter(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RDR_PUP_LOG");
        on = e && *e == '1';
    }
    if (on) {
        char nm[64];
        uint32_t a = g32v(g_esp + 4), i, n = 0;
        for (i = 0; i < 63 && (nm[i] = *(volatile char *)gm(a + i)); i++)
            ;
        nm[i] = 0;
        fprintf(stderr, "[PUP] load '%s' this %08X from", nm, g_ecx);
        for (i = 0; i < 160 && n < 10; i++) {
            uint32_t v = g32v(g_esp + 4 * i);
            if (v >= 0x00011000u && v < 0x00380000u) {
                fprintf(stderr, " %08X", v);
                n++;
            }
        }
        fprintf(stderr, " | next level '%s'\n", (const char *)gm(0x00652E70u));
        fflush(stderr);
    }
}

/* ── Story co-op: player 2 starts beside player 1 ──────────────────
 *
 * sub_00137370(this = the level's start points) puts every player
 * character at its start: per player slot k a list at this+8+4k (count)
 * and this+0x18+k*0x168 (up to 10 entries of 0x24: x, y, z at +0..+8,
 * heading at +0xC), character i of slot k at entry i (past the end: the
 * last one, 2 units along x). It places a character the game's own way --
 * sub_001EB490 (position), sub_001EB4C0 (heading), then its physics
 * (sub_00219D60) -- which moving the position by hand did not (player 2
 * fell through the ground). A story level fills only player 1's list,
 * sometimes slot 2's with somewhere else entirely (player 2 started away
 * from player 1, user report). In co-op the extra slots get player 1's
 * list for the call, 2 units apart per player, and their own back after.
 * Also runs at Restart Level. */
extern void sub_00137370_gen(void);
extern int rdr_coop_count(void);

void sub_00137370(void)
{
    uint32_t sp = g_ecx;
    int n = rdr_coop_count(), k;
    uint8_t keep[4][4 + 0x168];
    if (n < 2 || n > 4 || !sp || !g32v(sp + 8)) {
        sub_00137370_gen();
        return;
    }
    for (k = 1; k < n; k++) {
        uint32_t i, cnt = g32v(sp + 8), dst = sp + 0x18 + 0x168u * (uint32_t)k;
        for (i = 0; i < 4; i++)
            keep[k][i] = *gm(sp + 8 + 4u * (uint32_t)k + i);
        for (i = 0; i < 0x168; i++)
            keep[k][4 + i] = *gm(dst + i);
        *(volatile uint32_t *)gm(sp + 8 + 4u * (uint32_t)k) = cnt;
        for (i = 0; i < 0x168; i++)
            *gm(dst + i) = *gm(sp + 0x18 + i);
        for (i = 0; i < cnt && i < 10; i++)
            sf(dst + 0x24 * i, gf(sp + 0x18 + 0x24 * i) + 2.0f * (float)k);
        fprintf(stderr, "[COOP] player %d starts beside player 1 (%.2f %.2f %.2f)\n", k + 1,
                gf(dst), gf(dst + 4), gf(dst + 8));
    }
    fflush(stderr);
    sub_00137370_gen();
    for (k = 1; k < n; k++) {
        uint32_t i, dst = sp + 0x18 + 0x168u * (uint32_t)k;
        for (i = 0; i < 4; i++)
            *gm(sp + 8 + 4u * (uint32_t)k + i) = keep[k][i];
        for (i = 0; i < 0x168; i++)
            *gm(dst + i) = keep[k][4 + i];
    }
}

/* ── XAPI: a control transfer queued twice ─────────────────────────
 *
 * sub_003C3998(ecx = the USB host, edx = an URB) appends a control
 * transfer to the host's queue (+0x41C head, +0x420 tail, linked at
 * URB+0x24) and runs it (sub_003C38DC). The game's rumble (sub_00047940:
 * XInputSetState with the pad's feedback block at 0x006526F0 + 0x46*port,
 * its URB at +0x10) sends again only once the last one is no longer
 * pending -- but after a death in co-op, Restart Level set the blocks up
 * afresh while one was still queued, the same URB went in a second time,
 * the queue looped on itself and XAPI read through -1 (guest 0x003C391D,
 * a crash on Restart Level, user report). An URB already in the queue is
 * not added again: it completes once, as queued. */
extern void sub_003C3998_gen(void);
extern RECOMP_TLS uint32_t g_edx;

void sub_003C3998(void)
{
    uint32_t host = g_ecx, urb = g_edx, q = host ? g32v(host + 0x41C) : 0;
    int n = 0;
    while (q && n++ < 64) {
        if (q == urb) {
            static int said;
            if (said++ < 8) {
                fprintf(stderr, "[RDR] USB: transfer %08X already queued, not added again\n", urb);
                fflush(stderr);
            }
            g_eax = 0x40000000u;            /* as when queued: pending */
            g_esp += 4;                     /* ret */
            return;
        }
        q = g32v(q + 0x24);
    }
    sub_003C3998_gen();
}

/* ── Rumble straight to the host pad ───────────────────────────────
 *
 * XInputSetState (sub_003BE767, stdcall: handle, feedback) sends the
 * motor speeds to the pad as a USB transfer, and the recompiled XAPI's
 * USB host driver, under the emulated OHCI controller, now and then ran
 * out of or corrupted its transfer descriptors: a crash in its control
 * queue (guest 0x003C391D, write through -1) a minute or so into a match
 * with rumble -- with the preview characters in Showdown, after a death
 * in co-op (user reports, logs). The only XInputSetState caller is the
 * game's rumble (sub_00047940), with the port's feedback block at
 * 0x006526F0 + 0x46 * port (motors at +0x42/+0x44). Those calls now go to
 * the host pad directly, as the controller model forwarded them, and
 * complete at once: status 0 in the block, "pending" returned, as the
 * game expects. No USB transfer, nothing for the driver to lose.
 * RDR_USB_RUMBLE=1 sends them the old way. */
extern void sub_003BE767_gen(void);
/* xbox_InputSetPortState, xbox_InputRumbleScale: xinput_xbox.h */

/* ── Lockstep pads (rdr_lockstep.c) ────────────────────────────
 * The XDK's pad calls, wrapped: recorded per game frame, or played back
 * on virtual handles, for the determinism tests of online "option 3". */
int rdr_lockstep_mode(void);
int rdr_ls_device_changes(uint32_t ins_va, uint32_t rem_va, uint32_t *ret);
void rdr_ls_device_changes_seen(uint32_t ins_va, uint32_t rem_va);
int rdr_ls_open(uint32_t port, uint32_t *ret);
int rdr_ls_virtual(uint32_t h);
int rdr_ls_get_state(uint32_t h, uint32_t st_va, uint32_t *ret);
void rdr_ls_state_seen(uint32_t h, uint32_t st_va);
void rdr_ls_frame(uint32_t ret);
extern void sub_003BE81F_gen(void);
extern void sub_003BE692_gen(void);
extern void sub_003BE6E8_gen(void);
extern void sub_003BE6F4_gen(void);

/* Online guests' pads (src/net/rdr_net_host.c) go through the same calls:
 * a guest's port reports a pad arriving, opens to a virtual handle and
 * reads the guest's latest state. A port a guest has is not offered to
 * the host's own pads. */
#include "net/rdr_net.h"
#define RDR_VPAD(h) (((h) & 0xFFFFFFF0u) == 0xF00D0000u)

void sub_003BE81F(void)                     /* XGetDeviceChanges(type, &ins, &rem) */
{
    uint32_t ins = g32v(g_esp + 8), rem = g32v(g_esp + 12), r;
    if (rdr_ls_device_changes(ins, rem, &r)) {
        g_eax = r;
        g_esp += 16;                        /* ret 0xC */
        return;
    }
    sub_003BE81F_gen();
    if (rdr_net_host_active()) {
        uint32_t a = g32v(ins), b = g32v(rem), p;
        for (p = 0; p < 4; p++)
            if (rdr_net_pad_port_taken((int)p)) {
                a &= ~(1u << p);            /* the host's own pad there is not the game's */
                b &= ~(1u << p);
            }
        rdr_net_pad_changes(&a, &b);
        *(volatile uint32_t *)gm(ins) = a;
        *(volatile uint32_t *)gm(rem) = b;
        g_eax = (a | b) ? 1u : 0u;
    }
    rdr_ls_device_changes_seen(ins, rem);
}

void sub_003BE692(void)                     /* XInputOpen(type, port, slot, params) */
{
    uint32_t r, port = g32v(g_esp + 8);
    if (rdr_ls_open(port, &r)) {
        g_eax = r;
        g_esp += 20;                        /* ret 0x10 */
        return;
    }
    if (port < 4 && rdr_net_pad_port_taken((int)port)) {
        g_eax = 0xF00D0000u | port;
        g_esp += 20;
        return;
    }
    sub_003BE692_gen();
}

void sub_003BE6E8(void)                     /* XInputClose(h) */
{
    if (RDR_VPAD(g32v(g_esp + 4))) {
        g_esp += 8;                         /* ret 4 */
        return;
    }
    sub_003BE6E8_gen();
}

/* Stuck controls (an idea from another port of the game: on a slow PC its
 * player walked on forever with the controls dead, and once crashed in
 * here reading a device record left unset). The game reads a controller
 * through the XDK's USB stack: the emulated pad's reports go to the
 * guest's driver, and XInputGetState copies out the last one it got. If
 * the driver stops asking for reports, the game keeps the last state it
 * had -- a stick held, a button down -- however the player lets go. Here
 * the reports each pad has been asked for are watched (the toolkit's
 * ohci_pad_reports); a pad the game reads that has had none for a second
 * is read straight from the PC instead (the same report the USB pad would
 * give: the controller, the keyboard and mouse through the pad filter),
 * until its reports come again. Logged both ways. */
extern unsigned long ohci_pad_reports(int pad);
extern int usb_gamepad_report(int pad, uint8_t *out, int max);
extern uint64_t xbox_HostNowNs(void);
static int pad_direct(uint32_t h, uint32_t st)
{
    static unsigned long seen[4];
    static uint64_t when[4];
    static int stalled[4];
    static uint8_t last[4][18];
    static uint32_t packet[4];
    uint64_t now = xbox_HostNowNs();
    unsigned long n;
    int p;
    for (p = 0; p < 4 && g32v(0x004041D4u + 0x38u * (uint32_t)p) != h; p++)
        ;
    if (p == 4 || !h)
        return 0;
    n = ohci_pad_reports(p);
    {
        static int test = -1;               /* (tests: the reports taken as stopped) */
        if (test < 0)
            test = getenv("RDR_TEST_PAD_STALL") != NULL;
        if (test && when[p])
            n = seen[p];
    }
    if (n != seen[p] || !when[p]) {
        seen[p] = n;
        when[p] = now;
        if (stalled[p]) {
            stalled[p] = 0;
            fprintf(stderr, "[INPUT] pad %d: its USB reports came again; read as usual\n", p + 1);
            fflush(stderr);
        }
        return 0;
    }
    if (!stalled[p]) {
        if (now - when[p] < 1000000000ull)
            return 0;
        stalled[p] = 1;
        fprintf(stderr, "[INPUT] pad %d: no USB reports for %.1f s; read straight from the PC\n", p + 1,
                (double)(now - when[p]) / 1e9);
        fflush(stderr);
    }
    {
        uint8_t rep[20];
        memset(rep, 0, sizeof rep);
        usb_gamepad_report(p, rep, (int)sizeof rep);
        if (memcmp(last[p], rep + 2, 18)) {
            memcpy(last[p], rep + 2, 18);
            packet[p]++;
        }
        memcpy((void *)gm(st), &packet[p], 4);
        memcpy((void *)gm(st + 4), rep + 2, 18);
    }
    return 1;
}

void sub_003BE6F4(void)                     /* XInputGetState(h, &state) */
{
    uint32_t h = g32v(g_esp + 4), st = g32v(g_esp + 8), r;
    {
        extern void rdr_cheats_tick(void);
        extern void rdr_attract_hold_tick(void);
        rdr_cheats_tick();                  /* (the game's thread: rdr_cheats.c) */
        rdr_attract_hold_tick();            /* no demo while our menus are open */
    }
    if (rdr_ls_get_state(h, st, &r)) {
        g_eax = r;
        g_esp += 12;                        /* ret 8 */
        return;
    }
    if (RDR_VPAD(h)) {
        uint8_t s[22];
        rdr_net_pad_state((int)(h & 3u), s);
        memcpy((void *)gm(st), s, 22);
        g_eax = 0;
        g_esp += 12;
        return;
    }
    if (pad_direct(h, st)) {
        g_eax = 0;
        g_esp += 12;                        /* ret 8 */
        {
            extern void rdr_desk_pad(uint32_t st);
            rdr_desk_pad(st);
        }
        return;
    }
    sub_003BE6F4_gen();
    rdr_ls_state_seen(h, st);
    {
        extern void rdr_desk_pad(uint32_t st);
        rdr_desk_pad(st);                   /* the desk's Y: Online Co-op */
    }
}

/* The CPU tick count (sub_00084880: jmp to rdtsc), edx:eax. Lockstep:
 * a count of frames (rdr_lockstep.c rdr_ls_ticks). */
int rdr_ls_ticks(uint64_t *t);
extern void sub_00084880_gen(void);
int g_rdr_test_frame_us;                /* tests ("framems N"): the clock moves N ms a frame */
void sub_00084880(void)
{
    uint64_t t;
    if (g_rdr_test_frame_us > 0) {
        /* as a PC running at 1000/N frames a second: the game's frame
         * number ([0x0065AA98]) times N ms, in 733 MHz ticks */
        static uint64_t base;
        static uint32_t f0;
        uint32_t f = g32v(0x0065AA98u);
        if (!base) {
            sub_00084880_gen();
            base = (uint64_t)g_edx << 32 | g_eax;
            f0 = f;
            return;
        }
        t = base + (uint64_t)(f - f0) * (uint64_t)g_rdr_test_frame_us * 733ull;
        g_eax = (uint32_t)t;
        g_edx = (uint32_t)(t >> 32);
        g_esp += 4;
        return;
    }
    if (rdr_ls_ticks(&t)) {
        g_eax = (uint32_t)t;
        g_edx = (uint32_t)(t >> 32);
        g_esp += 4;                         /* ret */
        return;
    }
    sub_00084880_gen();
}

/* The level loader's body (sub_0006AF70, on the thread sub_0006B080
 * starts at 0x0006B060 for each load; the main loop shows the loading
 * screen meanwhile). Lockstep holds the main loop's frames while it runs:
 * how many loading-screen frames a load took was the machine's speed. */
void rdr_ls_loading(int delta);
extern void sub_0006AF70_gen(void);
void sub_0006AF70(void)
{
    rdr_ls_loading(1);
    sub_0006AF70_gen();
    rdr_ls_loading(-1);
}

/* the game clock (once a loop, before the pads): online guests heard, the
 * lockstep tests' fixed step and fingerprint */
void sub_00083670_enter(void)
{
    rdr_net_host_tick();
    rdr_ls_frame(g32v(g_esp));          /* the return address: which loop */
}

void sub_003BE767(void)
{
    static int usb = -1;
    uint32_t fb = g32v(g_esp + 8);
    if (RDR_VPAD(g32v(g_esp + 4))) {
        if (fb)
            *(volatile uint32_t *)gm(fb) = 0;
        g_eax = 0;
        g_esp += 12;                        /* ret 8 */
        return;
    }
    if (usb < 0) {
        const char *e = getenv("RDR_USB_RUMBLE");
        usb = e && *e == '1';
    }
    if (!usb && fb >= 0x006526F0u && fb < 0x006526F0u + 4u * 0x46u
        && (fb - 0x006526F0u) % 0x46u == 0) {
        uint32_t port = (fb - 0x006526F0u) / 0x46u;
        float k = xbox_InputRumbleScale();
        uint16_t l = *(volatile uint16_t *)gm(fb + 0x42), r = *(volatile uint16_t *)gm(fb + 0x44);
        XBOX_VIBRATION v;
        v.wLeftMotorSpeed = (WORD)(l * k);
        v.wRightMotorSpeed = (WORD)(r * k);
        xbox_InputSetPortState((DWORD)port, &v);
        *(volatile uint32_t *)gm(fb) = 0;           /* done */
        g_eax = 0x3E5;                              /* ERROR_IO_PENDING, as queued */
        g_esp += 12;                                /* ret 8 */
        return;
    }
    sub_003BE767_gen();
}

/* ── Unlockables past the retail table ─────────────────────────────
 *
 * The unlock manager (0x006585C0) keeps one bit per unlockables.csv row:
 * the saved ones at [mgr]+0x24 and the session's at mgr+0x18, 256 bits
 * each; IsUnlocked (sub_00057C90, thiscall, the row) tests both, SetBit
 * (sub_00057D30, thiscall: row, which, set) changes one. The retail table
 * has 221 rows. Characters a mod adds (the 2002 preview: 55) need a row
 * each -- without one the line-up hides them -- but past row 255 there
 * is no bit: the test read the manager's and the save's next fields, and
 * a set would write over them. And rows 221-255 of a profile made before
 * the mod are clear, so its characters showed or hid at random (user
 * report: preview line-up pages with only a few of their characters).
 * Rows past the retail table are always unlocked, and no bit past 255 is
 * ever written. */
#define RDR_RETAIL_UNLOCKABLES  221u
extern void sub_00057C90_gen(void);
extern void sub_00057D30_gen(void);

void sub_00057C90(void)
{
    static int all = -1;                    /* tests: RDR_UNLOCK_ALL=1, every row open */
    if (all < 0)
        all = getenv("RDR_UNLOCK_ALL") != NULL;
    if (all || g32v(g_esp + 4) >= RDR_RETAIL_UNLOCKABLES) {
        g_eax = 1;
        g_esp += 8;                         /* ret 4 */
        return;
    }
    sub_00057C90_gen();
}

void sub_00057D30(void)
{
    if (g32v(g_esp + 4) >= 256u) {
        g_esp += 16;                        /* ret 12 */
        return;
    }
    sub_00057D30_gen();
}

/* A Flash menu's frame (thiscall: the menu, the frame time): the shop and
 * the weapon select take the mouse (rdr_overrides.c, rdr_swf_menu). */
void rdr_swf_menu(uint32_t menu);

void sub_00018490_enter(void)
{
    rdr_swf_menu(g_ecx);
}

/* The front-end screen's enter (sub_0006BAA0, thiscall, plain ret; returns
 * 0 or a transition to follow): during a chapter warp from a story level
 * it hands straight over to the game screen (rdr_overrides.c,
 * rdr_warp_direct). */
extern void sub_0006BAA0_gen(void);
extern uint32_t rdr_warp_direct(uint32_t screen);
void sub_0006BAA0(void)
{
    uint32_t tr = rdr_warp_direct(g_ecx);
    if (tr) {
        g_eax = tr;
        g_esp += 4;
        return;
    }
    sub_0006BAA0_gen();
}

/* D3D's push-buffer kickoff (sub_0033C7F0, thiscall on the device, plain
 * ret). On the console it flushes the CPU's write-back cache to memory --
 * sets bit 16 of PFB_WBC (0x00100410) and spins until the hardware clears
 * it, microseconds -- then writes the new DMA_PUT. Here there is no such
 * cache and the bit is cleared only by the push-buffer thread, between its
 * commands or between its passes: every kickoff waited for that thread to
 * come round, many times a frame, so the game's thread and the drawing
 * thread took turns instead of running together (profile: 4-player co-op,
 * a third of all samples spinning in this loop; the user's log: the
 * drawing thread idle a third of the time at 70 fps).
 *
 * The plain kickoff -- no PUT relocation (device flag 0x2000), no wait for
 * idle (0x00346D60), the flip not held for its vblank (RECOMP_FLIP_PACING,
 * whose hold works through this very bit) -- writes DMA_PUT straight away.
 * RDR_KICKOFF_WAIT=1: as on the console. */
extern void sub_0033C7F0_gen(void);
extern int nv2a_pb_exec_flip_pacing(void);
void sub_0033C7F0(void)
{
    static int wait = -1;
    uint32_t dev = g_ecx, f;
    if (wait < 0) {
        const char *e = getenv("RDR_KICKOFF_WAIT");
        wait = (e && e[0] == '1') || nv2a_pb_exec_flip_pacing() != 0;
    }
    f = g32v(dev + 8);
    if (wait || (f & 0x2000u) || g32v(0x00346D60u)) {
        sub_0033C7F0_gen();
        return;
    }
    {
        uint32_t put = (f & 4u) ? g32v(dev + 0x770) : g32v(dev);
        uint32_t fifo = g32v(dev + 0x1C20);
        /* x86 keeps stores in order and guest memory is volatile throughout:
         * the commands are in memory before PUT says so. */
        *(volatile uint32_t *)gm(fifo + 0x40) = put & 0x0FFFFFFFu;
    }
    g_esp += 4;                                 /* plain ret */
}

/* The same write-back-cache flush on its own (sub_0033C790, plain ret):
 * nothing to flush here, so no wait (as sub_0033C7F0 above). */
extern void sub_0033C790_gen(void);
void sub_0033C790(void)
{
    static int wait = -1;
    if (wait < 0) {
        const char *e = getenv("RDR_KICKOFF_WAIT");
        wait = (e && e[0] == '1') || nv2a_pb_exec_flip_pacing() != 0;
    }
    if (wait) {
        sub_0033C790_gen();
        return;
    }
    g_esp += 4;
}

/* A bullet's hit on a character (sub_00088050, cdecl: a1 the hit, +8 the
 * one hit; a2 the shot, +0x1C who fired -- the pair its own team check,
 * Showdown's FriendlyFireFactor, compares). Co-op without friendly fire:
 * one player's shot at another does nothing (rdr_overrides.c). */
extern void sub_00088050_gen(void);
extern int rdr_coop_friendly_hit(uint32_t hit, uint32_t shot);
extern int rdr_duel_win_hit(uint32_t hit, uint32_t shot);
void sub_00088050(void)
{
    if (rdr_coop_friendly_hit(g32v(g_esp + 4), g32v(g_esp + 8)) || rdr_duel_win_hit(g32v(g_esp + 4), g32v(g_esp + 8))) {
        /* as the game's own "not this one" exit does (0x00089423): the
         * shot's byte +8 cleared, so whatever reads it next sees a hit that
         * did nothing -- not one left as it was */
        *(volatile uint8_t *)gm(g32v(g_esp + 8) + 8) = 0;
        g_esp += 4;                             /* plain ret */
        return;
    }
    {
        /* RDR_DMG_LOG: each hit, who and the health before and after */
        static int lg = -1;
        uint32_t hit = g32v(g_esp + 4), shot = g32v(g_esp + 8), v = 0, hh = 0, stk0 = g_esp;
        float before = -1.0f;
        if (lg < 0)
            lg = getenv("RDR_DMG_LOG") != NULL;
        if (lg && hit) {
            v = g32v(hit + 8);
            hh = v ? g32v(v + 0x18) : 0;
            if (hh) { uint32_t b = g32v(hh + 0x10); memcpy(&before, &b, 4); }
        }
        sub_00088050_gen();
        if (lg && hit) {
            float after = -1.0f;
            if (hh) { uint32_t b = g32v(hh + 0x10); memcpy(&after, &b, 4); }
            fprintf(stderr, "[DMG] hit %08X on %08X (vtable %08X) by %08X: health %.1f -> %.1f; stack", hit, v,
                    v ? g32v(v) : 0, shot ? g32v(shot + 0x1C) : 0, before, after);
            {
                int k2, shown = 0;
                for (k2 = 0; k2 < 64 && shown < 10; k2++) {
                    uint32_t w = g32v(stk0 + (uint32_t)k2 * 4u);
                    if (w >= 0x00011000u && w < 0x003A0000u) { fprintf(stderr, " %08X", w); shown++; }
                }
            }
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
}

/* ── Freeing a block twice (user crash unloading Brimstone after its
 * stores) ──────────────────────────────────────────────────────────
 *
 * The heap's free (sub_0005D8F0, thiscall: the heap, the block, 0; ret 8)
 * checks the block's neighbours and ends the game when they disagree
 * ("Free(%p) - Prev node no longer points back to us"). The user's log
 * showed the block's header already cleared -- not in use: a name map's
 * node freed a second time at the level's end, its memory by then part of
 * something else. Such a free is left out (logged) instead: the block is
 * not the caller's any more, and freeing it again would only break the
 * heap for whoever has it now (rdr_overrides.c, rdr_heap_free_ok). */
extern void sub_0005D8F0_gen(void);
int rdr_heap_free_ok(uint32_t heap, uint32_t ptr, uint32_t caller, uint32_t esp);

void sub_0005D8F0(void)
{
    uint32_t ptr = g32v(g_esp + 4);
    if (ptr && !rdr_heap_free_ok(g_ecx, ptr, g32v(g_esp + 12), g_esp)) {
        g_esp += 12;                        /* ret 8 */
        return;
    }
    sub_0005D8F0_gen();
}

/* ── The game's own menus: PC Options, and the book's co-op pages ─────
 *
 * The .swui menu pages are plain text, read a line at a time (sub_00084F00,
 * thiscall on the reader: the buffer, its size, the end-of-line character;
 * ret 12; the line's length back, trailing blanks cut, -1 at the end). As
 * the lines go by, two are changed and two added -- in every archive's
 * copy, as each level carries its own pause.swui:
 *
 *  - pause.swui: "Xbox Live" (nothing on a PC) becomes "PC Options"; its
 *    callback stays "xblive", which the pause page's buttons (sub_0002FAB0)
 *    now answer by opening the F1 menu over it.
 *  - storyselect.swui (the chapter book's left page): "Play As" and "Story
 *    Co-op" under Player Options (callbacks PCPLAYAS / PCCOOP; Bounty
 *    Hunter, when unlocked, moves below them). The book's buttons
 *    (sub_00037E40) open the matching page of the F1 menu: who rides, and
 *    as whom, is settled before the chapter starts.
 *
 * The pressed button is [page+0x374], its callback name at +0x29; the
 * handlers clear it once acted on. RDR_NATIVE_MENUS=0 leaves the files
 * and buttons alone. */
extern void sub_00084F00_gen(void);
extern void sub_0002FAB0_gen(void);
extern void sub_00037E40_gen(void);
void rdr_overlay_show(int what);          /* ui/rdr_overlay.cpp */

static int native_menus(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RDR_NATIVE_MENUS");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* Lines to hand out before the reader's next one (one reader at a time
 * loads pages: they load on the game's thread). */
static char s_swui_queue[4][320];
static int s_swui_qn, s_swui_qi;
static uint32_t s_swui_reader;

static void swui_put(uint32_t buf, uint32_t max, const char *s)
{
    uint32_t i, n = (uint32_t)strlen(s);
    if (max == 0) return;
    if (n > max - 1) n = max - 1;
    for (i = 0; i < n; i++) *(volatile char *)gm(buf + i) = s[i];
    *(volatile char *)gm(buf + n) = 0;
    g_eax = n;
}

/* The 2002 look (Display): the pause menu as the January 2002 preview
 * had it (its pause.swui) -- the picture tinted brown instead of darkened,
 * no scrolls, the rows in font02 (the preview's lettering, still on the
 * retail disc) and "Paused" pulsing at the bottom right. Every line
 * matched is pause.swui's own (no other page has them). From the next
 * level loaded: each level carries its pause.swui. */
static int pause2002(uint32_t reader, uint32_t buf, uint32_t max, const char *line)
{
    if (!strcmp(line, "<TEXTURE:0,0,0:640,448:NoTexture><BASECOLOR:0,0,0,0.584>")) {
        swui_put(buf, max, "<TEXTURE:0,0,0:640,448:NoTexture><BASECOLOR:0.300,0.200,0.150,0.550>");
        return 1;
    }
    if (!strncmp(line, "<TEXTURE:158,1", 14) && strstr(line, ":NAME:Scroll") && strstr(line, ":pausescrolls>")) {
        char out[320];
        snprintf(out, sizeof out, "%s<BASECOLOR:1,1,1,0>", line);     /* kept (placed by name), unseen */
        swui_put(buf, max, out);
        return 1;
    }
    if (!strcmp(line, "<FONT:berrylium:SCALE:0.750,0.750:DROPSHADOW:CENTER>")) {
        swui_put(buf, max, "<FONT:font02:SCALE:1.000,1.000:DROPSHADOW:CENTER>");
        return 1;
    }
    if (!strncmp(line, ";<TEXT:460,400,0:Paused>", 24)) {
        swui_put(buf, max, "<FONT:font02:DROPSHADOW>");
        snprintf(s_swui_queue[0], sizeof s_swui_queue[0], "<TEXT:460,400,0:Paused><PULSE:1,1,1,1:1,1,1,0.1:0.2>");
        s_swui_qn = 1;
        s_swui_qi = 0;
        s_swui_reader = reader;
        return 1;
    }
    return 0;
}

void sub_00084F00(void)
{
    uint32_t reader = g_ecx, buf = g32v(g_esp + 4), max = g32v(g_esp + 8);
    char line[256];
    uint32_t i;
    if (s_swui_qi < s_swui_qn && reader == s_swui_reader) {
        swui_put(buf, max, s_swui_queue[s_swui_qi++]);
        g_esp += 16;                            /* ret 12 */
        return;
    }
    s_swui_qn = s_swui_qi = 0;
    sub_00084F00_gen();
    if ((int32_t)g_eax < 9 || !native_menus())
        return;
    for (i = 0; i < sizeof line - 1 && i < (uint32_t)g_eax; i++)
        line[i] = *(volatile char *)gm(buf + i);
    line[i] = 0;
    if (g_rdr_settings.look2002 && pause2002(reader, buf, max, line))
        return;
    if (line[0] != '<' || strncmp(line, "<BUTTON:", 8))
        return;
    if (!strncmp(line, "<BUTTON:265,277,0:Xbox Live:CALLB:", 34)) {
        char out[320];
        snprintf(out, sizeof out, "<BUTTON:265,277,0:PC Options:CALLB:%s", line + 34);
        swui_put(buf, max, out);
    } else if (strstr(line, ":story_playeropt:CALLB:")) {
        /* the book's Player Options: ours follow it, in its own style */
        const char *style = strstr(line, "><BASECOLOR");
        style = style ? style + 1 : "";
        snprintf(s_swui_queue[0], sizeof s_swui_queue[0],
                 "<BUTTON:40,190,0:Play As:CALLB:0,0:640,448:0:-1:PCPLAYAS>%s", style);
        snprintf(s_swui_queue[1], sizeof s_swui_queue[1],
                 "<BUTTON:40,210,0:Story Co-op:CALLB:0,0:640,448:0:-1:PCCOOP>%s", style);
        s_swui_qn = 2;
        s_swui_qi = 0;
        s_swui_reader = reader;
    } else if (!strncmp(line, "<BUTTON:40,190,0:story_bhunter:", 31)) {
        char out[320];
        snprintf(out, sizeof out, "<BUTTON:40,230,0:story_bhunter:%s", line + 31);
        swui_put(buf, max, out);
    }
}

/* the name of the pressed button's callback, or NULL */
static const char *pressed_callback(uint32_t page, char *out, size_t n)
{
    uint32_t b = g32v(page + 0x374);
    size_t i;
    if (!b) return NULL;
    for (i = 0; i + 1 < n; i++)
        if (!(out[i] = *(volatile char *)gm(b + 0x29 + (uint32_t)i)))
            break;
    out[i] = 0;
    return out;
}

void sub_0002FAB0(void)
{
    char cb[32];
    if (native_menus() && pressed_callback(g_ecx, cb, sizeof cb) && !strcmp(cb, "xblive")) {
        *(volatile uint32_t *)gm(g_ecx + 0x374) = 0;
        fprintf(stderr, "[MENU] pause: PC Options\n");
        rdr_overlay_show(0);
        g_esp += 4;                             /* plain ret */
        return;
    }
    sub_0002FAB0_gen();
}

/* A player's look for someone to talk to (sub_0013DFE0, thiscall on the
 * player): the nearest person in reach and in front is put in the one
 * global talk partner [0x0065A9A0] (sub_000736A0) and the player's HUD
 * shows "Press X to talk". In co-op every player runs it each frame, so
 * the second player's look (nobody near) cleared what the first had found
 * -- the prompt stayed on, but X did nothing (user video, the train). Now
 * a player who finds nobody does not clear another player's find made
 * moments before (rdr_overrides.c). */
extern void sub_0013DFE0_gen(void);
extern void rdr_coop_talk_scan(uint32_t player, uint32_t before, uint32_t cool_before);
void sub_0013DFE0(void)
{
    uint32_t player = g_ecx, before = g32v(0x0065A9A0u), cool = g32v(0x0065A9A4u);
    sub_0013DFE0_gen();
    rdr_coop_talk_scan(player, before, cool);
}

void sub_00037E40(void)
{
    char cb[32];
    if (native_menus() && pressed_callback(g_ecx, cb, sizeof cb)
        && (!strcmp(cb, "PCPLAYAS") || !strcmp(cb, "PCCOOP"))) {
        *(volatile uint32_t *)gm(g_ecx + 0x374) = 0;
        fprintf(stderr, "[MENU] book: %s\n", cb);
        rdr_overlay_show(!strcmp(cb, "PCPLAYAS") ? 1 : 2);
        g_esp += 4;                             /* plain ret */
        return;
    }
    sub_00037E40_gen();
}
