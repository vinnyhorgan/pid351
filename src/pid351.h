/* pid351 - shared definitions
 *
 * Pixels are RGB565 end to end: cores produce it natively (it is the libretro
 * default), the RK3326 VOP scans it out directly, and it halves every blit's
 * memory bandwidth versus XRGB8888. On a device where battery is the whole
 * point, that is not a micro-optimisation.
 */
#ifndef PID351_H
#define PID351_H

#include <stdint.h>
#include <stddef.h>

/* The panel. Landscape as the user holds it; the physical panel is portrait
 * and rotated 90 degrees, which the device backend deals with (via RGA). */
#define PANEL_W 480
#define PANEL_H 320

typedef uint16_t px_t;

#define RGB565(r, g, b) \
    ((px_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Button bitmask. Names are what is printed on the shell, not what any
 * particular console calls them; per-core mapping happens above this.
 *
 * PAD_ rather than BTN_ because <linux/input-event-codes.h> already owns
 * BTN_A, BTN_START, BTN_SELECT and friends as macros. The device backend has
 * to include that header, and a macro silently rewriting our enum into the
 * kernel's keycodes is a bug that would only ever show up as the wrong button
 * doing the wrong thing on hardware. */
enum {
    PAD_A      = 1u << 0,
    PAD_B      = 1u << 1,
    PAD_X      = 1u << 2,
    PAD_Y      = 1u << 3,
    PAD_UP     = 1u << 4,
    PAD_DOWN   = 1u << 5,
    PAD_LEFT   = 1u << 6,
    PAD_RIGHT  = 1u << 7,
    PAD_L1     = 1u << 8,
    PAD_R1     = 1u << 9,
    PAD_L2     = 1u << 10,
    PAD_R2     = 1u << 11,
    PAD_SELECT = 1u << 12,
    PAD_START  = 1u << 13,
    PAD_MENU   = 1u << 14,
};

#endif /* PID351_H */
