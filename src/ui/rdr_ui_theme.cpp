/*
 * rdr_ui_theme.cpp - fonts, textures, drawing and the row widgets.
 */
#include "rdr_ui_common.h"
#include "rdr_ui_game.h"
#include "rdr_gl.h"
#include "imgui_internal.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
extern const unsigned char rdr_font_rye[];
extern const unsigned int  rdr_font_rye_size;
extern const unsigned char rdr_font_fell[];
extern const unsigned int  rdr_font_fell_size;
extern const unsigned char rdr_font_fell_italic[];
extern const unsigned int  rdr_font_fell_italic_size;
}

namespace rdrui {

Fonts g_fonts;
float g_scale = 1.0f;
static GLuint s_parch, s_wood, s_dark;
static const char *s_hint;
static float s_row_w;

/* ---- Fonts and style ------------------------------------------------- */

static ImFont *add_font(const unsigned char *data, unsigned size, float px)
{
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;   /* the arrays are static */
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void *)data, (int)size, px, &cfg);
}

void Init(float scale)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGuiStyle &st = ImGui::GetStyle();
    g_scale = scale;
    io.Fonts->Clear();
    g_fonts.body = add_font(rdr_font_fell, rdr_font_fell_size, 30.0f * scale);
    g_fonts.title = add_font(rdr_font_rye, rdr_font_rye_size, 58.0f * scale);
    g_fonts.heading = add_font(rdr_font_rye, rdr_font_rye_size, 34.0f * scale);
    g_fonts.small = add_font(rdr_font_fell, rdr_font_fell_size, 23.0f * scale);
    g_fonts.italic = add_font(rdr_font_fell_italic, rdr_font_fell_italic_size, 25.0f * scale);
    io.FontDefault = g_fonts.body;
    io.IniFilename = nullptr;            /* no imgui.ini beside the game */
    io.LogFilename = nullptr;

    st = ImGuiStyle();
    st.WindowPadding = ImVec2(0, 0);
    st.WindowBorderSize = 0;
    st.WindowRounding = 0;
    st.FramePadding = ImVec2(6 * scale, 4 * scale);
    st.ItemSpacing = ImVec2(8 * scale, 4 * scale);
    st.ScrollbarSize = 10 * scale;
    st.ScrollbarRounding = 2 * scale;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(kInk);
    st.Colors[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(kInkSoft);
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.18f, 0.12f, 0.08f, 0.10f);
    st.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.45f, 0.14f, 0.09f, 0.55f);
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.50f, 0.12f, 0.08f, 0.80f);
    st.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.55f, 0.12f, 0.08f, 1.0f);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);   /* rows show focus themselves */
    st.Colors[ImGuiCol_Border] = ImVec4(0, 0, 0, 0);
}

/* ---- Procedural textures --------------------------------------------- */

static uint32_t hash2(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float lattice(int x, int y, uint32_t seed)
{
    return (float)(hash2(x, y, seed) & 0xFFFF) / 65535.0f;
}

/* Value noise, tileable with period p (in lattice cells). */
static float vnoise(float x, float y, int p, uint32_t seed)
{
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    int ax = ((x0 % p) + p) % p, ay = ((y0 % p) + p) % p;
    int bx = (ax + 1) % p, by = (ay + 1) % p;
    float a = lattice(ax, ay, seed), b = lattice(bx, ay, seed);
    float c = lattice(ax, by, seed), d = lattice(bx, by, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

static float fbm(float x, float y, int p, int oct, uint32_t seed)
{
    float s = 0, amp = 0.5f, norm = 0;
    for (int i = 0; i < oct; i++) {
        s += amp * vnoise(x, y, p, seed + (uint32_t)i * 17u);
        norm += amp;
        x *= 2; y *= 2; p *= 2; amp *= 0.5f;
    }
    return s / norm;
}

static GLuint upload(const uint32_t *px, int w, int h)
{
    GLuint t = 0;
    GLint prev = 0;
    rdr_gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    rdr_gl.GenTextures(1, &t);
    rdr_gl.BindTexture(GL_TEXTURE_2D, t);
    rdr_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    rdr_gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    rdr_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    rdr_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    rdr_gl.BindTexture(GL_TEXTURE_2D, (GLuint)prev);
    return t;
}

static inline uint32_t rgba(float r, float g, float b)
{
    auto c = [](float v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    return c(r) | c(g) << 8 | c(b) << 16 | 0xFF000000u;
}

/* Aged paper: warm base, blotches, fibres, faint folds. Tiles. */
static GLuint make_parchment()
{
    const int N = 512;
    uint32_t *px = (uint32_t *)malloc(sizeof(uint32_t) * N * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float u = (float)x / N, v = (float)y / N;
            float blot = fbm(u * 4, v * 4, 4, 5, 11);
            float stain = fbm(u * 3 + 7, v * 3 + 3, 3, 3, 29);
            float fibre = fbm(u * 64, v * 8, 64, 2, 41);
            float speck = lattice(x, y, 77) > 0.996f ? 0.75f : 1.0f;
            float k = 0.84f + 0.28f * blot - 0.07f * fibre;
            float st = stain > 0.62f ? (stain - 0.62f) * 1.6f : 0.0f;
            /* The game's posters are a greyer, dirtier tan than new paper. */
            float r = (218 * k - 46 * st) * speck;
            float g = (199 * k - 56 * st) * speck;
            float b = (163 * k - 62 * st) * speck;
            px[y * N + x] = rgba(r, g, b);
        }
    /* Two soft fold creases. */
    for (int y = 0; y < N; y++)
        for (int dx = -2; dx <= 2; dx++) {
            int x = N / 2 + (int)(10 * sinf(y * 0.013f)) + dx;
            x = (x + N) % N;
            uint32_t c = px[y * N + x];
            float m = dx < 0 ? 0.93f : dx > 0 ? 1.04f : 0.90f;
            px[y * N + x] = rgba((c & 255) * m, ((c >> 8) & 255) * m, ((c >> 16) & 255) * m);
        }
    GLuint t = upload(px, N, N);
    free(px);
    return t;
}

/* Dark planks with grain, like the desk under the menu book. */
static GLuint make_wood()
{
    const int N = 512;
    uint32_t *px = (uint32_t *)malloc(sizeof(uint32_t) * N * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float u = (float)x / N, v = (float)y / N;
            int plank = y / 128;
            float off = lattice(plank, 0, 5) * 3.0f;
            float warp = fbm(u * 2 + off, v * 8, 2, 4, 3) * 9.0f;
            float grain = 0.5f + 0.5f * sinf((v * 128.0f + warp) * 1.1f + plank * 2.0f);
            float fine = fbm(u * 32 + off, v * 64, 32, 2, 9);
            float k = 0.70f + 0.20f * grain + 0.18f * fine + 0.06f * lattice(plank, 1, 5);
            int seam = y % 128;
            if (seam < 2) k *= 0.35f;
            else if (seam < 4) k *= 0.75f;
            px[y * N + x] = rgba(96 * k, 66 * k, 42 * k);
        }
    GLuint t = upload(px, N, N);
    free(px);
    return t;
}

/* Near-black planks, scuffed and grimy: the website's backdrop. */
static GLuint make_dark_wood()
{
    const int N = 512;
    uint32_t *px = (uint32_t *)malloc(sizeof(uint32_t) * N * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float u = (float)x / N, v = (float)y / N;
            int plank = y / 128;
            float off = lattice(plank, 0, 15) * 5.0f;
            /* Grain: long irregular streaks, warped along the plank. */
            float warp = fbm(u * 1.5f + off, v * 4, 2, 4, 23) * 14.0f;
            float streak = fbm(u * 3 + off, v * 64 + warp, 4, 3, 37);
            float grain = powf(streak, 2.2f);
            float fine = fbm(u * 64 + off, v * 128, 64, 2, 31);
            /* Grime and wear: big dark patches, a few scuffed light ones. */
            float grime = fbm(u * 4 + 11, v * 4 + 5, 4, 5, 47);
            float scuff = fbm(u * 6 + 3, v * 2 + 9, 6, 4, 53);
            float k = 0.35f + 0.60f * grain + 0.30f * fine;
            k *= grime < 0.42f ? 0.30f + 0.70f * (grime / 0.42f) : 1.0f;
            if (scuff > 0.66f) k *= 1.0f + (scuff - 0.66f) * 2.2f;
            int seam = y % 128;
            if (seam < 3) k *= 0.08f;
            else if (seam < 6) k *= 0.5f;
            if (lattice(x / 5, y / 2, 61) > 0.9990f) k = 1.25f;        /* nicks */
            px[y * N + x] = rgba(40 * k, 28 * k, 19 * k);
        }
    GLuint t = upload(px, N, N);
    free(px);
    return t;
}

void CreateTextures()
{
    if (!s_parch) s_parch = make_parchment();
    if (!s_wood) s_wood = make_wood();
    if (!s_dark) s_dark = make_dark_wood();
}

void DestroyTextures()
{
    if (s_parch) rdr_gl.DeleteTextures(1, &s_parch);
    if (s_wood) rdr_gl.DeleteTextures(1, &s_wood);
    if (s_dark) rdr_gl.DeleteTextures(1, &s_dark);
    s_parch = s_wood = s_dark = 0;
}

/* ---- Drawing --------------------------------------------------------- */

static ImTextureID tid(GLuint t) { return (ImTextureID)(intptr_t)t; }

void DrawWood(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    float tile = 512.0f * g_scale;
    dl->AddImage(tid(s_wood), a, b, ImVec2(0, 0), ImVec2((b.x - a.x) / tile, (b.y - a.y) / tile));
    /* Lamp-lit: the edges fall into shadow. */
    float ew = (b.x - a.x) * 0.20f, eh = (b.y - a.y) * 0.24f;
    ImU32 d = IM_COL32(0, 0, 0, 120), z = IM_COL32(0, 0, 0, 0);
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + eh), d, d, z, z);
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - eh), b, z, z, d, d);
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + ew, b.y), d, z, z, d);
    dl->AddRectFilledMultiColor(ImVec2(b.x - ew, a.y), b, z, d, d, z);
}

/* A point on the rectangle's edge at distance d along its perimeter, with
 * the outward normal. */
static void edge_point(ImVec2 a, ImVec2 b, float d, ImVec2 *p, ImVec2 *n)
{
    float w = b.x - a.x, h = b.y - a.y;
    if (d < w) { *p = ImVec2(a.x + d, a.y); *n = ImVec2(0, -1); return; }
    d -= w;
    if (d < h) { *p = ImVec2(b.x, a.y + d); *n = ImVec2(1, 0); return; }
    d -= h;
    if (d < w) { *p = ImVec2(b.x - d, b.y); *n = ImVec2(0, 1); return; }
    d -= w;
    *p = ImVec2(a.x, b.y - d); *n = ImVec2(-1, 0);
}

void DrawParchment(ImDrawList *dl, ImVec2 a, ImVec2 b, unsigned seed)
{
    const float s = g_scale;
    const float burn = 22.0f * s, tear = 7.0f * s;
    float w = b.x - a.x, h = b.y - a.y, per = 2 * (w + h);
    int n = (int)(per / (9.0f * s));
    if (n < 64) n = 64;
    if (n > 1200) n = 1200;
    float tile = 512.0f * s;
    ImVec2 uv0(0.13f * (seed % 7), 0.11f * (seed % 5));

    static ImVec2 outer[1200], inner[1200];
    for (int i = 0; i < n; i++) {
        ImVec2 p, nn;
        edge_point(a, b, per * i / n, &p, &nn);
        float t = (float)i / n * 40.0f;
        float j = (vnoise(t, (float)seed, 40, 3) - 0.35f) * tear
                + (lattice(i, (int)seed, 9) - 0.5f) * tear * 0.5f;
        outer[i] = ImVec2(p.x - nn.x * j, p.y - nn.y * j);
        inner[i] = ImVec2(p.x - nn.x * burn, p.y - nn.y * burn);
    }
    /* Corners pulled in a little: inner ring is a rectangle inset by burn. */
    auto uv = [&](ImVec2 p) {
        return ImVec2(uv0.x + (p.x - a.x) / tile, uv0.y + (p.y - a.y) / tile);
    };

    /* Shadow on the desk. */
    {
        ImVec2 off(7 * s, 10 * s);
        for (int i = 0; i < n; i++)
            dl->PathLineTo(ImVec2(outer[i].x + off.x, outer[i].y + off.y));
        dl->PathFillConcave(IM_COL32(0, 0, 0, 110));
    }

    dl->PushTextureID(tid(s_parch));
    const ImU32 kMid = IM_COL32(255, 255, 255, 255);
    const ImU32 kEdge = IM_COL32(150, 98, 52, 255);
    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    /* Centre to the inner ring: a fan. Inner ring to the torn edge: a
     * strip, darkening outward -- the scorched rim. */
    dl->PrimReserve(n * 3 + n * 6, 1 + n * 2);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv(c), kMid);
    for (int i = 0; i < n; i++)
        dl->PrimWriteVtx(inner[i], uv(inner[i]), kMid);
    for (int i = 0; i < n; i++)
        dl->PrimWriteVtx(outer[i], uv(outer[i]), kEdge);
    for (int i = 0; i < n; i++) {
        ImDrawIdx i0 = (ImDrawIdx)(base + 1 + i), i1 = (ImDrawIdx)(base + 1 + (i + 1) % n);
        ImDrawIdx o0 = (ImDrawIdx)(base + 1 + n + i), o1 = (ImDrawIdx)(base + 1 + n + (i + 1) % n);
        dl->PrimWriteIdx(base); dl->PrimWriteIdx(i0); dl->PrimWriteIdx(i1);
        dl->PrimWriteIdx(i0); dl->PrimWriteIdx(o0); dl->PrimWriteIdx(o1);
        dl->PrimWriteIdx(i0); dl->PrimWriteIdx(o1); dl->PrimWriteIdx(i1);
    }
    dl->PopTextureID();
    /* A thin charred line along the tear. */
    for (int i = 0; i < n; i++)
        dl->PathLineTo(outer[i]);
    dl->PathStroke(IM_COL32(70, 40, 20, 170), ImDrawFlags_Closed, 1.6f * s);
}

static void curl(ImDrawList *dl, ImVec2 p, float sx, float sy, float s, ImU32 col)
{
    /* A corner flourish: a scroll curling into the corner, and a dot. */
    float r = 16 * s;
    dl->AddBezierCubic(ImVec2(p.x + sx * 2.2f * r, p.y), ImVec2(p.x + sx * 0.6f * r, p.y - sy * 0.2f * r),
                       ImVec2(p.x + sx * 0.2f * r, p.y + sy * 0.9f * r), ImVec2(p.x + sx * 0.9f * r, p.y + sy * 0.9f * r),
                       col, 2.0f * s);
    dl->AddBezierCubic(ImVec2(p.x, p.y + sy * 2.2f * r), ImVec2(p.x - sx * 0.2f * r, p.y + sy * 0.6f * r),
                       ImVec2(p.x + sx * 0.9f * r, p.y + sy * 0.2f * r), ImVec2(p.x + sx * 0.9f * r, p.y + sy * 0.9f * r),
                       col, 2.0f * s);
    dl->AddCircleFilled(ImVec2(p.x + sx * 0.9f * r, p.y + sy * 0.9f * r), 3.0f * s, col);
    dl->AddCircleFilled(ImVec2(p.x + sx * 2.9f * r, p.y + sy * 0.15f * r), 2.2f * s, col);
    dl->AddCircleFilled(ImVec2(p.x + sx * 0.15f * r, p.y + sy * 2.9f * r), 2.2f * s, col);
}

void DrawFrame(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    const float s = g_scale;
    ImU32 col = IM_COL32(52, 34, 22, 220);
    float i1 = 30 * s, i2 = 38 * s;
    dl->AddRect(ImVec2(a.x + i1, a.y + i1), ImVec2(b.x - i1, b.y - i1), col, 0, 0, 2.6f * s);
    dl->AddRect(ImVec2(a.x + i2, a.y + i2), ImVec2(b.x - i2, b.y - i2), col, 0, 0, 1.2f * s);
    curl(dl, ImVec2(a.x + i2 + 4 * s, a.y + i2 + 4 * s), 1, 1, s, col);
    curl(dl, ImVec2(b.x - i2 - 4 * s, a.y + i2 + 4 * s), -1, 1, s, col);
    curl(dl, ImVec2(a.x + i2 + 4 * s, b.y - i2 - 4 * s), 1, -1, s, col);
    curl(dl, ImVec2(b.x - i2 - 4 * s, b.y - i2 - 4 * s), -1, -1, s, col);
}

void DrawTitle(ImDrawList *dl, ImFont *font, float size, const char *text, ImVec2 center, ImU32 col)
{
    ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    ImVec2 p(floorf(center.x - sz.x * 0.5f), floorf(center.y - sz.y * 0.5f));
    float o = (size > 40 ? 3.0f : 2.0f) * g_scale;
    dl->AddText(font, size, ImVec2(p.x + o, p.y + o), kShadow, text);
    dl->AddText(font, size, ImVec2(p.x + o * 0.5f, p.y + o * 0.5f), IM_COL32(60, 18, 12, 255), text);
    dl->AddText(font, size, p, col, text);
}

void DrawFlourish(ImDrawList *dl, ImVec2 c, float width)
{
    const float s = g_scale;
    ImU32 col = IM_COL32(52, 34, 22, 210);
    float hw = width * 0.5f;
    dl->AddLine(ImVec2(c.x - hw, c.y), ImVec2(c.x - 14 * s, c.y), col, 1.6f * s);
    dl->AddLine(ImVec2(c.x + 14 * s, c.y), ImVec2(c.x + hw, c.y), col, 1.6f * s);
    dl->AddQuadFilled(ImVec2(c.x, c.y - 7 * s), ImVec2(c.x + 9 * s, c.y), ImVec2(c.x, c.y + 7 * s),
                      ImVec2(c.x - 9 * s, c.y), col);
    dl->AddCircleFilled(ImVec2(c.x - hw - 4 * s, c.y), 2.5f * s, col);
    dl->AddCircleFilled(ImVec2(c.x + hw + 4 * s, c.y), 2.5f * s, col);
    dl->AddCircleFilled(ImVec2(c.x - 20 * s, c.y), 2.0f * s, col);
    dl->AddCircleFilled(ImVec2(c.x + 20 * s, c.y), 2.0f * s, col);
}

void DrawGlyph(ImDrawList *dl, ImVec2 c, float r, char letter)
{
    ImU32 base, lite;
    switch (letter) {
    case 'A': base = IM_COL32(58, 122, 34, 255);  lite = IM_COL32(128, 190, 80, 255); break;
    case 'B': base = IM_COL32(150, 30, 24, 255);  lite = IM_COL32(214, 92, 70, 255); break;
    case 'X': base = IM_COL32(34, 66, 150, 255);  lite = IM_COL32(96, 140, 214, 255); break;
    case 'Y': base = IM_COL32(176, 140, 22, 255); lite = IM_COL32(236, 206, 90, 255); break;
    default:  base = IM_COL32(70, 60, 50, 255);   lite = IM_COL32(150, 140, 120, 255); break;
    }
    dl->AddCircleFilled(ImVec2(c.x + 1.5f * g_scale, c.y + 2 * g_scale), r, IM_COL32(20, 10, 6, 150));
    dl->AddCircleFilled(c, r, base);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.18f, c.y - r * 0.22f), r * 0.62f, lite);
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.05f), r * 0.72f, base);
    dl->AddCircle(c, r, IM_COL32(30, 18, 10, 230), 0, 1.4f * g_scale);
    char t[2] = { letter, 0 };
    ImFont *f = g_fonts.heading;
    float fs = r * 1.25f;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, t);
    dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f + 1, c.y - ts.y * 0.5f + 1), IM_COL32(0, 0, 0, 140), t);
    dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(250, 244, 228, 255), t);
}

void DrawDarkWood(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    float tile = 512.0f * g_scale;
    dl->AddImage(tid(s_dark), a, b, ImVec2(0, 0), ImVec2((b.x - a.x) / tile, (b.y - a.y) / tile));
    float ew = (b.x - a.x) * 0.12f, eh = (b.y - a.y) * 0.16f;
    ImU32 d = IM_COL32(0, 0, 0, 200), z = IM_COL32(0, 0, 0, 0);
    dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + eh), d, d, z, z);
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - eh), b, z, z, d, d);
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + ew, b.y), d, z, z, d);
    dl->AddRectFilledMultiColor(ImVec2(b.x - ew, a.y), b, z, d, d, z);
}

void DrawTornPanel(ImDrawList *dl, ImVec2 a, ImVec2 b, unsigned seed)
{
    const float s = g_scale;
    const float burn = 5.0f * s, tear = 14.0f * s;
    float w = b.x - a.x, h = b.y - a.y, per = 2 * (w + h);
    int n = (int)(per / (4.0f * s));
    if (n < 64) n = 64;
    if (n > 1200) n = 1200;
    float tile = 512.0f * s;
    static ImVec2 outer[1200], inner[1200];
    for (int i = 0; i < n; i++) {
        ImVec2 p, nn;
        edge_point(a, b, per * i / n, &p, &nn);
        float t = (float)i / n * 90.0f;
        /* Ragged: slow bites plus sharp splinters. */
        float j = vnoise(t, (float)seed, 90, 13) * tear * 0.8f
                + (lattice(i, (int)seed, 19) > 0.82f ? lattice(i, 3, 23) * tear * 0.9f : 0.0f);
        outer[i] = ImVec2(p.x - nn.x * j, p.y - nn.y * j);
        inner[i] = ImVec2(p.x - nn.x * (j + burn), p.y - nn.y * (j + burn));
    }
    auto uv = [&](ImVec2 p) { return ImVec2(0.3f + (p.x - a.x) / tile, 0.2f + (p.y - a.y) / tile); };
    dl->PushTextureID(tid(s_parch));
    const ImU32 kMid = IM_COL32(255, 255, 255, 255);
    const ImU32 kEdge = IM_COL32(70, 50, 30, 255);
    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    dl->PrimReserve(n * 3 + n * 6, 1 + n * 2);
    ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv(c), kMid);
    for (int i = 0; i < n; i++) dl->PrimWriteVtx(inner[i], uv(inner[i]), kMid);
    for (int i = 0; i < n; i++) dl->PrimWriteVtx(outer[i], uv(outer[i]), kEdge);
    for (int i = 0; i < n; i++) {
        ImDrawIdx i0 = (ImDrawIdx)(base + 1 + i), i1 = (ImDrawIdx)(base + 1 + (i + 1) % n);
        ImDrawIdx o0 = (ImDrawIdx)(base + 1 + n + i), o1 = (ImDrawIdx)(base + 1 + n + (i + 1) % n);
        dl->PrimWriteIdx(base); dl->PrimWriteIdx(i0); dl->PrimWriteIdx(i1);
        dl->PrimWriteIdx(i0); dl->PrimWriteIdx(o0); dl->PrimWriteIdx(o1);
        dl->PrimWriteIdx(i0); dl->PrimWriteIdx(o1); dl->PrimWriteIdx(i1);
    }
    dl->PopTextureID();
    for (int i = 0; i < n; i++) dl->PathLineTo(outer[i]);
    dl->PathStroke(IM_COL32(8, 5, 2, 255), ImDrawFlags_Closed, 2.0f * s);
    /* Grime: dark smudges just inside the edge, here and there. */
    for (int i = 0; i < n; i += 2) {
        float k = vnoise((float)i / n * 50.0f, (float)seed, 50, 83);
        if (k < 0.62f) continue;
        ImVec2 p = outer[i], q = inner[i];
        ImVec2 m(p.x + (q.x - p.x) * 0.9f, p.y + (q.y - p.y) * 0.9f);
        dl->AddCircleFilled(m, (k - 0.55f) * 30.0f * s, IM_COL32(40, 26, 14, 40));
    }
}

static void glow_line(ImDrawList *dl, ImVec2 a, ImVec2 b, float t)
{
    dl->AddLine(a, b, IM_COL32(236, 222, 120, 45), t * 4.0f);
    dl->AddLine(a, b, kGlow, t);
}

static void glow_curl(ImDrawList *dl, ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float t)
{
    dl->AddBezierCubic(p0, c0, c1, p1, IM_COL32(236, 222, 120, 45), t * 4.0f);
    dl->AddBezierCubic(p0, c0, c1, p1, kGlow, t);
}

void DrawGlowFlourish(ImDrawList *dl, ImVec2 c, float width)
{
    const float s = g_scale;
    float hw = width * 0.5f, t = 2.0f * s;
    /* Centre: a lozenge between two small scrolls; arms tapering out with
     * a curl and a dot at each end -- a printer's ornament. */
    dl->AddEllipseFilled(c, ImVec2(13 * s, 5 * s), IM_COL32(236, 222, 120, 60));
    dl->AddEllipseFilled(c, ImVec2(9 * s, 3.2f * s), kGlow);
    for (int sd = -1; sd <= 1; sd += 2) {
        float d = (float)sd;
        glow_curl(dl, ImVec2(c.x + d * 9 * s, c.y), ImVec2(c.x + d * 20 * s, c.y - 10 * s),
                  ImVec2(c.x + d * 34 * s, c.y - 6 * s), ImVec2(c.x + d * 30 * s, c.y + 1 * s), t);
        glow_curl(dl, ImVec2(c.x + d * 9 * s, c.y), ImVec2(c.x + d * 20 * s, c.y + 10 * s),
                  ImVec2(c.x + d * 34 * s, c.y + 6 * s), ImVec2(c.x + d * 30 * s, c.y - 1 * s), t);
        glow_line(dl, ImVec2(c.x + d * 36 * s, c.y), ImVec2(c.x + d * (hw - 16 * s), c.y), t * 0.9f);
        glow_curl(dl, ImVec2(c.x + d * (hw - 16 * s), c.y), ImVec2(c.x + d * (hw - 2 * s), c.y),
                  ImVec2(c.x + d * (hw - 2 * s), c.y - 9 * s), ImVec2(c.x + d * (hw - 10 * s), c.y - 7 * s), t * 0.9f);
        dl->AddCircleFilled(ImVec2(c.x + d * (hw + 5 * s), c.y), 2.6f * s, kGlow);
    }
}

void DrawOutlined(ImDrawList *dl, ImFont *font, float size, ImVec2 p, ImU32 fill, const char *text, float o)
{
    o *= g_scale;
    ImU32 dark = IM_COL32(16, 9, 4, 255);
    static const float d[8][2] = { {-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1} };
    dl->AddText(font, size, ImVec2(p.x + 2.5f * o, p.y + 3 * o), IM_COL32(0, 0, 0, 160), text);
    for (auto &k : d)
        dl->AddText(font, size, ImVec2(p.x + k[0] * o, p.y + k[1] * o), dark, text);
    dl->AddText(font, size, p, fill, text);
}

bool SideItem(const char *label, bool selected, float size, float cx)
{
    ImFont *f = g_fonts.heading;
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, label);
    ImVec2 p(floorf(cx - ts.x * 0.5f), ImGui::GetCursorScreenPos().y);
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##side", ImVec2(ts.x, ts.y));
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot || selected) {
        /* Lamplight behind the word. */
        ImVec2 c(cx, p.y + ts.y * 0.5f);
        for (int i = 3; i >= 1; i--)
            dl->AddEllipseFilled(c, ImVec2(ts.x * 0.5f + i * 10 * g_scale, ts.y * 0.32f + i * 5 * g_scale),
                                 IM_COL32(236, 210, 110, selected ? 12 : 9));
    }
    DrawOutlined(dl, f, size, p, selected ? kCreamHot : hot ? IM_COL32(246, 238, 190, 255) : kCream, label);
    return clicked;
}

/* ---- Rows ------------------------------------------------------------ */

void BeginRows(float width)
{
    s_row_w = width;
    s_hint = nullptr;
}

const char *CurrentHint() { return s_hint; }

static bool s_focus_first;
void FocusFirstRow() { s_focus_first = true; }

/* One row: an invisible button the full width; reports hover/focus and
 * the left/right presses meant for it. */
struct Row {
    ImVec2 a, b;
    bool hot, clicked, left, right;
    float base;                 /* the label's and value's baseline (game art) */
};

/* In the game's art (rdr_ui_game): the options pages' rows -- berrylium,
 * white under the cursor and grey otherwise, the value between two solid
 * triangles in the right half, sliders with the chevrons and the red X. */
static bool gs() { return game::Ready(); }
static float gS() { return game::g_space.s; }
/* Where things sit across a row, as fractions of its width (the game's
 * Player Options: labels at 160, arrows at 705 and 1105, the value at 905,
 * the slider from '-' at 655 to '+' at 1150). */
static const float kArrowL = 0.55f, kArrowR = 0.955f, kValue = 0.7525f;
static float row_baseline(const Row &r) { return r.base; }

/* The row the cursor was last on, and one to take as if A were pressed on
 * it (the Accept prompt clicked: clicking the prompt takes the focus from
 * the rows, so an Enter sent after it reached none -- user: "the accept
 * option in pc options not doing anything"). */
static ImGuiID s_cursor_row, s_accept_row;
void AcceptCursorRow() { s_accept_row = s_cursor_row; }

/* extra: more height under the row's own (a sub-text), in pixels */
static Row row_begin(const char *id, const char *hint, float extra = 0)
{
    Row r;
    float h0 = gs() ? 48 * gS() : ImGui::GetFontSize() + 12 * g_scale, h = h0 + extra;
    r.a = ImGui::GetCursorScreenPos();
    r.b = ImVec2(r.a.x + s_row_w, r.a.y + h);
    r.base = r.a.y + h0 * 0.70f;
    ImGui::PushID(id);
    /* (InvisibleButton takes no keyboard or controller focus unless asked) */
    r.clicked = ImGui::InvisibleButton("##row", ImVec2(s_row_w, h), ImGuiButtonFlags_EnableNav);
    {
        /* One row is the cursor's, as in the game's menus: the first when a
         * page opens, the one under the mouse when the mouse moves, the
         * next one up or down for the keys and the d-pad. */
        ImGuiContext &g = *ImGui::GetCurrentContext();
        ImGuiIO &io = ImGui::GetIO();
        bool mouse_moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
        if (s_focus_first || (ImGui::IsItemHovered() && mouse_moved && g.NavId != ImGui::GetItemID())) {
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            g.NavCursorVisible = false;
            s_focus_first = false;
        }
        r.hot = ImGui::IsItemFocused() || (ImGui::IsItemHovered() && g.NavId == 0);
        if (ImGui::IsItemFocused())
            s_cursor_row = ImGui::GetItemID();
        if (s_accept_row && s_accept_row == ImGui::GetItemID()) {
            r.clicked = true;
            s_accept_row = 0;
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        }
    }
    r.left = r.right = false;
    if (ImGui::IsItemFocused()) {
        r.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)
              || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft);
        r.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)
               || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight);
    }
    if (r.hot && hint)
        s_hint = hint;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (r.hot && !gs()) {
        /* A faint pencilled band behind the row under the cursor. */
        dl->AddRectFilled(ImVec2(r.a.x - 10 * g_scale, r.a.y + 2 * g_scale),
                          ImVec2(r.b.x + 10 * g_scale, r.b.y - 2 * g_scale), IM_COL32(120, 70, 30, 34),
                          4 * g_scale);
    }
    return r;
}

/* A label in the game's art in no more than max_w: smaller, then cut. */
static void game_label(ImDrawList *dl, float x, float baseline, const char *text, float max_w, ImU32 col)
{
    float sx = 1.5f, sy = 1.2f;
    float w = game::Width(game::kText, text, sx);
    if (w > max_w) {
        float k = max_w / w * 0.995f;
        if (k < 0.7f) k = 0.7f;
        sx *= k;
        sy *= k > 0.88f ? k : 0.88f;
    }
    char buf[160];
    snprintf(buf, sizeof buf, "%s", text);
    size_t n = strlen(buf);
    while (n > 3 && game::Width(game::kText, buf, sx) > max_w + 0.5f) {
        buf[--n] = 0;
        buf[n - 1] = buf[n - 2] = buf[n - 3] = '.';
    }
    game::Text(dl, game::kText, x, baseline, buf, sx, sy, col);
}

static void row_label(const Row &r, const char *label, bool full = false)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (gs()) {
        float w = r.b.x - r.a.x;
        game_label(dl, r.a.x, row_baseline(r), label, full ? w : w * kArrowL - 26 * gS(),
                   r.hot ? game::kWhite : game::kGrey);
        return;
    }
    float rh = (r.base - r.a.y) / 0.70f;   /* the row's own height, without a sub-text */
    float y = r.a.y + (rh - ImGui::GetFontSize()) * 0.5f;
    dl->AddText(ImVec2(r.a.x, y), r.hot ? kRed : kInk, label);
    if (r.hot) {
        /* The book's cursor: a small pointing hand would be the thing; a
         * red diamond reads at any size. */
        float s = g_scale, cy = r.a.y + rh * 0.5f;
        dl->AddQuadFilled(ImVec2(r.a.x - 22 * s, cy - 5 * s), ImVec2(r.a.x - 16 * s, cy),
                          ImVec2(r.a.x - 22 * s, cy + 5 * s), ImVec2(r.a.x - 28 * s, cy), kRed);
    }
}

/* A value in the game's art, fitted between x0 and x1 (smaller, then cut). */
static void game_value(ImDrawList *dl, float cx, float x0, float x1, float baseline, const char *text, ImU32 col)
{
    float sx = 1.5f, sy = 1.2f, room = x1 - x0;
    float w = game::Width(game::kText, text, sx);
    if (w > room) {
        float k = room / w * 0.995f;
        if (k < 0.62f) k = 0.62f;
        sx *= k; sy *= k > 0.85f ? k : 0.85f;
    }
    char buf[160];
    snprintf(buf, sizeof buf, "%s", text);
    size_t n = strlen(buf);
    while (n > 3 && game::Width(game::kText, buf, sx) > room + 0.5f) {
        buf[--n] = 0;
        if (n > 3) { buf[n - 1] = buf[n - 2] = buf[n - 3] = '.'; }
    }
    game::Text(dl, game::kText, cx, baseline, buf, sx, sy, col, 1);
}

static void row_value(const Row &r, const char *text, bool arrows)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (gs()) {
        float w = r.b.x - r.a.x, base = row_baseline(r), cy = base - 10 * gS();
        float xl = r.a.x + w * kArrowL, xr = r.a.x + w * kArrowR;
        ImU32 c = r.hot ? game::kWhite : game::kGrey;
        if (arrows) {
            ImU32 ac = r.hot ? IM_COL32(240, 240, 240, 255) : IM_COL32(118, 118, 118, 255);
            game::Tri(dl, ImVec2(xl, cy), false, 24 * gS(), ac);
            game::Tri(dl, ImVec2(xr, cy), true, 24 * gS(), ac);
        }
        game_value(dl, r.a.x + w * kValue, xl + (arrows ? 30 : 0) * gS(), xr - (arrows ? 30 : 0) * gS(), base, text, c);
        return;
    }
    float s = g_scale;
    ImVec2 ts = ImGui::CalcTextSize(text);
    float right = r.b.x - (arrows ? 26 * s : 0);
    float y = r.a.y + (r.b.y - r.a.y - ts.y) * 0.5f;
    dl->AddText(ImVec2(right - ts.x, y), r.hot ? kRed : kInk, text);
    if (arrows) {
        float cy = (r.a.y + r.b.y) * 0.5f;
        ImU32 c = r.hot ? kRed : kInkSoft;
        float lx = right - ts.x - 22 * s, rx = r.b.x - 12 * s;
        dl->AddTriangleFilled(ImVec2(lx + 8 * s, cy - 7 * s), ImVec2(lx + 8 * s, cy + 7 * s), ImVec2(lx, cy), c);
        dl->AddTriangleFilled(ImVec2(rx - 4 * s, cy - 7 * s), ImVec2(rx - 4 * s, cy + 7 * s), ImVec2(rx + 4 * s, cy), c);
    }
}

/* Pages opened from the chapter book (Play As, Story Co-op): A is the
 * game's Accept there -- it closes the page, as the book's own pages do --
 * so A on a switch or a choice row accepts instead of stepping it (the
 * arrows and a click still step it), and a name picked from a list
 * accepts too. */
bool g_rows_accept_closes, g_rows_accepted;
static bool accept_instead(Row &r)
{
    if (!g_rows_accept_closes || !r.clicked)
        return false;
    if (!(ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)
          || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false)))
        return false;                   /* a click */
    r.clicked = false;
    g_rows_accepted = true;
    return true;
}

bool Toggle(const char *label, int *v, const char *hint)
{
    Row r = row_begin(label, hint);
    bool ch = false;
    accept_instead(r);
    if (r.clicked || r.left || r.right) {
        *v = !*v;
        ch = true;
    }
    row_label(r, label);
    if (gs()) {
        /* the game's own switches are Off / On between two triangles */
        row_value(r, *v ? "On" : "Off", true);
        ImGui::PopID();
        return ch;
    }
    /* An inked box, crossed when on. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float s = g_scale, bs = 20 * s, cy = (r.a.y + r.b.y) * 0.5f;
    ImVec2 b0(r.b.x - bs - 4 * s, cy - bs * 0.5f), b1(r.b.x - 4 * s, cy + bs * 0.5f);
    ImU32 c = r.hot ? kRed : kInk;
    dl->AddRect(b0, b1, c, 0, 0, 2.0f * s);
    if (*v) {
        dl->AddLine(ImVec2(b0.x + 4 * s, b0.y + 4 * s), ImVec2(b1.x - 4 * s, b1.y - 4 * s), kRed, 3.0f * s);
        dl->AddLine(ImVec2(b1.x - 4 * s, b0.y + 4 * s), ImVec2(b0.x + 4 * s, b1.y - 4 * s), kRed, 3.0f * s);
    }
    const char *t = *v ? "On" : "Off";
    ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(b0.x - 12 * s - ts.x, r.a.y + (r.b.y - r.a.y - ts.y) * 0.5f), c, t);
    ImGui::PopID();
    return ch;
}

/* Words wrapped into lines no wider than max_w (the game's berrylium at
 * sx), left-aligned; returns the lines (at most max_lines), drawing them
 * when dl is given. */
static int sub_lines(ImDrawList *dl, float x, float baseline, float line_h, float max_w, int max_lines,
                     const char *s, float sx, float sy, ImU32 col)
{
    char line[256];
    int lines = 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        /* as many words as fit (at least one) */
        int best = 0;
        const char *q = p, *next = p;
        while (*q) {
            const char *we = q;
            while (*we && *we != ' ') we++;
            int nl = (int)(we - p);
            if (nl >= (int)sizeof line) break;
            memcpy(line, p, (size_t)nl);
            line[nl] = 0;
            if (best && game::Width(game::kText, line, sx) > max_w) break;
            best = nl;
            next = we;
            q = we;
            while (*q == ' ') q++;
        }
        memcpy(line, p, (size_t)best);
        line[best] = 0;
        while (*next == ' ') next++;
        if (lines == max_lines - 1 && *next) {
            /* the last line and more to say: cut it with an ellipsis */
            size_t m = strlen(line);
            while (m > 3 && game::Width(game::kText, line, sx) + game::Width(game::kText, "...", sx) > max_w)
                line[--m] = 0;
            snprintf(line + m, sizeof line - m, "...");
        }
        if (dl)
            game::Text(dl, game::kText, x, baseline + lines * line_h, line, sx, sy, col, 0, false);
        lines++;
        p = next;
    }
    return lines;
}

bool ToggleSub(const char *label, int *v, const char *sub, const char *hint)
{
    if (!sub || !*sub)
        return Toggle(label, v, hint);
    if (!gs()) {
        /* the parchment: the name's row, then the line under it */
        bool ch = Toggle(label, v, hint);
        ImGui::PushFont(g_fonts.small);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kInkSoft));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + s_row_w);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * g_scale);
        ImGui::TextWrapped("%s", sub);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        return ch;
    }
    float S = gS(), sx = 0.95f, sy = 0.82f, lh = 21 * S, room = s_row_w - 20 * S;
    int n = sub_lines(nullptr, 0, 0, lh, room, 2, sub, sx, sy, 0);
    Row r = row_begin(label, hint, (n * 21 + 4) * S);
    bool ch = false;
    accept_instead(r);
    if (r.clicked || r.left || r.right) {
        *v = !*v;
        ch = true;
    }
    row_label(r, label);
    row_value(r, *v ? "On" : "Off", true);
    sub_lines(ImGui::GetWindowDrawList(), r.a.x + 20 * S, r.base + 23 * S, lh, room, 2, sub, sx, sy,
              r.hot ? IM_COL32(214, 204, 182, 255) : IM_COL32(150, 144, 132, 255));
    ImGui::PopID();
    return ch;
}

/* A click on a row's left arrow (or the left half of its value): back a
 * step, as the arrow says -- a click anywhere else on the row steps on.
 * Money's left arrow in the Quick menu added instead (user video). */
static bool clicked_left(const Row &r, const char *value)
{
    if (!r.clicked || !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        return false;
    float x = ImGui::GetIO().MousePos.x, w = r.b.x - r.a.x;
    if (gs())
        return x >= r.a.x + w * kArrowL - 30 * gS() && x < r.a.x + w * kValue;
    float right = r.b.x - 26 * g_scale, tw = ImGui::CalcTextSize(value ? value : "").x;
    return x >= right - tw - 34 * g_scale && x < right - tw * 0.5f;
}

bool Choice(const char *label, int *idx, const char *const *items, int n, const char *hint)
{
    Row r = row_begin(label, hint);
    bool ch = false;
    accept_instead(r);
    if (*idx < 0 || *idx >= n) *idx = 0;
    if (clicked_left(r, items[*idx])) {
        *idx = (*idx + n - 1) % n;
        ch = true;
    } else if (r.right || r.clicked) {
        *idx = (*idx + 1) % n;
        ch = true;
    } else if (r.left) {
        *idx = (*idx + n - 1) % n;
        ch = true;
    }
    if (*idx < 0 || *idx >= n) *idx = 0;
    row_label(r, label);
    row_value(r, items[*idx], true);
    ImGui::PopID();
    return ch;
}

/* A Choice for long lists: the arrows still step through it, and a click
 * (or A) opens a search box with the whole list under it -- type part of a
 * name, then click one or pick it with the d-pad and A. */
static bool contains_nocase(const char *hay, const char *needle)
{
    if (!*needle)
        return true;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n)
            return true;
    }
    return false;
}

static ImGuiID s_picker_open;
bool PickerOpen() { return s_picker_open != 0; }

bool Picker(const char *label, int *idx, const char *const *items, int n, const char *hint)
{
    ImGuiID &s_open_id = s_picker_open;
    static char s_filter[64];
    static bool s_just_opened;
    ImGuiID id = ImGui::GetID(label);
    bool open = s_open_id == id, ch = false;
    Row r = row_begin(label, hint);
    if (r.clicked) {
        open = !open;
        s_open_id = open ? id : 0;
        s_filter[0] = 0;
        s_just_opened = open;
    } else if (r.right) {
        *idx = (*idx + 1) % n;
        ch = true;
    } else if (r.left) {
        *idx = (*idx + n - 1) % n;
        ch = true;
    }
    if (*idx < 0 || *idx >= n) *idx = 0;
    row_label(r, label);
    row_value(r, items[*idx], true);
    ImGui::PopID();
    if (!open)
        return ch;

    float s = g_scale, row_h = ImGui::GetFontSize() + 8 * s;
    ImGui::PushID(id);
    if (gs()) {
        /* a dark list on the dark page: the game's white and grey, its red */
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(28, 25, 22, 240));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(10, 9, 8, 240));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(120, 120, 120, 160));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(225, 222, 214, 255));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(128, 128, 128, 255));
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(147, 19, 0, 150));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(147, 19, 0, 90));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(147, 19, 0, 200));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(45, 45, 45, 200));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(150, 150, 150, 230));
    } else {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(236, 222, 186, 255));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(240, 228, 196, 235));
    ImGui::PushStyleColor(ImGuiCol_Border, kInkSoft);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kInk));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(kInkSoft));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(150, 30, 20, 70));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(150, 30, 20, 45));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(150, 30, 20, 90));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(120, 80, 40, 160));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * s, 4 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s, 5 * s));
    ImGui::SetNextItemWidth(s_row_w);
    if (s_just_opened)
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##search", "Type to search...", s_filter, sizeof s_filter);
    bool typing = ImGui::IsItemActive();
    int shown = 0, first = -1;
    for (int i = 0; i < n; i++)
        if (contains_nocase(items[i], s_filter)) { shown++; if (first < 0) first = i; }
    int rows = shown < 9 ? (shown < 1 ? 1 : shown) : 9;
    ImGui::BeginChild("##list", ImVec2(s_row_w, rows * (row_h + ImGui::GetStyle().ItemSpacing.y) + 10 * s),
                      ImGuiChildFlags_Borders);
    for (int i = 0; i < n; i++) {
        if (!contains_nocase(items[i], s_filter))
            continue;
        bool sel = i == *idx;
        if (ImGui::Selectable(items[i], sel, 0, ImVec2(0, row_h - 4 * s))) {
            *idx = i;
            ch = true;
            s_open_id = 0;
            if (g_rows_accept_closes)
                g_rows_accepted = true;
        }
        if (sel && s_just_opened)
            ImGui::SetScrollHereY(0.4f);
    }
    if (!shown)
        ImGui::TextDisabled("No one by that name.");
    ImGui::EndChild();
    if (s_just_opened)
        ImGui::SetScrollHereY(1.0f);     /* the page scrolls the list into view */
    /* Enter in the search box takes the first match. */
    if (typing && first >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
        *idx = first;
        ch = true;
        s_open_id = 0;
        if (g_rows_accept_closes)
            g_rows_accepted = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))
        s_open_id = 0;
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(10);
    ImGui::PopID();
    s_just_opened = false;
    return ch;
}

bool Slider(const char *label, int *v, int lo, int hi, int step, const char *fmt, const char *hint)
{
    Row r = row_begin(label, hint);
    bool ch = false;
    float s = g_scale;
    float track_w = 220 * s, x1 = r.b.x - 70 * s, x0 = x1 - track_w;
    if (gs()) {
        /* the game's groove: from just right of the left chevron to just
         * left of the right one */
        float w = r.b.x - r.a.x;
        x0 = r.a.x + w * 0.5616f;
        x1 = r.a.x + w * 0.8200f;   /* (shorter than the game's, 0.9414: the amount goes after it) */
        track_w = x1 - x0;
    }
    float cy = (r.a.y + r.b.y) * 0.5f;
    int old = *v;
    if (r.left) *v -= step;
    if (r.right) *v += step;
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float mx = ImGui::GetIO().MousePos.x;
        if (mx >= x0 - 12 * s) {
            float t = (mx - x0) / track_w;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            *v = lo + (int)((hi - lo) * t / step + 0.5f) * step;
        }
    }
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
    ch = *v != old;
    row_label(r, label);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    float t = (float)(*v - lo) / (float)(hi - lo);
    if (gs()) {
        float w = r.b.x - r.a.x, S = gS(), base = row_baseline(r);
        cy = base - 9 * S;
        ImU32 c = r.hot ? game::kWhite : game::kGrey;
        /* the amount, which the game's own sliders do not show: at the
         * right end, after the right arrow. (It was before the left arrow,
         * with a '-' and a '+', and a long name ran into it: "Mouse
         * sensitivity" under "100%", user.) */
        game::Text(dl, game::kText, r.b.x, base, buf, 1.3f, 1.1f, c, 2);
        ImU32 ac = r.hot ? IM_COL32(170, 170, 170, 255) : IM_COL32(95, 95, 95, 255);
        game::Chevron(dl, ImVec2(r.a.x + w * 0.5475f, cy), false, 30 * S, ac);
        game::Chevron(dl, ImVec2(r.a.x + w * 0.8340f, cy), true, 30 * S, ac);
        dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), r.hot ? IM_COL32(215, 215, 215, 235) : IM_COL32(125, 125, 125, 210),
                    ImMax(1.0f, 2 * S));
        game::XSpot(dl, ImVec2(x0 + track_w * t, cy), 56 * S, 30 * S, r.hot ? game::kRed : IM_COL32(90, 90, 90, 255));
        ImGui::PopID();
        return ch;
    }
    ImU32 c = r.hot ? kRed : kInk;
    /* A rope with a revolver round for a knob. */
    dl->AddLine(ImVec2(x0, cy), ImVec2(x0 + track_w, cy), IM_COL32(90, 60, 36, 255), 4.0f * s);
    for (float x = x0; x < x0 + track_w; x += 7 * s)
        dl->AddLine(ImVec2(x, cy - 2 * s), ImVec2(x + 4 * s, cy + 2 * s), IM_COL32(60, 38, 22, 255), 1.0f * s);
    float kx = x0 + track_w * t;
    dl->AddCircleFilled(ImVec2(kx + 1 * s, cy + 2 * s), 9 * s, IM_COL32(0, 0, 0, 90));
    dl->AddCircleFilled(ImVec2(kx, cy), 9 * s, IM_COL32(176, 128, 54, 255));
    dl->AddCircleFilled(ImVec2(kx - 2 * s, cy - 2 * s), 4.5f * s, IM_COL32(226, 186, 108, 255));
    dl->AddCircle(ImVec2(kx, cy), 9 * s, c, 0, 1.6f * s);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(r.b.x - ts.x, r.a.y + (r.b.y - r.a.y - ts.y) * 0.5f), c, buf);
    ImGui::PopID();
    return ch;
}

bool Action(const char *label, const char *value, const char *hint)
{
    Row r = row_begin(label, hint);
    row_label(r, label, !value || !*value);
    if (value)
        row_value(r, value, false);
    ImGui::PopID();
    return r.clicked;
}

/* A section's heading on a long page: small capitals in red over a rule
 * (in the game's art: regulatorcondensed in the titles' red). */
void Section(const char *title)
{
    float s = g_scale;
    if (gs()) {
        float S = gS();
        ImGui::Dummy(ImVec2(1, 10 * S));
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        game::Text(dl, game::kTitle, p.x, p.y + 30 * S, title, 1.45f, 1.35f, game::kRed, 0, true);
        dl->AddLine(ImVec2(p.x, p.y + 38 * S), ImVec2(p.x + s_row_w, p.y + 38 * S), IM_COL32(110, 110, 110, 120),
                    ImMax(1.0f, S));
        ImGui::Dummy(ImVec2(s_row_w, 44 * S));
        return;
    }
    ImGui::Dummy(ImVec2(1, 8 * s));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(g_fonts.heading);
    float fs = ImGui::GetFontSize() * 0.62f;
    ImVec2 ts = g_fonts.heading->CalcTextSizeA(fs, FLT_MAX, 0.0f, title);
    dl->AddText(g_fonts.heading, fs, p, kRed, title);
    ImGui::PopFont();
    float y = p.y + ts.y + 3 * s;
    dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + s_row_w, y), kInkSoft, 1.0f * s);
    ImGui::Dummy(ImVec2(s_row_w, ts.y + 7 * s));
}

void Note(const char *text)
{
    if (gs()) {
        float S = gS();
        ImVec2 p = ImGui::GetCursorScreenPos();
        int n = game::TextWrapped(ImGui::GetWindowDrawList(), game::kText, p.x + s_row_w * 0.5f, p.y + 22 * S, 24 * S,
                                  s_row_w, 6, text, 1.15f, 1.0f, game::kHint);
        ImGui::Dummy(ImVec2(s_row_w, (n * 24 + 8) * S));
        return;
    }
    ImGui::PushFont(g_fonts.italic);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kInkSoft));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + s_row_w);
    ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

bool Tab(const char *label, bool selected)
{
    ImGui::PushFont(g_fonts.heading);
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##tab", ImVec2(ts.x, ts.y + 6 * g_scale));
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 c = selected ? kRed : hot ? kRedBright : kInk;
    if (selected)
        dl->AddText(ImVec2(p.x + 2 * g_scale, p.y + 2 * g_scale), IM_COL32(40, 14, 8, 120), label);
    dl->AddText(p, c, label);
    if (selected)
        dl->AddLine(ImVec2(p.x, p.y + ts.y + 2 * g_scale), ImVec2(p.x + ts.x, p.y + ts.y + 2 * g_scale),
                    kRed, 2.0f * g_scale);
    ImGui::PopFont();
    return clicked;
}

bool Prompt(char glyph, const char *label)
{
    ImGui::PushFont(g_fonts.body);
    float s = g_scale, r = 15 * s;
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##prompt", ImVec2(r * 2 + 8 * s + ts.x, ImMax(r * 2, ts.y)),
                                          ImGuiButtonFlags_None);
    bool hot = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = ImMax(r * 2, ts.y);
    DrawGlyph(dl, ImVec2(p.x + r, p.y + h * 0.5f), r, glyph);
    dl->AddText(ImVec2(p.x + r * 2 + 8 * s, p.y + (h - ts.y) * 0.5f), hot ? kRed : kInk, label);
    ImGui::PopFont();
    return clicked;
}

} /* namespace rdrui */
