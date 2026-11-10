/*
 * rdr_ui_game.cpp - the menus drawn with the game's own art (see the header).
 */
#include "rdr_ui_game.h"
#include "rdr_gameart.h"
#include "rdr_gl.h"
#include "rdr_settings.h"
#include "rdr_input.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

static char s_game_dir[1024];

extern "C" {
extern const unsigned char rdr_font_berylium[];   /* assets/fonts/Berylium.ttf */
}

static volatile bool s_dir_changed;    /* a failed look is not kept: Load looks again */
extern "C" void rdr_ui_set_game_dir(const char *dir)
{
    if (strcmp(s_game_dir, dir ? dir : ""))
        s_dir_changed = true;
    snprintf(s_game_dir, sizeof s_game_dir, "%s", dir ? dir : "");
}

extern "C" const char *rdr_ui_game_dir(void)
{
    return s_game_dir;
}

namespace rdrui {
namespace game {

Space g_space;

namespace {

/* What is read from the disc, kept for the life of the process (a few MB). */
struct Pic {
    const char *name;
    RdrArtImage im;
    GLuint tex;
};
Pic s_pics[] = {
    { "options_bg_10k", {}, 0 },
    { "pausescrolls", {}, 0 },
    { "mp_arrow", {}, 0 },
    { "mp_xspot", {}, 0 },
    { "book_red", {}, 0 },
};
enum { kBg, kScroll, kArrow, kXSpot, kPortrait, kPicCount };

struct GFont {
    const char *name;
    RdrArtFont art;
    GLuint tex[32];
};
GFont s_fonts[kFontCount] = {
    { "berryliumx", {}, {} },
    { "regulatorcondensed", {}, {} },
    { "logo", {}, {} },
    { "berrylium", {}, {} },
};

std::mutex s_mu;
int s_loaded = -1;               /* -1 not tried, 0 failed, 1 loaded */
bool s_uploaded;

ImTextureID tid(GLuint t) { return (ImTextureID)(intptr_t)t; }

/* RGBA with its mipmaps (box filtered): the fonts' enlarged pages are drawn
 * much smaller than they are at 720 lines. */
GLuint upload(const RdrArtImage &im, bool clamp)
{
    GLuint t = 0;
    GLint prev = 0;
    rdr_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    rdr_gl.GenTextures(1, &t);
    rdr_gl.BindTexture(GL_TEXTURE_2D, t);
    rdr_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    rdr_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    int w = im.w, h = im.h, level = 0;
    unsigned char *cur = (unsigned char *)malloc((size_t)w * h * 4);
    if (!cur) return 0;
    memcpy(cur, im.rgba, (size_t)w * h * 4);
    for (;;) {
        rdr_gl.TexImage2D(GL_TEXTURE_2D, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, cur);
        if (w == 1 && h == 1) break;
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        unsigned char *nx = (unsigned char *)malloc((size_t)nw * nh * 4);
        if (!nx) break;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                /* premultiplied average, so transparent texels add no colour */
                float acc[4] = { 0, 0, 0, 0 };
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int sx = x * 2 + dx < w ? x * 2 + dx : w - 1, sy = y * 2 + dy < h ? y * 2 + dy : h - 1;
                        const unsigned char *p = cur + ((size_t)sy * w + sx) * 4;
                        float a = p[3] / 255.0f;
                        acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += p[3];
                    }
                unsigned char *o = nx + ((size_t)y * nw + x) * 4;
                float A = acc[3] / 4.0f, a1 = acc[3] / 255.0f;
                for (int c = 0; c < 3; c++) {
                    float v = a1 > 0.0001f ? acc[c] / a1 : 0;
                    o[c] = (unsigned char)(v > 255 ? 255 : v);
                }
                o[3] = (unsigned char)(A + 0.5f);
            }
        free(cur);
        cur = nx;
        w = nw; h = nh;
        level++;
    }
    free(cur);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    rdr_gl.BindTexture(GL_TEXTURE_2D, (GLuint)prev);
    return t;
}

const RdrArtGlyph *glyph(const GFont &f, unsigned char c)
{
    const RdrArtGlyph *g = &f.art.glyph[c];
    if (g->w) return g;
    /* the logo's typeface has capitals only */
    if (c >= 'a' && c <= 'z' && f.art.glyph[c - 32].w) return &f.art.glyph[c - 32];
    return nullptr;
}

} /* namespace */

bool Load()
{
    std::lock_guard<std::mutex> l(s_mu);
    if (s_dir_changed && s_loaded == 0)
        s_loaded = -1;
    s_dir_changed = false;
    if (s_loaded >= 0) return s_loaded == 1;
    s_loaded = 0;
    if (getenv("RDR_UI_PARCHMENT")) {               /* the menus' own look */
        fprintf(stderr, "[UI] RDR_UI_PARCHMENT: the parchment menus\n");
        return false;
    }
    if (!s_game_dir[0] || !rdr_art_open(s_game_dir)) {
        fprintf(stderr, "[UI] no swat.dat in '%s': the menus keep their parchment look\n", s_game_dir);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < kPicCount && ok; i++)
        ok = rdr_art_image(s_pics[i].name, &s_pics[i].im) != 0;
    for (int i = 0; i < kFontCount && ok; i++) {
        /* berrylium (the PS2 buttons' font) was packed without '%': read
         * without saying so, every letter after it was one off and the
         * buttons came out as other letters (user: "Â Quit") */
        ok = rdr_art_font(s_fonts[i].name, !strcmp(s_fonts[i].name, "berrylium") ? "%" : nullptr,
                          &s_fonts[i].art) != 0;
        if (!ok && i == kTextPS) {          /* (only the PlayStation buttons: without them, Xbox's) */
            memset(&s_fonts[i].art, 0, sizeof s_fonts[i].art);
            ok = true;
            continue;
        }
        /* the button pictures (A, X, Y, B and the white/black) are only enlarged */
        if (ok) ok = rdr_art_font_upscale(&s_fonts[i].art, 4, "\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf") != 0;
        /* berrylium is Ray Larabie's Berylium: its letters from the outlines */
        const char *v = getenv("RDR_VECTOR_TEXT");
        if (ok && i == kText && !(v && v[0] == '0')) {
            RdrArtFont &f = s_fonts[i].art;
            for (int p = 0; p < f.npages; p++)
                rdr_art_vector_letters(f.pages[p].rgba, f.pages[p].w, f.pages[p].h, f.up, &f, p, rdr_font_berylium,
                                       "\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf");
        }
    }
    rdr_art_close();
    if (!ok) {
        fprintf(stderr, "[UI] the game's menu art did not read: the menus keep their parchment look\n");
        for (auto &p : s_pics) rdr_art_free_image(&p.im);
        for (auto &f : s_fonts) rdr_art_free_font(&f.art);
        return false;
    }
    fprintf(stderr, "[UI] menus in the game's own art (fonts enlarged x4, berrylium from its outlines)\n");
    s_loaded = 1;
    return true;
}

bool Upload()
{
    if (!Load()) return false;
    if (s_uploaded) return true;
    for (auto &p : s_pics)
        p.tex = upload(p.im, true);
    for (auto &f : s_fonts)
        for (int i = 0; i < f.art.npages && i < 32; i++)
            f.tex[i] = upload(f.art.pages[i], true);
    s_uploaded = true;
    return true;
}

void Release()
{
    if (!s_uploaded) return;
    for (auto &p : s_pics)
        if (p.tex) { rdr_gl.DeleteTextures(1, &p.tex); p.tex = 0; }
    for (auto &f : s_fonts)
        for (int i = 0; i < 32; i++)
            if (f.tex[i]) { rdr_gl.DeleteTextures(1, &f.tex[i]); f.tex[i] = 0; }
    s_uploaded = false;
}

bool Ready() { return s_loaded == 1 && s_uploaded; }

/* Again, after the game's files arrived (the launcher's install page). */
bool Retry()
{
    if (s_loaded == 0)
        s_loaded = -1;
    return Upload();
}

Space Fit(float w, float h)
{
    Space sp;
    sp.s = w / 1280.0f < h / 720.0f ? w / 1280.0f : h / 720.0f;
    sp.ox = (w - 1280.0f * sp.s) * 0.5f;
    sp.oy = (h - 720.0f * sp.s) * 0.5f;
    return sp;
}

extern "C" int rdr_buttons_style(void);   /* rdr_cheats.c */

/* The font a letter is drawn from: berrylium's button pictures (the PS2
 * version's) for kText's when the buttons are PlayStation's. */
static const GFont &button_font(Font fi, unsigned char c)
{
    if (fi == kText && c >= 0xB8 && c <= 0xBF && s_fonts[kTextPS].art.npages && rdr_buttons_style() == 2)
        return s_fonts[kTextPS];
    return s_fonts[fi];
}

float Width(Font fi, const char *s, float sx, const Space *sp)
{
    const GFont &f = s_fonts[fi];
    float k = (sp ? sp->s : g_space.s) * sx, w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        const GFont &ff = button_font(fi, *p);
        const RdrArtGlyph *g = glyph(ff, *p);
        w += g ? (g->w + ff.art.track) * k : f.art.space * k;
    }
    return w;
}

float Text(ImDrawList *dl, Font fi, float x, float baseline, const char *s, float sx, float sy, ImU32 col,
           int align, bool shadow, const Space *sp)
{
    const GFont &f = s_fonts[fi];
    if (!Ready() || !s) return 0;
    float S = sp ? sp->s : g_space.s, kx = S * sx, ky = S * sy;
    float w = Width(fi, s, sx, sp);
    if (align == 1) x -= w * 0.5f;
    else if (align == 2) x -= w;
    unsigned alpha = (col >> IM_COL32_A_SHIFT) & 0xFF;
    for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
        float cx = x;
        float ox = pass ? 0 : 1.5f * kx, oy = pass ? 0 : 1.5f * ky;
        ImU32 c = pass ? col : IM_COL32(0, 0, 0, alpha * 170 / 255);
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            const GFont &ff = button_font(fi, *p);
            const RdrArtGlyph *g = glyph(ff, *p);
            if (!g) { cx += f.art.space * kx; continue; }
            const RdrArtImage &pg = ff.art.pages[g->page];
            float pw = (float)pg.w / ff.art.up, ph = (float)pg.h / ff.art.up;
            ImVec2 a(cx + ox, baseline + g->yoff * ky + oy), b(a.x + g->w * kx, a.y + g->h * ky);
            ImVec2 uva(g->x / pw, g->y / ph), uvb((g->x + g->w) / pw, (g->y + g->h) / ph);
            /* the button pictures keep their colours */
            ImU32 cc = (*p >= 0xB8 && *p <= 0xBF && pass) ? IM_COL32(255, 255, 255, alpha) : c;
            dl->AddImage(tid(ff.tex[g->page]), a, b, uva, uvb, cc);
            cx += (g->w + ff.art.track) * kx;
        }
    }
    return w;
}

int TextWrapped(ImDrawList *dl, Font f, float cx, float baseline, float line_h, float max_w, int max_lines,
                const char *s, float sx, float sy, ImU32 col, const Space *sp)
{
    char line[512];
    int lines = 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        /* as many words as fit */
        int n = 0, last_space = -1;
        line[0] = 0;
        const char *q = p;
        while (*q && n < (int)sizeof line - 1) {
            line[n] = *q;
            line[n + 1] = 0;
            if (*q == ' ') last_space = n;
            if (Width(f, line, sx, sp) > max_w && last_space > 0) {
                n = last_space;
                break;
            }
            n++;
            q++;
        }
        line[n] = 0;
        bool more = p[n] != 0;
        if (more && lines == max_lines - 1) {
            /* the last line that fits ends with an ellipsis */
            while (n > 0 && Width(f, line, sx, sp) + Width(f, "...", sx, sp) > max_w) line[--n] = 0;
            snprintf(line + n, sizeof line - n, "...");
        }
        Text(dl, f, cx, baseline + lines * line_h, line, sx, sy, col, 1, true, sp);
        lines++;
        p += n;
    }
    return lines;
}

void OptionsBg(ImDrawList *dl, const Space &sp, float w, float h)
{
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(14, 12, 11, 255));
    if (!Ready()) return;
    /* the game draws its 512 x 512 background over the whole screen, the
     * rules 6 pixels lower than the picture's own edge */
    dl->AddImage(tid(s_pics[kBg].tex), sp.P(0, 6), sp.P(1280, 726));
}

void OptionsBgRect(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    if (!Ready()) return;
    dl->AddImage(tid(s_pics[kBg].tex), a, b);
}

void OptionsBgPart(ImDrawList *dl, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1)
{
    if (!Ready()) return;
    dl->AddImage(tid(s_pics[kBg].tex), a, b, uv0, uv1);
}

void ScrollRule(ImDrawList *dl, float cx, float y, float w, bool top, ImU32 col)
{
    if (!Ready()) return;
    float h = 128.0f * w / 256.0f * 0.75f;      /* the 4:3 menus on 16:9: x2, y1.5 */
    float line = (top ? 128.0f - 61.0f : 61.0f) * h / 128.0f;
    ImVec2 a(cx - w * 0.5f, y - line), b(cx + w * 0.5f, y - line + h);
    if (top)
        dl->AddImage(tid(s_pics[kScroll].tex), a, b, ImVec2(0, 1), ImVec2(1, 0), col);
    else
        dl->AddImage(tid(s_pics[kScroll].tex), a, b, ImVec2(0, 0), ImVec2(1, 1), col);
}

void Tri(ImDrawList *dl, ImVec2 c, bool right, float h, ImU32 col)
{
    float hw = h * 0.45f, hh = h * 0.62f;
    if (right)
        dl->AddTriangleFilled(ImVec2(c.x + hw, c.y), ImVec2(c.x - hw, c.y - hh), ImVec2(c.x - hw, c.y + hh), col);
    else
        dl->AddTriangleFilled(ImVec2(c.x - hw, c.y), ImVec2(c.x + hw, c.y + hh), ImVec2(c.x + hw, c.y - hh), col);
}

void Chevron(ImDrawList *dl, ImVec2 c, bool right, float h, ImU32 col)
{
    if (!Ready()) return;
    ImVec2 a(c.x - h * 0.5f, c.y - h * 0.5f), b(c.x + h * 0.5f, c.y + h * 0.5f);
    if (right)
        dl->AddImage(tid(s_pics[kArrow].tex), a, b, ImVec2(1, 0), ImVec2(0, 1), col);
    else
        dl->AddImage(tid(s_pics[kArrow].tex), a, b, ImVec2(0, 0), ImVec2(1, 1), col);
}

void XSpot(ImDrawList *dl, ImVec2 c, float w, float h, ImU32 col)
{
    if (!Ready()) return;
    dl->AddImage(tid(s_pics[kXSpot].tex), ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f),
                 ImVec2(0, 0), ImVec2(1, 1), col);
}

ImTextureID Image(const char *name, int *w, int *h)
{
    for (auto &p : s_pics)
        if (!strcmp(p.name, name) && p.tex) {
            if (w) *w = p.im.w;
            if (h) *h = p.im.h;
            return tid(p.tex);
        }
    return (ImTextureID)0;
}

bool Prompt(ImDrawList *dl, float x, float baseline, char button, const char *label, float sx, float sy,
            const Space &sp, float *advance)
{
    /* the berrylium button pictures: A 0xBA, B 0xBE, X 0xBD, Y 0xBC */
    char g[2] = { (char)(button == 'A' ? 0xBA : button == 'B' ? 0xBE : button == 'X' ? 0xBD : 0xBC), 0 };
    if (rdr_buttons_style() == 3) {
        /* keys and mouse: the key, on a cap in the menus' colours */
        char kx[32];
        snprintf(kx, sizeof kx, "%s", button == 'A' ? "Enter" : button == 'B' ? "Esc"
                 : rdr_key_name(g_rdr_settings.keys[button == 'X' ? RDR_KEY_X : RDR_KEY_Y]));
        const char *key = kx;
        float kw = Width(kText, key, sx * 0.62f, &sp), pad = 7 * sx * sp.s, sw = Width(kText, " ", sx, &sp);
        float lw = Width(kText, label, sx, &sp), top = baseline - 20 * sy * sp.s, bot = baseline + 5 * sy * sp.s;
        ImVec2 a(x, top), b(x + kw + 2 * pad, bot);
        ImVec2 ea(x, top), eb(b.x + sw + lw, bot);
        bool hot = ImGui::IsMouseHoveringRect(ea, eb);
        dl->AddRectFilled(ImVec2(a.x + 2 * sp.s, a.y + 3 * sp.s), ImVec2(b.x + 2 * sp.s, b.y + 3 * sp.s),
                          IM_COL32(0, 0, 0, 150), 5 * sp.s);
        dl->AddRectFilled(a, b, IM_COL32(222, 210, 186, 255), 5 * sp.s);
        dl->AddRect(a, b, IM_COL32(90, 70, 50, 255), 5 * sp.s, 0, 1.5f * sp.s);
        Text(dl, kText, x + pad, baseline - 2 * sy * sp.s, key, sx * 0.62f, sy * 0.62f, IM_COL32(40, 30, 20, 255), 0,
             false, &sp);
        Text(dl, kText, b.x + sw, baseline, label, sx, sy, hot ? kRed : kWhite, 0, true, &sp);
        if (advance) *advance = (b.x - x) + sw + lw;
        return hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    }
    float gw = Width(kText, g, sx, &sp), sw = Width(kText, " ", sx, &sp), lw = Width(kText, label, sx, &sp);
    float top = baseline - 24 * sy * sp.s, bottom = baseline + 6 * sy * sp.s;
    ImVec2 a(x, top), b(x + gw + sw + lw, bottom);
    bool hot = ImGui::IsMouseHoveringRect(a, b);
    bool clicked = hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    Text(dl, kText, x, baseline, g, sx, sy, IM_COL32_WHITE, 0, true, &sp);
    Text(dl, kText, x + gw + sw, baseline, label, sx, sy, hot ? kRed : kWhite, 0, true, &sp);
    if (advance) *advance = gw + sw + lw;
    return clicked;
}

void ScrollBar(ImDrawList *dl, float x, float y0, float y1, const Space &sp)
{
    static ImGuiID s_drag;
    static float s_grab;
    float sy = ImGui::GetScrollY(), sm = ImGui::GetScrollMaxY(), view = ImGui::GetWindowHeight();
    ImGuiID id = ImGui::GetID("##rdr_scrollbar");
    if (sm < 1) {
        if (s_drag == id) s_drag = 0;
        return;
    }
    const ImU32 dim = IM_COL32(70, 70, 70, 255), grey = IM_COL32(150, 150, 150, 255);
    float s = sp.s, ah = 11 * s, aw = 9 * s;
    float t0 = y0 + ah + 8 * s, t1 = y1 - ah - 8 * s, len = t1 - t0;
    if (len < 20 * s)
        return;
    float thumb = len * view / (view + sm);
    if (thumb < 28 * s) thumb = 28 * s;
    if (thumb > len) thumb = len;
    float pos = t0 + (len - thumb) * (sy / sm);
    ImVec2 m = ImGui::GetIO().MousePos;
    bool over_bar = m.x >= x - 16 * s && m.x <= x + 16 * s && m.y >= y0 - 4 * s && m.y <= y1 + 4 * s;
    bool over_track = over_bar && m.y >= t0 && m.y <= t1;
    bool over_thumb = over_track && m.y >= pos && m.y <= pos + thumb;
    float step = view * 0.25f, want = sy;
    bool up_hot = over_bar && m.y < t0, dn_hot = over_bar && m.y > t1;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (over_thumb) {
            s_drag = id;
            s_grab = m.y - pos;
        } else if (over_track) {
            s_drag = id;                    /* the thumb jumps under the mouse, then drags */
            s_grab = thumb * 0.5f;
        } else if (up_hot)
            want = sy - step;
        else if (dn_hot)
            want = sy + step;
    }
    if (s_drag == id) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float f = len > thumb ? (m.y - s_grab - t0) / (len - thumb) : 0;
            want = (f < 0 ? 0 : f > 1 ? 1 : f) * sm;
        } else
            s_drag = 0;
    }
    if (over_bar && ImGui::GetIO().MouseWheel != 0)
        want = sy - ImGui::GetIO().MouseWheel * step;
    if (want != sy) {
        want = want < 0 ? 0 : want > sm ? sm : want;
        ImGui::SetScrollY(want);
        sy = want;
        pos = t0 + (len - thumb) * (sy / sm);
    }

    /* the arrows (dim at the end), the track, the thumb */
    ImU32 cu = sy > 1 ? (up_hot ? kWhite : grey) : dim, cd = sy < sm - 1 ? (dn_hot ? kWhite : grey) : dim;
    dl->AddTriangleFilled(ImVec2(x, y0), ImVec2(x + aw, y0 + ah), ImVec2(x - aw, y0 + ah), cu);
    dl->AddTriangleFilled(ImVec2(x, y1), ImVec2(x - aw, y1 - ah), ImVec2(x + aw, y1 - ah), cd);
    dl->AddRectFilled(ImVec2(x - 2 * s, t0), ImVec2(x + 2 * s, t1), IM_COL32(45, 45, 45, 230), 2 * s);
    ImU32 ct = s_drag == id ? kRed : over_thumb ? kWhite : grey;
    dl->AddRectFilled(ImVec2(x - 4 * s, pos), ImVec2(x + 4 * s, pos + thumb), ct, 3 * s);
}

} /* namespace game */
} /* namespace rdrui */
