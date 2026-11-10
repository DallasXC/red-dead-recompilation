/*
 * rdr_cheats.h - Red Dead Revolver's own cheats, switched from outside.
 *
 * The game keeps the pause menu's cheats as bits of one word in the player's
 * profile (see rdr_cheats.c). Setting a bit is exactly what the menu's
 * checkbox does, so a cheat behaves as it does on the console -- and the
 * menu itself never has to be unlocked.
 */
#ifndef RDR_CHEATS_H
#define RDR_CHEATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int bit;                 /* in the profile's cheat word */
    const char *name;        /* as the game calls it */
    const char *what;        /* one line for the overlay */
} RdrCheat;

extern const RdrCheat g_rdr_cheats[];
extern const int g_rdr_cheat_count;

/* 1 while a player profile is loaded (the word exists). */
int  rdr_cheats_available(void);
int  rdr_cheat_get(int bit);
void rdr_cheat_set(int bit, int on);

/* Our own cheats, beyond the game's. */

/* Every mission's rating targets (time, damage taken, accuracy, combo)
 * opened wide while on, so the results screen rates the mission Excellent
 * and pays out what that unlocks. The targets are restored when it is
 * switched off. Applied by rdr_cheats_tick(). */
extern int g_rdr_always_excellent;

/* Every gun's clip and ammo reserve held full while on: no reloading, and
 * the count never goes down. Applied by rdr_cheats_tick(). */
extern int g_rdr_infinite_ammo;
extern int g_rdr_duel_win;            /* Always Win Duels (rdr_overrides.c) */

/* People and animals stop where they are: no moving, shooting or noticing
 * you (rdr_overrides.c skips their per-frame update). */
extern int g_rdr_freeze_npcs;

/* The money the player carries in a mission (what the HUD shows). -1 when
 * no player is in a mission. */
int  rdr_money_get(void);
int  rdr_money_set(int amount);      /* 1 when it was set */

/* Go to level (work in progress, test command only -- see rdr_cheats.c):
 * the level being played swapped for another of the same kind. */
typedef struct {
    char name[24];           /* lvl_ghost */
    int  var;                /* story: the chapter's variation (lvl_tomb 2) */
    char label[48];          /* for the list */
} RdrLevel;
/* 0 out of a level, 1 a story chapter, 2 Showdown. */
int  rdr_level_kind(void);
/* The level now ("lvl_ghost", variation); 0 out of a level. */
int  rdr_level_current(char *name, int n, int *var);
/* The levels of the current kind: the story's chapters (and the three on
 * the disc no chapter uses), or Showdown's stages (the 14 and any in the
 * mods folder's lvls_m2). Returns the count. */
int  rdr_level_list(int kind, RdrLevel *out, int max);
/* Asks for the switch; done on the next frame. 0 out of a level. */
int  rdr_level_go(const char *name, int var);

/* Tests only (RDR_OVERLAY_FLAG): empty the current gun as firing would;
 * log its counts. */
void rdr_ammo_test_drain(void);
void rdr_ammo_test_report(void);

/* 1 in the game's menus: the pause menu, or the front end (no player). */
int rdr_menu_mode(void);
/* 1 while the pause menu is up. */
int rdr_pause_menu(void);
/* The port the keyboard and mouse drive now (see rdr_cheats.c). */
int rdr_keyboard_port(int kbm_port);
/* In a menu whose controller is gone: the menus read the keyboard's port. */
int rdr_menu_port_take(int port);
/* 1 with a player loaded (a level, paused or not). */
int rdr_in_level(void);
/* The game's last frame time, in seconds. */
double rdr_frame_seconds(void);

/* Once a frame, from the presenter: keeps the custom cheats applied. */
void rdr_cheats_tick(void);         /* on the game's thread, at each pad read */

/* Showdown characters and stages, journal pages and cheats: everything in
 * the game's Unlockables table, for every profile. Returns how many were
 * newly unlocked, -1 when the game has not loaded its table yet. */
int rdr_unlock_everything(void);
/* Undo: lock again exactly what Unlock Everything unlocked (kept in
 * unlock_undo.txt beside settings.ini); 0 when there is nothing to undo. */
int rdr_unlock_can_undo(void);
int rdr_unlock_undo(void);

/* Showdown power-ups worn as cheats: bit i of g_rdr_powerup_mask wears
 * g_rdr_powerups[i] on every played character (see rdr_overrides.c).
 * group: 0 the character, 1 a weapon, 2 everyone. */
typedef struct { const char *code, *name; int group; } RdrPowerup;
extern const RdrPowerup g_rdr_powerups[];
extern const int g_rdr_powerup_count;
extern volatile unsigned long long g_rdr_powerup_mask;
void rdr_powerups_test_init(void);

/* Play as: the story's player character swapped at level start. */
typedef struct { const char *code, *name; } RdrCharacter;
#define RDR_MAX_CHARACTERS 240
extern RdrCharacter g_rdr_characters[RDR_MAX_CHARACTERS];
extern int g_rdr_character_count;
/* The game's own (the first g_rdr_character_retail), then the characters
 * the mods add (rdr_modchars.c), named as on the line-up. */
extern const int g_rdr_character_retail;
int rdr_characters_scan_mods(const char *game_dir, const char *mods_dirs);
const char *rdr_play_as(void);
void rdr_play_as_spawn(const char *code);   /* a co-op player's own, NULL: player 1's */
const char *rdr_coop_char(int k);          /* player k+1's co-op choice, "" = player 1's */
/* In a story level whose players are not what Story co-op says now: 1. */
int rdr_coop_changed(void);
/* Starts the chapter afresh so its players are made as Story co-op says
 * (rdr_overrides.c; Restart Level does it too when co-op changed). */
int rdr_coop_apply_now(void);
/* The table index of a code ("mwyn"), 0 (story default) when unknown. */
int rdr_character_index(const char *code);

/* The spawner (rdr_overrides.c): the level's characters that wait to come
 * in, by kind; the menu asks for count of kind index (rdr_spawn_kinds'
 * order, -1 any) in front of player (0-based). Online the request goes to
 * every machine with this player's input. */
typedef struct { char type[16]; char name[40]; int waiting, enemy, animal; } RdrSpawnKind;
int rdr_spawn_kinds(RdrSpawnKind *out, int max);
/* New people (rdr_npc.c): the level's ways of acting -- one per AI table its
 * people use -- and a request to make someone. who: an index into
 * g_rdr_characters (1..), or 1000 + an acts index for that person's own
 * character. side: 0 as the level has them, 1 against the players, 2 with
 * them. In front of player (0-3); online, with that player's input. */
typedef struct { char table[48]; char type[16]; char name[40]; int team, enemy, animal; } RdrNpcAct;
int rdr_npc_acts(RdrNpcAct *out, int max);
/* How many more people or animals the level can have in at once. */
int rdr_npc_room(void);
void rdr_npc_ask(int who, int act, int side, int player);
void rdr_npc_opts_ask(int count, int place, int arms, int player);
/* "Acts like" someone from another level: the story levels' people
 * (rdr_npc_others.inc); idx -1 for this level's own. Sent before the make. */
void rdr_npc_other_ask(int idx, int player);
int rdr_npc_others_count(void);
int rdr_npc_other_info(int i, const char **where, const char **type, int *team, int *armed);
int rdr_npc_beasts_count(void);
int rdr_npc_beast_info(int i, const char **where, const char **name);
void rdr_npc_remove_ask(int player);
void rdr_spawn_ask(int kind, int count, int player);
/* Give Weapon: the game's weapons (swat.guns), whether a player (0-3)
 * has one, and a request to give one (online: with the player's input). */
int rdr_weapon_count(void);
int rdr_weapon_name(int i, char *out, int n);
int rdr_weapon_owned(int player, int i);
void rdr_give_weapon_ask(int weapon, int player);
void rdr_fill_ammo_ask(int player);       /* fill every carried gun (a command) */
/* The Co-op Shop (story co-op, each player's own weapons and money) */
int rdr_weapon_price(int i);            /* the game's price, -1 none */
int rdr_coop_player_money(int player);
void rdr_coop_buy_ask(int weapon, int player);
void rdr_coop_loadouts_clear(void);
const char *rdr_type_name(const char *code);   /* "ncb" -> "Smitty"; NULL not known */
int rdr_coop_count(void);
/* The game's own menu sounds (its menus' table, sub_0009B400): played on
 * the game's thread at its next pad read. */
enum { RDR_SOUND_ACCEPT = 1, RDR_SOUND_MOVE = 2, RDR_SOUND_BACK = 3 };
void rdr_ui_sound(int which);


/* The 2002 preview's HUD (rdr_overrides.c reads, rdr_hud2002.cpp draws). */
typedef struct {
    uint32_t level;             /* the level loaded (its object): a new one restarts the clock */
    int hud_on;                 /* the game's HUD is up (not a cutscene) */
    int health_on, valor_on, ammo_on;   /* the parts the game shows now */
    float health, health_max, health_shown;
    float valor, valor_max;     /* Dead Eye: how full, in tanks; the tanks */
    int money;                  /* -1: none (Showdown) */
    int weapon;                 /* the weapon held (-1 none) */
    char weapon_name[40];
    int hands;                  /* guns held: 1, or 2 (one in each hand) */
    int clip, loaded, left;     /* the (right hand's) gun: clip size, loaded, reserve (-1 endless) */
    int clip2, loaded2;         /* the other hand's */
} RdrHud2002;
int rdr_hud2002_read(RdrHud2002 *o);
uint32_t rdr_hud2002_hide_mask(void);
int rdr_title_phase(void);           /* 1 the title, 2 Start pressed there, 0 not */
int rdr_buttons_style(void);          /* 1 Xbox, 2 PlayStation, 3 keys (settings: buttons) */

#ifdef __cplusplus
}
#endif

#endif
