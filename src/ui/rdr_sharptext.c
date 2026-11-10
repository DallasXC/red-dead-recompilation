/*
 * rdr_sharptext.c - the game's own lettering, sharp on big screens.
 *
 * Every word the game draws -- its menus, the subtitles, the money and
 * ammunition counts -- comes from bitmap fonts: pages of 128 x 128 texels
 * whose letters are some 20 texels tall, stretched to fill a 1440- or
 * 2160-line picture. When the game puts one of those pages on the GPU, the
 * renderer asks (nvgl_set_texture_hook) and gets it back four times bigger,
 * redrawn with clean edges (rdr_art_upscale: the coverage enlarged, its blur
 * turned back into an edge): the same letters, the same layout, sharp.
 *
 * A page is known by its coverage: the game's own fonts are read from the
 * player's swat.dat and front.dat at the first texture of a page's size,
 * and an uploaded texture whose transparent and opaque texels fall exactly
 * as one of their pages' do is that page. The button pictures drawn among
 * the letters (A, B, X, Y) are only enlarged. Settings: sharp_text
 * (Graphics); RDR_SHARP_TEXT=0 switches it off for a run.
 */
#include "rdr_gameart.h"
#include "rdr_settings.h"
#include "rdr_input.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t *(*nvgl_texture_fn)(uint64_t hash, const uint32_t *bgra, uint32_t w, uint32_t h,
                                     uint32_t *rw, uint32_t *rh);
void nvgl_set_texture_hook(nvgl_texture_fn fn);
const char *rdr_ui_game_dir(void);          /* rdr_ui_game.cpp */
extern const unsigned char rdr_font_berylium[];   /* assets/fonts/Berylium.ttf */

#define SIG_N 256                           /* texels sampled for a quick look */

typedef struct {
    int w, h;
    unsigned char *cover;                   /* w*h: alpha > 0 */
    unsigned char *keep;                    /* w*h: a button picture (or NULL) */
    uint8_t sig[SIG_N / 8];
    char name[64];
    RdrArtFont *glyphs;                     /* berrylium's: its letters redrawn from outlines */
    int index;                              /* the page's number in its font */
    struct { int x, y, w, h; } btn[8];      /* 0xB8..0xBF on this page (w 0: not here) */
    char ps_font[40];                       /* the font with the PS2 pictures ("berrylium" for berryliumx) */
} Page;

/* The PS2 version's button pictures (cross, circle, square, triangle ...):
 * berrylium and damaged carry them where berryliumx and damagedx carry
 * Xbox's (the game asks for "<name>x", "%sx" at 0x003CAC64). Kept as RGBA
 * crops, by font and letter (0xB8..0xBF). */
typedef struct { char font[40]; int w[8], h[8]; unsigned char *rgba[8]; } PsGlyphs;
static PsGlyphs s_ps[8];
static int s_nps;
int rdr_buttons_style(void);                /* rdr_cheats.c: 2 PlayStation */

static Page *s_pages;
static int s_npages, s_cap;
static int s_state;                         /* 0 not read, 1 read, -1 nothing to read */
static int s_k = 4;
static unsigned s_hits, s_ps_said;
static int s_sharp = 1;                    /* sharp_text: pages enlarged (else only the buttons swapped) */

/* hash -> page + 1, or 0 for "not a font page" (known misses: -1) */
#define CACHE_N 4096
static struct { uint64_t hash; int v; } s_cache[CACHE_N];

static void signature(const unsigned char *cover_or_alpha, int w, int h, int stride, int is_alpha, uint8_t *sig)
{
    memset(sig, 0, SIG_N / 8);
    for (int i = 0; i < SIG_N; i++) {
        /* a fixed spread of texels over the page */
        uint32_t x = (uint32_t)((i * 2654435761u) >> 7) % (uint32_t)w, y = (uint32_t)((i * 40503u + 17u) % (uint32_t)h);
        size_t o = (size_t)y * w + x;
        int on = is_alpha ? cover_or_alpha[o * stride + 3] != 0 : cover_or_alpha[o] != 0;
        if (on) sig[i / 8] |= (uint8_t)(1u << (i & 7));
    }
}

typedef struct { char seen[256][40]; int n; } Seen;

static void add_font(const char *file, void *user)
{
    Seen *seen = (Seen *)user;
    char base[40];
    size_t n = strlen(file) - strlen(".fonttex");
    if (n == 0 || n >= sizeof base) return;
    memcpy(base, file, n);
    base[n] = 0;
    for (int i = 0; i < seen->n; i++)
        if (!strcmp(seen->seen[i], base)) return;   /* in both archives */
    if (seen->n < 256) snprintf(seen->seen[seen->n++], sizeof seen->seen[0], "%s", base);
    RdrArtFont f;
    if (!rdr_art_font(base, !strcmp(base, "berrylium") ? "%" : NULL, &f))
        return;
    RdrArtFont *glyphs = NULL;
    const char *v = getenv("RDR_VECTOR_TEXT");
    if (!strncmp(base, "berrylium", 9) && !(v && v[0] == '0')) {
        glyphs = (RdrArtFont *)malloc(sizeof *glyphs);
        if (glyphs) {
            *glyphs = f;
            glyphs->pages = NULL;           /* the letters' places only */
            glyphs->npages = 0;
        }
    }
    {
        /* a font with PS2 pictures whose Xbox twin is on the disc */
        char twin[48];
        snprintf(twin, sizeof twin, "%sx.fonttex", base);
        size_t tl = 0;
        unsigned char *t = s_nps < 8 ? rdr_art_read(twin, &tl) : NULL;
        if (t) {
            PsGlyphs *g = &s_ps[s_nps++];
            free(t);
            memset(g, 0, sizeof *g);
            snprintf(g->font, sizeof g->font, "%s", base);
            for (int c = 0; c < 8; c++) {
                const RdrArtGlyph *gl = &f.glyph[0xB8 + c];
                if (!gl->w || gl->page >= f.npages)
                    continue;
                const RdrArtImage *pg = &f.pages[gl->page];
                g->rgba[c] = (unsigned char *)malloc((size_t)gl->w * gl->h * 4);
                if (!g->rgba[c])
                    continue;
                for (int y = 0; y < gl->h; y++)
                    memcpy(g->rgba[c] + (size_t)y * gl->w * 4, pg->rgba + ((size_t)(gl->y + y) * pg->w + gl->x) * 4,
                           (size_t)gl->w * 4);
                g->w[c] = gl->w;
                g->h[c] = gl->h;
            }
        }
    }
    for (int p = 0; p < f.npages; p++) {
        RdrArtImage *im = &f.pages[p];
        if (s_npages == s_cap) {
            int cap = s_cap ? s_cap * 2 : 64;
            Page *np = (Page *)realloc(s_pages, (size_t)cap * sizeof *np);
            if (!np) break;
            s_pages = np;
            s_cap = cap;
        }
        Page *pg = &s_pages[s_npages];
        memset(pg, 0, sizeof *pg);
        pg->w = im->w;
        pg->h = im->h;
        pg->cover = (unsigned char *)malloc((size_t)im->w * im->h);
        if (!pg->cover) break;
        for (size_t i = 0; i < (size_t)im->w * im->h; i++)
            pg->cover[i] = im->rgba[i * 4 + 3] != 0;
        pg->keep = rdr_art_font_keep_mask(&f, p, "\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf");
        signature(pg->cover, pg->w, pg->h, 1, 0, pg->sig);
        snprintf(pg->name, sizeof pg->name, "%s_%02d", base, p);
        pg->glyphs = glyphs;
        pg->index = p;
        for (int c = 0; c < 8; c++) {
            const RdrArtGlyph *gl = &f.glyph[0xB8 + c];
            if (gl->w && gl->page == p) {
                pg->btn[c].x = gl->x; pg->btn[c].y = gl->y; pg->btn[c].w = gl->w; pg->btn[c].h = gl->h;
            }
        }
        if (n > 1 && base[n - 1] == 'x')
            snprintf(pg->ps_font, sizeof pg->ps_font, "%.*s", (int)(n - 1), base);
        s_npages++;
    }
    rdr_art_free_font(&f);
}

static void load(void)
{
    static Seen seen;
    s_state = -1;
    if (!rdr_art_open(rdr_ui_game_dir()))
        return;
    rdr_art_names(".fonttex", add_font, &seen);
    rdr_art_close();
    if (s_npages) {
        s_state = 1;
        fprintf(stderr, "[TEXT] %d font pages from %d fonts: drawn %dx finer\n", s_npages, seen.n, s_k);
    }
}

/* The other lettering: the legal screen, "Loading game", "Press START"
 * and the like are drawn from two font pictures the game makes at the start
 * (a serif in two sizes, every letter of Latin-1 in rows: 256x256 and
 * 512x256), not from its .fonttex pages. Known by their pictures' hashes,
 * the same for every copy of the disc; enlarged the same way. */
static int boot_atlas(uint64_t hash)
{
    static const uint64_t k[] = { 0xab255ded58c79130ull, 0x6012ad6961c73b95ull };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i] == hash) return 1;
    return 0;
}

/* The PS2 pictures over an Xbox font page's (k times bigger): each button
 * letter's box cleared, its PS2 picture drawn in, fitted and centred. */
static int ps_paste(unsigned char *out, int W, int k, const Page *pg)
{
    const PsGlyphs *g = NULL;
    int done = 0;
    if (!pg->ps_font[0])
        return 0;
    for (int i = 0; i < s_nps; i++)
        if (!strcmp(s_ps[i].font, pg->ps_font))
            g = &s_ps[i];
    if (!g)
        return 0;
    for (int c = 0; c < 8; c++) {
        int bw = pg->btn[c].w * k, bh = pg->btn[c].h * k, bx = pg->btn[c].x * k, by = pg->btn[c].y * k;
        if (!bw || !g->rgba[c])
            continue;
        for (int y = 0; y < bh; y++)
            memset(out + ((size_t)(by + y) * W + bx) * 4, 0, (size_t)bw * 4);
        float s = (float)bw / g->w[c] < (float)bh / g->h[c] ? (float)bw / g->w[c] : (float)bh / g->h[c];
        int dw = (int)(g->w[c] * s), dh = (int)(g->h[c] * s), ox = bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                /* bilinear from the crop (RGBA) into the page (BGRA, as uploaded) */
                float fx = (x + 0.5f) / s - 0.5f, fy = (y + 0.5f) / s - 0.5f;
                int x0 = (int)fx, y0 = (int)fy;
                if (fx < 0) { fx = 0; x0 = 0; }
                if (fy < 0) { fy = 0; y0 = 0; }
                int x1 = x0 + 1 < g->w[c] ? x0 + 1 : x0, y1 = y0 + 1 < g->h[c] ? y0 + 1 : y0;
                float ax = fx - x0, ay = fy - y0;
                const unsigned char *p00 = g->rgba[c] + ((size_t)y0 * g->w[c] + x0) * 4,
                                    *p10 = g->rgba[c] + ((size_t)y0 * g->w[c] + x1) * 4,
                                    *p01 = g->rgba[c] + ((size_t)y1 * g->w[c] + x0) * 4,
                                    *p11 = g->rgba[c] + ((size_t)y1 * g->w[c] + x1) * 4;
                unsigned char *o = out + ((size_t)(oy + y) * W + ox + x) * 4;
                float v[4];
                for (int ch = 0; ch < 4; ch++)
                    v[ch] = (p00[ch] * (1 - ax) + p10[ch] * ax) * (1 - ay) + (p01[ch] * (1 - ax) + p11[ch] * ax) * ay;
                o[0] = (unsigned char)v[2]; o[1] = (unsigned char)v[1]; o[2] = (unsigned char)v[0];
                o[3] = (unsigned char)v[3];
            }
        done++;
    }
    return done;
}

/* Keys and mouse: each button letter's box a key cap with the key's name
 * (A Enter and B Esc, as in the menus; X and Y their keys). */
static int kb_paste(unsigned char *out, int W, int k, const Page *pg)
{
    int done = 0;
    for (int c = 0; c < 8; c++) {
        const char *label = NULL;
        int ch = 0xB8 + c;
        if (!pg->btn[c].w)
            continue;
        if (ch == 0xBA) label = "Enter";
        else if (ch == 0xBE) label = "Esc";
        else if (ch == 0xBD) label = rdr_key_name(g_rdr_settings.keys[RDR_KEY_X]);
        else if (ch == 0xBC) label = rdr_key_name(g_rdr_settings.keys[RDR_KEY_Y]);
        if (!label)
            continue;
        rdr_art_keycap(out, W, pg->btn[c].x * k, pg->btn[c].y * k, pg->btn[c].w * k, pg->btn[c].h * k, label,
                       rdr_font_berylium);
        done++;
    }
    return done;
}

/* The shop's and the weapon screen's button pictures (and the other
 * screens drawn from the same Flash art): A, B, X and Y as 32 x 32
 * pictures of their own, not font letters -- known by their hashes (user:
 * "It would be nice if the options in the Shop and Weapon selected were
 * translated as well"; found in a texture dump of the weapon screen).
 * With a PlayStation controller each becomes a dark button with the PS2
 * version's mark from berrylium (A cross, B circle, X square, Y
 * triangle, as the PS2 version maps them); on keys and mouse a key cap
 * with its key, as the game's prompts show. Drawn 4 times bigger. */
static int shop_button(uint64_t hash)
{
    static const uint64_t k[4] = { 0xae7507efc47a3202ull,    /* A */
                                   0x3c4ed18d671845e1ull,    /* B */
                                   0x80619f645f353124ull,    /* X */
                                   0x62810b34cf1c21e2ull };  /* Y */
    for (int i = 0; i < 4; i++)
        if (k[i] == hash) return i;
    return -1;
}

static uint32_t *shop_button_swap(int b, uint32_t w, uint32_t h, uint32_t *rw, uint32_t *rh)
{
    static const int ps_letter[4] = { 2, 6, 5, 4 };     /* 0xBA, 0xBE, 0xBD, 0xBC */
    int style = rdr_buttons_style(), K = 4, W = (int)w * K, H = (int)h * K;
    const PsGlyphs *g = NULL;
    unsigned char *out;
    if (style != 2 && style != 3)
        return NULL;
    if (style == 2) {
        if (s_state == 0) load();
        for (int i = 0; i < s_nps; i++)
            if (!strcmp(s_ps[i].font, "berrylium"))
                g = &s_ps[i];
        if (!g && s_nps)
            g = &s_ps[0];
        if (!g || !g->rgba[ps_letter[b]])
            return NULL;
    }
    out = (unsigned char *)calloc((size_t)W * H, 4);
    if (!out)
        return NULL;
    if (style == 3) {
        const char *label = b == 0 ? "Enter" : b == 1 ? "Esc"
                          : rdr_key_name(g_rdr_settings.keys[b == 2 ? RDR_KEY_X : RDR_KEY_Y]);
        int m = W / 16;
        rdr_art_keycap(out, W, m, H / 6, W - 2 * m, H - H / 3, label, rdr_font_berylium);
    } else {
        /* a dark round button, lit from the top left, as the game's own */
        float cx = W * 0.5f, cy = H * 0.5f, R = W * 0.46f;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
                float cov = d < R - 1.0f ? 1.0f : (d > R + 1.0f ? 0.0f : (R + 1.0f - d) * 0.5f);
                if (cov <= 0.0f) continue;
                float lx = (x + 0.5f - (cx - R * 0.35f)) / R, ly = (y + 0.5f - (cy - R * 0.4f)) / R;
                float lit = 1.0f - sqrtf(lx * lx + ly * ly) * 0.6f;
                if (lit < 0.25f) lit = 0.25f;
                float v = 30.0f + 55.0f * lit;
                if (d > R * 0.9f) v *= 0.6f;                       /* the rim */
                unsigned char *o = out + ((size_t)y * W + x) * 4;
                o[0] = (unsigned char)(v + 4); o[1] = (unsigned char)v; o[2] = (unsigned char)v;
                o[3] = (unsigned char)(255.0f * cov);
            }
        /* the mark, fitted in the middle */
        {
            int c = ps_letter[b], gw = g->w[c], gh = g->h[c];
            /* the mark's ink only (the letter's box has room around it) */
            int ix0 = gw, iy0 = gh, ix1 = -1, iy1 = -1;
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++)
                    if (g->rgba[c][((size_t)y * gw + x) * 4 + 3] > 32) {
                        if (x < ix0) ix0 = x;
                        if (x > ix1) ix1 = x;
                        if (y < iy0) iy0 = y;
                        if (y > iy1) iy1 = y;
                    }
            if (ix1 < 0) { ix0 = iy0 = 0; ix1 = gw - 1; iy1 = gh - 1; }
            int iw = ix1 - ix0 + 1, ih = iy1 - iy0 + 1;
            float box = R * 1.2f, sc = box / (iw > ih ? iw : ih);
            int dw = (int)(iw * sc), dh = (int)(ih * sc), ox = (int)(cx - dw * 0.5f), oy = (int)(cy - dh * 0.5f);
            for (int y = 0; y < dh; y++)
                for (int x = 0; x < dw; x++) {
                    float fx = (x + 0.5f) / sc - 0.5f + ix0, fy = (y + 0.5f) / sc - 0.5f + iy0;
                    if (fx < 0) fx = 0;
                    if (fy < 0) fy = 0;
                    int x0 = (int)fx, y0 = (int)fy;
                    int x1 = x0 + 1 < gw ? x0 + 1 : x0, y1 = y0 + 1 < gh ? y0 + 1 : y0;
                    float ax = fx - x0, ay = fy - y0, v[4];
                    const unsigned char *p00 = g->rgba[c] + ((size_t)y0 * gw + x0) * 4,
                                        *p10 = g->rgba[c] + ((size_t)y0 * gw + x1) * 4,
                                        *p01 = g->rgba[c] + ((size_t)y1 * gw + x0) * 4,
                                        *p11 = g->rgba[c] + ((size_t)y1 * gw + x1) * 4;
                    for (int ch = 0; ch < 4; ch++)
                        v[ch] = (p00[ch] * (1 - ax) + p10[ch] * ax) * (1 - ay) + (p01[ch] * (1 - ax) + p11[ch] * ax) * ay;
                    if (ox + x < 0 || ox + x >= W || oy + y < 0 || oy + y >= H) continue;
                    unsigned char *o = out + ((size_t)(oy + y) * W + ox + x) * 4;
                    float a = v[3] / 255.0f;                       /* over the button (RGBA -> BGRA) */
                    o[0] = (unsigned char)(v[2] * a + o[0] * (1 - a));
                    o[1] = (unsigned char)(v[1] * a + o[1] * (1 - a));
                    o[2] = (unsigned char)(v[0] * a + o[2] * (1 - a));
                }
        }
    }
    {
        static unsigned said;
        if (said++ < 8) {
            fprintf(stderr, "[TEXT] the shop's %c button: %s\n", "ABXY"[b], style == 2 ? "PlayStation" : "a key cap");
            fflush(stderr);
        }
    }
    *rw = (uint32_t)W;
    *rh = (uint32_t)H;
    return (uint32_t *)out;
}

static uint32_t *texture_hook(uint64_t hash, const uint32_t *bgra, uint32_t w, uint32_t h, uint32_t *rw, uint32_t *rh)
{
    if (w == 32 && h == 32) {
        int b = shop_button(hash);
        if (b >= 0)
            return shop_button_swap(b, w, h, rw, rh);
    }
    if (w > 512 || h > 512 || w < 32 || h < 32)
        return NULL;
    if (s_sharp && boot_atlas(hash)) {
        unsigned char *big = rdr_art_upscale((const unsigned char *)bgra, (int)w, (int)h, s_k, NULL);
        if (!big) return NULL;
        fprintf(stderr, "[TEXT] the start's lettering %ux%u -> %ux%u\n", w, h, w * s_k, h * s_k);
        fflush(stderr);
        *rw = w * (uint32_t)s_k;
        *rh = h * (uint32_t)s_k;
        return (uint32_t *)big;
    }
    size_t slot = (size_t)(hash ^ (hash >> 29)) % CACHE_N;
    int page = -1;
    if (s_cache[slot].hash == hash && s_cache[slot].v) {
        if (s_cache[slot].v < 0) return NULL;
        page = s_cache[slot].v - 1;
    } else {
        if (s_state == 0) load();
        if (s_state < 0) return NULL;
        uint8_t sig[SIG_N / 8];
        int have_sig = 0;
        const unsigned char *px = (const unsigned char *)bgra;
        for (int i = 0; i < s_npages && page < 0; i++) {
            Page *pg = &s_pages[i];
            if (pg->w != (int)w || pg->h != (int)h) continue;
            if (!have_sig) { signature(px, (int)w, (int)h, 4, 1, sig); have_sig = 1; }
            if (memcmp(sig, pg->sig, sizeof sig)) continue;
            size_t miss = 0, n = (size_t)w * h;
            for (size_t t = 0; t < n && miss <= n / 400; t++)
                miss += (px[t * 4 + 3] != 0) != pg->cover[t];
            if (miss <= n / 400) page = i;
        }
        {
            static int dbg = -1;
            if (dbg < 0) dbg = getenv("RDR_SHARP_DEBUG") != NULL;
            if (dbg && page < 0) {
                /* the nearest page of this size, by texels that differ */
                const unsigned char *px2 = (const unsigned char *)bgra;
                size_t best = (size_t)-1, n = (size_t)w * h, on = 0;
                int bi = -1;
                for (size_t t = 0; t < n; t++) on += px2[t * 4 + 3] != 0;
                for (int i = 0; i < s_npages; i++) {
                    Page *pg = &s_pages[i];
                    size_t miss = 0;
                    if (pg->w != (int)w || pg->h != (int)h) continue;
                    for (size_t t = 0; t < n; t++) miss += (px2[t * 4 + 3] != 0) != pg->cover[t];
                    if (miss < best) { best = miss; bi = i; }
                }
                static int said;
                if (said++ < 200)
                    fprintf(stderr, "[TEXT] miss %ux%u (%zu on): nearest %s, %zu differ (hash %016llx)\n", w, h, on,
                            bi >= 0 ? s_pages[bi].name : "-", bi >= 0 ? best : 0, (unsigned long long)hash);
            }
        }
        s_cache[slot].hash = hash;
        s_cache[slot].v = page >= 0 ? page + 1 : -1;
        if (page < 0) return NULL;
    }
    int style = rdr_buttons_style();
    int ps = style == 2 && s_pages[page].ps_font[0];
    int kb = style == 3 && (s_pages[page].btn[2].w || s_pages[page].btn[6].w);
    if (!s_sharp && !ps && !kb)
        return NULL;
    int k = s_sharp ? s_k : 1;
    unsigned char *out;
    if (k > 1)
        out = rdr_art_upscale((const unsigned char *)bgra, (int)w, (int)h, k, s_pages[page].keep);
    else if ((out = (unsigned char *)malloc((size_t)w * h * 4)) != NULL)
        memcpy(out, bgra, (size_t)w * h * 4);
    if (!out) return NULL;
    if (ps && ps_paste(out, (int)w * k, k, &s_pages[page]) && s_ps_said++ < 8) {
        fprintf(stderr, "[TEXT] %s: the PS2 version's button pictures\n", s_pages[page].name);
        fflush(stderr);
    }
    if (kb && kb_paste(out, (int)w * k, k, &s_pages[page]) && s_ps_said++ < 8) {
        fprintf(stderr, "[TEXT] %s: key caps for the buttons\n", s_pages[page].name);
        fflush(stderr);
    }
    if (k > 1 && s_pages[page].glyphs)
        rdr_art_vector_letters(out, (int)w * k, (int)h * k, k, s_pages[page].glyphs, s_pages[page].index,
                               rdr_font_berylium, "\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf");
    {
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("RDR_SHARP_DEBUG") != NULL;
        if (dbg && s_hits < 16) {
            /* the page as drawn now, and as it was: TGA files */
            char path[96];
            for (int v = 0; v < 2; v++) {
                const unsigned char *src = v ? out : (const unsigned char *)bgra;
                int W = v ? (int)w * k : (int)w, H = v ? (int)h * k : (int)h;
                snprintf(path, sizeof path, "sharp_%s_%s.tga", s_pages[page].name, v ? "big" : "orig");
                FILE *f = fopen(path, "wb");
                if (!f) continue;
                unsigned char hd[18] = { 0, 0, 2 };
                hd[12] = (unsigned char)W; hd[13] = (unsigned char)(W >> 8);
                hd[14] = (unsigned char)H; hd[15] = (unsigned char)(H >> 8);
                hd[16] = 32; hd[17] = 0x28;
                fwrite(hd, 1, 18, f);
                fwrite(src, 4, (size_t)W * H, f);
                fclose(f);
            }
        }
    }
    if (s_hits++ < 64) {
        fprintf(stderr, "[TEXT] %s: %ux%u -> %ux%u\n", s_pages[page].name, w, h, w * k, h * k);
        fflush(stderr);
    }
    *rw = w * (uint32_t)k;
    *rh = h * (uint32_t)k;
    return (uint32_t *)out;
}

void rdr_sharptext_install(void)
{
    const char *e = getenv("RDR_SHARP_TEXT");
    s_sharp = !(e ? e[0] == '0' : !g_rdr_settings.sharp_text);
    /* (installed either way: the button pictures need it too) */
    nvgl_set_texture_hook(texture_hook);
}
