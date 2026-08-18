/* pid351 - scaling policy
 *
 * Shared by both backends so they cannot disagree about where the image goes.
 * Policy: largest integer scale that fits, centred. GBA lands on exactly 2x
 * and is pixel perfect; the 4:3 consoles get borders rather than a smeared
 * non-integer scale. Whether that stays the policy for NES/SNES/Genesis is a
 * decision for when there is a real image on the real panel to look at.
 */
#ifndef SCALE_H
#define SCALE_H

typedef struct { int x, y, w, h; } rect_t;

static inline rect_t fit_integer(int src_w, int src_h, int dst_w, int dst_h)
{
    int scale = dst_w / src_w;
    int sy    = dst_h / src_h;
    if (sy < scale) scale = sy;
    if (scale < 1)  scale = 1;   /* source larger than panel: clip, do not shrink */

    rect_t r;
    r.w = src_w * scale;
    r.h = src_h * scale;
    r.x = (dst_w - r.w) / 2;
    r.y = (dst_h - r.h) / 2;
    return r;
}

#endif /* SCALE_H */
