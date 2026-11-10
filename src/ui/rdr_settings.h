/*
 * rdr_settings.h - the player's settings, kept in settings.ini.
 *
 * The launcher edits them before the game starts, the in-game overlay while
 * it runs. At startup they become the toolkit's RECOMP_* switches; during
 * play the overlay changes the live values directly (nvgl_options() and the
 * input filter), so most take effect at once.
 */
#ifndef RDR_SETTINGS_H
#define RDR_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controls a key can drive, for mouse-and-keyboard play (rdr_input.c). */
enum {
    RDR_KEY_MOVE_UP, RDR_KEY_MOVE_DOWN, RDR_KEY_MOVE_LEFT, RDR_KEY_MOVE_RIGHT,
    RDR_KEY_A, RDR_KEY_B, RDR_KEY_X, RDR_KEY_Y, RDR_KEY_BLACK, RDR_KEY_WHITE,
    RDR_KEY_LT, RDR_KEY_RT, RDR_KEY_START, RDR_KEY_BACK,
    RDR_KEY_DPAD_UP, RDR_KEY_DPAD_DOWN, RDR_KEY_DPAD_LEFT, RDR_KEY_DPAD_RIGHT,
    RDR_KEY_L3, RDR_KEY_R3,
    RDR_KEY_ZOOM,           /* rifle zoom: the black button, on a key of its own */
    RDR_KEY_PREV_WEAPON,    /* d-pad left: the previous weapon (wheel up) */
    RDR_KEY_NEXT_WEAPON,    /* d-pad right: the next weapon (wheel down) */
    RDR_KEY_DEADEYE,        /* Dead Eye / special move, two keys of its own */
    RDR_KEY_DEADEYE2,
    RDR_KEY_COUNT
};

typedef struct {
    /* Launcher */
    int show_launcher;      /* open the launcher at every start */

    /* Display */
    int fullscreen;         /* borderless fullscreen */
    int borderless;         /* a window without frame or title bar */
    int play_as;            /* story character: g_rdr_characters index, 0 = default */
    int coop_char[4];       /* story co-op players 2-4 ([1]..[3]): the same list, 0 = as player 1 */
    int dump_textures;      /* write every texture to texture_dump\ (packs) */
    int window_scale;       /* window size: 1-4 times 480 lines (older files) */
    int window_height;      /* window size: 720, 900, 1080, 1440, 2160 lines
                             * at 16:9; 0 = window_scale */
    int widescreen;         /* tell the game the TV is 16:9 (it renders wider) */
    int stretch;            /* fill the window instead of keeping the shape */
    int letterbox;          /* the game's black bars, top and bottom: 2 always (as
                             * the game), 1 in cutscenes only, 0 never */
    int look2002;           /* 1: the January 2002 preview's HUD (rdr_hud2002.cpp) */
    int look2002_bands;     /* 1: with its blue bands at the top and bottom */
    int vsync;
    int fps_cap;            /* frame rate: 30, 60 (the console's), 120 ... 500 */
    int show_fps;           /* frame counter in the corner (F2) */

    /* Graphics */
    int render_scale;       /* 1-6: internal resolution, times 480 lines */
    int fxaa;
    int aniso;              /* 1, 2, 4, 8, 16 */
    int sharp_filter;       /* nearest-neighbour upscaling */
    int sharp_text;         /* the game's lettering redrawn 4x finer (rdr_sharptext.c) */

    /* Controls */
    int vibration;          /* 0-100 % */
    int deadzone;           /* 0-40 % of the stick's travel */
    int invert_cam_x;       /* right stick, horizontal */
    int buttons;            /* button pictures: 0 by the controller, 1 Xbox, 2 PlayStation */
    int invert_cam_y;       /* right stick, vertical */
    int mouse_aim;          /* mouse and keyboard play */
    int mouse_sens;         /* 10-400 % */
    int mouse_duel_sens;    /* 25-400 %: a duel's aim, on top of mouse_sens */
    int mouse_invert_y;     /* forward looks down */
    int mouse_invert_x;     /* right turns left */
    int pause_on_focus;     /* the pause menu when the window loses focus */
    /* Players (Showdown's split screen, co-op): which player the keyboard
     * and mouse are (1-4), and which controller each player holds (1-4, in
     * the order Windows connected them; 0 none). */
    int kbm_player;
    int pad_player[4];
    /* A second keyboard and mouse (Windows): the player they control (0
     * none, 1-4), and the two devices, by Windows' names for them. */
    int kbm2_player;
    /* Hosting online: the internet's view of this PC (its public IP
     * address) in the join code, for play over the internet with a
     * forwarded port. Off: the code holds only home-network and VPN
     * addresses. */
    int online_share_ip;
    /* Story co-op: players in story levels (1 = off, 2-4), and the screen
     * (0 shared, 1 split top/bottom, 2 split side by side; 3-4: quarters). */
    int coop_players;
    int coop_split;           /* 3: each player's view in a window of its own */
    int coop_friendly_fire;   /* 0: co-op players' shots pass each other by */
    int enemy_health;         /* story enemies' health, % (100: as the game has it) */
    int town_guns;            /* 1: guns drawn where the story keeps them holstered (Brimstone) */
    int town_respawn;         /* with town_guns: Brimstone's people shot dead come back */
    int update_check;         /* the launcher asks GitHub for a newer release */
    int coop_loadout;         /* story co-op: 0 shared (player 1 buys and picks), 1 each player's own */
    int coop_duels;           /* story co-op: 0 player 1 duels alone, 1 every player duels */
    int protect_friendly;     /* 1: the story's friendly people (not the players) take no damage */
    int preview;              /* 1: the 2002 preview's characters and stages (Mods/Preview) */
    /* The free camera: who flies it. 0 flies the camera, 1 plays on (its
     * player keeps moving under the free camera). The keyboard and mouse,
     * and each player's controller (bit n: player n+1's). */
    int freecam_kbm;
    int freecam_pads;
    /* The codes play_as and coop_char were saved as: a character a mod
     * adds is in the list only once the mods are read (after the
     * settings), so its code waits here (rdr_settings_resolve_characters). */
    char play_as_code[16];
    char coop_code[4][16];
    /* Mods and texture packs switched off (rdr_mods.c): names with '|'
     * between them, "<loose>" the files put straight into the folder. */
    char mods_off[1024];
    char packs_off[1024];
    /* The play folder's game where it is already unpacked elsewhere (the
     * launcher's Install page: "Use an extracted copy"); "" = Game\. */
    char game_folder[1024];
    char kb2_device[256];
    char mouse2_device[256];
    int keys[RDR_KEY_COUNT];  /* key (Windows virtual-key code) per control */
} RdrSettings;

/* Defaults, then whatever the file has. Returns 1 if the file existed. */
int  rdr_settings_load(RdrSettings *s);
int  rdr_settings_save(const RdrSettings *s);
void rdr_settings_defaults(RdrSettings *s);
/* The file's path (for the About page). */
const char *rdr_settings_path(void);
/* Set the RECOMP_* switches the toolkit reads at startup. A variable the
 * player set by hand wins over the file. */
void rdr_settings_apply_env(const RdrSettings *s);
void rdr_effective_pad_map(const RdrSettings *s, int out[4]);
/* Play As and the co-op choices from their saved codes, once the mods'
 * characters are in the list. */
void rdr_settings_resolve_characters(RdrSettings *s);
/* The folder of the 2002 preview's characters and stages (made in the
 * launcher: Mods/Preview), and whether it is there. */
const char *rdr_preview_dir(void);
int rdr_preview_ready(void);
int rdr_preview_active(void);
/* Setting them up from the preview's disc image (rdr_preview_setup.c):
 * picked in a window, converted in the background. State: 0 idle, 1
 * running, 2 done, 3 failed (text says why). */
int rdr_preview_pick_image(char *out, int n);
int rdr_preview_setup_start(const char *image);
int rdr_preview_setup_state(int *pct, char *text, int n);
int rdr_preview_remove(void);
/* The story's people alone (Mods/Story People), set up with no disc:
 * rdr_preview_setup_start(NULL). */
const char *rdr_people_dir(void);
int rdr_people_ready(void);
/* Set up, but with an older list of the story's people (some missing). */
int rdr_preview_people_old(void);
int rdr_people_old(void);
/* Mods and texture packs (rdr_mods.c). kind: 0 a mod (a folder in Mods),
 * 1 the files directly in Mods, 2 a texture pack (a folder in
 * Mods\textures), 3 the textures directly there. */
typedef struct { char name[64]; int kind; int on; } RdrModEntry;
int  rdr_mods_list(const RdrSettings *s, RdrModEntry *out, int max);
void rdr_mods_set(RdrSettings *s, const RdrModEntry *e, int on);
void rdr_mods_compose(const RdrSettings *s, const char *first, char *mods, size_t mn, char *tex, size_t tn);
const char *rdr_mods_base(void);
const char *rdr_texpacks_base(void);
void rdr_open_folder(const char *path);
/* Online play: a hash of what these mods folders serve (';' between them),
 * and of what this machine's would be with the preview's on or off. */
unsigned rdr_mods_fingerprint(const char *dirs);
unsigned rdr_mods_fingerprint_for(const RdrSettings *s, int preview);

/* A file picked in a window (Windows' filter pairs; elsewhere zenity or the
 * env variable named). */
int rdr_pick_file(const char *title, const char *filter, const char *env, char *out, int n);
/* The play folder with no game yet (main.c), and installing it from the
 * Xbox disc image (the launcher's install page). State as above. */
int rdr_game_missing(void);
int rdr_game_install_pick(char *out, int n);
int rdr_game_install_start(const char *image);
int rdr_game_install_state(int *pct, char *text, int n);
/* An extracted copy instead (its default.xbe picked): 1 used, -2 another
 * game, 0 not a game folder. */
int rdr_game_use_folder(const char *xbe_path);
int rdr_game_pick_xbe(char *out, int n);

/* The one live copy, shared by the launcher, the overlay and the filters. */
extern RdrSettings g_rdr_settings;

#ifdef __cplusplus
}
#endif

#endif
