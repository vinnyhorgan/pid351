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

/* The blit's cost is dominated by the access pattern - measured, 480x320
 * strided 2487 us against 695 us for the same writes read sequentially - and
 * separately by whether the source fits in cache, which differs for every
 * console. So the sweep runs every console's size against every candidate
 * implementation, and the winner is whatever the panel's own silicon says it
 * is rather than whatever the argument says it should be. */
static void bench_blit(void)
{
    static const struct { const char *name; int w, h; } sizes[] = {
        { "GBA",     240, 160 },
        { "NES",     256, 240 },
        { "SNES",    256, 224 },
        { "GENESIS", 320, 224 },
        { "NATIVE",  480, 320 },
        /* Not a target since the Game Boy was dropped - kept because it is
         * the one source small enough to sit entirely in cache, which is the
         * other end of the range everything above is measured against. */
        { "PROBE",   160, 144 },
    };
    static const struct { const char *name; int id; } variants[] = {
        { "STRIDED", PLAT_BLIT_STRIDED },
        { "TILED",   PLAT_BLIT_TILED   },
        { "STAGED",  PLAT_BLIT_STAGED  },
    };
    enum { ITER = 200, TILE = 32 };
    static uint32_t samples[ITER];

    /* Real data rather than a cleared buffer, so nothing downstream can be
     * getting away with a shortcut on uniform bytes. */
    for (int i = 0; i < PANEL_W * PANEL_H; i++)
        framebuffer[i] = (px_t)(i ^ (i >> 5));

    /* Correctness before speed. The three walk the buffer in three different
     * orders and a faster blit that draws the wrong thing is worth nothing. */
    int checked = 0;
    for (size_t v = 1; v < sizeof variants / sizeof variants[0]; v++) {
        for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            int bad = plat_blit_verify(framebuffer, sizes[k].w, sizes[k].h,
                                       variants[v].id, TILE);
            if (bad < 0)
                continue;               /* backend has nothing to compare */
            checked++;
            if (bad > 0) {
                printf("pid351: VERIFY %s %s: %d pixels differ\n",
                       variants[v].name, sizes[k].name, bad);
                fflush(stdout);
            }
        }
    }
    printf("pid351: verify: %d comparisons run, only mismatches printed\n",
           checked);

    printf("pid351: blit variants, %d iterations each, tile %d, into the real "
           "back buffer, no page flip\n", ITER, TILE);

    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        /* Every console fills the panel now, so the destination is always
         * 153600 pixels and the only thing that varies is the source and the
         * ratio it is stretched by. */
        printf("pid351:   %-8s %3dx%-3d src %4uKB  %.3fx%.3f",
               sizes[k].name, sizes[k].w, sizes[k].h,
               (unsigned)((size_t)sizes[k].w * (size_t)sizes[k].h
                          * sizeof(px_t) / 1024),
               (double)PANEL_W / sizes[k].w, (double)PANEL_H / sizes[k].h);

        int ok = 1;
        for (size_t v = 0; v < sizeof variants / sizeof variants[0]; v++) {
            if (plat_bench(framebuffer, sizes[k].w, sizes[k].h,
                           variants[v].id, TILE, samples, ITER) < 0) {
                ok = 0;
                break;
            }
            stat_t s = stat_of(samples, ITER);
            printf("   %s %5u/%5u", variants[v].name, s.min, s.med);
        }
        printf("%s\n", ok ? "" : "   unavailable on this backend");
        fflush(stdout);
        if (!ok)
            return;
    }

    if (plat_bench(framebuffer, PANEL_W, PANEL_H, PLAT_BLIT_LINEAR, TILE,
                   samples, ITER) == 0) {
        stat_t s = stat_of(samples, ITER);
        printf("pid351:   %-8s %3dx%-3d x1  src %4uKB  %6u sourced px"
               "   LINEAR  %5u/%5u   <- floor: same writes, sequential reads\n",
               "CONTROL", PANEL_W, PANEL_H,
               (unsigned)(sizeof framebuffer / 1024),
               (unsigned)(PANEL_W * PANEL_H), s.min, s.med);
    }

    /* Tile size is a compile-time constant in the end, so it gets chosen the
     * same way everything else here does. Swept at full screen because that
     * is where every console lands once the black bars go. */
    static const int tiles[] = { 8, 16, 32, 64 };
    for (size_t v = 1; v < sizeof variants / sizeof variants[0]; v++) {
        printf("pid351:   tile sweep 480x320  %-7s", variants[v].name);
        for (size_t t = 0; t < sizeof tiles / sizeof tiles[0]; t++) {
            if (plat_bench(framebuffer, PANEL_W, PANEL_H, variants[v].id,
                           tiles[t], samples, ITER) < 0)
                break;
            stat_t s = stat_of(samples, ITER);
            printf("   %2d:%5u/%5u", tiles[t], s.min, s.med);
        }
        printf("\n");
        fflush(stdout);
    }

    /* Flushed at every step. first-light.sh kills the process with SIGTERM
     * after 300 seconds and stdout here is a file on the SD card, so a block
     * buffer would throw away the run that told us the most. */
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
    long charge_uah;     /* coulomb counter, see power_phase */
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
    /* The rk817 exposes current_avg and voltage_avg but no _now, so every
     * current reading is already filtered and lags a load change by longer
     * than a short measurement lasts - which is exactly how the first run
     * reported the same 663 mA before and after a seven second benchmark.
     * charge_now is the coulomb counter underneath it: unfiltered, and a
     * difference across a known interval is an average current with no
     * filter to wait out. */
    static const char *const chg[] = {
        "/sys/class/power_supply/battery/charge_now",
        "/sys/class/power_supply/BAT0/charge_now", NULL };

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
    s->charge_uah = read_long_any(chg, -1);
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

#define GOV_PATH "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"
#define BL_PATH  "/sys/class/backlight/backlight/brightness"

static int write_long(const char *path, long v)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    int ok = fprintf(f, "%ld", v) > 0;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}


/* All four cores share policy0 on this SoC, so there is one governor and one
 * frequency for the machine. Only 1008 and 1296 MHz exist; anything lower has
 * to be authored with voltages we would be choosing ourselves, which is phase
 * 4 work. Until then powersave is the whole of the low end. */
static int write_governor(const char *g)
{
    FILE *f = fopen(GOV_PATH, "w");
    if (!f)
        return -1;
    int ok = fputs(g, f) >= 0;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
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

/* What non-integer scaling actually looks like, which arithmetic cannot
 * settle. Filling the panel means 15/8 across and 10/7 down for NES and SNES,
 * so some source pixels become two on the panel and some stay one. The
 * pattern repeats every 8 and every 7, which should read as a regular texture
 * rather than as noise - but should is doing a lot of work in that sentence,
 * and single pixel lines are where it either holds up or does not. So most of
 * this card is single pixel lines, and one thing on it moves, because shimmer
 * is a motion artefact that a still frame hides completely. */
static void draw_testcard(canvas_t *c, const char *label, unsigned frame)
{
    gfx_rect(c, 0, 0, c->w, c->h, C_BG);

    int top = 14, bot = c->h - 26;
    int third = c->w / 3;

    for (int x = 2; x < third - 4; x += 2)
        gfx_rect(c, x, top, 1, bot - top, C_TEXT);

    for (int y = top; y < bot; y += 2)
        gfx_rect(c, third + 4, y, third - 12, 1, C_TEXT);

    for (int i = 0; i < bot - top; i++)
        gfx_px(c, third * 2 + 4 + i / 2, top + i, C_ACCENT);

    gfx_disc(c, c->w - third / 3, (top + bot) / 2, third / 4, C_LIT);
    gfx_frame(c, 0, 0, c->w, c->h, C_ACCENT);

    gfx_rect(c, (int)(frame % (unsigned)(c->w - 10)), bot + 2, 8, 6, C_ACCENT);

    gfx_text(c, 3, 3, label, 1, C_ACCENT);
    gfx_text(c, 3, c->h - 10, "L1/R1 SOURCE", 1, C_DIM);
}

/* ------------------------------------------------------------ power slope */

/* The question this answers is the one the whole project turns on, and it has
 * never been measured: what is a millisecond of Cortex-A35 time worth in
 * milliamps? Without it, "the blit costs 2.5 ms a frame" cannot be converted
 * into a battery argument, and every optimisation is justified by taste.
 *
 * So: hold everything else fixed - same backlight, same panel, same drawing -
 * and vary only how many extra blits happen per frame. The slope of current
 * against CPU time is the exchange rate. Then repeat the zero-load point at
 * the lower operating point, which gives the other lever's worth on the same
 * scale.
 *
 * Current comes from differencing the coulomb counter rather than reading the
 * filtered current_avg, and the first seconds of each phase are discarded, so
 * neither the governor settling nor the gauge's own filter is inside the
 * window being attributed to the load. */
struct phase_result {
    const char *name;
    long   khz;
    int    extra;
    double secs, fps;
    long   q0, q1, ua_implied, ua_avg, uv, temp_mc;
    int    q_samples, q_steps;   /* how often the counter actually moved */
    uint32_t blit_min, blit_med, work_med;
};

static long read_charge_uah(void)
{
    static const char *const p[] = {
        "/sys/class/power_supply/battery/charge_now",
        "/sys/class/power_supply/BAT0/charge_now", NULL };
    return read_long_any(p, -1);
}

static void power_phase(canvas_t *c, const char *name, const char *gov,
                        int extra, int backlight, int secs,
                        struct phase_result *out)
{
    enum { SETTLE_S = 6 };

    memset(out, 0, sizeof *out);
    out->name = name;
    out->ua_implied = -1;

    /* Once the run is ending, every remaining phase would otherwise print a
     * zero length window that reads exactly like a real measurement. */
    if (plat_should_quit()) {
        printf("pid351: PHASE %-12s skipped, run ending\n", name);
        fflush(stdout);
        return;
    }

    static uint32_t junk[8];
    static uint32_t work[TIME_RING];

    struct sysinfo_s si;
    memset(&si, 0, sizeof si);

    if (gov && write_governor(gov) != 0)
        printf("pid351: WARN could not write governor %s\n", gov);
    if (backlight >= 0 && write_long(BL_PATH, backlight) != 0)
        printf("pid351: WARN could not write backlight %d\n", backlight);

    uint64_t t0 = plat_now_us();
    uint64_t next = t0;
    uint64_t mark_at = t0 + (uint64_t)SETTLE_S * 1000000u;
    unsigned frames = 0, marked_frames = 0;
    int wn = 0, wi = 0;
    long q_mark = -1;
    int  marked = 0;      /* separate from q_mark: the counter may read -1 */
    uint64_t t_mark = 0;
    uint64_t q_at = t0;
    long q_last = -1;
    int q_n = 0, q_steps = 0;

    ring_n = ring_i = 0;
    memset(blit_ring, 0, sizeof blit_ring);

    for (;;) {
        uint64_t now = plat_now_us();
        if (now - t0 >= (uint64_t)secs * 1000000u)
            break;

        uint32_t held = plat_input();
        if (plat_should_quit() || ((held & PAD_START) && (held & PAD_SELECT)))
            break;

        /* The load. Deliberately the same blit the real path runs, so the
         * slope is expressed in the units we care about rather than in some
         * synthetic loop's. */
        uint64_t w0 = plat_now_us();

        if (extra > 0)
            plat_bench(framebuffer, PANEL_W, PANEL_H, PLAT_BLIT_STRIDED,
                       32, junk, extra);

        gfx_rect(c, 0, 0, PANEL_W, PANEL_H, C_BG);
        gfx_text(c, 8, 8, "PID351 POWER MEASUREMENT", 2, C_ACCENT);
        gfx_text(c, 8, 40, "DO NOT TOUCH - LEAVE IT ALONE", 1, C_DIM);
        {
            char v[96];
            snprintf(v, sizeof v, "PHASE %s", name);
            gfx_text(c, 8, 70, v, 1, C_TEXT);
            snprintf(v, sizeof v, "EXTRA BLITS %d", extra);
            gfx_text(c, 8, 84, v, 1, C_TEXT);
            snprintf(v, sizeof v, "%lu / %d S",
                     (unsigned long)((now - t0) / 1000000u), secs);
            gfx_text(c, 8, 98, v, 1, C_TEXT);
            gfx_rect(c, 8, 116, 464, 10, C_PANEL);
            gfx_rect(c, 8, 116,
                     (int)((now - t0) * 464u / ((uint64_t)secs * 1000000u)),
                     10, C_ACCENT);
            gfx_text(c, 8, 140, "START+SELECT ABORTS", 1, C_DIM);
        }

        plat_present(framebuffer, PANEL_W, PANEL_H);

        uint32_t b_us = 0, w_us = 0;
        plat_frame_us(&b_us, &w_us);
        blit_ring[ring_i] = b_us;
        ring_i = (ring_i + 1) % TIME_RING;
        if (ring_n < TIME_RING)
            ring_n++;
        work[wi] = (uint32_t)(plat_now_us() - w0);
        wi = (wi + 1) % TIME_RING;
        if (wn < TIME_RING)
            wn++;

        frames++;
        if (!marked && plat_now_us() >= mark_at) {
            marked = 1;
            sysinfo_read(&si);
            q_mark = si.charge_uah;
            t_mark = plat_now_us();
            marked_frames = frames;
        }

        /* A counter that only ticks once or twice inside the window would
         * make the implied current meaningless while still printing a
         * confident number, so its granularity is measured alongside it. */
        if (plat_now_us() >= q_at) {
            long q = read_charge_uah();
            if (q_n && q != q_last)
                q_steps++;
            q_last = q;
            q_n++;
            q_at += 1000000u;
        }

        next += FRAME_US;
        plat_sleep_until(next);
    }

    sysinfo_read(&si);
    uint64_t t_end = plat_now_us();

    stat_t b = stat_of(blit_ring, ring_n);
    stat_t w = stat_of(work, wn);

    out->name  = name;
    out->khz   = si.cpu_khz;
    out->extra = extra;
    out->q0    = q_mark;
    out->q1    = si.charge_uah;
    out->ua_avg = si.current_ua;
    out->uv    = si.voltage_uv;
    out->temp_mc = si.temp_mc;
    out->blit_min = b.min;
    out->blit_med = b.med;
    out->work_med = w.med;
    out->q_samples = q_n;
    out->q_steps   = q_steps;

    double dt = marked ? (double)(t_end - t_mark) / 1000000.0 : 0.0;
    out->secs = dt;
    out->fps  = dt > 0.0 ? (double)(frames - marked_frames) / dt : 0.0;

    /* uAh over dt seconds back to uA. Discharging counts down, so the
     * difference is taken in the direction that yields a positive draw. */
    out->ua_implied = (q_mark >= 0 && dt > 0.5 && si.charge_uah >= 0)
                    ? (long)((double)(q_mark - si.charge_uah) * 3600.0 / dt)
                    : -1;

    printf("pid351: PHASE %-12s gov=%s khz=%ld extra=%d bl=%ld window=%.1fs fps=%.2f "
           "q %ld->%ld uAh (%d samples, %d steps)  implied=%ld uA  "
           "current_avg=%ld uA  volt=%ld uV  "
           "temp_mc=%ld  blit=%u/%u  work_med=%u us\n",
           name, gov ? gov : "-", out->khz, extra, si.backlight,
           out->secs, out->fps,
           out->q0, out->q1, out->q_samples, out->q_steps, out->ua_implied,
           out->ua_avg, out->uv,
           out->temp_mc, out->blit_min, out->blit_med, out->work_med);
    fflush(stdout);
}

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
    char gov0[24];
    snprintf(gov0, sizeof gov0, "%s", si.governor);

    conditions("before bench", &si);
    clock_calibrate();

    /* Whether the scanout buffer is cached or write combined is the fact the
     * entire blit argument rests on, and it has been assumed in both
     * directions across this session without once being checked. */
    {
        plat_mem_t m;
        if (plat_mem_probe(&m, 5) == 0)
            printf("pid351: memory 300KB sequential, best of 5, us: "
                   "scanout w=%u r=%u rmw=%u   ram w=%u r=%u rmw=%u   "
                   "(scanout reads far above ram reads means write combined)\n",
                   m.fb_write, m.fb_read, m.fb_rmw,
                   m.ram_write, m.ram_read, m.ram_rmw);
        else
            printf("pid351: memory probe unavailable\n");
        fflush(stdout);
    }

    /* We have been pacing to a hardcoded 59.727 Hz and reading back 59.72,
     * which proves the sleep works and nothing else. Per-console timing needs
     * the panel's real rate, so take it from the mode's own arithmetic and
     * then from flipping as fast as the hardware allows. */
    {
        uint32_t exact = 0, ck = 0, ht = 0, vt = 0, meas = 0;
        plat_mode_timing(&exact, &ck, &ht, &vt);
        printf("pid351: mode clock=%u kHz htotal=%u vtotal=%u -> exact %u.%03u Hz\n",
               ck, ht, vt, exact / 1000u, exact % 1000u);
        if (plat_vblank_probe(300, &meas) == 0)
            printf("pid351: vblank measured over 300 flips: %u.%03u Hz "
                   "(frame budget %u us against our hardcoded %d)\n",
                   meas / 1000u, meas % 1000u,
                   meas ? (unsigned)(1000000000u / meas) : 0u, FRAME_US);
        else
            printf("pid351: vblank probe unavailable\n");
        fflush(stdout);
    }

    bench_blit();
    sysinfo_read(&si);
    conditions("after bench ", &si);

    /* Four phases, about two and a half minutes. Three at the high operating
     * point with rising load give the slope of current against CPU time; the
     * fourth repeats the idle point at the low one, which puts both levers on
     * the same scale for the first time. */
    long bl0 = si.backlight;
    {
        struct phase_result bl[3];

        /* The backlight first, because if it turns out to dominate then every
         * CPU number that follows is a rounding error and we should know that
         * before reading them. It has been called the largest consumer on
         * this device three times in this project without anyone measuring
         * it once. */
        power_phase(&c, "bl-255", "performance", 0, 255, 35, &bl[0]);
        power_phase(&c, "bl-127", "performance", 0, 127, 35, &bl[1]);
        power_phase(&c, "bl-32",  "performance", 0,  32, 35, &bl[2]);
        if (bl[0].ua_implied >= 0 && bl[2].ua_implied >= 0)
            printf("pid351: BACKLIGHT 255=%ld  127=%ld  32=%ld uA  "
                   "-> %ld uA between full and near dark\n",
                   bl[0].ua_implied, bl[1].ua_implied, bl[2].ua_implied,
                   bl[0].ua_implied - bl[2].ua_implied);
        fflush(stdout);

        /* Everything below is at the backlight the machine arrived with, so
         * the CPU phases are comparable with each other and with the run we
         * already have. */
        struct phase_result pr[5];
        power_phase(&c, "base-1296",  "performance", 0, (int)bl0, 35, &pr[0]);
        power_phase(&c, "load2-1296", "performance", 2, -1, 35, &pr[1]);
        power_phase(&c, "load4-1296", "performance", 4, -1, 35, &pr[2]);
        power_phase(&c, "base-1008",  "powersave",   0, -1, 35, &pr[3]);
        /* The control. Phases run cold to hot, and leakage rises with
         * temperature, so without repeating the first point at the end a
         * thermal drift would be indistinguishable from the slope we are
         * trying to measure. If this disagrees with base-1296, the slope is
         * contaminated and the numbers above it are not to be trusted. */
        power_phase(&c, "base-1296-b", "performance", 0, -1, 35, &pr[4]);

        /* The whole point of the exercise, stated in the units the decision
         * gets made in. Extra CPU per frame is the count times the blit we
         * just measured, converted to a duty cycle against the frame. */
        /* Against the work actually measured, not against extra x blit_med.
         * Those differ whenever a phase failed to hold 60 fps, and using the
         * assumed figure would hide exactly that failure. */
        for (int i = 1; i < 3; i++) {
            if (pr[i].ua_implied < 0 || pr[0].ua_implied < 0)
                continue;
            double dms = ((double)pr[i].work_med
                        - (double)pr[0].work_med) / 1000.0;
            double dua = (double)(pr[i].ua_implied - pr[0].ua_implied);
            printf("pid351: SLOPE %s vs base: +%.2f ms/frame measured -> "
                   "%+.0f uA (%.1f uA per ms per frame)%s\n",
                   pr[i].name, dms, dua, dms > 0.0 ? dua / dms : 0.0,
                   pr[i].fps < 59.0 ? "   INVALID: dropped below 59 fps" : "");
        }
        printf("pid351: THERMAL CONTROL base-1296 %ld uA vs base-1296-b %ld uA "
               "at %ld and %ld mC - a large gap here invalidates the slopes\n",
               pr[0].ua_implied, pr[4].ua_implied,
               pr[0].temp_mc, pr[4].temp_mc);
        if (pr[3].ua_implied >= 0 && pr[0].ua_implied >= 0)
            printf("pid351: OPP 1296 %ld uA vs 1008 %ld uA -> %+ld uA "
                   "(%.1f%%) for the lower operating point at idle load\n",
                   pr[0].ua_implied, pr[3].ua_implied,
                   pr[3].ua_implied - pr[0].ua_implied,
                   pr[0].ua_implied ? (double)(pr[3].ua_implied
                       - pr[0].ua_implied) * 100.0 / (double)pr[0].ua_implied
                       : 0.0);
        fflush(stdout);
    }

    /* Put it back. Leaving the machine on powersave after a measurement would
     * silently poison every number taken after it, including the ones read off
     * the panel by eye. */
    if (gov0[0] && gov0[0] != '-' && write_governor(gov0) != 0)
        printf("pid351: WARN could not restore governor %s\n", gov0);
    if (bl0 >= 0 && write_long(BL_PATH, bl0) != 0)
        printf("pid351: WARN could not restore backlight %ld\n", bl0);
    sysinfo_read(&si);
    conditions("after phases", &si);

    uint64_t start = plat_now_us();
    uint64_t next  = start;
    uint64_t last_refresh = start;
    uint64_t fps_mark = start;
    uint64_t report_mark = start;
    unsigned frames = 0, fps_frames = 0;
    double fps = 0.0;
    const char *reason = "?";

    /* NES and SNES are the same 256x224, so three cards cover all four
     * consoles. Cycled by hand because the only way to settle whether
     * non-integer scaling looks acceptable is to look at it. */
    static const struct { const char *name; int w, h; } src[] = {
        { "NATIVE 480X320 - NO SCALING",   480, 320 },
        { "GBA 240X160 - EXACT 2X",        240, 160 },
        { "NES/SNES 256X224 - 15/8 X 10/7", 256, 224 },
        { "GENESIS 320X224 - 3/2 X 10/7",  320, 224 },
    };
    const int src_n = (int)(sizeof src / sizeof src[0]);
    int src_i = 0;
    uint32_t prev_held = 0;

    for (;;) {
        uint32_t held = plat_input();
        uint32_t went_down = held & ~prev_held;
        prev_held = held;

        /* Only the combo exits. Binding a single button to quit meant that
         * button could never be seen to light up, which is the one thing this
         * program is for. */
        if ((held & PAD_START) && (held & PAD_SELECT))  { reason = "combo"; break; }
        if (plat_should_quit())                         { reason = "quit";  break; }

        if (went_down & PAD_R1) src_i = (src_i + 1) % src_n;
        if (went_down & PAD_L1) src_i = (src_i + src_n - 1) % src_n;

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

        if (src_i != 0) {
            canvas_t tc = { framebuffer, src[src_i].w, src[src_i].h };
            draw_testcard(&tc, src[src_i].name, frames);
            uint32_t d_us = (uint32_t)(plat_now_us() - draw_t0);
            plat_present(framebuffer, src[src_i].w, src[src_i].h);

            uint32_t bb = 0, ww = 0;
            plat_frame_us(&bb, &ww);
            blit_ring[ring_i] = bb;
            wait_ring[ring_i] = ww;
            draw_ring[ring_i] = d_us;
            ring_i = (ring_i + 1) % TIME_RING;
            if (ring_n < TIME_RING)
                ring_n++;

            frames++;
            fps_frames++;
            next += FRAME_US;
            plat_sleep_until(next);
            continue;
        }

        gfx_rect(&c, 0, 0, PANEL_W, PANEL_H, C_BG);
        gfx_rect(&c, 0, 0, PANEL_W, 17, C_PANEL);
        gfx_rect(&c, 0, 17, PANEL_W, 1, C_ACCENT);
        gfx_text(&c, 6, 2, "PID351", 2, C_ACCENT);
        gfx_text(&c, 84, 5, "ONE PID EVER RUNNING", 1, C_DIM);
        {
            const char *hint = "L1/R1 SOURCE  START+SELECT EXIT";
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
