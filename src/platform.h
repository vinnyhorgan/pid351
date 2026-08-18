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

/* Being PID 1. plat_boot_init mounts what an init has to mount and returns
 * non-zero only when we really are PID 1; everything else here is inert
 * otherwise, so the same binary still runs as an ordinary process. On the
 * laptop these are stubs - there is no init to be.
 *
 * plat_boot_save_log copies the kernel ring buffer to the boot partition.
 * With no serial port that is the only way a boot can explain itself, so it
 * is called on the way out and again if anything goes wrong.
 *
 * plat_boot_shutdown does not return when we are PID 1: returning from main
 * as PID 1 is a kernel panic, so exiting has to be an explicit act. */
int  plat_boot_init(void);
int  plat_is_init(void);
int  plat_boot_save_log(const char *name);
void plat_boot_shutdown(int power_off);

/* Time n blits of a src_w x src_h source into the back buffer, filling
 * samples[] with the duration of each in microseconds. Nothing is presented.
 *
 * Separate from the live timing above because the live path only ever runs
 * one source size and one implementation, and the measurement said the cost
 * is dominated by the access pattern - so the interesting comparison is
 * between implementations, on every console's native size, before any of them
 * ships.
 *
 * src must hold at least src_w * src_h pixels and must not exceed the panel.
 * tile is ignored except by the tiled variants. Returns 0, or -1 if the
 * backend cannot do this meaningfully. */
enum {
    PLAT_BLIT_LINEAR = 0, /* control: same writes, sequential reads, no rotate */
    PLAT_BLIT_STRIDED,    /* the first one: linear writes, strided reads */
    PLAT_BLIT_TILED,      /* inverted: sequential reads, strided writes */
    PLAT_BLIT_STAGED,     /* sequential both ways, transposing via a cache tile */
    PLAT_BLIT_STAGED_ROW, /* as STAGED but the tile is the full panel width */
};

int plat_bench(const px_t *src, int src_w, int src_h, int variant, int tile,
               uint32_t *samples, int n);

/* Number of pixels where the variant disagrees with what ships today. A blit
 * that is faster and wrong is worth nothing, and the three implementations
 * walk the buffer in three different orders, so this is checked rather than
 * eyeballed. 0 means identical output. -1 if unavailable. */
int plat_blit_verify(const px_t *src, int src_w, int src_h,
                     int variant, int tile);

/* Microseconds to touch a full panel's worth of pixels sequentially, in the
 * scanout buffer and in ordinary memory, taking the best of iters.
 *
 * This is the question the whole blit argument has been resting on without
 * anyone checking: a DRM dumb buffer may be cached or it may be write
 * combined, and that single fact decides which candidate can win. Write
 * combined memory takes sequential writes at close to full speed and punishes
 * reads and partial line writes brutally, so if fb_read and fb_rmw come back
 * an order of magnitude above ram_read and ram_rmw, the staged variant is the
 * only one that can help and the tiled one will be worse than what we have.
 *
 * Cheap to measure, and it turns the choice from an argument into a lookup. */
typedef struct {
    uint32_t fb_write, fb_read, fb_rmw;
    uint32_t ram_write, ram_read, ram_rmw;
} plat_mem_t;

int plat_mem_probe(plat_mem_t *out, int iters);

/* The panel's real refresh rate, two independent ways.
 *
 * Both are needed because they can disagree and the disagreement is the
 * interesting part. exact_mhz is what the mode's own timing says: the pixel
 * clock divided by the total line and frame counts, which is what the
 * hardware will actually do. measured_mhz comes from doing flips back to back
 * with no sleep at all and counting how long they took, which is what we will
 * actually get.
 *
 * Settled: 60.0186 Hz, from a 17.000000 MHz pixel clock (cpll 408 MHz over an
 * exact divide by 24) and 584x485 totals. Both are in millihertz. */
void plat_mode_timing(uint32_t *exact_mhz, uint32_t *clock_khz,
                      uint32_t *htotal, uint32_t *vtotal);

int plat_vblank_probe(int flips, uint32_t *measured_mhz);

/* Everything the display pipeline will tell us about itself: every plane,
 * every property with its name and permitted values, every pixel format.
 *
 * One question in there is worth the whole probe. If a plane carries a
 * rotation property that accepts 90 degrees, the display controller can do
 * the transform itself and the entire rotate blit - the thing four commits
 * and two device runs have been spent on - deletes. Reasoning says a 90
 * degree rotation cannot be streamed and so needs a line buffer the VOP
 * probably has not got, which is exactly why Rockchip ships RGA. But that is
 * reasoning, and this is the last chance to look. */
void plat_dump_props(void);

#endif /* PLATFORM_H */
