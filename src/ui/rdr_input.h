/*
 * rdr_input.h - mouse and keyboard play, made for this game.
 *
 * The mouse drives the right stick (the camera and the aim) from raw mouse
 * movement; every pad control can be bound to a key or a mouse button
 * (RdrSettings.keys). Windows only: it reads the game window's messages.
 */
#ifndef RDR_INPUT_H
#define RDR_INPUT_H

#include <stdint.h>
#include "input/xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The mouse wheel, bindable like a key: codes Windows leaves unassigned. */
#define RDR_VK_WHEEL_UP    0x0E
#define RDR_VK_WHEEL_DOWN  0x0F

/* The game window's messages, from the overlay's hook (window thread);
 * menu_open 2 is the launcher's window (rebinding only). Returns nonzero
 * to keep a message from the game. */
int  rdr_input_msg(void *hwnd, unsigned msg, uintptr_t w, intptr_t l, int menu_open);
/* The menu opened or closed: the pointer is freed or taken back. */
void rdr_input_menu(void *hwnd, int open);
/* Merge mouse and keys into the pad state (USB thread). */
void rdr_input_merge(XBOX_INPUT_STATE *st);
/* Mouse look into the camera (rdr_overrides.c): the counts since the last
 * call; 0 when mouse play is off. */
int  rdr_input_take_look(long *dx, long *dy);
/* Tests: as if the mouse had moved. */
void rdr_input_test_mouse(long dx, long dy);
/* Pointing at menu items: the window's size each frame (overlay); the
 * pointer in the menus' 640x448 space, and (u, v) across from the image's
 * centre and down from its top in image heights, and a counter that moves with it
 * (0 when there is no pointer); the hook's report that a page it can
 * point at is up, and whether the pointer is over an item. */
void rdr_input_set_view(int w, int h);
int  rdr_input_pointer(float *sx, float *sy, float *u, float *v, unsigned *gen);
void rdr_input_pointer_page(int hovering);

/* Clickable button prompts (see rdr_input.c): called each frame while some
 * are on screen, with the one under the pointer or 0. */
enum { RDR_HUD_A = 1, RDR_HUD_B, RDR_HUD_X, RDR_HUD_Y, RDR_HUD_LEFT, RDR_HUD_RIGHT, RDR_HUD_START,
       RDR_HUD_NOTHING /* a click that presses nothing */,
       RDR_HUD_LEFT2, RDR_HUD_RIGHT2 /* two steps (the desk: the far one) */,
       RDR_HUD_DOWN /* d-pad down (the desk: back to the book) */,
       RDR_HUD_UP };
void rdr_input_hud(int code);
void rdr_input_desk(void);          /* the front end's desk is up (no mouse steps) */
void rdr_input_swf_press(int code);  /* one press a Flash menu sees (~180 ms) */
int  rdr_input_swf_busy(void);
void rdr_input_swf_cancel(void);
void rdr_input_step(int code);       /* one d-pad step: RDR_HUD_LEFT / RIGHT / UP / DOWN */
void rdr_input_steps(void);         /* this frame: the mouse steps the highlight */     /* one d-pad step, RDR_HUD_LEFT / RIGHT */
/* Tests: the pointer at a place in the menus' space. */
void rdr_input_test_pointer(float sx, float sy);
void rdr_input_test_pointer_px(float px, float py);   /* in a 640x360 frame */
void rdr_input_test_stick(float x, float y, int lt, int ms);
/* Tests: the second controller's left stick (story co-op). */
void rdr_input_test_stick2(float x, float y, int ms);
void rdr_input_test_port2(XBOX_INPUT_STATE *st);
/* Typing a name into the profile sheet's letter grid (rdr_overrides.c):
 * the grid is up (call every frame it is); the next key typed -- 'A'..'Z',
 * '>' (space), '<' (delete), RDR_TEXT_END or RDR_TEXT_CANCEL -- 0 when
 * none; make one press of A (1) or B (2). push: tests, as if typed. */
#define RDR_TEXT_END     1
#define RDR_TEXT_CANCEL  2
void rdr_input_text_mode(void);
int  rdr_input_text_take(void);
void rdr_input_text_press(int button);
/* Whether the game's window lost the focus since the last call (Windows). */
int  rdr_input_focus_lost_take(void);
void rdr_input_text_push(int c);
/* For rebinding: the next key or mouse button pressed (0 while waiting). */
void rdr_input_capture_next(void);
int  rdr_input_captured(void);     /* the key, -1 cancelled (Esc), -2 unbind (Delete), 0 none */
int  rdr_input_capturing(void);

/* A second keyboard and mouse for another player (local co-op, Windows):
 * rdr_kbm2_port() is that player's port (0-3) or -1 (not in use).
 * Picking: the next key pressed names the keyboard, the next click the
 * mouse (Esc on the new keyboard: none; Esc elsewhere cancels).
 * rdr_input_kb2_take() hands the picked device names over once;
 * rdr_input_kb2_changed() after the settings' names change. */
int  rdr_kbm2_port(void);
void rdr_input_merge2(XBOX_INPUT_STATE *st);
int  rdr_input_take_look2(long *dx, long *dy);
void rdr_input_look_share2(long dx, long dy);
void rdr_input_kb2_pick(void);
int  rdr_input_kb2_picking(void);     /* 0 no, 1 waiting for a key, 2 for a click */
void rdr_input_kb2_cancel(void);
int  rdr_input_kb2_take(char *kbd, char *mouse, int n);
void rdr_input_kb2_changed(void);
void rdr_input_test_kb2_key(int vk, int ms);
void rdr_input_test_kb2_mouse(long dx, long dy);
/* A key's name, for the bindings page. */
const char *rdr_key_name(int vk);
/* The control's name ("A button", "Move forward", ...). */
const char *rdr_control_name(int control);

#ifdef __cplusplus
}
#endif

#endif
