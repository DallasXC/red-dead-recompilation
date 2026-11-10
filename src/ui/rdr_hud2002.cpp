/*
 * rdr_hud2002.cpp - the January 2002 preview's HUD (Display: "2002 look").
 *
 * The preview (PS2, January 2002) drew its HUD from art the finished game
 * still carries on its disc, unused: the clock's font (timer: the digits
 * and a "TIME" picture), the Dead Eye badge's (toughnums), the money's
 * (score: big digits, small ones for the cents), the ammunition count's
 * (ammo), the bullet (hud_bullet), the weapons (hud_rifle, hud_knife ...)
 * and the bars and circles (hud_valor). They are read from swat.dat as the
 * menus' art is (rdr_gameart.c), so nothing has to be set up.
 *
 * The layout follows the preview as it played (user's video of the
 * January build) and its HUD file (swat7.hud):
 *   - "TIME 01:52" at the top left: here the time spent in the chapter;
 *   - "$20.00" at the top right;
 *   - at the bottom left the revolver's rounds in an arc, one fewer each
 *     shot (other weapons: their picture), the ammunition count above the
 *     health bar (green, yellowing, then red as it runs down), a thin red
 *     line under it and a red badge at its end with a number: the Dead
 *     Eye ("valor" in both games), 0 to 10;
 *   - dark blue curved bands at the top and bottom of the picture
 *     ("Shutters", Battle: TopCurved/BottomCurved, colour 0, 0, 0.16).
 *
 * The game's own health bar, Dead Eye meter and ammunition are left out of
 * its drawing (rdr_overrides.c, hud_draw); the rest of its HUD (the
 * crosshair, the bosses' bars, the prompts) stays. One view only: in split
 * screen the game's HUD is kept.
 *
 * Positions are in a 640 x 360 frame (the video's picture), scaled by the
 * picture's height and anchored to its left and right edges.
 */
#include "rdr_gameart.h"
#include "rdr_gl.h"
#include "rdr_cheats.h"
#include "rdr_settings.h"
#include "imgui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

extern "C" {
const char *rdr_ui_game_dir(void);
const char *rdr_preview_dir(void);
int xbox_VideoWidescreen(void);
int rdr_pause_menu(void);
int rdr_menu_mode(void);
}

namespace {

enum { kTimer, kTough, kScore, kAmmo, kFont02, kFonts };
const char *const kFontNames[kFonts] = { "timer", "toughnums", "score", "ammo", "font02" };

enum { kBullet, kValor, kRifle, kShotgun, kKnife, kDyna, kFist, kPistol, kPics };
const char *const kPicNames[kPics] = { "hud_bullet", "hud_valor", "hud_rifle", "hud_sawedoff",
                                       "hud_knife", "hud_dyna", "hud_fist", "hud_pistola" };

struct Font {
    RdrArtFont art;
    GLuint tex[8];
};
Font s_font[kFonts];
RdrArtImage s_pic[kPics];
GLuint s_pic_tex[kPics];
int s_loaded = -1;                   /* -1 not tried, 0 failed, 1 read */
bool s_uploaded;

ImTextureID tid(GLuint t) { return (ImTextureID)(intptr_t)t; }

GLuint upload(const RdrArtImage &im)
{
    GLuint t = 0;
    GLint prev = 0;
    rdr_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    rdr_gl.GenTextures(1, &t);
    rdr_gl.BindTexture(GL_TEXTURE_2D, t);
    rdr_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    rdr_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    rdr_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, im.rgba);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    rdr_gl.BindTexture(GL_TEXTURE_2D, (GLuint)prev);
    return t;
}

bool load()
{
    if (s_loaded >= 0) return s_loaded == 1;
    s_loaded = 0;
    const char *dir = rdr_ui_game_dir();
    if (!dir || !*dir || !rdr_art_open(dir)) {
        fprintf(stderr, "[HUD2002] no swat.dat: the 2002 look is not drawn\n");
        return false;
    }
    bool ok = true;
    for (int i = 0; i < kFonts && ok; i++)
        ok = rdr_art_font(kFontNames[i], nullptr, &s_font[i].art) != 0;
    for (int i = 0; i < kPics && ok; i++)
        ok = rdr_art_image(kPicNames[i], &s_pic[i]) != 0;
    rdr_art_close();
    if (!ok) {
        fprintf(stderr, "[HUD2002] the preview's HUD art is not in swat.dat: the 2002 look is not drawn\n");
        for (auto &f : s_font) rdr_art_free_font(&f.art);
        for (auto &p : s_pic) rdr_art_free_image(&p);
        return false;
    }
    fprintf(stderr, "[HUD2002] the 2002 preview's HUD art read from swat.dat\n");
    fflush(stderr);
    s_loaded = 1;
    return true;
}

bool upload_all()
{
    if (!load()) return false;
    if (s_uploaded) return true;
    for (auto &f : s_font)
        for (int i = 0; i < f.art.npages && i < 8; i++)
            f.tex[i] = upload(f.art.pages[i]);
    for (int i = 0; i < kPics; i++)
        s_pic_tex[i] = upload(s_pic[i]);
    s_uploaded = true;
    return true;
}

/* The frame: 640 x 360 units over the game's picture. */
struct Frame {
    float x0, y0, w, h, s;              /* the picture, and pixels per unit */
    float L(float x) const { return x0 + x * s; }                   /* from the left */
    float R(float x) const { return x0 + w - (640.0f - x) * s; }    /* from the right */
    float Y(float y) const { return y0 + y * s; }
};

Frame picture(int ww, int wh)
{
    Frame f;
    f.x0 = 0; f.y0 = 0; f.w = (float)ww; f.h = (float)wh;
    if (!g_rdr_settings.stretch && ww > 0 && wh > 0) {
        float want = xbox_VideoWidescreen() ? 16.0f / 9.0f : 4.0f / 3.0f, ar = (float)ww / (float)wh;
        if (ar > want) {
            f.w = (float)wh * want;
            f.x0 = ((float)ww - f.w) * 0.5f;
        } else {
            f.h = (float)ww / want;
            f.y0 = ((float)wh - f.h) * 0.5f;
        }
    }
    f.s = f.h / 360.0f;
    return f;
}

ImU32 rgba(int r, int g, int b, float a)
{
    int A = (int)(a * 255.0f + 0.5f);
    return IM_COL32(r, g, b, A < 0 ? 0 : A > 255 ? 255 : A);
}

/* Text in one of the preview's fonts: x the left (align 0), centre (1) or
 * right (2), on a baseline, k pixels per atlas pixel. Returns the width. */
float text_w(int fi, const char *s, float k)
{
    const RdrArtFont &a = s_font[fi].art;
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const RdrArtGlyph &g = a.glyph[*p];
        w += g.w ? (g.w + (float)(int)a.track) * k : a.space * k;
    }
    return w;
}

float text(ImDrawList *dl, int fi, float x, float base, const char *s, float k, ImU32 col, int align = 0)
{
    const Font &f = s_font[fi];
    float w = text_w(fi, s, k);
    if (align == 1) x -= w * 0.5f;
    else if (align == 2) x -= w;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const RdrArtGlyph &g = f.art.glyph[*p];
        if (!g.w) { x += f.art.space * k; continue; }
        const RdrArtImage &pg = f.art.pages[g.page];
        float pw = (float)pg.w / f.art.up, ph = (float)pg.h / f.art.up;
        ImVec2 a(x, base + g.yoff * k), b(x + g.w * k, base + (g.yoff + g.h) * k);
        dl->AddImage(tid(f.tex[g.page]), a, b, ImVec2(g.x / pw, g.y / ph),
                     ImVec2((g.x + g.w) / pw, (g.y + g.h) / ph), col);
        x += (g.w + (float)(int)f.art.track) * k;
    }
    return w;
}

/* A picture turned by ang (radians, clockwise on screen) about its centre. */
void pic_turned(ImDrawList *dl, int pi, ImVec2 c, float w, float h, float ang, ImU32 col,
                ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1))
{
    float cs = cosf(ang), sn = sinf(ang), hw = w * 0.5f, hh = h * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
    dl->AddImageQuad(tid(s_pic_tex[pi]), P(-hw, -hh), P(hw, -hh), P(hw, hh), P(-hw, hh),
                     ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv0.y), ImVec2(uv1.x, uv1.y), ImVec2(uv0.x, uv1.y), col);
}

/* The weapon's kind, from its name. */
enum Kind { kRevolver, kLong, kShot, kBlade, kThrown, kBare };
Kind kind_of(const char *n)
{
    char l[48];
    size_t i = 0;
    for (; n[i] && i < sizeof l - 1; i++) l[i] = (char)((n[i] >= 'A' && n[i] <= 'Z') ? n[i] + 32 : n[i]);
    l[i] = 0;
    if (strstr(l, "shotgun") || strstr(l, "sawed") || strstr(l, "sawn") || strstr(l, "scattergun")
        || strstr(l, "blunderbuss"))
        return kShot;
    if (strstr(l, "rifle") || strstr(l, "carbine") || strstr(l, "sharps") || strstr(l, "winchester")
        || strstr(l, "repeater") || strstr(l, "gatling") || strstr(l, "bow"))
        return kLong;
    if (strstr(l, "knife") || strstr(l, "hatchet") || strstr(l, "axe") || strstr(l, "tomahawk")
        || strstr(l, "machete") || strstr(l, "sword") || strstr(l, "saber"))
        return kBlade;
    if (strstr(l, "dynamite") || strstr(l, "molotov") || strstr(l, "bomb") || strstr(l, "grenade")
        || strstr(l, "tnt") || strstr(l, "oil") || strstr(l, "stick"))
        return kThrown;
    if (strstr(l, "fist") || strstr(l, "unarmed") || strstr(l, "hands") || strstr(l, "punch"))
        return kBare;
    return kRevolver;
}

/* The navy bands: the top one hangs lowest in the middle, the bottom one
 * rises highest there. depth: 0..1 as they come in. */
void bands(ImDrawList *dl, const Frame &f, float a)
{
    const int N = 48;
    ImU32 col = rgba(0, 0, 41, a);
    float top_edge = 4.0f, top_mid = 46.0f, bot_edge = 12.0f, bot_mid = 44.0f;
    for (int i = 0; i < N; i++) {
        float u0 = (float)i / N, u1 = (float)(i + 1) / N;
        float t0 = u0 * 2 - 1, t1 = u1 * 2 - 1;
        float x0 = f.x0 + u0 * f.w, x1 = f.x0 + u1 * f.w;
        float d0 = top_edge + (top_mid - top_edge) * (1 - t0 * t0), d1 = top_edge + (top_mid - top_edge) * (1 - t1 * t1);
        dl->AddQuadFilled(ImVec2(x0, f.y0), ImVec2(x1, f.y0), ImVec2(x1, f.Y(d1)), ImVec2(x0, f.Y(d0)), col);
        d0 = bot_edge + (bot_mid - bot_edge) * (1 - t0 * t0);
        d1 = bot_edge + (bot_mid - bot_edge) * (1 - t1 * t1);
        float yb = f.y0 + f.h;
        dl->AddQuadFilled(ImVec2(x0, yb - d0 * f.s), ImVec2(x1, yb - d1 * f.s), ImVec2(x1, yb), ImVec2(x0, yb), col);
    }
}

/* The revolver's rounds: an arc about a centre low at the left, the top
 * round the first to go. second: the other hand's gun, further out. */
void rounds(ImDrawList *dl, const Frame &f, int loaded, int clip, bool second, float a)
{
    if (clip <= 0 || clip > 12) clip = 6;
    if (loaded < 0) loaded = clip;                   /* never runs out */
    if (loaded > clip) loaded = clip;
    const float cx = 46.0f, cy = 318.0f, r = second ? 98.0f : 76.0f;
    const float a_top = -82.0f, a_bot = -14.0f;
    float step = (a_bot - a_top) / (float)(clip > 1 ? clip - 1 : 1);
    for (int i = 0; i < loaded; i++) {
        float deg = a_bot - step * (float)i;        /* from the bottom up */
        float rad = deg * 3.14159265f / 180.0f;
        ImVec2 c(f.L(cx + r * cosf(rad)), f.Y(cy + r * sinf(rad)));
        /* the bullet stands tip up in its picture: turned to point out */
        pic_turned(dl, kBullet, c, 24.0f * f.s, 24.0f * f.s, rad + 3.14159265f * 0.5f, rgba(255, 255, 255, a));
    }
}

/* The preview's title picture (beta_look.py, from the player's preview
 * disc at its setup: Mods\Preview\2002look\title2002.tga, 32-bit, top row
 * first). -1 not looked for, 0 none, 1 read. */
int s_title = -1;
RdrArtImage s_title_im;
GLuint s_title_tex;

bool title_load()
{
    if (s_title >= 0) return s_title == 1;
    s_title = 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/2002look/title2002.tga", rdr_preview_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char h[18];
    if (fread(h, 1, 18, f) != 18 || h[2] != 2 || h[16] != 32) { fclose(f); return false; }
    int w = h[12] | h[13] << 8, ht = h[14] | h[15] << 8;
    if (w <= 0 || ht <= 0 || w > 4096 || ht > 4096) { fclose(f); return false; }
    fseek(f, 18 + h[0], SEEK_SET);
    unsigned char *px = (unsigned char *)malloc((size_t)w * ht * 4);
    if (!px || fread(px, 4, (size_t)w * ht, f) != (size_t)w * ht) { free(px); fclose(f); return false; }
    fclose(f);
    for (size_t i = 0; i < (size_t)w * ht; i++) {          /* BGRA to RGBA */
        unsigned char t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t;
    }
    if (!(h[17] & 0x20))                                      /* bottom row first: turn it */
        for (int y = 0; y < ht / 2; y++)
            for (int x = 0; x < w * 4; x++) {
                unsigned char t = px[(size_t)y * w * 4 + x];
                px[(size_t)y * w * 4 + x] = px[(size_t)(ht - 1 - y) * w * 4 + x];
                px[(size_t)(ht - 1 - y) * w * 4 + x] = t;
            }
    s_title_im.w = w; s_title_im.h = ht; s_title_im.rgba = px;
    s_title = 1;
    fprintf(stderr, "[HUD2002] the preview's title picture: %s\n", path);
    fflush(stderr);
    return true;
}

double s_clock;
uint32_t s_clock_level;
float s_fade;

} /* namespace */

/* Draws the 2002 HUD; from the overlay, every frame (in its GL context).
 * dt: seconds since the last frame. */
extern "C" void rdr_hud2002_draw(ImDrawList *dl, int ww, int wh, float dt, int menu_open)
{
    if (!g_rdr_settings.look2002) {
        s_fade = 0;
        return;
    }
    RdrHud2002 st;
    if (!rdr_hud2002_read(&st)) {
        s_fade = 0;
        s_clock_level = 0;
        return;
    }
    if (st.level != s_clock_level) {
        s_clock_level = st.level;
        s_clock = 0;
    }
    bool paused = rdr_pause_menu() || menu_open;
    bool up = st.hud_on && !paused && !rdr_menu_mode();
    if (up)
        s_clock += dt;
    s_fade += (up ? 1.0f : -1.0f) * dt * 4.0f;
    s_fade = s_fade < 0 ? 0 : s_fade > 1 ? 1 : s_fade;
    if (s_fade <= 0.0f || !upload_all())
        return;
    float a = s_fade;
    Frame f = picture(ww, wh);
    const float k = 0.6f * f.s;              /* the fonts' size: atlas pixels to screen */

    if (g_rdr_settings.look2002_bands)
        bands(dl, f, a);

    /* TIME 01:52 */
    {
        char t[16];
        int secs = (int)s_clock;
        snprintf(t, sizeof t, "%02d:%02d", (secs / 60) % 100, secs % 60);
        float x = f.L(26.0f), base = f.Y(36.0f);
        x += text(dl, kTimer, x, base, "T", k, rgba(255, 255, 255, a)) + 4.0f * f.s;
        text(dl, kTimer, x, base - 1.0f * f.s, t, k * 0.82f, rgba(255, 255, 255, a));
    }

    /* $20.00 */
    if (st.money >= 0) {
        char d[16];
        snprintf(d, sizeof d, "$%d", st.money);
        float right = f.R(606.0f), base = f.Y(27.0f);   /* above the bosses' bars */
        float km = k * 0.8f;
        float wc = text_w(kScore, "aa", km), wd = text_w(kScore, d, km) + text_w(kScore, ".", km);
        float x = right - wc - wd;
        x += text(dl, kScore, x, base, d, km, rgba(255, 255, 255, a));
        x += text(dl, kScore, x, base, ".", km, rgba(255, 255, 255, a));
        text(dl, kScore, x, base, "aa", km, rgba(255, 255, 255, a));   /* the small "00" */
    }

    /* the weapon: the revolver's rounds, or the weapon's picture */
    if (st.ammo_on && st.weapon >= 0) {
        Kind kd = kind_of(st.weapon_name);
        int total = -1;
        if (st.loaded >= 0 && st.left >= 0)
            total = st.loaded + st.left + (st.hands > 1 && st.loaded2 > 0 ? st.loaded2 : 0);
        if (kd == kRevolver) {
            rounds(dl, f, st.loaded, st.clip, false, a);
            if (st.hands > 1)
                rounds(dl, f, st.loaded2, st.clip2, true, a);
        } else {
            int pi = kd == kLong ? kRifle : kd == kShot ? kShotgun : kd == kBlade ? kKnife
                   : kd == kThrown ? kDyna : kFist;
            float pw = kd == kBare ? 44.0f : 92.0f, ph = kd == kBare ? 44.0f : 46.0f;
            ImVec2 c(f.L(26.0f + pw * 0.5f + 52.0f), f.Y(276.0f));
            pic_turned(dl, pi, c, pw * f.s, ph * f.s, -0.35f, rgba(255, 255, 255, a));
        }
        if (total >= 0 && kd != kBlade && kd != kBare) {
            char n[16];
            snprintf(n, sizeof n, "%d", total);
            text(dl, kAmmo, f.L(26.0f), f.Y(306.0f), n, k, rgba(255, 255, 255, a));
        }
    }

    /* the health bar, the Dead Eye under it and its badge */
    if (st.health_on && st.health_max > 0.0f) {
        float hp = (st.health_shown > 0.0f ? st.health_shown : st.health) / st.health_max;
        hp = hp < 0 ? 0 : hp > 1 ? 1 : hp;
        float x0 = f.L(30.0f), x1 = f.L(170.0f), y0 = f.Y(316.0f), y1 = f.Y(324.0f);
        dl->AddRectFilled(ImVec2(x0 - 1.5f * f.s, y0 - 1.5f * f.s), ImVec2(x1 + 1.5f * f.s, y1 + 1.5f * f.s),
                          rgba(0, 0, 0, a * 0.85f));
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), rgba(34, 58, 66, a));
        /* green full, through yellow, to red (HealthFull/Mid/EmptyColor) */
        float r, g, b;
        if (hp > 0.5f) {
            float t = (hp - 0.5f) * 2.0f;
            r = 0.8f * (1 - t); g = 0.6f + 0.4f * t; b = 0;
        } else {
            float t = hp * 2.0f;
            r = 0.8f; g = 0.6f * t; b = 0;
        }
        if (hp > 0)
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (x1 - x0) * hp, y1),
                              rgba((int)(r * 255), (int)(g * 255), (int)(b * 255), a));
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (x1 - x0) * hp, y0 + 2.0f * f.s),
                          rgba(255, 255, 255, a * 0.25f));
    }
    if (st.valor_on && st.valor_max > 0.0f) {
        float v = st.valor / st.valor_max;
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        if (st.health_on) {
            float x0 = f.L(30.0f), x1 = f.L(170.0f), y = f.Y(327.0f);
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * v, y + 2.5f * f.s), rgba(230, 24, 16, a));
        }
        /* the badge: red when full, fading to tan as it empties
         * (ValorFullColor 1,0,0; ValorEmptyColor 0.8,0.6,0.4) */
        ImVec2 c(f.L(185.0f), f.Y(321.0f));
        float rad = 15.0f * f.s;
        int R = (int)(255 * (0.8f + 0.2f * v)), G = (int)(255 * 0.6f * (1 - v)), B = (int)(255 * 0.4f * (1 - v));
        dl->AddCircleFilled(c, rad + 2.0f * f.s, rgba(255, 255, 255, a), 40);
        dl->AddCircleFilled(c, rad, rgba(R, G, B, a), 40);
        char n[8];
        snprintf(n, sizeof n, "%d", (int)floorf(v * 10.0f + 0.5f));
        text(dl, kTough, c.x, c.y + 9.0f * f.s, n, 0.55f * f.s, rgba(255, 255, 255, a), 1);
    }
}

/* The 2002 look's title: over the game's own title (from the end of the
 * logos to the desk), the preview's main menu picture -- red, the logo in
 * black, the gunman's shape -- at 4:3 in the middle, the sides in its red,
 * and "Press Start" pulsing at the bottom right where the preview had
 * "Press X To Confirm" (font02, the preview's lettering); "Loading" once
 * Start was pressed. Only with the picture made by the preview's setup. */
extern "C" int rdr_hud2002_title(ImDrawList *dl, int ww, int wh)
{
    static float fade;
    static uint64_t t;
    int phase = g_rdr_settings.look2002 ? rdr_title_phase() : 0;
    if (!phase || !title_load() || !upload_all()) {
        fade = 0;
        return 0;
    }
    if (!s_title_tex)
        s_title_tex = upload(s_title_im);
    t++;
    fade = fade + 0.08f > 1.0f ? 1.0f : fade + 0.08f;
    const unsigned char *edge = s_title_im.rgba + (size_t)(s_title_im.h / 2) * s_title_im.w * 4;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)ww, (float)wh), rgba(edge[0], edge[1], edge[2], fade));
    float h = (float)wh, w = h * 4.0f / 3.0f;
    if (w > ww) { w = (float)ww; h = w * 3.0f / 4.0f; }
    ImVec2 a(((float)ww - w) * 0.5f, ((float)wh - h) * 0.5f), b(a.x + w, a.y + h);
    dl->AddImage(tid(s_title_tex), a, b, ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, fade));
    float k = h / 448.0f, pulse = 0.55f + 0.45f * sinf((float)t * 0.12f);
    const char *msg = phase == 2 ? "Loading" : "Press Start";
    text(dl, kFont02, a.x + w - 40.0f * k, a.y + 410.0f * k, msg, 1.6f * k, rgba(255, 255, 255, fade * pulse), 2);
    return 1;
}

/* Off the GPU (the overlay's context going away). */
extern "C" void rdr_hud2002_release(void)
{
    if (!s_uploaded) return;
    for (auto &fo : s_font)
        for (int i = 0; i < 8; i++)
            if (fo.tex[i]) { rdr_gl.DeleteTextures(1, &fo.tex[i]); fo.tex[i] = 0; }
    for (int i = 0; i < kPics; i++)
        if (s_pic_tex[i]) { rdr_gl.DeleteTextures(1, &s_pic_tex[i]); s_pic_tex[i] = 0; }
    if (s_title_tex) { rdr_gl.DeleteTextures(1, &s_title_tex); s_title_tex = 0; }
    s_uploaded = false;
}

/* Whether it draws now (the overlay draws a frame for it). */
extern "C" int rdr_hud2002_wanted(void)
{
    return g_rdr_settings.look2002
           && (rdr_hud2002_hide_mask() != 0 || s_fade > 0.0f || (s_title != 0 && rdr_title_phase() != 0));
}
