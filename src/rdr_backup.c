/* Red Dead Recompilation - a copy of the saves before each start.
 *
 * Saves\Backup\1 is the saves as they were when the game last started with
 * them changed, 2 and 3 the two times before. A save that a crash, a full
 * disk or a cheat spoiled can be put back by copying UDATA and TDATA from one
 * of them into Saves\. Nothing is copied when the saves have not changed
 * since Backup\1, so starting the game again and again keeps the history.
 *
 * Copyright (c) 2026 DallasXC and the Red Dead Recompilation contributors.
 * MIT licence (LICENSE). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#define BK_KEEP 3
#define BK_MAX_FILE (16u << 20)        /* an Xbox save is a few KB; more is not a save */
#define BK_PATH 1200

/* a path that does not fit is skipped, never cut short (a cut one would
 * name another file) */
#define BK_FMT(buf, ...) ((size_t)snprintf(buf, sizeof buf, __VA_ARGS__) < sizeof buf)

/* each file under dir/rel (sorted order is not needed: the fingerprint is a sum
 * of the files' own hashes, so the order the system lists them in is moot) */
typedef void (*bk_fn)(const char *full, const char *rel, void *ctx);

static void bk_walk(const char *root, const char *rel, int depth, bk_fn fn, void *ctx)
{
    char dir[BK_PATH];
    if (depth > 6) return;
    if (!(rel[0] ? BK_FMT(dir, "%s/%s", root, rel) : BK_FMT(dir, "%s", root))) return;
#if defined(_WIN32)
    {
        char pat[BK_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        if (!BK_FMT(pat, "%s\\*", dir)) return;
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            char r2[BK_PATH], full[BK_PATH];
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            if (!BK_FMT(r2, "%s%s%s", rel, rel[0] ? "/" : "", fd.cFileName) ||
                !BK_FMT(full, "%s/%s", root, r2)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) bk_walk(root, r2, depth + 1, fn, ctx);
            else fn(full, r2, ctx);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (!d) return;
        while ((e = readdir(d)) != NULL) {
            char r2[BK_PATH], full[BK_PATH];
            struct stat st;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (!BK_FMT(r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) ||
                !BK_FMT(full, "%s/%s", root, r2)) continue;
            if (lstat(full, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) bk_walk(root, r2, depth + 1, fn, ctx);
            else if (S_ISREG(st.st_mode)) fn(full, r2, ctx);
        }
        closedir(d);
    }
#endif
}

static void bk_mkdir(const char *p)
{
#if defined(_WIN32)
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
}

static void bk_dirs_for(const char *path)
{
    char t[BK_PATH];
    size_t i;
    if (!BK_FMT(t, "%s", path)) return;
    for (i = 1; t[i]; i++)
        if (t[i] == '/' || t[i] == '\\') {
            char c = t[i];
            t[i] = 0;
            bk_mkdir(t);
            t[i] = c;
        }
}

static unsigned char *bk_read(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb");
    long sz;
    unsigned char *b;
    *n = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || (unsigned long)sz > BK_MAX_FILE) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    b = (unsigned char *)malloc(sz ? (size_t)sz : 1);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    if (b) *n = (uint32_t)sz;
    return b;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const unsigned char *c = (const unsigned char *)p;
    while (n--) { h ^= *c++; h *= 0x100000001B3ull; }
    return h;
}

typedef struct { uint64_t sum; int files; int bad; } BkPrint;

static void bk_print_one(const char *full, const char *rel, void *ctx)
{
    BkPrint *fp = (BkPrint *)ctx;
    uint32_t n;
    unsigned char *b = bk_read(full, &n);
    uint64_t h = 0xCBF29CE484222325ull;
    if (!b) { fp->bad = 1; return; }
    h = fnv(h, rel, strlen(rel) + 1);
    h = fnv(h, &n, sizeof n);
    h = fnv(h, b, n);
    free(b);
    fp->sum += h;
    fp->files++;
}

typedef struct { const char *dst; int bad; } BkCopy;

static void bk_copy_one(const char *full, const char *rel, void *ctx)
{
    BkCopy *c = (BkCopy *)ctx;
    char out[BK_PATH];
    uint32_t n;
    unsigned char *b = bk_read(full, &n);
    FILE *f;
    if (!b) { c->bad = 1; return; }
    if (!BK_FMT(out, "%s/%s", c->dst, rel)) { free(b); c->bad = 1; return; }
    bk_dirs_for(out);
    f = fopen(out, "wb");
    if (!f || fwrite(b, 1, n, f) != n) c->bad = 1;
    if (f && fclose(f) != 0) c->bad = 1;
    free(b);
}

static void bk_remove_one(const char *full, const char *rel, void *ctx)
{
    (void)rel; (void)ctx;
#if defined(_WIN32)
    SetFileAttributesA(full, FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(full);
#else
    unlink(full);
#endif
}

static void bk_rmdirs(const char *dir, int depth)
{
    /* the files are gone (bk_remove_one); the folders, deepest first */
    if (depth > 6) return;
#if defined(_WIN32)
    {
        char pat[BK_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        h = BK_FMT(pat, "%s\\*", dir) ? FindFirstFileA(pat, &fd) : INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char sub[BK_PATH];
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                if (BK_FMT(sub, "%s\\%s", dir, fd.cFileName))
                    bk_rmdirs(sub, depth + 1);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(dir);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (d) {
            while ((e = readdir(d)) != NULL) {
                char sub[BK_PATH];
                struct stat st;
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                if (!BK_FMT(sub, "%s/%s", dir, e->d_name)) continue;
                if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) bk_rmdirs(sub, depth + 1);
            }
            closedir(d);
        }
        rmdir(dir);
    }
#endif
}

static void bk_remove_tree(const char *dir)
{
    bk_walk(dir, "", 0, bk_remove_one, NULL);
    bk_rmdirs(dir, 0);
}

static int bk_exists(const char *p)
{
#if defined(_WIN32)
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p, &st) == 0;
#endif
}

static int bk_move(const char *from, const char *to)
{
#if defined(_WIN32)
    return MoveFileExA(from, to, 0) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

/* save_dir: the saves folder (RECOMP_SAVE_DIR). Returns 1 when a copy was made. */
int rdr_save_backup(const char *save_dir)
{
    static const char *const parts[] = { "UDATA", "TDATA" };
    char bk[BK_PATH], one[BK_PATH], stamp[BK_PATH], line[64], src[BK_PATH], dst[BK_PATH];
    char d2[BK_PATH];
    BkPrint fp = { 0, 0, 0 };
    BkCopy cp;
    FILE *f;
    unsigned long long prev = 0;
    int i;

    if (!save_dir || !*save_dir || getenv("RDR_NO_SAVE_BACKUP"))
        return 0;
    for (i = 0; i < 2; i++) {
        if (BK_FMT(src, "%s/%s", save_dir, parts[i]))
            bk_walk(src, "", 0, bk_print_one, &fp);
    }
    if (fp.files == 0 || fp.bad)
        return 0;                       /* no saves yet, or one not readable: leave the backups */
    if (!BK_FMT(bk, "%s/Backup", save_dir) || !BK_FMT(one, "%s/1", bk) ||
        !BK_FMT(stamp, "%s/1/fingerprint.txt", bk))
        return 0;
    f = fopen(stamp, "r");
    if (f) {
        if (fgets(line, sizeof line, f))
            prev = strtoull(line, NULL, 16);
        fclose(f);
    }
    if (prev == (unsigned long long)fp.sum)
        return 0;                       /* unchanged since the last copy */

    bk_mkdir(bk);
    /* 3 goes, 2 -> 3, 1 -> 2 */
    if (BK_FMT(src, "%s/%d", bk, BK_KEEP) && bk_exists(src)) bk_remove_tree(src);
    for (i = BK_KEEP - 1; i >= 1; i--) {
        if (!BK_FMT(src, "%s/%d", bk, i) || !BK_FMT(dst, "%s/%d", bk, i + 1)) return 0;
        if (bk_exists(src) && bk_move(src, dst) != 0) {
            fprintf(stderr, "[SAVE] could not move the save backup %s (left as it is)\n", src);
            return 0;
        }
    }
    bk_mkdir(one);
    cp.dst = one;
    cp.bad = 0;
    for (i = 0; i < 2; i++) {
        if (!BK_FMT(src, "%s/%s", save_dir, parts[i]) || !BK_FMT(d2, "%s/%s", one, parts[i])) {
            cp.bad = 1;
            break;
        }
        cp.dst = d2;
        bk_walk(src, "", 0, bk_copy_one, &cp);
    }
    if (cp.bad) {
        fprintf(stderr, "[SAVE] the save backup in %s is not complete (a file could not be copied)\n", one);
        return 0;
    }
    f = fopen(stamp, "w");
    if (f) {
        fprintf(f, "%016llx\n", (unsigned long long)fp.sum);
        fclose(f);
    }
    fprintf(stderr, "[SAVE] the saves (%d files) copied to %s (older copies: 2 to %d)\n",
            fp.files, one, BK_KEEP);
    return 1;
}
