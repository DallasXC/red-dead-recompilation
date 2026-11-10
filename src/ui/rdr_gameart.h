/*
 * rdr_gameart.h - the game's own menu art, read from the player's disc.
 *
 * The menus and the launcher draw with Red Dead Revolver's fonts and
 * textures (berrylium, regulatorcondensed, the pause scrolls, the options
 * background, the paper...). Nothing is shipped: they are read at run time
 * from swat.dat and front.dat in the game folder ("Dave" archives, see
 * tools/mods/dave.py) and decoded here to RGBA.
 */
#ifndef RDR_GAMEART_H
#define RDR_GAMEART_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int w, h;
    unsigned char *rgba;            /* w * h * 4, top row first as stored */
} RdrArtImage;

typedef struct {
    int page;                       /* which atlas page */
    int x, y, w, h;                 /* its rectangle on the page */
    int yoff;                       /* top of the glyph from the baseline */
} RdrArtGlyph;

typedef struct {
    int up;                         /* pages enlarged this many times (1: as on the disc) */
    float scale;                    /* header [0], 1.02 in every font */
    int track;                      /* extra advance after each glyph */
    int space;                      /* the space's advance */
    int line_h;                     /* the tallest glyph */
    RdrArtGlyph glyph[256];         /* by character (Latin-1); w == 0: none */
    int npages;
    RdrArtImage *pages;
} RdrArtFont;

/* Opens swat.dat and front.dat in game_dir. 1 if at least swat.dat is there. */
int rdr_art_open(const char *game_dir);
void rdr_art_close(void);

/* A file from the archives (swat.dat first, then front.dat), malloc'd;
 * NULL if it is in neither. */
unsigned char *rdr_art_read(const char *name, size_t *len);

/* An .xtex's largest level as RGBA: DXT1/3/5, 8- and 4-bit palettes, RGB.
 * name without the extension. */
int rdr_art_image(const char *name, RdrArtImage *out);
void rdr_art_free_image(RdrArtImage *im);

/* A .fonttex and its pages. skip: characters the atlas was packed without
 * (berrylium has no '%'); NULL for none. */
int rdr_art_font(const char *name, const char *skip, RdrArtFont *out);
void rdr_art_free_font(RdrArtFont *f);

/* A font's pages k times bigger with clean edges, for big screens: the
 * coverage is enlarged (cubic), its blur turned back into an edge
 * (smoothstep), then box-filtered down from twice the size. Glyphs listed
 * in keep (the button pictures) are only enlarged. Glyph rectangles and
 * metrics stay in the disc's atlas pixels; f->up says how much bigger the
 * pages are. */
int rdr_art_font_upscale(RdrArtFont *f, int k, const char *keep);

/* The same for any RGBA picture (or BGRA: the channels are treated alike):
 * malloc'd, (w*k) x (h*k). keep: w*h flags for texels only enlarged, or NULL. */
unsigned char *rdr_art_upscale(const unsigned char *rgba, int w, int h, int k, const unsigned char *keep);
/* The texels of a font page's glyphs listed in keep (w*h flags, malloc'd), or NULL. */
unsigned char *rdr_art_font_keep_mask(const RdrArtFont *f, int page, const char *keep);
/* Redraws a font page's letters from a TrueType font (the font the game's
 * own was made from: berrylium is Berylium), each fitted to the box its
 * bitmap letter's ink fills. rgba: the page k times bigger (W x H = the
 * disc's size times k), its glyph rectangles from f (in disc pixels).
 * Letters keep the page's colour and strongest alpha; those listed in
 * skip (the button pictures) and those the TrueType font has not are left. */
void rdr_art_vector_letters(unsigned char *rgba, int W, int H, int k, const RdrArtFont *f, int page,
                            const unsigned char *ttf, const char *skip);
/* A key cap with label over a page's box (BGRA page, W wide) -- the game's
 * button letters on keys and mouse. */
void rdr_art_keycap(unsigned char *bgra, int W, int x, int y, int w, int h, const char *label,
                    const unsigned char *ttf);
/* Each name in the open archives ending with suffix (".fonttex"); the count. */
int rdr_art_names(const char *suffix, void (*fn)(const char *name, void *user), void *user);

/* Raw DEFLATE (no zlib header) into out[out_len]. 1 when it fills it exactly. */
int rdr_inflate(const unsigned char *in, size_t in_len, unsigned char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
