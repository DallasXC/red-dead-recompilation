/*
 * rdr_net_host.c - online play, the host's side of "option 2" (streaming).
 *
 * RDR_NET_HOST=1 (or a port number) opens the game to guests. Each guest
 * that says hello gets the lowest pad port nobody uses (no pad of the
 * host's open on it, no other guest), and from then on is a pad there:
 * the XDK pad calls (rdr_hooks.c) report it arriving, give it a virtual
 * handle and read its latest state. The finished picture (the renderer's
 * present tap) is read back, scaled to the stream's size, made a JPEG on
 * a worker thread and sent to every guest in datagrams; the mixer's
 * sound (the audio tap) is sent as it is, 16-bit stereo at 48 kHz.
 *
 *   RDR_NET_HOST=1|<port>   host (default port 27031)
 *   RDR_NET_FPS=30          pictures a second (1-60)
 *   RDR_NET_SIZE=960x540    the picture's size
 *   RDR_NET_QUALITY=70      JPEG quality (20-95)
 */
#include "rdr_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../../third_party/stb/stb_image_write.h"

#include "../ui/rdr_gl.h"

extern void *xbox_GetMemoryOffset(void);
extern void (*xbox_present_tap)(int w, int h);
extern void (*xbox_audio_tap)(const int16_t *pcm, int frames, int rate);

typedef struct {
    int used;
    RdrAddr addr;
    char name[33];
    int port;                       /* the pad port it has */
    uint8_t st[22];
    uint64_t last_ms;
    int announced;                  /* its arrival reported to the game */
} Guest;

static int s_on = -1, s_sock = -1;
static Guest s_g[RDR_NET_MAX_GUESTS];
static uint32_t s_ins, s_rem;       /* pad changes not yet reported */
static int s_fps = 30, s_w = 960, s_h = 540, s_q = 70;

/* ── the lock and the worker ──────────────────────────────────── */
#if defined(_WIN32)
static CRITICAL_SECTION s_lock;
static HANDLE s_wake;
static void lk(void) { EnterCriticalSection(&s_lock); }
static void ulk(void) { LeaveCriticalSection(&s_lock); }
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wake = PTHREAD_COND_INITIALIZER;
static void lk(void) { pthread_mutex_lock(&s_lock); }
static void ulk(void) { pthread_mutex_unlock(&s_lock); }
#endif

/* split screen: each guest's part of the picture (its port, or -1 when
 * the screen is not split -- everyone gets the whole) */
static float s_parts[RDR_NET_MAX_GUESTS][4];
static int s_ports[RDR_NET_MAX_GUESTS];
extern int rdr_coop_view_frac(int k, float r[4]);
static uint8_t *s_raw;              /* the last picture read back, BGRA bottom-up */
static int s_raw_w, s_raw_h, s_raw_ready;
static uint32_t s_frame_id;

static void send_all(const void *p, int n)
{
    int i;
    for (i = 0; i < RDR_NET_MAX_GUESTS; i++)
        if (s_g[i].used)
            rdr_sock_send(s_sock, &s_g[i].addr, p, n);
}

typedef struct { uint8_t *buf; int len, cap; } Mem;
static void mem_put(void *ctx, void *data, int size)
{
    Mem *m = (Mem *)ctx;
    if (m->len + size > m->cap) {
        uint8_t *nb = (uint8_t *)realloc(m->buf, (size_t)(m->len + size) * 2);
        if (!nb) return;                /* this picture comes out short; the next one is whole */
        m->buf = nb;
        m->cap = (m->len + size) * 2;
    }
    memcpy(m->buf + m->len, data, (size_t)size);
    m->len += size;
}

/* One picture: the part (fractions from the top left) of the window read
 * back, scaled to the stream's width (its height as the part's shape
 * gives, at most the stream's height), as a JPEG to one guest (to < 0:
 * all). */
static void encode_send(const uint8_t *bgra, int w, int h, const float *part, int to)
{
    static uint8_t *rgb;
    static Mem jpg;
    int x, y, ow = s_w, oh = s_h, x0 = 0, y0 = 0, cw = w, ch = h, i;
    uint32_t id, parts, k;
    uint8_t pkt[16 + RDR_NET_PART];
    if (part) {
        x0 = (int)(part[0] * w);
        y0 = (int)(part[1] * h);
        cw = (int)(part[2] * w);
        ch = (int)(part[3] * h);
        if (x0 + cw > w) cw = w - x0;
        if (y0 + ch > h) ch = h - y0;
        if (cw < 16 || ch < 16)
            return;
        oh = (int)((int64_t)ow * ch / cw) & ~1;
        if (oh > s_h) {
            oh = s_h;
            ow = (int)((int64_t)oh * cw / ch) & ~1;
        }
    }
    if (!rgb)
        rgb = (uint8_t *)malloc((size_t)s_w * s_h * 3);
    /* nearest of a 2x2 average, top-down RGB (the read-back is bottom-up) */
    for (y = 0; y < oh; y++) {
        int ty = y0 + (int)((int64_t)y * ch / oh);
        int sy = h - 1 - ty, sy2 = sy > 0 ? sy - 1 : sy;
        const uint8_t *r0 = bgra + (size_t)sy * w * 4, *r1 = bgra + (size_t)sy2 * w * 4;
        uint8_t *o = rgb + (size_t)y * ow * 3;
        for (x = 0; x < ow; x++) {
            int sx = x0 + (int)((int64_t)x * cw / ow), sx2 = sx + 1 < w ? sx + 1 : sx;
            const uint8_t *a = r0 + sx * 4, *b = r0 + sx2 * 4, *c = r1 + sx * 4, *d = r1 + sx2 * 4;
            o[0] = (uint8_t)((a[2] + b[2] + c[2] + d[2] + 2) >> 2);
            o[1] = (uint8_t)((a[1] + b[1] + c[1] + d[1] + 2) >> 2);
            o[2] = (uint8_t)((a[0] + b[0] + c[0] + d[0] + 2) >> 2);
            o += 3;
        }
    }
    jpg.len = 0;
    if (!stbi_write_jpg_to_func(mem_put, &jpg, ow, oh, 3, rgb, s_q))
        return;
    id = ++s_frame_id;
    parts = (uint32_t)(jpg.len + RDR_NET_PART - 1) / RDR_NET_PART;
    for (k = 0; k < parts; k++) {
        int n = jpg.len - (int)k * RDR_NET_PART;
        uint32_t magic = RDR_NET_MAGIC;
        uint16_t pk = (uint16_t)k, pn = (uint16_t)parts;
        if (n > RDR_NET_PART)
            n = RDR_NET_PART;
        memcpy(pkt, &magic, 4);
        pkt[4] = RDRN_FRAME;
        memcpy(pkt + 5, &id, 4);
        memcpy(pkt + 9, &pk, 2);
        memcpy(pkt + 11, &pn, 2);
        memcpy(pkt + 13, jpg.buf + k * RDR_NET_PART, (size_t)n);
        lk();
        if (to < 0)
            send_all(pkt, 13 + n);
        else
            for (i = 0; i < RDR_NET_MAX_GUESTS; i++)
                if (s_g[i].used && s_g[i].port == to)
                    rdr_sock_send(s_sock, &s_g[i].addr, pkt, 13 + n);
        ulk();
    }
}

#if defined(_WIN32)
static DWORD WINAPI worker(LPVOID arg)
#else
static void *worker(void *arg)
#endif
{
    uint8_t *mine = NULL;
    int cap = 0, ports[RDR_NET_MAX_GUESTS];
    float parts[RDR_NET_MAX_GUESTS][4];
    (void)arg;
    for (;;) {
        int w, h;
        lk();
#if defined(_WIN32)
        while (!s_raw_ready) {
            ulk();
            WaitForSingleObject(s_wake, 100);
            lk();
        }
#else
        while (!s_raw_ready)
            pthread_cond_wait(&s_wake, &s_lock);
#endif
        w = s_raw_w;
        h = s_raw_h;
        if (cap < w * h * 4) {
            uint8_t *nm = (uint8_t *)realloc(mine, (size_t)w * h * 4);
            if (!nm) {                  /* no memory for this picture: skip it */
                s_raw_ready = 0;
                ulk();
                continue;
            }
            mine = nm;
            cap = w * h * 4;
        }
        memcpy(mine, s_raw, (size_t)w * h * 4);
        memcpy(parts, s_parts, sizeof parts);
        memcpy(ports, s_ports, sizeof ports);
        s_raw_ready = 0;
        ulk();
        {
            int i, split = 0;
            for (i = 0; i < RDR_NET_MAX_GUESTS; i++)
                split |= ports[i] >= 0;
            if (!split)
                encode_send(mine, w, h, NULL, -1);
            else
                for (i = 0; i < RDR_NET_MAX_GUESTS; i++)
                    if (ports[i] >= 0)
                        encode_send(mine, w, h, parts[i], ports[i]);
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* ── the taps ─────────────────────────────────────────────────── */

static int guests(void)
{
    int i, n = 0;
    for (i = 0; i < RDR_NET_MAX_GUESTS; i++)
        n += s_g[i].used;
    return n;
}

void rdr_net_host_present(int w, int h)
{
    static uint64_t next;
    static uint8_t *buf;
    static int cap;
    uint64_t now;
    if (s_on != 1 || !guests() || w <= 0 || h <= 0)
        return;
    if (!rdr_gl.ReadPixels && !rdr_gl_load())
        return;                         /* the context is current here */
    now = rdr_net_now_ms();
    if (now < next)
        return;
    next = (now - next > 200 ? now : next) + (uint64_t)(1000 / s_fps);
    if (cap < w * h * 4) {
        uint8_t *nb = (uint8_t *)realloc(buf, (size_t)w * h * 4);
        if (!nb)
            return;                     /* no memory for the picture: the guests miss this one */
        buf = nb;
        cap = w * h * 4;
    }
    rdr_gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    rdr_gl.ReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, buf);
    lk();
    if (!s_raw_ready) {                 /* the worker took the last one */
        static int raw_cap;
        if (raw_cap < w * h * 4) {
            uint8_t *nr = (uint8_t *)realloc(s_raw, (size_t)w * h * 4);
            if (!nr) {
                ulk();
                return;
            }
            s_raw = nr;
            raw_cap = w * h * 4;
        }
        memcpy(s_raw, buf, (size_t)w * h * 4);
        s_raw_w = w;
        s_raw_h = h;
        {
            int i;
            for (i = 0; i < RDR_NET_MAX_GUESTS; i++) {
                s_ports[i] = -1;
                if (s_g[i].used && rdr_coop_view_frac(s_g[i].port, s_parts[i]))
                    s_ports[i] = s_g[i].port;
            }
        }
        s_raw_ready = 1;
#if defined(_WIN32)
        SetEvent(s_wake);
#else
        pthread_cond_signal(&s_wake);
#endif
    }
    ulk();
}

void rdr_net_host_audio(const int16_t *pcm, int frames, int rate)
{
    static uint32_t seq;
    uint8_t pkt[16 + 256 * 4];
    uint32_t magic = RDR_NET_MAGIC, r = (uint32_t)rate;
    uint16_t fr;
    if (s_on != 1 || !guests() || frames <= 0 || frames > 256)
        return;
    fr = (uint16_t)frames;
    memcpy(pkt, &magic, 4);
    pkt[4] = RDRN_AUDIO;
    seq++;
    memcpy(pkt + 5, &seq, 4);
    memcpy(pkt + 9, &r, 4);
    memcpy(pkt + 13, &fr, 2);
    memcpy(pkt + 15, pcm, (size_t)frames * 4);
    lk();
    send_all(pkt, 15 + frames * 4);
    ulk();
}

/* ── each game frame ──────────────────────────────────────────── */

static int port_free(int port)
{
    int i;
    uint32_t h;
    memcpy(&h, (uint8_t *)xbox_GetMemoryOffset() + 0x004041D4u + (uint32_t)port * 0x38u, 4);
    if (h && (h & 0xFFFFFFF0u) != 0xF00D0000u)
        return 0;                       /* a pad of the host's is open there */
    for (i = 0; i < RDR_NET_MAX_GUESTS; i++)
        if (s_g[i].used && s_g[i].port == port)
            return 0;
    return 1;
}

static void start(void)
{
    const char *e = getenv("RDR_NET_HOST"), *v;
    int port = RDR_NET_PORT;
    s_on = 0;
    if (!e || !*e || *e == '0')
        return;
    if (atoi(e) > 1)
        port = atoi(e);
    if ((v = getenv("RDR_NET_FPS")) && atoi(v) >= 1 && atoi(v) <= 60)
        s_fps = atoi(v);
    if ((v = getenv("RDR_NET_SIZE"))) {
        int w, h;
        if (sscanf(v, "%dx%d", &w, &h) == 2 && w >= 160 && h >= 90 && w <= 1920 && h <= 1080) {
            s_w = w & ~1;
            s_h = h & ~1;
        }
    }
    if ((v = getenv("RDR_NET_QUALITY")) && atoi(v) >= 20 && atoi(v) <= 95)
        s_q = atoi(v);
    s_sock = rdr_sock_open((uint16_t)port);
    if (s_sock < 0) {
        fprintf(stderr, "[NET] cannot open UDP port %d\n", port);
        return;
    }
#if defined(_WIN32)
    InitializeCriticalSection(&s_lock);
    s_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, worker, NULL, 0, NULL);
#else
    {
        pthread_t t;
        pthread_create(&t, NULL, worker, NULL);
        pthread_detach(t);
    }
#endif
    xbox_present_tap = rdr_net_host_present;
    xbox_audio_tap = rdr_net_host_audio;
    s_on = 1;
    fprintf(stderr, "[NET] hosting on UDP port %d (%dx%d, %d pictures a second)\n", port, s_w, s_h, s_fps);
    fflush(stderr);
}

static void reply(const RdrAddr *to, uint8_t type, const void *body, int n)
{
    uint8_t pkt[64];
    uint32_t magic = RDR_NET_MAGIC;
    memcpy(pkt, &magic, 4);
    pkt[4] = type;
    if (n)
        memcpy(pkt + 5, body, (size_t)n);
    rdr_sock_send(s_sock, to, pkt, 5 + n);
}

void rdr_net_host_tick(void)
{
    uint8_t buf[2048];
    RdrAddr from;
    int n, i;
    uint64_t now;
    if (s_on < 0)
        start();
    if (s_on != 1)
        return;
    now = rdr_net_now_ms();
    while ((n = rdr_sock_recv(s_sock, &from, buf, sizeof buf)) > 0) {
        uint32_t magic;
        Guest *g = NULL;
        if (n < 5 || (memcpy(&magic, buf, 4), magic != RDR_NET_MAGIC))
            continue;
        for (i = 0; i < RDR_NET_MAX_GUESTS; i++)
            if (s_g[i].used && rdr_addr_same(&s_g[i].addr, &from))
                g = &s_g[i];
        /* a pad from a guest that timed out (a long stall here or there):
         * taken as a hello, so it comes back in */
        if (buf[4] == RDRN_HELLO || (!g && buf[4] == RDRN_PAD)) {
            if (!g) {
                int port;
                for (port = 1; port < 4 && !port_free(port); port++)
                    ;
                for (i = 0; i < RDR_NET_MAX_GUESTS && s_g[i].used; i++)
                    ;
                if (port >= 4 || i >= RDR_NET_MAX_GUESTS) {
                    reply(&from, RDRN_FULL, NULL, 0);
                    continue;
                }
                lk();
                g = &s_g[i];
                memset(g, 0, sizeof *g);
                g->used = 1;
                g->addr = from;
                g->port = port;
                if (buf[4] == RDRN_HELLO)
                    memcpy(g->name, buf + 5, n - 5 < 32 ? (size_t)(n - 5) : 32);
                else
                    strcpy(g->name, "Guest");
                g->name[32] = 0;
                ulk();
                s_ins |= 1u << port;
                s_rem &= ~(1u << port);
                fprintf(stderr, "[NET] %s joined from %u.%u.%u.%u:%u: the pad on port %d\n", g->name,
                        from.ip[0], from.ip[1], from.ip[2], from.ip[3], from.port, port);
                fflush(stderr);
            }
            g->last_ms = now;
            {
                uint8_t w[5];
                uint16_t sw = (uint16_t)s_w, sh = (uint16_t)s_h;
                w[0] = (uint8_t)g->port;
                memcpy(w + 1, &sw, 2);
                memcpy(w + 3, &sh, 2);
                reply(&from, RDRN_WELCOME, w, 5);
            }
        } else if (g && buf[4] == RDRN_PAD && n >= 9 + 22) {
            memcpy(g->st, buf + 9, 22);
            g->last_ms = now;
        } else if (g && buf[4] == RDRN_BYE) {
            g->last_ms = 0;
        }
    }
    for (i = 0; i < RDR_NET_MAX_GUESTS; i++) {
        Guest *g = &s_g[i];
        if (g->used && now - g->last_ms > 10000) {
            fprintf(stderr, "[NET] %s left (port %d)\n", g->name, g->port);
            fflush(stderr);
            s_rem |= 1u << g->port;
            s_ins &= ~(1u << g->port);
            lk();
            g->used = 0;
            ulk();
        }
    }
}

int rdr_net_host_active(void) { if (s_on < 0) start(); return s_on == 1; }

int rdr_net_pad_port_taken(int port)
{
    int i;
    if (s_on != 1)
        return 0;
    for (i = 0; i < RDR_NET_MAX_GUESTS; i++)
        if (s_g[i].used && s_g[i].port == port)
            return 1;
    return 0;
}

int rdr_net_pad_state(int port, uint8_t st[22])
{
    int i;
    for (i = 0; i < RDR_NET_MAX_GUESTS; i++)
        if (s_g[i].used && s_g[i].port == port) {
            memcpy(st, s_g[i].st, 22);
            return 1;
        }
    memset(st, 0, 22);
    return 0;
}

void rdr_net_pad_changes(uint32_t *ins, uint32_t *rem)
{
    *ins |= s_ins;
    *rem |= s_rem;
    *ins &= ~s_rem;
    s_ins = s_rem = 0;
}
