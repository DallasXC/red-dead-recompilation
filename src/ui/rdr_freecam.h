/* rdr_freecam.h - a free camera (see rdr_freecam.c). */
#ifndef RDR_FREECAM_H
#define RDR_FREECAM_H

#include <stdint.h>
#include "input/xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

extern volatile int g_rdr_freecam;     /* on/off: F6, or the in-game menu */

int  rdr_freecam_active(void);
/* The pad filter, for each port: while on, the controllers and the
 * keyboard set to fly it drive the camera, not the game. `kb`: on the
 * keyboard's port while on, the keyboard and mouse's own state (not merged
 * into st), else NULL. */
void rdr_freecam_pad(unsigned port, XBOX_INPUT_STATE *st, const XBOX_INPUT_STATE *kb);
/* 1 while on and the mouse flies it (not the game's camera). */
int  rdr_freecam_takes_mouse(void);
/* A line for the picture's corner for a few seconds after it is switched
 * on or off: who flies it, who plays. 0 when there is none to show. */
int  rdr_freecam_hint(char *out, int n);
/* Once a presented frame: move the camera by the frame's time. */
void rdr_freecam_tick(void);
/* The hook (rdr_hooks.c): the camera is being set; put ours in. */
void rdr_freecam_apply(void);

#ifdef __cplusplus
}
#endif
#endif
