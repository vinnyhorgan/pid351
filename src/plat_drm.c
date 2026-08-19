/* pid351 - RG351P backend: KMS, evdev, and the rotation the panel demands
 *
 * Talks to the kernel through raw ioctls on the DRM uapi headers rather than
 * linking libdrm. libdrm would add a shared library to a binary whose whole
 * premise is that it depends on nothing; <drm/drm_mode.h> is a kernel uapi
 * header in exactly the same sense as <linux/input.h>, so using it costs
 * nothing at link time.
 *
 * The panel is natively portrait 320x480 and advertises no other mode, so
 * every frame has to be rotated 90 degrees. Every property on every plane was
 * dumped on the device and there is no rotation property anywhere on the
 * pipeline, so the CPU doing it is not a fallback - it is the only mechanism
 * that exists. It happens with contiguous writes into write-combined memory,
 * which is what the whole design is bent around. Scaling used to ride along
 * in the same pass and no longer does: scale.c resamples first and hands this
 * file something already at panel size, so the blit here is a pure rotation.
 * See docs/hardware.md.
 *
 * This file also contains what it takes to be PID 1, because mounting /dev is
 * as much a property of this machine as opening card0 is.
 *
 * Deliberately not here yet:
 *   - ALSA. The platform interface has no audio entry point yet.
 *   - Suspend and savestate-on-poweroff.
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/input.h>
#include <linux/reboot.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "pid351.h"
#include "platform.h"
#include "scale.h"

/* This targets one machine, so these are constants rather than a search.
 * card0 is the only DRM device and DSI-1 the only connector; the gamepad is
 * a USB HID device with a fixed id. All confirmed live in docs/hardware.md. */
#define DRM_CARD    "/dev/dri/card0"
#define PAD_VENDOR  0x1209
#define PAD_PRODUCT 0x3100

/* How long to let hardware turn up before giving up on it. Only ever reached
 * when we are PID 1 - under another init everything is long since probed. */
#define DRM_WAIT_MS  8000
#define PAD_WAIT_MS  8000
#define MMC_WAIT_MS  8000

/* The panel's own orientation: a scanline is 320 pixels and there are 480 of
 * them. Our framebuffer is landscape, so physical width is our height. */
#define PHYS_W PANEL_H
#define PHYS_H PANEL_W

/* Which way the panel is mounted relative to how the console is held.
 *
 * Settled by looking at it: clockwise put the red top-left marker in the
 * bottom-right corner and the green top-right marker in the bottom-left, both
 * markers landing exactly 180 degrees out with no mirroring, so the panel
 * wants the other direction. That is what the asymmetric markers in main.c
 * exist for. */
#define ROTATE_CW 0

/* Log every key event with its raw code. Off now that the map below is
 * confirmed against the hardware; turn it back on to re-derive the map if a
 * different shell ever turns up. */
#define PAD_TRACE 0

/* The left stick stands in for the d-pad, which is more comfortable for long
 * sessions. It is resolved down here so that nothing above the platform layer
 * ever learns this machine has an analog stick - the cores certainly must not,
 * since not one console pid351 targets has one.
 *
 * ABS_Z and ABS_RX are the left stick and are inverted on BOTH axes relative
 * to the right one (docs/hardware.md). Assuming the usual convention would
 * have produced a stick that moves the wrong way in all four directions.
 *
 * The deadzone is large because this is a d-pad substitute, not an analog
 * control: an accidental brush must not register, and there is no benefit to
 * resolving anything finer than the eight directions a hat gives. */
#define STICK_AS_DPAD 1
#define STICK_X_AXIS  ABS_Z
#define STICK_Y_AXIS  ABS_RX
#define STICK_DEADZONE_PCT 45

/* Which rotate blit the live path uses. Settled by measurement on this panel
 * and then fixed, because there are no config files and there will not be
 * any. The full-panel-width variant won on every emulated source and lost by
 * 20% on a native 480x320 one: its gather touches one cache line per panel
 * column, and that working set only stays resident while the source is small.
 * The crossover sits between the largest console frame (320x224, 71680 px)
 * and a native one (153600 px), so split on source area rather than pick a
 * single loser. Sizes below are the winning tile from the same sweep. */
#define BLIT_ROW_MAX_PX 100000
#define BLIT_ROW_ROWS   32
#define BLIT_TILE       64

#define NBUF 2

struct dumb_buf {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    uint64_t size;
    px_t    *px;
};

/* sig_atomic_t because a signal handler may only touch this type. SIGTERM is
 * how first-light.sh ends the run - without this the most informative run
 * would exit through _exit() with its report unwritten and the cpu governor
 * left wherever the measurement put it. */
static volatile sig_atomic_t sig_quit;

static void on_signal(int sig)
{
    (void)sig;
    sig_quit = 1;
}

static struct {
    int      fd;
    uint32_t crtc_id;
    uint32_t conn_id;
    struct drm_mode_modeinfo mode;
    struct dumb_buf buf[NBUF];
    int      front;          /* buffer currently being scanned out */
    int      flip_pending;
    int      had_master;

    uint32_t blit_us;        /* see plat_frame_us */
    uint32_t scale_us;
    uint32_t wait_us;
    /* When the display latched the last flip, from the flip event's own
     * timestamp. It is the one instant in the pipeline we cannot infer: from
     * the queue onwards the clock is the panel's, not ours. */
    uint64_t flip_us;
    /* The vblank counter the flip was latched at. Its gaps are the only
     * direct evidence of a frame the panel showed twice: everything else we
     * measure is our side of the handover, and a flip that misses its vblank
     * simply goes up one period later with nothing anywhere saying so. */
    uint32_t flip_seq;

    int      pad_fd;
    uint32_t buttons;

    plat_axis_t axis[PLAT_AXIS_MAX];
    uint16_t    axis_code[PLAT_AXIS_MAX];
    int         axis_count;

    /* Kept apart so that a stick pushed one way and the hat pushed the other
     * do not fight over the same bits; plat_input merges them. */
    uint32_t hat_dirs;
    uint32_t stick_dirs;
} g;

/* ---------------------------------------------------------------- helpers */

/* The DRM_IOCTL_* macros are unsigned long, which is also what glibc's ioctl
 * takes, so no cast belongs here. Retrying on EINTR and EAGAIN is the caller
 * contract every one of these ioctls expects. */
static int xioctl(unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(g.fd, req, arg);
    } while (r == -1 && (errno == EINTR || errno == EAGAIN));
    return r;
}

/* KERN_ERR, so that this survives `quiet` on the command line and reaches the
 * panel. Every other line we print is deliberately below the console's
 * threshold - a running game must not be able to scribble on its own screen -
 * but this one is only ever reached when something has already gone wrong,
 * and the panel is the only channel left if it went wrong before the card was
 * mounted and pid351-fail.log could be written.
 *
 * The prefix is /dev/kmsg's, is stripped before the message is stored, and
 * only makes sense when we are PID 1 writing there; anywhere else it would
 * print literally into a terminal. */
static int is_init;

static void fail(const char *what)
{
    fprintf(stderr, "%spid351: %s: %s\n",
            is_init ? "<3>" : "", what, strerror(errno));
}

/* ------------------------------------------------------------------- KMS */

/* Find the connected connector and the crtc that can drive it. There is only
 * one of each on this machine, but the ioctl dance is the same either way. */
static int pick_display(void)
{
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fail("MODE_GETRESOURCES");
        return -1;
    }

    uint32_t *conns = calloc(res.count_connectors, sizeof *conns);
    uint32_t *crtcs = calloc(res.count_crtcs, sizeof *crtcs);
    uint32_t *encs  = calloc(res.count_encoders, sizeof *encs);
    uint32_t *fbs   = calloc(res.count_fbs ? res.count_fbs : 1, sizeof *fbs);
    if (!conns || !crtcs || !encs || !fbs) {
        fprintf(stderr, "pid351: out of memory picking display\n");
        free(conns); free(crtcs); free(encs); free(fbs);
        return -1;
    }

    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
    res.encoder_id_ptr   = (uint64_t)(uintptr_t)encs;
    res.fb_id_ptr        = (uint64_t)(uintptr_t)fbs;
    if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        fail("MODE_GETRESOURCES (2)");
        goto err;
    }

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector c;
        memset(&c, 0, sizeof c);
        c.connector_id = conns[i];
        if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
            continue;
        if (c.connection != 1 || c.count_modes == 0)   /* 1 = connected */
            continue;

        struct drm_mode_modeinfo *modes =
            calloc(c.count_modes, sizeof *modes);
        uint32_t *cencs = calloc(c.count_encoders ? c.count_encoders : 1,
                                 sizeof *cencs);
        uint32_t *props = calloc(c.count_props ? c.count_props : 1,
                                 sizeof *props);
        uint64_t *pvals = calloc(c.count_props ? c.count_props : 1,
                                 sizeof *pvals);
        if (!modes || !cencs || !props || !pvals) {
            free(modes); free(cencs); free(props); free(pvals);
            goto err;
        }

        c.modes_ptr        = (uint64_t)(uintptr_t)modes;
        c.encoders_ptr     = (uint64_t)(uintptr_t)cencs;
        c.props_ptr        = (uint64_t)(uintptr_t)props;
        c.prop_values_ptr  = (uint64_t)(uintptr_t)pvals;
        int ok = xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c) == 0
                 && c.count_modes > 0;

        if (ok) {
            g.conn_id = c.connector_id;
            g.mode    = modes[0];

            /* Prefer the encoder already attached; fall back to any crtc the
             * connector's encoders allow. */
            uint32_t enc_id = c.encoder_id;
            if (enc_id == 0 && c.count_encoders > 0)
                enc_id = cencs[0];

            struct drm_mode_get_encoder e;
            memset(&e, 0, sizeof e);
            e.encoder_id = enc_id;
            if (enc_id && xioctl(DRM_IOCTL_MODE_GETENCODER, &e) == 0) {
                if (e.crtc_id)
                    g.crtc_id = e.crtc_id;
                else
                    for (uint32_t b = 0; b < res.count_crtcs; b++)
                        if (e.possible_crtcs & (1u << b)) {
                            g.crtc_id = crtcs[b];
                            break;
                        }
            }
            if (g.crtc_id == 0 && res.count_crtcs > 0)
                g.crtc_id = crtcs[0];
        }

        free(modes); free(cencs); free(props); free(pvals);
        if (ok && g.crtc_id)
            break;
    }

    free(conns); free(crtcs); free(encs); free(fbs);

    if (!g.conn_id || !g.crtc_id) {
        fprintf(stderr, "pid351: no connected connector with a crtc\n");
        return -1;
    }
    printf("pid351: connector %u, crtc %u, mode %ux%u@%u\n",
           g.conn_id, g.crtc_id, g.mode.hdisplay, g.mode.vdisplay,
           g.mode.vrefresh);

    if (g.mode.hdisplay != PHYS_W || g.mode.vdisplay != PHYS_H)
        fprintf(stderr, "pid351: warning: mode is %ux%u, built for %dx%d\n",
                g.mode.hdisplay, g.mode.vdisplay, PHYS_W, PHYS_H);
    return 0;

err:
    free(conns); free(crtcs); free(encs); free(fbs);
    return -1;
}

/* RGB565 dumb buffers. depth 16 with bpp 16 is what the legacy ADDFB path
 * turns into DRM_FORMAT_RGB565, which is the format the VOP scans out
 * natively - the whole reason px_t is 16 bits wide. */
static int make_buffer(struct dumb_buf *b)
{
    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof cd);
    cd.width  = (uint32_t)PHYS_W;
    cd.height = (uint32_t)PHYS_H;
    cd.bpp    = 16;
    if (xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        fail("MODE_CREATE_DUMB");
        return -1;
    }
    b->handle = cd.handle;
    b->pitch  = cd.pitch;
    b->size   = cd.size;

    struct drm_mode_fb_cmd fb;
    memset(&fb, 0, sizeof fb);
    fb.width  = (uint32_t)PHYS_W;
    fb.height = (uint32_t)PHYS_H;
    fb.pitch  = cd.pitch;
    fb.bpp    = 16;
    fb.depth  = 16;
    fb.handle = cd.handle;
    if (xioctl(DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        fail("MODE_ADDFB");
        return -1;
    }
    b->fb_id = fb.fb_id;

    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (xioctl(DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        fail("MODE_MAP_DUMB");
        return -1;
    }
    void *p = mmap(NULL, (size_t)cd.size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, g.fd, (off_t)md.offset);
    if (p == MAP_FAILED) {
        fail("mmap dumb buffer");
        return -1;
    }
    b->px = p;
    memset(b->px, 0, (size_t)cd.size);
    return 0;
}

/* ----------------------------------------------------------------- input */

/* The kernel names axes generically, so these labels are only a rendering
 * convenience - which physical stick moves which is what the demo is for. */
static const char *axis_name(unsigned code)
{
    switch (code) {
    case ABS_X:        return "X";
    case ABS_Y:        return "Y";
    case ABS_Z:        return "Z";
    case ABS_RX:       return "RX";
    case ABS_RY:       return "RY";
    case ABS_RZ:       return "RZ";
    case ABS_THROTTLE: return "THR";
    case ABS_RUDDER:   return "RUD";
    case ABS_WHEEL:    return "WHL";
    case ABS_GAS:      return "GAS";
    case ABS_BRAKE:    return "BRK";
    default:           return "ABS";
    }
}

/* The hats are the d-pad and are already reported as buttons, so they are not
 * axes as far as anything above here is concerned. */
static void discover_axes(int fd)
{
    unsigned long bits[(ABS_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, (unsigned long)EVIOCGBIT(EV_ABS, sizeof bits), bits) < 0)
        return;

    /* Unsigned rather than uint16_t so that EVIOCGABS's internal 0x40 + code
     * stays unsigned instead of promoting to int and back. */
    for (unsigned code = 0; code < ABS_CNT && g.axis_count < PLAT_AXIS_MAX;
         code++) {
        size_t word = code / (8 * sizeof(long));
        size_t bit  = code % (8 * sizeof(long));
        if (!(bits[word] & (1UL << bit)))
            continue;
        if (code == ABS_HAT0X || code == ABS_HAT0Y)
            continue;

        struct input_absinfo ai;
        memset(&ai, 0, sizeof ai);
        if (ioctl(fd, (unsigned long)EVIOCGABS(code), &ai) < 0)
            continue;

        int i = g.axis_count++;
        g.axis_code[i]  = (uint16_t)code;
        g.axis[i].name  = axis_name(code);
        g.axis[i].value = ai.value;
        g.axis[i].min   = ai.minimum;
        g.axis[i].max   = ai.maximum;
    }
}

/* Event node numbering is not stable - the pad was event2 on ArkOS and event5
 * on ROCKNIX, because the vibrator, power key and jack detect took the low
 * numbers. So match on the USB id, and among this device's three nodes take
 * the one with absolute axes, which is the pad rather than the keyboard or
 * mouse it also presents. */
static int scan_pad(void)
{
    DIR *d = opendir("/dev/input");
    if (!d)
        return -1;

    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;

        /* d_name can be 255 bytes; sized so the path cannot be truncated. */
        char path[288];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct input_id id;
        unsigned long abs_bits = 0;
        if (ioctl(fd, EVIOCGID, &id) == 0
            && id.vendor == PAD_VENDOR && id.product == PAD_PRODUCT
            && ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), &abs_bits) >= 0
            && abs_bits != 0) {
            char name[128] = "";
            ioctl(fd, EVIOCGNAME(sizeof name), name);
            printf("pid351: pad on %s (%s)\n", path, name);
            discover_axes(fd);
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    return found;
}

/* The pad is a full speed USB device behind an internal high speed hub, and
 * enumerating it takes about 2.4 seconds from power on - measured, see the
 * boot log in docs/. Under another system's init that wait was absorbed by
 * everything else starting up; as PID 1 there is nobody to absorb it, so we
 * do it ourselves.
 *
 * Sleeping between looks is not the busy-waiting the project forbids: that
 * rule is about burning a core to pass time we could have blocked through.
 * There is nothing here to block on - device nodes appear without announcing
 * themselves - and 20 ms of sleep per look costs nothing measurable. */
static int open_pad(void)
{
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    int waited = 0;

    for (;;) {
        int fd = scan_pad();
        if (fd >= 0) {
            if (waited)
                printf("pid351: pad appeared after %d ms\n", waited);
            return fd;
        }
        if (waited >= PAD_WAIT_MS)
            break;
        nanosleep(&ts, NULL);
        waited += 20;
    }

    fprintf(stderr, "pid351: no pad matching %04x:%04x with abs axes "
            "after %d ms\n", PAD_VENDOR, PAD_PRODUCT, PAD_WAIT_MS);
    return -1;
}

/* The pad reports a plain sequential HID button order starting at BTN_A
 * (0x130), and the kernel's names across that range describe a generic
 * gamepad rather than this shell. They disagree badly: 0x13a is BTN_SELECT to
 * the kernel and L2 on the plastic, 0x136 is BTN_TL to the kernel and SELECT
 * on the plastic. Mapping by kernel name therefore produces a pad where
 * START+SELECT does nothing and L2+R2 opens the menu, which is precisely what
 * it did.
 *
 * So the codes are written numerically with the shell's own label beside each
 * one, derived by pressing every button in a known order and reading the log
 * back (docs/first-light-2.log). Numbers with a comment beat names that lie.
 */
static uint32_t map_key(uint16_t code)
{
    switch (code) {
    case 0x130: return PAD_A;        /* kernel BTN_A      */
    case 0x131: return PAD_B;        /* kernel BTN_B      */
    case 0x132: return PAD_X;        /* kernel BTN_C      */
    case 0x133: return PAD_Y;        /* kernel BTN_X      */
    case 0x134: return PAD_L1;       /* kernel BTN_Y      */
    case 0x135: return PAD_R1;       /* kernel BTN_Z      */
    /* The pad reports START before SELECT, which is the opposite of how they
     * sit on the shell. Confirmed by pressing them. */
    case 0x136: return PAD_START;    /* kernel BTN_TL     */
    case 0x137: return PAD_SELECT;   /* kernel BTN_TR     */
    case 0x138: return PAD_L3;       /* kernel BTN_TL2, left stick click  */
    case 0x139: return PAD_R3;       /* kernel BTN_TR2, right stick click */
    case 0x13a: return PAD_L2;       /* kernel BTN_SELECT */
    case 0x13b: return PAD_R2;       /* kernel BTN_START  */
    default:    return 0;
    }
}

#if STICK_AS_DPAD
/* Centre and half range come from the driver rather than being assumed, so a
 * stick that does not report a full 0..4095 still deadzones correctly. */
static uint32_t stick_dir(unsigned code, uint32_t low, uint32_t high)
{
    for (int i = 0; i < g.axis_count; i++) {
        if (g.axis_code[i] != code)
            continue;

        int span = g.axis[i].max - g.axis[i].min;
        if (span <= 0)
            return 0;

        int centre = g.axis[i].min + span / 2;
        int offset = g.axis[i].value - centre;
        int limit  = (span / 2) * STICK_DEADZONE_PCT / 100;

        if (offset >  limit) return high;
        if (offset < -limit) return low;
        return 0;
    }
    return 0;
}

static void update_stick_dirs(void)
{
    /* Inverted on both axes: a low value on X is right, a low value on Y is
     * down. Measured, not assumed. */
    g.stick_dirs = stick_dir(STICK_X_AXIS, PAD_RIGHT, PAD_LEFT)
                 | stick_dir(STICK_Y_AXIS, PAD_DOWN,  PAD_UP);
}
#endif

static void pump_input(void)
{
    if (g.pad_fd < 0)
        return;

    struct input_event ev;
    ssize_t n;
    while ((n = read(g.pad_fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
        if (ev.type == EV_KEY) {
            uint32_t bit = map_key(ev.code);
#if PAD_TRACE
            if (ev.value != 2)   /* 2 is autorepeat, not a state change */
                printf("pid351: key 0x%03x %s%s\n", ev.code,
                       ev.value ? "down" : "up  ",
                       bit ? "" : "   UNMAPPED");
#endif
            if (!bit)
                continue;
            if (ev.value)
                g.buttons |= bit;
            else
                g.buttons &= ~bit;
        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_HAT0X) {
                g.hat_dirs &= ~(uint32_t)(PAD_LEFT | PAD_RIGHT);
                if (ev.value < 0) g.hat_dirs |= PAD_LEFT;
                if (ev.value > 0) g.hat_dirs |= PAD_RIGHT;
            } else if (ev.code == ABS_HAT0Y) {
                g.hat_dirs &= ~(uint32_t)(PAD_UP | PAD_DOWN);
                if (ev.value < 0) g.hat_dirs |= PAD_UP;
                if (ev.value > 0) g.hat_dirs |= PAD_DOWN;
            } else {
                for (int i = 0; i < g.axis_count; i++)
                    if (g.axis_code[i] == ev.code) {
                        g.axis[i].value = ev.value;
#if STICK_AS_DPAD
                        if (ev.code == STICK_X_AXIS
                            || ev.code == STICK_Y_AXIS)
                            update_stick_dirs();
#endif
                        break;
                    }
            }
        }
    }
}

/* ----------------------------------------------------------- rotate+scale */

/* Policy: the source always fills the panel, minus the status bar's columns.
 * No black bars on anything, which is a decision about how it looks rather
 * than about code - but it has a cost worth stating, because it deletes the
 * cheap cases. The 4:3 consoles used to be half black bar being memset; now
 * every console lands on the same 153600 pixel blit, so there is exactly one
 * blit configuration in the system and it is the expensive one.
 *
 * These tables are the identity on both axes for anything scale.c has
 * already resampled, and remain a real scale only for the sources it passes
 * through untouched - GBA. Left as tables rather than special-cased: an
 * arbitrary ratio is the same lookup as an integer one, and a branch here
 * would buy nothing while making two paths to test instead of one.
 *
 * The measurement that shaped the rest: at full screen the strided blit takes
 * 2487 us against 695 us for the same writes with a sequential source. Nearly
 * three quarters of the cost is walking down a column of the source, touching
 * a fresh cache line for every two bytes kept. So there are three
 * implementations here and the panel's own silicon picks the winner. */

/* Destination x picks a source row, destination y picks a source column.
 * Filling means neither can ever be out of range, so the per-pixel bounds
 * test that the bordered version needed is gone from all three inner loops. */
static int map_row[PHYS_W];
static int map_col[PHYS_H];

/* How many landscape columns the picture gets. The rest is status bar, and is
 * written after the blit rather than inside it: a destination scanline is a
 * column of the landscape image, so the bar occupies a contiguous run of
 * whole scanlines and the variants need no per-pixel test. That trades one
 * extra pass over 53x320 - about a tenth of the blit - against a branch in
 * the inner loop of an in-order core, which is the right way round. */
static int game_w = PANEL_W;

static void build_maps(int w, int h)
{
    for (int px = 0; px < PHYS_W; px++) {
#if ROTATE_CW
        int ly = PANEL_H - 1 - px;
#else
        int ly = px;
#endif
        map_row[px] = ly * h / PANEL_H;
    }
    for (int py = 0; py < PHYS_H; py++) {
#if ROTATE_CW
        int lx = py;
#else
        int lx = PANEL_W - 1 - py;
#endif
        /* Columns past the picture are the bar's, and are overwritten
         * below; mapping them to source column 0 keeps the blit branchless
         * and costs one wasted write per bar pixel. */
        map_col[py] = lx < game_w ? lx * w / game_w : 0;
    }
}

/* The bar, straight into the scanout buffer, in the same rotation the blit
 * uses. `bar` is BAR_W by PANEL_H in landscape order. */
static void blit_bar(struct dumb_buf *b, const px_t *bar)
{
    int stride = (int)(b->pitch / sizeof(px_t));
    int bw = PANEL_W - game_w;

    for (int py = 0; py < PHYS_H; py++) {
#if ROTATE_CW
        int lx = py;
#else
        int lx = PANEL_W - 1 - py;
#endif
        if (lx < game_w)
            continue;
        px_t *dst = b->px + (size_t)py * (size_t)stride;
        for (int px = 0; px < PHYS_W; px++) {
#if ROTATE_CW
            int ly = PANEL_H - 1 - px;
#else
            int ly = px;
#endif
            dst[px] = bar[(size_t)ly * (size_t)bw + (size_t)(lx - game_w)];
        }
    }
}

/* Linear writes, strided reads. What shipped first, and the baseline the
 * other two have to beat. A destination scanline is a column of the landscape
 * image, so the writes stay sequential and the source pays for it. */
static void blit_strided(struct dumb_buf *b, const px_t *src, int w, int h)
{
    int stride = (int)(b->pitch / sizeof(px_t));

    build_maps(w, h);

    for (int py = 0; py < PHYS_H; py++) {
        px_t *dst = b->px + (size_t)py * (size_t)stride;
        const px_t *col = src + map_col[py];

        for (int px = 0; px < PHYS_W; px++)
            dst[px] = col[(size_t)map_row[px] * (size_t)w];
    }
}

/* Sequential reads, strided writes. Within a tile the same handful of
 * destination lines are revisited by every column, so they should stay in L1
 * for the whole tile - if the destination is cacheable at all. */
static void blit_tiled(struct dumb_buf *b, const px_t *src, int w, int h,
                       int tile)
{
    int stride = (int)(b->pitch / sizeof(px_t));

    build_maps(w, h);

    for (int py0 = 0; py0 < PHYS_H; py0 += tile) {
        int pyN = py0 + tile < PHYS_H ? py0 + tile : PHYS_H;

        for (int px0 = 0; px0 < PHYS_W; px0 += tile) {
            int pxN = px0 + tile < PHYS_W ? px0 + tile : PHYS_W;

            for (int px = px0; px < pxN; px++) {
                const px_t *row = src + (size_t)map_row[px] * (size_t)w;

                for (int py = py0; py < pyN; py++)
                    b->px[(size_t)py * (size_t)stride + (size_t)px] =
                        row[map_col[py]];
            }
        }
    }
}

/* Sequential both ways. Gathers a tile into a staging block small enough to
 * live in L1, transposing it there where a scattered write costs nothing,
 * then writes each destination row out as one contiguous run. This is the
 * variant that should survive a write-combined destination, where partial
 * cache line writes are exactly what hurts. */
#define STAGE_MAX 64

static void blit_staged(struct dumb_buf *b, const px_t *src, int w, int h,
                        int tile)
{
    static px_t stage[STAGE_MAX * STAGE_MAX];

    int stride = (int)(b->pitch / sizeof(px_t));

    if (tile > STAGE_MAX)
        tile = STAGE_MAX;

    build_maps(w, h);

    for (int py0 = 0; py0 < PHYS_H; py0 += tile) {
        int pyN = py0 + tile < PHYS_H ? py0 + tile : PHYS_H;

        for (int px0 = 0; px0 < PHYS_W; px0 += tile) {
            int pxN = px0 + tile < PHYS_W ? px0 + tile : PHYS_W;

            for (int px = px0; px < pxN; px++) {
                const px_t *row = src + (size_t)map_row[px] * (size_t)w;

                for (int py = py0; py < pyN; py++)
                    stage[(size_t)(py - py0) * (size_t)tile
                          + (size_t)(px - px0)] = row[map_col[py]];
            }

            for (int py = py0; py < pyN; py++)
                memcpy(b->px + (size_t)py * (size_t)stride + (size_t)px0,
                       stage + (size_t)(py - py0) * (size_t)tile,
                       (size_t)(pxN - px0) * sizeof(px_t));
        }
    }
}

/* The control: same writes, same loop shape, sequential source, no rotation.
 * Not a candidate - it draws the wrong thing - but it is the floor the
 * candidates are aiming at, and the gap to it is what the striding costs. */
static void blit_linear(struct dumb_buf *b, const px_t *src)
{
    int stride = (int)(b->pitch / sizeof(px_t));

    for (int py = 0; py < PHYS_H; py++) {
        px_t *dst = b->px + (size_t)py * (size_t)stride;
        const px_t *row = src + (size_t)py * (size_t)PHYS_W;
        for (int px = 0; px < PHYS_W; px++)
            dst[px] = row[px];
    }
}

/* As STAGED, but the tile spans the whole panel width, so every write to the
 * scanout buffer is a complete destination row. Write combining rewards long
 * contiguous runs and the tile sweep was still improving at the largest
 * square tile tried, so this is where that trend points. The staging block is
 * PHYS_W by rows - 20KB at 32 rows, inside L1 - and 32 destination rows is
 * also exactly the 32 pixels a 64 byte cache line holds on the read side. */
static void blit_staged_row(struct dumb_buf *b, const px_t *src, int w, int h,
                            int rows)
{
    static px_t stage[PHYS_W * 32];

    int stride = (int)(b->pitch / sizeof(px_t));

    if (rows > 32) rows = 32;
    if (rows < 1)  rows = 1;

    build_maps(w, h);

    for (int py0 = 0; py0 < PHYS_H; py0 += rows) {
        int pyN = py0 + rows < PHYS_H ? py0 + rows : PHYS_H;

        for (int px = 0; px < PHYS_W; px++) {
            const px_t *row = src + (size_t)map_row[px] * (size_t)w;

            for (int py = py0; py < pyN; py++)
                stage[(size_t)(py - py0) * (size_t)PHYS_W + (size_t)px] =
                    row[map_col[py]];
        }

        for (int py = py0; py < pyN; py++)
            memcpy(b->px + (size_t)py * (size_t)stride,
                   stage + (size_t)(py - py0) * (size_t)PHYS_W,
                   (size_t)PHYS_W * sizeof(px_t));
    }
}

static void run_variant(struct dumb_buf *b, const px_t *src, int w, int h,
                        int variant, int tile)
{
    switch (variant) {
    case PLAT_BLIT_LINEAR: blit_linear(b, src);            break;
    case PLAT_BLIT_TILED:  blit_tiled(b, src, w, h, tile);  break;
    case PLAT_BLIT_STAGED: blit_staged(b, src, w, h, tile); break;
    case PLAT_BLIT_STAGED_ROW: blit_staged_row(b, src, w, h, tile); break;
    default:               blit_strided(b, src, w, h);      break;
    }
}

/* ------------------------------------------------------- platform surface */

int plat_init(void)
{
    memset(&g, 0, sizeof g);
    g.pad_fd = -1;

    g.fd = open(DRM_CARD, O_RDWR | O_CLOEXEC);
    if (g.fd < 0) {
        fail("open " DRM_CARD);
        fprintf(stderr, "pid351: is the user in group video?\n");
        return -1;
    }

    /* Whoever draws the UI holds DRM master until it is stopped. Say so
     * plainly, because the symptom otherwise is a mystifying EACCES. */
    if (ioctl(g.fd, DRM_IOCTL_SET_MASTER, 0) < 0) {
        fail("SET_MASTER");
        fprintf(stderr, "pid351: something else holds the display - stop the "
                        "UI first\n");
        close(g.fd);
        g.fd = -1;
        return -1;
    }
    g.had_master = 1;

    /* SA_RESTART deliberately absent: the point is to interrupt the blocking
     * read on the drm fd and the nanosleep, so the loop notices within a
     * frame rather than at the next vblank. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    if (pick_display() < 0)
        goto err;

    for (int i = 0; i < NBUF; i++)
        if (make_buffer(&g.buf[i]) < 0)
            goto err;

    struct drm_mode_crtc set;
    memset(&set, 0, sizeof set);
    set.crtc_id          = g.crtc_id;
    set.fb_id            = g.buf[0].fb_id;
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&g.conn_id;
    set.count_connectors = 1;
    set.mode             = g.mode;
    set.mode_valid       = 1;
    if (xioctl(DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        fail("MODE_SETCRTC");
        goto err;
    }
    g.front = 0;

    g.pad_fd = open_pad();   /* not fatal: a blank screen is still a result */
    printf("pid351: display up, rotating %s\n",
           ROTATE_CW ? "clockwise" : "counter-clockwise");
    return 0;

err:
    plat_shutdown();
    return -1;
}

void plat_shutdown(void)
{
    if (g.pad_fd >= 0) {
        close(g.pad_fd);
        g.pad_fd = -1;
    }
    if (g.fd < 0)
        return;

    for (int i = 0; i < NBUF; i++) {
        if (g.buf[i].px)
            munmap(g.buf[i].px, (size_t)g.buf[i].size);
        if (g.buf[i].fb_id) {
            uint32_t id = g.buf[i].fb_id;
            xioctl(DRM_IOCTL_MODE_RMFB, &id);
        }
        if (g.buf[i].handle) {
            struct drm_mode_destroy_dumb dd;
            memset(&dd, 0, sizeof dd);
            dd.handle = g.buf[i].handle;
            xioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        }
        memset(&g.buf[i], 0, sizeof g.buf[i]);
    }

    /* Hand the display back so whatever ran before us can have it again. */
    if (g.had_master)
        ioctl(g.fd, DRM_IOCTL_DROP_MASTER, 0);
    close(g.fd);
    g.fd = -1;
}

/* Drain one page flip completion. This is the only place the process waits on
 * the display, and it waits in poll rather than spinning, because a spin here
 * would burn a core for a whole frame every frame. */
static void wait_flip(void)
{
    if (!g.flip_pending)
        return;

    char buf[128];
    for (;;) {
        ssize_t n = read(g.fd, buf, sizeof buf);
        if (n >= (ssize_t)sizeof(struct drm_event)) {
            /* The event has always carried the vblank the flip was latched
             * at, and it was always being thrown away - which is why input
             * latency could only ever be quoted as a bound, assuming a whole
             * panel period between queueing a flip and it going up. The real
             * gap is whatever is left of the period, and the difference is
             * most of a frame.
             *
             * The kernel documents this stamp as the moment the refresh
             * cycle's first pixel leaves the display engine, which is exactly
             * the boundary wanted. memcpy rather than a cast because a read
             * buffer carries no alignment guarantee and drm_event_vblank has
             * a __u64 in it. */
            ssize_t off = 0;
            while (off + (ssize_t)sizeof(struct drm_event) <= n) {
                struct drm_event hdr;
                memcpy(&hdr, buf + off, sizeof hdr);
                if (hdr.length < sizeof hdr ||
                    (ssize_t)hdr.length > n - off)
                    break;
                if (hdr.type == DRM_EVENT_FLIP_COMPLETE &&
                    hdr.length >= sizeof(struct drm_event_vblank)) {
                    struct drm_event_vblank v;
                    memcpy(&v, buf + off, sizeof v);
                    g.flip_us = (uint64_t)v.tv_sec * 1000000u
                              + (uint64_t)v.tv_usec;
                    g.flip_seq = v.sequence;
                }
                off += (ssize_t)hdr.length;
            }
            break;
        }
        if (n < 0 && errno != EINTR) {
            fail("read drm event");
            break;
        }
    }
    g.flip_pending = 0;
}

/* ---- being PID 1 -------------------------------------------------------
 *
 * A process normally inherits /dev, /proc and /sys from whatever started it.
 * PID 1 inherits nothing, and CONFIG_DEVTMPFS_MOUNT says in as many words
 * that it does not apply to an initramfs root - so we mount them ourselves.
 *
 * Every function here is a no-op unless we really are PID 1, which is what
 * lets the identical binary still run as an ordinary process under another
 * system. That is not politeness, it is the only way to compare the two.
 *
 * reboot() and klogctl() are glibc extensions that _POSIX_C_SOURCE hides, so
 * they go through syscall() rather than widening the feature-test macros for
 * the whole build. <linux/reboot.h> is a uapi header in the same sense as
 * <drm/drm.h>, so the constants cost nothing. */
extern long syscall(long number, ...);

/* sync() is another _POSIX_C_SOURCE casualty; same treatment as the rest. */
static void flush_disks(void)
{
    syscall(__NR_sync);
}

#define BOOT_DEV   "/dev/mmcblk0p1"
#define BOOT_MOUNT "/boot"

/* Wait for a device node to be created by devtmpfs as its driver probes. See
 * the note above open_pad for why sleeping here is not the busy-waiting the
 * project forbids. */
static int wait_for_node(const char *path, int timeout_ms)
{
    struct timespec ts = { 0, 20 * 1000 * 1000 };
    int waited = 0;

    while (access(path, F_OK) != 0) {
        if (waited >= timeout_ms)
            return -1;
        nanosleep(&ts, NULL);
        waited += 20;
    }
    if (waited)
        printf("pid351: %s appeared after %d ms\n", path, waited);
    return 0;
}

int plat_is_init(void)
{
    return is_init;
}

/* mount(2) failing with EBUSY means someone already mounted it, which is a
 * success for our purposes. Anything else is worth knowing about but is not
 * worth dying for: a missing /sys costs us the battery readings, not the
 * frame loop. */
static void mount_or_warn(const char *src, const char *dst, const char *type)
{
    mkdir(dst, 0755);
    if (mount(src, dst, type, 0, NULL) == 0 || errno == EBUSY)
        return;
    fprintf(stderr, "pid351: mount %s on %s failed: %s\n",
            type, dst, strerror(errno));
}

int plat_boot_init(void)
{
    if (getpid() != 1)
        return 0;
    is_init = 1;

    mount_or_warn("devtmpfs", "/dev",  "devtmpfs");
    mount_or_warn("proc",     "/proc", "proc");
    mount_or_warn("sysfs",    "/sys",  "sysfs");

    /* /dev/kmsg rate limits a userspace writer to ten messages every five
     * seconds by default, and drops the rest on the floor without a word.
     * That silently truncated four rows out of the blit table and most of a
     * diagnostic census before anyone noticed, because the surviving lines
     * looked complete. Turn it off: this machine has exactly one process, it
     * is not going to flood anybody, and a log that lies by omission is worse
     * than no log. Also set on the command line, in case /proc is not there. */
    {
        int r = open("/proc/sys/kernel/printk_devkmsg", O_WRONLY | O_CLOEXEC);
        if (r >= 0) {
            if (write(r, "on\n", 3) < 0)
                fprintf(stderr, "pid351: could not unlimit /dev/kmsg\n");
            close(r);
        }
    }

    /* Our own output has nowhere to go: no shell is capturing stdout and the
     * serial port is not wired to anything reachable. Writing it into the
     * kernel ring buffer instead puts our lines in the same place, in the
     * same order and with the same timestamps as the kernel's own - so one
     * dump at exit saves both, and a driver that failed to probe sits right
     * next to the line where we noticed. It still reaches the panel through
     * fbcon until we take DRM master, which is exactly the window where a
     * failure needs to be readable off the screen. */
    int k = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (k >= 0) {
        dup2(k, STDOUT_FILENO);
        dup2(k, STDERR_FILENO);
        if (k > STDERR_FILENO)
            close(k);
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    /* The DRM device is not there yet either. rockchip-drm binds a panel over
     * DSI and that chain finishes its deferred probe after the initramfs is
     * already executing, so plat_init would open a node that does not exist. */
    if (wait_for_node(DRM_CARD, DRM_WAIT_MS) != 0)
        fprintf(stderr, "pid351: %s never appeared\n", DRM_CARD);

    return 1;
}

/* The boot partition is where a post-mortem has to land, since a panel that
 * never lit cannot show us anything. Mounted here rather than in
 * plat_boot_init because mmcblk0p1 does not exist yet at that point - the MMC
 * host is still probing the card we booted from. */
static int mount_boot(void)
{
    static int mounted;

    if (mounted)
        return 0;
    if (wait_for_node(BOOT_DEV, MMC_WAIT_MS) != 0) {
        fprintf(stderr, "pid351: %s never appeared\n", BOOT_DEV);
        return -1;
    }
    mkdir(BOOT_MOUNT, 0755);
    if (mount(BOOT_DEV, BOOT_MOUNT, "vfat", 0, NULL) != 0 && errno != EBUSY) {
        fprintf(stderr, "pid351: mount %s failed: %s\n",
                BOOT_DEV, strerror(errno));
        return -1;
    }
    mounted = 1;
    return 0;
}

const char *plat_boot_mount(void)
{
    if (!is_init)
        return NULL;
    return mount_boot() == 0 ? BOOT_MOUNT : NULL;
}

/* SYSLOG_ACTION_READ_ALL. The ring buffer is the only account of what the
 * kernel did before we existed, and with no serial port it is the only thing
 * that can explain a driver that failed to probe. */
int plat_boot_save_log(const char *name)
{
    enum { READ_ALL = 3, BUF = 256 * 1024 };
    char path[256];
    char *buf;
    long n;
    int fd;

    if (!is_init)
        return 0;
    if (mount_boot() != 0)
        return -1;

    buf = malloc(BUF);
    if (!buf)
        return -1;
    n = syscall(__NR_syslog, READ_ALL, buf, (long)BUF);
    if (n < 0) {
        free(buf);
        return -1;
    }

    snprintf(path, sizeof path, "%s/%s", BOOT_MOUNT, name);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(buf);
        return -1;
    }
    if (write(fd, buf, (size_t)n) < 0)
        n = -1;
    close(fd);
    free(buf);
    flush_disks();
    return n < 0 ? -1 : 0;
}

/* PID 1 returning from main is a kernel panic, so this never returns and the
 * caller is not given the option of continuing. */
void plat_boot_shutdown(int power_off)
{
    if (!is_init)
        return;

    flush_disks();
    umount(BOOT_MOUNT);
    flush_disks();
    syscall(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            power_off ? LINUX_REBOOT_CMD_POWER_OFF : LINUX_REBOOT_CMD_RESTART,
            (void *)0);

    /* Unreachable unless the reboot syscall itself failed, and a panic with a
     * message beats a silent hang. */
    for (;;)
        pause();
}

void plat_present(const px_t *fb, int w, int h, const px_t *bar)
{
    if (g.fd < 0)
        return;

    game_w = bar ? fit_panel(w, h, PANEL_W, PANEL_H).w : PANEL_W;

    /* Resampling before the blit rather than inside it. Fusing them would
     * save writing and re-reading 273 KB, but it would also put the filter in
     * four blit variants and in the SDL backend separately, which is exactly
     * the split that let the two backends disagree about scaling for weeks.
     * One implementation, measured, and optimised only if the number says so.
     *
     * After this the blit has nothing left to scale: build_maps reduces to
     * the identity on both axes and the blit is a pure rotation. */
    uint64_t s0 = plat_now_us();
    const px_t *img = scale_frame(fb, w, h, game_w);
    if (img) {
        fb = img;
        w  = game_w;
        h  = PANEL_H;
    }
    g.scale_us = (uint32_t)(plat_now_us() - s0);

    /* Bracketing with two clock reads costs a fraction of a microsecond
     * against a blit measured in hundreds, and main.c prints the calibration
     * so that claim is checked rather than asserted. */
    uint64_t t0 = plat_now_us();
    wait_flip();
    uint64_t t1 = plat_now_us();

    int back = g.front ^ 1;
    if (w * h <= BLIT_ROW_MAX_PX)
        run_variant(&g.buf[back], fb, w, h, PLAT_BLIT_STAGED_ROW,
                    BLIT_ROW_ROWS);
    else
        run_variant(&g.buf[back], fb, w, h, PLAT_BLIT_STAGED, BLIT_TILE);
    if (bar && game_w < PANEL_W)
        blit_bar(&g.buf[back], bar);
    uint64_t t2 = plat_now_us();

    g.wait_us = (uint32_t)(t1 - t0);
    g.blit_us = (uint32_t)(t2 - t1);

    struct drm_mode_crtc_page_flip flip;
    memset(&flip, 0, sizeof flip);
    flip.crtc_id = g.crtc_id;
    flip.fb_id   = g.buf[back].fb_id;
    flip.flags   = DRM_MODE_PAGE_FLIP_EVENT;
    if (xioctl(DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0) {
        fail("MODE_PAGE_FLIP");
        return;
    }
    g.flip_pending = 1;
    g.front = back;
}

uint32_t plat_input(void)
{
    pump_input();
    return g.buttons | g.hat_dirs | g.stick_dirs;
}

uint64_t plat_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void plat_sleep_until(uint64_t deadline_us)
{
    /* Usually a no-op on this backend: the page flip above already parked the
     * process until vblank, and the panel's 60.02 Hz is close enough to the
     * frame budget that the deadline has normally passed. It stays honest
     * rather than being removed, because per-console timing in phase 2 will
     * retune the panel clock and the two will stop agreeing. */
    struct timespec ts;
    ts.tv_sec  = (time_t)(deadline_us / 1000000u);
    ts.tv_nsec = (long)(deadline_us % 1000000u) * 1000;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

/* The signal only. This used to also latch on START+SELECT, from the weeks
 * when the pad meant nothing else and there had to be some way out of a test
 * pattern - and it latched permanently, so once the combo had been seen the
 * backend answered yes to every caller forever after.
 *
 * That outlived its purpose the moment START+SELECT became the way out of a
 * game. Leaving a game set the flag, the list asked this on its first frame,
 * and the machine powered off; no amount of edge detection above it could
 * help, because the answer had already been latched a layer down. The pad is
 * the caller's to interpret and this reports nothing about it. */
int plat_should_quit(void)
{
    return sig_quit;
}

int plat_axes(plat_axis_t *out, int max)
{
    int n = g.axis_count < max ? g.axis_count : max;
    for (int i = 0; i < n; i++)
        out[i] = g.axis[i];
    return n;
}

uint64_t plat_flip_us(void)
{
    return g.flip_us;
}

uint32_t plat_flip_seq(void)
{
    return g.flip_seq;
}

void plat_frame_us(uint32_t *blit_us, uint32_t *wait_us, uint32_t *scale_us)
{
    if (scale_us) *scale_us = g.scale_us;
    if (blit_us) *blit_us = g.blit_us;
    if (wait_us) *wait_us = g.wait_us;
}

int plat_bench(const px_t *src, int src_w, int src_h, int variant, int tile,
               uint32_t *samples, int n)
{
    if (g.fd < 0 || !src || !samples || n <= 0 || tile <= 0)
        return -1;

    /* The maps index the source by a ratio, so a source larger than the panel
     * would still be read in range - but nothing we target is, and accepting
     * one here would silently measure a case that cannot occur. */
    if (src_w <= 0 || src_h <= 0 || src_w > PANEL_W || src_h > PANEL_H)
        return -1;

    /* The back buffer. Benchmarking into the one being scanned out would put
     * tearing on the panel and add the display controller's read traffic to
     * what we are trying to measure. */
    struct dumb_buf *b = &g.buf[g.front ^ 1];

    for (int i = 0; i < n; i++) {
        uint64_t t0 = plat_now_us();
        run_variant(b, src, src_w, src_h, variant, tile);
        samples[i] = (uint32_t)(plat_now_us() - t0);
    }
    return 0;
}

int plat_blit_verify(const px_t *src, int src_w, int src_h,
                     int variant, int tile)
{
    static px_t ref[PHYS_W * PHYS_H];

    if (g.fd < 0 || !src || tile <= 0)
        return -1;
    if (src_w <= 0 || src_h <= 0 || src_w > PANEL_W || src_h > PANEL_H)
        return -1;

    struct dumb_buf *b = &g.buf[g.front ^ 1];
    int stride = (int)(b->pitch / sizeof(px_t));

    run_variant(b, src, src_w, src_h, PLAT_BLIT_STRIDED, BLIT_TILE);
    for (int py = 0; py < PHYS_H; py++)
        memcpy(ref + (size_t)py * (size_t)PHYS_W,
               b->px + (size_t)py * (size_t)stride,
               (size_t)PHYS_W * sizeof(px_t));

    run_variant(b, src, src_w, src_h, variant, tile);

    int bad = 0;
    for (int py = 0; py < PHYS_H; py++)
        for (int px = 0; px < PHYS_W; px++)
            if (b->px[(size_t)py * (size_t)stride + (size_t)px]
                != ref[(size_t)py * (size_t)PHYS_W + (size_t)px])
                bad++;
    return bad;
}

/* ------------------------------------------------------------- probes */

static volatile uint32_t mem_sink;

static uint32_t time_seq(px_t *p, int stride, int iters, int mode)
{
    uint32_t best = UINT32_MAX;

    for (int i = 0; i < iters; i++) {
        uint64_t t0 = plat_now_us();
        uint32_t acc = 0;

        for (int y = 0; y < PHYS_H; y++) {
            px_t *row = p + (size_t)y * (size_t)stride;
            switch (mode) {
            case 0:                                    /* write */
                for (int x = 0; x < PHYS_W; x++)
                    row[x] = (px_t)x;
                break;
            case 1:                                    /* read */
                for (int x = 0; x < PHYS_W; x++)
                    acc += row[x];
                break;
            default:                                   /* read-modify-write */
                for (int x = 0; x < PHYS_W; x++)
                    row[x] = (px_t)(row[x] + 1u);
                break;
            }
        }

        mem_sink = acc;   /* or the read loop is legal to delete entirely */
        uint32_t d = (uint32_t)(plat_now_us() - t0);
        if (d < best)
            best = d;
    }
    return best;
}

int plat_mem_probe(plat_mem_t *out, int iters)
{
    static px_t ram[PHYS_W * PHYS_H];

    if (g.fd < 0 || !out || iters <= 0)
        return -1;

    struct dumb_buf *b = &g.buf[g.front ^ 1];
    int stride = (int)(b->pitch / sizeof(px_t));

    out->fb_write  = time_seq(b->px, stride, iters, 0);
    out->fb_read   = time_seq(b->px, stride, iters, 1);
    out->fb_rmw    = time_seq(b->px, stride, iters, 2);
    out->ram_write = time_seq(ram, PHYS_W, iters, 0);
    out->ram_read  = time_seq(ram, PHYS_W, iters, 1);
    out->ram_rmw   = time_seq(ram, PHYS_W, iters, 2);
    return 0;
}

void plat_mode_timing(uint32_t *exact_mhz, uint32_t *clock_khz,
                      uint32_t *htotal, uint32_t *vtotal)
{
    uint32_t ht = g.mode.htotal, vt = g.mode.vtotal, ck = g.mode.clock;

    if (clock_khz) *clock_khz = ck;
    if (htotal)    *htotal    = ht;
    if (vtotal)    *vtotal    = vt;
    if (exact_mhz)
        *exact_mhz = (ht && vt)
                   ? (uint32_t)((uint64_t)ck * 1000000u / ((uint64_t)ht * vt))
                   : 0;
}

int plat_vblank_probe(int flips, uint32_t *measured_mhz)
{
    if (g.fd < 0 || flips <= 0 || !measured_mhz)
        return -1;

    /* One flip up front so the first interval below starts from a vblank
     * rather than from wherever in the frame we happened to be called. */
    wait_flip();

    uint64_t t0 = plat_now_us();
    for (int i = 0; i < flips; i++) {
        int back = g.front ^ 1;
        struct drm_mode_crtc_page_flip flip;
        memset(&flip, 0, sizeof flip);
        flip.crtc_id = g.crtc_id;
        flip.fb_id   = g.buf[back].fb_id;
        flip.flags   = DRM_MODE_PAGE_FLIP_EVENT;
        if (xioctl(DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0)
            return -1;
        g.flip_pending = 1;
        g.front = back;
        wait_flip();
    }
    uint64_t dt = plat_now_us() - t0;

    *measured_mhz = dt ? (uint32_t)((uint64_t)flips * 1000000000u / dt) : 0;
    return 0;
}

/* ------------------------------------------------- display property dump */

static void print_fourcc(uint32_t f)
{
    printf("%c%c%c%c", (char)(f & 0xff), (char)((f >> 8) & 0xff),
           (char)((f >> 16) & 0xff), (char)((f >> 24) & 0xff));
}

/* Names and permitted values for every property on one object. The names are
 * the point: "rotation" appearing on a plane, with 90 among its enum values,
 * would mean the hardware can do the transform we have been doing by hand. */
static void dump_object_props(uint32_t id, uint32_t type, const char *label)
{
    struct drm_mode_obj_get_properties op;
    uint32_t props[64];
    uint64_t vals[64];

    memset(&op, 0, sizeof op);
    op.obj_id   = id;
    op.obj_type = type;
    if (xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0)
        return;
    if (op.count_props > 64)
        op.count_props = 64;
    op.props_ptr        = (uint64_t)(uintptr_t)props;
    op.prop_values_ptr  = (uint64_t)(uintptr_t)vals;
    if (xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0)
        return;

    for (uint32_t i = 0; i < op.count_props; i++) {
        struct drm_mode_get_property gp;
        struct drm_mode_property_enum en[32];
        uint64_t pv[32];

        memset(&gp, 0, sizeof gp);
        gp.prop_id = props[i];
        if (xioctl(DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0)
            continue;
        if (gp.count_values > 32)     gp.count_values = 32;
        if (gp.count_enum_blobs > 32) gp.count_enum_blobs = 32;
        gp.values_ptr    = (uint64_t)(uintptr_t)pv;
        gp.enum_blob_ptr = (uint64_t)(uintptr_t)en;
        if (xioctl(DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0)
            continue;

        printf("pid351:     %s prop %-22s = %llu", label, gp.name,
               (unsigned long long)vals[i]);
        for (uint32_t e = 0; e < gp.count_enum_blobs; e++)
            printf("  [%llu=%s]", (unsigned long long)en[e].value, en[e].name);
        printf("\n");
    }
}

void plat_dump_props(void)
{
    if (g.fd < 0)
        return;

    /* Without this only overlay planes are visible, and the primary plane -
     * the one that would have to carry a rotation - is hidden. */
    struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
    if (ioctl(g.fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0)
        printf("pid351:   universal planes unavailable\n");

    printf("pid351:   connector %u:\n", g.conn_id);
    dump_object_props(g.conn_id, DRM_MODE_OBJECT_CONNECTOR, "conn");
    printf("pid351:   crtc %u:\n", g.crtc_id);
    dump_object_props(g.crtc_id, DRM_MODE_OBJECT_CRTC, "crtc");

    struct drm_mode_get_plane_res pres;
    uint32_t planes[32];
    memset(&pres, 0, sizeof pres);
    if (xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0) {
        printf("pid351:   no plane resources\n");
        return;
    }
    if (pres.count_planes > 32)
        pres.count_planes = 32;
    pres.plane_id_ptr = (uint64_t)(uintptr_t)planes;
    if (xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0)
        return;

    for (uint32_t i = 0; i < pres.count_planes; i++) {
        struct drm_mode_get_plane gp;
        uint32_t fmts[64];

        memset(&gp, 0, sizeof gp);
        gp.plane_id = planes[i];
        if (xioctl(DRM_IOCTL_MODE_GETPLANE, &gp) < 0)
            continue;
        if (gp.count_format_types > 64)
            gp.count_format_types = 64;
        gp.format_type_ptr = (uint64_t)(uintptr_t)fmts;
        if (xioctl(DRM_IOCTL_MODE_GETPLANE, &gp) < 0)
            continue;

        printf("pid351:   plane %u (crtc %u, possible_crtcs 0x%x) formats:",
               gp.plane_id, gp.crtc_id, gp.possible_crtcs);
        for (uint32_t f = 0; f < gp.count_format_types; f++) {
            printf(" ");
            print_fourcc(fmts[f]);
        }
        printf("\n");
        dump_object_props(gp.plane_id, DRM_MODE_OBJECT_PLANE, "plane");
    }
    fflush(stdout);
}
