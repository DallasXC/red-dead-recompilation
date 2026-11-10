/*
 * rdr_ui_game.h - the menus drawn with the game's own art.
 *
 * The F1 menu and the launcher look like Red Dead Revolver's pause menu and
 * options pages: its fonts (berrylium for the rows, regulatorcondensed for
 * the titles, the logo's typeface), the options page's black background
 * with its scroll rules, the slider's chevrons and red X. All of it is read
 * from the player's swat.dat and front.dat (rdr_gameart.c); the fonts'
 * pages are enlarged four times with clean edges so text stays sharp on big
 * screens. Without the game's files the menus keep their own parchment look.
 *
 * Positions are in a 1280 x 720 design space (the game's 640 x 448 menus
 * as they appear on a 16:9 picture), fitted to the window.
 */
#ifndef RDR_UI_GAME_H
#define RDR_UI_GAME_H

#include "imgui.h"

extern "C" void rdr_ui_set_game_dir(const char *dir);

namespace rdrui {
namespace game {

/* kTextPS: berrylium, the PS2 version's button pictures (Text and Width
 * take them for kText's when the buttons are PlayStation's) */
enum Font { kText, kTitle, kLogo, kTextPS, kFontCount };

/* The game's colours */
static const ImU32 kWhite = IM_COL32(240, 240, 240, 255);   /* the item under the cursor */
static const ImU32 kGrey = IM_COL32(128, 128, 128, 255);    /* the others, on an options page */
static const ImU32 kDim = IM_COL32(200, 200, 200, 150);     /* the others, in the pause menu */
static const ImU32 kRed = IM_COL32(147, 19, 0, 255);        /* titles, the slider's X */
static const ImU32 kHint = IM_COL32(208, 196, 172, 255);

/* Reads the art (once; later calls return the first answer). */
bool Load();
/* Whether the art is loaded and on the GPU (in this GL context). */
bool Ready();
/* Puts the art on the GPU of the current GL context / takes it off. */
bool Upload();
bool Retry();
void Release();

/* The design space fitted into a w x h picture (16:9, centred). */
struct Space {
    float ox = 0, oy = 0, s = 1;
    float X(float x) const { return ox + x * s; }
    float Y(float y) const { return oy + y * s; }
    ImVec2 P(float x, float y) const { return ImVec2(X(x), Y(y)); }
};
Space Fit(float w, float h);
/* The space the rows are drawn in (set by the page that draws them). */
extern Space g_space;

/* Text on a baseline. sx, sy: the glyphs' size against the atlas, in design
 * units (the game's rows are 1.5 x 1.2, its titles 4.12 x 3.2). align: 0
 * left, 1 centred, 2 right. Returns the width drawn, in pixels. */
float Text(ImDrawList *dl, Font f, float x, float baseline, const char *s, float sx, float sy, ImU32 col,
           int align = 0, bool shadow = true, const Space *sp = nullptr);
float Width(Font f, const char *s, float sx, const Space *sp = nullptr);
/* Wrapped into lines no wider than max_w pixels; returns the lines drawn. */
int TextWrapped(ImDrawList *dl, Font f, float cx, float baseline, float line_h, float max_w, int max_lines,
                const char *s, float sx, float sy, ImU32 col, const Space *sp = nullptr);

/* The options page's background (black, grain, the two scroll rules). */
void OptionsBg(ImDrawList *dl, const Space &sp, float w, float h);
/* The same picture stretched over a rectangle (the launcher's board). */
void OptionsBgRect(ImDrawList *dl, ImVec2 a, ImVec2 b);
/* A part of it: uv0..uv1 of the picture (0, 0 its top left) over a..b. */
void OptionsBgPart(ImDrawList *dl, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1);
/* A pause-menu scroll rule: its line at y, w pixels wide; top: ornament up. */
void ScrollRule(ImDrawList *dl, float cx, float y, float w, bool top, ImU32 col = IM_COL32_WHITE);
/* The combo's solid triangle, the slider's chevron, the slider's red X. */
void Tri(ImDrawList *dl, ImVec2 c, bool right, float h, ImU32 col);
void Chevron(ImDrawList *dl, ImVec2 c, bool right, float h, ImU32 col);
void XSpot(ImDrawList *dl, ImVec2 c, float w, float h, ImU32 col);
/* An image from the archives (the launcher's portrait). */
ImTextureID Image(const char *name, int *w, int *h);

/* A prompt: the button's picture and its word; true when clicked. With the
 * keyboard (Button pictures, playing on keys and mouse) a key cap instead. */
bool Prompt(ImDrawList *dl, float x, float baseline, char button, const char *label, float sx, float sy,
            const Space &sp, float *advance);

/* A scroll bar for the rows: arrows at y0 and y1 (pixels), the track and
 * its thumb between, at x. Call inside the rows' child, before EndChild:
 * it reads and sets that child's scroll (thumb dragged, track clicked,
 * arrows clicked, the wheel over the bar). Nothing when it all fits. */
void ScrollBar(ImDrawList *dl, float x, float y0, float y1, const Space &sp);

} /* namespace game */
} /* namespace rdrui */

#endif
