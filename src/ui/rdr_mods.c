/*
 * rdr_mods.c - mods and texture packs, each switched on or off in the
 * launcher (and the game's PC Options: from the next start).
 *
 *   Mods\<name>\...            a mod: files at the disc's paths (swat.dat,
 *                              lvls_m2\x.dat, bik\...), read instead of the
 *                              disc's
 *   Mods\swat.dat, Mods\bik\.. files put straight into Mods (the old way):
 *                              "Files directly in Mods", one more mod
 *   Mods\textures\<name>\...   a texture pack (.tga named by hash, any
 *                              subfolders)
 *   Mods\textures\*.tga        loose textures: one more pack
 *   Mods\Story People\         the story's people alone (launcher > Extras), not
 *                              one of these either;
 *   Mods\Preview\              the 2002 preview's (launcher > Extras), not
 *                              listed here
 *
 * A subfolder of Mods named as one of the game's own folders (bik,
 * lvls_m2 ...) is part of the loose files, not a mod. Everything is on
 * until switched off; the settings keep what is off (mods_off,
 * texture_packs_off: names with '|' between them, "<loose>" for the loose
 * ones), so a mod added later is on. Where two have the same file the one
 * earlier in the list (by name) wins.
 *
 * At the start the mods that are on become RECOMP_MODS_DIR (folders with
 * ';' between them) and the packs RECOMP_TEXTURE_PACK ("=<dir>": that
 * folder's own files only).
 */
#include "rdr_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#endif

const char *rdr_ui_game_dir(void);

static const char *env_first(const char *name, char *buf, size_t n)
{
    const char *e = getenv(name);
    if (!e || !*e || !strcmp(e, "-"))
        return NULL;
    snprintf(buf, n, "%.*s", (int)strcspn(e, ";"), e[0] == '=' ? e + 1 : e);
    return buf;
}

/* The player's own mods folder, and the texture packs' (as the start set
 * them: RDR_*_BASE once the settings were applied). */
const char *rdr_mods_base(void)
{
    static char b[1100];
    const char *r = env_first("RDR_MODS_BASE", b, sizeof b);
    return r ? r : env_first("RECOMP_MODS_DIR", b, sizeof b);
}

const char *rdr_texpacks_base(void)
{
    static char b[1100];
    const char *r = env_first("RDR_TEXPACK_BASE", b, sizeof b);
    return r ? r : env_first("RECOMP_TEXTURE_PACK", b, sizeof b);
}

static int listed(const char *list, const char *name)
{
    size_t n = strlen(name);
    const char *p = list;
    while (p && *p) {
        const char *end = strchr(p, '|');
        size_t l = end ? (size_t)(end - p) : strlen(p);
        if (l == n && !strncmp(p, name, n))
            return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static void list_set(char *list, size_t cap, const char *name, int in)
{
    char out[1024] = "";
    const char *p = list;
    size_t n = strlen(name);
    while (p && *p) {
        const char *end = strchr(p, '|');
        size_t l = end ? (size_t)(end - p) : strlen(p);
        if (!(l == n && !strncmp(p, name, n)) && l)
            snprintf(out + strlen(out), sizeof out - strlen(out), "%s%.*s", out[0] ? "|" : "", (int)l, p);
        p = end ? end + 1 : NULL;
    }
    if (in)
        snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", out[0] ? "|" : "", name);
    snprintf(list, cap, "%s", out);
}

/* One of the game's own folders (the disc's): part of the loose files. */
static int game_folder(const char *name)
{
    static const char *known[] = { "bik", "lvls_m2", "lvls_m4", "lvls_sng", "lvls", "media", "music",
                                   "sound", "stream", "streams", "movies" };
    char p[1200];
    unsigned i;
    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcasecmp(name, known[i]))
            return 1;
    if (rdr_ui_game_dir() && rdr_ui_game_dir()[0]) {
#if defined(_WIN32)
        DWORD a;
        snprintf(p, sizeof p, "%s\\%s", rdr_ui_game_dir(), name);
        a = GetFileAttributesA(p);
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", rdr_ui_game_dir(), name);
        return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
    }
    return 0;
}

/* A folder's entries: cb(name, is_dir). */
typedef void (*EntryFn)(const char *name, int is_dir, void *ctx);
static void each_entry(const char *dir, EntryFn cb, void *ctx)
{
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    char pat[1200];
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.cFileName[0] == '.')
            continue;
        cb(fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ctx);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char p[1200];
        struct stat st;
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        cb(e->d_name, stat(p, &st) == 0 && S_ISDIR(st.st_mode), ctx);
    }
    closedir(d);
#endif
}

typedef struct { RdrModEntry *out; int n, max, kind, loose; const RdrSettings *s; } Gather;

static int ends_with(const char *s, const char *t)
{
    size_t a = strlen(s), b = strlen(t);
    return a >= b && !strcasecmp(s + a - b, t);
}

static void add(Gather *g, const char *name, int kind)
{
    RdrModEntry *e;
    if (g->n >= g->max)
        return;
    e = &g->out[g->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->kind = kind;
    e->on = !listed(kind < 2 ? g->s->mods_off : g->s->packs_off, kind == 1 || kind == 3 ? "<loose>" : name);
}

static void mods_entry(const char *name, int is_dir, void *ctx)
{
    Gather *g = (Gather *)ctx;
    if (is_dir) {
        if (!strcasecmp(name, "textures") || !strcasecmp(name, "Preview") || !strcasecmp(name, "Preview.new")
            || !strcasecmp(name, "Preview.old") || !strncasecmp(name, "Story People", 12) || name[0] == '_')
            return;
        if (game_folder(name))
            g->loose = 1;
        else
            add(g, name, 0);
    } else if (!ends_with(name, ".txt") && !ends_with(name, ".md") && strcasecmp(name, "desktop.ini")) {
        g->loose = 1;
    }
}

static void packs_entry(const char *name, int is_dir, void *ctx)
{
    Gather *g = (Gather *)ctx;
    if (is_dir)
        add(g, name, 2);
    else if (ends_with(name, ".tga"))
        g->loose = 1;
}

static int cmp_entry(const void *a, const void *b)
{
    const RdrModEntry *x = (const RdrModEntry *)a, *y = (const RdrModEntry *)b;
    if (x->kind != y->kind)
        return x->kind - y->kind;
    return strcasecmp(x->name, y->name);
}

int rdr_mods_list(const RdrSettings *s, RdrModEntry *out, int max)
{
    Gather g;
    const char *b;
    memset(&g, 0, sizeof g);
    g.out = out;
    g.max = max;
    g.s = s;
    if ((b = rdr_mods_base()) != NULL) {
        each_entry(b, mods_entry, &g);
        if (g.loose)
            add(&g, "Loose files in Mods", 1);
    }
    g.loose = 0;
    if ((b = rdr_texpacks_base()) != NULL) {
        each_entry(b, packs_entry, &g);
        if (g.loose)
            add(&g, "Loose textures", 3);
    }
    qsort(out, (size_t)g.n, sizeof *out, cmp_entry);
    return g.n;
}

void rdr_mods_set(RdrSettings *s, const RdrModEntry *e, int on)
{
    const char *key = e->kind == 1 || e->kind == 3 ? "<loose>" : e->name;
    if (e->kind < 2)
        list_set(s->mods_off, sizeof s->mods_off, key, !on);
    else
        list_set(s->packs_off, sizeof s->packs_off, key, !on);
}

/* The folders for this start: mods (after the preview's, given first) and
 * packs. */
void rdr_mods_compose(const RdrSettings *s, const char *first, char *mods, size_t mn, char *tex, size_t tn)
{
    RdrModEntry e[128];
    char base[1100] = "", tbase[1100] = "";
    int n, i, loose_mods = 0;
    const char *b = rdr_mods_base(), *t = rdr_texpacks_base();
    if (b) snprintf(base, sizeof base, "%s", b);
    if (t) snprintf(tbase, sizeof tbase, "%s", t);
    n = rdr_mods_list(s, e, 128);
    mods[0] = tex[0] = 0;
    if (first && *first)
        snprintf(mods, mn, "%s", first);
    for (i = 0; i < n; i++) {
        size_t l;
        if (!e[i].on)
            continue;
        switch (e[i].kind) {
        case 0:
            l = strlen(mods);
            snprintf(mods + l, mn - l, "%s%s/%s", l ? ";" : "", base, e[i].name);
            break;
        case 1:
            loose_mods = 1;
            break;
        case 2:
            l = strlen(tex);
            snprintf(tex + l, tn - l, "%s%s/%s", l ? ";" : "", tbase, e[i].name);
            break;
        case 3:
            l = strlen(tex);
            snprintf(tex + l, tn - l, "%s=%s", l ? ";" : "", tbase);
            break;
        }
    }
    /* the loose files last, and the folder itself whenever it is there and
     * not switched off (an empty one costs nothing) */
    if (base[0] && (loose_mods || !listed(s->mods_off, "<loose>"))) {
        size_t l = strlen(mods);
        snprintf(mods + l, mn - l, "%s%s", l ? ";" : "", base);
    }
}

/* The folder shown in Explorer (made first when missing). */
void rdr_open_folder(const char *path)
{
    if (!path || !*path)
        return;
#if defined(_WIN32)
    CreateDirectoryA(path, NULL);
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
#else
    {
        char cmd[1300];
        snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", path);
        if (system(cmd) != 0) { /* nothing to do */ }
    }
#endif
}

/* ── The mods' fingerprint, for online play ─────────────────────────────
 *
 * Every machine in an online game runs the whole game, so the files the
 * mods put in place must be the same everywhere (the 2002 preview's
 * characters above all). A hash of what a list of mods folders serves:
 * each file's path inside its folder and its size, in the list's order;
 * an archive (.dat, "Dave") by its directory -- its names and their
 * unpacked sizes, not the packed ones a different zlib could change. The
 * mods base folder counts only its loose files (its mod, texture and
 * preview folders are listed on their own or are not game files). Text
 * files (.txt, .md: preview.txt has the date) do not count. */
typedef struct { uint32_t h; const char *base; int is_base; } Fp;

static void fp_add(Fp *f, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) {
        f->h ^= *b++;
        f->h *= 16777619u;
    }
}

static void fp_file(Fp *f, const char *full, const char *rel)
{
    FILE *fl = fopen(full, "rb");
    uint8_t h[16];
    char low[600];
    size_t i;
    long size = 0;
    for (i = 0; rel[i] && i < sizeof low - 1; i++)
        low[i] = (char)((rel[i] >= 'A' && rel[i] <= 'Z') ? rel[i] + 32 : (rel[i] == '\\' ? '/' : rel[i]));
    low[i] = 0;
    fp_add(f, low, strlen(low) + 1);
    if (!fl)
        return;
    if (fread(h, 1, 16, fl) == 16 && !memcmp(h, "Dave", 4)) {
        uint32_t count = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
        uint32_t dirb = (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24;
        uint32_t nameb = (uint32_t)h[12] | (uint32_t)h[13] << 8 | (uint32_t)h[14] << 16 | (uint32_t)h[15] << 24;
        if (count < 1000000 && dirb >= count * 16 && dirb < (64u << 20) && nameb < (64u << 20)) {
            uint8_t *d = (uint8_t *)malloc((size_t)dirb + nameb);
            if (d && fseek(fl, 0x800, SEEK_SET) == 0 && fread(d, 1, dirb + nameb, fl) == dirb + nameb) {
                uint32_t k;
                fp_add(f, &count, 4);
                for (k = 0; k < count; k++) {
                    fp_add(f, d + k * 16, 4);       /* the name's place */
                    fp_add(f, d + k * 16 + 8, 4);   /* the unpacked size */
                }
                fp_add(f, d + dirb, nameb);         /* the names */
            }
            free(d);
            fclose(fl);
            return;
        }
    }
    fseek(fl, 0, SEEK_END);
    size = ftell(fl);
    fclose(fl);
    fp_add(f, &size, sizeof size);
}

typedef struct { Fp *f; char full[1024]; char rel[600]; int depth; } FpWalk;

static void fp_entry(const char *name, int is_dir, void *ctx);
static void each_entry_sorted(const char *dir, EntryFn cb, void *ctx);
static void fp_dir(Fp *f, const char *full, const char *rel, int depth)
{
    FpWalk w;
    w.f = f;
    snprintf(w.full, sizeof w.full, "%s", full);
    snprintf(w.rel, sizeof w.rel, "%s", rel);
    w.depth = depth;
    each_entry_sorted(full, fp_entry, &w);
}

static void fp_entry(const char *name, int is_dir, void *ctx)
{
    FpWalk *w = (FpWalk *)ctx;
    char full[1100], rel[700];
    if (ends_with(name, ".txt") || ends_with(name, ".md") || !strcasecmp(name, "desktop.ini"))
        return;
    if (w->depth == 0 && w->f->is_base && is_dir && !game_folder(name))
        return;                         /* the base's mods, packs, preview: not its loose files */
    snprintf(full, sizeof full, "%s/%s", w->full, name);
    snprintf(rel, sizeof rel, "%s%s%s", w->rel, w->rel[0] ? "/" : "", name);
    if (is_dir) {
        if (w->depth < 6)
            fp_dir(w->f, full, rel, w->depth + 1);
    } else {
        fp_file(w->f, full, rel);
    }
}

/* Hashed in a fixed order: each folder's entries sorted first. */
typedef struct { char name[260]; int dir; } FpName;
typedef struct { FpName *v; int n, cap; } FpList;
static void fp_collect(const char *name, int is_dir, void *ctx)
{
    FpList *l = (FpList *)ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        FpName *v = (FpName *)realloc(l->v, (size_t)cap * sizeof *v);
        if (!v) return;
        l->v = v;
        l->cap = cap;
    }
    snprintf(l->v[l->n].name, sizeof l->v[0].name, "%s", name);
    l->v[l->n].dir = is_dir;
    l->n++;
}
static int fp_cmp(const void *a, const void *b)
{
    return strcasecmp(((const FpName *)a)->name, ((const FpName *)b)->name);
}

/* (each_entry's order is the file system's: sorted here instead) */
static void each_entry_sorted(const char *dir, EntryFn cb, void *ctx)
{
    FpList l = { 0, 0, 0 };
    int i;
    each_entry(dir, fp_collect, &l);
    if (l.n)
        qsort(l.v, (size_t)l.n, sizeof *l.v, fp_cmp);
    for (i = 0; i < l.n; i++)
        cb(l.v[i].name, l.v[i].dir, ctx);
    free(l.v);
}

unsigned rdr_mods_fingerprint(const char *dirs)
{
    Fp f;
    const char *m = dirs, *base = rdr_mods_base();
    int idx = 0;
    f.h = 2166136261u;
    f.base = base;
    while (m && *m) {
        const char *end = strchr(m, ';');
        int len = end ? (int)(end - m) : (int)strlen(m);
        char d[1100];
        if (len > 0 && len < (int)sizeof d) {
            FpWalk w;
            memcpy(d, m, (size_t)len);
            d[len] = 0;
            f.is_base = base && !strcmp(d, base);
            fp_add(&f, &idx, sizeof idx);
            w.f = &f;
            snprintf(w.full, sizeof w.full, "%s", d);
            w.rel[0] = 0;
            w.depth = 0;
            each_entry_sorted(d, fp_entry, &w);
            idx++;
        }
        m = end ? end + 1 : NULL;
    }
    return f.h;
}

/* What this machine's mods would be with the preview's on or off (a guest
 * matching a host). */
unsigned rdr_mods_fingerprint_for(const RdrSettings *s, int preview)
{
    static char mods[8192], tex[8192];
    char first[1200] = "";
    if (preview && rdr_preview_ready())
        snprintf(first, sizeof first, "%s", rdr_preview_dir());
    rdr_mods_compose(s, first, mods, sizeof mods, tex, sizeof tex);
    return rdr_mods_fingerprint(mods);
}
