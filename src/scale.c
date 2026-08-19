/* pid351 - the resampler
 *
 * Everything here exists to fix one artefact. The panel is 480x320 and the
 * NES is 256x224, and no whole number relates them: 1x leaves the picture in
 * a sea of black, 2x is 512x448 and overflows the panel on both axes. So the
 * scale is fractional, and a fractional nearest-neighbour scale quantises
 * every source pixel to a whole number of destination pixels. Down the 10/7
 * vertical it means three source rows in every seven are drawn two pixels
 * tall and the other four are drawn one. That irregularity is what reads as
 * broken pixels on a static image and as crawling on a scrolling one.
 *
 * The fix is to let a source pixel's edge land between destination pixels and
 * pay for it with a blend, which is ordinary bilinear - and ordinary bilinear
 * on pixel art is mush, because it spreads every edge across the full width
 * of a source pixel. So the interpolation coordinate is stretched about the
 * pixel centre first:
 *
 *     t' = clamp((t - 1/2) * S + 1/2, 0, 1)
 *
 * S = 1 is plain bilinear. S = infinity is nearest. The useful value is not a
 * matter of taste: at S = dst/src, the scale factor itself, an edge crosses
 * from one source pixel to the next over exactly one destination pixel. That
 * is the narrowest a transition can be while still being free of quantisation
 * error - anything sharper is back to whole-pixel snapping, anything softer
 * is blurring for nothing. So there is no constant to tune here, which is the
 * point; the right answer falls out of the geometry.
 *
 * It also means an integer scale resolves to exact nearest-neighbour on its
 * own: at S = 2 every sample lands at t = 1/4 or 3/4 and clamps to 0 or 1.
 * GBA is 240x160 into 480x320 and comes out untouched, provably rather than
 * by a special case. It still takes the early exit below, because being
 * bit-identical is not a reason to spend the battery proving it.
 *
 * What this deliberately does not do: any of the sharpening, scanline or
 * pixel-art upscaling filters. They are all more expensive than this and
 * several of them need more than two taps. This is the cheapest thing that
 * removes the artefact rather than trading it for a different one.
 */
#include <string.h>

#include "scale.h"

/* Sized for the largest source any core here produces, not for generality:
 * Genesis is 320 wide and the NES is 240 tall before the overscan crop. */
#define SCALE_SRC_W_MAX 320

static px_t out[PANEL_W * PANEL_H];
static px_t rowbuf[SCALE_SRC_W_MAX];

/* Both axes' taps, rebuilt only when the geometry changes. It never changes
 * inside a game, so this is once per boot in practice. */
static tap_t tap_v[PANEL_H];
static tap_t tap_h[PANEL_W];
static int have_w, have_h, have_dw;

/* Weight toward i1 in 0..32, from `s` source samples onto `d` destination
 * samples, sample centres aligned. Corner-aligned mapping - the ly * h / H
 * this backend used to do inline - biases the picture by half a source pixel
 * and never reads the last source sample at full strength; that is invisible
 * under nearest and turns into a visible edge under a blend. */
static void axis_taps(tap_t *t, int s, int d)
{
    int den = 2 * d;

    for (int i = 0; i < d; i++) {
        /* Source coordinate is num/den. Negative only at i = 0, and never
         * below -den, so one branch covers the floor division C will not do
         * for us on negative operands. */
        int num = (2 * i + 1) * s - d;
        int idx = num < 0 ? -1      : num / den;
        int frac = num < 0 ? num + den : num % den;

        /* 32 * clamp((frac/den - 1/2) * d/s + 1/2, 0, 1), with den = 2d
         * cancelling out entirely. */
        int w = 16 + 16 * (frac - d) / s;

        t[i].i0 = (int16_t)(idx < 0 ? 0 : idx);
        t[i].i1 = (int16_t)(idx + 1 >= s ? s - 1 : idx + 1);
        t[i].w  = (uint8_t)(w < 0 ? 0 : w > 32 ? 32 : w);
    }
}

/* The standard RGB565 two-tap blend: spread the three channels into one
 * 32-bit word so a single multiply-add does all three at once. The gaps left
 * by the mask are exactly wide enough for a 5-bit weight - R lands in bits
 * 11..20 and G in 21..31 with nothing to spare - which is why the weight is
 * 0..32 and not 0..255. Written as x*(32-w) + y*w rather than the shorter
 * x + ((y-x)*w >> 5) because that form borrows across channel boundaries
 * whenever a channel decreases. */
static inline px_t blend565(px_t a, px_t b, uint32_t w)
{
    uint32_t x = ((uint32_t)a | ((uint32_t)a << 16)) & 0x07E0F81FU;
    uint32_t y = ((uint32_t)b | ((uint32_t)b << 16)) & 0x07E0F81FU;
    uint32_t r = ((x * (32U - w) + y * w) >> 5) & 0x07E0F81FU;

    return (px_t)(r | (r >> 16));
}

const px_t *scale_frame(const px_t *src, int src_w, int src_h, int dst_w)
{
    /* An integer scale is already exact, and running the general path over it
     * would burn a millisecond to reproduce the input. GBA is the whole
     * reason this test is here and is also the console where the battery
     * matters most. */
    if (dst_w % src_w == 0 && PANEL_H % src_h == 0)
        return NULL;

    if (src_w > SCALE_SRC_W_MAX || dst_w > PANEL_W)
        return NULL;

    if (src_w != have_w || src_h != have_h || dst_w != have_dw) {
        axis_taps(tap_v, src_h, PANEL_H);
        axis_taps(tap_h, src_w, dst_w);
        have_w = src_w; have_h = src_h; have_dw = dst_w;
    }

    for (int y = 0; y < PANEL_H; y++) {
        const px_t *r0 = src + (size_t)tap_v[y].i0 * (size_t)src_w;
        const px_t *r1 = src + (size_t)tap_v[y].i1 * (size_t)src_w;
        uint32_t wv = tap_v[y].w;
        const px_t *row;
        px_t *o = out + (size_t)y * (size_t)dst_w;

        /* A row whose weight saturated needs no vertical work at all, and
         * roughly two in five do. Taking the source row by pointer rather
         * than copying it into rowbuf keeps that case free. */
        if (wv == 0)
            row = r0;
        else if (wv == 32)
            row = r1;
        else {
            for (int x = 0; x < src_w; x++)
                rowbuf[x] = blend565(r0[x], r1[x], wv);
            row = rowbuf;
        }

        /* No such shortcut across: the weight varies per pixel here, so a
         * test would be a data-dependent branch in the inner loop of an
         * in-order core. The blend is about a dozen cheap operations and a
         * mispredict costs most of that, so it is cheaper to always blend.
         * `row` is at most 640 bytes and stays in L1 for the whole line. */
        for (int x = 0; x < dst_w; x++)
            o[x] = blend565(row[tap_h[x].i0], row[tap_h[x].i1], tap_h[x].w);
    }

    return out;
}
