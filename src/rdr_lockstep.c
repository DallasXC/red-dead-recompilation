/*
 * rdr_lockstep.c - lockstep play, step one: is the game deterministic?
 *
 * Online co-op "option 3": every machine runs the whole game and only the
 * pads are sent, so every machine must make the same game of the same
 * pads, frame for frame. This file gives the game a pad layer that can be
 * fed per game frame, and a fingerprint of the play each frame:
 *
 *   RDR_LOCKSTEP_RECORD=<file>  the pads as the game read them, and the
 *                               pads it saw come and go, each game frame
 *   RDR_LOCKSTEP_PLAY=<file>    the game reads those instead: no real pad
 *                               is opened (virtual handles), each frame
 *                               gets the recorded state for its number
 *   RDR_LOCKSTEP_HASH=1         "[LS] frame N hash H" each frame (with
 *                               either; the two runs' lines compared show
 *                               the first frame they differ)
 *
 * Both make the game's clock a count of frames (ls_clock_frame below: the
 * CPU tick count every timer reads, 1/60 s a frame) and the refresh
 * period ([0x00409DAC], rdr_overrides.c rdr_timing_tick) 1/60.
 *
 * The frame is the game clock's own count, [0x0065AA98], bumped once a
 * loop by sub_00083670 before the pads are read (sub_00047C40: device
 * changes, XInputOpen/Close, and per pad sub_00047940 -> XInputGetState).
 * The XDK calls wrapped (rdr_hooks.c): XGetDeviceChanges 0x003BE81F,
 * XInputOpen 0x003BE692, XInputClose 0x003BE6E8, XInputGetState
 * 0x003BE6F4, XInputSetState 0x003BE767.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *xbox_GetMemoryOffset(void);

#define LS_FRAME        0x0065AA98u     /* the game clock's frame number */
#define LS_PAD_HANDLES  0x004041D4u     /* + port * 0x38: the open handle */
#define LS_FAKE         0xF00D0000u     /* + port: a virtual handle */
#define LS_STATE_LEN    22              /* XINPUT_STATE: packet + gamepad */

static uint64_t ls_clock_ns(void);
static void ls_net_start(const char *host, const char *join);
static uint32_t ls_net_ports_mask(void);
static void ls_net_state(int port, uint32_t frame, uint8_t *out);
static void ls_net_frame(uint32_t frame, uint32_t hash);
static int s_mode = -1;                 /* 0 off, 1 record, 2 play */
static int s_hash;
static FILE *s_rec;

typedef struct {
    uint32_t frame;
    uint32_t ins, rem;                  /* device changes seen that frame */
    uint8_t have;                       /* ports with a state that frame */
    uint8_t st[4][LS_STATE_LEN];
} LsFrame;

static LsFrame *s_frames;
static size_t s_count, s_cap;
static uint8_t s_cur[4][LS_STATE_LEN];  /* play: the state each port holds */
static uint32_t s_changes_frame = 0xFFFFFFFFu;

static uint8_t *gm(uint32_t va) { return (uint8_t *)xbox_GetMemoryOffset() + va; }
static uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, gm(va), 4); return v; }
static float rdf(uint32_t va) { float v; memcpy(&v, gm(va), 4); return v; }

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static LsFrame *frame_at(uint32_t f, int make)
{
    size_t lo = 0, hi = s_count;
    while (lo < hi) {                   /* frames come in order: binary search */
        size_t mid = (lo + hi) / 2;
        if (s_frames[mid].frame < f)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < s_count && s_frames[lo].frame == f)
        return &s_frames[lo];
    if (!make)
        return NULL;
    if (s_count == s_cap) {
        size_t nc = s_cap ? s_cap * 2 : 4096;
        LsFrame *nf = (LsFrame *)realloc(s_frames, nc * sizeof *s_frames);
        if (!nf)
            return NULL;
        s_frames = nf;
        s_cap = nc;
    }
    memmove(&s_frames[lo + 1], &s_frames[lo], (s_count - lo) * sizeof *s_frames);
    memset(&s_frames[lo], 0, sizeof *s_frames);
    s_frames[lo].frame = f;
    s_count++;
    return &s_frames[lo];
}

static void load_play(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    if (!f) {
        fprintf(stderr, "[LS] cannot read %s\n", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned fr, a, b, port;
        char hex[64];
        if (sscanf(line, "D %u %x %x", &fr, &a, &b) == 3) {
            LsFrame *e = frame_at(fr, 1);
            if (!e) break;
            e->ins |= a;
            e->rem |= b;
        } else if (sscanf(line, "P %u %u %63s", &fr, &port, hex) == 3 && port < 4 && strlen(hex) == 2 * LS_STATE_LEN) {
            LsFrame *e = frame_at(fr, 1);
            int k;
            if (!e) break;
            for (k = 0; k < LS_STATE_LEN; k++)
                e->st[port][k] = (uint8_t)(hexval(hex[2 * k]) * 16 + hexval(hex[2 * k + 1]));
            e->have |= (uint8_t)(1u << port);
        }
    }
    fclose(f);
    fprintf(stderr, "[LS] playing %s: %zu frames\n", path, s_count);
}

int rdr_lockstep_mode(void)
{
    if (s_mode < 0) {
        const char *r = getenv("RDR_LOCKSTEP_RECORD"), *p = getenv("RDR_LOCKSTEP_PLAY");
        const char *h = getenv("RDR_LOCKSTEP_HASH");
        s_mode = 0;
        s_hash = h && *h && *h != '0';
        const char *nh = getenv("RDR_LS_HOST"), *nj = getenv("RDR_LS_JOIN");
        if ((nh && *nh && *nh != '0') || (nj && *nj)) {
            s_mode = 4;                 /* online lockstep (ls_net_* below) */
            ls_net_start(nh, nj);
        } else if (p && *p) {
            s_mode = 2;
            load_play(p);
        } else if (r && *r) {
            s_rec = fopen(r, "w");
            s_mode = s_rec ? 1 : 0;
            fprintf(stderr, "[LS] recording to %s%s\n", r, s_rec ? "" : " (cannot write)");
        }
        if (s_hash && !s_mode)
            s_mode = 3;                 /* hashing only, frame clock */
        if (s_mode) {
            extern uint64_t (*xbox_clock_override_ns)(void);
            xbox_clock_override_ns = ls_clock_ns;
        }
    }
    return s_mode;
}

/* The lockstep frame: the game's frames that are not loading frames.
 * A load (the level loader's thread, sub_0006AF70; the loading screens'
 * own loops) takes as many of the game's frames as the machine needs, so
 * those are not counted: pads, recordings and fingerprints go by this. */
static uint32_t s_lsf;
static uint32_t cur_frame(void) { return s_lsf; }

static int port_of(uint32_t h)
{
    int p;
    if ((h & 0xFFFFFFF0u) == LS_FAKE)
        return (int)(h & 3u);
    for (p = 0; p < 4; p++)
        if (rd32(LS_PAD_HANDLES + (uint32_t)p * 0x38u) == h)
            return p;
    return -1;
}

/* ── the wrappers' halves (rdr_hooks.c) ─────────────────────── */

/* XGetDeviceChanges(type, &ins, &rem): 1 = handled, *ret set */
int rdr_ls_device_changes(uint32_t ins_va, uint32_t rem_va, uint32_t *ret)
{
    uint32_t f = cur_frame();
    LsFrame *e;
    if (rdr_lockstep_mode() == 4) {
        /* online: every player's pad arrives at the first look, on every
         * machine alike */
        static int told;
        uint32_t ins = told ? 0u : ls_net_ports_mask(), zero = 0;
        told = 1;
        memcpy(gm(ins_va), &ins, 4);
        memcpy(gm(rem_va), &zero, 4);
        *ret = ins ? 1u : 0u;
        return 1;
    }
    if (rdr_lockstep_mode() != 2)
        return 0;
    e = f != s_changes_frame ? frame_at(f, 0) : NULL;
    s_changes_frame = f;
    {
        uint32_t zero = 0;
        memcpy(gm(ins_va), e ? &e->ins : &zero, 4);
        memcpy(gm(rem_va), e ? &e->rem : &zero, 4);
    }
    *ret = e && (e->ins | e->rem) ? 1u : 0u;
    return 1;
}

void rdr_ls_device_changes_seen(uint32_t ins_va, uint32_t rem_va)
{
    uint32_t a = rd32(ins_va), b = rd32(rem_va);
    if (rdr_lockstep_mode() == 1 && (a | b)) {
        fprintf(s_rec, "D %u %x %x\n", cur_frame(), a, b);
        fflush(s_rec);
    }
}

/* XInputOpen(type, port, slot, params): 1 = handled (a virtual handle) */
int rdr_ls_open(uint32_t port, uint32_t *ret)
{
    int m = rdr_lockstep_mode();
    if (m != 2 && m != 4)
        return 0;
    *ret = LS_FAKE | (port & 3u);
    memset(s_cur[port & 3], 0, LS_STATE_LEN);
    return 1;
}

int rdr_ls_virtual(uint32_t h) { int m = rdr_lockstep_mode(); return (m == 2 || m == 4) && (h & 0xFFFFFFF0u) == LS_FAKE; }

/* Showdown's loading screen (sub_000685A0, run by the level loader's thread
 * while the main thread loads) waits, once the load is done, for A or Start
 * on the pad (0x00068699: the edge in the pad record), and only then lets
 * the main thread on. Its frames are loading frames -- not lockstep frames,
 * so the pads do not move there -- and the online game stood at "loading"
 * forever (harness: host and guest, 25 minutes). Online, its reads see A
 * pressed and let go in turn, so each machine goes on as soon as its own
 * load is done, as a chapter's load does (it waits for no press). */
static int s_press_loop;

/* The menus before a mission -- the weapon select, the shop, then "Start /
 * Weapon Selection / Shop" (Flash menus, their frame in rdr_overrides.c)
 * -- run while the level loads (sub_00069B20 at 0x00069C15): loading
 * frames, where the pads do not move, so nobody could buy, choose or leave
 * (user). A frame in which one of them ran is a lockstep frame after all:
 * the pads are passed round and every machine's menu sees the same
 * presses on the same frames; the frames between (and the wait for the
 * load after Start) stay loading frames. Its fingerprint is only the
 * frame's number, the load going on beside it at each machine's pace. */
static volatile long s_loading;
static int s_prelevel_menu;
static uint32_t s_prelevel_said;
void rdr_ls_prelevel_menu(void)
{
    if (rdr_lockstep_mode() != 4 || s_loading <= 0)
        return;
    s_prelevel_menu = 1;
    if (!s_prelevel_said++)
        fprintf(stderr, "[LS] online: the menus before the mission are played in step\n");
}

/* XInputGetState(h, &state): play gives the frame's state */
int rdr_ls_get_state(uint32_t h, uint32_t st_va, uint32_t *ret)
{
    int p;
    LsFrame *e;
    if (rdr_lockstep_mode() == 4 && (p = port_of(h)) >= 0) {
        ls_net_state(p, cur_frame(), gm(st_va));
        if (s_press_loop) {
            static uint32_t n[4];
            uint8_t *st = gm(st_va);
            uint32_t pk;
            n[p]++;
            st[6] = (n[p] & 1) ? 0xFF : 0x00;   /* bAnalogButtons[0]: A */
            memcpy(&pk, st, 4);
            pk += n[p];
            memcpy(st, &pk, 4);
        }
        *ret = 0;
        return 1;
    }
    if (rdr_lockstep_mode() != 2 || (p = port_of(h)) < 0)
        return 0;
    e = frame_at(cur_frame(), 0);
    if (e && (e->have & (1u << p)) && memcmp(s_cur[p] + 4, e->st[p] + 4, LS_STATE_LEN - 4)) {
        uint32_t pk;
        memcpy(&pk, s_cur[p], 4);
        memcpy(s_cur[p] + 4, e->st[p] + 4, LS_STATE_LEN - 4);
        pk++;
        memcpy(s_cur[p], &pk, 4);       /* the packet count: up once a change */
    }
    memcpy(gm(st_va), s_cur[p], LS_STATE_LEN);
    *ret = 0;
    return 1;
}

void rdr_ls_state_seen(uint32_t h, uint32_t st_va)
{
    static uint8_t last[4][LS_STATE_LEN];
    static uint8_t had[4];
    static uint32_t frame_of[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    int p, k;
    if (rdr_lockstep_mode() != 1 || (p = port_of(h)) < 0)
        return;
    /* One state a frame: the pad is read more than once in a frame (the
     * pad records, sub_00047940, and others), and a press landing between
     * two reads was seen by one and not the other -- recorded as the
     * frame's last, so played back to both (the first runs parted at a
     * Start press). The frame's first read is the frame's state. */
    if (frame_of[p] == cur_frame() && had[p]) {
        memcpy(gm(st_va), last[p], LS_STATE_LEN);
        return;
    }
    frame_of[p] = cur_frame();
    if (had[p] && !memcmp(last[p] + 4, gm(st_va) + 4, LS_STATE_LEN - 4)) {
        memcpy(last[p], gm(st_va), 4);  /* the packet count kept up to date */
        return;                         /* recorded only when it changes (not the packet count) */
    }
    memcpy(last[p], gm(st_va), LS_STATE_LEN);
    had[p] = 1;
    fprintf(s_rec, "P %u %d ", cur_frame(), p);
    for (k = 0; k < LS_STATE_LEN; k++)
        fprintf(s_rec, "%02x", last[p][k]);
    fputc('\n', s_rec);
    fflush(s_rec);
}

/* ── each frame: the fixed step, and the fingerprint ────────── */

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

/* The game's clock, made of frames. Everything the game times reads the
 * CPU's tick count through sub_00084880 (rdtsc; 733 MHz on the console):
 * the frame time (sub_00083670), and timers and waits elsewhere (122
 * calls). Its own fixed step ([0x0065AA9D], a developer switch nothing
 * sets) held the game on the legal screen, so instead the count itself
 * is made deterministic: each frame starts 1/60 s of ticks after the last,
 * and each read within a frame adds 1000 ticks (about 1.4 us), so a wait
 * on the clock still ends -- after the same number of reads on every
 * machine. */
#define LS_TICKS_PER_SEC   733333333ull
#define LS_TICKS_PER_FRAME (LS_TICKS_PER_SEC / 60ull)
#if defined(_MSC_VER)
#define LS_TLS __declspec(thread)
#else
#define LS_TLS __thread
#endif
/* The frame's start, and each thread's reads within it: a thread's clock
 * is the frame's start plus 1000 ticks a read of its own, so what one
 * thread reads never depends on how often another one read (the game has
 * twelve threads; a shared count made the main thread's time depend on
 * how busy the movie and sound threads had been). */
static volatile uint64_t s_vt_frame = LS_TICKS_PER_SEC;
static LS_TLS uint64_t t_vt_base, t_vt_reads;

static void ls_clock_frame(void)
{
    s_vt_frame += LS_TICKS_PER_FRAME;
}

static uint64_t ls_now_ticks(void)
{
    uint64_t base = s_vt_frame;
    if (t_vt_base != base) {
        t_vt_base = base;
        t_vt_reads = 0;
    }
    return base + ++t_vt_reads * 1000u;
}

/* the frame's tick count without the reads within it (rdr_hooks.c: the
 * start's random seed) */
int rdr_ls_ticks_base(uint64_t *t)
{
    if (!rdr_lockstep_mode())
        return 0;
    *t = s_vt_frame;
    return 1;
}

/* Time for our own timers that change the game (co-op: a new level's
 * weapons, the sound voice taken when all are busy). Online that is the
 * frames played in step, a sixtieth of a second each: the clock on the
 * wall differs between machines, and a timer read from it fires on a
 * different frame on each (two games parted that way: the host waited
 * three seconds for the other's pad, its two seconds were up a frame
 * after, the other's not). 1 = given; 0 = not online, use the wall. */
int rdr_ls_game_ns(uint64_t *t)
{
    if (!rdr_lockstep_mode())
        return 0;
    *t = (uint64_t)s_lsf * 1000000000ull / 60ull;
    return 1;
}

/* the tick count's read (rdr_hooks.c, sub_00084880): 1 = given */
int rdr_ls_ticks(uint64_t *t)
{
    if (!rdr_lockstep_mode())
        return 0;
    *t = ls_now_ticks();
    return 1;
}

/* the kernel's clocks (KeQueryPerformanceCounter, KeQuerySystemTime: the
 * toolkit's xbox_clock_override_ns), from the same count */
static uint64_t ls_clock_ns(void)
{
    uint64_t t = ls_now_ticks();
    return t / 733333333ull * 1000000000ull + (t % 733333333ull) * 1000000000ull / 733333333ull;
}

/* The level loader's thread at work (rdr_hooks.c, sub_0006AF70): the main
 * loop's frame waits for it, the CPU handed over meanwhile (s_loading,
 * above). */

void rdr_ls_loading(int delta)
{
    s_loading += delta;
}

static void ls_hold_while_loading(void)
{
    extern int xbox_GuestCpuRelease(int kind, uint32_t where);
    extern void xbox_GuestCpuAcquire(void);
    extern void rdr_net_sleep_ms(int ms);
    extern uint64_t rdr_net_now_ms(void);
    uint64_t t0;
    if (s_loading <= 0)
        return;
    t0 = rdr_net_now_ms();
    while (s_loading > 0 && rdr_net_now_ms() - t0 < 120000) {
        int rel = xbox_GuestCpuRelease(0, 238);
        rdr_net_sleep_ms(1);
        if (rel)
            xbox_GuestCpuAcquire();
    }
    if (s_hash)
        fprintf(stderr, "[LS] frame %u waited %u ms for the level loader\n", cur_frame(),
                (unsigned)(rdr_net_now_ms() - t0));
}

static int in_heap(uint32_t a) { return a >= 0x80000000u && a < 0x84000000u; }

/* ── Online lockstep ("option 3" over the network) ──────────────
 *
 *   RDR_LS_HOST=<players>   host; waits for players-1 guests
 *   RDR_LS_JOIN=<host[:port]>  guest
 *   RDR_LS_PORT=27032
 *   RDR_LS_DELAY=<frames>   a fixed input delay (unset: 3 at the start,
 *                           then set by the ping; see ls_delay_tick)
 *   RDR_LS_AUTORESYNC=<n>   start again together at most n times when the
 *                           games part (5; 0 never; see ls_resync_*)
 *   RDR_LS_SIM_LAG_MS=<ms>  (tests) every packet sent this much later,
 *                           RDR_LS_SIM_JITTER_MS=<ms> up to this much more,
 *                           RDR_LS_SIM_LOSS=<percent> that many not at all
 *   RDR_LS_SIM_DESYNC=<frame>  (tests) this machine's fingerprint wrong
 *                           from that frame, in the first game only
 *
 * Every machine runs the whole game. Each frame a machine reads its own
 * controller (the first pad as the game would have it: the host pad, or
 * the keyboard and mouse), keeps it for frame + delay, and sends its last eight to the host,
 * which passes them on to the other guests; the frame does not start
 * until every player's pad for it is in. Every player's pad is a virtual
 * pad on its port from the first pad look (port 0 the host, then the
 * guests in the order they joined). Each machine sends its fingerprint
 * of every 30th frame; a different one is reported ("[LS] DESYNC"). */
#include "net/rdr_net.h"

#define LSN_MAGIC 0x534C4452u     /* "RDLS" */
#define LSN_RING  1024
/* A player's frame: the XINPUT_GAMEPAD (18), a command (rdr_cmd: 4), and
 * the mouse (its counts this frame, x and y, s16; the player's mouse
 * sensitivity and duel aim sensitivity, %, u16 each; bit 0 x inverted,
 * bit 1 y): every machine turns that player's camera and moves its duel
 * cursor with it alike (rdr_overrides.c, mouse_look_net, duel_mouse_aim) */
#define LSN_PAD   31
#define LSN_GAMEPAD 18
#define LSN_MOUSE 22
enum { LSN_HELLO = 1, LSN_WELCOME = 2, LSN_INPUT = 3, LSN_HASH = 4, LSN_GO = 5,
       LSN_PING = 6, LSN_PONG = 7,
       LSN_RS = 8, LSN_RS_CHUNK = 9, LSN_RS_NEED = 10, LSN_RS_READY = 11, LSN_RS_GO = 12 };
#define LSN_RS_CHUNK_N 1000

typedef struct { int used; RdrAddr addr; int port; } LsPeer;
static int s_ns = -1, s_host, s_me, s_players = 1, s_delay = 3, s_go, s_want_slot, s_go_sent;
static uint32_t s_session;              /* the lobby's game (RDR_LS_SESSION), 0 none */
static RdrAddr s_host_addr;
static LsPeer s_peer[4];
static uint8_t s_in[LSN_RING][4][LSN_PAD];
static uint32_t s_in_frame[LSN_RING][4];        /* the frame each slot holds (+1; 0 none) */
static uint32_t s_hash_of[LSN_RING], s_hash_f[LSN_RING];     /* ours, and its frame + 1 */
static uint32_t s_their_of[LSN_RING], s_their_f[LSN_RING];   /* theirs come first: kept */
static uint32_t s_pkt_cur[4];

/* how long since the poll before this one: a packet may have waited that
 * long, unread (a load, a long frame): no ping is measured across one */
static uint64_t s_last_poll_ms;
static int s_poll_gap_ms = 1000;
#define LSN_POLL_GAP_OK 150             /* (ms: a slow machine's frame is under it; a load is not) */

/* (tests) a slow network: each packet held for RDR_LS_SIM_LAG_MS, plus up
 * to RDR_LS_SIM_JITTER_MS, and sent from lsn_poll when its time comes */
#define LSN_LAGQ 512
static struct { uint64_t at; RdrAddr to; int n; uint8_t b[1100]; } s_lagq[LSN_LAGQ];
static int s_lag_ms = -1, s_jitter_ms, s_loss_pct;
static uint32_t s_lag_rng = 12345u;
static void lsn_lag_flush(void);
/* the held packets go out on time from a thread of their own, as a
 * network would carry them, whatever the game's thread is doing */
#if defined(_WIN32)
#include <windows.h>
static CRITICAL_SECTION s_lag_lk;
static void lag_lock(void) { EnterCriticalSection(&s_lag_lk); }
static void lag_unlock(void) { LeaveCriticalSection(&s_lag_lk); }
static DWORD WINAPI lag_thread(LPVOID a) { (void)a; for (;;) { lsn_lag_flush(); Sleep(1); } }
static void lag_start(void) { InitializeCriticalSection(&s_lag_lk); CreateThread(NULL, 0, lag_thread, NULL, 0, NULL); }
#else
#include <pthread.h>
static pthread_mutex_t s_lag_lk = PTHREAD_MUTEX_INITIALIZER;
static void lag_lock(void) { pthread_mutex_lock(&s_lag_lk); }
static void lag_unlock(void) { pthread_mutex_unlock(&s_lag_lk); }
static void *lag_thread(void *a) { (void)a; for (;;) { lsn_lag_flush(); rdr_net_sleep_ms(1); } return NULL; }
static void lag_start(void) { pthread_t t; pthread_create(&t, NULL, lag_thread, NULL); pthread_detach(t); }
#endif

static void lsn_raw(const RdrAddr *to, const uint8_t *b, int n)
{
    if (s_lag_ms < 0) {
        const char *e = getenv("RDR_LS_SIM_LAG_MS"), *j = getenv("RDR_LS_SIM_JITTER_MS");
        const char *l = getenv("RDR_LS_SIM_LOSS");
        s_lag_ms = e && atoi(e) > 0 ? atoi(e) : 0;
        s_jitter_ms = j && atoi(j) > 0 ? atoi(j) : 0;
        s_loss_pct = l && atoi(l) > 0 ? atoi(l) : 0;
        if (s_lag_ms || s_jitter_ms || s_loss_pct) {
            fprintf(stderr, "[LS] (test) packets sent %d ms late, up to %d more, %d%% lost\n", s_lag_ms,
                    s_jitter_ms, s_loss_pct);
            if (s_lag_ms || s_jitter_ms)
                lag_start();
        }
    }
    if (s_loss_pct) {
        s_lag_rng = s_lag_rng * 1103515245u + 12345u;
        if ((int)((s_lag_rng >> 16) % 100u) < s_loss_pct)
            return;
    }
    if ((s_lag_ms || s_jitter_ms) && n <= (int)sizeof s_lagq[0].b) {
        int k;
        lag_lock();
        for (k = 0; k < LSN_LAGQ; k++)
            if (!s_lagq[k].n) {
                s_lag_rng = s_lag_rng * 1103515245u + 12345u;
                s_lagq[k].at = rdr_net_now_ms() + (uint64_t)s_lag_ms +
                               (s_jitter_ms ? (s_lag_rng >> 16) % (uint32_t)(s_jitter_ms + 1) : 0);
                s_lagq[k].to = *to;
                memcpy(s_lagq[k].b, b, (size_t)n);
                s_lagq[k].n = n;
                lag_unlock();
                return;
            }
        lag_unlock();
    }
    if (s_ns >= 0)
        rdr_sock_send(s_ns, to, b, n);
}

static void lsn_lag_flush(void)
{
    uint64_t now;
    int k;
    if (s_lag_ms <= 0 && !s_jitter_ms)
        return;
    lag_lock();
    now = rdr_net_now_ms();
    for (k = 0; k < LSN_LAGQ; k++)
        if (s_lagq[k].n && s_lagq[k].at <= now) {
            if (s_ns >= 0)
                rdr_sock_send(s_ns, &s_lagq[k].to, s_lagq[k].b, s_lagq[k].n);
            s_lagq[k].n = 0;
        }
    lag_unlock();
}

static void lsn_send(const RdrAddr *to, uint8_t type, const void *body, int n)
{
    uint8_t pkt[1100];
    uint32_t m = LSN_MAGIC;
    if (n < 0 || n > (int)sizeof pkt - 5)
        return;
    memcpy(pkt, &m, 4);
    pkt[4] = type;
    if (n)
        memcpy(pkt + 5, body, (size_t)n);
    lsn_raw(to, pkt, 5 + n);
}

static void lsn_put(int port, uint32_t frame, const uint8_t *pad)
{
    uint32_t k = frame % LSN_RING;
    memcpy(s_in[k][port], pad, LSN_PAD);
    s_in_frame[k][port] = frame + 1;
}

/* What the player is told (the overlay, rdr_ls_status): waiting for the
 * others, or the games parted. */
static volatile int s_wait_state;               /* 0, 1 waiting for players, 2 for the host/guests to join */
static uint64_t s_heard_ms[4];                  /* the last pad that came for each player */
static volatile int s_wait_for = -1, s_wait_secs;   /* the player a frame waits for, how long */
static volatile uint32_t s_parted_at = 0xFFFFFFFFu;
static volatile int s_rs_state;                 /* 0; 1 starting again together; 2 that failed */
static int s_resyncs, s_resync_max = 5;         /* this session's restarts so far (RDR_LS_RESYNC); the most */

static void rdr_ls_desync_notice(uint32_t f) { s_parted_at = f; }

int rdr_ls_status(char *buf, int n)
{
    if (rdr_lockstep_mode() != 4)
        return 0;
    if (s_rs_state == 1)
        snprintf(buf, (size_t)n, "Online: the games parted. Starting again together on the host's save...");
    else if (s_parted_at != 0xFFFFFFFFu)
        snprintf(buf, (size_t)n, "Online: the games parted at frame %u. Restart to play on", (unsigned)s_parted_at);
    else if (s_wait_state == 2 && s_resyncs)
        snprintf(buf, (size_t)n, "Online: started again after the games parted; waiting for everyone...");
    else if (s_wait_state == 2)
        snprintf(buf, (size_t)n, s_host ? "Online: waiting for the other players to join..." : "Online: looking for the host...");
    else if (s_wait_state == 1 && s_wait_for >= 0 && s_wait_secs >= 5)
        snprintf(buf, (size_t)n, "Online: no word from player %d for %d s (a slow connection, or their game "
                 "stopped). Online Co-op > Leave the online game to stop waiting", s_wait_for + 1, s_wait_secs);
    else if (s_wait_state == 1)
        snprintf(buf, (size_t)n, "Online: waiting for the other players...");
    else
        return 0;
    return 1;
}

/* What went into each frame's fingerprint, kept for the last 128 frames:
 * when the games part, every machine writes those since the last frame
 * the fingerprints agreed, so the logs side by side show the first frame
 * and the first thing that differed (the random numbers, a player's place
 * or health, the number of players), and the game's own frame count and
 * clock. */
#define LS_DETAIL_N 128
typedef struct {
    uint32_t f, game_frame, rng, rng2, cnt, hash;
    uint64_t clock;
    float pos[4][3], hp[4];
} LsDetail;
static LsDetail s_detail[LS_DETAIL_N];

/* Who drew the game's random numbers in each frame (the four rand
 * functions' entries, rdr_hooks.c): the return addresses of the first
 * LS_RAND_N, a high bit set for a draw on a thread other than the main
 * loop's. A parting with the same positions and only the number apart
 * (user logs: in frame 1605 one machine drew 18, the other 16) says which
 * code drew on one machine only when the two logs' lists are put side by
 * side. */
#define LS_RAND_N 32
static struct { uint32_t f, n; uint32_t ret[LS_RAND_N]; } s_rand[LS_DETAIL_N];
static LS_TLS int t_main_loop;

void rdr_ls_rand_seen(uint32_t ret)
{
    uint32_t f, k;
    if (!s_mode)
        return;
    f = cur_frame();
    k = f % LS_DETAIL_N;
    if (s_rand[k].f != f) {
        s_rand[k].f = f;
        s_rand[k].n = 0;
    }
    if (s_rand[k].n < LS_RAND_N)
        s_rand[k].ret[s_rand[k].n] = ret | (t_main_loop ? 0 : 0x80000000u);
    s_rand[k].n++;
}

static void ls_detail_dump(uint32_t from, uint32_t to)
{
    uint32_t f;
    fprintf(stderr, "[LS] what each frame held, %u to %u (compare with the other machines' logs):\n", from, to);
    for (f = from; f <= to; f++) {
        const LsDetail *d = &s_detail[f % LS_DETAIL_N];
        uint32_t k;
        char line[400];
        int n;
        if (d->f != f)
            continue;
        n = snprintf(line, sizeof line, "[LS]   %u: hash %08X game %u clock %llu rng %08X %08X players %u",
                     f, d->hash, d->game_frame, (unsigned long long)d->clock, d->rng, d->rng2, d->cnt);
        for (k = 0; k < d->cnt && k < 4 && n > 0 && n < (int)sizeof line; k++)
            n += snprintf(line + n, sizeof line - (size_t)n, " | %.4f %.4f %.4f hp %.3f",
                          d->pos[k][0], d->pos[k][1], d->pos[k][2], d->hp[k]);
        fprintf(stderr, "%s\n", line);
        if (s_rand[f % LS_DETAIL_N].f == f && s_rand[f % LS_DETAIL_N].n) {
            const uint32_t *r = s_rand[f % LS_DETAIL_N].ret;
            uint32_t j, c = s_rand[f % LS_DETAIL_N].n;
            n = snprintf(line, sizeof line, "[LS]     rand x%u:", (unsigned)c);
            for (j = 0; j < c && j < LS_RAND_N && n > 0 && n < (int)sizeof line - 12; j++)
                n += snprintf(line + n, sizeof line - (size_t)n, " %s%06X", (r[j] >> 31) ? "*" : "",
                              (unsigned)(r[j] & 0x7FFFFFFFu));
            fprintf(stderr, "%s\n", line);
        }
    }
    fflush(stderr);
}

static uint32_t s_last_frame_done;
static int s_rs_want;                           /* host: start again together, at the next chance */

static void lsn_hash_check(uint32_t f, uint32_t ours, uint32_t theirs)
{
    static int said;
    static uint32_t first_bad = 0xFFFFFFFFu;
    if (ours == theirs)
        return;
    if (first_bad == 0xFFFFFFFFu) {
        first_bad = f;
        fprintf(stderr, "[LS] DESYNC at frame %u: ours %08X, theirs %08X -- the games have parted\n",
                f, ours, theirs);
        ls_detail_dump(f >= 30 ? f - 30 : 0, s_last_frame_done > f ? s_last_frame_done : f);
        rdr_ls_desync_notice(f);
        /* The host decides (it sees every machine's fingerprint, so it
         * sees every parting): all start again together on its save. */
        if (s_host) {
            /* Started again already, and parted again within a minute of
             * the start: the start again puts every game at the same place
             * as before, so it would part there again, and again (the
             * user: five starts again in a row, each a minute of loading,
             * "completely unplayable"). The games carry on instead, as
             * before 231. */
            if (s_resyncs > 0 && f < 3600u) {
                fprintf(stderr, "[LS] parted again soon after starting again: not again (it would part at the "
                        "same place); the games carry on\n");
                s_resync_max = s_resyncs;
            }
            if (s_resyncs < s_resync_max) {
                s_rs_want = 1;
                fprintf(stderr, "[LS] starting again together (%d of at most %d)\n", s_resyncs + 1, s_resync_max);
            } else
                fprintf(stderr, "[LS] started again %d times already: not again (RDR_LS_AUTORESYNC)\n", s_resyncs);
        }
    } else if (said++ < 5)
        fprintf(stderr, "[LS] DESYNC at frame %u too\n", f);
}

/* ── the ping, and the input delay it asks for ──────────────────
 *
 * Each frame waits until every player's pad for it is in. A machine puts
 * its pad for frame + delay, so a delay shorter than the trip to the
 * others stalled every frame; a long one made the controls slow for
 * nothing. The delay is each machine's own (only the pads cross the
 * network, each for the frame its player's machine put it at, so the
 * others need not know it): set from the round trip to the host (and the
 * host's to the other guests, its pads passing through the host): the
 * TCP way: a smoothed time and its spread. Up straight away, down one
 * frame at a time after five steady seconds. */
typedef struct { double srtt, rttvar; int have; } LsRtt;     /* have: the samples so far */
static LsRtt s_rtt[4];                          /* host: each guest's; guest: [0] the host's */
static int s_relay_ms;                          /* guest: the host's longest to the other guests */
static int s_delay_fixed, s_delay_down;
static double s_frame_ms = 16.667;              /* the lockstep frame's wall time, smoothed */

static void rtt_sample(LsRtt *r, double ms)
{
    if (ms < 0 || ms > 5000)
        return;
    if (!r->have) {
        r->srtt = ms;
        r->rttvar = ms / 4;
        r->have = 1;
    } else {
        r->have++;
        double d = ms - r->srtt;
        r->rttvar += ((d < 0 ? -d : d) - r->rttvar) / 4;
        r->srtt += d / 8;
    }
}

static int rtt_longest_but(int skip)
{
    int p, m = 0;
    for (p = 1; p < 4; p++)
        if (p != skip && s_peer[p].used && s_rtt[p].have && s_rtt[p].srtt > m)
            m = (int)s_rtt[p].srtt;
    return m;
}

/* the one-way time my pads take to reach the furthest machine, with room
 * for its spread */
static double ls_need_ms(void)
{
    int p;
    double m = 0;
    if (s_host) {
        for (p = 1; p < s_players; p++)
            if (s_peer[p].used && s_rtt[p].have >= 4) {
                double v = s_rtt[p].srtt / 2 + 2 * s_rtt[p].rttvar;
                if (v > m)
                    m = v;
            }
    } else if (s_rtt[0].have >= 4)
        m = s_rtt[0].srtt / 2 + 2 * s_rtt[0].rttvar + s_relay_ms / 2.0;
    return m;
}

int rdr_ls_net_info(int *ping_ms, int *delay)
{
    int p, m = 0;
    if (rdr_lockstep_mode() != 4)
        return 0;
    if (s_host) {
        for (p = 1; p < 4; p++)
            if (s_rtt[p].have && s_rtt[p].srtt > m)
                m = (int)s_rtt[p].srtt;
    } else if (s_rtt[0].have)
        m = (int)s_rtt[0].srtt;
    if (ping_ms)
        *ping_ms = m;
    if (delay)
        *delay = s_delay;
    return 1;
}

static void ls_delay_tick(uint32_t frame)
{
    double need, fm;
    int want;
    if (s_delay_fixed || frame % 30)
        return;
    need = ls_need_ms();
    if (need <= 0)
        return;                                 /* no ping yet */
    fm = s_frame_ms < 16.667 ? 16.667 : s_frame_ms > 50 ? 50 : s_frame_ms;
    /* the trip, and a frame for the other machine being a frame ahead */
    want = (int)(need / fm + 0.999) + 1;
    if (want < 2) want = 2;
    if (want > 15) want = 15;
    if (want > s_delay) {
        int to = want > s_delay + 4 ? s_delay + 4 : want;
        fprintf(stderr, "[LS] input delay %d -> %d frames (pads %.0f ms on the way, frame %.1f ms)\n",
                s_delay, to, need, s_frame_ms);
        s_delay = to;
        s_delay_down = 0;
    } else if (want < s_delay - 1) {
        if (++s_delay_down >= 10) {             /* (a check every 30 frames: five seconds) */
            fprintf(stderr, "[LS] input delay %d -> %d frames (pads %.0f ms on the way, frame %.1f ms)\n",
                    s_delay, s_delay - 1, need, s_frame_ms);
            s_delay--;
            s_delay_down = 0;
        }
    } else
        s_delay_down = 0;
}

static void lsn_ping_all(void)
{
    static uint64_t last;
    uint64_t now = rdr_net_now_ms();
    uint32_t t = (uint32_t)now;
    int p;
    if (now - last < 250 || s_poll_gap_ms > LSN_POLL_GAP_OK)
        return;
    last = now;
    if (s_host) {
        for (p = 1; p < s_players; p++)
            if (s_peer[p].used)
                lsn_send(&s_peer[p].addr, LSN_PING, &t, 4);
    } else if (s_me >= 0)
        lsn_send(&s_host_addr, LSN_PING, &t, 4);
}

/* ── starting again together, when the games have parted ────────
 *
 * Every machine runs the whole game from its boot, so a game that has
 * parted from the others cannot be put back by itself: it starts again.
 * The host sends the save its game plays on (Saves/Online) to the others,
 * which write it over theirs; then every machine starts the game again in
 * lockstep, as the lobby's start does, with a new session number, and the
 * host's "Start in" takes them all back into the story (Continue: the
 * chapter they were in, from its start). No one has to leave and join
 * again. (rdr_lobby.c: rdr_online_pack_save, rdr_online_restart.) */
static struct {
    uint32_t session, size, crc;
    uint8_t *data, *have;
    int nchunks, got, go;
    int ready[4];
    uint64_t last_need, last_ready, began;
} R;
static uint32_t s_boot_save_crc;                /* host: its save as the game started */

static int rs_peer_of(const RdrAddr *from)
{
    int i;
    for (i = 1; i < 4; i++)
        if (s_peer[i].used && rdr_addr_same(&s_peer[i].addr, from))
            return i;
    return -1;
}

static void ls_cpu_nap(int ms)
{
    extern int xbox_GuestCpuRelease(int kind, uint32_t where);
    extern void xbox_GuestCpuAcquire(void);
    int rel = xbox_GuestCpuRelease(0, 238);
    rdr_net_sleep_ms(ms);
    if (rel)
        xbox_GuestCpuAcquire();
}

static void lsn_poll(void);

static void ls_resync_host(void)
{
    uint8_t *blob = NULL;
    uint32_t n = 0, crc, session;
    int p, mode, all = 0;
    uint64_t t0, last = 0;
    const char *e;
    s_rs_want = 0;
    if (rdr_online_pack_save(&blob, &n) != 0) {
        fprintf(stderr, "[LS] the save could not be read: not starting again\n");
        s_rs_state = 2;
        return;
    }
    crc = rdr_online_crc(blob, n);
    session = (uint32_t)(rdr_net_now_ms() * 2654435761u) ^ (s_session * 40503u) ^ crc;
    if (!session || session == s_session)
        session ^= 0x5A5A5A5Bu;
    /* "Start in": the chapter by the book's Continue, unless the game
     * started a new story and nothing has been saved since */
    e = getenv("RDR_ONLINE_START");
    mode = e ? atoi(e) : 0;
    if (mode == 2 && crc != s_boot_save_crc)
        mode = 1;
    memset(&R, 0, sizeof R);
    R.session = session;
    R.size = n;
    R.crc = crc;
    R.data = blob;
    R.nchunks = (int)((n + LSN_RS_CHUNK_N - 1) / LSN_RS_CHUNK_N);
    s_rs_state = 1;
    fprintf(stderr, "[LS] the host's save to the others: %u bytes in %d pieces; session %08X, start %d\n",
            (unsigned)n, R.nchunks, (unsigned)session, mode);
    fflush(stderr);
    t0 = rdr_net_now_ms();
    while (rdr_net_now_ms() - t0 < 60000) {
        uint64_t now = rdr_net_now_ms();
        if (now - last >= 200) {
            uint8_t b[12];
            memcpy(b, &session, 4);
            memcpy(b + 4, &n, 4);
            memcpy(b + 8, &crc, 4);
            for (p = 1; p < s_players; p++)
                if (s_peer[p].used && !R.ready[p])
                    lsn_send(&s_peer[p].addr, LSN_RS, b, 12);
            last = now;
        }
        lsn_poll();
        all = 1;
        for (p = 1; p < s_players; p++)
            all &= !s_peer[p].used || R.ready[p];
        if (all)
            break;
        ls_cpu_nap(2);
    }
    fprintf(stderr, "[LS] %s: everyone starts again\n", all ? "every guest has the save" : "a guest did not answer");
    for (p = 0; p < 10; p++) {
        int q;
        for (q = 1; q < s_players; q++)
            if (s_peer[q].used)
                lsn_send(&s_peer[q].addr, LSN_RS_GO, &session, 4);
        rdr_net_sleep_ms(20);
    }
    while (s_lag_ms > 0 || s_jitter_ms) {       /* (tests: the held packets out first) */
        int k, left = 0;
        for (k = 0; k < LSN_LAGQ; k++)
            left |= s_lagq[k].n;
        if (!left)
            break;
        rdr_net_sleep_ms(2);
    }
    fflush(stderr);
    rdr_sock_close(s_ns);
    s_ns = -1;
    rdr_online_restart(NULL, 0, session, mode, s_resyncs + 1);
    fprintf(stderr, "[LS] could not start again\n");
    s_rs_state = 2;
}

static void ls_resync_guest(void)
{
    uint64_t t0 = rdr_net_now_ms();
    fprintf(stderr, "[LS] the host starts everyone again: %u bytes of save, session %08X\n",
            (unsigned)R.size, (unsigned)R.session);
    fflush(stderr);
    while (rdr_net_now_ms() - t0 < 90000) {
        uint64_t now = rdr_net_now_ms();
        lsn_poll();
        if (R.got < R.nchunks) {
            if (now - R.last_need >= 50) {
                uint8_t b[4 + 1 + 64 * 2];
                int k, c = 0;
                memcpy(b, &R.session, 4);
                for (k = 0; k < R.nchunks && c < 64; k++)
                    if (!R.have[k]) {
                        uint16_t i16 = (uint16_t)k;
                        memcpy(b + 5 + c * 2, &i16, 2);
                        c++;
                    }
                b[4] = (uint8_t)c;
                lsn_send(&s_host_addr, LSN_RS_NEED, b, 5 + c * 2);
                R.last_need = now;
            }
        } else if (rdr_online_crc(R.data, R.size) != R.crc) {
            fprintf(stderr, "[LS] the host's save arrived wrong: again\n");
            memset(R.have, 0, (size_t)R.nchunks);
            R.got = 0;
        } else {
            if (now - R.last_ready >= 100) {
                lsn_send(&s_host_addr, LSN_RS_READY, &R.session, 4);
                R.last_ready = now;
            }
            if (R.go) {
                rdr_sock_close(s_ns);
                s_ns = -1;
                rdr_online_restart(R.data, R.size, R.session, -1, s_resyncs + 1);
                fprintf(stderr, "[LS] the host's save could not be written: not starting again\n");
                s_rs_state = 2;
                return;
            }
        }
        ls_cpu_nap(2);
    }
    fprintf(stderr, "[LS] the host did not finish starting again\n");
    s_rs_state = 2;
}

/* at the frame's start and while it waits: 1 if a restart was tried (it
 * does not come back if it went) */
static int ls_resync_due(void)
{
    if (s_rs_state == 2)
        return 0;
    if (s_host && s_rs_want) {
        ls_resync_host();
        return 1;
    }
    if (!s_host && s_rs_state == 1 && R.data && !R.began) {
        R.began = 1;
        ls_resync_guest();
        return 1;
    }
    return 0;
}

/* INPUT body: port u8, count u8, first frame u32, count x 18 bytes */
static void lsn_handle(const uint8_t *b, int n, const RdrAddr *from)
{
    uint32_t m;
    int i;
    if (n < 5 || (memcpy(&m, b, 4), m != LSN_MAGIC))
        return;
    switch (b[4]) {
    case LSN_HELLO:
        if (s_host) {
            int slot = -1, want = n >= 6 ? b[5] : 0;
            uint32_t sess = 0;
            if (n >= 10)
                memcpy(&sess, b + 6, 4);
            if (s_session && sess != s_session)
                break;                  /* another game's (an old lobby's) */
            for (i = 1; i < 4; i++)
                if (s_peer[i].used && rdr_addr_same(&s_peer[i].addr, from))
                    slot = i;
            /* the player the lobby made it, when it says */
            if (slot < 0 && want >= 1 && want < s_players && !s_peer[want].used) {
                s_peer[want].used = 1;
                s_peer[want].addr = *from;
                s_peer[want].port = want;
                slot = want;
                fprintf(stderr, "[LS] a guest joined: player %d\n", want + 1);
            }
            for (i = 1; slot < 0 && i < s_players; i++)
                if (!s_peer[i].used) {
                    s_peer[i].used = 1;
                    s_peer[i].addr = *from;
                    s_peer[i].port = i;
                    slot = i;
                    fprintf(stderr, "[LS] a guest joined: player %d\n", i + 1);
                }
            if (slot > 0) {
                uint8_t w[2] = { (uint8_t)slot, (uint8_t)s_players };
                lsn_send(from, LSN_WELCOME, w, 2);
                if (s_go_sent)
                    lsn_send(from, LSN_GO, NULL, 0);    /* (its go was lost: it asks again) */
            }
        }
        break;
    case LSN_WELCOME:
        if (!s_host && n >= 7 && s_me < 0 && rdr_addr_same(from, &s_host_addr) && b[5] >= 1 && b[5] < 4 &&
            b[6] >= 2 && b[6] <= 4 && b[5] < b[6]) {
            s_me = b[5];
            s_players = b[6];
            fprintf(stderr, "[LS] joined: player %d of %d\n", s_me + 1, s_players);
        }
        break;
    case LSN_GO:
        if (!s_host && rdr_addr_same(from, &s_host_addr))
            s_go = 1;
        break;
    case LSN_INPUT:
        /* Only from who has that player: the host takes a guest's own
         * pads from that guest; a guest takes pads from the host only (its
         * own and the other guests', passed on). Never this machine's own
         * player, and only for frames near ours: a stray or forged packet
         * (the port is open to the internet) could otherwise move a player
         * on one machine only, run a command, or wipe a pad not yet used. */
        if (n >= 11) {
            int port = b[5], cnt = b[6], k = s_host ? rs_peer_of(from) : -1;
            uint32_t first, now_f = cur_frame();
            memcpy(&first, b + 7, 4);
            if (port >= s_players || port == s_me || cnt < 1 || cnt > 16)
                break;
            if (s_host ? (k < 0 || s_peer[k].port != port) : !rdr_addr_same(from, &s_host_addr))
                break;
            if (first + (uint32_t)cnt + 64u < now_f || first > now_f + 64u)
                break;
            if (n >= 11 + cnt * LSN_PAD) {
                for (i = 0; i < cnt; i++) {
                    uint32_t f = first + (uint32_t)i;
                    /* the first word for a frame stands (a resend is the
                     * same; nothing later replaces a pad) */
                    if (s_in_frame[f % LSN_RING][port] != f + 1)
                        lsn_put(port, f, b + 11 + i * LSN_PAD);
                }
                s_heard_ms[port] = rdr_net_now_ms();
                if (s_host)             /* passed on to the other guests */
                    for (i = 1; i < 4; i++)
                        if (s_peer[i].used && s_peer[i].port != port)
                            lsn_raw(&s_peer[i].addr, b, n);
            }
        }
        break;
    case LSN_HASH:
        if (n >= 13 && (s_host ? rs_peer_of(from) > 0 : rdr_addr_same(from, &s_host_addr))) {
            uint32_t f, h, now_f = cur_frame();
            memcpy(&f, b + 5, 4);
            memcpy(&h, b + 9, 4);
            if (f + 256u < now_f || f > now_f + 256u)
                break;                          /* (not this game's, or long gone) */
            /* a machine may be a few frames ahead (the input delay): a
             * fingerprint of a frame not ours yet waits for ours */
            if (s_hash_f[f % LSN_RING] == f + 1)
                lsn_hash_check(f, s_hash_of[f % LSN_RING], h);
            else {
                s_their_of[f % LSN_RING] = h;
                s_their_f[f % LSN_RING] = f + 1;
            }
            if (s_host)
                for (i = 1; i < 4; i++)
                    if (s_peer[i].used && !rdr_addr_same(&s_peer[i].addr, from))
                        lsn_raw(&s_peer[i].addr, b, n);
        }
        break;
    case LSN_PING:
        if (n >= 9 && (s_host ? rs_peer_of(from) > 0 : rdr_addr_same(from, &s_host_addr))) {
            /* back at once, with (the host's) the longest trip to the
             * other guests: a guest's pads go on from the host */
            uint8_t r[7];
            uint16_t relay = 0;
            if (s_host) {
                int k = rs_peer_of(from);
                int m = rtt_longest_but(k);
                relay = (uint16_t)(m > 65535 ? 65535 : m);
            }
            memcpy(r, b + 5, 4);
            memcpy(r + 4, &relay, 2);
            r[6] = (uint8_t)(s_poll_gap_ms > LSN_POLL_GAP_OK);   /* it may have waited here */
            lsn_send(from, LSN_PONG, r, 7);
        }
        break;
    case LSN_PONG:
        if (n >= 12 && (s_host ? rs_peer_of(from) > 0 : rdr_addr_same(from, &s_host_addr))) {
            uint32_t t;
            uint16_t relay;
            int k = s_host ? rs_peer_of(from) : 0;
            memcpy(&t, b + 5, 4);
            memcpy(&relay, b + 9, 2);
            if (k >= 0 && k < 4) {
                /* not across a wait on either side (a load: the trip
                 * would be the load's length) */
                if (!b[11] && s_poll_gap_ms <= LSN_POLL_GAP_OK)
                    rtt_sample(&s_rtt[k], (double)(uint32_t)((uint32_t)rdr_net_now_ms() - t));
                if (!s_host)
                    s_relay_ms = relay;
            }
        }
        break;
    case LSN_RS:
        /* guest: the host starts everyone again (only the host's word) */
        if (!s_host && n >= 17 && rdr_addr_same(from, &s_host_addr) && s_rs_state != 2) {
            uint32_t sess, size, crc;
            memcpy(&sess, b + 5, 4);
            memcpy(&size, b + 9, 4);
            memcpy(&crc, b + 13, 4);
            if (R.data && R.session == sess)
                break;                          /* (said again) */
            if (size < 8 || size > (8u << 20))
                break;
            free(R.data);
            free(R.have);
            memset(&R, 0, sizeof R);
            R.session = sess;
            R.size = size;
            R.crc = crc;
            R.nchunks = (int)((size + LSN_RS_CHUNK_N - 1) / LSN_RS_CHUNK_N);
            R.data = (uint8_t *)malloc(size);
            R.have = (uint8_t *)calloc((size_t)R.nchunks, 1);
            if (!R.data || !R.have) {
                free(R.data);
                free(R.have);
                memset(&R, 0, sizeof R);
                break;
            }
            s_rs_state = 1;
        }
        break;
    case LSN_RS_CHUNK:
        if (!s_host && n >= 11 && R.data && rdr_addr_same(from, &s_host_addr)) {
            uint32_t sess;
            uint16_t idx;
            uint32_t off, len;
            memcpy(&sess, b + 5, 4);
            memcpy(&idx, b + 9, 2);
            if (sess != R.session || idx >= R.nchunks || R.have[idx])
                break;
            off = (uint32_t)idx * LSN_RS_CHUNK_N;
            len = R.size - off < LSN_RS_CHUNK_N ? R.size - off : LSN_RS_CHUNK_N;
            if ((uint32_t)(n - 11) != len)
                break;
            memcpy(R.data + off, b + 11, len);
            R.have[idx] = 1;
            R.got++;
        }
        break;
    case LSN_RS_NEED:
        if (s_host && n >= 10 && R.data && s_rs_state == 1) {
            uint32_t sess;
            int k = rs_peer_of(from), c = b[9], j;
            memcpy(&sess, b + 5, 4);
            if (k < 0 || sess != R.session || n < 10 + c * 2)
                break;
            for (j = 0; j < c; j++) {
                uint16_t idx;
                uint8_t pk[4 + 2 + LSN_RS_CHUNK_N];
                uint32_t off, len;
                memcpy(&idx, b + 10 + j * 2, 2);
                if (idx >= R.nchunks)
                    continue;
                off = (uint32_t)idx * LSN_RS_CHUNK_N;
                len = R.size - off < LSN_RS_CHUNK_N ? R.size - off : LSN_RS_CHUNK_N;
                memcpy(pk, &R.session, 4);
                memcpy(pk + 4, &idx, 2);
                memcpy(pk + 6, R.data + off, len);
                lsn_send(from, LSN_RS_CHUNK, pk, 6 + (int)len);
            }
        }
        break;
    case LSN_RS_READY:
        if (s_host && n >= 9 && s_rs_state == 1) {
            uint32_t sess;
            int k = rs_peer_of(from);
            memcpy(&sess, b + 5, 4);
            if (k > 0 && sess == R.session && !R.ready[k]) {
                R.ready[k] = 1;
                fprintf(stderr, "[LS] player %d has the save\n", k + 1);
            }
        }
        break;
    case LSN_RS_GO:
        if (!s_host && n >= 9 && R.data && rdr_addr_same(from, &s_host_addr)) {
            uint32_t sess;
            memcpy(&sess, b + 5, 4);
            if (sess == R.session)
                R.go = 1;
        }
        break;
    }
}

static void lsn_poll(void)
{
    uint8_t buf[1500];
    RdrAddr from;
    int n;
    uint64_t now = rdr_net_now_ms();
    s_poll_gap_ms = s_last_poll_ms ? (int)(now - s_last_poll_ms) : 1000;
    s_last_poll_ms = now;
    if (s_ns < 0)
        return;
    while ((n = rdr_sock_recv(s_ns, &from, buf, sizeof buf)) > 0)
        lsn_handle(buf, n, &from);
}

static void ls_net_start(const char *host, const char *join)
{
    const char *e;
    int port = (e = getenv("RDR_LS_PORT")) && atoi(e) > 0 ? atoi(e) : 27032;
    if ((e = getenv("RDR_LS_DELAY")) && atoi(e) >= 1 && atoi(e) <= 30) {
        s_delay = atoi(e);
        s_delay_fixed = 1;
    }
    if ((e = getenv("RDR_LS_RESYNC")) && atoi(e) > 0)
        s_resyncs = atoi(e);
    if ((e = getenv("RDR_LS_AUTORESYNC")) && *e)
        s_resync_max = atoi(e) < 0 ? 0 : atoi(e);
    if ((e = getenv("RDR_LS_SESSION")) && *e)
        s_session = (uint32_t)strtoul(e, NULL, 16);
    if ((e = getenv("RDR_LS_SLOT")) && atoi(e) >= 1 && atoi(e) <= 3)
        s_want_slot = atoi(e);
    s_host = host && *host && *host != '0';
    if (s_host) {
        s_players = atoi(host) >= 2 && atoi(host) <= 4 ? atoi(host) : 2;
        s_me = 0;
        s_ns = rdr_sock_open((uint16_t)port);
    } else {
        s_me = -1;
        s_ns = rdr_sock_open(0);
        if (rdr_addr_parse(join, &s_host_addr) != 0) {
            fprintf(stderr, "[LS] cannot find %s\n", join);
            s_ns = -1;
        } else if (!strchr(join, ':'))
            s_host_addr.port = (uint16_t)port;
    }
    if (s_host) {
        uint8_t *blob = NULL;
        uint32_t n = 0;
        if (rdr_online_pack_save(&blob, &n) == 0) {
            s_boot_save_crc = rdr_online_crc(blob, n);
            free(blob);
        }
    }
    fprintf(stderr, "[LS] online lockstep: %s, input delay %d frames%s%s\n",
            s_host ? "host" : "guest", s_delay, s_delay_fixed ? " (fixed)" : " (set by the ping)",
            s_ns < 0 ? " -- NO SOCKET" : "");
    if (s_resyncs)
        fprintf(stderr, "[LS] started again together after the games parted (%d so far)\n", s_resyncs);
}

static uint32_t ls_net_ports_mask(void)
{
    return (1u << s_players) - 1u;
}

static void ls_net_state(int port, uint32_t frame, uint8_t *out)
{
    uint32_t k = frame % LSN_RING, pk;
    uint8_t pad[LSN_PAD];
    if (s_in_frame[k][port] == frame + 1)
        memcpy(pad, s_in[k][port], LSN_PAD);
    else
        memset(pad, 0, LSN_PAD);
    pk = ++s_pkt_cur[port];
    memcpy(out, &pk, 4);
    memcpy(out + 4, pad, LSN_GAMEPAD);
    {
        /* a player's command (the spawner, Give Weapon): run by every
         * machine at the same point -- this port's pad read for this frame,
         * once (the game may read a pad more than once a frame) */
        static uint32_t done[4];
        uint32_t cmd;
        memcpy(&cmd, pad + LSN_GAMEPAD, 4);
        if (cmd && done[port & 3] != frame + 1) {
            extern void rdr_cmd_run(int player, uint32_t cmd);
            done[port & 3] = frame + 1;
            fprintf(stderr, "[LS] frame %u: player %d command %08X\n", frame, port + 1, cmd);
            rdr_cmd_run(port, cmd);
        }
    }
}

/* A command from this machine's player, sent with its next input (online);
 * a few may wait. */
static volatile uint32_t s_cmd_q[8];
static volatile int s_cmd_qn;
int rdr_ls_my_port(void) { return rdr_lockstep_mode() == 4 ? s_me : 0; }

int rdr_ls_send_cmd(uint32_t cmd)
{
    if (rdr_lockstep_mode() != 4 || s_cmd_qn >= 8)
        return 0;
    s_cmd_q[s_cmd_qn++] = cmd;
    return 1;
}
static uint32_t ls_take_cmd(void)
{
    uint32_t c = 0;
    int k;
    if (s_cmd_qn > 0) {
        c = s_cmd_q[0];
        for (k = 1; k < s_cmd_qn; k++)
            s_cmd_q[k - 1] = s_cmd_q[k];
        s_cmd_qn--;
    }
    return c;
}

/* The last frame this machine has put its own pad at (frame + the delay
 * it had then), and what a frame left out held (the delay going down: two
 * frames' pads would land on one; the one left out gives its buttons to
 * the next, so a quick press is not lost). */
static uint32_t s_last_put;
static uint8_t s_carry[LSN_GAMEPAD];
static int s_carry_on;
static long s_mouse_acc_x, s_mouse_acc_y;      /* counts not sent yet */

/* A player's mouse for this frame (online): 1 and its counts and settings
 * if the frame's pad is in, else 0 */
int rdr_ls_mouse(int port, long *dx, long *dy, int *sens, int *duel_sens, int *inv_x, int *inv_y)
{
    uint32_t f = cur_frame(), k = f % LSN_RING;
    const uint8_t *m;
    int16_t x, y;
    uint16_t a, b;
    if (rdr_lockstep_mode() != 4 || port < 0 || port > 3 || s_in_frame[k][port] != f + 1)
        return 0;
    m = s_in[k][port] + LSN_MOUSE;
    memcpy(&x, m, 2);
    memcpy(&y, m + 2, 2);
    memcpy(&a, m + 4, 2);
    memcpy(&b, m + 6, 2);
    *dx = x;
    *dy = y;
    *sens = a ? a : 100;
    *duel_sens = b ? b : 100;
    *inv_x = m[8] & 1;
    *inv_y = (m[8] >> 1) & 1;
    return 1;
}

/* this machine's mouse into its pad for the frame (the counts since the
 * last, and its settings), what does not fit kept for the next */
static void mouse_into(uint8_t *pad)
{
    extern int rdr_input_take_look_net(long *dx, long *dy, int *sens, int *duel_sens, int *inv_x, int *inv_y);
    long dx = 0, dy = 0, sx, sy;
    int sens = 100, dsens = 100, ix = 0, iy = 0;
    int16_t x, y;
    uint16_t a, b;
    rdr_input_take_look_net(&dx, &dy, &sens, &dsens, &ix, &iy);
    s_mouse_acc_x += dx;
    s_mouse_acc_y += dy;
    sx = s_mouse_acc_x > 32767 ? 32767 : s_mouse_acc_x < -32767 ? -32767 : s_mouse_acc_x;
    sy = s_mouse_acc_y > 32767 ? 32767 : s_mouse_acc_y < -32767 ? -32767 : s_mouse_acc_y;
    s_mouse_acc_x -= sx;
    s_mouse_acc_y -= sy;
    x = (int16_t)sx;
    y = (int16_t)sy;
    a = (uint16_t)sens;
    b = (uint16_t)dsens;
    memcpy(pad + LSN_MOUSE, &x, 2);
    memcpy(pad + LSN_MOUSE + 2, &y, 2);
    memcpy(pad + LSN_MOUSE + 4, &a, 2);
    memcpy(pad + LSN_MOUSE + 6, &b, 2);
    pad[LSN_MOUSE + 8] = (uint8_t)((ix ? 1 : 0) | (iy ? 2 : 0));
}

/* The start: everyone in (the host says go), then frame by frame. */
static void ls_net_frame(uint32_t frame, uint32_t hash)
{
    static uint32_t first = 0xFFFFFFFFu;
    static uint64_t last_frame_ms;
    uint8_t body[11 + 8 * LSN_PAD];
    uint8_t mine[LSN_PAD];
    uint64_t t0, last_send = 0, last_say;
    int p, k;
    if (s_ns < 0)
        return;
    {
        /* (tests) a parting on purpose, in the first game only */
        static int sim = -2;
        if (sim == -2) {
            const char *e = getenv("RDR_LS_SIM_DESYNC");
            sim = e && *e && !s_resyncs ? atoi(e) : -1;
        }
        if (sim >= 0 && frame >= (uint32_t)sim)
            hash ^= 0x0BADF00Du;
    }
    if (first == 0xFFFFFFFFu) {
        /* wait for the others (the host for every guest, a guest for its
         * welcome and the go) */
        uint64_t t = rdr_net_now_ms(), said = 0;
        for (;;) {
            int all = 1;
            lsn_poll();
            if (s_host) {
                for (p = 1; p < s_players; p++)
                    all &= s_peer[p].used;
                if (all) {
                    for (k = 0; k < 10; k++)
                        for (p = 1; p < s_players; p++)
                            lsn_send(&s_peer[p].addr, LSN_GO, NULL, 0);
                    s_go_sent = 1;
                    break;
                }
            } else {
                static uint64_t hello_t;
                /* hello until welcomed, and again now and then until the
                 * go comes (a lost go is sent again for it) */
                if (rdr_net_now_ms() - hello_t >= (s_me < 0 ? 250u : 500u)) {
                    uint8_t hb[5];
                    hb[0] = (uint8_t)s_want_slot;
                    memcpy(hb + 1, &s_session, 4);
                    lsn_send(&s_host_addr, LSN_HELLO, hb, 5);
                    hello_t = rdr_net_now_ms();
                }
                if (s_me >= 0 && s_go)
                    break;
            }
            s_wait_state = 2;
            if (rdr_net_now_ms() - said >= 3000) {
                fprintf(stderr, "[LS] waiting for %s...\n", s_host ? "the guests" : "the host");
                said = rdr_net_now_ms();
            }
            {
                extern int xbox_GuestCpuRelease(int kind, uint32_t where);
                extern void xbox_GuestCpuAcquire(void);
                int rel = xbox_GuestCpuRelease(0, 238);
                rdr_net_sleep_ms(5);
                if (rel)
                    xbox_GuestCpuAcquire();
            }
            (void)t;
        }
        s_wait_state = 0;
        first = frame;
        memset(mine, 0, sizeof mine);
        /* the first frames: nobody pressed anything (every machine starts
         * with the same delay, 3 or RDR_LS_DELAY: the lobby sets none) */
        for (k = 0; k < s_delay; k++)
            for (p = 0; p < s_players; p++)
                lsn_put(p, frame + (uint32_t)k, mine);
        s_last_put = frame + (uint32_t)s_delay - 1;
        fprintf(stderr, "[LS] everyone in at frame %u: %d players\n", frame, s_players);
    }
    {
        /* the frame's wall time (a slow machine's frames are longer: fewer
         * of them cover the trip) */
        uint64_t now = rdr_net_now_ms();
        if (last_frame_ms && now - last_frame_ms < 1000)
            s_frame_ms += ((double)(now - last_frame_ms) - s_frame_ms) / 32;
        last_frame_ms = now;
    }
    if (ls_resync_due())
        return;
    lsn_ping_all();
    ls_delay_tick(frame);
    /* mine, for frame + delay */
    {
        /* the first pad as the game would have had it (the toolkit's
         * report for USB pad 0: the host pad, the keyboard and mouse through
         * the game's filter, and scripted presses): bytes 2..19 are the
         * XINPUT_GAMEPAD */
        extern int usb_gamepad_report(int pad, uint8_t *out, int max);
        uint8_t rep[20];
        memset(rep, 0, sizeof rep);
        usb_gamepad_report(0, rep, (int)sizeof rep);
        memcpy(mine, rep + 2, LSN_GAMEPAD);
        {
            /* the host's "Start in": its presses, frame by frame
             * (rdr_overrides.c; whatever port its keyboard is on) */
            extern void rdr_online_start_merge(uint8_t *gamepad);
            if (s_host)
                rdr_online_start_merge(mine);
        }
        {
            uint32_t target = frame + (uint32_t)s_delay, f2;
            if (target <= s_last_put) {
                /* the delay went down: this frame's pad has no frame of its
                 * own; its buttons wait for the next (a command waits in
                 * the queue, the mouse's counts in theirs) */
                uint16_t w0, w1;
                extern int rdr_input_take_look_net(long *dx, long *dy, int *sens, int *duel_sens, int *inv_x,
                                                   int *inv_y);
                long dx = 0, dy = 0;
                int a, b, c2, d2;
                rdr_input_take_look_net(&dx, &dy, &a, &b, &c2, &d2);
                s_mouse_acc_x += dx;
                s_mouse_acc_y += dy;
                if (!s_carry_on) {
                    memcpy(s_carry, mine, LSN_GAMEPAD);
                    s_carry_on = 1;
                } else {
                    memcpy(&w0, s_carry, 2);
                    memcpy(&w1, mine, 2);
                    w0 |= w1;
                    memcpy(s_carry, mine, LSN_GAMEPAD);
                    memcpy(s_carry, &w0, 2);
                }
            } else {
                uint32_t c = ls_take_cmd();
                if (s_carry_on) {
                    uint16_t w0, w1;
                    memcpy(&w0, s_carry, 2);
                    memcpy(&w1, mine, 2);
                    w1 |= w0;
                    memcpy(mine, &w1, 2);
                    for (k = 2; k < 10; k++)    /* (the analog buttons: the harder press) */
                        if (s_carry[k] > mine[k])
                            mine[k] = s_carry[k];
                    s_carry_on = 0;
                }
                memset(mine + LSN_GAMEPAD, 0, LSN_PAD - LSN_GAMEPAD);
                /* the delay went up: the frames between get this pad too
                 * (no command and no mouse counts: they go once, on their
                 * own frame) */
                for (f2 = s_last_put + 1; f2 < target; f2++)
                    lsn_put(s_me, f2, mine);
                memcpy(mine + LSN_GAMEPAD, &c, 4);
                mouse_into(mine);
                lsn_put(s_me, target, mine);
                s_last_put = target;
            }
        }
    }
    s_hash_of[frame % LSN_RING] = hash;
    s_hash_f[frame % LSN_RING] = frame + 1;
    if (s_their_f[frame % LSN_RING] == frame + 1) {
        lsn_hash_check(frame, hash, s_their_of[frame % LSN_RING]);
        s_their_f[frame % LSN_RING] = 0;
    }
    t0 = last_say = rdr_net_now_ms();
    for (;;) {
        int all = 1;
        uint64_t now = rdr_net_now_ms();
        if (now - last_send >= 8 || last_send == 0) {
            /* my last eight (lost ones get there with the next) */
            uint32_t from = s_last_put >= 7 ? s_last_put - 7 : 0;
            int cnt = 0;
            body[0] = (uint8_t)s_me;
            for (k = 0; k < 8; k++) {
                uint32_t f = from + (uint32_t)k, slot = f % LSN_RING;
                if (s_in_frame[slot][s_me] == f + 1) {
                    if (!cnt)
                        memcpy(body + 2, &f, 4);
                    memcpy(body + 6 + cnt * LSN_PAD, s_in[slot][s_me], LSN_PAD);
                    cnt++;
                } else if (cnt)
                    break;
            }
            body[1] = (uint8_t)cnt;
            if (cnt) {
                if (s_host) {
                    for (p = 1; p < s_players; p++)
                        if (s_peer[p].used)
                            lsn_send(&s_peer[p].addr, LSN_INPUT, body, 6 + cnt * LSN_PAD);
                } else
                    lsn_send(&s_host_addr, LSN_INPUT, body, 6 + cnt * LSN_PAD);
            }
            if (frame % 30 == 0) {
                uint8_t hb[8];
                memcpy(hb, &frame, 4);
                memcpy(hb + 4, &hash, 4);
                if (s_host) {
                    for (p = 1; p < s_players; p++)
                        if (s_peer[p].used)
                            lsn_send(&s_peer[p].addr, LSN_HASH, hb, 8);
                } else
                    lsn_send(&s_host_addr, LSN_HASH, hb, 8);
            }
            last_send = now;
        }
        lsn_poll();
        if ((s_host && s_rs_want) || (!s_host && s_rs_state == 1 && R.data && !R.began)) {
            s_wait_state = 0;
            ls_resync_due();
            return;
        }
        lsn_ping_all();
        for (p = 0; p < s_players; p++)
            all &= s_in_frame[frame % LSN_RING][p] == frame + 1;
        if (all)
            break;
        if (now - t0 >= 500) {
            s_wait_state = 1;
            for (p = 0; p < s_players; p++)
                if (s_in_frame[frame % LSN_RING][p] != frame + 1) {
                    s_wait_for = p;
                    s_wait_secs = (int)((now - (s_heard_ms[p] ? s_heard_ms[p] : t0)) / 1000);
                    break;
                }
        }
        if (now - last_say >= 3000) {
            fprintf(stderr, "[LS] frame %u: waiting for player %d's pad (%u ms; nothing from them for %d s)\n",
                    frame, s_wait_for + 1, (unsigned)(now - t0), s_wait_secs);
            last_say = now;
        }
        {
            extern int xbox_GuestCpuRelease(int kind, uint32_t where);
            extern void xbox_GuestCpuAcquire(void);
            int rel = xbox_GuestCpuRelease(0, 238);
            rdr_net_sleep_ms(1);
            if (rel)
                xbox_GuestCpuAcquire();
        }
    }
    s_wait_state = 0;
    s_wait_for = -1;
}

/* A screen fade's thread, from its creation (on the main loop, at the
 * same point on every machine) to its end: the frame waits for the whole
 * of it. Waiting only while the fade's flag was set left a gap: the
 * thread had been created but not yet run and set the flag, and a machine
 * that reached the frame's end in that gap counted a frame the others did
 * not (user logs: the same frames one frame apart from 1683 on, through a
 * cut to a cutscene, parted by 1710). Toolkit: g_xbox_thread_hook. */
#define LS_FADE_CTX 0x00106720u
static volatile int s_fade_pending;
/* The thread a movie call starts (sub_0006FE00, its data at 0x0058D3C0),
 * even with no movie: at a level's start the main loop waits for what it
 * leaves (0x0058D27C-0x0058D488, [0x0065A80C]), and the frame it ended in
 * was each PC's own: the watch (244) saw it start in frame 1390 and end in
 * 1391 on the host, the flags clear in 1393 and the game go on in 1394,
 * a frame later on the other PC. Lockstep: no frame while it runs (246). */
#define LS_MOVIE_CTX 0x0006FE00u
static volatile int s_movie_pending;

static void ls_count(volatile int *n, int event)
{
#if defined(_MSC_VER) && !defined(__clang__)
    if (event == 1)
        InterlockedIncrement((volatile LONG *)n);
    else if (event == 2 && *n > 0)
        InterlockedDecrement((volatile LONG *)n);
#else
    if (event == 1)
        __atomic_add_fetch(n, 1, __ATOMIC_SEQ_CST);
    else if (event == 2 && *n > 0)
        __atomic_sub_fetch(n, 1, __ATOMIC_SEQ_CST);
#endif
}

static void ls_thread_hook(int event, uint32_t routine, uint32_t ctx1)
{
    (void)routine;
    if (ctx1 == LS_MOVIE_CTX) {
        ls_count(&s_movie_pending, event);
        return;
    }
    if (ctx1 != LS_FADE_CTX)
        return;
#if defined(_MSC_VER) && !defined(__clang__)
    if (event == 1)
        InterlockedIncrement((volatile LONG *)&s_fade_pending);
    else if (event == 2 && s_fade_pending > 0)
        InterlockedDecrement((volatile LONG *)&s_fade_pending);
#else
    if (event == 1)
        __atomic_add_fetch(&s_fade_pending, 1, __ATOMIC_SEQ_CST);
    else if (event == 2 && s_fade_pending > 0)
        __atomic_sub_fetch(&s_fade_pending, 1, __ATOMIC_SEQ_CST);
#endif
}

/* The game's other threads finish what they can before a frame is counted.
 *
 * The user's sessions after 241 and 242 parted the same way three times
 * more, at a level's start after a cutscene: a stretch of frames where the
 * game only waits (no random numbers drawn), 3 frames long on the host and
 * 4 on the other PC, every time; inside it the main thread stood still for
 * 2 s on the host and over 3 s on the other (the host waited for its pads:
 * "waiting for player 2's pad"), a turn of 2 s on one guest thread with
 * another queued for the CPU. Another thread builds something big while
 * the main loop asks each frame whether it is done; how many frames the
 * main loop fits in meanwhile is a matter of each PC's speed and of the
 * scheduler's clock (its quantum, its grace), so one PC saw "done" a frame
 * later. Online, at each frame's start the main thread now lets go of the
 * CPU while any other guest thread is running or queued for it, and takes
 * it back once they have all stopped (waiting on something, or ended):
 * work another thread was given is finished before the next frame on every
 * PC alike. Nothing queued (most frames): nothing done. A thread that runs
 * on and on (one waiting by polling) is let be after 15 s, logged, and not
 * waited for again for 600 frames.
 *
 * Off since: the user's next session (243) waited 60-440 ms a frame in
 * the shop and the menus before the mission (a thread of theirs runs
 * there all along), unplayable, and the level's start parted all the same
 * (no wait was logged inside the stretch: nothing was queued at those
 * frames' starts). RDR_LS_SETTLE=1 (tests) turns it on. */
extern int xbox_GuestCpuOthersBusy(unsigned *acquires);
extern int xbox_GuestCpuWaiting(void);
static uint32_t s_settle_off_until;
static unsigned s_settles, s_settle_long;

static void ls_settle_threads(void)
{
    extern uint64_t rdr_net_now_ms(void);
    extern void rdr_net_sleep_ms(int ms);
    extern int xbox_GuestCpuRelease(int kind, uint32_t where);
    extern void xbox_GuestCpuAcquire(void);
    static int off = -1;
    uint64_t t0, now;
    unsigned acq = 0, last;
    int quiet = 0, rel, busy;
    if (off < 0)
        off = !(getenv("RDR_LS_SETTLE") && atoi(getenv("RDR_LS_SETTLE")) > 0);
    if (off || (s_mode != 4 && !s_hash))
        return;
    if (s_lsf < s_settle_off_until || xbox_GuestCpuWaiting() <= 0)
        return;
    t0 = rdr_net_now_ms();
    rel = xbox_GuestCpuRelease(0, 238);         /* handed over, as NtYieldExecution */
    if (!rel)
        return;
    xbox_GuestCpuOthersBusy(&last);
    for (;;) {
        rdr_net_sleep_ms(1);
        busy = xbox_GuestCpuOthersBusy(&acq);
        now = rdr_net_now_ms();
        /* still: nobody on the CPU or queued for it, twice running, and
         * nobody took it in between (a kernel call's quick return) */
        if (!busy && acq == last) {
            if (++quiet >= 2)
                break;
        } else {
            quiet = 0;
        }
        last = acq;
        /* a thread that wakes again and again on its own clock (not
         * working: waiting) is not waited for past 4 ms */
        if (!busy && now - t0 >= 4)
            break;
        if (now - t0 >= 15000) {
            s_settle_off_until = s_lsf + 600;
            fprintf(stderr, "[LS] frame %u: another thread of the game ran on for 15 s; not waited for again "
                    "for 600 frames\n", s_lsf);
            fflush(stderr);
            break;
        }
    }
    xbox_GuestCpuAcquire();
    s_settles++;
    if (now - t0 >= 50 && s_settle_long++ < 40) {
        fprintf(stderr, "[LS] frame %u: the game's other threads finished first (%u ms)\n", s_lsf,
                (unsigned)(now - t0));
        fflush(stderr);
    }
}

/* What changes while the game waits (tests and logs).
 *
 * Every parting since 240 came at a level's start: after the frame with
 * the level's first random numbers, a few frames with none (the game
 * waiting for something), 3 on the host and 4 on the other PC each time.
 * What it waits for is not known. When a frame draws no random number
 * after one that drew 15 or more (the level's first drew 22), the game's
 * data (0x00380000-0x00700000) and heap (0x80000000-0x84000000) are
 * copied at each frame's start, up to and including the first frame that
 * draws again (10 frames at most, 3 times a session), and the words that
 * changed in one waiting frame alone are logged (address, old, new). On two PCs' logs the word that changes a
 * frame later on one is what the game waited for. RDR_LS_WATCH=0: off. */
#define LSW_N 3
static const struct { uint32_t lo, hi; } s_watch_rng[] = {
    { 0x00380000u, 0x00700000u }, { 0x80000000u, 0x84000000u }
};
static uint8_t *s_watch_buf;
static int s_watch_on, s_watch_frames, s_watch_count;
static volatile int s_watch_armed;

/* rdr_hooks.c, a movie left out online (a level's start): the next stretch
 * is watched even if the shop's used the watches up */
void rdr_ls_watch_arm(void)
{
    s_watch_armed = 1;
}

static uint32_t rand_count(uint32_t f)
{
    return s_rand[f % LS_DETAIL_N].f == f ? s_rand[f % LS_DETAIL_N].n : 0;
}

static size_t watch_size(void)
{
    size_t n = 0, i;
    for (i = 0; i < sizeof s_watch_rng / sizeof s_watch_rng[0]; i++)
        n += s_watch_rng[i].hi - s_watch_rng[i].lo;
    return n;
}

static void watch_take(void)
{
    size_t i, o = 0;
    for (i = 0; i < sizeof s_watch_rng / sizeof s_watch_rng[0]; i++) {
        size_t n = s_watch_rng[i].hi - s_watch_rng[i].lo;
        memcpy(s_watch_buf + o, gm(s_watch_rng[i].lo), n);
        o += n;
    }
}

/* Each frame's changes are counted per word; a word's first change is
 * kept (where, which frame, old, new). At the end only the words that
 * changed in one waiting frame alone are shown: the screen's buffers and
 * the like change every frame and are left out. */
#define LSW_FIRSTS 200000
static uint8_t *s_watch_cnt;
static struct { uint32_t va, was, now, f; } *s_watch_first;
static unsigned s_watch_nfirst;

static void watch_diff(uint32_t f, int waiting)
{
    size_t i, o = 0;
    unsigned total = 0;
    for (i = 0; i < sizeof s_watch_rng / sizeof s_watch_rng[0]; i++) {
        uint32_t lo = s_watch_rng[i].lo, n = s_watch_rng[i].hi - lo, a;
        const uint8_t *cur = gm(lo);
        for (a = 0; a < n; a += 4) {
            uint32_t was, now;
            size_t w = (o + a) / 4;
            memcpy(&was, s_watch_buf + o + a, 4);
            memcpy(&now, cur + a, 4);
            if (was == now)
                continue;
            total++;
            if (!waiting)
                continue;
            if (s_watch_cnt[w] == 0 && s_watch_nfirst < LSW_FIRSTS) {
                s_watch_first[s_watch_nfirst].va = lo + a;
                s_watch_first[s_watch_nfirst].was = was;
                s_watch_first[s_watch_nfirst].now = now;
                s_watch_first[s_watch_nfirst].f = f;
                s_watch_nfirst++;
            }
            if (s_watch_cnt[w] < 255)
                s_watch_cnt[w]++;
        }
        o += n;
    }
    fprintf(stderr, "[LS] watch: frame %u (%u random numbers): %u words changed\n", f, (unsigned)rand_count(f), total);
}

static size_t watch_word(uint32_t va)
{
    return (va >= 0x80000000u ? (size_t)(s_watch_rng[0].hi - s_watch_rng[0].lo) + (va - 0x80000000u)
                              : (size_t)(va - s_watch_rng[0].lo)) / 4;
}

/* the last waiting frame first (what changed just before the game went
 * on), 150 words a frame at most */
static void watch_report(void)
{
    unsigned k, total = 0;
    uint32_t f = 0xFFFFFFFFu;
    for (;;) {
        uint32_t next = 0;
        unsigned shown = 0, n = 0;
        int any = 0;
        for (k = 0; k < s_watch_nfirst; k++)
            if (s_watch_first[k].f < f && s_watch_first[k].f >= next) {
                next = s_watch_first[k].f;
                any = 1;
            }
        if (!any)
            break;
        f = next;
        for (k = 0; k < s_watch_nfirst; k++) {
            if (s_watch_first[k].f != f || s_watch_cnt[watch_word(s_watch_first[k].va)] != 1)
                continue;
            if (!n++)
                fprintf(stderr, "[LS] watch: changed in frame %u alone:\n", f);
            if (shown++ < 150)
                fprintf(stderr, "[LS]   %08X %08X -> %08X\n", s_watch_first[k].va, s_watch_first[k].was,
                        s_watch_first[k].now);
        }
        if (n > 150)
            fprintf(stderr, "[LS]   (and %u more)\n", n - 150);
        total += n;
    }
    fprintf(stderr, "[LS] watch: done (%u words changed in one waiting frame alone)\n", total);
    fflush(stderr);
}

static void ls_watch_frame(void)
{
    static int off = -1;
    uint32_t f = s_lsf;                 /* frame f starts; f - 1 has ended */
    if (off < 0)
        off = getenv("RDR_LS_WATCH") && atoi(getenv("RDR_LS_WATCH")) == 0;
    if (off || f < 3)
        return;
    if (!s_watch_on) {
        static long at = -2;            /* (tests: RDR_LS_WATCH_AT=<frame> watches from there) */
        if (at == -2)
            at = getenv("RDR_LS_WATCH_AT") ? atol(getenv("RDR_LS_WATCH_AT")) : -1;
        if ((s_watch_count >= LSW_N && !s_watch_armed)
            || ((long)f != at && (rand_count(f - 1) != 0 || rand_count(f - 2) < 15)))
            return;
        s_watch_armed = 0;
        if (!s_watch_buf && !(s_watch_buf = (uint8_t *)malloc(watch_size())))
            off = 1;
        if (!off && !(s_watch_cnt = (uint8_t *)calloc(watch_size() / 4, 1)))
            off = 1;
        if (!off && !(s_watch_first = malloc(LSW_FIRSTS * sizeof *s_watch_first)))
            off = 1;
        if (off) {
            free(s_watch_buf); free(s_watch_cnt); free(s_watch_first);
            s_watch_buf = NULL; s_watch_cnt = NULL; s_watch_first = NULL;
            return;
        }
        s_watch_nfirst = 0;
        s_watch_on = 1;
        s_watch_frames = 0;
        s_watch_count++;
        fprintf(stderr, "[LS] watch: frame %u drew no random numbers after %u in frame %u; watching\n", f - 1,
                (unsigned)rand_count(f - 2), f - 2);
        watch_take();
        return;
    }
    {
        int waiting = rand_count(f - 1) == 0;
        watch_diff(f - 1, waiting);
        if (!waiting || ++s_watch_frames >= 10) {
            s_watch_on = 0;
            watch_report();
            free(s_watch_buf); free(s_watch_cnt); free(s_watch_first);
            s_watch_buf = NULL; s_watch_cnt = NULL; s_watch_first = NULL;
            return;
        }
    }
    watch_take();
}

void rdr_ls_frame(uint32_t ret)
{
    t_main_loop = 1;
    {
        extern void (*g_xbox_thread_hook)(int event, uint32_t routine, uint32_t ctx1);
        if (s_mode && g_xbox_thread_hook != ls_thread_hook)
            g_xbox_thread_hook = ls_thread_hook;
    }
    uint32_t f, h = 2166136261u, cnt, k, v;
    static uint32_t last_ret;
    static int was_loading;
    int loading, menu;
    if (!rdr_lockstep_mode())
        return;
    /* where the frame comes from: the main loop (0x00017ABD and its kin),
     * a loading screen's loop (0x00066291, 0x00066509), or the loading
     * screen's own thread (0x0006B7EF, from 0x0006B960: it runs the clock
     * too while the main thread loads) */
    loading = s_loading > 0 || ret == 0x00066291u || ret == 0x00066509u || ret == 0x0006B7EFu
              /* a movie's frames (rdr_hooks.c, sub_000705A0): the main
               * loop's cutscene branch, the logos at the start */
              || ret == 0x00017C85u || ret == 0x00017875u;
    menu = loading && s_prelevel_menu && ret == 0x00069C15u;
    s_prelevel_menu = 0;
    if (menu)
        loading = 0;                    /* a menu before the mission: in step */
    /* The clock in a load. Held, the loading screen's show before a story
     * chapter (sub_00066CA0: Red turning, then the camera closing in)
     * never reached its end, timed by the clock: online the chapter after
     * the shop stood there for good (harness). Its frames now move the
     * clock as any frame does, each machine as many as its load takes;
     * and the first frame in step after the load puts every machine's
     * clock at the same place, half an hour on from where the load began
     * (as a long load would on the console: the game takes a big step
     * after a load anyway). */
    {
        static int in_load;
        static uint64_t load_t0;
        if (loading) {
            if (!in_load) {
                in_load = 1;
                load_t0 = s_vt_frame;
            }
            ls_clock_frame();
        } else if (in_load) {
            uint64_t to = load_t0 + LS_TICKS_PER_SEC * 1800ull;
            in_load = 0;
            if (s_vt_frame < to)
                s_vt_frame = to;
            else if (s_hash)
                fprintf(stderr, "[LS] a load longer than half an hour: the clocks may part\n");
        }
    }
    if (!loading)
        ls_clock_frame();
    if (s_hash && (ret != last_ret || loading != was_loading))
        fprintf(stderr, "[LS] frame %u (game %u): from %08X%s\n", s_lsf, rd32(LS_FRAME), ret,
                loading ? ", loading" : "");
    last_ret = ret;
    was_loading = loading;
    s_press_loop = loading && ret == 0x00068685u;
    if (loading)
        return;                         /* not a lockstep frame */
    s_lsf++;
    {
        /* A screen fade runs on a thread of its own (sub_00105B40, from
         * 0x00106720; [0x00663338] set while it runs) and the main loop
         * goes on meanwhile: how many frames it counted before the fade
         * ended was the machine's speed (the first runs parted ~200 frames
         * after the legal screen). Lockstep: no frame while a fade runs. */
        uint64_t t0 = 0;
        int waited = 0;
        extern uint64_t rdr_net_now_ms(void);
        int seen = 0;
        while (*gm(0x00663338u) || s_fade_pending > 0) {
            if (!t0)
                t0 = rdr_net_now_ms();
            /* the fade's own end is enough: its thread's last steps need
             * not be waited for */
            if (*gm(0x00663338u))
                seen = 1;
            else if (seen) {
                s_fade_pending = 0;
                break;
            }
            if (rdr_net_now_ms() - t0 > 10000) {
                s_fade_pending = 0;
                break;
            }
            extern void rdr_net_sleep_ms(int ms);
            extern int xbox_GuestCpuRelease(int kind, uint32_t where);
            extern void xbox_GuestCpuAcquire(void);
            /* the CPU handed over as NtYieldExecution does (kernel call
             * 238), or the fade's thread never runs */
            int rel = xbox_GuestCpuRelease(0, 238);
            rdr_net_sleep_ms(1);
            if (rel)
                xbox_GuestCpuAcquire();
            waited = 1;
        }
        if (waited && s_hash)
            fprintf(stderr, "[LS] frame %u waited %u ms for a fade%s\n", cur_frame(),
                    (unsigned)(rdr_net_now_ms() - t0), *gm(0x00663338u) ? " (still on: gave up)" : "");
        /* the movie call's thread (LS_MOVIE_CTX): ended before the frame */
        t0 = 0;
        while (s_movie_pending > 0) {
            extern void rdr_net_sleep_ms(int ms);
            extern int xbox_GuestCpuRelease(int kind, uint32_t where);
            extern void xbox_GuestCpuAcquire(void);
            int rel;
            if (!t0)
                t0 = rdr_net_now_ms();
            if (rdr_net_now_ms() - t0 > 10000) {
                fprintf(stderr, "[LS] frame %u: the movie call's thread still running after 10 s; went on\n",
                        cur_frame());
                s_movie_pending = 0;
                break;
            }
            rel = xbox_GuestCpuRelease(0, 238);
            rdr_net_sleep_ms(1);
            if (rel)
                xbox_GuestCpuAcquire();
        }
        if (t0) {
            fprintf(stderr, "[LS] frame %u waited %u ms for the movie call's thread\n", cur_frame(),
                    (unsigned)(rdr_net_now_ms() - t0));
            fflush(stderr);
        }
    }
    ls_settle_threads();
    if (s_mode == 4)
        ls_watch_frame();
    if (!s_hash && s_mode != 4)
        return;
    f = cur_frame();
    if (menu) {
        h = fnv(h, &f, 4);
        if (s_hash)
            fprintf(stderr, "[LS] frame %u hash %08X (a menu before the mission)\n", f, h);
        if (s_mode == 4)
            ls_net_frame(f, h);
        return;
    }
    h = fnv(h, &f, 4);
    v = rd32(0x00652A70u); h = fnv(h, &v, 4);          /* the game's random numbers */
    /* not the second one (0x0040C094): the screen fades' thread draws its
     * grain from it, as many times as the fade had frames at the host's
     * pace -- a picture's noise, not the game's */
    cnt = rd32(0x0067592Cu);
    h = fnv(h, &cnt, 4);
    {
        float pos[3] = {0, 0, 0}, hp = 0;
        uint32_t c0 = 0;
        for (k = 0; k < cnt && k < 8; k++) {
            uint32_t c = rd32(0x00675930u + k * 4u), hh;
            if (!in_heap(c))
                continue;
            pos[0] = rdf(c + 0x4C); pos[1] = rdf(c + 0x50); pos[2] = rdf(c + 0x54);
            h = fnv(h, pos, sizeof pos);
            if (in_heap(hh = rd32(c + 0x18))) {
                hp = rdf(hh + 0x10);
                h = fnv(h, &hp, 4);
            }
            if (!k)
                c0 = c;
        }
        {
            LsDetail *d = &s_detail[f % LS_DETAIL_N];
            uint32_t j;
            memset(d, 0, sizeof *d);
            d->f = f;
            d->hash = h;
            d->game_frame = rd32(LS_FRAME);
            d->clock = s_vt_frame;
            d->rng = rd32(0x00652A70u);
            d->rng2 = rd32(0x0040C094u);
            d->cnt = cnt;
            for (j = 0; j < cnt && j < 4; j++) {
                uint32_t c = rd32(0x00675930u + j * 4u), hh;
                if (!in_heap(c))
                    continue;
                d->pos[j][0] = rdf(c + 0x4C); d->pos[j][1] = rdf(c + 0x50); d->pos[j][2] = rdf(c + 0x54);
                if (in_heap(hh = rd32(c + 0x18)))
                    d->hp[j] = rdf(hh + 0x10);
            }
            s_last_frame_done = f;
        }
        if (s_hash)
            fprintf(stderr, "[LS] frame %u hash %08X p0 %.3f %.3f %.3f rng %08X %08X\n", f, h,
                    c0 ? rdf(c0 + 0x4C) : 0.0f, c0 ? rdf(c0 + 0x50) : 0.0f, c0 ? rdf(c0 + 0x54) : 0.0f,
                    rd32(0x00652A70u), rd32(0x0040C094u));
    }
    if (s_mode == 4)
        ls_net_frame(f, h);
}
