/*
 * rdr_npc.c - new people in a level (the Spawner's "New" section).
 *
 * A level's people come from its enemy file (<level>.enm, plain text):
 *
 *   Enemy
 *   {
 *       Table       Tomb_Deputy         the AI table (.tbl)
 *       Type        frk                 the character (.swt, swat.dat)
 *       Name        Paper boy
 *       Alignment   0                   the team ([inst+0xB4])
 *       Group       innocents
 *       ... WeaponList, StartWeapon, BossType, MyBoss, PowerUp, Cullable,
 *       Pushable, Spawn n { Point x y z Angle a } ...
 *   }
 *
 * enmManager::Load (0x00075E50, thiscall on the manager [0x0065A998]) opens
 * it through the archives ([0x0065ABB0] vtable +0x24: name, "enm", 0, 1 ->
 * a stream), reads it with a tokenizer (sub_00085DE0 over the stream) and
 * for each "Enemy" block: "{", "Table" and its name, "Type" and its name,
 * the type (sub_00097900(type, 1, IsBeast), a .swt from swat.dat), an
 * instance -- 0x2A4 bytes and sub_00077D40(type) for a person; the type's
 * +0x1B0 1 or 3 (or sub_00096F30) makes one of the animal classes -- the
 * rest of the block (sub_000779B0: Name, Alignment, Group, ..., the spawn
 * points), "}", and the instance goes on the end of the manager's list
 * (+0x24, then +0x1B8). After the file, each instance is given its AI
 * table by name (sub_000764C0).
 *
 * Here the same steps are taken for a block written on the spot: the file
 * the archives are asked for ("rdrnpcmake.enm") is served from memory --
 * the archive driver's open, read, seek, size and close (the table at
 * [0x00406268]; called through it, so overridden without a regen) answer
 * for that one name. The new person is then brought in like any waiting
 * one (rdr_overrides.c, the spawner). Characters are swat.dat's: every
 * story and Showdown type is there (Play As uses the same), and the AI
 * tables too (the Showdown bots' default_bot, the story's own); a level's
 * own tables and types are there while it is loaded.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xbox/xboxrecomp.h>   /* RECOMP_TLS */

typedef void (*recomp_func_t)(void);
extern ptrdiff_t xbox_GetMemoryOffset(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

extern void sub_000D4F50(void), sub_000D5000(void), sub_000D5020(void), sub_000D3FE0(void),
    sub_000D4010(void);
extern void sub_00086A20(void);
extern void sub_00085DE0(void), sub_00084AB0(void), sub_00085240(void), sub_00027D20(void),
    sub_00097900(void), sub_0005DCC0(void), sub_00077D40(void), sub_000779B0(void),
    sub_000764C0(void), sub_0005CA20(void), sub_00096F30(void), sub_000794C0(void), sub_000749F0(void);

#define ENM_MGR     0x0065A998u
#define ARCHIVES    0x0065ABB0u
#define NPC_HANDLE  0x5A5A0001u         /* no archive file has such a handle */
#define NPC_FILE    "rdrnpcmake"

static uint8_t *gm(uint32_t va) { return (uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + va); }
static uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, gm(va), 4); return v; }
static void wr32(uint32_t va, uint32_t v) { memcpy(gm(va), &v, 4); }
static int in_heap(uint32_t va)
{
    return (va >= 0x00010000u && va < 0x04000000u) || (va >= 0x80000000u && va < 0x84000000u);
}
static void push(uint32_t v) { g_esp -= 4; wr32(g_esp, v); }

/* ── The file ───────────────────────────────────────────────────── */

static char s_text[4096];
static uint32_t s_len, s_pos;
static int s_open;

static void str_at(uint32_t va, char *out, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n && in_heap(va + (uint32_t)i) && *gm(va + (uint32_t)i); i++)
        out[i] = (char)*gm(va + (uint32_t)i);
    out[i] = 0;
}

/* open(path, mode), cdecl */
static void npc_open(void)
{
    char path[260];
    uint32_t p = rd32(g_esp + 4);
    path[0] = 0;
    if (in_heap(p) || p < 0x00800000u)
        str_at(p, path, sizeof path);
    if (strstr(path, NPC_FILE)) {
        s_pos = 0;
        s_open = 1;
        g_eax = NPC_HANDLE;
        g_esp += 4;
        return;
    }
    sub_000D4F50();
}

/* read(handle, buffer, count) */
static void npc_read(void)
{
    uint32_t h = rd32(g_esp + 4), buf = rd32(g_esp + 8), n = rd32(g_esp + 12);
    if (h != NPC_HANDLE) {
        sub_000D5000();
        return;
    }
    if (n > s_len - s_pos)
        n = s_len - s_pos;
    if (n)
        memcpy(gm(buf), s_text + s_pos, n);
    s_pos += n;
    g_eax = n;
    g_esp += 4;
}

/* seek(handle, offset, whence) -> position */
static void npc_seek(void)
{
    uint32_t h = rd32(g_esp + 4), wh = rd32(g_esp + 12);
    int32_t off = (int32_t)rd32(g_esp + 8), at;
    if (h != NPC_HANDLE) {
        sub_000D5020();
        return;
    }
    at = wh == 0 ? off : wh == 1 ? (int32_t)s_pos + off : (int32_t)s_len + off;
    if (at < 0) at = 0;
    if (at > (int32_t)s_len) at = (int32_t)s_len;
    s_pos = (uint32_t)at;
    g_eax = s_pos;
    g_esp += 4;
}

static void npc_close(void)
{
    if (rd32(g_esp + 4) != NPC_HANDLE) {
        sub_000D3FE0();
        return;
    }
    s_open = 0;
    g_eax = 0;
    g_esp += 4;
}

static void npc_size(void)
{
    if (rd32(g_esp + 4) != NPC_HANDLE) {
        sub_000D4010();
        return;
    }
    g_eax = s_len;
    g_esp += 4;
}

/* rdr_overrides.c's lookup: the archive driver's functions */
recomp_func_t rdr_npc_override(uint32_t va)
{
    switch (va) {
    case 0x000D4F50u: return npc_open;
    case 0x000D5000u: return npc_read;
    case 0x000D5020u: return npc_seek;
    case 0x000D3FE0u: return npc_close;
    case 0x000D4010u: return npc_size;
    }
    return (recomp_func_t)0;
}

/* ── Making one ─────────────────────────────────────────────────── */

/* Guest scratch: strings, the tokenizer, buffers (the game's own heap,
 * sub_0005DCC0, once). */
static uint32_t s_scratch;
#define SC_NAME   0x000u                /* "rdrnpcmake" */
#define SC_TOK    0x040u                /* the tokenizer (its size is not known: room for 0x300) */
#define SC_WORD   0x340u                /* a token, 0x40 */
#define SC_TABLE  0x380u                /* the Table, 0x40 */
#define SC_TYPE   0x3C0u                /* the Type, 0x40 */
#define SC_STR    0x400u                /* "{", "Table", "Type", "}" */
#define SC_SIZE   0x480u

static uint32_t call(recomp_func_t f, uint32_t ecx, int n, const uint32_t *a, int callee_pops)
{
    uint32_t esp0 = g_esp, ecx0 = g_ecx, eax0 = g_eax, edx0 = g_edx, r;
    int i;
    g_esp -= 64;                        /* (room below whatever was there) */
    for (i = n - 1; i >= 0; i--)
        push(a[i]);
    push(0x00076251u);                  /* a return address inside enmManager::Load */
    g_ecx = ecx;
    f();
    r = g_eax;
    (void)callee_pops;
    g_esp = esp0; g_ecx = ecx0; g_eax = eax0; g_edx = edx0;
    return r;
}

static uint32_t vcall(uint32_t obj, uint32_t off, int n, const uint32_t *a)
{
    uint32_t fn = rd32(rd32(obj) + off);
    recomp_func_t f = recomp_lookup_manual(fn);
    if (!f)
        f = recomp_lookup(fn);
    return f ? call(f, obj, n, a, 1) : 0;
}

static int scratch(void)
{
    static const char strs[] = "{\0Table\0Type\0}";
    uint32_t a[1] = { SC_SIZE };
    if (s_scratch && in_heap(s_scratch))
        return 1;
    s_scratch = call(sub_0005DCC0, 0, 1, a, 0);
    if (!in_heap(s_scratch))
        return s_scratch = 0, 0;
    memset(gm(s_scratch), 0, SC_SIZE);
    memcpy(gm(s_scratch + SC_NAME), NPC_FILE, sizeof NPC_FILE);
    memcpy(gm(s_scratch + SC_STR), strs, sizeof strs);
    return 1;
}
#define S_OPEN   (s_scratch + SC_STR + 0)
#define S_TABLE  (s_scratch + SC_STR + 2)
#define S_TYPE   (s_scratch + SC_STR + 8)
#define S_CLOSE  (s_scratch + SC_STR + 13)

/* The archives' cache of names not found ([0x00662368]: a hash table of
 * 0x708 nodes, sub_000DA050 adds each miss) is made for one level. A
 * character from elsewhere looks for many files of its own that are not
 * there -- Bad Bessie's animations under Red's prefix and others -- and the
 * table ran out of nodes: "Couldn't find an empty hash table slot", the
 * game's fatal error (harness). Before each new person the cache is
 * replaced by a new, empty one with room for 0x1000 (the old is left: only
 * misses are lost, looked up again once) -- once a level, not for each
 * person (each was a new table of 0x1000 nodes left behind). */
static uint32_t s_miss_cache, s_miss_mgr;
static void miss_cache_fresh(void)
{
    uint32_t a[5], t;
    if (s_miss_cache && rd32(0x00662368u) == s_miss_cache && rd32(ENM_MGR) == s_miss_mgr)
        return;                         /* ours already (one per level, not one per person) */
    a[0] = 0x18;
    t = call(sub_0005DCC0, 0, 1, a, 0);
    if (!in_heap(t))
        return;
    a[0] = 0x1000; a[1] = 1; a[2] = 0x40; a[3] = 1; a[4] = 0;
    t = call(sub_00086A20, t, 5, a, 1);
    if (in_heap(t))
        wr32(0x00662368u, s_miss_cache = t), s_miss_mgr = rd32(ENM_MGR);
}

/* The manager's riders' table ([mgr+0x18], [mgr+0x14] entries of 12 bytes:
 * a rider and its animal, sub_00073030 / sub_000730B0) has one entry for
 * each animal the level loaded; a new animal gets one more. The table is
 * copied into a bigger one (the old is left: a few bytes). */
static void rider_slot_add(uint32_t mgr)
{
    uint32_t n = rd32(mgr + 0x14), old = rd32(mgr + 0x18), t, a[1];
    if (n > 4096)
        return;
    a[0] = (n + 1) * 12;
    t = call(sub_0005DCC0, 0, 1, a, 0);
    if (!in_heap(t))
        return;
    memset(gm(t), 0, (n + 1) * 12);
    if (n && in_heap(old))
        memcpy(gm(t), gm(old), n * 12);
    wr32(mgr + 0x18, t);
    wr32(mgr + 0x14, n + 1);
}

/* A new person of type `type` with AI table `table`, named `name`, on team
 * `team`, its spawn point at x, y, z facing `angle` degrees: put on the end
 * of the level's list, not brought in (the spawner does that). Returns the
 * instance, or 0. On the game's thread. */
uint32_t rdr_npc_make(const char *type, const char *table, const char *name, int team, int armed,
                      float x, float y, float z, float angle, char *why, size_t why_n)
{
    uint32_t mgr = rd32(ENM_MGR), stream, tok, ty, inst, kind, last, a[4];
    /* armed's bit 1: the table is an animal's; bit 2: a boss -- BossType 2,
     * which the bring-in (0x00072D8C) gives a name and a health bar on the
     * HUD (sub_001EB090), as the levels' bosses and their seconds have.
     * (3, the level's own boss, some levels look for: not given.) */
    int n, animal, animal_table = armed & 2, boss = (armed & 4) != 0;
    armed &= 1;
    why[0] = 0;
    if (!in_heap(mgr)) {
        snprintf(why, why_n, "no level");
        return 0;
    }
    if (!scratch()) {
        snprintf(why, why_n, "no memory");
        return 0;
    }
    miss_cache_fresh();
    n = snprintf(s_text, sizeof s_text,
                 "Enemy\r\n{\r\n"
                 "\tTable\t\t%s\r\n"
                 "\tType\t\t%s\r\n"
                 "\tName\t\t%s\r\n"
                 "\tComment\t\t---\r\n"
                 "\tAlignment\t\t%d \r\n"
                 "\tGroup\t\tspawned\r\n"
                 "\tPopFreq\t100 \r\n"
                 "\tWeaponList\t%s\t\r\n"
                 "\tStartWeapon\t0 \r\n"
                 "\tBossType\t%d \r\n"
                 "\tMyBoss\tnull\r\n"
                 "\tPowerUp\t0 \t0.000000 \r\n"
                 "\tCullable\t0 \r\n"
                 "\tPushable\t1 \r\n"
                 "\tSpawn\t\t1 \r\n"
                 "\t{\r\n"
                 "\t\tPoint\t%f\t%f\t%f \t\tAngle\t%f \r\n"
                 "\t}\r\n"
                 "}\r\n",
                 table, type, name, team, armed ? "1 \t1 \t1 \t1 \t1 \t1 "
                 : animal_table ? "1 \t0 \t0 \t0 \t0 \t0 " /* as the levels' horses */ : "0 \t0 \t0 \t0 \t0 \t0 ",
                 boss ? 2 : 1, x, y, z, angle);
    s_len = n > 0 && n < (int)sizeof s_text ? (uint32_t)n : 0;
    a[0] = s_scratch + SC_NAME; a[1] = 0x003CDFA8u /* "enm" */; a[2] = 0; a[3] = 1;
    stream = vcall(rd32(ARCHIVES), 0x24, 4, a);
    if (!stream || !s_open) {
        snprintf(why, why_n, "the file did not open (%08X)", stream);
        return 0;
    }
    tok = s_scratch + SC_TOK;
    memset(gm(tok), 0, 0x300);
    a[0] = s_scratch + SC_NAME; a[1] = stream;
    call(sub_00085DE0, tok, 2, a, 1);
    a[0] = s_scratch + SC_WORD; a[1] = 0x20;
    if (!call(sub_00084AB0, tok, 2, a, 1)) {
        snprintf(why, why_n, "no block in the file");
        goto out;
    }
    a[0] = S_OPEN;  call(sub_00085240, tok, 1, a, 1);
    a[0] = S_TABLE; call(sub_00085240, tok, 1, a, 1);
    a[0] = s_scratch + SC_TABLE; a[1] = 0x40; call(sub_00027D20, tok, 2, a, 1);
    a[0] = S_TYPE;  call(sub_00085240, tok, 1, a, 1);
    a[0] = s_scratch + SC_TYPE; a[1] = 0x40; call(sub_00027D20, tok, 2, a, 1);
    a[0] = s_scratch + SC_TYPE; a[1] = 1; a[2] = 0;
    ty = call(sub_00097900, 0, 3, a, 0);
    if (!in_heap(ty)) {
        snprintf(why, why_n, "no character type %s", type);
        goto out;
    }
    /* a person (0x2A4 bytes, sub_00077D40) or one of the animal classes
     * (0x5E8: kind 3 sub_000749F0, kind 1 or a beast sub_000794C0), as the
     * loader decides (0x00076040) */
    kind = rd32(ty + 0x1B0);
    animal = kind == 1 || kind == 3 || (call(sub_00096F30, ty, 0, a, 1) & 0xFF);
    if (!animal != !animal_table) {
        /* a person with an animal's ways, or an animal with a person's */
        snprintf(why, why_n, animal ? "%s is an animal: it can only act as one" : "%s is not an animal", type);
        goto out;
    }
    a[0] = animal ? 0x5E8 : 0x2A4;
    inst = call(sub_0005DCC0, 0, 1, a, 0);
    if (!in_heap(inst)) {
        snprintf(why, why_n, "no memory for the person");
        goto out;
    }
    a[0] = ty;
    inst = call(!animal ? sub_00077D40 : kind == 3 ? sub_000749F0 : sub_000794C0, inst, 1, a, 1);
    if (animal)
        rider_slot_add(mgr);
    a[0] = tok;
    call(sub_000779B0, inst, 1, a, 1);
    if (vcall(inst, 0x44, 0, a) & 0xFF)
        vcall(inst, 0x150, 0, a);
    a[0] = S_CLOSE; call(sub_00085240, tok, 1, a, 1);
    /* on the end of the level's list */
    wr32(inst + 0x1B8, 0);
    last = rd32(mgr + 0x24);
    if (!in_heap(last))
        wr32(mgr + 0x24, inst);
    else {
        int guard = 0;
        while (in_heap(rd32(last + 0x1B8)) && guard++ < 2048)
            last = rd32(last + 0x1B8);
        wr32(last + 0x1B8, inst);
    }
    a[0] = s_scratch + SC_TABLE;
    call(sub_000764C0, inst, 1, a, 1);
    /* then, as the loader does for each of the level's people once all
     * are read (sub_00075C60, 0x00076238), its reset (vt+4, sub_00076DD0):
     * its team from the file, at its spawn point, the timers. Without it
     * the bring-in left a new person wherever its controller was last
     * (harness: always -11.3, 26.1 in Brimstone, whatever the point). */
    vcall(inst, 0x04, 0, a);
    call(sub_0005CA20, stream, 0, a, 1);
    stream = 0;
    fprintf(stderr, "[NPC] made %08X: type %s (%08X%s), table %s (%08X), team %d\n", inst, type, ty,
            animal ? ", an animal" : "", table, rd32(inst + 0x158), (int)rd32(inst + 0xB4));
    fflush(stderr);
    return inst;
out:
    if (stream)
        call(sub_0005CA20, stream, 0, a, 1);
    fprintf(stderr, "[NPC] %s %s: %s\n", type, table, why);
    fflush(stderr);
    return 0;
}

/* The game's heaps (memMemoryAllocator, the list at [0x0065A5F8], the
 * current one [0x0065A608]): base, size, the bump's use. Test command
 * "heaps". */
void rdr_heaps_dump(void)
{
    uint32_t h = rd32(0x0065A5F8u), cur = rd32(0x0065A608u);
    int guard = 0;
    for (; h && guard++ < 32; h = rd32(h + 4))
        fprintf(stderr, "[HEAP] %08X%s: base %08X size %u, bump %u used (%u left), flags %02X %02X %02X\n",
                h, h == cur ? " (current)" : "", rd32(h + 8), rd32(h + 0xC), rd32(h + 0x10),
                rd32(h + 0xC) - rd32(h + 0x10), *gm(h + 0x20), *gm(h + 0x21), *gm(h + 0x22));
    fflush(stderr);
}
