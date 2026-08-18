/* pid351 - RG351P backend: KMS, evdev, and the rotation the panel demands
 *
 * Talks to the kernel through raw ioctls on the DRM uapi headers rather than
 * linking libdrm. libdrm would add a shared library to a binary whose whole
 * premise is that it depends on nothing; <drm/drm_mode.h> is a kernel uapi
 * header in exactly the same sense as <linux/input.h>, so using it costs
 * nothing at link time.
 *
 * The panel is natively portrait 320x480 and advertises no other mode, so
 * every frame has to be rotated 90 degrees. That rotation belongs in the RGA,
 * which is idle silicon sitting on the die - but ROCKNIX ships no device tree
 * node for it, so there is nothing to open yet. Until there is, the CPU does
 * it, structured so the rotate and the integer scale happen in a single pass
 * with contiguous writes. See docs/hardware.md.
 *
 * Deliberately not here yet:
 *   - RGA. Needs a device tree overlay first; the CPU path is the fallback it
 *     will hide behind, which is why the fallback exists at all.
 *   - ALSA. The platform interface has no audio entry point yet.
 *   - Suspend, backlight and governor control. Phase 3.
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/input.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

#define NBUF 2

struct dumb_buf {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    uint64_t size;
    px_t    *px;
};

static struct {
    int      fd;
    uint32_t crtc_id;
    uint32_t conn_id;
    struct drm_mode_modeinfo mode;
    struct dumb_buf buf[NBUF];
    int      front;          /* buffer currently being scanned out */
    int      flip_pending;
    int      had_master;

    int      pad_fd;
    uint32_t buttons;
    int      quit;

    plat_axis_t axis[PLAT_AXIS_MAX];
    uint16_t    axis_code[PLAT_AXIS_MAX];
    int         axis_count;
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

static void fail(const char *what)
{
    fprintf(stderr, "pid351: %s: %s\n", what, strerror(errno));
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
static int open_pad(void)
{
    DIR *d = opendir("/dev/input");
    if (!d) {
        fail("opendir /dev/input");
        return -1;
    }

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

    if (found < 0)
        fprintf(stderr, "pid351: no pad matching %04x:%04x with abs axes\n",
                PAD_VENDOR, PAD_PRODUCT);
    return found;
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
                g.buttons &= ~(uint32_t)(PAD_LEFT | PAD_RIGHT);
                if (ev.value < 0) g.buttons |= PAD_LEFT;
                if (ev.value > 0) g.buttons |= PAD_RIGHT;
            } else if (ev.code == ABS_HAT0Y) {
                g.buttons &= ~(uint32_t)(PAD_UP | PAD_DOWN);
                if (ev.value < 0) g.buttons |= PAD_UP;
                if (ev.value > 0) g.buttons |= PAD_DOWN;
            } else {
                for (int i = 0; i < g.axis_count; i++)
                    if (g.axis_code[i] == ev.code) {
                        g.axis[i].value = ev.value;
                        break;
                    }
            }
        }
    }

    /* No window to close and no keyboard, so the pad has to carry the exit.
     * START+SELECT together is unreachable by accident. */
    if ((g.buttons & (PAD_START | PAD_SELECT)) == (PAD_START | PAD_SELECT))
        g.quit = 1;
}

/* ----------------------------------------------------------- rotate+scale */

/* One pass, writing whole destination scanlines in order. A destination
 * scanline is a column of the landscape image, so the source reads stride
 * across a framebuffer small enough to sit in cache while the writes - the
 * expensive half on this memory bus - stay linear. */
static void blit_rotated(struct dumb_buf *b, const px_t *src, int w, int h)
{
    rect_t r = fit_integer(w, h, PANEL_W, PANEL_H);
    int scale = r.w / w;
    int stride = (int)(b->pitch / sizeof(px_t));

    /* Destination x maps to a fixed logical y, so resolve it once per frame
     * rather than once per pixel. */
    static int map_sy[PHYS_W];
    for (int px = 0; px < PHYS_W; px++) {
#if ROTATE_CW
        int ly = PANEL_H - 1 - px;
#else
        int ly = px;
#endif
        map_sy[px] = (ly >= r.y && ly < r.y + r.h) ? (ly - r.y) / scale : -1;
    }

    for (int py = 0; py < PHYS_H; py++) {
#if ROTATE_CW
        int lx = py;
#else
        int lx = PANEL_W - 1 - py;
#endif
        px_t *dst = b->px + (size_t)py * (size_t)stride;

        if (lx < r.x || lx >= r.x + r.w) {
            memset(dst, 0, (size_t)PHYS_W * sizeof(px_t));
            continue;
        }
        const px_t *col = src + (lx - r.x) / scale;

        for (int px = 0; px < PHYS_W; px++) {
            int sy = map_sy[px];
            dst[px] = (sy < 0) ? (px_t)0 : col[(size_t)sy * (size_t)w];
        }
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
        if (n >= (ssize_t)sizeof(struct drm_event))
            break;
        if (n < 0 && errno != EINTR) {
            fail("read drm event");
            break;
        }
    }
    g.flip_pending = 0;
}

void plat_present(const px_t *fb, int w, int h)
{
    if (g.fd < 0)
        return;

    wait_flip();

    int back = g.front ^ 1;
    blit_rotated(&g.buf[back], fb, w, h);

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
    return g.buttons;
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

int plat_should_quit(void)
{
    return g.quit;
}

int plat_axes(plat_axis_t *out, int max)
{
    int n = g.axis_count < max ? g.axis_count : max;
    for (int i = 0; i < n; i++)
        out[i] = g.axis[i];
    return n;
}
