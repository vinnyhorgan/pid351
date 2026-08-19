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
 *   NES/SNES 256x224 -> 417x320   plus a 63 wide status bar
 *
 * Filling all 480 columns instead would stretch the picture horizontally by
 * 9/8 - 12.5% - because the panel is 3:2 and the NES is not. Correcting that
 * costs 63 columns, and pillarboxing 63 columns of dead black on a 3.5 inch
 * screen is worse than the stretch was; so the bar is what occupies them.
 * See GAME_W below for where 417 comes from.
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

/* How wide the picture is, and why the bar gets what is left rather than the
 * other way round: the width is a property of the console and the bar is a
 * decision about the leftovers.
 *
 * The NES's pixel aspect ratio is 8:7. That is not a convention repeated from
 * somewhere - it is the ratio of the PPU's 5.369318 MHz pixel clock to NTSC's
 * 12.272727 MHz square-pixel rate, and fceumm states exactly 1.1429 through
 * retro_get_system_av_info, which core.c prints at load so it stays checked.
 *
 * 256 pixels at 8:7 across 224 rows is 1.30612 wide, so 320 rows want 417.96
 * columns:
 *
 *   game   bar        pixel aspect   error against 8:7
 *    480     0 even        1.3125        +14.84%
 *    427    53 odd         1.1676         +2.16%
 *    419    61 odd         1.1457         +0.25%
 *    418    62 even        1.1430         +0.01%
 *    417    63 odd         1.1402         -0.23%
 *
 * 418 is nearest and 417 is chosen anyway, because a bar of even width cannot
 * centre an odd-width figure and every figure in it is odd on purpose. 0.23%
 * is about one column across the whole picture and nothing can see it; half a
 * pixel of lean on the battery is the kind of thing that gets noticed
 * immediately, and was.
 *
 * The old 427 came from assuming the NES filled a 4:3 television. It mostly
 * did, because overscan hid the borders, but the signal says 8:7 and the
 * emulator says 8:7, so that is what this follows.
 *
 * GBA is 3:2 already, wants the whole panel, and is the one exception
 * fit_panel was left as a function for. Genesis is not wired up; when it is
 * it needs its own width, because 320x224 is not this ratio. */
#define GAME_W 417
#define BAR_W  (PANEL_W - GAME_W)

/* Where a source frame lands on the panel. Kept as a function rather than
 * inlined at the call site so the exception above has somewhere to live. */
static inline rect_t fit_panel(int src_w, int src_h, int dst_w, int dst_h)
{
    (void)src_h;
    /* 240 wide is GBA, which is exactly half the panel and needs no bar to
     * look right - and would lose its pixel-perfect 2x if it got one. */
    int w = src_w == 240 ? dst_w : GAME_W;
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
