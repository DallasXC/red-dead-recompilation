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
 *   RDR_LS_PORT=27032  RDR_LS_DELAY=3 (frames of input delay)
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
#define LSN_PAD   22              /* XINPUT_GAMEPAD (18), then a command (rdr_cmd: 4) */
#define LSN_GAMEPAD 18
enum { LSN_HELLO = 1, LSN_WELCOME = 2, LSN_INPUT = 3, LSN_HASH = 4, LSN_GO = 5 };

typedef struct { int used; RdrAddr addr; int port; } LsPeer;
static int s_ns = -1, s_host, s_me, s_players = 1, s_delay = 3, s_go, s_want_slot;
static uint32_t s_session;              /* the lobby's game (RDR_LS_SESSION), 0 none */
static RdrAddr s_host_addr;
static LsPeer s_peer[4];
static uint8_t s_in[LSN_RING][4][LSN_PAD];
static uint32_t s_in_frame[LSN_RING][4];        /* the frame each slot holds (+1; 0 none) */
static uint32_t s_hash_of[LSN_RING], s_hash_f[LSN_RING];     /* ours, and its frame + 1 */
static uint32_t s_their_of[LSN_RING], s_their_f[LSN_RING];   /* theirs come first: kept */
static uint32_t s_pkt_cur[4];


static void lsn_send(const RdrAddr *to, uint8_t type, const void *body, int n)
{
    uint8_t pkt[512];
    uint32_t m = LSN_MAGIC;
    memcpy(pkt, &m, 4);
    pkt[4] = type;
    if (n)
        memcpy(pkt + 5, body, (size_t)n);
    rdr_sock_send(s_ns, to, pkt, 5 + n);
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
static volatile uint32_t s_parted_at = 0xFFFFFFFFu;

static void rdr_ls_desync_notice(uint32_t f) { s_parted_at = f; }

int rdr_ls_status(char *buf, int n)
{
    if (rdr_lockstep_mode() != 4)
        return 0;
    if (s_parted_at != 0xFFFFFFFFu)
        snprintf(buf, (size_t)n, "Online: the games parted at frame %u -- restart to play on", (unsigned)s_parted_at);
    else if (s_wait_state == 2)
        snprintf(buf, (size_t)n, s_host ? "Online: waiting for the other players to join..." : "Online: looking for the host...");
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
    }
    fflush(stderr);
}

static uint32_t s_last_frame_done;

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
    } else if (said++ < 5)
        fprintf(stderr, "[LS] DESYNC at frame %u too\n", f);
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
            }
        }
        break;
    case LSN_WELCOME:
        if (!s_host && n >= 7 && s_me < 0) {
            s_me = b[5];
            s_players = b[6];
            fprintf(stderr, "[LS] joined: player %d of %d\n", s_me + 1, s_players);
        }
        break;
    case LSN_GO:
        s_go = 1;
        break;
    case LSN_INPUT:
        if (n >= 11) {
            int port = b[5], cnt = b[6];
            uint32_t first;
            memcpy(&first, b + 7, 4);
            if (port < 4 && n >= 11 + cnt * LSN_PAD) {
                for (i = 0; i < cnt; i++)
                    lsn_put(port, first + (uint32_t)i, b + 11 + i * LSN_PAD);
                if (s_host)             /* passed on to the other guests */
                    for (i = 1; i < 4; i++)
                        if (s_peer[i].used && s_peer[i].port != port)
                            rdr_sock_send(s_ns, &s_peer[i].addr, b, n);
            }
        }
        break;
    case LSN_HASH:
        if (n >= 13) {
            uint32_t f, h;
            memcpy(&f, b + 5, 4);
            memcpy(&h, b + 9, 4);
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
                        rdr_sock_send(s_ns, &s_peer[i].addr, b, n);
        }
        break;
    }
}

static void lsn_poll(void)
{
    uint8_t buf[1024];
    RdrAddr from;
    int n;
    while ((n = rdr_sock_recv(s_ns, &from, buf, sizeof buf)) > 0)
        lsn_handle(buf, n, &from);
}

static void ls_net_start(const char *host, const char *join)
{
    const char *e;
    int port = (e = getenv("RDR_LS_PORT")) && atoi(e) > 0 ? atoi(e) : 27032;
    if ((e = getenv("RDR_LS_DELAY")) && atoi(e) >= 1 && atoi(e) <= 30)
        s_delay = atoi(e);
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
    fprintf(stderr, "[LS] online lockstep: %s, input delay %d frames%s\n",
            s_host ? "host" : "guest", s_delay, s_ns < 0 ? " -- NO SOCKET" : "");
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

/* The start: everyone in (the host says go), then frame by frame. */
static void ls_net_frame(uint32_t frame, uint32_t hash)
{
    static uint32_t first = 0xFFFFFFFFu;
    uint8_t body[11 + 8 * LSN_PAD];
    uint8_t mine[LSN_PAD];
    uint64_t t0, last_send = 0, last_say;
    int p, k;
    if (s_ns < 0)
        return;
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
                    break;
                }
            } else {
                static uint64_t hello_t;
                if (s_me < 0 && rdr_net_now_ms() - hello_t >= 250) {
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
        for (k = 0; k < s_delay; k++)           /* the first frames: nobody pressed anything */
            for (p = 0; p < s_players; p++)
                lsn_put(p, frame + (uint32_t)k, mine);
        fprintf(stderr, "[LS] everyone in at frame %u: %d players\n", frame, s_players);
    }
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
            uint32_t c = ls_take_cmd();
            memcpy(mine + LSN_GAMEPAD, &c, 4);
        }
        lsn_put(s_me, frame + (uint32_t)s_delay, mine);
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
            uint32_t from = frame + (uint32_t)s_delay >= 7 ? frame + (uint32_t)s_delay - 7 : 0;
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
        for (p = 0; p < s_players; p++)
            all &= s_in_frame[frame % LSN_RING][p] == frame + 1;
        if (all)
            break;
        if (now - t0 >= 500)
            s_wait_state = 1;
        if (now - last_say >= 3000) {
            fprintf(stderr, "[LS] frame %u: waiting for the other players' pads (%u ms)\n", frame,
                    (unsigned)(now - t0));
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
}

void rdr_ls_frame(uint32_t ret)
{
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
    loading = s_loading > 0 || ret == 0x00066291u || ret == 0x00066509u || ret == 0x0006B7EFu;
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
        while (*gm(0x00663338u)) {
            if (!t0)
                t0 = rdr_net_now_ms();
            if (rdr_net_now_ms() - t0 > 10000)
                break;
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
    }
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
