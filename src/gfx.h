/* pid351 - drawing primitives and a font
 *
 * There is no font on this machine and nothing to link that would provide
 * one, so the font is here: 5x7 cells, ASCII 32 to 95, one byte per column
 * with bit 0 at the top. Uppercase only, and lowercase folds onto it, which
 * suits both the aesthetic and the 320 bytes it costs.
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

#define FONT_W 5
#define FONT_H 7
#define FONT_ADVANCE 6

static const unsigned char font5x7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x07,0x00,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
};

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

static inline void gfx_char(canvas_t *c, int x, int y, char ch, int s, px_t col)
{
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    if (ch < 32 || ch > 95)
        ch = '?';

    const unsigned char *g = font5x7[(unsigned char)ch - 32];
    for (int col_i = 0; col_i < FONT_W; col_i++)
        for (int row = 0; row < FONT_H; row++)
            if (g[col_i] & (1u << row))
                gfx_rect(c, x + col_i * s, y + row * s, s, s, col);
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
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    if (ch < 32 || ch > 95)
        ch = '?';

    const unsigned char *g = font5x7[(unsigned char)ch - 32];
    for (int col_i = 0; col_i < FONT_W; col_i++)
        for (int row = 0; row < FONT_H; row++)
            if (g[col_i] & (1u << row))
                gfx_rect(c, x + row * s, y - col_i * s, s, s, col);
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

#endif /* GFX_H */
