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
 * particular console calls them; per-core mapping happens above this. */
enum {
    BTN_A      = 1u << 0,
    BTN_B      = 1u << 1,
    BTN_X      = 1u << 2,
    BTN_Y      = 1u << 3,
    BTN_UP     = 1u << 4,
    BTN_DOWN   = 1u << 5,
    BTN_LEFT   = 1u << 6,
    BTN_RIGHT  = 1u << 7,
    BTN_L1     = 1u << 8,
    BTN_R1     = 1u << 9,
    BTN_L2     = 1u << 10,
    BTN_R2     = 1u << 11,
    BTN_SELECT = 1u << 12,
    BTN_START  = 1u << 13,
    BTN_MENU   = 1u << 14,
};

#endif /* PID351_H */
