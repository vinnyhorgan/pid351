/* pid351 - platform interface
 *
 * Two implementations exist and exactly one is linked in:
 *   plat_sdl.c  - laptop, SDL2, the fast development loop
 *   plat_drm.c  - the RG351P, KMS + RGA + evdev + ALSA
 *
 * Deliberately plain functions rather than a struct of pointers: the choice is
 * made at link time, so runtime indirection would buy nothing and cost a call
 * through memory on every frame.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include "pid351.h"

/* Returns 0 on success. On failure the platform prints its own diagnosis. */
int  plat_init(void);
void plat_shutdown(void);

/* Current button state as a bitmask of BTN_*. Edge detection is the caller's
 * job; the platform only reports what is held right now. */
uint32_t plat_input(void);

/* Hand over one frame at the core's native resolution. The platform scales,
 * rotates and blits it to the panel however is cheapest for that platform. */
void plat_present(const px_t *fb, int w, int h);

/* Monotonic microseconds. Never wall clock; this drives frame pacing. */
uint64_t plat_now_us(void);

/* Block until the given timestamp. Must genuinely block - a spin loop here is
 * the single most effective way to ruin battery life on this device. */
void plat_sleep_until(uint64_t deadline_us);

/* True once the platform wants the process to end (window closed, etc). */
int plat_should_quit(void);

/* Analog axes, raw driver values.
 *
 * Bring-up only. Not one of GBC, GBA, NES, SNES or Genesis has an analog
 * stick, so nothing in pid351 proper will ever call this - it exists so the
 * demo can show which physical stick drives which axis, because that is not
 * written down anywhere and the pad reports axes the kernel names generically.
 * Delete it once the answer is in docs/hardware.md. */
#define PLAT_AXIS_MAX 8

typedef struct {
    const char *name;    /* short label from the kernel's code, e.g. "RX" */
    int value;
    int min, max;        /* driver-reported range, for scaling a bar */
} plat_axis_t;

/* Fills up to max entries, returns how many. */
int plat_axes(plat_axis_t *out, int max);

#endif /* PLATFORM_H */
