/* pid351 - drawing primitives and a font

 * There is no font on this machine and nothing to link that would provide
 * one, so the font is here: monogram, 12 rows a glyph, one byte a row, bit 0
 * at the left. Generated into font.h from monogram-bitmap.json by
 * tools/mkfont.py, ASCII plus seven symbols, 1.6 KB.
 *
 * Monospaced at a six pixel advance, which is what monogram is drawn for -
 * the narrow glyphs are narrow on purpose and setting them proportionally
 * throws away the grid the face is built on. Ink width is carried anyway, so
 * a centred string is centred on the ink and not on a trailing gap.
 *
 * Everything draws into a canvas rather than a global, so the same code runs
 * against a core's framebuffer or straight at panel resolution.
 */
#ifndef GFX_H
#define GFX_H

#include "pid351.h"

typedef struct {
    px_t *px;
    int   w, h;
} canvas_t;

#include "font.h"

/* FONT_W is the widest glyph rather than every glyph's width; FONT_ADVANCE is
 * the cell. The gap between them is the one pixel of letter spacing, and it
 * is why ink_w exists at the call sites that centre things. */
#define FONT_W       FONT_MAX_W
#define FONT_H       FONT_ROWS
#define FONT_ADVANCE (FONT_MAX_W + 1)

/* Lowercase is real now. Anything outside the table draws as a blank rather
 * than as a question mark: a missing glyph in a ROM name should leave a hole
 * you can read around, not punctuation the name never had. */
static inline unsigned char gfx_slot(char ch)
{
    unsigned char u = (unsigned char)ch;
    return u < FONT_SLOTS ? u : 0;
}

static inline void gfx_px(canvas_t *c, int x, int y, px_t col)
{
    if (x >= 0 && y >= 0 && x < c->w && y < c->h)
        c->px[(size_t)y * (size_t)c->w + (size_t)x] = col;
}

static inline void gfx_rect(canvas_t *c, int x, int y, int w, int h, px_t col)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;

    for (int row = y; row < y + h; row++) {
        px_t *p = c->px + (size_t)row * (size_t)c->w + (size_t)x;
        for (int i = 0; i < w; i++)
            p[i] = col;
    }
}

static inline void gfx_frame(canvas_t *c, int x, int y, int w, int h, px_t col)
{
    gfx_rect(c, x, y, w, 1, col);
    gfx_rect(c, x, y + h - 1, w, 1, col);
    gfx_rect(c, x, y, 1, h, col);
    gfx_rect(c, x + w - 1, y, 1, h, col);
}

/* Filled circle by span, so each row is one contiguous write. */
static inline void gfx_disc(canvas_t *c, int cx, int cy, int r, px_t col)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r)
            dx++;
        gfx_rect(c, cx - dx, cy + dy, 2 * dx + 1, 1, col);
    }
}

static inline void gfx_char(canvas_t *c, int x, int y, char ch, int s,
                            px_t col)
{
    const unsigned char *g = font_row[gfx_slot(ch)];

    for (int row = 0; row < FONT_ROWS; row++) {
        unsigned bits = g[row];
        for (int bit = 0; bits >> bit; bit++)
            if (bits & (1u << bit))
                gfx_rect(c, x + bit * s, y + row * s, s, s, col);
    }
}

static inline void gfx_text(canvas_t *c, int x, int y, const char *s,
                            int scale, px_t col)
{
    for (const char *p = s; *p; p++) {
        gfx_char(c, x, y, *p, scale, col);
        x += FONT_ADVANCE * scale;
    }
}

/* The same glyphs turned a quarter turn anticlockwise, so a line runs up a
 * narrow column instead of across it. (x, y) is the bottom left of the first
 * character and the string advances upward; it occupies FONT_H * scale across
 * the column, which is odd for odd scales and so centres exactly.
 *
 * The font is stored column major, so this is the same loop with the two axes
 * exchanged rather than a second copy of anything. */
static inline void gfx_char_rot(canvas_t *c, int x, int y, char ch, int s,
                                px_t col)
{
    const unsigned char *g = font_row[gfx_slot(ch)];

    for (int row = 0; row < FONT_ROWS; row++) {
        unsigned bits = g[row];
        for (int bit = 0; bits >> bit; bit++)
            if (bits & (1u << bit))
                gfx_rect(c, x + row * s, y - bit * s, s, s, col);
    }
}

static inline void gfx_text_rot(canvas_t *c, int x, int y, const char *s,
                                int scale, px_t col)
{
    for (const char *p = s; *p; p++) {
        gfx_char_rot(c, x, y, *p, scale, col);
        y -= FONT_ADVANCE * scale;
    }
}

static inline int gfx_text_w(const char *s, int scale)
{
    int n = 0;
    for (const char *p = s; *p; p++)
        n++;
    return n * FONT_ADVANCE * scale;
}

/* The width the string actually inks, which is the cell width less the gap no
 * pixel of the last glyph occupies. Centring on gfx_text_w instead leaves a
 * string sitting one gap left of centre - visible at scale 4, and the reason
 * the splash looked off for a week. */
static inline int gfx_ink_w(const char *s, int scale)
{
    int w = 0;
    char last = 0;

    for (const char *p = s; *p; p++) {
        w += FONT_ADVANCE * scale;
        last = *p;
    }
    if (!w)
        return 0;
    return w - (FONT_ADVANCE - font_w[gfx_slot(last)]) * scale;
}

#endif /* GFX_H */
