/* pid351 - scaling policy
 *
 * One statement of where the image goes, so the two backends cannot disagree
 * about it. They did disagree, for a while, and it was not visible from
 * either side: the device stretched to fill the panel while the laptop drew
 * the same frame at 1:1 inside black borders. Since the laptop is where most
 * of the work happens, every judgement made there was made against a picture
 * the handheld would never show.
 *
 * The policy is fill the height, and give the width to the picture only as
 * far as its own aspect wants:
 *
 *   GBA      240x160 -> 480x320   exactly 2x, pixel perfect, no bar
 *   NES/SNES 256x224 -> 427x320   plus a 53 wide status bar
 *   Genesis  320x224 -> 427x320   plus a 53 wide status bar
 *
 * Filling all 480 columns instead would stretch the three 4:3 consoles
 * horizontally by 9/8 - 12.5% - because the panel is 3:2 and they are not.
 * Correcting that costs 53 columns, and pillarboxing 53 columns of dead
 * black on a 3.5 inch screen is worse than the stretch was; so the bar is
 * what occupies them. See BAR_W below for why it is exactly 53.
 *
 * GBA is the reason the panel choice was easy: 240x160 is exactly half of
 * 480x320, so the console that matters most for battery life is also the one
 * that needs neither a stretch nor a bar nor the resampler.
 *
 * Neither backend scales any more. Both hand the frame to scale_frame() and
 * then place the result, which is the only arrangement under which they
 * cannot disagree about it again - and they did, for a while, invisibly.
 */
#ifndef SCALE_H
#define SCALE_H

#include "pid351.h"

typedef struct { int x, y, w, h; } rect_t;

/* The status bar, and why it is exactly this wide.
 *
 * The 12.5% stretch above and the panel's spare width are the same fact seen
 * twice: 320 rows shown at 4:3 want 426.67 columns, and the panel has 480. So
 * the columns that were being spent stretching the picture are precisely the
 * columns a bar can occupy for free.
 *
 *   bar   game     pixel aspect   error against 4:3
 *     0   480x320       1.3125        +12.50%
 *    48   432x320       1.1812         +1.25%
 *    53   427x320       1.1676         +0.08%
 *    64   416x320       1.1375         -2.50%
 *
 * 53 is not a round number and is the right one: it lands within 0.08% of
 * correct, which is closer than the panel can resolve. Rounding it to 48 or
 * 64 would reintroduce an error to buy a tidier constant, and the constant is
 * never read by a person twice.
 *
 * This applies to the 4:3 consoles. GBA is 3:2 already and wants the whole
 * panel, which is the one exception fit_panel was left as a function for. */
#define BAR_W  53
#define GAME_W (PANEL_W - BAR_W)

/* Where a source frame lands on the panel. Kept as a function rather than
 * inlined at the call site so the exception above has somewhere to live. */
static inline rect_t fit_panel(int src_w, int src_h, int dst_w, int dst_h)
{
    (void)src_h;
    /* 240 wide is GBA, which is exactly half the panel and needs no bar to
     * look right - and would lose its pixel-perfect 2x if it got one. */
    int w = src_w == 240 ? dst_w : dst_w - BAR_W;
    rect_t r = { 0, 0, w, dst_h };
    return r;
}

/* One destination sample: two source indices and the weight toward the
 * second, 0..32. Public only because both backends want to see that the
 * thing between the core and the panel is two taps and nothing more. */
typedef struct { int16_t i0, i1; uint8_t w; } tap_t;

/* Resample `src` to dst_w x PANEL_H, sharp bilinear. The returned buffer is
 * static and valid until the next call - there is one frame in flight and
 * there will never be two. Returns NULL when the scale is already integer on
 * both axes, meaning the caller should draw `src` directly; that is not a
 * failure and is the fast path for GBA. */
const px_t *scale_frame(const px_t *src, int src_w, int src_h, int dst_w);

#endif /* SCALE_H */
