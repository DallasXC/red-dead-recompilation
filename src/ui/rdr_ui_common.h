/*
 * rdr_ui_common.h - the look shared by the launcher and the overlay.
 *
 * Modelled on the game's own menus: aged parchment with burnt, torn edges
 * and an inked double border (the Sign-In and Gunslinger Profiles posters),
 * on dark planks (the desk the menu book lies on); woodtype headings in
 * the posters' oxblood red with a hard shadow; serif body text in brown ink;
 * the item under the cursor turns red, as in the book's menu; Xbox-style
 * button prompts along the bottom.
 *
 * Fonts: Rye (woodtype) and IM Fell English (old print), both SIL OFL,
 * embedded at build time. Textures are drawn procedurally at start-up, so
 * nothing from the game is copied.
 */
#ifndef RDR_UI_COMMON_H
#define RDR_UI_COMMON_H

#include "imgui.h"
#include "rdr_settings.h"

namespace rdrui {

/* Colours */
static const ImU32 kInk       = IM_COL32(46, 30, 20, 255);
static const ImU32 kInkSoft   = IM_COL32(46, 30, 20, 150);
static const ImU32 kRed       = IM_COL32(128, 30, 22, 255);
static const ImU32 kRedBright = IM_COL32(160, 38, 26, 255);
static const ImU32 kShadow    = IM_COL32(30, 12, 8, 210);
static const ImU32 kParchHi   = IM_COL32(236, 220, 186, 255);

struct Fonts {
    ImFont *title = nullptr;     /* Rye, large */
    ImFont *heading = nullptr;   /* Rye, medium */
    ImFont *body = nullptr;      /* IM Fell, rows */
    ImFont *small = nullptr;     /* IM Fell, hints and footers */
    ImFont *italic = nullptr;    /* IM Fell italic */
};
extern Fonts g_fonts;
extern float g_scale;            /* 1 at 720 lines */

/* Style and fonts: after ImGui::CreateContext, before the first frame. */
void Init(float scale);
/* Procedural textures: once a GL context is current. */
void CreateTextures();
void DestroyTextures();

/* Drawing */
void DrawWood(ImDrawList *dl, ImVec2 a, ImVec2 b);
void DrawParchment(ImDrawList *dl, ImVec2 a, ImVec2 b, unsigned seed);
void DrawFrame(ImDrawList *dl, ImVec2 a, ImVec2 b);
void DrawTitle(ImDrawList *dl, ImFont *font, float size, const char *text,
               ImVec2 center, ImU32 col);
void DrawFlourish(ImDrawList *dl, ImVec2 center, float width);
void DrawGlyph(ImDrawList *dl, ImVec2 center, float r, char letter);

/* The launcher's look, after the game's 2004 website: near-black weathered
 * planks, cream woodtype with a dark outline, glowing yellow scrollwork,
 * and a panel with a ragged black edge. */
static const ImU32 kCream      = IM_COL32(226, 219, 166, 255);
static const ImU32 kCreamHot   = IM_COL32(255, 250, 214, 255);
static const ImU32 kGlow       = IM_COL32(236, 222, 120, 255);
void DrawDarkWood(ImDrawList *dl, ImVec2 a, ImVec2 b);
void DrawTornPanel(ImDrawList *dl, ImVec2 a, ImVec2 b, unsigned seed);
void DrawGlowFlourish(ImDrawList *dl, ImVec2 center, float width);
void DrawOutlined(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 fill, const char *text,
                  float outline = 1.5f);
/* A menu entry down the left: cream, glowing under the cursor. */
bool SideItem(const char *label, bool selected, float size, float center_x);

/* Rows: a label on the left, the value on the right; the row under the
 * cursor or the controller turns red. Return true when the value changed. */
void BeginRows(float width);
bool Toggle(const char *label, int *v, const char *hint);
/* A Toggle with a line or two under its name saying what it does (always
 * shown, not only under the cursor). */
bool ToggleSub(const char *label, int *v, const char *sub, const char *hint);
bool Choice(const char *label, int *idx, const char *const *items, int n,
            const char *hint);
/* A Choice for long lists, with a search box and the list on a click. */
bool Picker(const char *label, int *idx, const char *const *items, int n,
            const char *hint);
bool Slider(const char *label, int *v, int lo, int hi, int step,
            const char *fmt, const char *hint);
bool Action(const char *label, const char *value, const char *hint);
void Note(const char *text);
/* The chapter book's pages: A accepts and closes (see rdr_ui_theme.cpp). */
extern bool g_rows_accept_closes, g_rows_accepted;
void Section(const char *title);
const char *CurrentHint();
/* The next row drawn takes the keyboard/controller focus (a page opened). */
void FocusFirstRow();
/* Whether a Picker's list is open (Esc / B close it, not the page). */
bool PickerOpen();
/* The "A Accept" prompt clicked: the row the cursor was last on is taken,
 * as A takes it, when it is drawn next. */
void AcceptCursorRow();

/* A text tab, as in the book's menu. Returns true when clicked. */
bool Tab(const char *label, bool selected);
/* A prompt with its button: true when clicked. */
bool Prompt(char glyph, const char *label);

/* The settings pages, shared. live: running game (apply at once). */
bool PageDisplay(RdrSettings *s, bool live);
bool PageGraphics(RdrSettings *s, bool live);
bool PageControls(RdrSettings *s, bool live);
bool PageExtras(RdrSettings *s, bool live);
bool PageMods(RdrSettings *s, bool live);

/* Settings that act on the running game, applied after a change. */
void ApplyLive(const RdrSettings *s);

} /* namespace rdrui */

#endif
