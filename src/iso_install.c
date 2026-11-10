/*
 * iso_install.c - unpack an Xbox disc image into a folder.
 *
 * The player drags the game's ISO onto the exe (or onto "Install Game from
 * ISO.bat"); the files land in the game folder and the game starts. No other
 * tool is needed.
 *
 * An Xbox disc's game files are in an XDVDFS partition:
 *   - the partition's sector 32 (byte 0x10000) holds the volume descriptor:
 *     "MICROSOFT*XBOX*MEDIA" (20 bytes), u32 root directory sector, u32 root
 *     directory size, ... and the same 20 bytes again at 0x7EC;
 *   - a directory is a binary tree of entries packed into its sectors:
 *       u16 left, u16 right   (offsets of the subtrees in 4-byte units from
 *                              the directory's start; 0 = none)
 *       u32 sector, u32 size  (the file's data, or a subdirectory's entries)
 *       u8 attributes         (0x10: directory)
 *       u8 name length, then the name (no terminator)
 *     an entry never straddles a sector; the rest of a sector is 0xFF.
 * Sectors are 2048 bytes, counted from the partition's start. Where the
 * partition starts depends on the dump: at 0 for an "xiso" (extract-xiso,
 * the usual emulator format), else after the video partition of a full
 * disc dump (Redump): 0x0FD90000 (XGD1), 0x18300000 (XGD2), 0x02080000
 * (XGD3).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#define SEP '\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#define mkdir_one(p) mkdir(p, 0755)
#define fseek64 fseeko
#define ftell64 ftello
#define SEP '/'
#endif

#define XSECT 2048u
static const char XMAGIC[20] = { 'M','I','C','R','O','S','O','F','T','*','X','B','O','X','*','M','E','D','I','A' };

typedef struct {
    FILE *f;
    uint64_t base;          /* the partition's byte offset in the image */
    uint64_t image_bytes;
    uint64_t total, done;   /* bytes of file data, for progress */
    unsigned files, dirs;
    char *buf;              /* copy buffer */
    int error;
    /* Directories walked this pass (by sector): a damaged or made-up image
     * whose directory points back at one already walked is not walked
     * again (it would never end). */
    uint32_t walked[8192];
    unsigned nwalked;
} Iso;

static int read_at(Iso *x, uint64_t at, void *p, size_t n)
{
    if (at + n > x->image_bytes)
        return 0;
    if (fseek64(x->f, (int64_t)at, SEEK_SET) != 0)
        return 0;
    return fread(p, 1, n, x->f) == n;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Every directory on the way, as mkdir -p. */
static int make_dirs(const char *path)
{
    char tmp[4096];
    size_t i, n = strlen(path);
    if (n >= sizeof tmp)
        return 0;
    memcpy(tmp, path, n + 1);
    for (i = 1; i <= n; i++)
        if (tmp[i] == '/' || tmp[i] == '\\' || tmp[i] == 0) {
            char c = tmp[i];
            tmp[i] = 0;
            if (!(i == 2 && tmp[1] == ':'))          /* "C:" */
                if (mkdir_one(tmp) != 0 && errno != EEXIST)
                    return 0;
            tmp[i] = c;
        }
    return 1;
}

/* A name from the disc is used as one path component: nothing that could
 * climb out of the destination or name a device. */
static int name_ok(const char *s)
{
    if (!*s || !strcmp(s, ".") || !strcmp(s, ".."))
        return 0;
    for (; *s; s++)
        if (*s == '/' || *s == '\\' || *s == ':' || (unsigned char)*s < 32)
            return 0;
    return 1;
}

/* For the launcher's install page (rdr_install_progress). */
static volatile int s_install_pct;
static volatile unsigned s_install_files;

void rdr_install_progress(int *pct, unsigned *files)
{
    if (pct) *pct = s_install_pct;
    if (files) *files = s_install_files;
}

static void progress(Iso *x)
{
    static int last = -1;
    int pc = x->total ? (int)(x->done * 100 / x->total) : 0;
    s_install_pct = pc;
    s_install_files = x->files;
    if (pc != last) {
        last = pc;
        fprintf(stderr, "\r  %3d%%  %u files", pc, x->files);
        fflush(stderr);
    }
}

static int copy_file(Iso *x, uint32_t sector, uint32_t size, const char *dst)
{
    FILE *o = fopen(dst, "wb");
    uint64_t at = x->base + (uint64_t)sector * XSECT;
    uint32_t left = size;
    if (!o) {
        fprintf(stderr, "\n  cannot write %s\n", dst);
        return 0;
    }
    if (fseek64(x->f, (int64_t)at, SEEK_SET) != 0) {
        fclose(o);
        return 0;
    }
    while (left) {
        size_t n = left < (1u << 20) ? left : (1u << 20);
        if (fread(x->buf, 1, n, x->f) != n || fwrite(x->buf, 1, n, o) != n) {
            fclose(o);
            fprintf(stderr, "\n  read or write failed: %s\n", dst);
            return 0;
        }
        left -= (uint32_t)n;
        x->done += n;
        progress(x);
    }
    x->files++;
    return fclose(o) == 0;
}

/* Walk a directory's tree. With out == NULL only sizes are counted. */
static int walk_dir(Iso *x, uint32_t sector, uint32_t size, const char *out, int depth)
{
    uint8_t *d, *seen;
    uint32_t *stack, sp = 0, cap;
    unsigned i;
    int ok = 1;
    if (depth > 32 || size == 0)
        return depth <= 32;
    if (size > (64u << 20))
        return 0;
    for (i = 0; i < x->nwalked; i++)
        if (x->walked[i] == sector) {
            fprintf(stderr, "\n  skipped a folder the disc lists twice\n");
            return 1;
        }
    if (x->nwalked == sizeof x->walked / sizeof x->walked[0])
        return 0;                             /* far more folders than any disc has */
    x->walked[x->nwalked++] = sector;
    d = (uint8_t *)malloc(size);
    seen = (uint8_t *)calloc(size / 4 + 1, 1);   /* the entries walked in this folder */
    if (!d || !seen || !read_at(x, x->base + (uint64_t)sector * XSECT, d, size)) {
        free(d);
        free(seen);
        return 0;
    }
    cap = size / 14 + 4;
    stack = (uint32_t *)malloc(cap * sizeof *stack);
    if (!stack) {
        free(d);
        free(seen);
        return 0;
    }
    stack[sp++] = 0;
    while (sp && ok) {
        uint32_t off = stack[--sp] * 4u;
        uint16_t l, r;
        uint32_t fs, fz;
        uint8_t attr, nl;
        char name[256], path[4096];
        if (off + 14 > size || seen[off / 4])
            continue;
        seen[off / 4] = 1;
        l = rd16(d + off);
        r = rd16(d + off + 2);
        if (l == 0xFFFF && r == 0xFFFF)
            continue;                         /* sector padding */
        fs = rd32(d + off + 4);
        fz = rd32(d + off + 8);
        attr = d[off + 12];
        nl = d[off + 13];
        if (off + 14 + nl > size)
            continue;
        memcpy(name, d + off + 14, nl);
        name[nl] = 0;
        if (l && sp < cap) stack[sp++] = l;
        if (r && sp < cap) stack[sp++] = r;
        if (!name_ok(name)) {
            fprintf(stderr, "\n  skipped a bad name on the disc\n");
            continue;
        }
        if (out) {
            if ((size_t)snprintf(path, sizeof path, "%s%c%s", out, SEP, name) >= sizeof path) {
                ok = 0;
                break;
            }
        }
        if (attr & 0x10) {
            x->dirs++;
            if (out && !make_dirs(path)) {
                fprintf(stderr, "\n  cannot create %s\n", path);
                ok = 0;
                break;
            }
            ok = walk_dir(x, fs, fz, out ? path : NULL, depth + 1);
        } else if (out) {
            ok = copy_file(x, fs, fz, path);
        } else {
            x->total += fz;
        }
    }
    free(stack);
    free(seen);
    free(d);
    return ok;
}

/* Where the XDVDFS partition starts in this image, or -1. */
static int64_t find_partition(Iso *x, uint32_t *root_sector, uint32_t *root_size)
{
    static const uint64_t offsets[] = { 0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull };
    size_t i;
    for (i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
        uint8_t v[XSECT];
        if (!read_at(x, offsets[i] + 32u * XSECT, v, sizeof v))
            continue;
        if (memcmp(v, XMAGIC, 20) || memcmp(v + 0x7EC, XMAGIC, 20))
            continue;
        *root_sector = rd32(v + 20);
        *root_size = rd32(v + 24);
        return (int64_t)offsets[i];
    }
    return -1;
}

/* The title ID in the image's default.xbe (its certificate's +8), or 0.
 * Red Dead Revolver (USA): 0x5454001A. */
static uint32_t root_title_id(Iso *x, uint32_t rs, uint32_t rz)
{
    uint8_t *d;
    uint32_t off, id = 0;
    if (rz == 0 || rz > (1u << 20))
        return 0;
    d = (uint8_t *)malloc(rz);
    if (!d || !read_at(x, x->base + (uint64_t)rs * XSECT, d, rz)) {
        free(d);
        return 0;
    }
    /* the root's entries, sector by sector (the tree's order does not matter
     * for one name) */
    for (off = 0; off + 14 <= rz;) {
        uint16_t l = rd16(d + off), r = rd16(d + off + 2);
        uint8_t nl = d[off + 13];
        if (l == 0xFFFF && r == 0xFFFF) {
            off = (off / XSECT + 1) * XSECT;   /* the rest of this sector is padding */
            continue;
        }
        if (off + 14 + nl > rz)
            break;
        if (nl == 11 && !memcmp(d + off + 14, "default.xbe", 11)) {
            uint8_t h[0x200];
            uint32_t fs = rd32(d + off + 4), base, cert;
            if (read_at(x, x->base + (uint64_t)fs * XSECT, h, sizeof h) && !memcmp(h, "XBEH", 4)) {
                uint8_t c[12];
                base = rd32(h + 0x104);
                cert = rd32(h + 0x118);
                if (cert > base && read_at(x, x->base + (uint64_t)fs * XSECT + (cert - base), c, sizeof c))
                    id = rd32(c + 8);
            }
            break;
        }
        off = (off + 14 + nl + 3) & ~3u;
    }
    free(d);
    return id;
}

/* Unpack image into dest (created if missing). 1 on success, 0 on a
 * failure, -2 when the image is another game. */
int rdr_install_iso(const char *image, const char *dest)
{
    Iso x;
    uint32_t rs = 0, rz = 0;
    int64_t base;
    int ok;
    memset(&x, 0, sizeof x);
    x.f = fopen(image, "rb");
    if (!x.f) {
        fprintf(stderr, "Cannot open %s\n", image);
        return 0;
    }
    fseek64(x.f, 0, SEEK_END);
    x.image_bytes = (uint64_t)ftell64(x.f);
    base = find_partition(&x, &rs, &rz);
    if (base < 0) {
        fprintf(stderr, "%s is not an Xbox disc image (no XDVDFS volume found).\n", image);
        fclose(x.f);
        return 0;
    }
    x.base = (uint64_t)base;
    {
        uint32_t id = root_title_id(&x, rs, rz);
        if (id != 0x5454001Au) {
            fprintf(stderr, "%s is not Red Dead Revolver (USA): its title ID is %08X.\n", image, (unsigned)id);
            fclose(x.f);
            return -2;
        }
    }
    s_install_pct = 0;
    s_install_files = 0;
    x.buf = (char *)malloc(1u << 20);
    if (!x.buf) {
        fclose(x.f);
        return 0;
    }
    /* Sizes first, for the progress; then the files. */
    if (!walk_dir(&x, rs, rz, NULL, 0)) {
        fprintf(stderr, "The disc image's directory could not be read.\n");
        free(x.buf);
        fclose(x.f);
        return 0;
    }
    fprintf(stderr, "Installing %.1f MB from %s\n  into %s\n", x.total / 1048576.0, image, dest);
    x.files = x.dirs = 0;
    x.nwalked = 0;
    ok = make_dirs(dest) && walk_dir(&x, rs, rz, dest, 0);
    fprintf(stderr, "\n%s: %u files, %u folders.\n", ok ? "Done" : "Failed", x.files, x.dirs);
    free(x.buf);
    fclose(x.f);
    return ok;
}
