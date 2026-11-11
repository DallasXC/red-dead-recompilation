/*
 * rdr_settings.c - settings.ini: plain "key = value" lines.
 *
 * Where: RDR_SETTINGS if set (tests point it elsewhere),
 * else beside the executable.
 */
#include "rdr_settings.h"
#include "rdr_cheats.h"
#include "rdr_input.h"     /* RDR_VK_WHEEL_* */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

RdrSettings g_rdr_settings;

void rdr_settings_defaults(RdrSettings *s)
{
    memset(s, 0, sizeof *s);
    s->show_launcher = 1;               /* the launcher at every start, until switched off */
    s->fullscreen = 0;
    s->window_scale = 2;
    s->window_height = 720;
    s->widescreen = 1;
    s->stretch = 0;
    s->letterbox = 2;
    s->look2002_bands = 1;
    s->vsync = 1;
    s->fps_cap = 60;
    s->show_fps = 0;
    s->render_scale = 2;
    s->fxaa = 1;
    s->aniso = 16;
    s->sharp_filter = 0;
    s->sharp_text = 1;
    s->vibration = 100;
    s->deadzone = 0;
    s->invert_cam_x = 0;
    s->invert_cam_y = 0;
    s->mouse_aim = 0;
    s->mouse_sens = 100;
    s->mouse_duel_sens = 100;
    s->mouse_invert_y = 0;
    s->mouse_invert_x = 0;
    s->pause_on_focus = 1;
    s->kbm_player = 1;
    s->online_share_ip = 1;
    s->coop_players = 1;
    s->coop_split = 1;
    s->town_respawn = 1;
    s->update_check = 1;
    s->enemy_health = 100;
    s->pad_player[0] = 1;
    s->pad_player[1] = 2;
    s->pad_player[2] = 0;
    s->pad_player[3] = 0;
    {
        /* Mouse to aim, left button to fire (right trigger), right button
         * the left trigger; WASD to move. X is the game's action button
         * (doors, shops, talking, cover: E), Y reloads (R), B is the
         * melee and back button (F). */
        static const int def[RDR_KEY_COUNT] = {
            'W', 'S', 'A', 'D',
            0x20 /* Space */, 'F', 'E', 'R', 'Q', 'C',
            0x02 /* right mouse */, 0x01 /* left mouse */, 0x0D /* Enter */, 0x09 /* Tab */,
            0x26, 0x28, 0x25, 0x27 /* arrows */,
            0x10 /* Shift */, 0x04 /* middle mouse */,
            'Z' /* rifle zoom */,
            RDR_VK_WHEEL_UP, RDR_VK_WHEEL_DOWN /* weapons */,
            0x06 /* mouse 5 */, 0x14 /* Caps Lock */ /* Dead Eye */,
        };
        memcpy(s->keys, def, sizeof def);
    }
}

static const char *const s_key_names[RDR_KEY_COUNT] = {
    "key_move_up", "key_move_down", "key_move_left", "key_move_right",
    "key_a", "key_b", "key_x", "key_y", "key_black", "key_white",
    "key_lt", "key_rt", "key_start", "key_back",
    "key_dpad_up", "key_dpad_down", "key_dpad_left", "key_dpad_right",
    "key_l3", "key_r3",
    "key_zoom",
    "key_prev_weapon", "key_next_weapon",
    "key_deadeye", "key_deadeye2",
};

/* name, offset, min, max */
typedef struct { const char *key; size_t off; int lo, hi; } Field;
#define F(k, lo, hi) { #k, offsetof(RdrSettings, k), lo, hi }
static const Field s_fields[] = {
    F(show_launcher, 0, 1),
    F(fullscreen, 0, 1),
    F(borderless, 0, 1),
    F(dump_textures, 0, 1),
    F(window_scale, 1, 4),
    F(window_height, 0, 4320),
    F(widescreen, 0, 1),
    F(stretch, 0, 1),
    F(letterbox, 0, 2),
    F(look2002, 0, 1),
    F(look2002_bands, 0, 1),
    F(vsync, 0, 1),
    F(fps_cap, 0, 500),
    F(show_fps, 0, 1),
    F(render_scale, 1, 6),
    F(fxaa, 0, 1),
    F(aniso, 1, 16),
    F(sharp_filter, 0, 1),
    F(sharp_text, 0, 1),
    F(vibration, 0, 100),
    F(deadzone, 0, 40),
    F(invert_cam_x, 0, 1),
    F(buttons, 0, 2),
    F(invert_cam_y, 0, 1),
    F(mouse_aim, 0, 1),
    F(mouse_sens, 10, 400),
    F(mouse_duel_sens, 25, 400),
    F(mouse_invert_y, 0, 1),
    F(mouse_invert_x, 0, 1),
    F(pause_on_focus, 0, 1),
    F(kbm_player, 1, 4),
    F(kbm2_player, 0, 4),
    F(online_share_ip, 0, 1),
    F(coop_players, 1, 4),
    F(coop_split, 0, 3),
    F(coop_friendly_fire, 0, 1),
    F(enemy_health, 100, 500),
    F(town_guns, 0, 1),
    F(town_respawn, 0, 1),
    F(update_check, 0, 1),
    F(protect_friendly, 0, 1),
    F(coop_loadout, 0, 1),
    F(coop_duels, 0, 1),
    F(freecam_kbm, 0, 1),
    F(freecam_pads, 0, 15),
    { "preview_characters", offsetof(RdrSettings, preview), 0, 1 },
    { "player1_controller", offsetof(RdrSettings, pad_player) + 0 * sizeof(int), 0, 4 },
    { "player2_controller", offsetof(RdrSettings, pad_player) + 1 * sizeof(int), 0, 4 },
    { "player3_controller", offsetof(RdrSettings, pad_player) + 2 * sizeof(int), 0, 4 },
    { "player4_controller", offsetof(RdrSettings, pad_player) + 3 * sizeof(int), 0, 4 },
};
#undef F
#define NFIELDS (sizeof s_fields / sizeof s_fields[0])

static char s_path[1024];

const char *rdr_settings_path(void)
{
    const char *e;
    if (s_path[0])
        return s_path;
    e = getenv("RDR_SETTINGS");
    if (e && *e) {
        snprintf(s_path, sizeof s_path, "%s", e);
        return s_path;
    }
#if defined(_WIN32)
    {
        DWORD n = GetModuleFileNameA(NULL, s_path, (DWORD)sizeof s_path);
        char *slash;
        if (n == 0 || n >= sizeof s_path)
            s_path[0] = 0;
        slash = strrchr(s_path, '\\');
        if (slash)
            slash[1] = 0;
        else
            s_path[0] = 0;
    }
#else
    {
        ssize_t n = readlink("/proc/self/exe", s_path, sizeof s_path - 1);
        char *slash;
        s_path[n > 0 ? n : 0] = 0;
        slash = strrchr(s_path, '/');
        if (slash)
            slash[1] = 0;
        else
            s_path[0] = 0;
    }
#endif
    strncat(s_path, "settings.ini", sizeof s_path - strlen(s_path) - 1);
    return s_path;
}

int rdr_settings_load(RdrSettings *s)
{
    FILE *f;
    char line[4096];            /* game_folder and the mod lists run to 1023 */
    size_t i;

    int keys_version = 1;

    rdr_settings_defaults(s);
    f = fopen(rdr_settings_path(), "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *k = line, *v, *end;
        if (!eq || line[0] == '#' || line[0] == ';')
            continue;
        *eq = 0;
        v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        end = k + strlen(k);
        while (end > k && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (i = 0; i < RDR_KEY_COUNT; i++)
            if (!strcmp(k, s_key_names[i])) {
                int x = atoi(v);
                if (x >= 0 && x < 256)
                    s->keys[i] = x;
            }
        if (!strcmp(k, "keys_version"))
            keys_version = atoi(v);
        if (!strncmp(k, "coop_char", 9) && k[9] >= '2' && k[9] <= '4' && !k[10]) {
            char code[16];
            size_t n = strcspn(v, " \t\r\n");
            snprintf(code, sizeof code, "%.*s", (int)(n < 15 ? n : 15), v);
            s->coop_char[k[9] - '1'] = rdr_character_index(code);
            snprintf(s->coop_code[k[9] - '1'], sizeof s->coop_code[0], "%s", code);
        }
        if (!strcmp(k, "game_folder")) {
            size_t n = strcspn(v, "\r\n");
            snprintf(s->game_folder, sizeof s->game_folder, "%.*s", (int)(n < 1023 ? n : 1023), v);
        }
        if (!strcmp(k, "kb2_device") || !strcmp(k, "mouse2_device")) {
            char *dst = k[0] == 'k' ? s->kb2_device : s->mouse2_device;
            size_t n = strcspn(v, "\r\n");
            snprintf(dst, sizeof s->kb2_device, "%.*s", (int)(n < 255 ? n : 255), v);
        }
        if (!strcmp(k, "mods_off") || !strcmp(k, "texture_packs_off")) {
            char *dst = k[0] == 'm' ? s->mods_off : s->packs_off;
            size_t n = strcspn(v, "\r\n");
            snprintf(dst, sizeof s->mods_off, "%.*s", (int)(n < 1023 ? n : 1023), v);
        }
        if (!strcmp(k, "play_as")) {
            /* A character code ("mwyn"); a number is an older file's index
             * into a list that has since changed: the story default. */
            char code[16];
            size_t n = strcspn(v, " \t\r\n");
            snprintf(code, sizeof code, "%.*s", (int)(n < 15 ? n : 15), v);
            s->play_as = rdr_character_index(code);
            snprintf(s->play_as_code, sizeof s->play_as_code, "%s", code);
        }
        for (i = 0; i < NFIELDS; i++)
            if (!strcmp(k, s_fields[i].key)) {
                int x = atoi(v);
                if (x < s_fields[i].lo) x = s_fields[i].lo;
                if (x > s_fields[i].hi) x = s_fields[i].hi;
                *(int *)((char *)s + s_fields[i].off) = x;
            }
    }
    fclose(f);
    /* Files from before keys_version 2 kept the old defaults B = E,
     * X = R, Y = F, which had reload on E and the action button on R.
     * Left as they were, they move to the new ones; changed ones stay. */
    if (keys_version < 2 && s->keys[RDR_KEY_B] == 'E' && s->keys[RDR_KEY_X] == 'R'
        && s->keys[RDR_KEY_Y] == 'F') {
        s->keys[RDR_KEY_B] = 'F';
        s->keys[RDR_KEY_X] = 'E';
        s->keys[RDR_KEY_Y] = 'R';
    }
    return 1;
}

/* Written to settings.ini.tmp and moved over settings.ini once whole, so a
 * crash or a full disk mid-write leaves the old file, not half of one. */
static int settings_commit(FILE *f, const char *tmp, const char *path)
{
    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return 0; }
#if defined(_WIN32)
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tmp);
        return 0;
    }
#else
    if (rename(tmp, path) != 0) { remove(tmp); return 0; }
#endif
    return 1;
}

int rdr_settings_save(const RdrSettings *s)
{
    const char *path = rdr_settings_path();
    char tmp[1100];
    FILE *f;
    size_t i;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return 0;
    fprintf(f, "# Red Dead Revolver (recompiled) - settings\n"
               "# Written by the launcher (hold Shift while starting) and the\n"
               "# in-game menu (F1). Delete this file to go back to the defaults.\n");
    for (i = 0; i < NFIELDS; i++)
        fprintf(f, "%s = %d\n", s_fields[i].key,
                *(const int *)((const char *)s + s_fields[i].off));
    /* (a code not in the list now -- a mod's character with the mods
     * not read yet, or switched off -- is kept as it was) */
    fprintf(f, "play_as = %s\n",
            s->play_as > 0 && s->play_as < g_rdr_character_count
                ? g_rdr_characters[s->play_as].code : s->play_as_code);
    {
        int k;
        for (k = 1; k < 4; k++)
            fprintf(f, "coop_char%d = %s\n", k + 1,
                    s->coop_char[k] > 0 && s->coop_char[k] < g_rdr_character_count
                        ? g_rdr_characters[s->coop_char[k]].code : s->coop_code[k]);
    }
    fprintf(f, "# The game's files, when not in the Game folder (an extracted copy elsewhere):\n");
    fprintf(f, "game_folder = %s\n", s->game_folder);
    fprintf(f, "# Mods and texture packs switched off (the launcher's Mods page), '|' between them:\n");
    fprintf(f, "mods_off = %s\n", s->mods_off);
    fprintf(f, "texture_packs_off = %s\n", s->packs_off);
    fprintf(f, "# The second keyboard and mouse (Controls: picked in the game), Windows' names for them:\n");
    fprintf(f, "kb2_device = %s\n", s->kb2_device);
    fprintf(f, "mouse2_device = %s\n", s->mouse2_device);
    fprintf(f, "# Keys: Windows virtual-key codes (65 = A, 32 = Space, 1/2/4/5/6 = mouse buttons).\n");
    fprintf(f, "keys_version = 2\n");
    for (i = 0; i < RDR_KEY_COUNT; i++)
        fprintf(f, "%s = %d\n", s_key_names[i], s->keys[i]);
    return settings_commit(f, tmp, path);
}

int rdr_coop_players(void) { return g_rdr_settings.coop_players; }
int rdr_coop_split(void) { return g_rdr_settings.coop_split; }
int rdr_coop_friendly_fire(void) { return g_rdr_settings.coop_friendly_fire; }
int rdr_town_guns(void) { return g_rdr_settings.town_guns; }
int rdr_protect_friendly(void) { return g_rdr_settings.protect_friendly; }
int rdr_coop_loadout(void) { return g_rdr_settings.coop_loadout; }
int rdr_coop_duels(void) { return g_rdr_settings.coop_duels; }

/* The keyboard-and-mouse player's port, 0-3 (rdr_overrides.c). */
int rdr_kbm_port(void)
{
    int p = g_rdr_settings.kbm_player - 1;
    return p < 0 || p > 3 ? 0 : p;
}

int rdr_duel_mouse_scale(void)
{
    return g_rdr_settings.mouse_duel_sens;
}

/* For rdr_overrides.c, which does not see RdrSettings. */
int rdr_enemy_health_pct(void)
{
    return g_rdr_settings.enemy_health;
}

int rdr_pause_on_focus(void)
{
    return g_rdr_settings.pause_on_focus;
}

/* For the mouse look in rdr_overrides.c, which does not see RdrSettings. */
int rdr_mouse_settings(int *sens, int *invert_x, int *invert_y)
{
    *sens = g_rdr_settings.mouse_sens;
    *invert_x = g_rdr_settings.mouse_invert_x;
    *invert_y = g_rdr_settings.mouse_invert_y;
    return g_rdr_settings.mouse_aim;
}

static void put(const char *name, const char *value)
{
    if (getenv(name))
        return;                         /* set by hand: that wins */
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

static void put_always(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void puti(const char *name, int v)
{
    char b[32];
    snprintf(b, sizeof b, "%d", v);
    put(name, b);
}

/* The controller each player holds, with story co-op's extra players
 * given one when they have none: the lowest one nobody else holds (the
 * keyboard player needs none). Without it a third and fourth co-op player
 * had no controller until one was picked on the Controls page. */
void rdr_effective_pad_map(const RdrSettings *s, int out[4])
{
    int i, c, taken;
    for (i = 0; i < 4; i++)
        out[i] = s->pad_player[i];
    for (i = 1; i < 4 && i < s->coop_players; i++) {
        if (out[i] > 0 || i + 1 == s->kbm_player
            || (i + 1 == s->kbm2_player && (s->kb2_device[0] || s->mouse2_device[0])))
            continue;                   /* (the keyboards' players need none) */
        for (c = 1; c <= 4; c++) {
            int j;
            for (taken = 0, j = 0; j < 4; j++)
                if (out[j] == c)
                    taken = 1;
            if (!taken) {
                out[i] = c;
                break;
            }
        }
    }
}

void rdr_settings_resolve_characters(RdrSettings *s)
{
    int k;
    if (!s->play_as && s->play_as_code[0])
        s->play_as = rdr_character_index(s->play_as_code);
    for (k = 1; k < 4; k++)
        if (!s->coop_char[k] && s->coop_code[k][0])
            s->coop_char[k] = rdr_character_index(s->coop_code[k]);
}

/* Mods/Preview in the first mods folder (RDR_PREVIEW_DIR overrides). */
const char *rdr_preview_dir(void)
{
    static char dir[1100];
    const char *e = getenv("RDR_PREVIEW_DIR"), *m;
    if (e && *e)
        return e;
    m = getenv("RDR_MODS_BASE");
    if (!m || !*m)
        m = getenv("RECOMP_MODS_DIR");
    if (!m || !*m || !strcmp(m, "-"))
        m = "mods";
    snprintf(dir, sizeof dir, "%.*s/Preview", (int)strcspn(m, ";"), m);
    return dir;
}

/* The story's people alone (setup_preview.py --people): Mods/Story People,
 * beside Mods/Preview. Play As and story co-op offer them from there while
 * the preview characters are off (Mods/Preview has them too). */
const char *rdr_people_dir(void)
{
    static char dir[1200];
    const char *p = rdr_preview_dir(), *sl1 = strrchr(p, '/'), *sl2 = strrchr(p, '\\');
    const char *sl = sl1 > sl2 ? sl1 : sl2;
    snprintf(dir, sizeof dir, "%.*sStory People", sl ? (int)(sl - p) + 1 : 0, p);
    return dir;
}

int rdr_people_ready(void)
{
    char p[1300];
    FILE *f;
    snprintf(p, sizeof p, "%s/swat.dat", rdr_people_dir());
    f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* What the setup makes now (setup_preview.py's SETUP_VERSION): a folder
 * made by an older one lacks what was added or fixed since (2: Dirk and
 * Gilbert; Long Coat Red's head. 3: 19 more of the story's people. 4: archives
 * whose names the game can always read -- the crash taking off as Bat Boy.
 * 5: the story's people's names put right. 6: the preview's Teen Red). The setup writes "setup version N" into
 * the folder's preview.txt / people.txt; none: made before there was one. */
#define RDR_SETUP_VERSION 6
static int people_list_in(const char *dir, const char *file)
{
    char p[1300], line[256];
    int v = 0;
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", dir, file);
    f = fopen(p, "rb");
    if (!f)
        return -1;                      /* (not set up) */
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "setup version %d", &v) == 1)
            break;
    fclose(f);
    return v;
}

/* 1 when that folder is set up but made by an older setup. */
int rdr_preview_people_old(void)
{
    int v = people_list_in(rdr_preview_dir(), "preview.txt");
    return v >= 0 && v < RDR_SETUP_VERSION;
}

int rdr_people_old(void)
{
    int v = people_list_in(rdr_people_dir(), "people.txt");
    return v >= 0 && v < RDR_SETUP_VERSION;
}

/* This start plays with them (the mods folders begin with theirs). */
int rdr_preview_active(void)
{
    const char *m = getenv("RECOMP_MODS_DIR"), *d = rdr_preview_dir();
    return m && !strncmp(m, d, strlen(d)) && (m[strlen(d)] == ';' || !m[strlen(d)]);
}

int rdr_preview_ready(void)
{
    char p[1200];
    FILE *f;
    snprintf(p, sizeof p, "%s/swat.dat", rdr_preview_dir());
    f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

void rdr_settings_apply_env(const RdrSettings *s)
{
    /* The mods that are on (rdr_mods.c), after the preview's folder when
     * that is on: a file in both is the preview's. RDR_MODS_BASE and
     * RDR_TEXPACK_BASE keep the player's own folders (set once: a game
     * started again for online play inherits all of these). */
    {
        static char mods[8192], tex[8192];
        const char *m = getenv("RDR_MODS_BASE");
        char first[1200] = "";
        if (!m) {
            m = getenv("RECOMP_MODS_DIR");
            put_always("RDR_MODS_BASE", m && *m ? m : "-");      /* "-": none */
        }
        m = getenv("RDR_TEXPACK_BASE");
        if (!m) {
            m = getenv("RECOMP_TEXTURE_PACK");
            put_always("RDR_TEXPACK_BASE", m && *m ? m : "-");
        }
        if ((s->preview || (getenv("RDR_PREVIEW") && atoi(getenv("RDR_PREVIEW")))) && rdr_preview_ready())
            snprintf(first, sizeof first, "%s", rdr_preview_dir());
        else if (rdr_people_ready())
            snprintf(first, sizeof first, "%s", rdr_people_dir());   /* the story's people alone */
        rdr_mods_compose(s, first, mods, sizeof mods, tex, sizeof tex);
        if (mods[0])
            put_always("RECOMP_MODS_DIR", mods);
        if (rdr_texpacks_base())
            put_always("RECOMP_TEXTURE_PACK", tex[0] ? tex : "-");
        fprintf(stderr, "[MODS] mods: %s\n[MODS] texture packs: %s\n", mods[0] ? mods : "(none)",
                tex[0] ? tex : "(none)");
    }
    puti("RECOMP_FULLSCREEN", s->fullscreen);
    puti("RECOMP_BORDERLESS", s->borderless);
    if (s->dump_textures) {
        /* texture_dump\ beside settings.ini (the game's folder) */
        char dir[1024];
        const char *sp = rdr_settings_path();
        const char *sl1 = strrchr(sp, '\\'), *sl2 = strrchr(sp, '/');
        const char *sl = sl1 > sl2 ? sl1 : sl2;
        int n = sl ? (int)(sl - sp) + 1 : 0;
        snprintf(dir, sizeof dir, "%.*stexture_dump", n, sp);
        put("RECOMP_TEXTURE_DUMP", dir);
    }
    puti("RECOMP_WINDOW_SCALE", s->window_scale);
    if (s->window_height >= 240) {
        char b[32];
        snprintf(b, sizeof b, "%dx%d", (s->window_height * 16 + 8) / 9, s->window_height);
        put("RECOMP_WINDOW_SIZE", b);
    }
    puti("RECOMP_WIDESCREEN", s->widescreen);
    put("RECOMP_ASPECT", s->stretch ? "stretch" : "auto");
    puti("RECOMP_VSYNC", s->vsync);
    /* fps_cap is the frame rate: 30 caps the presenter, anything else
     * sets the vblank rate the game flips on (0, from older files: 60). */
    puti("RECOMP_FPS_CAP", s->fps_cap == 30 ? 30 : 0);
    puti("RECOMP_VBLANK_HZ", s->fps_cap == 30 || !s->fps_cap ? 60 : s->fps_cap);
    puti("RECOMP_RENDER_SCALE", s->render_scale);
    puti("RECOMP_FXAA", s->fxaa);
    puti("RECOMP_ANISO", s->aniso);
    put("RECOMP_PRESENT_FILTER", s->sharp_filter ? "nearest" : "linear");
    /* Mouse and keyboard play is the title's own (rdr_input.c): it
     * replaces the toolkit's keyboard map, so that is switched off. */
    if (s->mouse_aim)
        put("RECOMP_KEYBOARD", "0");
    {
        /* Players: each port reads its player's controller; as many
         * controllers are plugged into the console as the last player
         * with one (or the keyboard). */
        char b[32];
        int i, n = s->kbm_player, pm[4];
        rdr_effective_pad_map(s, pm);
        for (i = 0; i < 4; i++)
            if (pm[i] > 0 && i + 1 > n)
                n = i + 1;
        if (s->kbm2_player > n && (s->kb2_device[0] || s->mouse2_device[0]))
            n = s->kbm2_player;
        if (s->coop_players > n)
            n = s->coop_players;
        snprintf(b, sizeof b, "%d,%d,%d,%d", pm[0], pm[1], pm[2], pm[3]);
        put("RECOMP_PAD_MAP", b);
        puti("RECOMP_KEYBOARD_PORT", s->kbm_player);
        puti("RECOMP_USB_PADS", n < 1 ? 1 : n);
    }
}
