/*
 * rdr_lobby.c - the online co-op lobby (the desk's "Online Co-op", where the
 * Xbox's Friends Menu was; also PC Options > Online Co-op).
 *
 * Online co-op is lockstep (rdr_lockstep.c): every machine runs the whole
 * game and only the controllers cross the network, so every machine has to
 * start the same game -- the same save, the same options -- at the same
 * time. The lobby is where that is arranged:
 *
 *   1. The host picks how many players, and gets a join code to give the
 *      others (one per way to reach this PC: the LAN, a VPN such as
 *      Tailscale, the internet with UDP port 27032 forwarded).
 *   2. Each friend types the code (or an address). The host sends them its
 *      save (the profiles, UDATA and TDATA) and its game options; each
 *      shows ready when it has them.
 *   3. The host starts: every machine starts the game again, in lockstep,
 *      on a copy of the host's save (Saves\Online) -- nobody's own saves
 *      are touched -- and they play the story together.
 *
 * UDP, port 27032 (the lockstep's own, used after the lobby closes).
 * Messages: "RDLB", a type byte, a body:
 *   HELLO   guest->host  version u16, name[32], id u32, key u32
 *   WELCOME host->guest  slot u8, players u8, session u32, size u32, crc u32
 *   FULL / REFUSE (reason u8: 1 another version, 2 already started, 4 not
 *            this game's code)
 *
 * The key: 20 random bits the host makes for each game, in its join code.
 * A join without it is refused, except from a home network, a VPN or this
 * PC (an address typed instead of the code), so a stranger who finds the
 * port open on the internet can neither take a place nor get the save.
 * Wrong keys are answered five times a second at most.
 *   ROSTER  host->guest  players u8, 4 x { present u8, ready u8, name[32] }
 *   FILE    host->guest  chunk u16, data (CHUNK bytes; the last shorter)
 *   NEED    guest->host  count u8, chunk u16 x count
 *   READY   guest->host  (it has everything, the checksum agrees)
 *   START   host->guest  session u32
 *   PING / BYE
 */
#define _CRT_RAND_S                     /* rand_s: Windows' random numbers */
#include "rdr_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#define LB_MAGIC   0x424C4452u      /* "RDLB" */
#define LB_VERSION 8                    /* 8: the join code's key */
#define LB_KEY_BITS 20
#define LB_CHUNK   1000
#define LB_MAXBLOB (4u << 20)
enum { LB_HELLO = 1, LB_WELCOME, LB_FULL, LB_REFUSE, LB_ROSTER, LB_FILE, LB_NEED, LB_READY,
       LB_START, LB_PING, LB_BYE, LB_MISMATCH };
/* A guest's state on the roster (Peer.ready): 0 getting the save, 1 ready,
 * 2 lacks the 2002 preview's characters the host plays with, 3 its mods
 * differ from the host's (rdr_mods_fingerprint). */
enum { LB_NEEDS_PREVIEW = 2, LB_OTHER_MODS = 3 };

/* the game options the host's game is played with (main.c applies them
 * over the settings file: rdr_online_apply_settings) */
#include "../ui/rdr_settings.h"
#define LB_NOPTS 16

typedef struct {
    int used, ready;
    RdrAddr addr;
    uint32_t id;                /* the guest's own (a code's addresses can reach us twice) */
    char name[33];
    uint64_t last_ms;
    uint64_t need_ms;           /* its chunk budget's window */
    int need_sent;
} Peer;

static struct {
    int state;                  /* RdrLobbyView.state */
    int players, me;
    char myname[33];
    int sock;
    uint32_t session;
    uint32_t key;               /* host: this game's (in the code); guest: the code's, or 0 */
    /* host */
    Peer peer[4];
    int start_req;
    /* guest */
    RdrAddr host;
    uint32_t myid;
    RdrAddr cand[4];            /* guest: the code's addresses, until one answers */
    int ncand;
    char host_text[96];
    uint64_t heard_ms, began_ms;
    uint8_t *have;              /* chunks received */
    uint32_t want_crc;
    int welcomed, complete, checked, mismatch;
    /* the blob: the options and the save */
    uint8_t *blob;
    uint32_t blob_n;
    /* what the page shows */
    char status[160];
    int ncodes;
    char codes[6][40], where[6][48], reach[128];
    char rnames[4][33];
    int rpresent[4], rready[4];
    int quit;
    int running;
} L;

#if defined(_WIN32)
static CRITICAL_SECTION s_lk;
static int s_lk_init;
static void lk(void) { if (!s_lk_init) { InitializeCriticalSection(&s_lk); s_lk_init = 1; } EnterCriticalSection(&s_lk); }
static void ulk(void) { LeaveCriticalSection(&s_lk); }
#else
static pthread_mutex_t s_lk = PTHREAD_MUTEX_INITIALIZER;
static void lk(void) { pthread_mutex_lock(&s_lk); }
static void ulk(void) { pthread_mutex_unlock(&s_lk); }
#endif

/* 32 random bits from the system (the key must not be guessable from the
 * time the game was hosted). */
static uint32_t random_u32(void)
{
    uint32_t v = 0;
#if defined(_WIN32)
    unsigned int r;
    if (rand_s(&r) == 0)
        return (uint32_t)r;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(&v, 1, sizeof v, f);
        fclose(f);
        if (got == sizeof v)
            return v;
    }
#endif
    v = (uint32_t)rdr_net_now_ms() * 2654435761u ^ (uint32_t)rand() * 2246822519u;
    return v ^ (uint32_t)(uintptr_t)&v;
}

/* Home networks, VPNs (Tailscale's range) and this PC: where a join by
 * address alone, without the code's key, is taken. */
static int addr_private(const RdrAddr *a)
{
    const uint8_t *ip = a->ip;
    return ip[0] == 10 || ip[0] == 127 || (ip[0] == 192 && ip[1] == 168)
        || (ip[0] == 172 && (ip[1] & 0xF0) == 16) || (ip[0] == 169 && ip[1] == 254)
        || (ip[0] == 100 && (ip[1] & 0xC0) == 64);
}

static void say(const char *fmt, const char *a)
{
    lk();
    snprintf(L.status, sizeof L.status, fmt, a ? a : "");
    ulk();
    fprintf(stderr, "[LOBBY] %s\n", L.status);
    fflush(stderr);
}

/* ── the join code: every address this PC can be reached at, in one ───
 *
 * Crockford's base32, in groups of five ("ABCDE-FGHJK-MN"). The bits, first
 * to last: the number of addresses less one (2 bits); each address as a
 * kind (2 bits) and its bits -- 0: 192.168.x.y (16), 1: 10.x.y.z (24),
 * 2: 100.64.0.0/10, the VPNs' (Tailscale's) range (22), 3: any (32); zeros
 * to the end of the last letter; then a check letter. The port is always
 * 27032. Typically 5 letters on a LAN only, 13 with the internet's view
 * too. The joining game calls every address at once and keeps the first
 * that answers, so one code serves at home, over a VPN and over the
 * internet. (The first codes, of one address and its port, are still read.) */
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

typedef struct { uint8_t b[24]; int n; } Bits;          /* up to 192 bits */
static void bits_put(Bits *s, uint32_t v, int n)
{
    while (n--) {
        if (s->n < 192 && (v >> n & 1u))
            s->b[s->n >> 3] |= (uint8_t)(0x80u >> (s->n & 7));
        s->n++;
    }
}
static uint32_t bits_get(const Bits *s, int *at, int n)
{
    uint32_t v = 0;
    while (n--) {
        int i = (*at)++;
        v = v << 1 | (i < s->n && i < 192 ? (uint32_t)(s->b[i >> 3] >> (7 - (i & 7)) & 1u) : 0u);
    }
    return v;
}
static uint32_t ip_u32(const uint8_t ip[4])
{
    return (uint32_t)ip[0] << 24 | (uint32_t)ip[1] << 16 | (uint32_t)ip[2] << 8 | ip[3];
}
static int ip_kind(const uint8_t ip[4])
{
    if (ip[0] == 192 && ip[1] == 168) return 0;
    if (ip[0] == 10) return 1;
    if (ip[0] == 100 && (ip[1] & 0xC0) == 64) return 2;
    return 3;
}
static const int kKindBits[4] = { 16, 24, 22, 32 };
/* the check letter: catches any one letter mistyped and two swapped */
static int code_check(const uint8_t *d, int n)
{
    int k, c = 0;
    for (k = 0; k < n; k++)
        c = (c + (k + 1) * d[k]) % 31;
    return c;
}

static void code_make_multi(const uint8_t ips[][4], int n, uint32_t key, char *out, int cap)
{
    Bits s;
    uint8_t d[40];
    int k, at = 0, o = 0, letters;
    memset(&s, 0, sizeof s);
    bits_put(&s, (uint32_t)(n - 1), 2);
    for (k = 0; k < n; k++) {
        int kind = ip_kind(ips[k]);
        bits_put(&s, (uint32_t)kind, 2);
        bits_put(&s, ip_u32(ips[k]) & (kKindBits[kind] == 32 ? 0xFFFFFFFFu : (1u << kKindBits[kind]) - 1u),
                 kKindBits[kind]);
    }
    bits_put(&s, key, LB_KEY_BITS);
    letters = (s.n + 4) / 5;
    for (k = 0; k < letters; k++)
        d[k] = (uint8_t)bits_get(&s, &at, 5);
    d[letters] = (uint8_t)code_check(d, letters);
    for (k = 0; k <= letters && o + 2 < cap; k++) {
        if (k && k % 5 == 0)
            out[o++] = '-';
        out[o++] = B32[d[k]];
    }
    out[o] = 0;
}

/* The letters of a code, as 5-bit values (typing slips forgiven). */
static int code_letters(const char *text, uint8_t *d, int max)
{
    int n = 0;
    for (; *text; text++) {
        int c = toupper((unsigned char)*text);
        const char *p;
        if (c == '-' || c == ' ')
            continue;
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        p = c ? strchr(B32, c) : NULL;
        if (!p || n >= max)
            return -1;
        d[n++] = (uint8_t)(p - B32);
    }
    return n;
}

/* A code to the addresses it holds (all with the lobby's port): their
 * number; *key the game's key (0: none, an address or an old code). */
static int code_read(const char *text, RdrAddr *out, int max, uint32_t *key)
{
    uint8_t d[40];
    int n = code_letters(text, d, 40), k;
    *key = 0;
    if (n >= 2 && d[n - 1] == code_check(d, n - 1)) {
        Bits s;
        uint8_t ips[4][4];
        int at = 0, cnt;
        memset(&s, 0, sizeof s);
        for (k = 0; k < n - 1; k++)
            bits_put(&s, d[k], 5);
        cnt = (int)bits_get(&s, &at, 2) + 1;
        for (k = 0; k < cnt; k++) {
            int kind = (int)bits_get(&s, &at, 2);
            uint32_t v = bits_get(&s, &at, kKindBits[kind]);
            if (kind == 0) v |= 0xC0A80000u;
            else if (kind == 1) v |= 0x0A000000u;
            else if (kind == 2) v |= 0x64400000u;
            ips[k][0] = (uint8_t)(v >> 24); ips[k][1] = (uint8_t)(v >> 16);
            ips[k][2] = (uint8_t)(v >> 8); ips[k][3] = (uint8_t)v;
        }
        *key = bits_get(&s, &at, LB_KEY_BITS);
        if (at <= s.n && s.n - at < 5 && bits_get(&s, &at, s.n - at) == 0) {
          for (k = 0; k < cnt && k < max; k++) {
            memcpy(out[k].ip, ips[k], 4);
            out[k].port = RDR_LOBBY_PORT;
          }
          return cnt < max ? cnt : max;
        }
    }
    if (n == 10) {                      /* the first codes: one address and its port */
        uint64_t v = 0;
        for (k = 0; k < 10; k++)
            v = v << 5 | d[k];
        if (!(v >> 48) && (uint16_t)v) {
            out[0].ip[0] = (uint8_t)(v >> 40); out[0].ip[1] = (uint8_t)(v >> 32);
            out[0].ip[2] = (uint8_t)(v >> 24); out[0].ip[3] = (uint8_t)(v >> 16);
            out[0].port = (uint16_t)v;
            return 1;
        }
    }
    return -1;
}

static uint32_t crc32_of(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* ── files: the save's UDATA and TDATA, packed and unpacked ──────────── */
typedef struct { uint8_t *p; uint32_t n, cap; int bad; } Buf;
static void put(Buf *b, const void *d, uint32_t n)
{
    if (b->bad) return;
    if (b->n + n > LB_MAXBLOB) { b->bad = 1; return; }
    if (b->n + n > b->cap) {
        uint32_t c = b->cap ? b->cap * 2 : 65536;
        uint8_t *np;
        while (c < b->n + n) c *= 2;
        np = (uint8_t *)realloc(b->p, c);
        if (!np) { b->bad = 1; return; }
        b->p = np;
        b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put32(Buf *b, uint32_t v) { put(b, &v, 4); }

static void pack_file(Buf *b, const char *full, const char *rel)
{
    FILE *f = fopen(full, "rb");
    long sz;
    uint16_t pl = (uint16_t)strlen(rel);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz >= 0 && sz < (long)LB_MAXBLOB) {
        uint8_t *d = (uint8_t *)malloc((size_t)sz + 1);
        if (d && fread(d, 1, (size_t)sz, f) == (size_t)sz) {
            put(b, &pl, 2);
            put(b, rel, pl);
            put32(b, (uint32_t)sz);
            put(b, d, (uint32_t)sz);
        }
        free(d);
    }
    fclose(f);
}

/* every file under root/rel, recursively */
static void pack_dir(Buf *b, const char *root, const char *rel, int depth)
{
    char dir[1024];
    if (depth > 6) return;
    snprintf(dir, sizeof dir, "%s/%s", root, rel);
#if defined(_WIN32)
    {
        char pat[1100];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof pat, "%s\\*", dir);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            char r2[1024], full[1100];
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(r2, sizeof r2, "%s/%s", rel, fd.cFileName);
            snprintf(full, sizeof full, "%s/%s", root, r2);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) pack_dir(b, root, r2, depth + 1);
            else pack_file(b, full, r2);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (!d) return;
        while ((e = readdir(d)) != NULL) {
            char r2[1024], full[1100];
            struct stat st;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(r2, sizeof r2, "%s/%s", rel, e->d_name);
            snprintf(full, sizeof full, "%s/%s", root, r2);
            if (stat(full, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) pack_dir(b, root, r2, depth + 1);
            else pack_file(b, full, r2);
        }
        closedir(d);
    }
#endif
}

static void remove_tree(const char *dir, int depth)
{
    if (depth > 8) return;
#if defined(_WIN32)
    {
        char pat[1100];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof pat, "%s\\*", dir);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char full[1100];
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(full, depth + 1);
                else { SetFileAttributesA(full, FILE_ATTRIBUTE_NORMAL); DeleteFileA(full); }
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
                char full[1100];
                struct stat st;
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
                if (lstat(full, &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) remove_tree(full, depth + 1);
                else unlink(full);
            }
            closedir(d);
        }
        rmdir(dir);
    }
#endif
}

static void make_dirs_for(const char *path)
{
    char p[1200];
    size_t i;
    if ((size_t)snprintf(p, sizeof p, "%s", path) >= sizeof p)
        return;
    for (i = 1; p[i]; i++)
        if (p[i] == '/' || p[i] == '\\') {
            char c = p[i];
            p[i] = 0;
            MKDIR(p);
            p[i] = c;
        }
}

static const char *home_save_dir(void)
{
    const char *h = getenv("RDR_ONLINE_HOME_SAVE");
    if (h && *h) return h;
    h = getenv("RECOMP_SAVE_DIR");
    return h && *h ? h : "saves";
}

/* the host's game uses the 2002 preview's characters (option 11) */
static int host_preview(void)
{
    uint32_t nopt;
    int32_t v;
    if (!L.blob || L.blob_n < 8 + 12 * 4)
        return 0;
    memcpy(&nopt, L.blob + 4, 4);
    if (nopt < 12)
        return 0;
    memcpy(&v, L.blob + 8 + 11 * 4, 4);
    return v != 0;
}

/* the host's mods' fingerprint (option 12) */
static uint32_t host_mods_fp(void)
{
    uint32_t nopt, v;
    if (!L.blob || L.blob_n < 8 + 13 * 4)
        return 0;
    memcpy(&nopt, L.blob + 4, 4);
    if (nopt < 13)
        return 0;
    memcpy(&v, L.blob + 8 + 12 * 4, 4);
    return v;
}

/* the blob: "RDOB", the options, then the files */
static void make_blob(void)
{
    Buf b = { 0, 0, 0, 0 };
    int32_t o[LB_NOPTS];
    const RdrSettings *s = &g_rdr_settings;
    uint32_t count_at;
    int k = 0;
    o[k++] = s->play_as;
    o[k++] = s->coop_char[0]; o[k++] = s->coop_char[1]; o[k++] = s->coop_char[2]; o[k++] = s->coop_char[3];
    o[k++] = L.players;                 /* the lobby's players are the co-op players */
    o[k++] = s->coop_split;
    o[k++] = s->coop_friendly_fire;
    o[k++] = s->enemy_health;
    o[k++] = s->town_guns | (s->town_respawn ? 2 : 0);
    o[k++] = s->widescreen;
    o[k++] = rdr_preview_active();
    o[k++] = (int32_t)rdr_mods_fingerprint(getenv("RECOMP_MODS_DIR"));
    o[k++] = s->protect_friendly;
    o[k++] = s->coop_loadout;
    o[k++] = s->coop_duels;
    put(&b, "RDOB", 4);
    put32(&b, LB_NOPTS);
    put(&b, o, sizeof o);
    count_at = b.n;
    put32(&b, 0);
    pack_dir(&b, home_save_dir(), "UDATA", 0);
    pack_dir(&b, home_save_dir(), "TDATA", 0);
    if (!b.bad) {
        /* the file count, for the reader */
        uint32_t n = 0, p = count_at + 4;
        while (p + 2 <= b.n) {
            uint16_t pl;
            uint32_t sz;
            memcpy(&pl, b.p + p, 2);
            memcpy(&sz, b.p + p + 2 + pl, 4);
            p += 2 + pl + 4 + sz;
            n++;
        }
        memcpy(b.p + count_at, &n, 4);
    }
    free(L.blob);
    L.blob = b.bad ? NULL : b.p;
    L.blob_n = b.bad ? 0 : b.n;
    if (b.bad) free(b.p);
}

/* The host's "Start in": 1 continue the story, 2 a new game, 0 the menus
 * by hand. Only the host's game needs it: its controller works the menus,
 * and the others' games follow it frame for frame. */
static int s_start_mode = 1;
void rdr_lobby_set_start(int mode) { s_start_mode = mode < 0 ? 0 : mode > 2 ? 2 : mode; }
int rdr_lobby_get_start(void) { return s_start_mode; }

/* ── starting the game again, in lockstep ─────────────────────────────── */
#if !defined(_WIN32)
extern char **g_rdr_argv;               /* main.c */
#endif

static void env_set(const char *k, const char *v)
{
#if defined(_WIN32)
    _putenv_s(k, v ? v : "");
    SetEnvironmentVariableA(k, v && *v ? v : NULL);
#else
    if (v && *v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void relaunch(void)
{
    fprintf(stderr, "[LOBBY] starting the game again\n");
    fflush(stderr);
    fflush(stdout);
#if defined(_WIN32)
    {
        wchar_t exe[MAX_PATH];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        wchar_t *cmd = _wcsdup(GetCommandLineW());
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        ExitProcess(0);
    }
#else
    execv("/proc/self/exe", g_rdr_argv);
    _exit(1);
#endif
}

/* Writes the host's files into Saves/Online, sets the lockstep up for the
 * next start, and starts. Does not return, unless the host's save is not
 * whole (then the lobby shows why). */
static void launch(void)
{
    char online[1024], path[1200], val[160], tmp[64];
    const char *home = home_save_dir();
    const char *home_ini = getenv("RDR_ONLINE_HOME_SETTINGS");
    uint32_t p = 4, n, i, nopt;
    char opts[160] = "";

    snprintf(online, sizeof online, "%s/Online", home);
    MKDIR(online);
    snprintf(path, sizeof path, "%s/UDATA", online);
    remove_tree(path, 0);
    snprintf(path, sizeof path, "%s/TDATA", online);
    remove_tree(path, 0);
    /* The blob came over the network: every length in it is checked
     * against what arrived before it is used. */
    if (!L.blob || L.blob_n < 12 || memcmp(L.blob, "RDOB", 4) != 0) {
        say("The host's save did not arrive whole%s: try joining again.", NULL);
        lk(); L.state = 5; ulk();
        return;
    }
    memcpy(&nopt, L.blob + p, 4);
    p += 4;
    if (nopt > 64 || (uint64_t)p + (uint64_t)nopt * 4 + 4 > L.blob_n) {
        say("The host's save did not arrive whole%s: try joining again.", NULL);
        lk(); L.state = 5; ulk();
        return;
    }
    for (i = 0; i < nopt && i < 32; i++) {
        int32_t v;
        memcpy(&v, L.blob + p + i * 4, 4);
        snprintf(tmp, sizeof tmp, "%s%d", i ? "," : "", (int)v);
        strncat(opts, tmp, sizeof opts - strlen(opts) - 1);
    }
    p += nopt * 4;
    memcpy(&n, L.blob + p, 4);
    p += 4;
    for (i = 0; i < n && (uint64_t)p + 2 <= L.blob_n; i++) {
        uint16_t pl;
        uint32_t sz;
        char rel[600];
        FILE *f;
        memcpy(&pl, L.blob + p, 2);
        if (pl == 0 || pl >= sizeof rel || (uint64_t)p + 2 + pl + 4 > L.blob_n) break;
        memcpy(rel, L.blob + p + 2, pl);
        rel[pl] = 0;
        memcpy(&sz, L.blob + p + 2 + pl, 4);
        if ((uint64_t)p + 2 + pl + 4 + sz > L.blob_n) break;
        /* only a relative path inside Saves/Online: no "..", no drive
         * (':'), no leading slash, no NUL inside the name */
        if (strlen(rel) != pl || strstr(rel, "..") || strchr(rel, ':') ||
            rel[0] == '/' || rel[0] == '\\') {
            fprintf(stderr, "[LOBBY] the host's save has a file name not taken: %.60s\n", rel);
            break;
        }
        if ((size_t)snprintf(path, sizeof path, "%s/%s", online, rel) >= sizeof path) {
            fprintf(stderr, "[LOBBY] the host's save has a name too long here: %.60s\n", rel);
            break;                      /* (cut short, it would name another file) */
        }
        make_dirs_for(path);
        f = fopen(path, "wb");
        if (f) {
            fwrite(L.blob + p + 2 + pl + 4, 1, sz, f);
            fclose(f);
        }
        p += 2 + pl + 4 + sz;
    }
    /* this machine's own settings (display, controls) on a copy, with the
     * host's game options over them at the start */
    if (!home_ini || !*home_ini)
        home_ini = rdr_settings_path();
    env_set("RDR_ONLINE_HOME_SETTINGS", home_ini);
    env_set("RDR_ONLINE_HOME_SAVE", home);
    snprintf(path, sizeof path, "%s/settings.ini", online);
    {
        FILE *in = fopen(home_ini, "rb"), *out = fopen(path, "wb");
        if (in && out) {
            char b[4096];
            size_t k;
            while ((k = fread(b, 1, sizeof b, in)) > 0)
                fwrite(b, 1, k, out);
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }
    env_set("RDR_SETTINGS", path);
    env_set("RECOMP_SAVE_DIR", online);
    env_set("RDR_ONLINE_GAME", opts);
    env_set("RDR_NO_LAUNCHER", "1");
    snprintf(val, sizeof val, "%u", (unsigned)RDR_LOBBY_PORT);
    env_set("RDR_LS_PORT", val);
    snprintf(val, sizeof val, "%08X", (unsigned)L.session);
    env_set("RDR_LS_SESSION", val);
    env_set("RDR_LS_NAME", L.myname);
    if (L.me == 0) {
        snprintf(val, sizeof val, "%d", L.players);
        env_set("RDR_LS_HOST", val);
        /* where the game goes by itself once started (rdr_overrides.c,
         * online_start_step): 1 the story's Continue, 2 New, 0 the menus */
        snprintf(val, sizeof val, "%d", s_start_mode);
        env_set("RDR_ONLINE_START", s_start_mode ? val : NULL);
        env_set("RDR_LS_JOIN", NULL);
        env_set("RDR_LS_SLOT", NULL);
    } else {
        snprintf(val, sizeof val, "%u.%u.%u.%u:%u", L.host.ip[0], L.host.ip[1], L.host.ip[2], L.host.ip[3],
                 (unsigned)L.host.port);
        env_set("RDR_LS_JOIN", val);
        env_set("RDR_LS_HOST", NULL);
        snprintf(val, sizeof val, "%d", L.me);
        env_set("RDR_LS_SLOT", val);
    }
    rdr_sock_close(L.sock);
    L.sock = -1;
    relaunch();
}

/* ── the wire ─────────────────────────────────────────────────────────── */
static void send_to(const RdrAddr *to, uint8_t type, const void *body, int n)
{
    uint8_t pkt[1100];
    uint32_t m = LB_MAGIC;
    if (L.sock < 0 || n > (int)sizeof pkt - 5) return;
    memcpy(pkt, &m, 4);
    pkt[4] = type;
    if (n) memcpy(pkt + 5, body, (size_t)n);
    rdr_sock_send(L.sock, to, pkt, 5 + n);
}

static void host_roster(void)
{
    uint8_t b[1 + 4 * 34];
    int k;
    b[0] = (uint8_t)L.players;
    lk();
    for (k = 0; k < 4; k++) {
        uint8_t *e = b + 1 + k * 34;
        e[0] = (uint8_t)(k == 0 || L.peer[k].used);
        e[1] = (uint8_t)(k == 0 ? 1 : L.peer[k].ready);
        memset(e + 2, 0, 32);
        memcpy(e + 2, k == 0 ? L.myname : L.peer[k].name, 32);
        L.rpresent[k] = e[0] && k < L.players;
        L.rready[k] = k < L.players ? e[1] : 0;
        memcpy(L.rnames[k], e + 2, 32);
        L.rnames[k][32] = 0;
    }
    ulk();
    for (k = 1; k < 4; k++)
        if (L.peer[k].used)
            send_to(&L.peer[k].addr, LB_ROSTER, b, (int)sizeof b);
}

static void send_chunk(const RdrAddr *to, uint32_t c)
{
    uint8_t b[2 + LB_CHUNK];
    uint32_t off = c * LB_CHUNK, n;
    uint16_t c16 = (uint16_t)c;
    if (off >= L.blob_n) return;
    n = L.blob_n - off < LB_CHUNK ? L.blob_n - off : LB_CHUNK;
    memcpy(b, &c16, 2);
    memcpy(b + 2, L.blob + off, n);
    send_to(to, LB_FILE, b, 2 + (int)n);
}

static void host_packet(const uint8_t *b, int n, const RdrAddr *from)
{
    int k, slot = -1;
    for (k = 1; k < 4; k++)
        if (L.peer[k].used && rdr_addr_same(&L.peer[k].addr, from))
            slot = k;
    switch (b[0]) {
    case LB_HELLO: {
        uint16_t ver;
        uint8_t w[14];
        uint32_t crc, id = 0;
        if (n < 3 + 32) return;
        memcpy(&ver, b + 1, 2);
        if (ver != LB_VERSION) {
            uint8_t r = 1;
            send_to(from, LB_REFUSE, &r, 1);
            return;
        }
        if (n >= 3 + 32 + 4)
            memcpy(&id, b + 3 + 32, 4);
        if (slot < 0) {
            /* a newcomer: this game's key, or (no key) from a home network,
             * a VPN or this PC */
            uint32_t key = 0;
            int known = 0;
            if (n >= 3 + 32 + 4 + 4)
                memcpy(&key, b + 3 + 32 + 4, 4);
            for (k = 1; k < 4; k++)
                known |= id && L.peer[k].used && L.peer[k].id == id;
            if (!known && key != L.key && !(key == 0 && addr_private(from))) {
                static uint64_t window;
                static int count;
                uint64_t now = rdr_net_now_ms();
                if (now - window >= 1000) {
                    window = now;
                    count = 0;
                }
                if (++count <= 5) {
                    uint8_t r = 4;
                    send_to(from, LB_REFUSE, &r, 1);
                    fprintf(stderr, "[LOBBY] a join from %u.%u.%u.%u without this game's code: refused\n",
                            from->ip[0], from->ip[1], from->ip[2], from->ip[3]);
                }
                return;
            }
        }
        if (slot < 0 && id)                     /* the same guest by another way: */
            for (k = 1; k < 4; k++)             /* its latest is the way it took */
                if (L.peer[k].used && L.peer[k].id == id) {
                    lk();
                    L.peer[k].addr = *from;
                    ulk();
                    slot = k;
                }
        if (slot < 0) {
            for (k = 1; k < L.players && slot < 0; k++)
                if (!L.peer[k].used) {
                    lk();
                    memset(&L.peer[k], 0, sizeof L.peer[k]);
                    L.peer[k].used = 1;
                    L.peer[k].addr = *from;
                    L.peer[k].id = id;
                    memcpy(L.peer[k].name, b + 3, 32);
                    L.peer[k].name[32] = 0;
                    ulk();
                    slot = k;
                    fprintf(stderr, "[LOBBY] %s joined as player %d\n", L.peer[k].name, k + 1);
                }
            if (slot < 0) {
                send_to(from, LB_FULL, NULL, 0);
                return;
            }
        }
        L.peer[slot].last_ms = rdr_net_now_ms();
        crc = crc32_of(L.blob, L.blob_n);
        w[0] = (uint8_t)slot;
        w[1] = (uint8_t)L.players;
        memcpy(w + 2, &L.session, 4);
        memcpy(w + 6, &L.blob_n, 4);
        memcpy(w + 10, &crc, 4);
        send_to(from, LB_WELCOME, w, 14);
        host_roster();
        break;
    }
    case LB_NEED:
        if (slot > 0 && n >= 2) {
            /* what the guest asks for, at most 128 chunks (128 KB) in any
             * 100 ms (it asks for 64 every 150 ms) */
            int cnt = b[1];
            uint64_t now = rdr_net_now_ms();
            if (now - L.peer[slot].need_ms >= 100) {
                L.peer[slot].need_ms = now;
                L.peer[slot].need_sent = 0;
            }
            if (cnt > 128 - L.peer[slot].need_sent)
                cnt = 128 - L.peer[slot].need_sent;
            L.peer[slot].need_sent += cnt;
            for (k = 0; k < cnt && 2 + k * 2 + 2 <= n; k++) {
                uint16_t c;
                memcpy(&c, b + 2 + k * 2, 2);
                send_chunk(from, c);
            }
            L.peer[slot].last_ms = rdr_net_now_ms();
        }
        break;
    case LB_READY:
        if (slot > 0) {
            lk();
            L.peer[slot].ready = 1;
            ulk();
            L.peer[slot].last_ms = rdr_net_now_ms();
        }
        break;
    case LB_PING:
        if (slot > 0)
            L.peer[slot].last_ms = rdr_net_now_ms();
        break;
    case LB_MISMATCH:
        if (slot > 0 && n >= 2 && (b[1] == LB_NEEDS_PREVIEW || b[1] == LB_OTHER_MODS)) {
            int was = L.peer[slot].ready;
            lk();
            L.peer[slot].ready = b[1];
            ulk();
            L.peer[slot].last_ms = rdr_net_now_ms();
            if (was != b[1]) {
                fprintf(stderr, "[LOBBY] %s %s\n", L.peer[slot].name, b[1] == LB_NEEDS_PREVIEW
                        ? "does not have the 2002 preview characters" : "has different mods");
                host_roster();
            }
        }
        break;
    case LB_BYE:
        if (slot > 0) {
            lk();
            fprintf(stderr, "[LOBBY] %s left\n", L.peer[slot].name);
            memset(&L.peer[slot], 0, sizeof L.peer[slot]);
            ulk();
            host_roster();
        }
        break;
    }
}

static int guest_missing(uint16_t *out, int max)
{
    uint32_t chunks = (L.blob_n + LB_CHUNK - 1) / LB_CHUNK, c;
    int n = 0;
    for (c = 0; c < chunks && n < max; c++)
        if (!L.have[c])
            out[n++] = (uint16_t)c;
    return n;
}

static void guest_packet(const uint8_t *b, int n, const RdrAddr *from)
{
    int k;
    if (!L.welcomed && L.ncand > 1) {       /* the first of the code's addresses to answer */
        for (k = 0; k < L.ncand; k++)
            if (rdr_addr_same(from, &L.cand[k]))
                break;
        if (k == L.ncand || b[0] != LB_WELCOME)
            return;
        L.host = L.cand[k];
        L.ncand = 1;
        snprintf(L.host_text, sizeof L.host_text, "%u.%u.%u.%u", L.host.ip[0], L.host.ip[1],
                 L.host.ip[2], L.host.ip[3]);
        fprintf(stderr, "[LOBBY] the host answered at %s\n", L.host_text);
    }
    if (!rdr_addr_same(from, &L.host))
        return;
    L.heard_ms = rdr_net_now_ms();
    switch (b[0]) {
    case LB_WELCOME:
        if (n >= 15 && !L.welcomed) {
            uint32_t size;
            if (b[1] < 1 || b[1] > 3 || b[2] < 2 || b[2] > 4 || b[1] >= b[2])
                return;                 /* not a welcome this port sends */
            L.me = b[1];
            L.players = b[2];
            memcpy(&L.session, b + 3, 4);
            memcpy(&size, b + 7, 4);
            memcpy(&L.want_crc, b + 11, 4);
            if (size == 0 || size > LB_MAXBLOB) {
                say("The host has no save to share%s", NULL);
                return;
            }
            free(L.blob);
            free(L.have);
            L.blob = (uint8_t *)calloc(1, size);
            L.blob_n = size;
            L.have = (uint8_t *)calloc(1, (size + LB_CHUNK - 1) / LB_CHUNK);
            if (!L.blob || !L.have) {
                free(L.blob); free(L.have);
                L.blob = NULL; L.have = NULL; L.blob_n = 0;
                say("Not enough memory for the host's save%s.", NULL);
                return;
            }
            L.welcomed = 1;
            lk();
            L.state = 3;
            ulk();
            {
                char t[64];
                snprintf(t, sizeof t, "player %d of %d", L.me + 1, L.players);
                say("Joined as %s: getting the host's save...", t);
            }
        }
        break;
    case LB_FULL:
        say("That game is full%s.", NULL);
        break;
    case LB_REFUSE:
        if (n >= 2 && b[1] == 4)
            say("The host's game did not take that%s: type its join code exactly (an address alone "
                "works at home or over a VPN, not over the internet).", NULL);
        else
            say("The host plays another version of the port%s: both need the same build.", NULL);
        lk(); L.state = 5; ulk();
        break;
    case LB_FILE:
        if (L.welcomed && n >= 3) {
            uint16_t c;
            uint32_t off;
            memcpy(&c, b + 1, 2);
            off = (uint32_t)c * LB_CHUNK;
            if (off < L.blob_n && off + (uint32_t)(n - 3) <= L.blob_n) {
                memcpy(L.blob + off, b + 3, (size_t)(n - 3));
                L.have[c] = 1;
            }
        }
        break;
    case LB_ROSTER:
        if (n >= 2 + 4 * 34) {
            lk();
            L.players = b[1] < 2 ? 2 : b[1] > 4 ? 4 : b[1];
            for (k = 0; k < 4; k++) {
                const uint8_t *e = b + 2 + k * 34;
                L.rpresent[k] = e[0] && k < L.players;
                L.rready[k] = k < L.players ? e[1] : 0;
                memcpy(L.rnames[k], e + 2, 32);
                L.rnames[k][32] = 0;
            }
            ulk();
        }
        break;
    case LB_START:
        if (L.welcomed && L.complete) {
            lk(); L.state = 4; ulk();
            say("Starting%s...", NULL);
            launch();
        }
        break;
    case LB_BYE:
        say("The host closed the game%s.", NULL);
        lk(); L.state = 5; ulk();
        break;
    }
}

#if defined(_WIN32)
static DWORD WINAPI lobby_thread(LPVOID arg)
#else
static void *lobby_thread(void *arg)
#endif
{
    uint64_t last_roster = 0, last_hello = 0, last_need = 0;
    int hello_after = 0;
    (void)arg;
    if (L.me == 0) {
        /* the code: this PC's addresses, and the internet's view of it --
         * home networks, then a VPN's; the rest (virtual adapters' mostly:
         * Hyper-V, WSL) only when there is neither; three at most */
        uint8_t ips[8][4], pick[4][4];
        RdrAddr pub;
        int n = rdr_net_local_ips(ips, 8), k, c = 0, pass, net = 0, lan = 0, vpn = 0;
        if (g_rdr_settings.online_share_ip
            && (rdr_net_stun(L.sock, "stun.l.google.com:19302", &pub, 1500) == 0
                || rdr_net_stun(L.sock, "stun.cloudflare.com:3478", &pub, 1500) == 0)) {
            memcpy(pick[c++], pub.ip, 4);
            net = 1;
        }
        for (pass = 0; pass < 3; pass++)
            for (k = 0; k < n && c < 3 && !(pass == 2 && (lan || vpn)); k++) {
                int kind = ip_kind(ips[k]), j, dup = 0;
                int want = pass == 0 ? (kind == 0 || kind == 1)
                         : pass == 1 ? kind == 2 : !(kind == 0 || kind == 1 || kind == 2);
                if (!want || ips[k][0] == 127 || (ips[k][0] == 169 && ips[k][1] == 254))
                    continue;
                for (j = 0; j < c; j++)
                    dup |= !memcmp(pick[j], ips[k], 4);
                if (dup)
                    continue;
                memcpy(pick[c++], ips[k], 4);
                if (kind == 2) vpn = 1; else lan = 1;
            }
        lk();
        if (c) {
            code_make_multi((const uint8_t (*)[4])pick, c, L.key, L.codes[0], (int)sizeof L.codes[0]);
            snprintf(L.where[0], sizeof L.where[0], "Join code");
            snprintf(L.reach, sizeof L.reach, "Works %s%s%s%s.",
                     lan ? "on your home network" : "",
                     vpn ? (lan ? (net ? ", over a VPN" : " and over a VPN") : "over a VPN") : "",
                     net ? (lan || vpn ? " and " : "") : "",
                     net ? "over the internet when your router forwards UDP port 27032" : "");
        }
        L.ncodes = c ? 1 : 0;
        ulk();
        for (k = 0; k < c; k++)
            fprintf(stderr, "[LOBBY] code address %u.%u.%u.%u\n", pick[k][0], pick[k][1], pick[k][2], pick[k][3]);
        if (c)
            fprintf(stderr, "[LOBBY] join code %s\n", L.codes[0]);
        say(c ? "Give your friends the join code%s; they type it under Join a Game."
              : "This PC has no network address%s.", NULL);
    }
    while (!L.quit) {
        uint8_t buf[1200];
        RdrAddr from;
        int n;
        uint64_t now = rdr_net_now_ms();
        uint32_t m;
        while ((n = rdr_sock_recv(L.sock, &from, buf, sizeof buf)) > 0) {
            if (n < 5 || (memcpy(&m, buf, 4), m != LB_MAGIC))
                continue;
            if (L.me == 0) host_packet(buf + 4, n - 4, &from);
            else guest_packet(buf + 4, n - 4, &from);
        }
        if (L.me == 0) {
            int k;
            if (now - last_roster >= 500) {
                for (k = 1; k < 4; k++)
                    if (L.peer[k].used && now - L.peer[k].last_ms > 6000) {
                        lk();
                        fprintf(stderr, "[LOBBY] %s timed out\n", L.peer[k].name);
                        memset(&L.peer[k], 0, sizeof L.peer[k]);
                        ulk();
                    }
                host_roster();
                last_roster = now;
            }
            if (L.start_req) {
                uint32_t s = L.session;
                int r;
                lk(); L.state = 4; ulk();
                say("Starting%s...", NULL);
                for (r = 0; r < 15; r++) {
                    for (k = 1; k < 4; k++)
                        if (L.peer[k].used)
                            send_to(&L.peer[k].addr, LB_START, &s, 4);
                    rdr_net_sleep_ms(20);
                }
                launch();
            }
        } else {
            if ((!L.welcomed && now - last_hello >= 500) || (L.welcomed && !hello_after)) {
                uint8_t h[2 + 32 + 4 + 4];
                uint16_t v = LB_VERSION;
                int k;
                memcpy(h, &v, 2);
                memcpy(h + 2, L.myname, 32);
                memcpy(h + 34, &L.myid, 4);
                memcpy(h + 38, &L.key, 4);
                if (L.welcomed) {               /* once more, the chosen way only */
                    send_to(&L.host, LB_HELLO, h, (int)sizeof h);
                    hello_after = 1;
                } else
                    for (k = 0; k < L.ncand; k++)
                        send_to(&L.cand[k], LB_HELLO, h, (int)sizeof h);
                last_hello = now;
                if (now - L.began_ms > 8000 && !L.heard_ms)
                    say("No answer from the host yet (%s): check the code; over the internet the host's "
                        "router has to forward UDP port 27032.", L.host_text);
            }
            if (L.welcomed && now - last_need >= 150) {
                uint16_t miss[64];
                int k = guest_missing(miss, 64);
                if (k) {
                    uint8_t b[1 + 128];
                    b[0] = (uint8_t)k;
                    memcpy(b + 1, miss, (size_t)k * 2);
                    send_to(&L.host, LB_NEED, b, 1 + k * 2);
                } else if (L.complete || crc32_of(L.blob, L.blob_n) == L.want_crc) {
                    /* the same game files as the host's: the 2002 preview's
                     * characters and every mod (all run the whole game) */
                    int why;
                    if (!L.checked) {
                        L.checked = 1;
                        L.mismatch = 0;
                        why = 0;
                        if (host_preview() && !rdr_preview_ready())
                            why = LB_NEEDS_PREVIEW;
                        else if (host_mods_fp() != rdr_mods_fingerprint_for(&g_rdr_settings, host_preview()))
                            why = LB_OTHER_MODS;
                        if (why == LB_NEEDS_PREVIEW)
                            say("The host plays with the 2002 preview characters and this PC does not have "
                                "them. Set them up (the launcher's Extras page), then join again%s.", NULL);
                        else if (why == LB_OTHER_MODS)
                            say(host_preview()
                                ? "Your 2002 preview characters or mods are not the same as the host's. Set the "
                                  "preview up again and switch on the same mods as the host (the launcher's "
                                  "Mods and Extras pages), then join again%s."
                                : "Your mods are not the same as the host's. Switch on the same mods as the "
                                  "host (the launcher's Mods page), then join again%s.", NULL);
                        fprintf(stderr, "[LOBBY] mods: host %08X, here %08X (preview %s)\n", host_mods_fp(),
                                rdr_mods_fingerprint_for(&g_rdr_settings, host_preview()),
                                host_preview() ? "on" : "off");
                        L.mismatch = why;
                    }
                    why = L.mismatch;
                    if (why) {
                        uint8_t r = (uint8_t)why;
                        send_to(&L.host, LB_MISMATCH, &r, 1);
                        L.complete = 1;
                    } else
                        send_to(&L.host, LB_READY, NULL, 0);
                    if (!L.complete && !why)
                        say("Ready: waiting for the host to start%s.", NULL);
                    L.complete = 1;
                } else {
                    memset(L.have, 0, (L.blob_n + LB_CHUNK - 1) / LB_CHUNK);   /* again */
                }
                send_to(&L.host, LB_PING, NULL, 0);
                last_need = now;
            }
            if (L.heard_ms && now - L.heard_ms > 8000 && L.state != 5) {
                say("Lost the host%s.", NULL);
                lk(); L.state = 5; ulk();
            }
        }
        rdr_net_sleep_ms(5);
    }
    L.running = 0;
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void begin(void)
{
#if defined(_WIN32)
    CreateThread(NULL, 0, lobby_thread, NULL, 0, NULL);
#else
    pthread_t t;
    pthread_create(&t, NULL, lobby_thread, NULL);
    pthread_detach(t);
#endif
}

/* ── the page's calls ─────────────────────────────────────────────────── */
void rdr_lobby_leave(void)
{
    int k;
    if (L.sock >= 0) {
        if (L.me == 0) {
            for (k = 1; k < 4; k++)
                if (L.peer[k].used) send_to(&L.peer[k].addr, LB_BYE, NULL, 0);
        } else if (L.state >= 2) {
            send_to(&L.host, LB_BYE, NULL, 0);
        }
    }
    L.quit = 1;
    while (L.running)
        rdr_net_sleep_ms(5);
    rdr_sock_close(L.sock);
    L.sock = -1;
    lk();
    L.state = 0;
    L.ncodes = 0;
    L.status[0] = 0;
    memset(L.rpresent, 0, sizeof L.rpresent);
    memset(L.rready, 0, sizeof L.rready);
    ulk();
    memset(L.peer, 0, sizeof L.peer);
}

static void reset_for(int me, const char *name)
{
    rdr_lobby_leave();
    L.quit = 0;
    L.me = me;
    L.start_req = 0;
    L.welcomed = 0;
    L.complete = 0;
    L.checked = 0;
    L.mismatch = 0;
    L.heard_ms = 0;
    L.began_ms = rdr_net_now_ms();
    L.ncand = 0;
    L.ncodes = 0;
    L.reach[0] = 0;
    L.myid = random_u32() | 1u;
    L.key = 0;
    memset(L.myname, 0, sizeof L.myname);
    snprintf(L.myname, sizeof L.myname, "%s", name && *name ? name : (me ? "Guest" : "Host"));
}

void rdr_lobby_host(int players, const char *name)
{
    reset_for(0, name);
    L.players = players < 2 ? 2 : players > 4 ? 4 : players;
    L.session = random_u32();
    L.key = random_u32() & ((1u << LB_KEY_BITS) - 1u);
    if (!L.key)
        L.key = 1;                      /* (0 is "no key") */
    make_blob();
    if (!L.blob) {
        say("Could not read the save%s.", NULL);
        L.state = 5;
        return;
    }
    L.sock = rdr_sock_open(RDR_LOBBY_PORT);
    if (L.sock < 0) {
        say("UDP port 27032 is in use%s: is another copy of the game hosting?", NULL);
        L.state = 5;
        return;
    }
    L.state = 1;
    say("Finding this PC's addresses%s...", NULL);
    L.running = 1;
    host_roster();
    begin();
}

void rdr_lobby_set_players(int players)
{
    int k;
    if (L.state != 1) return;
    L.players = players < 2 ? 2 : players > 4 ? 4 : players;
    lk();
    for (k = L.players; k < 4; k++)
        if (L.peer[k].used) {
            send_to(&L.peer[k].addr, LB_BYE, NULL, 0);
            memset(&L.peer[k], 0, sizeof L.peer[k]);
        }
    ulk();
    make_blob();                        /* the players are in the options */
    for (k = 1; k < 4; k++)
        L.peer[k].ready = 0;            /* fetch it again */
    host_roster();
}

void rdr_lobby_join(const char *text, const char *name)
{
    char t[96];
    reset_for(1, name);
    snprintf(t, sizeof t, "%s", text ? text : "");
    while (*t && (t[strlen(t) - 1] == ' ' || t[strlen(t) - 1] == '\n')) t[strlen(t) - 1] = 0;
    L.ncand = code_read(t, L.cand, 4, &L.key);
    if (L.ncand <= 0) {
        char a[100];
        snprintf(a, sizeof a, "%s%s", t, strchr(t, ':') ? "" : ":27032");
        if (rdr_addr_parse(a, &L.cand[0]) != 0) {
            L.ncand = 0;
            say("\"%s\" is not a join code or an address.", t);
            L.state = 5;
            return;
        }
        L.ncand = 1;
    }
    L.host = L.cand[0];
    if (L.ncand == 1)
        snprintf(L.host_text, sizeof L.host_text, "%u.%u.%u.%u", L.host.ip[0], L.host.ip[1], L.host.ip[2],
                 L.host.ip[3]);
    else {
        int k, o = 0;
        for (k = 0; k < L.ncand && o < (int)sizeof L.host_text - 20; k++)
            o += snprintf(L.host_text + o, sizeof L.host_text - o, "%s%u.%u.%u.%u", k ? ", " : "",
                          L.cand[k].ip[0], L.cand[k].ip[1], L.cand[k].ip[2], L.cand[k].ip[3]);
    }
    L.sock = rdr_sock_open(0);
    if (L.sock < 0) {
        say("Could not open a network socket%s.", NULL);
        L.state = 5;
        return;
    }
    L.state = 2;
    say("Calling %s...", L.host_text);
    L.running = 1;
    begin();
}

int rdr_lobby_can_start(void)
{
    int k, ok = L.state == 1;
    for (k = 1; k < L.players && ok; k++)
        ok = L.peer[k].used && L.peer[k].ready == 1;
    return ok;
}

void rdr_lobby_start(void)
{
    if (rdr_lobby_can_start())
        L.start_req = 1;
}

/* Hosting, joining or about to start (the demo is held off meanwhile). */
int rdr_lobby_busy(void)
{
    return L.state >= 1 && L.state <= 4;
}

void rdr_lobby_view(RdrLobbyView *v)
{
    int k;
    lk();
    v->state = L.state;
    v->players = L.players;
    v->me = L.me;
    snprintf(v->status, sizeof v->status, "%s", L.status);
    v->ncodes = L.ncodes;
    for (k = 0; k < L.ncodes && k < 6; k++) {
        snprintf(v->codes[k], sizeof v->codes[k], "%s", L.codes[k]);
        snprintf(v->where[k], sizeof v->where[k], "%s", L.where[k]);
    }
    snprintf(v->reach, sizeof v->reach, "%s", L.reach);
    for (k = 0; k < 4; k++) {
        snprintf(v->names[k], sizeof v->names[k], "%s", L.rnames[k]);
        v->present[k] = L.rpresent[k];
        v->ready[k] = L.rready[k];
    }
    ulk();
}

/* ── in the game started from the lobby ───────────────────────────────── */
int rdr_online_session(void)
{
    const char *h = getenv("RDR_LS_HOST"), *j = getenv("RDR_LS_JOIN"), *s = getenv("RDR_LS_SESSION");
    return s && *s && ((h && *h && *h != '0') || (j && *j));
}

/* the host's game options over this machine's settings (main.c, at the
 * start, after the settings file is read) */
void rdr_online_apply_settings(void *settings)
{
    RdrSettings *s = (RdrSettings *)settings;
    const char *e = getenv("RDR_ONLINE_GAME");
    int o[LB_NOPTS], n = 0;
    if (!e || !*e || !rdr_online_session())
        return;
    while (n < LB_NOPTS && *e) {
        o[n++] = atoi(e);
        e = strchr(e, ',');
        if (!e) break;
        e++;
    }
    if (n < LB_NOPTS)
        return;
    s->play_as = o[0];
    s->coop_char[0] = o[1]; s->coop_char[1] = o[2]; s->coop_char[2] = o[3]; s->coop_char[3] = o[4];
    s->coop_players = o[5];
    s->coop_split = o[6];
    s->coop_friendly_fire = o[7];
    s->enemy_health = o[8];
    s->town_guns = o[9] & 1;
    s->town_respawn = (o[9] >> 1) & 1;
    s->widescreen = o[10];
    s->preview = o[11];
    s->protect_friendly = o[13];
    s->coop_loadout = o[14];
    s->coop_duels = o[15];
    s->play_as_code[0] = 0;             /* the host's choices, not a saved one */
    for (n = 0; n < 4; n++)
        s->coop_code[n][0] = 0;
    fprintf(stderr, "[LOBBY] online game: %d players, the host's options\n", o[5]);
}

/* back to this machine's own game: its own saves and settings */
void rdr_online_leave_session(void)
{
    const char *hs = getenv("RDR_ONLINE_HOME_SAVE"), *hi = getenv("RDR_ONLINE_HOME_SETTINGS");
    char save[1024], ini[1024];
    snprintf(save, sizeof save, "%s", hs ? hs : "");
    snprintf(ini, sizeof ini, "%s", hi ? hi : "");
    env_set("RECOMP_SAVE_DIR", save[0] ? save : NULL);
    env_set("RDR_SETTINGS", ini[0] ? ini : NULL);
    env_set("RDR_ONLINE_HOME_SAVE", NULL);
    env_set("RDR_ONLINE_HOME_SETTINGS", NULL);
    env_set("RDR_ONLINE_GAME", NULL);
    env_set("RDR_ONLINE_START", NULL);
    env_set("RDR_LS_HOST", NULL);
    env_set("RDR_LS_JOIN", NULL);
    env_set("RDR_LS_SLOT", NULL);
    env_set("RDR_LS_SESSION", NULL);
    env_set("RDR_LS_NAME", NULL);
    env_set("RDR_LS_PORT", NULL);
    relaunch();
}
