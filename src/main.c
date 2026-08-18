/* pid351 - hardware demo
 *
 * Draws what the machine says about itself, and lights up every control as it
 * is pressed. Two jobs: prove the platform layer end to end on real hardware,
 * and make a fault visible from across the room rather than in a log file
 * after the fact.
 *
 * Renders at panel resolution rather than a core's, so the integer scaler
 * runs at 1x and the only transform between here and the glass is the
 * rotation. If something looks wrong here, it is the rotation or the mode.
 *
 * The analog axes are shown raw, with the kernel's own generic names, because
 * which physical stick drives which axis is not written down anywhere. That
 * is the one thing on screen that is a question rather than a report.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pid351.h"
#include "platform.h"
#include "gfx.h"
#include "scale.h"

#define FRAME_US 16743          /* 59.727 Hz, the GBA's real rate */

#define C_BG     RGB565(  8, 10, 14)
#define C_PANEL  RGB565( 20, 24, 32)
#define C_EDGE   RGB565( 46, 56, 72)
#define C_TEXT   RGB565(198,208,220)
#define C_DIM    RGB565(104,116,132)
#define C_ACCENT RGB565( 54,200,170)
#define C_LIT    RGB565(255,186, 40)
#define C_WARN   RGB565(232, 76, 62)

static px_t framebuffer[PANEL_W * PANEL_H];

/* ------------------------------------------------------------- timing */

/* Why any of this exists: we were about to move the rotate blit onto the RGA
 * without having measured what the blit costs. These numbers decide whether
 * that work is worth doing, so they are gathered honestly - a distribution
 * rather than an average, and a sweep over every console's native size rather
 * than the one size this demo happens to run at. */

#define TIME_RING 256           /* about 4 seconds of history at 60 Hz */

typedef struct { uint32_t min, med, max; } stat_t;

static uint32_t blit_ring[TIME_RING];
static uint32_t wait_ring[TIME_RING];
static uint32_t draw_ring[TIME_RING];
static int      ring_n, ring_i;

/* Cached, because sorting the ring is far too expensive to do inside the
 * frame we are trying to measure. Refreshed once a second alongside the fps
 * counter. */
static stat_t blit_stat, wait_stat, draw_stat;

/* Over the whole run rather than the window, since the smallest blit we ever
 * saw is the closest we get to the cost with nothing else interfering. */
static uint32_t blit_min_life = UINT32_MAX;
static uint32_t blit_max_life;

static void sort_u32(uint32_t *a, int n)
{
    for (int i = 1; i < n; i++) {
        uint32_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

/* Min, median and max - not a mean. The scheduler can only ever add time to a
 * sample, so the mean of a preempted run measures the rest of the system as
 * much as it measures us. The minimum is the real cost, the median is what
 * typically happens, and the gap between median and max is the interference. */
static stat_t stat_of(const uint32_t *src, int n)
{
    static uint32_t tmp[TIME_RING];
    stat_t s = { 0, 0, 0 };

    if (n <= 0)
        return s;
    if (n > TIME_RING)
        n = TIME_RING;

    memcpy(tmp, src, (size_t)n * sizeof tmp[0]);
    sort_u32(tmp, n);
    s.min = tmp[0];
    s.med = tmp[n / 2];
    s.max = tmp[n - 1];
    return s;
}

/* Every measurement below is bracketed by two clock reads, so the bracket
 * itself has to be shown to be negligible rather than assumed to be. This
 * also answers whether clock_gettime is being served from the vDSO or is
 * trapping into the kernel, which is an order of magnitude either way and
 * is not obvious in a statically linked binary. */
static void clock_calibrate(void)
{
    enum { N = 20000 };
    static volatile uint64_t sink;   /* or the loop is legal to delete */

    uint64_t t0 = plat_now_us();
    for (int i = 0; i < N; i++)
        sink = plat_now_us();
    uint64_t t1 = plat_now_us();
    (void)sink;

    printf("pid351: clock %lu ns per plat_now_us() over %d calls "
           "(vDSO if well under 1000 ns, a syscall trap if not); "
           "plat_now_us resolution is 1 us by construction\n",
           (unsigned long)((t1 - t0) * 1000u / (uint64_t)N), N);
}

/* The blit's cost is dominated by whether the source fits in cache, and every
 * console has a different native size - so the number this demo produces at
 * 480x320 is the worst case and applies to nothing we will actually run.
 * Sweep the real sizes instead. */
static void bench_blit(void)
{
    static const struct { const char *name; int w, h; } sizes[] = {
        { "GB/GBC",  160, 144 },
        { "GBA",     240, 160 },
        { "NES",     256, 240 },
        { "SNES",    256, 224 },
        { "GENESIS", 320, 224 },
        { "NATIVE",  480, 320 },
    };
    enum { ITER = 200 };
    static uint32_t samples[ITER];

    /* Real data rather than a cleared buffer, so nothing downstream can be
     * getting away with a shortcut on uniform bytes. */
    for (int i = 0; i < PANEL_W * PANEL_H; i++)
        framebuffer[i] = (px_t)(i ^ (i >> 5));

    printf("pid351: blit bench, %d iterations per size, into the real back "
           "buffer, no page flip\n", ITER);

    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        if (plat_bench(framebuffer, sizes[k].w, sizes[k].h,
                       samples, ITER) < 0) {
            printf("pid351:   unavailable on this backend\n");
            fflush(stdout);
            return;
        }

        stat_t s = stat_of(samples, ITER);
        rect_t r = fit_integer(sizes[k].w, sizes[k].h, PANEL_W, PANEL_H);
        unsigned pct = s.min * 1000u / (unsigned)FRAME_US;

        printf("pid351:   %-8s %3dx%-3d x%d  src %4uKB  "
               "min %5u  med %5u  max %6u us   %u.%u%% of a %d us frame\n",
               sizes[k].name, sizes[k].w, sizes[k].h, r.w / sizes[k].w,
               (unsigned)((size_t)sizes[k].w * (size_t)sizes[k].h
                          * sizeof(px_t) / 1024),
               s.min, s.med, s.max, pct / 10u, pct % 10u, FRAME_US);
    }

    if (plat_bench_linear(framebuffer, samples, ITER) == 0) {
        stat_t s = stat_of(samples, ITER);
        unsigned pct = s.min * 1000u / (unsigned)FRAME_US;
        printf("pid351:   %-8s %3dx%-3d x1  src %4uKB  "
               "min %5u  med %5u  max %6u us   %u.%u%% of a %d us frame"
               "   <- control: same writes, sequential reads\n",
               "LINEAR", PANEL_W, PANEL_H,
               (unsigned)(sizeof framebuffer / 1024),
               s.min, s.med, s.max, pct / 10u, pct % 10u, FRAME_US);
    }

    /* Flushed here rather than at the first report: if the 300 second kill
     * lands before then, this is the half of the output that cannot be
     * gathered any other way. */
    fflush(stdout);
}

/* ------------------------------------------------------------- system info */

/* Everything here is a small text file that may not exist - on the laptop
 * almost none of it does. A missing file is not an error, it is a dash. */
static int read_text(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t got = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got] = '\0';

    /* Device tree strings are NUL terminated inside the file; /sys values
     * carry a trailing newline. Both stop the string here. */
    for (size_t i = 0; i < got; i++)
        if (buf[i] == '\n' || buf[i] == '\0') {
            buf[i] = '\0';
            break;
        }
    return 1;
}

static long read_long(const char *path, long missing)
{
    char buf[64];
    if (!read_text(path, buf, sizeof buf))
        return missing;
    return strtol(buf, NULL, 10);
}

/* The first of these that exists wins, so the same binary says something
 * sensible on a laptop and on the handheld. */
static long read_long_any(const char *const *paths, long missing)
{
    for (int i = 0; paths[i]; i++) {
        long v = read_long(paths[i], missing);
        if (v != missing)
            return v;
    }
    return missing;
}

struct sysinfo_s {
    char model[64];
    char kernel[64];
    char governor[24];
    long cpu_khz;
    long gpu_hz;
    long temp_mc;
    long temp_gpu_mc;
    long capacity;
    long voltage_uv;
    long current_ua;
    long backlight, backlight_max;
    long mem_total_kb, mem_avail_kb;
    long uptime_s;
};

static void sysinfo_read(struct sysinfo_s *s)
{
    static const char *const volt[] = {
        "/sys/class/power_supply/battery/voltage_avg",
        "/sys/class/power_supply/battery/voltage_now",
        "/sys/class/power_supply/BAT0/voltage_now", NULL };
    static const char *const curr[] = {
        "/sys/class/power_supply/battery/current_avg",
        "/sys/class/power_supply/battery/current_now",
        "/sys/class/power_supply/BAT0/current_now", NULL };
    static const char *const cap[] = {
        "/sys/class/power_supply/battery/capacity",
        "/sys/class/power_supply/BAT0/capacity", NULL };

    if (!read_text("/proc/device-tree/model", s->model, sizeof s->model))
        snprintf(s->model, sizeof s->model, "-");
    if (!read_text("/proc/sys/kernel/osrelease", s->kernel, sizeof s->kernel))
        snprintf(s->kernel, sizeof s->kernel, "-");
    if (!read_text("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
                   s->governor, sizeof s->governor))
        snprintf(s->governor, sizeof s->governor, "-");

    s->cpu_khz = read_long(
        "/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", -1);
    s->gpu_hz  = read_long("/sys/class/devfreq/ff400000.gpu/cur_freq", -1);
    s->temp_mc = read_long("/sys/class/thermal/thermal_zone0/temp", -1);
    s->temp_gpu_mc = read_long("/sys/class/thermal/thermal_zone1/temp", -1);
    s->capacity   = read_long_any(cap,  -1);
    s->voltage_uv = read_long_any(volt, -1);
    s->current_ua = read_long_any(curr, -1);
    s->backlight     = read_long("/sys/class/backlight/backlight/brightness", -1);
    s->backlight_max = read_long("/sys/class/backlight/backlight/max_brightness", -1);

    char buf[4096];
    s->mem_total_kb = s->mem_avail_kb = -1;
    if (read_text("/proc/uptime", buf, sizeof buf))
        s->uptime_s = strtol(buf, NULL, 10);
    else
        s->uptime_s = -1;

    FILE *f = fopen("/proc/meminfo", "rb");
    if (f) {
        while (fgets(buf, sizeof buf, f)) {
            if (!strncmp(buf, "MemTotal:", 9))
                s->mem_total_kb = strtol(buf + 9, NULL, 10);
            else if (!strncmp(buf, "MemAvailable:", 13))
                s->mem_avail_kb = strtol(buf + 13, NULL, 10);
        }
        fclose(f);
    }
}

/* ------------------------------------------------------------- info column */

/* One greppable line per interval, with the raw sysfs integers rather than
 * pretty units - unit conversion in a log is just somewhere for a bug to hide,
 * and every sample has to carry the conditions it was taken under or it is not
 * a measurement.
 *
 * Flushed explicitly: stdout here is a file on the SD card, so it is block
 * buffered, and first-light.sh kills us with SIGTERM after 300 seconds. An
 * unflushed buffer would mean the run that told us the most produced nothing. */
static void conditions(const char *when, const struct sysinfo_s *s)
{
    printf("pid351: %s cpu_khz=%ld gov=%s gpu_hz=%ld temp_mc=%ld "
           "volt_uv=%ld curr_ua=%ld backlight=%ld\n",
           when, s->cpu_khz, s->governor, s->gpu_hz, s->temp_mc,
           s->voltage_uv, s->current_ua, s->backlight);
    fflush(stdout);
}

static void report(uint64_t elapsed_us, const struct sysinfo_s *s,
                   double fps, unsigned frames)
{
    printf("pid351: t=%llus frames=%u fps=%.2f n=%d "
           "blit=%u/%u/%u life=%u/%u draw=%u/%u/%u wait=%u/%u/%u "
           "cpu_khz=%ld gov=%s gpu_hz=%ld temp_mc=%ld "
           "volt_uv=%ld curr_ua=%ld backlight=%ld\n",
           (unsigned long long)(elapsed_us / 1000000u), frames, fps, ring_n,
           blit_stat.min, blit_stat.med, blit_stat.max,
           blit_min_life == UINT32_MAX ? 0u : blit_min_life, blit_max_life,
           draw_stat.min, draw_stat.med, draw_stat.max,
           wait_stat.min, wait_stat.med, wait_stat.max,
           s->cpu_khz, s->governor, s->gpu_hz, s->temp_mc,
           s->voltage_uv, s->current_ua, s->backlight);
    fflush(stdout);
}

static void info_line(canvas_t *c, int x, int y, const char *label,
                      const char *value, px_t value_col)
{
    gfx_text(c, x, y, label, 1, C_DIM);
    gfx_text(c, x + 66, y, value, 1, value_col);
}

static void draw_info(canvas_t *c, int x, int y, const struct sysinfo_s *s,
                      double fps, unsigned frames)
{
    char v[64];
    int lh = 11;

    gfx_text(c, x, y, "SYSTEM", 1, C_ACCENT);
    gfx_rect(c, x, y + 9, 216, 1, C_EDGE);
    y += 16;

    info_line(c, x, y, "MODEL", s->model, C_TEXT);                 y += lh;
    info_line(c, x, y, "KERNEL", s->kernel, C_TEXT);               y += lh;

    snprintf(v, sizeof v, "%dX%d  1X  ROT", PANEL_W, PANEL_H);
    info_line(c, x, y, "PANEL", v, C_TEXT);                        y += lh;

    if (s->cpu_khz > 0)
        snprintf(v, sizeof v, "%ld MHZ  %s", s->cpu_khz / 1000, s->governor);
    else
        snprintf(v, sizeof v, "-");
    info_line(c, x, y, "CPU", v, C_TEXT);                          y += lh;

    if (s->temp_mc > 0)
        snprintf(v, sizeof v, "%ld.%ld C", s->temp_mc / 1000,
                 (s->temp_mc % 1000) / 100);
    else
        snprintf(v, sizeof v, "-");
    info_line(c, x, y, "TEMP", v,
              s->temp_mc > 70000 ? C_WARN : C_TEXT);               y += lh;

    if (s->gpu_hz > 0)
        snprintf(v, sizeof v, "%ld MHZ", s->gpu_hz / 1000000);
    else
        snprintf(v, sizeof v, "-");
    info_line(c, x, y, "GPU", v, C_TEXT);                          y += lh;

    if (s->temp_gpu_mc > 0)
        snprintf(v, sizeof v, "%ld.%ld C", s->temp_gpu_mc / 1000,
                 (s->temp_gpu_mc % 1000) / 100);
    else
        snprintf(v, sizeof v, "-");
    info_line(c, x, y, "GPU TEMP", v,
              s->temp_gpu_mc > 70000 ? C_WARN : C_TEXT);           y += lh;

    y += 6;
    gfx_text(c, x, y, "POWER", 1, C_ACCENT);
    gfx_rect(c, x, y + 9, 216, 1, C_EDGE);
    y += 16;

    if (s->capacity >= 0 && s->voltage_uv > 0)
        snprintf(v, sizeof v, "%ld%%   %ld.%02ld V", s->capacity,
                 s->voltage_uv / 1000000, (s->voltage_uv / 10000) % 100);
    else
        snprintf(v, sizeof v, "-");
    info_line(c, x, y, "BATTERY", v,
              (s->capacity >= 0 && s->capacity < 15) ? C_WARN : C_TEXT);
    y += lh;

    /* Negative current means discharging. Watts is the number that actually
     * matters, and it is why this demo exists at panel resolution: it is the
     * baseline every later power change gets measured against. */
    if (s->current_ua != -1 && s->voltage_uv > 0) {
        long ma = s->current_ua / 1000;
        if (ma < 0) ma = -ma;
        long mw = ma * (s->voltage_uv / 1000) / 1000;
        snprintf(v, sizeof v, "%ld MA   %ld.%02ld W",
                 ma, mw / 1000, (mw / 10) % 100);
    } else {
        snprintf(v, sizeof v, "-");
    }
    info_line(c, x, y, "DRAW", v, C_TEXT);                         y += lh;

    if (s->backlight >= 0)
        snprintf(v, sizeof v, "%ld / %ld", s->backlight, s->backlight_max);
    else
        snprintf(v, sizeof v, "-");
    info_line(c, x, y, "BACKLIGHT", v, C_TEXT);                    y += lh;

    if (s->mem_total_kb > 0 && s->mem_avail_kb > 0)
        snprintf(v, sizeof v, "%ld / %ld MB",
                 (s->mem_total_kb - s->mem_avail_kb) / 1024,
                 s->mem_total_kb / 1024);
    else
        snprintf(v, sizeof v, "-");
    info_line(c, x, y, "MEMORY", v, C_TEXT);                       y += lh;

    y += 6;
    gfx_text(c, x, y, "RUNTIME", 1, C_ACCENT);
    gfx_rect(c, x, y + 9, 216, 1, C_EDGE);
    y += 16;

    snprintf(v, sizeof v, "%d.%02d", (int)fps, (int)(fps * 100) % 100);
    info_line(c, x, y, "FPS", v, fps > 55.0 ? C_TEXT : C_WARN);    y += lh;

    snprintf(v, sizeof v, "%u", frames);
    info_line(c, x, y, "FRAMES", v, C_TEXT);                       y += lh;

    snprintf(v, sizeof v, "%u/%u/%u", blit_stat.min, blit_stat.med,
             blit_stat.max);
    info_line(c, x, y, "BLIT US", v, C_TEXT);                      y += lh;

    if (s->uptime_s >= 0)
        snprintf(v, sizeof v, "%ld:%02ld:%02ld", s->uptime_s / 3600,
                 (s->uptime_s / 60) % 60, s->uptime_s % 60);
    else
        snprintf(v, sizeof v, "-");
    info_line(c, x, y, "UPTIME", v, C_TEXT);
}

/* --------------------------------------------------------------- the pad */

static void key_cap(canvas_t *c, int x, int y, int w, int h,
                    const char *label, int lit)
{
    gfx_rect(c, x, y, w, h, lit ? C_LIT : C_PANEL);
    gfx_frame(c, x, y, w, h, lit ? C_LIT : C_EDGE);
    int tw = gfx_text_w(label, 1);
    gfx_text(c, x + (w - tw) / 2, y + (h - FONT_H) / 2, label, 1,
             lit ? C_BG : C_DIM);
}

static void round_key(canvas_t *c, int cx, int cy, int r,
                      const char *label, int lit)
{
    gfx_disc(c, cx, cy, r, lit ? C_LIT : C_PANEL);
    gfx_disc(c, cx, cy, r, lit ? C_LIT : C_PANEL);
    /* A ring, drawn as the difference of two discs. */
    if (!lit) {
        gfx_disc(c, cx, cy, r, C_EDGE);
        gfx_disc(c, cx, cy, r - 1, C_PANEL);
    }
    int tw = gfx_text_w(label, 1);
    gfx_text(c, cx - tw / 2, cy - FONT_H / 2, label, 1, lit ? C_BG : C_DIM);
}

static void draw_pad(canvas_t *c, int x, int y, uint32_t b)
{
    gfx_text(c, x, y, "CONTROLS", 1, C_ACCENT);
    gfx_rect(c, x, y + 9, 232, 1, C_EDGE);
    y += 18;

    /* Shoulders, laid out as they sit on the shell: 1 above 2. */
    key_cap(c, x + 4,   y,      46, 13, "L1", b & PAD_L1);
    key_cap(c, x + 4,   y + 16, 46, 13, "L2", b & PAD_L2);
    key_cap(c, x + 182, y,      46, 13, "R1", b & PAD_R1);
    key_cap(c, x + 182, y + 16, 46, 13, "R2", b & PAD_R2);

    /* D-pad, each arm lighting on its own so a stuck direction is obvious. */
    int dx = x + 46, dy = y + 70;
    gfx_rect(c, dx - 9, dy - 9, 18, 18, C_PANEL);
    key_cap(c, dx - 9,  dy - 30, 18, 21, "",  b & PAD_UP);
    key_cap(c, dx - 9,  dy + 9,  18, 21, "",  b & PAD_DOWN);
    key_cap(c, dx - 30, dy - 9,  21, 18, "",  b & PAD_LEFT);
    key_cap(c, dx + 9,  dy - 9,  21, 18, "",  b & PAD_RIGHT);

    /* Face buttons in the shell's own diamond: X top, Y left, A right,
     * B bottom. Not the Nintendo arrangement, and not the kernel's names. */
    int fx = x + 186, fy = y + 70;
    round_key(c, fx,      fy - 26, 11, "X", b & PAD_X);
    round_key(c, fx - 26, fy,      11, "Y", b & PAD_Y);
    round_key(c, fx + 26, fy,      11, "A", b & PAD_A);
    round_key(c, fx,      fy + 26, 11, "B", b & PAD_B);

    key_cap(c, x + 74,  y + 118, 40, 12, "SEL",  b & PAD_SELECT);
    key_cap(c, x + 122, y + 118, 40, 12, "STRT", b & PAD_START);

    round_key(c, x + 46,  y + 124, 13, "L3", b & PAD_L3);
    round_key(c, x + 186, y + 124, 13, "R3", b & PAD_R3);
}

static void draw_axes(canvas_t *c, int x, int y)
{
    plat_axis_t ax[PLAT_AXIS_MAX];
    int n = plat_axes(ax, PLAT_AXIS_MAX);

    gfx_text(c, x, y, "ANALOG", 1, C_ACCENT);
    gfx_rect(c, x, y + 9, 232, 1, C_EDGE);
    y += 16;

    if (n == 0) {
        gfx_text(c, x, y, "NONE REPORTED", 1, C_DIM);
        return;
    }

    for (int i = 0; i < n; i++) {
        int bx = x + 34, bw = 150, bh = 7;
        gfx_text(c, x, y, ax[i].name, 1, C_DIM);
        gfx_frame(c, bx, y, bw, bh, C_EDGE);

        int span = ax[i].max - ax[i].min;
        if (span > 0) {
            int v = ax[i].value - ax[i].min;
            if (v < 0) v = 0;
            if (v > span) v = span;
            int mid = bx + bw / 2;
            int pos = bx + 1 + (int)((long)v * (bw - 2) / span);
            /* Drawn from the centre, so a centred stick is visibly centred
             * and drift shows as a bar that never quite closes. */
            if (pos >= mid)
                gfx_rect(c, mid, y + 1, pos - mid, bh - 2, C_LIT);
            else
                gfx_rect(c, pos, y + 1, mid - pos, bh - 2, C_LIT);
            gfx_rect(c, mid, y, 1, bh, C_ACCENT);
        }

        char v[16];
        snprintf(v, sizeof v, "%d", ax[i].value);
        gfx_text(c, bx + bw + 6, y, v, 1, C_TEXT);
        y += 11;
    }
}

/* A gradient and a grey ramp. RGB565 gives 32 levels of red and blue and 64
 * of green, so the ramp is meant to band slightly - what it catches is a dead
 * column, a stuck bit, or a panel that is not actually receiving 565. */
static void draw_panel_check(canvas_t *c, int x, int y, int w)
{
    gfx_text(c, x, y, "PANEL", 1, C_ACCENT);
    gfx_rect(c, x, y + 9, w, 1, C_EDGE);
    y += 16;

    for (int i = 0; i < w; i++) {
        int t = i * 255 / (w - 1);
        gfx_rect(c, x + i, y,      1, 9, RGB565(t, 0, 0));
        gfx_rect(c, x + i, y +  9, 1, 9, RGB565(0, t, 0));
        gfx_rect(c, x + i, y + 18, 1, 9, RGB565(0, 0, t));
        gfx_rect(c, x + i, y + 27, 1, 9, RGB565(t, t, t));
    }
    gfx_frame(c, x, y, w, 36, C_EDGE);
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    if (plat_init() != 0) {
        fprintf(stderr, "pid351: platform init failed\n");
        return 1;
    }

    canvas_t c = { framebuffer, PANEL_W, PANEL_H };
    struct sysinfo_s si;
    memset(&si, 0, sizeof si);
    sysinfo_read(&si);

    /* Before the loop, so the sweep runs on an idle machine and its numbers
     * are not competing with the demo's own drawing.
     *
     * Bracketed by the conditions it ran under, printed twice: a governor
     * that ramps partway through the sweep would otherwise make the later
     * sizes look faster than the earlier ones for reasons that have nothing
     * to do with the blit. A 1008 MHz sample and a 1296 MHz one differ by
     * 29% before anything interesting has happened. */
    conditions("before bench", &si);
    clock_calibrate();
    bench_blit();
    sysinfo_read(&si);
    conditions("after bench ", &si);

    uint64_t start = plat_now_us();
    uint64_t next  = start;
    uint64_t last_refresh = start;
    uint64_t fps_mark = start;
    uint64_t report_mark = start;
    unsigned frames = 0, fps_frames = 0;
    double fps = 0.0;
    const char *reason = "?";

    for (;;) {
        uint32_t held = plat_input();

        /* Only the combo exits. Binding a single button to quit meant that
         * button could never be seen to light up, which is the one thing this
         * program is for. */
        if ((held & PAD_START) && (held & PAD_SELECT))  { reason = "combo"; break; }
        if (plat_should_quit())                         { reason = "quit";  break; }

        uint64_t now = plat_now_us();

        /* /sys reads are the most expensive thing in this loop and none of it
         * changes faster than the eye cares about. */
        if (now - last_refresh >= 500000) {
            sysinfo_read(&si);
            last_refresh = now;
        }
        if (now - fps_mark >= 1000000) {
            fps = (double)fps_frames * 1000000.0 / (double)(now - fps_mark);
            fps_frames = 0;
            fps_mark = now;

            /* Sorting three rings is far too expensive to do per frame inside
             * the thing being measured, so it happens here, once. */
            blit_stat = stat_of(blit_ring, ring_n);
            wait_stat = stat_of(wait_ring, ring_n);
            draw_stat = stat_of(draw_ring, ring_n);
        }

        if (now - report_mark >= 10000000) {
            report(now - start, &si, fps, frames);
            report_mark = now;
        }

        uint64_t draw_t0 = plat_now_us();

        gfx_rect(&c, 0, 0, PANEL_W, PANEL_H, C_BG);
        gfx_rect(&c, 0, 0, PANEL_W, 17, C_PANEL);
        gfx_rect(&c, 0, 17, PANEL_W, 1, C_ACCENT);
        gfx_text(&c, 6, 2, "PID351", 2, C_ACCENT);
        gfx_text(&c, 84, 5, "ONE PID EVER RUNNING", 1, C_DIM);
        {
            const char *hint = "START+SELECT TO EXIT";
            gfx_text(&c, PANEL_W - gfx_text_w(hint, 1) - 6, 5, hint, 1, C_DIM);
        }

        draw_pad(&c, 8, 24, held);
        draw_axes(&c, 8, 184);
        /* Full width along the bottom: a wider ramp shows banding and dead
         * columns that a narrow one hides. */
        draw_panel_check(&c, 8, 258, 464);
        draw_info(&c, 252, 24, &si, fps, frames);

        uint32_t draw_us = (uint32_t)(plat_now_us() - draw_t0);

        plat_present(framebuffer, PANEL_W, PANEL_H);

        uint32_t b_us = 0, w_us = 0;
        plat_frame_us(&b_us, &w_us);
        blit_ring[ring_i] = b_us;
        wait_ring[ring_i] = w_us;
        draw_ring[ring_i] = draw_us;
        ring_i = (ring_i + 1) % TIME_RING;
        if (ring_n < TIME_RING)
            ring_n++;
        if (b_us < blit_min_life) blit_min_life = b_us;
        if (b_us > blit_max_life) blit_max_life = b_us;

        frames++;
        fps_frames++;

        next += FRAME_US;
        plat_sleep_until(next);
    }

    blit_stat = stat_of(blit_ring, ring_n);
    wait_stat = stat_of(wait_ring, ring_n);
    draw_stat = stat_of(draw_ring, ring_n);
    report(plat_now_us() - start, &si, fps, frames);

    printf("pid351: exit (%s) after %u frames, %.2f fps\n",
           reason, frames, fps);
    plat_shutdown();
    return 0;
}
