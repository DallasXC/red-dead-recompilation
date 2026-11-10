/*
 * rdr_gameart.c - the game's menu art from the player's disc (see the header).
 */
#include "rdr_gameart.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_truetype, a private copy (Dear ImGui keeps its own static) */
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

/* ---- Raw DEFLATE ------------------------------------------------------
 * A small canonical-Huffman inflater (as zlib's puff.c does it): the
 * archives hold their files as raw DEFLATE streams. */

typedef struct {
    const unsigned char *in, *in_end;
    unsigned bitbuf;
    int bitcnt;
    unsigned char *out;
    size_t pos, len;
    int err;
} Inf;

typedef struct {
    unsigned short count[16];
    unsigned short symbol[320];
} Huff;

static int bit(Inf *s)
{
    if (!s->bitcnt) {
        if (s->in >= s->in_end) { s->err = 1; return 0; }
        s->bitbuf = *s->in++;
        s->bitcnt = 8;
    }
    int b = (int)(s->bitbuf & 1);
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}

static unsigned bits(Inf *s, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++)
        v |= (unsigned)bit(s) << i;
    return v;
}

static void build(Huff *h, const unsigned char *lens, int n)
{
    unsigned short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++)
        h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int l = 1; l < 15; l++)
        offs[l + 1] = (unsigned short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++)
        if (lens[i])
            h->symbol[offs[lens[i]]++] = (unsigned short)i;
}

static int decode(Inf *s, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= bit(s);
        int count = h->count[l];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static const unsigned short kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
                                             59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                             5, 5, 5, 5, 0 };
static const unsigned short kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                              513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
                                              10, 11, 11, 12, 12, 13, 13 };

static int codes(Inf *s, const Huff *lit, const Huff *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (s->err) return 0;
        if (sym < 256) {
            if (s->pos >= s->len) return 0;
            s->out[s->pos++] = (unsigned char)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            sym -= 257;
            if (sym >= 29) return 0;
            size_t n = kLenBase[sym] + bits(s, kLenExtra[sym]);
            int ds = decode(s, dist);
            if (s->err || ds < 0 || ds >= 30) return 0;
            size_t d = kDistBase[ds] + bits(s, kDistExtra[ds]);
            if (d > s->pos || s->pos + n > s->len) return 0;
            for (size_t i = 0; i < n; i++, s->pos++)
                s->out[s->pos] = s->out[s->pos - d];
        }
    }
}

int rdr_inflate(const unsigned char *in, size_t in_len, unsigned char *out, size_t out_len)
{
    Inf s;
    memset(&s, 0, sizeof s);
    s.in = in; s.in_end = in + in_len; s.out = out; s.len = out_len;
    static Huff *fl, *fd;                       /* the fixed codes, built once */
    static Huff fixed_lit, fixed_dist;
    int last;
    do {
        last = bit(&s);
        int type = (int)bits(&s, 2);
        if (s.err) return 0;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;           /* to the byte boundary */
            if (s.in + 4 > s.in_end) return 0;
            unsigned n = s.in[0] | (s.in[1] << 8), nn = s.in[2] | (s.in[3] << 8);
            s.in += 4;
            if ((n ^ 0xFFFF) != nn || s.in + n > s.in_end || s.pos + n > s.len) return 0;
            memcpy(s.out + s.pos, s.in, n);
            s.in += n; s.pos += n;
        } else if (type == 1) {
            if (!fl) {
                unsigned char l[288];
                int i;
                for (i = 0; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                build(&fixed_lit, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                build(&fixed_dist, l, 30);
                fl = &fixed_lit; fd = &fixed_dist;
            }
            if (!codes(&s, fl, fd)) return 0;
        } else if (type == 2) {
            static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            unsigned char l[320];
            Huff lc, lit, dist;
            int nlen = (int)bits(&s, 5) + 257, ndist = (int)bits(&s, 5) + 1, ncode = (int)bits(&s, 4) + 4;
            if (nlen > 286 || ndist > 30) return 0;
            memset(l, 0, sizeof l);
            for (int i = 0; i < ncode; i++) l[order[i]] = (unsigned char)bits(&s, 3);
            build(&lc, l, 19);
            int i = 0;
            while (i < nlen + ndist) {
                int sym = decode(&s, &lc);
                if (s.err || sym < 0) return 0;
                if (sym < 16) { l[i++] = (unsigned char)sym; continue; }
                int rep, val = 0;
                if (sym == 16) { if (!i) return 0; val = l[i - 1]; rep = 3 + (int)bits(&s, 2); }
                else if (sym == 17) rep = 3 + (int)bits(&s, 3);
                else rep = 11 + (int)bits(&s, 7);
                if (i + rep > nlen + ndist) return 0;
                while (rep--) l[i++] = (unsigned char)val;
            }
            build(&lit, l, nlen);
            build(&dist, l + nlen, ndist);
            if (!codes(&s, &lit, &dist)) return 0;
        } else {
            return 0;
        }
    } while (!last);
    return s.pos == out_len;
}

/* ---- Dave archives ---------------------------------------------------- */

typedef struct {
    FILE *f;
    uint32_t count;
    char **names;
    uint32_t *off, *size, *packed;
    char *name_buf;
} Dave;

static Dave s_arc[2];

static const char kAlpha[] = "\0 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~";

static int name_codes(const unsigned char *tab, size_t tab_len, uint32_t off, unsigned char *c, int max)
{
    int n = 0;
    for (;;) {
        if (off + 3 > tab_len) return -1;
        unsigned b0 = tab[off], b1 = tab[off + 1], b2 = tab[off + 2];
        off += 3;
        unsigned q[4] = { b0 & 0x3F, (b0 >> 6) | ((b1 & 0x0F) << 2), (b1 >> 4) | ((b2 & 0x03) << 4), b2 >> 2 };
        for (int k = 0; k < 4; k++) {
            if (n >= max) return -1;
            c[n++] = (unsigned char)q[k];
            if (!q[k]) return n;
        }
    }
}

static void dave_close(Dave *a)
{
    if (a->f) fclose(a->f);
    free(a->names); free(a->off); free(a->size); free(a->packed); free(a->name_buf);
    memset(a, 0, sizeof *a);
}

static int dave_open(Dave *a, const char *path)
{
    unsigned char head[16];
    memset(a, 0, sizeof *a);
    a->f = fopen(path, "rb");
    if (!a->f) return 0;
    if (fread(head, 1, 16, a->f) != 16 || memcmp(head, "Dave", 4)) { dave_close(a); return 0; }
    uint32_t count, dirb, nameb;
    memcpy(&count, head + 4, 4); memcpy(&dirb, head + 8, 4); memcpy(&nameb, head + 12, 4);
    if (count > 200000 || dirb < count * 16 || nameb > (64u << 20)) { dave_close(a); return 0; }
    unsigned char *d = (unsigned char *)malloc((size_t)dirb + nameb);
    if (!d || fseek(a->f, 0x800, SEEK_SET) || fread(d, 1, (size_t)dirb + nameb, a->f) != (size_t)dirb + nameb) {
        free(d); dave_close(a); return 0;
    }
    const unsigned char *tab = d + dirb;
    a->count = count;
    a->names = (char **)calloc(count, sizeof(char *));
    a->off = (uint32_t *)calloc(count, 4);
    a->size = (uint32_t *)calloc(count, 4);
    a->packed = (uint32_t *)calloc(count, 4);
    a->name_buf = (char *)malloc((size_t)count * 64);
    if (!a->names || !a->off || !a->size || !a->packed || !a->name_buf) { free(d); dave_close(a); return 0; }
    const char *prev = "";
    for (uint32_t i = 0; i < count; i++) {
        uint32_t no;
        memcpy(&no, d + i * 16, 4); memcpy(&a->off[i], d + i * 16 + 4, 4);
        memcpy(&a->size[i], d + i * 16 + 8, 4); memcpy(&a->packed[i], d + i * 16 + 12, 4);
        unsigned char c[200];
        int n = name_codes(tab, nameb, no, c, (int)sizeof c);
        char *out = a->name_buf + (size_t)i * 64;
        int k = 0, from = 0;
        if (n > 1 && c[0] >= 56) {            /* shares a prefix with the name before */
            int keep = (c[0] - 56) + 8 * (c[1] & 31);
            for (; k < keep && k < 63 && prev[k]; k++) out[k] = prev[k];
            from = 2;
        }
        for (int j = from; j < n && c[j] && k < 63; j++)
            out[k++] = c[j] < sizeof kAlpha - 1 ? kAlpha[c[j]] : '?';
        out[k] = 0;
        a->names[i] = out;
        prev = out;
    }
    free(d);
    return 1;
}

static unsigned char *dave_read(Dave *a, const char *name, size_t *len)
{
    if (!a->f) return NULL;
    for (uint32_t i = 0; i < a->count; i++) {
        if (strcmp(a->names[i], name)) continue;
        uint32_t size = a->size[i], packed = a->packed[i];
        unsigned char *raw = (unsigned char *)malloc(packed ? packed : 1);
        unsigned char *out = (unsigned char *)malloc(size ? size : 1);
        if (!raw || !out || fseek(a->f, (long)a->off[i], SEEK_SET) || fread(raw, 1, packed, a->f) != packed) {
            free(raw); free(out); return NULL;
        }
        if (packed < size) {
            if (!rdr_inflate(raw, packed, out, size)) { free(raw); free(out); return NULL; }
            free(raw);
        } else {
            free(out);
            out = raw;
        }
        if (len) *len = size;
        return out;
    }
    return NULL;
}

int rdr_art_names(const char *suffix, void (*fn)(const char *name, void *user), void *user)
{
    int n = 0;
    size_t ls = strlen(suffix);
    for (int a = 0; a < 2; a++)
        for (uint32_t i = 0; i < s_arc[a].count; i++) {
            const char *nm = s_arc[a].names[i];
            size_t ln = strlen(nm);
            if (ln > ls && !strcmp(nm + ln - ls, suffix)) {
                fn(nm, user);
                n++;
            }
        }
    return n;
}

int rdr_art_open(const char *game_dir)
{
    char p[1024];
    rdr_art_close();
    snprintf(p, sizeof p, "%s/swat.dat", game_dir);
    int ok = dave_open(&s_arc[0], p);
    snprintf(p, sizeof p, "%s/front.dat", game_dir);
    dave_open(&s_arc[1], p);
    return ok;
}

void rdr_art_close(void)
{
    dave_close(&s_arc[0]);
    dave_close(&s_arc[1]);
}

unsigned char *rdr_art_read(const char *name, size_t *len)
{
    for (int i = 0; i < 2; i++) {
        unsigned char *d = dave_read(&s_arc[i], name, len);
        if (d) return d;
    }
    return NULL;
}

/* ---- Textures --------------------------------------------------------- */

static void rgb565(unsigned v, unsigned char *c)
{
    c[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
    c[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
    c[2] = (unsigned char)((v & 31) * 255 / 31);
    c[3] = 255;
}

static void dxt_color(const unsigned char *b, unsigned char out[16][4], int four_always)
{
    unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    unsigned char pal[4][4];
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    if (c0 > c1 || four_always) {
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (unsigned char)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (unsigned char)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (unsigned char)((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 16; i++)
        memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
}

static void dxt_decode(const unsigned char *src, size_t n, int w, int h, int kind, unsigned char *rgba)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = kind == 1 ? 8 : 16;
    if ((size_t)bw * bh * bs > n) return;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const unsigned char *b = src + ((size_t)by * bw + bx) * bs;
            unsigned char px[16][4];
            if (kind == 1) {
                dxt_color(b, px, 0);
            } else {
                dxt_color(b + 8, px, 1);
                if (kind == 3) {
                    for (int i = 0; i < 16; i++) {
                        int a = (b[i / 2] >> ((i & 1) * 4)) & 15;
                        px[i][3] = (unsigned char)(a * 17);
                    }
                } else {
                    unsigned a[8];
                    a[0] = b[0]; a[1] = b[1];
                    if (a[0] > a[1]) for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
                    else { for (int k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5; a[6] = 0; a[7] = 255; }
                    uint64_t bitsv = 0;
                    for (int k = 0; k < 6; k++) bitsv |= (uint64_t)b[2 + k] << (8 * k);
                    for (int i = 0; i < 16; i++) px[i][3] = (unsigned char)a[(bitsv >> (3 * i)) & 7];
                }
            }
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + i / 4;
                if (x < w && y < h) memcpy(rgba + ((size_t)y * w + x) * 4, px[i], 4);
            }
        }
}

int rdr_art_image(const char *name, RdrArtImage *out)
{
    char fn[96];
    size_t len = 0;
    memset(out, 0, sizeof *out);
    snprintf(fn, sizeof fn, "%s.xtex", name);
    unsigned char *d = rdr_art_read(fn, &len);
    if (!d) return 0;
    if (len < 14) { free(d); return 0; }
    int w = d[0] | (d[1] << 8), h = d[2] | (d[3] << 8), fmt = d[4] | (d[5] << 8);
    const unsigned char *p = d + 14;
    size_t n = len - 14, px = (size_t)w * h;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { free(d); return 0; }
    unsigned char *rgba = (unsigned char *)calloc(px, 4);
    if (!rgba) { free(d); return 0; }
    int ok = 1;
    switch (fmt) {
    case 0x16: dxt_decode(p, n, w, h, 1, rgba); break;
    case 0x18: dxt_decode(p, n, w, h, 3, rgba); break;
    case 0x1A: dxt_decode(p, n, w, h, 5, rgba); break;
    case 0x0E: case 0x10: {
        /* palette first (BGRA), then linear indices; alpha runs 0..0x80 on
         * the Xbox, so stretch it to the palette's own maximum */
        int npal = fmt == 0x0E ? 256 : 16;
        size_t need = (size_t)npal * 4 + (fmt == 0x0E ? px : (px + 1) / 2);
        if (n < need) { ok = 0; break; }
        unsigned char pal[256][4];
        int amax = 1;
        for (int i = 0; i < npal; i++) {
            pal[i][0] = p[i * 4 + 2]; pal[i][1] = p[i * 4 + 1]; pal[i][2] = p[i * 4]; pal[i][3] = p[i * 4 + 3];
            if (pal[i][3] > amax) amax = pal[i][3];
        }
        for (int i = 0; i < npal; i++) {
            int a = pal[i][3] * 255 / amax;
            pal[i][3] = (unsigned char)(a > 255 ? 255 : a);
        }
        const unsigned char *ix = p + npal * 4;
        for (size_t i = 0; i < px; i++) {
            int c = fmt == 0x0E ? ix[i] : (ix[i >> 1] >> ((i & 1) * 4)) & 15;
            memcpy(rgba + i * 4, pal[c], 4);
        }
        break;
    }
    case 0x11:                                  /* 24-bit BGR, linear */
        if (n < px * 3) { ok = 0; break; }
        for (size_t i = 0; i < px; i++) {
            rgba[i * 4] = p[i * 3 + 2]; rgba[i * 4 + 1] = p[i * 3 + 1]; rgba[i * 4 + 2] = p[i * 3];
            rgba[i * 4 + 3] = 255;
        }
        break;
    default:
        ok = 0;
    }
    free(d);
    if (!ok) { free(rgba); return 0; }
    out->w = w; out->h = h; out->rgba = rgba;
    return 1;
}

void rdr_art_free_image(RdrArtImage *im)
{
    free(im->rgba);
    memset(im, 0, sizeof *im);
}

/* ---- Fonts ------------------------------------------------------------ */

int rdr_art_font(const char *name, const char *skip, RdrArtFont *out)
{
    char fn[96];
    size_t len = 0;
    memset(out, 0, sizeof *out);
    snprintf(fn, sizeof fn, "%s.fonttex", name);
    unsigned char *d = rdr_art_read(fn, &len);
    if (!d) return 0;
    uint32_t hdr[12];
    if (len < 48) { free(d); return 0; }
    memcpy(&out->scale, d, 4);
    memcpy(hdr, d, 48);
    uint32_t n = hdr[11];
    size_t off = 48;
    if (n > 256 || off + n * 10 + 4 > len) { free(d); return 0; }
    out->up = 1;
    out->track = (int)hdr[2];
    out->space = (int)hdr[3];
    struct { int c, x, y, w, h, yo; } rec[256];
    for (uint32_t i = 0; i < n; i++, off += 10) {
        uint32_t c;
        memcpy(&c, d + off, 4);
        rec[i].c = (int)c; rec[i].x = d[off + 4]; rec[i].y = d[off + 5];
        rec[i].w = d[off + 6]; rec[i].h = d[off + 7]; rec[i].yo = (signed char)d[off + 8];
    }
    uint32_t np;
    memcpy(&np, d + off, 4);
    off += 4;
    if (np == 0 || np > 32 || off + np * 4 > len) { free(d); return 0; }
    uint32_t ends[32];
    memcpy(ends, d + off, np * 4);
    free(d);

    out->pages = (RdrArtImage *)calloc(np, sizeof(RdrArtImage));
    if (!out->pages) return 0;
    out->npages = (int)np;
    for (uint32_t p = 0; p < np; p++) {
        char pn[96];
        snprintf(pn, sizeof pn, "%s_%02u", name, p);
        if (!rdr_art_image(pn, &out->pages[p])) { rdr_art_free_font(out); return 0; }
    }
    int pw = out->pages[0].w, ph = out->pages[0].h;
    int page = 0, x = 0, y = 0, rowh = 0;
    for (uint32_t i = 0; i < n; i++) {
        int c = rec[i].c;
        if (c < 0 || c > 255) continue;
        if (rec[i].h > out->line_h) out->line_h = rec[i].h;
        RdrArtGlyph *g = &out->glyph[c];
        if (skip) {
            /* the atlas was packed without these: lay the rest out as the
             * packer did (left to right, rows as tall as their tallest
             * glyph, a new page when a row no longer fits) */
            if (strchr(skip, c)) continue;
            if (x + rec[i].w > pw) { x = 0; y += rowh; rowh = 0; }
            if (y + rec[i].h > ph) { page++; x = y = rowh = 0; }
            g->page = page; g->x = x; g->y = y;
            x += rec[i].w;
            if (rec[i].h > rowh) rowh = rec[i].h;
        } else {
            while (page < (int)np - 1 && i >= ends[page]) page++;
            g->page = page; g->x = rec[i].x; g->y = rec[i].y;
        }
        g->w = rec[i].w; g->h = rec[i].h; g->yoff = rec[i].yo;
    }
    return 1;
}

/* ---- Upscaling -------------------------------------------------------- */

/* Keys' cubic (a = -0.5), as image editors' "bicubic". */
static float cubic_w(float x)
{
    x = x < 0 ? -x : x;
    if (x < 1) return (1.5f * x - 2.5f) * x * x + 1;
    if (x < 2) return ((-0.5f * x + 2.5f) * x - 4) * x + 2;
    return 0;
}

/* in (w x h, 4 floats a pixel) enlarged n times, cubic, separable. */
static float *cubic_up(const float *in, int w, int h, int n)
{
    int W = w * n, H = h * n;
    float *tmp = (float *)malloc((size_t)W * h * 4 * sizeof(float));
    float *out = (float *)malloc((size_t)W * H * 4 * sizeof(float));
    if (!tmp || !out) { free(tmp); free(out); return NULL; }
    for (int X = 0; X < W; X++) {
        float sx = (X + 0.5f) / n - 0.5f;
        int x0 = (int)floorf(sx);
        float wt[4], sum = 0;
        for (int k = 0; k < 4; k++) { wt[k] = cubic_w(sx - (x0 - 1 + k)); sum += wt[k]; }
        for (int y = 0; y < h; y++) {
            float acc[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                int xx = x0 - 1 + k;
                xx = xx < 0 ? 0 : xx >= w ? w - 1 : xx;
                const float *p = in + ((size_t)y * w + xx) * 4;
                for (int c = 0; c < 4; c++) acc[c] += p[c] * wt[k];
            }
            for (int c = 0; c < 4; c++) tmp[((size_t)y * W + X) * 4 + c] = acc[c] / sum;
        }
    }
    for (int Y = 0; Y < H; Y++) {
        float sy = (Y + 0.5f) / n - 0.5f;
        int y0 = (int)floorf(sy);
        float wt[4], sum = 0;
        for (int k = 0; k < 4; k++) { wt[k] = cubic_w(sy - (y0 - 1 + k)); sum += wt[k]; }
        for (int X = 0; X < W; X++) {
            float acc[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                int yy = y0 - 1 + k;
                yy = yy < 0 ? 0 : yy >= h ? h - 1 : yy;
                const float *p = tmp + ((size_t)yy * W + X) * 4;
                for (int c = 0; c < 4; c++) acc[c] += p[c] * wt[k];
            }
            for (int c = 0; c < 4; c++) out[((size_t)Y * W + X) * 4 + c] = acc[c] / sum;
        }
    }
    free(tmp);
    return out;
}

static unsigned char clamp8(float v)
{
    return (unsigned char)(v <= 0 ? 0 : v >= 255 ? 255 : v + 0.5f);
}

unsigned char *rdr_art_upscale(const unsigned char *in, int w, int h, int k, const unsigned char *keep)
{
    int n2 = k * 2, W = w * k, H = h * k;
    float *src = (float *)malloc((size_t)w * h * 4 * sizeof(float));
    if (!src) return NULL;
    /* colour premultiplied, so the transparent texels' colour does not
     * bleed into the edges */
    for (size_t i = 0; i < (size_t)w * h; i++) {
        float a = in[i * 4 + 3] / 255.0f;
        for (int c = 0; c < 3; c++) src[i * 4 + c] = in[i * 4 + c] * a;
        src[i * 4 + 3] = in[i * 4 + 3];
    }
    float *big = cubic_up(src, w, h, n2);
    free(src);
    if (!big) return NULL;
    unsigned char *out = (unsigned char *)malloc((size_t)W * H * 4);
    if (!out) { free(big); return NULL; }
    const int BW = w * n2;
    for (int Y = 0; Y < H; Y++)
        for (int X = 0; X < W; X++) {
            float acc[4] = { 0, 0, 0, 0 };
            int keep_px = keep && keep[(size_t)(Y / k) * w + (X / k)];
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const float *s = big + ((size_t)(Y * 2 + dy) * BW + (X * 2 + dx)) * 4;
                    float a = s[3] / 255.0f;
                    a = a < 0 ? 0 : a > 1 ? 1 : a;
                    float na = a;
                    if (!keep_px) {
                        /* the edge back: smoothstep across 0.35 .. 0.65 */
                        float t = (a - 0.35f) / 0.30f;
                        t = t < 0 ? 0 : t > 1 ? 1 : t;
                        na = t * t * (3 - 2 * t);
                    }
                    /* un-premultiply with the old coverage, weight by the new */
                    for (int c = 0; c < 3; c++) {
                        float col = a > 0.004f ? s[c] / a : 0;
                        acc[c] += col * na;
                    }
                    acc[3] += na;
                }
            unsigned char *o = out + ((size_t)Y * W + X) * 4;
            float A = acc[3] / 4;
            for (int c = 0; c < 3; c++) o[c] = clamp8(acc[3] > 0 ? acc[c] / acc[3] : 255);
            o[3] = clamp8(A * 255);
        }
    free(big);
    return out;
}

unsigned char *rdr_art_font_keep_mask(const RdrArtFont *f, int page, const char *keep)
{
    if (!keep || !*keep || page < 0 || page >= f->npages) return NULL;
    int w = f->pages[page].w / f->up, h = f->pages[page].h / f->up;
    unsigned char *m = (unsigned char *)calloc((size_t)w * h, 1);
    if (!m) return NULL;
    for (const char *q = keep; *q; q++) {
        const RdrArtGlyph *g = &f->glyph[(unsigned char)*q];
        if (!g->w || g->page != page) continue;
        for (int y = g->y; y < g->y + g->h && y < h; y++)
            for (int x = g->x; x < g->x + g->w && x < w; x++)
                m[(size_t)y * w + x] = 1;
    }
    return m;
}

int rdr_art_font_upscale(RdrArtFont *f, int k, const char *keep)
{
    if (k <= 1 || f->up > 1) return 1;
    for (int p = 0; p < f->npages; p++) {
        RdrArtImage *im = &f->pages[p];
        unsigned char *mask = rdr_art_font_keep_mask(f, p, keep);
        unsigned char *out = rdr_art_upscale(im->rgba, im->w, im->h, k, mask);
        free(mask);
        if (!out) return 0;
        free(im->rgba);
        im->rgba = out;
        im->w *= k;
        im->h *= k;
    }
    f->up = k;
    return 1;
}

void rdr_art_vector_letters(unsigned char *rgba, int W, int H, int k, const RdrArtFont *f, int page,
                            const unsigned char *ttf, const char *skip)
{
    stbtt_fontinfo fi;
    if (!ttf || !stbtt_InitFont(&fi, ttf, stbtt_GetFontOffsetForIndex(ttf, 0)))
        return;
    for (int c = 0x21; c < 0x100; c++) {
        const RdrArtGlyph *g = &f->glyph[c];
        if (!g->w || g->page != page || (skip && strchr(skip, c)) || !stbtt_FindGlyphIndex(&fi, c))
            continue;
        int x0 = g->x * k, y0 = g->y * k, x1 = (g->x + g->w) * k, y1 = (g->y + g->h) * k;
        if (x1 > W) x1 = W;
        if (y1 > H) y1 = H;
        /* the bitmap letter's ink: its box, colour and strongest alpha */
        int ix0 = INT_MAX, iy0 = INT_MAX, ix1 = -1, iy1 = -1, amax = 0;
        unsigned char col[3] = { 255, 255, 255 };
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                const unsigned char *p = rgba + ((size_t)y * W + x) * 4;
                if (p[3] > amax) { amax = p[3]; memcpy(col, p, 3); }
            }
        if (amax < 16) continue;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                if (rgba[((size_t)y * W + x) * 4 + 3] >= amax / 2) {
                    if (x < ix0) ix0 = x;
                    if (x > ix1) ix1 = x;
                    if (y < iy0) iy0 = y;
                    if (y > iy1) iy1 = y;
                }
        if (ix1 < ix0 || iy1 < iy0) continue;
        /* the outline's own box, in font units (y up) */
        int cx0, cy0, cx1, cy1;
        if (!stbtt_GetCodepointBox(&fi, c, &cx0, &cy0, &cx1, &cy1) || cx1 <= cx0 || cy1 <= cy0)
            continue;
        float sx = (float)(ix1 - ix0 + 1) / (float)(cx1 - cx0);
        float sy = (float)(iy1 - iy0 + 1) / (float)(cy1 - cy0);
        int bx0, by0, bx1, by1;
        stbtt_GetCodepointBitmapBoxSubpixel(&fi, c, sx, sy, 0, 0, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0, bh = by1 - by0;
        if (bw <= 0 || bh <= 0 || bw > 4096 || bh > 4096) continue;
        unsigned char *bm = (unsigned char *)calloc((size_t)bw * bh, 1);
        if (!bm) continue;
        stbtt_MakeCodepointBitmapSubpixel(&fi, bm, bw, bh, bw, sx, sy, 0, 0, c);
        /* a touch heavier, as the game's letters are (its bitmaps were
         * drawn bold): halfway to a 3 x 3 maximum */
        unsigned char *heavy = (unsigned char *)malloc((size_t)bw * bh);
        if (heavy) {
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int m = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if (xx >= 0 && xx < bw && yy >= 0 && yy < bh && bm[(size_t)yy * bw + xx] > m)
                                m = bm[(size_t)yy * bw + xx];
                        }
                    heavy[(size_t)y * bw + x] = (unsigned char)((bm[(size_t)y * bw + x] + m + 1) / 2);
                }
            free(bm);
            bm = heavy;
        }
        /* the old letter out, the new one in, its ink where the old ink was */
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                rgba[((size_t)y * W + x) * 4 + 3] = 0;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                int X = ix0 + x, Y = iy0 + y;
                if (X < x0 || X >= x1 || Y < y0 || Y >= y1) continue;
                unsigned char *p = rgba + ((size_t)Y * W + X) * 4;
                memcpy(p, col, 3);
                p[3] = (unsigned char)(bm[(size_t)y * bw + x] * amax / 255);
            }
        free(bm);
    }
}

/* A key cap drawn into a page (BGRA, W wide) over the box x, y, w, h: a
 * rounded cap in the menus' parchment with a dark rim and the key's name
 * in the TrueType font, fitted to the cap. Used for the game's own button
 * letters when playing on keys and mouse. */
void rdr_art_keycap(unsigned char *bgra, int W, int x, int y, int w, int h, const char *label,
                    const unsigned char *ttf)
{
    stbtt_fontinfo fi;
    if (w < 4 || h < 4)
        return;
    float r = (w < h ? w : h) * 0.22f, rim = (w < h ? w : h) * 0.07f;
    if (rim < 1.0f) rim = 1.0f;
    /* the cap: a rounded box, its rim, a shade at the foot */
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            float px = xx + 0.5f, py = yy + 0.5f;
            float dx = px < r ? r - px : (px > w - r ? px - (w - r) : 0);
            float dy = py < r ? r - py : (py > h - r ? py - (h - r) : 0);
            float d = sqrtf(dx * dx + dy * dy) - r;         /* < 0 inside */
            float cov = d < -0.5f ? 1.0f : (d > 0.5f ? 0.0f : 0.5f - d);
            unsigned char *o = bgra + ((size_t)(y + yy) * W + x + xx) * 4;
            if (cov <= 0.0f) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            int edge = d > -rim;
            float shade = (float)yy / (float)h;
            unsigned char cr, cg, cb;
            if (edge) { cr = 70; cg = 52; cb = 36; }
            else {
                cr = (unsigned char)(232 - 40 * shade);
                cg = (unsigned char)(220 - 40 * shade);
                cb = (unsigned char)(194 - 40 * shade);
            }
            o[0] = cb; o[1] = cg; o[2] = cr; o[3] = (unsigned char)(255 * cov);
        }
    if (!label || !*label || !ttf || !stbtt_InitFont(&fi, ttf, stbtt_GetFontOffsetForIndex(ttf, 0)))
        return;
    /* the name: as big as fits in the cap's face */
    int asc, desc, gap, adv_total = 0, n = (int)strlen(label);
    stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
    for (int i = 0; i < n; i++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&fi, (unsigned char)label[i], &adv, &lsb);
        adv_total += adv;
        if (i + 1 < n)
            adv_total += stbtt_GetCodepointKernAdvance(&fi, (unsigned char)label[i], (unsigned char)label[i + 1]);
    }
    if (adv_total <= 0)
        return;
    float face_w = w - 3.0f * rim, face_h = h - 3.0f * rim;
    float sc = face_w / (float)adv_total, sc_h = face_h * 0.78f / (float)(asc);
    if (sc_h < sc) sc = sc_h;
    float pen = x + (w - adv_total * sc) * 0.5f;
    int base = (int)(y + h * 0.5f + asc * sc * 0.5f - rim * 0.3f);
    for (int i = 0; i < n; i++) {
        int c = (unsigned char)label[i], adv, lsb, bx0, by0, bx1, by1;
        stbtt_GetCodepointHMetrics(&fi, c, &adv, &lsb);
        stbtt_GetCodepointBitmapBoxSubpixel(&fi, c, sc, sc, pen - (int)pen, 0, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0, bh = by1 - by0;
        if (bw > 0 && bh > 0 && bw < 1024 && bh < 1024) {
            unsigned char *bm = (unsigned char *)calloc((size_t)bw * bh, 1);
            if (bm) {
                stbtt_MakeCodepointBitmapSubpixel(&fi, bm, bw, bh, bw, sc, sc, pen - (int)pen, 0, c);
                {
                    /* heavier, to read at a glance on a small cap */
                    unsigned char *hv = (unsigned char *)malloc((size_t)bw * bh);
                    if (hv) {
                        for (int yy = 0; yy < bh; yy++)
                            for (int xx = 0; xx < bw; xx++) {
                                int m = 0;
                                for (int dy = -1; dy <= 1; dy++)
                                    for (int dx = -1; dx <= 1; dx++) {
                                        int X2 = xx + dx, Y2 = yy + dy;
                                        if (X2 >= 0 && X2 < bw && Y2 >= 0 && Y2 < bh && bm[(size_t)Y2 * bw + X2] > m)
                                            m = bm[(size_t)Y2 * bw + X2];
                                    }
                                hv[(size_t)yy * bw + xx] = (unsigned char)m;
                            }
                        free(bm);
                        bm = hv;
                    }
                }
                for (int yy = 0; yy < bh; yy++)
                    for (int xx = 0; xx < bw; xx++) {
                        int X = (int)pen + bx0 + xx, Y = base + by0 + yy;
                        if (X < x || X >= x + w || Y < y || Y >= y + h) continue;
                        unsigned char *o = bgra + ((size_t)Y * W + X) * 4;
                        int a = bm[(size_t)yy * bw + xx];
                        if (!a || !o[3]) continue;
                        /* dark ink over the cap */
                        o[0] = (unsigned char)((o[0] * (255 - a) + 20 * a) / 255);
                        o[1] = (unsigned char)((o[1] * (255 - a) + 28 * a) / 255);
                        o[2] = (unsigned char)((o[2] * (255 - a) + 38 * a) / 255);
                    }
                free(bm);
            }
        }
        pen += adv * sc;
        if (i + 1 < n)
            pen += stbtt_GetCodepointKernAdvance(&fi, c, (unsigned char)label[i + 1]) * sc;
    }
}

void rdr_art_free_font(RdrArtFont *f)
{
    for (int i = 0; i < f->npages; i++) rdr_art_free_image(&f->pages[i]);
    free(f->pages);
    memset(f, 0, sizeof *f);
}
