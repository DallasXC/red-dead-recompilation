/*
 * rdr_cheats.c - the pause menu's cheats, by the bits the game uses.
 *
 * Found from the pause menu's Cheats page (sub_00030E40 builds it from the
 * unlockables "cheat_invulnerable" ... "cheat_bigmoney"; sub_00030D40 routes
 * a toggle to one of nine small handlers at 0x000309C0-0x00030B40). Each
 * handler calls sub_00056BA0(player, bit, on), which sets or clears
 * 1 << bit in a word at +0x44 of:
 *
 *   single player:  profile slot [0x00657798], 0xB18 bytes each from
 *                   0x00653CC4 (sub_0005AA30)
 *   multiplayer ([0x00652E30] != 0): *(0x00652EE8 + player * 0x114)
 *                   (sub_000580E0), player = [0x00652024]
 *
 * The game reads the bits where it needs them (damage, the HUD, the film
 * grain), so a change takes effect at once, as from the menu.
 */
#if defined(_MSC_VER)
#include <intrin.h>             /* _ReadWriteBarrier */
#endif
#include "rdr_cheats.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rdr_settings.h"
#include "input/xinput_xbox.h"   /* xbox_InputPortName */
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define RDR_MULTIPLAYER    0x00652E30u
#define RDR_PLAYER_INDEX   0x00652024u
#define RDR_PROFILE_SLOT   0x00657798u
#define RDR_PROFILES       0x00653CC4u
#define RDR_PROFILE_SIZE   0x00000B18u
#define RDR_MP_PLAYERS     0x00652EE8u
#define RDR_MP_STRIDE      0x00000114u
#define RDR_CHEAT_WORD     0x44u

const RdrCheat g_rdr_cheats[] = {
    {  6, "Invincibility",     "Your health never drops." },
    { 10, "Infinite Dead Eye", "The Dead Eye meter never runs down." },
    {  7, "Golden Gun",        "Your shots hit far harder; theirs do not." },
    { 12, "Bite the Bullet",   "Every shot hits far harder - yours and theirs." },
    { 13, "Sad Stars",         "Sad-faced stars instead of blood." },
    { 14, "No HUD",            "Hides health, ammo, money and the crosshair." },
    { 15, "No Distortion",     "Turns off the old-film grain and blur." },
};
const int g_rdr_cheat_count = (int)(sizeof g_rdr_cheats / sizeof g_rdr_cheats[0]);

static volatile uint32_t *g32(uint32_t va)
{
    return (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

static uint32_t word_va(void)
{
    if (*g32(RDR_MULTIPLAYER) == 0) {
        uint32_t slot = *g32(RDR_PROFILE_SLOT);
        if (slot > 16)
            return 0;
        return RDR_PROFILES + slot * RDR_PROFILE_SIZE + RDR_CHEAT_WORD;
    } else {
        uint32_t player = *g32(RDR_PLAYER_INDEX), p;
        if (player > 8)
            return 0;
        p = *g32(RDR_MP_PLAYERS + player * RDR_MP_STRIDE);
        if (p < 0x00010000u || p >= 0x04000000u)
            return 0;
        return p + RDR_CHEAT_WORD;
    }
}

int rdr_cheats_available(void)
{
    return word_va() != 0;
}

int rdr_cheat_get(int bit)
{
    uint32_t va = word_va();
    if (!va || bit < 0 || bit > 15)
        return 0;
    return (*g32(va) >> bit) & 1;
}

void rdr_cheat_set(int bit, int on)
{
    uint32_t va = word_va();
    volatile uint32_t *w;
    if (!va || bit < 0 || bit > 15)
        return;
    w = g32(va);
    if (on)
        *w |= 1u << bit;
    else
        *w &= ~(1u << bit);
}

/* ---- Always Excellent ------------------------------------------------
 *
 * The results screen (sub_00065A60) rates a mission on four counts against
 * the level's record, [0x006577A0 + level * 4] (40 slots; the current level
 * from sub_000567D0):
 *   +0x14  time taken      <= target (seconds)
 *   +0x18  damage taken    <= target
 *   +0x1C  accuracy        >= target (percent)
 *   +0x20  combo score     >= target
 * Three or more is "Great" -- Excellent on screen -- and unlocks that
 * rank's reward; all four with the bounty met also unlock the bounty
 * reward. Opening the targets wide makes every count pass. */
#define RDR_LEVELS        0x006577A0u
#define RDR_LEVEL_COUNT   40

int g_rdr_always_excellent;

static struct { uint32_t rec; uint32_t v[4]; } s_saved[RDR_LEVEL_COUNT];
static int s_applied;

static int plausible(uint32_t va)
{
    return (va >= 0x00010000u && va < 0x04000000u) || (va >= 0x80000000u && va < 0x84000000u);
}

static void excellent_apply(int on)
{
    static int last = -1;
    int i, n = 0;
    for (i = 0; i < RDR_LEVEL_COUNT; i++) {
        uint32_t rec = *g32(RDR_LEVELS + i * 4u);
        /* A record: a name pointer, then the mode (sub_000567D0 matches
         * both); anything else is not touched. */
        if (!plausible(rec) || !plausible(*g32(rec)) || *g32(rec + 4) > 16)
            continue;
        if (on) {
            if (s_saved[i].rec != rec) {
                s_saved[i].rec = rec;
                s_saved[i].v[0] = *g32(rec + 0x14);
                s_saved[i].v[1] = *g32(rec + 0x18);
                s_saved[i].v[2] = *g32(rec + 0x1C);
                s_saved[i].v[3] = *g32(rec + 0x20);
            }
            *g32(rec + 0x14) = 0x7FFFFFFFu;     /* any time */
            *g32(rec + 0x18) = 0x7FFFFFFFu;     /* any damage */
            *g32(rec + 0x1C) = 0x80000000u;     /* any accuracy */
            *g32(rec + 0x20) = 0x80000000u;     /* any combo */
            n++;
        } else if (s_saved[i].rec == rec) {
            *g32(rec + 0x14) = s_saved[i].v[0];
            *g32(rec + 0x18) = s_saved[i].v[1];
            *g32(rec + 0x1C) = s_saved[i].v[2];
            *g32(rec + 0x20) = s_saved[i].v[3];
            s_saved[i].rec = 0;
            n++;
        }
    }
    if (on && n != last) {
        fprintf(stderr, "[CHEATS] Always Excellent: %d level record%s opened\n", n, n == 1 ? "" : "s");
        fflush(stderr);
    } else if (!on && n) {
        fprintf(stderr, "[CHEATS] Always Excellent off: %d level record%s restored\n", n, n == 1 ? "" : "s");
        fflush(stderr);
    }
    last = on ? n : -1;
}

/* ---- Infinite Ammo ---------------------------------------------------
 *
 * Found from the AI script conditions "loadedbullets" (sub_00232A40) and
 * "ammoleft" (sub_0015C3A0), which read a character's ammo:
 *   character        = [player + 0x0C]
 *   weapon list      = [character + 0x80]; current index at +0x48
 *   [list + 0x20]    = { item* array, u16 count, u16 capacity,
 *                        valid-flag bytes* }
 *   item             = two guns (both hands): gun* at +0, +4
 *   gun + 0x24       = rounds loaded (-1: never runs out)
 *   gun + 0x28       = the ammo pool it reloads from; pool + 8 = rounds
 *                      left (-1: endless). The HUD shows loaded + left.
 * While on, each gun's clip and each pool is held at the most it has been
 * seen holding -- a full clip and the reserve as it was when switched on
 * (or as large as a pickup made it). */
int g_rdr_infinite_ammo;

#define AMMO_TRACK 48
static struct { uint32_t va, most; } s_ammo[AMMO_TRACK];

static void ammo_keep(uint32_t va)
{
    int i, free_i = -1;
    int32_t now = (int32_t)*g32(va);
    if (now < 0)
        return;                             /* already endless */
    for (i = 0; i < AMMO_TRACK; i++) {
        if (s_ammo[i].va == va) {
            if ((uint32_t)now > s_ammo[i].most)
                s_ammo[i].most = (uint32_t)now;
            else if ((uint32_t)now < s_ammo[i].most)
                *g32(va) = s_ammo[i].most;
            return;
        }
        if (!s_ammo[i].va && free_i < 0)
            free_i = i;
    }
    if (free_i >= 0 && now > 0) {
        s_ammo[free_i].va = va;
        s_ammo[free_i].most = (uint32_t)now;
    }
}

static uint32_t player_va(void);

static void ammo_apply(void)
{
    uint32_t p = player_va(), ch, wl, tab, arr, valid, n, i, k;
    if (!p || *g32(RDR_MULTIPLAYER) != 0)
        return;
    ch = *g32(p + 0x0C);
    if (!plausible(ch)) return;
    wl = *g32(ch + 0x80);
    if (!plausible(wl)) return;
    {
        /* A new character (a new level or a restart): new guns, perhaps at
         * addresses the old ones had -- start counting afresh. */
        static uint32_t last_wl;
        if (wl != last_wl) {
            memset(s_ammo, 0, sizeof s_ammo);
            last_wl = wl;
        }
    }
    tab = *g32(wl + 0x20);
    if (!plausible(tab)) return;
    arr = *g32(tab);
    n = *g32(tab + 4) & 0xFFFFu;            /* u16 count, u16 capacity */
    valid = *g32(tab + 8);
    if (!plausible(arr) || !plausible(valid) || n > 256)
        return;
    for (i = 0; i < n; i++) {
        uint32_t item;
        if (!*(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + valid + i))
            continue;
        item = *g32(arr + i * 4);
        if (!plausible(item))
            continue;
        for (k = 0; k < 2; k++) {
            uint32_t gun = *g32(item + k * 4), pool;
            if (!plausible(gun))
                continue;
            ammo_keep(gun + 0x24);
            pool = *g32(gun + 0x28);
            if (plausible(pool))
                ammo_keep(pool + 8);
        }
    }
}

/* Tests (RDR_OVERLAY_FLAG "drain" / "report"): the first gun the player
 * carries -- down to 1 loaded and 10 left, as firing would leave it, or
 * its counts logged. */
static uint32_t test_gun(uint32_t *slot)
{
    uint32_t p = player_va(), ch, wl, tab, arr, valid, n, i, item, gun;
    if (!p) return 0;
    ch = *g32(p + 0x0C); wl = plausible(ch) ? *g32(ch + 0x80) : 0;
    tab = plausible(wl) ? *g32(wl + 0x20) : 0;
    if (!plausible(tab)) {
        fprintf(stderr, "[TEST] ammo: player %08X character %08X list %08X table %08X\n", p, ch, wl, tab);
        return 0;
    }
    arr = *g32(tab); n = *g32(tab + 4) & 0xFFFFu; valid = *g32(tab + 8);
    for (i = 0; plausible(arr) && plausible(valid) && i < n && i < 256; i++) {
        if (!*(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + valid + i))
            continue;
        item = *g32(arr + i * 4);
        if (!plausible(item))
            continue;
        gun = *g32(item);                   /* right hand, else left */
        if (!plausible(gun))
            gun = *g32(item + 4);
        if (plausible(gun) && plausible(*g32(gun + 0x28))) {
            *slot = i;
            return gun;
        }
    }
    fprintf(stderr, "[TEST] ammo: no gun in %u slots (current %d)\n", n, (int)*g32(wl + 0x48));
    return 0;
}

void rdr_ammo_test_drain(void)
{
    uint32_t slot = 0, gun = test_gun(&slot), pool;
    if (!gun) return;
    pool = *g32(gun + 0x28);
    fprintf(stderr, "[TEST] drain: slot %u loaded %d left %d -> 1, 10\n",
            slot, (int)*g32(gun + 0x24), (int)*g32(pool + 8));
    *g32(gun + 0x24) = 1;
    *g32(pool + 8) = 10;
}

void rdr_ammo_test_report(void)
{
    uint32_t slot = 0, gun = test_gun(&slot), pool;
    if (!gun) return;
    pool = *g32(gun + 0x28);
    fprintf(stderr, "[TEST] ammo now: slot %u loaded %d left %d\n",
            slot, (int)*g32(gun + 0x24), (int)*g32(pool + 8));
}

/* ---- Go to level ------------------------------------------------------
 *
 * The level to load is a name at 0x00652E70 (64 bytes, lower case) and a
 * number at [0x00652E60] -- the story chapter's variation (lvl_tomb 2), or
 * in Showdown the player count's digit -- written by sub_00056550 when a
 * menu starts a level. sub_0005FF90 builds the archive from them: the
 * name after "lvl_" cut to 8 characters, the number's digit in the 8th
 * place when it is above 1, under lvls_sng\ (or lvls_m2\ / lvls_m4\ in
 * Showdown, [0x00652E30] == 1). In a Showdown level the name already
 * carries the player count's digit (lvl_perf2) and the number is 1. The
 * pause menu's Restart Level sets the HUD menu's flag at 0x00652014
 * (0x00651ED4 + 0x140); the game object (0x0065A908, sub_00070990) looks
 * at it as it leaves the pause ([+0x2C] set, the pause menu [0x00651FF4]
 * closed). NOT WORKING YET (test command "level" only, no menu row):
 * that path (sub_0005EA50) only reloads the archive when the level flow's
 * level ([0x0065AA08], sub_0007BA90) differs from the one loaded
 * ([0x0065AA0C]+0x14); otherwise -- and always in Showdown -- it is a soft
 * restart in place, so the new name is not read. The reload request
 * itself is [[0x0065A6A4]+0x10], seen by sub_0006C8D0 only while no child
 * mode runs. */
#define RDR_NEXT_LEVEL   0x00652E70u
#define RDR_NEXT_VAR     0x00652E60u
#define RDR_SHOWDOWN     0x00652E30u
#define RDR_RESET_FLAG   0x00652014u
#define RDR_GAME_PAUSED  (0x0065A908u + 0x2C)
#define RDR_PAUSE_MENU    0x00651FF4u

/* Asked for on the menu's thread, done on the game's (rdr_cheats_tick):
 * the name and variant first, then pending. */
static char s_go_name[64];
static int  s_go_var;
static volatile int s_go_pending;

int rdr_level_kind(void)
{
    if (!rdr_in_level()) return 0;
    return *g32(RDR_SHOWDOWN) == 1 ? 2 : 1;
}

int rdr_level_current(char *name, int n, int *var)
{
    const volatile char *p;
    int i;
    if (!rdr_in_level() || n <= 0) return 0;
    p = (const volatile char *)((uintptr_t)xbox_GetMemoryOffset() + RDR_NEXT_LEVEL);
    for (i = 0; i < n - 1 && i < 63 && p[i]; i++)
        name[i] = p[i];
    name[i] = 0;
    /* Showdown: lvl_perf2 is the stage lvl_perf */
    if (*g32(RDR_SHOWDOWN) == 1 && i > 4 && name[i - 1] >= '0' && name[i - 1] <= '9')
        name[i - 1] = 0;
    if (var) *var = (int)*g32(RDR_NEXT_VAR);
    return 1;
}

static const struct { const char *name; int var; const char *label; } s_story[] = {
    { "lvl_perfnew", 1, "Chapter 1 (lvl_perfnew)" },     { "lvl_hng", 1, "Chapter 2 (lvl_hng)" },
    { "lvl_sher70", 1, "Chapter 3 (lvl_sher70)" },       { "lvl_traincar", 1, "Chapter 4 (lvl_traincar)" },
    { "lvl_traingood", 1, "Chapter 5 (lvl_traingood)" }, { "lvl_tomb", 1, "Bounty Hunter 1 (lvl_tomb)" },
    { "lvl_ghost", 1, "Chapter 6 (lvl_ghost)" },         { "lvl_shernite", 1, "Chapter 7 (lvl_shernite)" },
    { "lvl_tomb", 2, "Bounty Hunter 2 (lvl_tomb 2)" },   { "lvl_bomb", 1, "Chapter 8 (lvl_bomb)" },
    { "lvl_tomb", 3, "Bounty Hunter 3 (lvl_tomb 3)" },   { "lvl_gunsmoke", 1, "Chapter 9 (lvl_gunsmoke)" },
    { "lvl_cemetery", 1, "Chapter 10 (lvl_cemetery)" },  { "lvl_tomb", 4, "Bounty Hunter 4 (lvl_tomb 4)" },
    { "lvl_ranch", 1, "Chapter 11 (lvl_ranch)" },        { "lvl_barfight", 1, "Chapter 12 (lvl_barfight)" },
    { "lvl_bridge", 1, "Chapter 13 (lvl_bridge)" },      { "lvl_coach", 1, "Chapter 14 (lvl_coach)" },
    { "lvl_woods", 1, "Chapter 15 (lvl_woods)" },        { "lvl_core", 1, "Chapter 16 (lvl_core)" },
    { "lvl_corenew", 1, "Chapter 17 (lvl_corenew)" },    { "lvl_msn", 1, "Chapter 18 (lvl_msn)" },
    { "lvl_msncyn", 1, "Chapter 19 (lvl_msncyn)" },      { "lvl_fort", 1, "Chapter 20 (lvl_fort)" },
    { "lvl_train", 1, "Chapter 21 (lvl_train)" },        { "lvl_trainend", 1, "Chapter 22 (lvl_trainend)" },
    { "lvl_tomb", 5, "Bounty Hunter 5 (lvl_tomb 5)" },   { "lvl_roydusk", 6, "Chapter 23 (lvl_roydusk 6)" },
    { "lvl_royfinal", 7, "Chapter 24 (lvl_royfinal 7)" },{ "lvl_mans", 1, "Chapter 25 (lvl_mans)" },
    { "lvl_mansintb", 1, "Chapter 26 (lvl_mansintb)" },  { "lvl_mansroof", 1, "Chapter 27 (lvl_mansroof)" },
    /* on the disc, in no chapter */
    { "lvl_saloon", 1, "Unused: lvl_saloon" },           { "lvl_theater", 2, "Unused: lvl_theater 2" },
    { "lvl_royale", 1, "Unused: lvl_royale" },
};
static const struct { const char *name, *label; } s_stages[] = {
    { "lvl_hng", "Twin Rocks" },        { "lvl_fort", "Desert Fortress" }, { "lvl_shernit", "Widows Patch" },
    { "lvl_bridge", "The Bridge" },     { "lvl_ghost", "Fairground" },     { "lvl_coach", "Sunset Canyon" },
    { "lvl_perf", "Broken Creek" },     { "lvl_ranch", "The Ranch" },      { "lvl_core", "The Mine" },
    { "lvl_gun", "Ghost Town" },        { "lvl_flash", "Fallen Creek" },   { "lvl_mans", "Mansion Grounds" },
    { "lvl_int", "The Mansion" },       { "lvl_colseum", "The Coliseum" },
};

static int add_level(RdrLevel *out, int n, int max, const char *name, int var, const char *label)
{
    int i;
    if (n >= max) return n;
    for (i = 0; i < n; i++)
        if (!strcmp(out[i].name, name) && out[i].var == var) return n;
    snprintf(out[n].name, sizeof out[n].name, "%s", name);
    out[n].var = var;
    snprintf(out[n].label, sizeof out[n].label, "%s", label);
    return n + 1;
}

/* Stages added in the mods folders (addlevel.py): lvls_m2\<name>2.dat. */
static int mods_stages_in(const char *m, int mlen, RdrLevel *out, int n, int max);
static int mods_stages(RdrLevel *out, int n, int max)
{
    const char *m = getenv("RECOMP_MODS_DIR");
    while (m && *m) {                   /* several, ';' between them */
        const char *end = strchr(m, ';');
        int len = end ? (int)(end - m) : (int)strlen(m);
        if (len > 0)
            n = mods_stages_in(m, len, out, n, max);
        m = end ? end + 1 : NULL;
    }
    return n;
}

static int mods_stages_in(const char *m, int mlen, RdrLevel *out, int n, int max)
{
    char dir[1024], base[32], name[40], label[48];
    snprintf(dir, sizeof dir, "%.*s/lvls_m2", mlen, m);
#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        HANDLE h;
        char pat[1100];
        snprintf(pat, sizeof pat, "%s\\*2.dat", dir);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return n;
        do {
            const char *f = fd.cFileName;
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (!d) return n;
        while ((e = readdir(d)) != NULL) {
            const char *f = e->d_name;
#endif
            size_t L = strlen(f), i;
            if (L < 7 || L - 5 >= sizeof base || strcmp(f + L - 5, "2.dat") != 0) continue;
            for (i = 0; i < L - 5; i++)
                base[i] = (char)((f[i] >= 'A' && f[i] <= 'Z') ? f[i] + 32 : f[i]);
            base[L - 5] = 0;
            snprintf(name, sizeof name, "lvl_%s", base);
            snprintf(label, sizeof label, "%s (mods)", name);
            n = add_level(out, n, max, name, 0, label);
#ifdef _WIN32
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
        }
        closedir(d);
    }
#endif
    return n;
}

int rdr_level_list(int kind, RdrLevel *out, int max)
{
    int n = 0;
    size_t i;
    if (kind == 2) {
        for (i = 0; i < sizeof s_stages / sizeof s_stages[0]; i++)
            n = add_level(out, n, max, s_stages[i].name, 0, s_stages[i].label);
        n = mods_stages(out, n, max);
    } else if (kind == 1) {
        for (i = 0; i < sizeof s_story / sizeof s_story[0]; i++)
            n = add_level(out, n, max, s_story[i].name, s_story[i].var, s_story[i].label);
    }
    return n;
}

int rdr_level_go(const char *name, int var)
{
    if (!rdr_in_level() || !name || !*name) return 0;
    snprintf(s_go_name, sizeof s_go_name, "%s", name);
    s_go_var = var;
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#else
    __asm__ __volatile__("" ::: "memory");
#endif
    s_go_pending = 1;
    return 1;
}

static void level_go_apply(void)
{
    volatile char *p;
    int i;
    s_go_pending = 0;
    if (!rdr_in_level()) return;
    char digit = 0;
    p = (volatile char *)((uintptr_t)xbox_GetMemoryOffset() + RDR_NEXT_LEVEL);
    if (*g32(RDR_SHOWDOWN) == 1) {
        /* keep the player count's digit: lvl_perf2 -> lvl_ghost2 */
        for (i = 0; i < 63 && p[i]; i++) {}
        if (i > 0 && p[i - 1] >= '0' && p[i - 1] <= '9')
            digit = p[i - 1];
    }
    for (i = 0; i < 62 && s_go_name[i]; i++)
        p[i] = (char)((s_go_name[i] >= 'A' && s_go_name[i] <= 'Z') ? s_go_name[i] + 32 : s_go_name[i]);
    if (digit)
        p[i++] = digit;
    p[i] = 0;
    if (s_go_var > 0 && *g32(RDR_SHOWDOWN) != 1)
        *g32(RDR_NEXT_VAR) = (uint32_t)s_go_var;
    *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + 0x00652EAFu) = 0;
    *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + RDR_RESET_FLAG) = 1;
    /* sub_00070990 reads the flag on its way out of a pause */
    if (!*(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + RDR_PAUSE_MENU))
        *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + RDR_GAME_PAUSED) = 1;
    fprintf(stderr, "[LEVEL] going to %s %d\n", (const char *)p, (int)*g32(RDR_NEXT_VAR));
}

/* Tests (RDR_OVERLAY_FLAG "hudmask <hex>"): the HUD's parts mask
 * ([0x0067A1BC], 0x3FF in a level) ANDed with this every frame. */
uint32_t g_rdr_hud_mask_test = 0xFFFFFFFFu;

/* ---- Button pictures -----------------------------------------------
 *
 * Which the game's prompts show (rdr_sharptext.c redraws its fonts' button
 * pictures with the PS2 version's, still on the disc). */
int rdr_buttons_style(void)
{
    int k;
    if (g_rdr_settings.buttons == 1 || g_rdr_settings.buttons == 2)
        return g_rdr_settings.buttons;
    /* Player 1 on keyboard and mouse: key caps, even with a controller
     * plugged in (user: keyboard and mouse chosen, a pad connected but not
     * used, and the Xbox's pictures shown). Buttons > Xbox or PlayStation
     * in Controls still picks a controller's. */
    if (g_rdr_settings.mouse_aim && g_rdr_settings.kbm_player == 1)
        return 3;
    for (k = 0; k < 4; k++) {
        const char *n = xbox_InputPortName((DWORD)k);
        char l[96];
        size_t i;
        if (!n)
            continue;
        for (i = 0; n[i] && i < sizeof l - 1; i++)
            l[i] = (char)((n[i] >= 'A' && n[i] <= 'Z') ? n[i] + 32 : n[i]);
        l[i] = 0;
        return (strstr(l, "ps3") || strstr(l, "ps4") || strstr(l, "ps5") || strstr(l, "playstation")
                || strstr(l, "dualsense") || strstr(l, "dualshock") || strstr(l, "sony")) ? 2 : 1;
    }
    /* nothing plugged in: keys and mouse (key caps, in these menus and
     * the game's own prompts), else the game's own */
    return g_rdr_settings.mouse_aim ? 3 : 1;
}

/* On the game's thread, at each pad read (rdr_hooks.c, XInputGetState):
 * every frame, in the menus and paused too, and never while the game is
 * changing the same memory. Up to four times a frame (a pad each); all of
 * it is the same done again. Online, at the same point of every frame on
 * every machine. */
void rdr_cheats_tick(void)
{
    if (g_rdr_hud_mask_test != 0xFFFFFFFFu)
        *g32(0x0067A1BCu) &= g_rdr_hud_mask_test;
    if (s_go_pending)
        level_go_apply();
    if (g_rdr_infinite_ammo) {
        ammo_apply();
    } else if (s_ammo[0].va || s_ammo[1].va) {
        int i;
        for (i = 0; i < AMMO_TRACK; i++)
            s_ammo[i].va = 0;               /* forget: a new level, new guns */
    }
    /* Re-applied every frame while on: a level loaded later gets its
     * record filled in after the switch was thrown. */
    if (g_rdr_always_excellent) {
        excellent_apply(1);
        s_applied = 1;
    } else if (s_applied) {
        excellent_apply(0);
        s_applied = 0;
    }
}

/* ---- Unlock everything ----------------------------------------------
 *
 * Unlockables.csv lists what the game hands out over time: 48 Showdown
 * characters, 16 Showdown stages, 147 journal pages (the Extras: art,
 * bios, the gallery) and 10 cheats -- 221 in all. The manager at
 * 0x006585C0 holds the table (+0x14: entries, u16 count at +4) and a
 * pointer (+0) to the saved data shared by every profile, whose +0x24 is
 * 256 bits, one per entry: set = unlocked (sub_00057C90 asks, sub_00057D30
 * sets). The retail game still carries a debug switch, "unlockall" on its
 * command line, that skips the check for characters, stages and journal
 * pages; this sets the bits themselves instead, so the game saves them
 * with the rest and they stay. The two debug cheats in the table (Show
 * Position, Show AI Info) are left alone. */
#define RDR_UNLOCK_MGR     0x006585C0u

/* The bits this set are kept in unlock_undo.txt beside settings.ini, so
 * "Undo Unlock Everything" can clear exactly those again -- whatever was
 * unlocked before, or earned since, stays. */
static void undo_path(char *out, size_t n)
{
    const char *sp = rdr_settings_path();
    const char *a = strrchr(sp, '\\'), *b = strrchr(sp, '/');
    const char *sl = a > b ? a : b;
    int k = sl ? (int)(sl - sp) + 1 : 0;
    snprintf(out, n, "%.*sunlock_undo.txt", k, sp);
}

int rdr_unlock_everything(void)
{
    uint32_t data = *g32(RDR_UNLOCK_MGR), tab = *g32(RDR_UNLOCK_MGR + 0x14), arr, n, i;
    uint32_t added[8] = {0};
    int set = 0;
    if (!plausible(data) || !plausible(tab))
        return -1;
    arr = *g32(tab);
    n = *(volatile uint16_t *)((uintptr_t)xbox_GetMemoryOffset() + tab + 4);
    if (!plausible(arr) || n == 0)
        return -1;
    if (n > 256)
        n = 256;                /* a mod's rows past 255 have no bit (always open) */
    for (i = 0; i < n; i++) {
        uint32_t e = *g32(arr + i * 4), name = plausible(e) ? *g32(e) : 0;
        volatile uint32_t *w = g32(data + 0x24 + (i >> 5) * 4);
        if (plausible(name)) {
            const char *nm = (const char *)((uintptr_t)xbox_GetMemoryOffset() + name);
            if (!strcmp(nm, "cheat_showaiinfo") || !strcmp(nm, "cheat_showposition"))
                continue;
        }
        if (!(*w & (1u << (i & 31)))) {
            *w |= 1u << (i & 31);
            added[i >> 5] |= 1u << (i & 31);
            set++;
        }
    }
    if (set) {
        /* Added to what an earlier unlock (not yet undone) set. */
        char path[1100];
        uint32_t old[8] = {0};
        FILE *f;
        undo_path(path, sizeof path);
        f = fopen(path, "r");
        if (f) {
            for (i = 0; i < 8; i++)
                if (fscanf(f, "%x", &old[i]) != 1)
                    break;
            fclose(f);
        }
        f = fopen(path, "w");
        if (f) {
            for (i = 0; i < 8; i++)
                fprintf(f, "%08x\n", added[i] | old[i]);
            fclose(f);
        }
    }
    fprintf(stderr, "[CHEATS] Unlock everything: %d of %u newly unlocked\n", set, n);
    fflush(stderr);
    return set;
}

int rdr_unlock_can_undo(void)
{
    char path[1100];
    FILE *f;
    undo_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

int rdr_unlock_undo(void)
{
    uint32_t data = *g32(RDR_UNLOCK_MGR), mask[8] = {0}, i;
    char path[1100];
    int cleared = 0;
    FILE *f;
    if (!plausible(data))
        return -1;
    undo_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f)
        return 0;
    for (i = 0; i < 8; i++)
        if (fscanf(f, "%x", &mask[i]) != 1)
            break;
    fclose(f);
    for (i = 0; i < 8; i++) {
        volatile uint32_t *w = g32(data + 0x24 + i * 4);
        uint32_t b;
        for (b = 0; b < 32; b++)
            if ((mask[i] >> b & 1) && (*w >> b & 1))
                cleared++;
        *w &= ~mask[i];
    }
    remove(path);
    fprintf(stderr, "[CHEATS] Unlock everything undone: %d locked again\n", cleared);
    fflush(stderr);
    return cleared;
}

/* ---- Money -----------------------------------------------------------
 *
 * The player character is [0x00675930 + player * 4]. Its money is +0x9FC
 * (GetMoney, vtable +0x164 = sub_0013E220, returns it in single player);
 * SetMoney (vtable +0x168, sub_0013E2B0) writes it and resets the HUD's
 * counter, the object at +0x890, whose +0x53F8 is the amount it shows and
 * hands back to the player. Both are written, as SetMoney does. */
#define RDR_PLAYERS       0x00675930u
#define RDR_GETMONEY_FN   0x0013E220u

static uint32_t player_va(void)
{
    uint32_t idx = *g32(RDR_PLAYER_INDEX), p, vt, lv;
    if (idx > 3)
        return 0;
    /* The front end loaded ([0x0065AA0C]+0x14 "lvl_front"): no player, even
     * though the players' list still names the last level's. */
    lv = *g32(0x0065AA0Cu);
    if (plausible(lv)) {
        const volatile char *n = (const volatile char *)((uintptr_t)xbox_GetMemoryOffset() + lv + 0x14);
        if (n[0] == 'l' && n[1] == 'v' && n[2] == 'l' && n[3] == '_' && n[4] == 'f' && n[5] == 'r'
            && n[6] == 'o' && n[7] == 'n' && n[8] == 't')
            return 0;
    }
    p = *g32(RDR_PLAYERS + idx * 4u);
    if (!plausible(p))
        return 0;
    vt = *g32(p);
    if (vt < 0x003C5360u || vt >= 0x00403400u)      /* .rdata */
        return 0;
    if (*g32(vt + 0x164) != RDR_GETMONEY_FN)
        return 0;
    return p;
}

/* ---- Menus ---------------------------------------------------------
 *
 * [0x00651FF4] is set while the pause menu is open (sub_0002F8C0 opens it,
 * 0x0002F99A closes it; 0x000709D9 turns gameplay input off from it).
 * With no player loaded the game is in the front end (the desk, the book,
 * the poster). Either way the mouse works the menu, not the camera. */
#define RDR_PAUSE_MENU    0x00651FF4u

/* The game clock's last frame time, seconds ([0x00406504]). */
double rdr_frame_seconds(void)
{
    return *(volatile float *)((uintptr_t)xbox_GetMemoryOffset() + 0x00406504u);
}

int rdr_in_level(void)
{
    return player_va() != 0;
}

/* Where the keyboard and mouse go (pad_filter). The game takes its menus
 * from one controller -- the menu input map [0x0067991C], +4 its port: the
 * one that pressed START -- and a lone story player from its own pad
 * object ([player+0x894], one per port at 0x006795C8). With the keyboard
 * on player 1 and a controller on player 2, START pressed on the
 * controller left the keyboard and mouse driving a port nothing read: no
 * clicks in the pause menu, no key to leave it. So: in a menu, the menu's
 * port; one player in a level, that player's port; else (split screen,
 * co-op) the keyboard player's own. */
int rdr_keyboard_port(int kbm_port)
{
    uint32_t map, n, p, pad;
    if (rdr_menu_mode()) {
        map = *g32(0x0067991Cu);
        /* (In the heap, 0x8xxxxxxx: only below 64 MB was taken, so the
         * menus' port was never followed -- in co-op the menus read the
         * last player's port and the keyboard went to player 1's.) */
        if (plausible(map)) {
            uint32_t port = *g32(map + 4);
            if (port < 4)
                return (int)port;
        }
        return kbm_port;
    }
    n = *g32(0x0067592Cu);
    p = player_va();
    if (n == 1 && p) {
        pad = *g32(p + 0x894);
        if (pad >= 0x006795C8u && pad < 0x006795C8u + 4 * 0xB0u && (pad - 0x006795C8u) % 0xB0u == 0)
            return (int)((pad - 0x006795C8u) / 0xB0u);
    }
    return kbm_port;
}

/* The menus' port given to the keyboard: [map+4] is the controller that
 * pressed START, which the menus read. When that controller is unplugged
 * the menus listened to nothing -- the keyboard and mouse went with them to
 * a port with nobody on it (user report: controllers unplugged, back at
 * the main menu, the keyboard and mouse did nothing). */
int rdr_menu_port_take(int port)
{
    uint32_t map = *g32(0x0067991Cu);
    if (!rdr_menu_mode() || !plausible(map) || port < 0 || port > 3)
        return 0;
    if (*g32(map + 4) >= 4 || *g32(map + 4) == (uint32_t)port)
        return 0;
    *g32(map + 4) = (uint32_t)port;
    return 1;
}

int rdr_pause_menu(void)
{
    return *(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + RDR_PAUSE_MENU) != 0;
}

int rdr_menu_mode(void)
{
    if (*(volatile uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + RDR_PAUSE_MENU))
        return 1;
    return player_va() == 0;
}

int rdr_money_get(void)
{
    uint32_t p = player_va();
    if (!p || *g32(RDR_MULTIPLAYER) != 0)
        return -1;
    return (int)*g32(p + 0x9FC);
}

int rdr_money_set(int amount)
{
    uint32_t p = player_va(), hud;
    if (!p || *g32(RDR_MULTIPLAYER) != 0)
        return 0;
    if (amount < 0) amount = 0;
    if (amount > 9999999) amount = 9999999;
    hud = *g32(p + 0x890);
    if (plausible(hud))
        *g32(hud + 0x53F8) = (uint32_t)amount;
    *g32(p + 0x9FC) = (uint32_t)amount;
    return 1;
}

/* ---- Play as ------------------------------------------------------------
 *
 * The story's player character swapped for a Showdown one (rdr_hooks.c,
 * at level start). The codes are the .swt type files in swat.dat. */

/* Names: the Showdown line-up's own, from the front end's string table
 * (front.dat swat.strtbl, keyed by the code without its "m"; keys hash with
 * the ELF hash). Sorted by name; saved by code (settings.ini play_as). */
RdrCharacter g_rdr_characters[RDR_MAX_CHARACTERS] = {
    { "",     "Story default" },
    /* The story's own playable characters first, as the story has them
     * (their own type files, not Showdown's): */
    { "srd",  "Red (story)" },
    { "jck",  "Jack Swift (story)" },
    { "ani",  "Annie Stoakes (story)" },
    { "ttk",  "Shadow Wolf (story)" },
    { "bfs",  "Buffalo Soldier (story)" },
    { "mani", "Annie Stoakes" },
    { "mbty", "Bad Bessie" },
    { "mfrk", "\"Big Oaf\" Whitney" },
    { "mgay", "\"Bloody\" Tom" },
    { "mbfs", "Buffalo" },
    { "mchk", "Chicken" },
    { "mctn", "Christina" },
    { "mydr", "Colonel Daren" },
    { "mbkt", "Cooper" },
    { "mshp", "\"Curly\" Shaw" },
    { "mbdg", "Dan" },
    { "mgwk", "\"Dig\" Fowler" },
    { "mmom", "Falling Star" },
    { "mmdg", "Fidgit" },
    { "mzzt", "Gabriel Navarro" },
    { "mdgo", "General Diego" },
    { "mggl", "\"Gigolo\" Hancock" },
    { "mwyn", "Governor Griffon" },
    { "mmst", "Grizzly" },
    { "mhdh", "\"Hedgehog\" Thornton" },
    { "mtcb", "Holstein Hal" },
    { "mjck", "Jack Swift" },
    { "msev", "Jason Cornet" },
    { "mkti", "Katie O'Grady" },
    { "melt", "\"Kid\" Cougar" },
    { "mmok", "Manny Quinn" },
    { "mblk", "Mr. Black" },
    { "mmrk", "Mr. Kelley" },
    { "mntl", "Natalie" },
    { "mdad", "Nate Harlow" },
    { "mpgj", "\"Pig\" Josh" },
    { "mpry", "Professor Perry" },
    { "msrd", "Red Harlow" },
    { "mmrd", "Red Jr." },
    { "mbra", "Rico Pedrosa" },
    { "mtck", "Sam" },
    { "mttk", "Shadow Wolf" },
    { "mkng", "Sheriff Bartlett" },
    { "mcls", "Sheriff O'Grady" },
    { "msto", "\"Sissy\" Fess" },
    { "maka", "\"Smiley\" Fawler" },
    { "mncb", "Smitty" },
    { "mmsk", "Standing Snake" },
    { "mbwi", "\"The Blade\" Slade" },
    { "mtwg", "\"Twiggy\" Phelps" },
    { "mmon", "Udo Kang" },
    { "mugl", "\"Ugly\" Chris" },
    { "mwsk", "\"Whiskey\" Treadwell" },
};
#define RDR_RETAIL_CHARACTERS 54
const int g_rdr_character_retail = RDR_RETAIL_CHARACTERS;
int g_rdr_character_count = RDR_RETAIL_CHARACTERS;

int rdr_character_index(const char *code)
{
    int i;
    if (!code || !*code)
        return 0;
    for (i = 1; i < g_rdr_character_count; i++)
        if (!strcmp(code, g_rdr_characters[i].code))
            return i;
    return 0;
}

/* A character type's name for the spawner: its Showdown name ("m" and the
 * type), else the story's people (tools/story_npcs.py), else its code. */
static const RdrCharacter k_story_people[] = {
    { "own01", "Gunsmith" }, { "own02", "Banker" }, { "own03", "Realtor" },
    { "own04", "Shopkeeper" }, { "own05", "Barber" }, { "own06", "Fitch" },
    { "own07", "Barman" }, { "bma", "Barmaid" }, { "jdy", "Jody" },
    { "np1", "Townsman" }, { "np2", "Townsman" }, { "np3", "Pedro" },
    { "np4", "Townsman" }, { "npa", "Townswoman" }, { "npb", "Townswoman" },
    { "bt1", "Bartender" }, { "bt2", "Bartender" }, { "brb", "Wilbur" },
    { "tdv", "Conductor" }, { "sdh", "Eli" }, { "sdt", "Soldier" },
    { "gsg", "Guard" }, { "in1", "Brave" }, { "in4", "Chief" },
    { "nod", "Old Timer" }, { "frj", "George" }, { "gip", "Larouche" },
    { "pck", "Ozwald" }, { "kya", "Jesse" }, { "ct2", "Ashley" },
    { "nkd", "Jane" }, { "rok", "Atlas" }, { "jes", "Jesse Lynch" }, { "kt2", "Katie O'Grady (2nd Encounter)" },
    { "dcg", "Katie O'Grady (1st Encounter)" }, { "dar", "Colonel Darren" }, { "dvy", "Griffin" },
    { "mrd2", "Young Red (Bloody Hand)" }, { "rhd", "Ranch Hand" }, { "shr", "Horse" }, { "bul", "Cow" }, { "bfl", "Buffalo" },
};

const char *rdr_type_name(const char *code)
{
    char m[16];
    size_t i;
    int k;
    if (!code || !*code)
        return "Anyone";
    snprintf(m, sizeof m, "m%s", code);
    k = rdr_character_index(m);
    if (k > 0)
        return g_rdr_characters[k].name;
    for (i = 0; i < sizeof k_story_people / sizeof k_story_people[0]; i++)
        if (!strcmp(k_story_people[i].code, code))
            return k_story_people[i].name;
    return NULL;                        /* not known: the spawner names it by side */
}

/* A co-op player being made (rdr_overrides.c, rdr_coop_level_start): its
 * own choice for the next spawn, or NULL for player 1's. */
static const char *s_spawn_as;

void rdr_play_as_spawn(const char *code)
{
    s_spawn_as = code;
}

/* Player k's (1-3) co-op choice: a code, "" when it is player 1's. */
const char *rdr_coop_char(int k)
{
    const char *e;
    char name[16];
    int i;
    snprintf(name, sizeof name, "RDR_COOP_CHAR%d", k + 1);    /* tests */
    e = getenv(name);
    if (e && *e)
        return e;
    i = k > 0 && k < 4 ? g_rdr_settings.coop_char[k] : 0;
    return i > 0 && i < g_rdr_character_count ? g_rdr_characters[i].code : "";
}

const char *rdr_play_as(void)
{
    const char *e = getenv("RDR_PLAY_AS");      /* tests */
    int i = g_rdr_settings.play_as;
    if (s_spawn_as)
        return s_spawn_as;
    if (e && *e)
        return e;
    if (i <= 0 || i >= g_rdr_character_count)
        return "";
    return g_rdr_characters[i].code;
}

/* Showdown's power-ups, by the game's own names (swat.strtbl). Worn by
 * every played character while switched on (rdr_overrides.c). */
const RdrPowerup g_rdr_powerups[] = {
    { "cr_speed_up",          "Speed Up",               0 },
    { "cr_invulnerable",      "Untouchable",            0 },
    { "cr_ghost",             "Ghost",                  0 },
    { "cr_regen_health",      "Health Regeneration",    0 },
    { "cr_damagemult",        "Triple Damage",          0 },
    { "cr_fastbullet",        "Faster Bullets",         0 },
    { "cr_fast_reload",       "Fast Reload",            0 },
    { "cr_triplecash",        "Triple Cash",            0 },
    { "cr_retrib",            "Retribution",            0 },
    { "cr_burningrage",       "Burning Rage",           0 },
    { "cr_forgedcard",        "Forged Card",            0 },
    { "cr_stickybomb",        "Sticky Bomb",            0 },
    { "cr_speed_down",        "Slow Down",              0 },
    { "cr_pistol_ignite",     "Burning Pistol",         1 },
    { "cr_pistol_poison",     "Venomous Pistol",        1 },
    { "cr_pistol_knockdown",  "Knockdown Pistol",       1 },
    { "cr_dblgun_ignite",     "Burning Twin Revolvers", 1 },
    { "cr_dblgun_poison",     "Venomous Twin Revolvers", 1 },
    { "cr_dblgun_knockdown",  "Knockdown Twin Revolvers", 1 },
    { "cr_dblgun_cannon_call", "Diego's Twin Revolvers", 1 },
    { "cr_rifle_ice",         "Ice Rifle",              1 },
    { "cr_rifle_piercing",    "Piercing Rifle",         1 },
    { "cr_shotgun_ignite",    "Burning Shotgun",        1 },
    { "cr_shotgun_explode",   "Explosive Shotgun",      1 },
    { "cr_arrow_ignite",      "Flaming Arrows",         1 },
    { "cr_arrow_explode",     "Explosive Arrows",       1 },
    { "cr_arrow_piercing",    "Piercing Arrows",        1 },
    { "cr_arrow_poison",      "Venomous Arrows",        1 },
    { "cr_knife_boomerang",   "Boomerang Knife",        1 },
    { "cr_knife_pickpocket",  "Pick Pocket Knife",      1 },
    { "cr_knife_southpaw",    "Explosive Knife",        1 },
    { "cr_knife_spirit",      "Spirit Knife",           1 },
    { "cr_knife_vampire",     "Vampiric Knife",         1 },
    { "cr_molotov_explode",   "Nitroglycerine",         1 },
    { "cr_molotov_hotcoffee", "Hot Coffee",             1 },
    { "cr_molotov_ice",       "Ice Bottle",             1 },
    { "cr_dynamite_explode",  "Short-Fuse Dynamite",    1 },
    { "gb_drunk",             "Everyone Is Drunk",      2 },
    { "gb_fast_time",         "Time Speed Up",          2 },
    { "gb_cannonstrike",      "Cannon Strike",          2 },
};
const int g_rdr_powerup_count = (int)(sizeof g_rdr_powerups / sizeof g_rdr_powerups[0]);
volatile unsigned long long g_rdr_powerup_mask;

/* Tests: RDR_POWERUP_TEST=code[,code...] switches these on at start. */
void rdr_powerups_test_init(void)
{
    const char *e = getenv("RDR_POWERUP_TEST");
    int i;
    if (!e || !*e)
        return;
    for (i = 0; i < g_rdr_powerup_count; i++) {
        const char *p = strstr(e, g_rdr_powerups[i].code);
        size_t l = strlen(g_rdr_powerups[i].code);
        if (p && (p == e || p[-1] == ',') && (p[l] == 0 || p[l] == ','))
            g_rdr_powerup_mask |= 1ull << i;
    }
}
