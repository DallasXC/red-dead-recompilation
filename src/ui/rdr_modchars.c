/*
 * rdr_modchars.c - the characters a mod adds, for Play As and story co-op.
 *
 * A character added to Showdown (tools/mods: addchar.py, beta_roster.py for
 * the 2002 preview, story_npcs.py) is a set of files in the mod's swat.dat:
 * its "swat type" m<code>.swt, m<code>.cr and the rest -- what Play As
 * loads (rdr_hooks.c). Its name is the line-up's, in the archive's
 * swat.strtbl under the code. Every folder of RECOMP_MODS_DIR (';' between
 * them) is looked in; a code the game's own swat.dat has is not new.
 *
 * The archives ("Dave", see tools/mods/dave.py): "Dave", u32 count, u32
 * directory bytes, u32 name bytes; at 0x800 the entries { u32 name offset,
 * u32 data offset, u32 size, u32 packed size }; then the names, 6 bits a
 * character, 4 in 3 bytes, a name starting with code 56+ sharing a prefix
 * with the one before; data raw DEFLATE when packed < size.
 *
 * The string table (tools/mods/strtbl.py): u32 languages, a u32 offset
 * each; at the first: u32 count, records { u32 ELF hash of the key, u16,
 * u32 n + n bytes (font), u32 n + n UTF-16 units (the text, with its 0),
 * f32, f32, u16 }.
 */
#include "rdr_cheats.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../../third_party/stb/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static const char kAlpha[] = "\0 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~";

typedef struct {
    FILE *f;
    uint32_t count;
    uint8_t *dir;               /* the entries, then the names */
    uint32_t dirb, nameb;
    char **names;
} Dave;

static void dave_close(Dave *d)
{
    uint32_t i;
    if (d->names) {
        for (i = 0; i < d->count; i++)
            free(d->names[i]);
        free(d->names);
    }
    free(d->dir);
    if (d->f)
        fclose(d->f);
    memset(d, 0, sizeof *d);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The codes of the name at off (to its 0), at most max. */
static int name_codes(const uint8_t *tab, uint32_t n, uint32_t off, uint8_t *out, int max)
{
    int k = 0;
    while (off + 3 <= n) {
        uint8_t b0 = tab[off], b1 = tab[off + 1], b2 = tab[off + 2];
        uint8_t c[4];
        int j;
        c[0] = b0 & 0x3F;
        c[1] = (uint8_t)((b0 >> 6) | ((b1 & 0x0F) << 2));
        c[2] = (uint8_t)((b1 >> 4) | ((b2 & 0x03) << 4));
        c[3] = b2 >> 2;
        off += 3;
        for (j = 0; j < 4; j++) {
            if (k < max)
                out[k++] = c[j];
            if (c[j] == 0)
                return k;
        }
    }
    return -1;
}

static int dave_open(const char *path, Dave *d)
{
    uint8_t h[16];
    uint32_t i;
    char prev[256] = "";
    memset(d, 0, sizeof *d);
    d->f = fopen(path, "rb");
    if (!d->f)
        return 0;
    if (fread(h, 1, 16, d->f) != 16 || memcmp(h, "Dave", 4)) {
        dave_close(d);
        return 0;
    }
    d->count = rd32(h + 4);
    d->dirb = rd32(h + 8);
    d->nameb = rd32(h + 12);
    if (d->count > 1000000 || d->dirb < d->count * 16u || d->dirb > (64u << 20) || d->nameb > (64u << 20)) {
        dave_close(d);
        return 0;
    }
    d->dir = (uint8_t *)malloc((size_t)d->dirb + d->nameb + 4);
    d->names = (char **)calloc(d->count ? d->count : 1, sizeof *d->names);
    if (!d->dir || !d->names || fseek(d->f, 0x800, SEEK_SET) != 0
        || fread(d->dir, 1, d->dirb + d->nameb, d->f) != d->dirb + d->nameb) {
        dave_close(d);
        return 0;
    }
    for (i = 0; i < d->count; i++) {
        uint8_t c[300];
        char name[256];
        int n = name_codes(d->dir + d->dirb, d->nameb, rd32(d->dir + i * 16), c, (int)sizeof c), k, o = 0, from = 0;
        if (n <= 0) {
            name[0] = 0;
        } else {
            if (c[0] >= 56 && n > 1) {
                int keep = (c[0] - 56) + 8 * (c[1] & 31);
                if (keep > (int)strlen(prev))
                    keep = (int)strlen(prev);
                memcpy(name, prev, (size_t)keep);
                o = keep;
                from = 2;
            }
            for (k = from; k < n && c[k] && o < 255; k++)
                name[o++] = c[k] < sizeof kAlpha - 1 ? kAlpha[c[k]] : '?';
            name[o] = 0;
        }
        d->names[i] = (char *)malloc(strlen(name) + 1);
        if (d->names[i])
            strcpy(d->names[i], name);
        snprintf(prev, sizeof prev, "%s", name);
    }
    return 1;
}

static int dave_find(const Dave *d, const char *name)
{
    uint32_t i;
    for (i = 0; i < d->count; i++)
        if (d->names[i] && !strcmp(d->names[i], name))
            return (int)i;
    return -1;
}

/* An entry's bytes (malloc'd), or NULL. */
static uint8_t *dave_get(Dave *d, int i, uint32_t *len)
{
    const uint8_t *e = d->dir + (uint32_t)i * 16;
    uint32_t at = rd32(e + 4), size = rd32(e + 8), packed = rd32(e + 12);
    uint8_t *raw, *out;
    if (size > (32u << 20) || packed > (32u << 20) || fseek(d->f, (long)at, SEEK_SET) != 0)
        return NULL;
    raw = (uint8_t *)malloc(packed + 1);
    if (!raw || fread(raw, 1, packed, d->f) != packed) {
        free(raw);
        return NULL;
    }
    if (packed >= size) {
        *len = packed;
        return raw;
    }
    out = (uint8_t *)malloc(size + 1);
    if (!out || stbi_zlib_decode_noheader_buffer((char *)out, (int)size, (const char *)raw, (int)packed) != (int)size) {
        free(out);
        free(raw);
        return NULL;
    }
    free(raw);
    *len = size;
    return out;
}

static uint32_t elf_hash(const char *s)
{
    uint32_t h = 0, g;
    for (; *s; s++) {
        h = (h << 4) + (uint8_t)*s;
        g = h & 0xF0000000u;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* The text of a key in a string table, as UTF-8 with plain quotes. */
static int strtbl_get(const uint8_t *b, uint32_t n, const char *key, char *out, int max)
{
    uint32_t h = elf_hash(key), p, cnt, i;
    if (n < 8)
        return 0;
    p = rd32(b + 4);                            /* the first language */
    if (p + 4 > n)
        return 0;
    cnt = rd32(b + p);
    p += 4;
    for (i = 0; i < cnt && p + 10 <= n; i++) {
        uint32_t hh = rd32(b + p), fl, tl, k;
        p += 6;
        if (p + 4 > n) return 0;
        fl = rd32(b + p);
        p += 4 + fl;
        if (p + 4 > n) return 0;
        tl = rd32(b + p);
        p += 4;
        if (p + 2 * tl > n) return 0;
        if (hh == h) {
            int o = 0;
            for (k = 0; k < tl; k++) {
                unsigned c = b[p + 2 * k] | b[p + 2 * k + 1] << 8;
                if (!c) break;
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';     /* "Chicken\n(2002)" */
                else if (c == 0x201C || c == 0x201D) c = '"';
                else if (c == 0x2018 || c == 0x2019) c = '\'';
                if (c < 0x80) {
                    if (o + 1 < max) out[o++] = (char)c;
                } else if (c < 0x800) {
                    if (o + 2 < max) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
                } else if (o + 3 < max) {
                    out[o++] = (char)(0xE0 | c >> 12);
                    out[o++] = (char)(0x80 | ((c >> 6) & 63));
                    out[o++] = (char)(0x80 | (c & 63));
                }
            }
            out[o] = 0;
            while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
            {   /* "Nate  Harlow" */
                char *s;
                while ((s = strstr(out, "  ")) != NULL)
                    memmove(s, s + 1, strlen(s));
            }
            return out[0] != 0;
        }
        p += 2 * tl + 10;
    }
    return 0;
}

/* m<code>.cr names in an archive: the codes. */
static int is_type_name(const char *n, char *code, size_t max)
{
    size_t l = strlen(n);
    if (l < 6 || l > 12 || n[0] != 'm' || strcmp(n + l - 3, ".cr"))
        return 0;
    if (l - 4 >= max)
        return 0;
    memcpy(code, n + 1, l - 4);
    code[l - 4] = 0;
    return 1;
}

typedef struct { char code[16]; char name[64]; } Extra;

/* By name, a leading quote aside ("Pig" Josh with the P's). */
static int cmp_extra(const void *a, const void *b)
{
    const Extra *x = (const Extra *)a, *y = (const Extra *)b;
    const char *p = x->name + (x->name[0] == '"'), *q = y->name + (y->name[0] == '"');
    int c;
#if defined(_WIN32)
    c = _stricmp(p, q);
#else
    c = strcasecmp(p, q);
#endif
    return c ? c : strcmp(x->code, y->code);
}

/* The line-up's "Name '02" (a two-line name) as "Name (2002)", as the rest. */
static void tidy_name(char *n, size_t max)
{
    size_t l = strlen(n);
    if (l > 4 && !strcmp(n + l - 4, " '02") && l + 3 < max)
        snprintf(n + l - 4, max - (l - 4), " (2002)");
}

int rdr_characters_scan_mods(const char *game_dir, const char *mods)
{
    static Extra ex[RDR_MAX_CHARACTERS];
    static char storage[RDR_MAX_CHARACTERS][2][64];
    int nex = 0, i;
    Dave retail;
    char path[1200];
    const char *m;
    if (!mods || !*mods)
        return 0;
    snprintf(path, sizeof path, "%s/swat.dat", game_dir ? game_dir : "game");
    if (!dave_open(path, &retail))
        return 0;
    for (m = mods; m && *m;) {
        const char *end = strchr(m, ';');
        int len = end ? (int)(end - m) : (int)strlen(m);
        Dave d;
        if (len > 0 && len < 1000) {
            snprintf(path, sizeof path, "%.*s/swat.dat", len, m);
            if (dave_open(path, &d)) {
                uint8_t *tbl = NULL;
                uint32_t tn = 0;
                int ti = dave_find(&d, "swat.strtbl");
                uint32_t k;
                if (ti >= 0)
                    tbl = dave_get(&d, ti, &tn);
                for (k = 0; k < d.count && nex < RDR_MAX_CHARACTERS; k++) {
                    char code[16], want[24], name[64];
                    int j, dup = 0;
                    if (!d.names[k] || !is_type_name(d.names[k], code, sizeof code))
                        continue;
                    snprintf(want, sizeof want, "m%s.swt", code);
                    if (dave_find(&d, want) < 0 || dave_find(&retail, d.names[k]) >= 0)
                        continue;               /* not a whole character, or the game's own */
                    for (j = 0; j < nex; j++)
                        dup |= !strcmp(ex[j].code + 1, code);
                    if (dup)
                        continue;               /* an earlier folder's wins */
                    if (!tbl || !strtbl_get(tbl, tn, code, name, sizeof name)) {
                        for (j = 0; code[j] && j < 15; j++)
                            name[j] = (char)(code[j] >= 'a' && code[j] <= 'z' ? code[j] - 32 : code[j]);
                        name[j] = 0;
                    }
                    tidy_name(name, sizeof name);
                    snprintf(ex[nex].code, sizeof ex[nex].code, "m%s", code);
                    snprintf(ex[nex].name, sizeof ex[nex].name, "%s", name);
                    nex++;
                }
                free(tbl);
                dave_close(&d);
            }
        }
        m = end ? end + 1 : NULL;
    }
    dave_close(&retail);
    qsort(ex, (size_t)nex, sizeof ex[0], cmp_extra);
    /* the same name twice (the preview has two Chickens, two Navarros...):
     * the second "Chicken (2002) 2", in the list's order */
    {
        static char orig[RDR_MAX_CHARACTERS][64];
        for (i = 0; i < nex; i++)
            snprintf(orig[i], sizeof orig[i], "%s", ex[i].name);
        for (i = 1; i < nex; i++) {
            int j = i - 1;
            while (j >= 0 && !strcmp(orig[j], orig[i]))
                j--;
            if (i - j > 1) {
                char t[80];
                snprintf(t, sizeof t, "%s %d", orig[i], i - j);
                snprintf(ex[i].name, sizeof ex[i].name, "%s", t);
            }
        }
    }
    for (i = 0; i < nex && g_rdr_character_count < RDR_MAX_CHARACTERS; i++) {
        if (rdr_character_index(ex[i].code))
            continue;
        snprintf(storage[i][0], sizeof storage[i][0], "%s", ex[i].code);
        snprintf(storage[i][1], sizeof storage[i][1], "%s", ex[i].name);
        g_rdr_characters[g_rdr_character_count].code = storage[i][0];
        g_rdr_characters[g_rdr_character_count].name = storage[i][1];
        g_rdr_character_count++;
    }
    if (getenv("RDR_MODCHARS_LOG"))
        for (i = 0; i < nex; i++)
            fprintf(stderr, "[MODS]   %s  %s\n", ex[i].code, ex[i].name);
    fprintf(stderr, "[MODS] %d characters added by mods (Play As)\n", nex);
    fflush(stderr);
    return nex;
}
