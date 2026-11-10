/*
 * rdr_ui.h - the launcher and the in-game overlay (C interface).
 */
#ifndef RDR_UI_H
#define RDR_UI_H

#include "rdr_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether to show the launcher this start: the first start, Shift held,
 * --launcher on the command line, or "show at every start". */
int  rdr_launcher_wanted(int argc, char **argv, int settings_existed);

/* The settings window, before the game starts. Edits g_rdr_settings and
 * saves it. Returns 1 to start the game, 0 to quit. */
int  rdr_launcher_run(void);

/* The overlay: F1 (or both sticks clicked) opens it during play, F2 shows
 * the frame counter. Hooks the window, the controller filter and the GPU
 * presenter; call once before the game starts. */
void rdr_overlay_install(void);

#ifdef __cplusplus
}
#endif

#endif
