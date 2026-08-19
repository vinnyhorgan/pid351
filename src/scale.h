/* pid351 - scaling policy
 *
 * One statement of where the image goes, so the two backends cannot disagree
 * about it. They did disagree, for a while, and it was not visible from
 * either side: the device stretched to fill the panel while the laptop drew
 * the same frame at 1:1 inside black borders. Since the laptop is where most
 * of the work happens, every judgement made there was made against a picture
 * the handheld would never show.
 *
 * The policy is fill, with no bars anywhere:
 *
 *   GBA      240x160 -> 480x320   exactly 2x, pixel perfect
 *   NES/SNES 256x224 -> 480x320   15/8 across, 10/7 down
 *   Genesis  320x224 -> 480x320   3/2  across, 10/7 down
 *
 * The three 4:3 consoles are stretched horizontally by 9/8 - 12.5% - because
 * the panel is 3:2 and they are not. That was chosen over pillarboxing by
 * looking at it on the real panel: 12.5% is not perceptible on a 3.5 inch
 * screen, and 112 columns of dead black on a display this small is.
 *
 * GBA is the reason the panel choice was easy: 240x160 is exactly half of
 * 480x320, so the console that matters most for battery life is also the one
 * that needs no stretch at all.
 *
 * The device does not call fit_panel. Its blit has to build per-row and
 * per-column source tables anyway - it is rotating and scaling in one pass
 * over write-combined memory - so it computes the same mapping inline as
 * `ly * h / PANEL_H` and `lx * w / PANEL_W`. That is this policy; if either
 * changes, both change.
 */
#ifndef SCALE_H
#define SCALE_H

typedef struct { int x, y, w, h; } rect_t;

/* Where a source frame lands on the panel: all of it, always. Kept as a
 * function rather than inlined at the call site so that a future policy with
 * an exception in it has somewhere to live. */
static inline rect_t fit_panel(int src_w, int src_h, int dst_w, int dst_h)
{
    (void)src_w;
    (void)src_h;
    rect_t r = { 0, 0, dst_w, dst_h };
    return r;
}

#endif /* SCALE_H */
