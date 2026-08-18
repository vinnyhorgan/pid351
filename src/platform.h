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

/* ------------------------------------------------------- instrumentation */

/* Bring-up only, and the reason it exists is worth writing down: we were
 * about to spend real effort moving the rotate blit onto the RGA without
 * ever having measured what the blit costs. Everything below is here so that
 * decision is made from numbers.
 *
 * Microseconds spent in the last frame's rotate-and-scale blit, and in the
 * last block on vblank. Deliberately raw single samples and not an average -
 * the caller wants the distribution. Preemption can only ever add time to a
 * measurement, never subtract it, so the minimum over many frames is the
 * honest estimate of what the blit actually costs and the spread above it is
 * a measure of how much the rest of the system is interfering.
 *
 * Either may be left 0 by a backend where the quantity does not exist. */
void plat_frame_us(uint32_t *blit_us, uint32_t *wait_us);

/* Time n blits of a src_w x src_h source into the back buffer, filling
 * samples[] with the duration of each in microseconds. Nothing is presented.
 *
 * Separate from the live timing above because the live path only ever runs
 * one source size, and the blit's cost is dominated by whether the source
 * fits in cache - so the number that matters is different for every console.
 * Sweeping the sizes here is the only way to learn that before there is an
 * emulator to run.
 *
 * src must hold at least src_w * src_h pixels and must not exceed the panel.
 * Returns 0, or -1 if the backend cannot do this meaningfully. */
int plat_bench(const px_t *src, int src_w, int src_h,
               uint32_t *samples, int n);

/* The control for the above. Writes the same number of bytes into the same
 * back buffer with the same loop shape, but reads its source sequentially
 * instead of striding down a column.
 *
 * Without this the sweep only says the blit is expensive, not why. If the
 * control is nearly as slow, the cost is the sheer volume of writes and only
 * moving the work off the CPU helps. If the control is much faster, the cost
 * is the access pattern, and tiling the loop fixes it for twenty lines and no
 * device tree at all. Those two answers point at completely different work.
 *
 * src must hold at least a full panel. Returns 0, or -1 if unavailable. */
int plat_bench_linear(const px_t *src, uint32_t *samples, int n);

#endif /* PLATFORM_H */
