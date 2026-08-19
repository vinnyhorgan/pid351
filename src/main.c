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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "pid351.h"
#include "platform.h"
#include "gfx.h"
#include "scale.h"
#include "aud.h"
#include "core.h"
#include "tele.h"

/* The panel's real period, not a console's. cpll runs at 408 MHz and the VOP
 * divides it by exactly 24 for a 17.000000 MHz pixel clock, so 584x485 totals
 * give 60.0186 Hz with no rounding anywhere - the two flip-rate measurements
 * (60.109 and 60.050 Hz) bracket that within their own start/stop alignment
 * error, they do not contradict it. We pace the panel and resample audio,
 * rather than the reverse, because the panel cannot be retuned. */
#define FRAME_US 16661          /* 60.0186 Hz, the panel */

/* How long the wordmark stays up before the game does. Two seconds because
 * that is what was asked for, and the reasoning stands on its own: a splash
 * that lasts less than half a second is not a splash, it is a glitch. */
#define SPLASH_HOLD_US 2000000u


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
        { "STAGED",  PLAT_BLIT_STAGED  },
        { "ROW",     PLAT_BLIT_STAGED_ROW },
    };
    enum { ITER = 200, TILE = 64 };
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

    /* ROW clamps its band height internally, so saying "tile 64" for it would
     * be a lie in the log a year from now. */
    printf("pid351: blit variants, %d iterations each, into the real back "
           "buffer, no page flip (STAGED tile %d, ROW clamped to 32 rows)\n",
           ITER, TILE);

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
    static const int tiles[] = { 16, 32, 64 };
    static const int rows[]  = { 4, 8, 16, 24, 32 };

    printf("pid351:   sweep 480x320 STAGED square tile");
    for (size_t t = 0; t < sizeof tiles / sizeof tiles[0]; t++) {
        if (plat_bench(framebuffer, PANEL_W, PANEL_H, PLAT_BLIT_STAGED,
                       tiles[t], samples, ITER) < 0)
            break;
        stat_t s = stat_of(samples, ITER);
        printf("   %2d:%5u/%5u", tiles[t], s.min, s.med);
    }
    printf("\n");

    printf("pid351:   sweep 480x320 ROW full-width rows ");
    for (size_t t = 0; t < sizeof rows / sizeof rows[0]; t++) {
        if (plat_bench(framebuffer, PANEL_W, PANEL_H, PLAT_BLIT_STAGED_ROW,
                       rows[t], samples, ITER) < 0)
            break;
        stat_t s = stat_of(samples, ITER);
        printf("   %2d:%5u/%5u", rows[t], s.min, s.med);
    }
    printf("\n");
    fflush(stdout);

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
    long charge_full_uah;
    long mem_total_kb, mem_avail_kb;
    long uptime_s;
};

static void sysinfo_read(struct sysinfo_s *s)
{
    static const char *const volt[] = {
        "/sys/class/power_supply/rk817-battery/voltage_avg",
        "/sys/class/power_supply/battery/voltage_avg",
        "/sys/class/power_supply/battery/voltage_now",
        "/sys/class/power_supply/BAT0/voltage_now", NULL };
    static const char *const curr[] = {
        "/sys/class/power_supply/rk817-battery/current_avg",
        "/sys/class/power_supply/battery/current_avg",
        "/sys/class/power_supply/battery/current_now",
        "/sys/class/power_supply/BAT0/current_now", NULL };
    static const char *const cap[] = {
        "/sys/class/power_supply/rk817-battery/capacity",
        "/sys/class/power_supply/battery/capacity",
        "/sys/class/power_supply/BAT0/capacity", NULL };
    /* Two names for the same battery. Mainline's rk817_charger registers it
     * as "rk817-battery"; the vendor driver ROCKNIX ships calls it "battery".
     * The properties are identical - I checked the mainline driver's property
     * list rather than assuming again - so only the directory differs. Both
     * are tried, which is what keeps the same binary comparable across the
     * two systems.
     *
     * This cost two boots and an investigation into a kernel bug that did not
     * exist. The census that found it is worth more than the fix.
     *
     * The rk817 exposes current_avg and voltage_avg but no _now, so every
     * current reading is already filtered and lags a load change by longer
     * than a short measurement lasts - which is exactly how the first run
     * reported the same 663 mA before and after a seven second benchmark.
     * charge_now is the coulomb counter underneath it: unfiltered, and a
     * difference across a known interval is an average current with no
     * filter to wait out. */
    static const char *const chg[] = {
        "/sys/class/power_supply/rk817-battery/charge_now",
        "/sys/class/power_supply/battery/charge_now",
        "/sys/class/power_supply/BAT0/charge_now", NULL };
    static const char *const full[] = {
        "/sys/class/power_supply/rk817-battery/charge_full",
        "/sys/class/power_supply/battery/charge_full",
        "/sys/class/power_supply/BAT0/charge_full", NULL };

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
    s->charge_full_uah = read_long_any(full, -1);
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

/* Half of the panel's 1666 steps, which is what the kernel used to pick for
 * itself before the device tree told it to start dark. Every measurement in
 * docs/ was taken here, so it is not a preference - changing it invalidates
 * the drain figures. A brightness control belongs in the menu, on top of this
 * as the value it starts from. */
#define BL_ON 833

/* The panel is dark from reset until this is called, so that the second of
 * U-Boot's leftover screen between the backlight probing and anything setting
 * a mode is never lit. Called from splash(), which is the first thing every
 * path presents, and from every path that fails before getting there - a dark
 * screen and a broken one look identical, and only one of them can be
 * diagnosed by looking at it.
 *
 * Fails silently off the device, where there is no such file. */
static void backlight(long v);

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

/* -------------------------------------------- pinning an operating point */

/* We run our own device tree now, and mainline's carries two OPPs ROCKNIX
 * deleted: 816 MHz at 1.050 V and 600 MHz at 0.950 V. Dynamic power goes as
 * f*V^2 and vdd_arm drops with the OPP, so this is the largest untested lever
 * left in the project - and it only exists because we stopped running someone
 * else's kernel.
 *
 * With the performance governor the core sits at scaling_max_freq, so writing
 * that pins the frequency without needing the userspace governor. */
#define MAXF_PATH "/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq"
#define MINF_PATH "/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq"

static int pin_freq(long khz)
{
    /* Order matters: lower the floor before the ceiling when descending, or
     * the write is rejected for crossing it. Doing both twice is simpler than
     * reasoning about which direction we are going. */
    write_long(MINF_PATH, khz);
    if (write_long(MAXF_PATH, khz) != 0)
        return -1;
    return write_long(MINF_PATH, khz);
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
           "volt_uv=%ld curr_ua=%ld backlight=%ld/%ld\n",
           when, s->cpu_khz, s->governor, s->gpu_hz, s->temp_mc,
           s->voltage_uv, s->current_ua, s->backlight, s->backlight_max);
    fflush(stdout);
}

static void report(uint64_t elapsed_us, const struct sysinfo_s *s,
                   double fps, unsigned frames)
{
    printf("pid351: t=%llus frames=%u fps=%.2f n=%d "
           "blit=%u/%u/%u life=%u/%u draw=%u/%u/%u wait=%u/%u/%u "
           "cpu_khz=%ld gov=%s gpu_hz=%ld temp_mc=%ld "
           "volt_uv=%ld curr_ua=%ld backlight=%ld/%ld "
           "aud_hz=%d aud_lvl=%d aud_xrun=%d\n",
           (unsigned long long)(elapsed_us / 1000000u), frames, fps, ring_n,
           blit_stat.min, blit_stat.med, blit_stat.max,
           blit_min_life == UINT32_MAX ? 0u : blit_min_life, blit_max_life,
           draw_stat.min, draw_stat.med, draw_stat.max,
           wait_stat.min, wait_stat.med, wait_stat.max,
           s->cpu_khz, s->governor, s->gpu_hz, s->temp_mc,
           s->voltage_uv, s->current_ua, s->backlight, s->backlight_max,
           aud_rate(), aud_level(), aud_xruns());
    fflush(stdout);
}

/* ---------------------------------------------------------------- audio */

/* A tone while a face button is held, silence otherwise. Two things can be
 * independently wrong - that samples reach the codec at all, and that they
 * arrive at the rate the panel is pacing - and only the second one is subtle.
 * A continuous tone would prove the first and hide the second, because a
 * buffer that is slowly draining sounds exactly like one that is not, right
 * up until it runs dry. Silence between notes makes the buffer level on
 * screen the thing being watched rather than the sound. */
static uint32_t tone_phase;
static int tone_amp;
static int aud_lvl;

static void audio_frame(uint32_t held)
{
    static int16_t buf[2048 * 2];
    const int cap = (int)(sizeof buf / sizeof buf[0]) / 2;

    int n = aud_due();
    if (n <= 0)
        return;
    if (n > cap)
        n = cap;

    /* Four notes rather than one, so a button stuck down is audible as well
     * as visible - the pad is the part of this machine we cannot see. */
    unsigned hz = 0;
    if      (held & PAD_A) hz = 440;
    else if (held & PAD_B) hz = 349;
    else if (held & PAD_X) hz = 523;
    else if (held & PAD_Y) hz = 587;

    int rate = aud_rate();
    /* Phase as 32-bit turns: the integer wrap is the period, so the frequency
     * carries no accumulating rounding and there is no modulo per sample. */
    uint32_t step = rate ? (uint32_t)(((uint64_t)hz << 32) / (unsigned)rate)
                         : 0u;

    for (int i = 0; i < n; i++) {
        /* Ramped, not gated. A gated tone clicks, and a click is exactly what
         * an underrun sounds like, so gating would disguise the fault this
         * whole subsystem is being watched for. */
        int want = hz ? 2600 : 0;
        if (tone_amp < want) {
            tone_amp += 40;
            if (tone_amp > want) tone_amp = want;
        } else if (tone_amp > want) {
            tone_amp -= 40;
            if (tone_amp < want) tone_amp = want;
        }

        tone_phase += step;
        /* Triangle rather than sine: there is no libm in the device build,
         * and the harmonics make a wrong sample rate easy to hear. */
        int32_t t   = (int32_t)(tone_phase >> 16);
        int32_t tri = t < 32768 ? t - 16384 : 49152 - t;
        buf[i * 2] = buf[i * 2 + 1] = (int16_t)(tri * tone_amp / 16384);
    }

    aud_write(buf, n);
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

    /* Buffer level and underruns, not "playing". The level is the only thing
     * that shows the panel and the codec pulling against each other, and it
     * shows it long before anything becomes audible. */
    if (aud_rate() > 0)
        snprintf(v, sizeof v, "%d x%d", aud_lvl, aud_xruns());
    else
        snprintf(v, sizeof v, "none");
    info_line(c, x, y, "AUDIO", v, aud_xruns() ? C_WARN : C_TEXT); y += lh;

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


/* ------------------------------------------------------------- session log
 *
 * Enough measurement, taken during real play, to settle whether this machine
 * is doing the job - without needing a second session to answer the question
 * the first one raised. That has happened repeatedly here: a run showed fast
 * mode was slow but not why, showed the audio ring falling but not whether it
 * converged, showed a blit spike but not what else was happening in that
 * frame. All three were answerable from data the loop already had and threw
 * away.
 *
 * So every frame is counted, not just a rolling window, and the analysis
 * happens on the device at exit. A distribution beats an average and a
 * percentile beats a maximum: one late frame in a session is noise, one in
 * fifty is a defect, and min/med/max cannot tell those apart. */

struct tele_frame {
    uint32_t emu_us;     /* core_run plus any core_skip in this iteration */
    uint32_t scale_us;   /* the resampler */
    uint32_t blit_us;    /* rotate into scanout, plus the bar */
    uint32_t wait_us;    /* blocked on vblank - idle, not work */
    /* Time inside core_audio, which is mostly time inside aud_write. ALSA
     * blocks that call when the codec buffer is full, so this is a second
     * place the loop waits rather than works - and it was being counted as
     * work. Fast mode leaves the buffer nearly full by design, so every
     * release of R2 is followed by frames that block here until it drains. */
    uint32_t aud_us;
    uint32_t work_us;    /* whole iteration bar the sleep */
    /* work minus the vblank wait: the CPU time the frame actually cost, and
     * the only one of these that answers whether the machine keeps up. Once
     * the loop is vblank locked, work_us is pinned to the panel period no
     * matter how much or how little is being done inside it, so counting
     * frames where work_us overran would report the panel rather than us. */
    uint32_t busy_us;
    uint32_t fast;       /* 1 if R2 was held, so this frame ran six */
};

/* Kept as histograms rather than as a log of frames.
 *
 * The log came first and had a hard nine minute horizon: 32768 records, past
 * which a session kept its beginning and dropped the part still being played.
 * That is backwards for every question these numbers exist to answer, because
 * the interesting frames - a thermal throttle, a card gone slow, the hour
 * mark - are all at the far end. Sorting it was also quadratic, which is
 * tolerable for nine minutes and not for an evening.
 *
 * A histogram has no horizon, costs one increment per frame, and gives exact
 * percentiles in a single pass over its bins. The only thing it gives up is
 * the order the frames arrived in, and nothing here ever asked for that.
 *
 * Two bin widths, because these fields span three orders of magnitude and one
 * width cannot serve both ends: a top-up of the codec is tens of microseconds
 * and a fast frame is fifty thousand. Below 8192 us every microsecond is its
 * own bin, so everything that happens inside a frame budget is exact; above
 * it the bins are 64 us, which is 0.4% at one frame and finer as a proportion
 * higher up. The true maximum is tracked outside the bins, so the one number
 * a saturating top bin could misreport is not taken from them at all. */
#define HIST_FINE   8192            /* 1 us bins, 0 .. 8191 us */
#define HIST_SHIFT  6               /* then 64 us bins */
#define HIST_COARSE 8192            /* .. 532416 us, half a second */
#define HIST_BINS   (HIST_FINE + HIST_COARSE)

typedef struct {
    uint32_t bin[HIST_BINS];
    uint32_t n;
    uint32_t max;                   /* exact, not a bin */
    uint32_t over;                  /* samples past the last bin */
} hist_t;

static void hist_add(hist_t *h, uint32_t us)
{
    uint32_t b = us < HIST_FINE
               ? us
               : HIST_FINE + ((us - HIST_FINE) >> HIST_SHIFT);

    if (b >= HIST_BINS) {
        b = HIST_BINS - 1;
        h->over++;
    }
    h->bin[b]++;
    h->n++;
    if (us > h->max)
        h->max = us;
}

/* The low edge of a bin. Reporting the low edge errs by up to one bin width
 * in the honest direction - a percentile quoted from the top of its bin would
 * claim a frame took longer than any frame did. */
static uint32_t hist_us(uint32_t b)
{
    return b < HIST_FINE ? b : HIST_FINE + ((b - HIST_FINE) << HIST_SHIFT);
}

static uint32_t hist_pct(const hist_t *h, int pct)
{
    uint32_t want, seen = 0, b;

    if (h->n == 0)
        return 0;
    want = (uint32_t)(((uint64_t)h->n * (uint64_t)pct) / 100u);
    for (b = 0; b < HIST_BINS; b++) {
        seen += h->bin[b];
        if (seen > want)
            return hist_us(b);
    }
    return h->max;
}

/* Normal and fast frames are counted apart because they are not the same
 * measurement. A fast frame runs the core six times, so its emu is six frames
 * of work in one sample; mixed together, the p99 of a session that spent a
 * seventh of its length in fast mode is just "a fast frame", and the p50 has
 * moved to make room for it. Neither number then describes anything that
 * actually happens. */
enum { H_EMU, H_SCALE, H_BLIT, H_BUSY, H_AUD, H_WAIT, H_LAT, H_DEAD,
       H_PERIOD, H_FLIP, H_RING, H_CODEC, H_FIELDS };

static const char *const hist_name[H_FIELDS] = {
    "emu", "scale", "blit", "busy", "audio", "vblank", "latency", "deadtime",
    "period", "flip", "ring", "codec"
};

static hist_t hist[2][H_FIELDS];    /* [fast][field] */

static void tele_add(const struct tele_frame *f)
{
    hist_t *h = hist[f->fast ? 1 : 0];

    hist_add(&h[H_EMU],   f->emu_us);
    hist_add(&h[H_SCALE], f->scale_us);
    hist_add(&h[H_BLIT],  f->blit_us);
    hist_add(&h[H_BUSY],  f->busy_us);
    hist_add(&h[H_AUD],   f->aud_us);
    hist_add(&h[H_WAIT],  f->wait_us);
}

/* The median cost of emulating one frame, over the frames that emulated
 * exactly one. Never zero, because it is a divisor. */
static uint32_t tele_emu_p50(void)
{
    uint32_t v = hist_pct(&hist[0][H_EMU], 50);
    return v ? v : 1;
}

/* One field as p50/p90/p99/max. The name is padded here rather than at the
 * call site so the columns line up in a log read at 80 columns. */
static void hist_report(int fast, int field, uint32_t budget)
{
    const hist_t *h = &hist[fast][field];

    if (h->n == 0)
        return;
    printf("pid351:   %-8s p50 %6u  p90 %6u  p99 %6u  max %6u us  %5.1f%% "
           "of frame\n",
           hist_name[field], hist_pct(h, 50), hist_pct(h, 90),
           hist_pct(h, 99), h->max,
           (double)hist_pct(h, 50) * 100.0 / (double)budget);
}

/* A buffer level rather than a duration. Two things change: the interesting
 * tail is the low one, because the failure it predicts is running out rather
 * than taking too long, and there is no frame-budget column because an
 * occupancy is not a fraction of a frame. The bins are the same ones - a
 * level in frames never leaves the linear part of the histogram. */
static void hist_report_lo(int field, const char *unit)
{
    const hist_t *h = &hist[0][field];

    if (h->n == 0) {
        printf("pid351:   %-8s never sampled\n", hist_name[field]);
        return;
    }
    printf("pid351:   %-8s min %5u  p1 %5u  p10 %5u  p50 %5u  p90 %5u  "
           "max %5u %s (n=%u)\n",
           hist_name[field], hist_pct(h, 0), hist_pct(h, 1), hist_pct(h, 10),
           hist_pct(h, 50), hist_pct(h, 90), h->max, unit, h->n);
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
        "/sys/class/power_supply/rk817-battery/charge_now",
        "/sys/class/power_supply/battery/charge_now",
        "/sys/class/power_supply/BAT0/charge_now", NULL };
    return read_long_any(p, -1);
}

/* ------------------------------------------------ diagnosing the fuel gauge */

/* The battery is missing and the kernel will not say why. rk817_charger has
 * exactly one silent failure path - it returns -ENODEV without a word when it
 * cannot find a `charger` child under the PMIC's device tree node - and the
 * driver core logs -ENODEV at pr_debug, which is compiled out. Meanwhile the
 * node demonstrably is in the device tree we booted.
 *
 * So the three questions are: does the kernel see that node, was the platform
 * device created at all, and did anything bind to it. Static reading of the
 * source has not settled it, so ask the machine. */
static void list_dir(const char *path, const char *match)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;

    if (!d) {
        printf("pid351:   %s: %s\n", path, strerror(errno));
        return;
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (match && !strstr(e->d_name, match))
            continue;

        /* A platform device with no `driver` symlink was created and then
         * either rejected or never matched, which is the whole question. */
        char sub[512];
        snprintf(sub, sizeof sub, "%s/%s/driver", path, e->d_name);
        printf("pid351:   %-28s %s\n", e->d_name,
               access(sub, F_OK) == 0 ? "<- driver bound" : "");
        n++;
    }
    closedir(d);
    if (!n)
        printf("pid351:   (nothing%s%s)\n", match ? " matching " : "",
               match ? match : "");
}

static void census(void)
{
    static const char *const dt[] = {
        "/proc/device-tree/i2c@ff180000/pmic@20",
        "/proc/device-tree/i2c@ff180000/pmic@20/charger",
        "/proc/device-tree/i2c@ff180000/pmic@20/charger/monitored-battery",
        "/proc/device-tree/battery",
        "/proc/device-tree/battery/compatible",
        NULL
    };

    printf("pid351: fuel gauge census\n");
    printf("pid351:  device tree, as the kernel sees it:\n");
    for (int i = 0; dt[i]; i++)
        printf("pid351:   %-62s %s\n", dt[i],
               access(dt[i], F_OK) == 0 ? "present" : "MISSING");

    printf("pid351:  platform devices matching rk8:\n");
    list_dir("/sys/bus/platform/devices", "rk8");
    printf("pid351:  power supplies:\n");
    list_dir("/sys/class/power_supply", NULL);
    printf("pid351:  drivers matching rk8:\n");
    list_dir("/sys/bus/platform/drivers", "rk8");
    fflush(stdout);
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

        plat_present(framebuffer, PANEL_W, PANEL_H, NULL);

        uint32_t b_us = 0, w_us = 0;
        plat_frame_us(&b_us, &w_us, NULL);
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

/* Something on the panel before anything that can block.
 *
 * On the device the window between DRM coming up and the first emulated frame
 * is seconds long - the sound card is still probing and the core has a ring to
 * prime - and the panel holds whatever the bootloader left there until we
 * flip. Without this, booting straight into a game is indistinguishable from a
 * machine that never reached us, which is exactly the wrong question to be
 * asked when something goes wrong. A frozen splash says the display works and
 * something after it does not. */
/* The one thing on the screen between the kernel handing over and the game
 * appearing, which is about eight hundred milliseconds.
 *
 * It used to wear the demo's header bar and sit in the top left corner, and
 * at that length it read as a glimpse of some other program rather than as
 * this one starting - the honest description offered was "the demo for a
 * flash of a second". Centred on black it reads as a boot screen, which is
 * what it is.
 *
 * It is still here rather than deleted for the reason it was added: the panel
 * holds whatever the bootloader left until something flips it, so without
 * this a hang before the first frame looks exactly like a machine that never
 * reached us. A frozen wordmark says the display came up and something after
 * it did not. */
/* Ink, not advance. gfx_text_w counts a trailing gap after the last glyph
 * that nothing is drawn in, so centring on it leans every line left - and by
 * a different amount at each scale, which is why the wordmark and the word
 * under it agreed with neither the panel nor each other. */
static int ink_w(const char *s, int scale)
{
    return gfx_text_w(s, scale) - (FONT_ADVANCE - FONT_W) * scale;
}

static void splash(const char *line)
{
    canvas_t c = { framebuffer, PANEL_W, PANEL_H };
    const int mark = 4;
    const int gap = 10;
    /* Centre the pair as one block. Centring the wordmark on the panel and
     * then hanging the line off it puts the visual weight above the middle,
     * which is what "not quite centred" looked like. */
    int y = (PANEL_H - (FONT_H * mark + gap + FONT_H)) / 2;

    gfx_rect(&c, 0, 0, PANEL_W, PANEL_H, C_BG);
    gfx_text(&c, (PANEL_W - ink_w("PID351", mark)) / 2, y,
             "PID351", mark, C_ACCENT);
    gfx_text(&c, (PANEL_W - ink_w(line, 1)) / 2, y + FONT_H * mark + gap,
             line, 1, C_DIM);
    plat_present(framebuffer, PANEL_W, PANEL_H, NULL);
    /* After the flip, never before: the whole point of starting dark is that
     * the backlight comes up on our first frame and not on whatever was on
     * the panel beforehand. */
    backlight(BL_ON);
}

/* Where ROMs live on the card, relative to the FAT partition's root. A
 * compile-time constant because there is no config file and never will be
 * one; the card layout is as fixed as the hardware. */
static void backlight(long v)
{
    write_long(BL_PATH, v);
}

#define ROM_DIR "pid351/roms"

/* First file in `dir` that some core claims, by name order so that the same
 * card always boots the same game. scandir sorts for us and allocates, which
 * is fine exactly once at startup and would not be inside the frame loop. */
static int first_rom(const char *dir, char *out, size_t outsz)
{
    struct dirent **ents;
    int n = scandir(dir, &ents, NULL, alphasort);
    if (n < 0)
        return -1;

    int found = -1;
    for (int i = 0; i < n; i++) {
        if (found < 0 && ents[i]->d_name[0] != '.'
            && core_accepts(ents[i]->d_name)) {
            snprintf(out, outsz, "%s/%s", dir, ents[i]->d_name);
            found = 0;
        }
        free(ents[i]);
    }
    free(ents);
    return found;
}

/* Fast mode: a fixed four emulated frames per panel frame.
 *
 * An adaptive version came before this one and was removed on purpose. It
 * protected the panel's 60 Hz by giving up emulated frames, which is exactly
 * the wrong trade for what fast mode is for - it made the picture smooth and
 * the game slower, when the whole point is the game being faster.
 *
 * Be clear about what this does and does not buy. The machine emulates about
 * 115 NES frames a second at the capped 1008 MHz OPP, so wall clock speed
 * tops out near 1.9x however many frames are asked for; raising the multiple
 * does not make the game faster, it spends the whole CPU on emulation and
 * lets the panel fall to whatever is left, around 29 fps. That is a choppier
 * picture for a real if smaller speed gain, which is the trade that was
 * asked for. The status line reports the multiple actually achieved so the
 * difference between four and what the silicon does is never a guess. */
#define FAST_EXTRA 5

/* The status bar down the side of a game.
 *
 * See scale.h for why it is exactly 53 columns wide.
 *
 * Two things at the ends and nothing in between. The battery is the only
 * reading a handheld cannot give you any other way; the name is at the foot
 * of the rail turned on its side, the way it would be printed on the bezel of
 * a machine you could buy. The emptiness between them is the composition, not
 * a gap waiting to be filled - an earlier version put a session timer there
 * purely because the space existed.
 *
 * Widths are chosen odd wherever the element allows it, so (BAR_W - w) / 2
 * lands on whole pixel 26: at this size an element one pixel off centre is
 * visible, and several elements each off by a different amount is what makes
 * a layout look accidental rather than merely imperfect. The cell, its cap
 * and the chevrons are exact. The number and the wordmark are even widths at
 * this scale and sit half a pixel left, which is the closest a 53 column rail
 * can put them and is below what the panel resolves.
 *
 * No wall clock, and no volume. The RTC has never been set - the console
 * writes files onto the card dated 2017 - and volume is a potentiometer in
 * the analog path with nothing for software to read. Showing either would
 * mean showing a number we made up. */

#define BAR_INK   RGB565(214, 220, 228)
#define BAR_EDGE  RGB565( 72,  78,  88)
#define BAR_MARK  RGB565( 52,  58,  68)
#define BAR_WARN  RGB565(240, 176,  64)
#define BAR_CRIT  RGB565(236,  84,  68)
#define BAR_CHRG  RGB565( 96, 170, 240)

/* Odd, so it centres exactly. Sized to be read at a glance and no larger:
 * this sits beside the game for hours and is not the subject. */
#define CELL_W 21
#define CELL_H 46
#define CELL_X ((BAR_W - CELL_W) / 2)
#define CELL_Y 18

static px_t barbuf[BAR_W * PANEL_H];

static void draw_bar(uint32_t held)
{
    canvas_t c = { barbuf, BAR_W, PANEL_H };
    static struct sysinfo_s si;
    static uint64_t next_read;
    char buf[8];

    /* Five sysfs files at 60 Hz, to animate a figure that moves once a
     * minute, is precisely what a battery-first machine should not do. */
    uint64_t now = plat_now_us();
    if (now >= next_read) {
        sysinfo_read(&si);
        next_read = now + 5000000;
    }

    int pct = si.capacity < 0 ? -1 : (int)si.capacity;
    if (pct > 100)
        pct = 100;

    /* Colour means one thing: something needs attention. A gauge that is
     * green when nothing is wrong has spent its loudest colour on the most
     * common case. */
    px_t ink = pct < 0           ? BAR_EDGE
             : si.current_ua > 0 ? BAR_CHRG
             : pct <= 12         ? BAR_CRIT
             : pct <= 30         ? BAR_WARN
                                 : BAR_INK;

    gfx_rect(&c, 0, 0, BAR_W, PANEL_H, RGB565(0, 0, 0));

    /* Cap, body, level. Drawn as a battery so that nothing has to say so. */
    gfx_rect(&c, (BAR_W - 9) / 2, CELL_Y - 4, 9, 4, BAR_EDGE);
    gfx_frame(&c, CELL_X, CELL_Y, CELL_W, CELL_H, BAR_EDGE);
    if (pct >= 0) {
        int iw = CELL_W - 6, ih = CELL_H - 6;
        int fh = ih * pct / 100;
        gfx_rect(&c, CELL_X + 3, CELL_Y + 3 + (ih - fh), iw, fh, ink);
    }

    if (pct >= 0)
        snprintf(buf, sizeof buf, "%d", pct);
    else
        snprintf(buf, sizeof buf, "--");
    /* gfx_text_w counts the advance after the last glyph; the ink stops one
     * scale short of that, and centring on the advance is what left the
     * number sitting visibly left of everything above it. */
    int tw = gfx_text_w(buf, 2) - 2;
    gfx_text(&c, (BAR_W - tw) / 2, CELL_Y + CELL_H + 10, buf, 2, ink);

    /* Fast mode is held rather than toggled and makes the picture choppy by
     * design - unmarked, that reads as the machine struggling rather than as
     * the button working. Two chevrons, 8 and 8 with 3 between: 19 across,
     * which is odd and so lands on centre like everything else. */
    if (held & PAD_R2) {
        for (int i = 0; i < 2; i++) {
            int x = (BAR_W - 19) / 2 + i * 11;
            for (int k = 0; k < 8; k++)
                gfx_rect(&c, x + k, 150 + k, 1, 15 - 2 * k, BAR_INK);
        }
    }

    /* The name, up the foot of the rail. Dim: it is an identity, not a
     * reading, and it should be the last thing the eye stops on. */
    gfx_text_rot(&c, (BAR_W - FONT_H * 2) / 2, PANEL_H - 14, "PID351",
                 2, BAR_MARK);
}

/* Runs one ROM and nothing else: no demo, no sweep, no census.
 *
 * This is not the frontend and is not going to grow into one - it exists so a
 * core can be proven end to end before there is any menu to reach it
 * through, which is the same order the display and the pad were brought up
 * in. The frontend replaces the ROM argument, not this loop. */

/* Session counters. File scope rather than parameters because there is one
 * session per boot by construction, and the alternative was a fifteen
 * argument function. */
static unsigned n_save, n_load, n_undo;
static unsigned fast_panel, fast_emu, late_frames;
/* Wall clock spent in fast mode. Without it the speed multiple has to assume
 * fast frames are presented at the panel rate, and they are emphatically not:
 * a 6x frame runs the core six times and takes 54 ms, so the panel drops to
 * about 19 fps while it is held. The first version of this report made that
 * assumption and printed 5.99x for something delivering 1.97x. */
static uint64_t fast_us;
static int ring_lo = INT32_MAX, ring_hi;
static int aud_lo = INT32_MAX;   /* codec buffer low water */
static long temp_hi = -300000;
static int lat_bounded;          /* latency came from the bound, not the flip */
/* Frames the core returned no picture for, and frames the panel refreshed
 * without one of ours. Neither is visible anywhere else in this report: a
 * null frame still meets its deadline, and a repeated refresh still completes
 * its flip, so both are silent stutters unless they are counted here. */
static unsigned null_frames, input_frames;
static unsigned seq_repeat[2], seq_same, seq_jump;
/* Which frames the normal-mode repeats landed on. Eight numbers is enough to
 * say whether they cluster on the savestates, on the fast-mode boundaries or
 * nowhere in particular, and that was the one question the last session's
 * count could not answer about itself. */
#define NSEQ_WHEN 8
static unsigned seq_when[NSEQ_WHEN], seq_when_n;
static unsigned pad_press[16], pad_frames[16];
static const char *const pad_name[16] = {
    "A", "B", "X", "Y", "up", "down", "left", "right",
    "L1", "R1", "L2", "R2", "select", "start", "L3", "R3"
};
static struct sysinfo_s si_start;

/* The whole session, analysed on the device, at exit.
 *
 * On the device because the alternative is a log of numbers and a laptop to
 * turn them into an answer, and every time that has happened here the answer
 * arrived a session late. The machine has the data and nothing else to do
 * with the last twenty milliseconds before it powers off.
 *
 * The verdict lines at the end are deliberately stated as thresholds that
 * pass or fail rather than as values to be interpreted. A number invites the
 * reader to decide what it means; the point of writing them down in advance
 * is that the criterion was fixed before the measurement. */
static void tele_verdict(const char *reason, unsigned frames, unsigned emu,
                         uint64_t start)
{
    struct sysinfo_s si;
    double secs = (double)(plat_now_us() - start) / 1000000.0;
    unsigned normal = frames > fast_panel ? frames - fast_panel : 0;
    double core_hz = (double)core_fps_milli() / 1000.0;

    sysinfo_read(&si);
    if (si.temp_mc > temp_hi)
        temp_hi = si.temp_mc;

    printf("\npid351: ==== session report ====\n");
    printf("pid351: exit (%s) %.1f s, %u panel frames, %u emulated\n",
           reason, secs, frames, emu);
    printf("pid351: normal frames (%u of %u, budget %u us):\n",
           hist[0][H_EMU].n, frames, (unsigned)FRAME_US);
    hist_report(0, H_EMU,   FRAME_US);
    hist_report(0, H_SCALE, FRAME_US);
    hist_report(0, H_BLIT,  FRAME_US);
    hist_report(0, H_BUSY,  FRAME_US);
    hist_report(0, H_AUD,   FRAME_US);
    hist_report(0, H_WAIT,  FRAME_US);
    hist_report(0, H_LAT,   FRAME_US);
    hist_report(0, H_DEAD,  FRAME_US);
    hist_report(0, H_PERIOD, FRAME_US);
    hist_report(0, H_FLIP,  FRAME_US);
    /* Spelled out because a latency figure with an unstated boundary is worth
     * nothing and every published one draws it somewhere else. This one runs
     * from the pad read to the vblank that latched the frame, taken from the
     * flip event's own timestamp; the panel's scan is the line after it.
     *
     * deadtime is the part of that spent with the frame finished and queued,
     * waiting for a vblank - the only part that could be given back, by
     * starting the frame later rather than by making it faster. */
    printf("pid351:   latency is pad read to the latching vblank%s; the panel "
           "then paints it over %u us more, none of that at the first pixel "
           "scanned and all of it at the last\n",
           lat_bounded ? " (BOUND - no flip timestamp)" : "",
           (unsigned)FRAME_US);
    /* Both buffers between us and a speaker, at the two ends of the same
     * chain: the core's ring is what we have produced and not yet handed
     * over, the codec's is what the hardware has and not yet played. The
     * first one to reach zero is the one that clicks. */
    /* The margin, rather than the typical gap. deadtime says how long a
     * finished frame sat waiting for its vblank; the smallest one in the
     * session is how close the loop came to missing, and it is the number
     * that says whether the twenty percent of headroom is really there. */
    if (hist[0][H_DEAD].n)
        printf("pid351:   closest call %u us of margin before a normal frame "
               "would have missed its vblank\n",
               hist_pct(&hist[0][H_DEAD], 0));
    printf("pid351: buffers:\n");
    hist_report_lo(H_RING,  "frames");
    hist_report_lo(H_CODEC, "frames");
    printf("pid351: frames: %u produced no picture, %u had a control held "
           "(%.1f%%)\n", null_frames, input_frames,
           frames ? (double)input_frames * 100.0 / (double)frames : 0.0);
    printf("pid351: panel: %u refreshes with no new frame in normal play "
           "(%.3f%% of %u), %u in fast mode (of %u, %.1f expected), %u flips "
           "on one vblank, %u sequence jumps\n",
           seq_repeat[0],
           normal ? (double)seq_repeat[0] * 100.0 / (double)normal : 0.0,
           normal, seq_repeat[1], fast_panel,
           (double)fast_us / (double)FRAME_US - (double)fast_panel,
           seq_same, seq_jump);
    if (seq_when_n) {
        printf("pid351: panel: normal-play repeats on frame");
        for (unsigned i = 0; i < seq_when_n; i++)
            printf(" %u", seq_when[i]);
        printf("%s\n", seq_repeat[0] > seq_when_n ? " ..." : "");
    }
    {
        int any = 0;

        printf("pid351: input (presses/frames held):");
        for (int b = 0; b < 16; b++)
            if (pad_press[b]) {
                printf(" %s %u/%u", pad_name[b], pad_press[b], pad_frames[b]);
                any = 1;
            }
        printf("%s\n", any ? "" : " nothing was pressed");
    }
    if (fast_panel) {
        printf("pid351: fast frames (%u, each runs the core %d times):\n",
               fast_panel, FAST_EXTRA + 1);
        hist_report(1, H_EMU,  FRAME_US);
        hist_report(1, H_BUSY, FRAME_US);
        hist_report(1, H_AUD,   FRAME_US);
        hist_report(1, H_LAT,   FRAME_US);
        hist_report(1, H_PERIOD, FRAME_US);
    }

    printf("pid351: pacing: %u over budget of %u normal frames (%.3f%%), "
           "%.4f fps against %.4f panel\n",
           late_frames, normal,
           normal ? (double)late_frames * 100.0 / (double)normal : 0.0,
           secs > 0 ? (double)frames / secs : 0.0,
           1000000.0 / (double)FRAME_US);

    if (fast_panel && fast_us > 0) {
        double fsec = (double)fast_us / 1000000.0;
        double efps = (double)(fast_emu + fast_panel) / fsec;

        /* Two different numbers that both deserve to be called the multiple.
         * Per frame is what was asked for and is exact by construction. Wall
         * clock is how much faster the game actually gets, and is the only
         * one that answers "is fast mode fast enough" - they differ by 3x
         * here because the machine is emulation bound. */
        printf("pid351: fast mode: %.1f s, %u panel frames, %u emulated, "
               "%.2fx per frame, %.1f emu fps = %.2fx wall clock\n",
               fsec, fast_panel, fast_emu + fast_panel,
               (double)(fast_emu + fast_panel) / (double)fast_panel,
               efps, core_hz > 0 ? efps / core_hz : 0.0);
        printf("pid351:   ceiling is %.2fx - emulation alone is %u us/frame "
               "at p50, so nothing above that is reachable\n",
               core_hz > 0 ? 1000000.0
                   / (double)tele_emu_p50() / core_hz : 0.0,
               tele_emu_p50());
    } else {
        printf("pid351: fast mode: never used\n");
    }

    /* Said plainly, because the alternative reads as health: a session where
     * the device never opened prints zero xruns, a codec low water of zero
     * and a core ring pinned at its cap, and every one of those is the
     * absence of audio rather than a measurement of it.
     *
     * Asked of the device and not of the low water mark, which was the first
     * attempt and was wrong twice over. The low water sampler ignores the
     * first three hundred frames, so it is also unset after any session
     * shorter than five seconds - and it duly accused a perfectly healthy
     * two-second boot of having no audio at all. */
    if (aud_rate() <= 0)
        printf("pid351: audio: NEVER OPENED - the ring figure below is the "
               "core talking to nothing\n");
    printf("pid351: audio: %d xruns, core ring %d..%d, codec low water %s%d "
           "frames (%.1f ms)\n",
           aud_xruns(), ring_lo == INT32_MAX ? 0 : ring_lo, ring_hi,
           aud_lo == INT32_MAX ? "never sampled, " : "",
           aud_lo == INT32_MAX ? 0 : aud_lo,
           aud_lo == INT32_MAX ? 0.0
               : (double)aud_lo * 1000.0 / (double)(aud_rate() ? aud_rate()
                                                              : 48000));
    printf("pid351: state: %u saved, %u loaded, %u undone; last save %u us "
           "on the frame, published %d frames later in %u us\n",
           n_save, n_load, n_undo, core_state_save_us(),
           SAVE_SETTLE_FRAMES, core_state_rename_us());
    printf("pid351: power: backlight %ld/%ld, governor %s, %ld MHz\n",
           si.backlight, si.backlight_max, si.governor, si.cpu_khz / 1000);
    printf("pid351: power: %ld%% -> %ld%%, %ld -> %ld uA, %ld MHz, "
           "temp %ld.%01ld -> %ld.%01ld peak %ld.%01ld C\n",
           si_start.capacity, si.capacity, si_start.current_ua, si.current_ua,
           si.cpu_khz / 1000,
           si_start.temp_mc / 1000, (si_start.temp_mc % 1000) / 100,
           si.temp_mc / 1000, (si.temp_mc % 1000) / 100,
           temp_hi / 1000, (temp_hi % 1000) / 100);
    if (si_start.charge_uah > 0 && si.charge_uah > 0 && secs > 30.0) {
        double mA = (double)(si_start.charge_uah - si.charge_uah) / secs
                    * 3600.0 / 1000.0;
        double full = (double)(si.charge_full_uah > 0 ? si.charge_full_uah
                                                     : 3450000) / 1000.0;

        /* Projected here rather than worked out later, because the two things
         * it needs - the drain over a real session and this pack's actual
         * charge_full - are both only available on the device, and every time
         * that arithmetic has been done off it, it has been done against the
         * design capacity instead of the battery that is fitted. */
        printf("pid351: drain: %ld uAh over %.1f s = %.0f mA, "
               "%.0f mAh pack -> %.1f h from full, %.1f h left at %ld%%\n",
               si_start.charge_uah - si.charge_uah, secs, mA, full,
               mA > 0 ? full / mA : 0.0,
               mA > 0 ? full * (double)si.capacity / 100.0 / mA : 0.0,
               si.capacity);
    }

    /* The machine's half of the session, printed before the verdicts so the
     * verdicts stay the last thing in the log. */
    tele_report();

    /* Thresholds fixed in advance. See the comment above. */
    printf("pid351: verdict:\n");
    printf("pid351:   pacing   %s  (late frames under 0.1%%)\n",
           normal && (double)late_frames / (double)normal < 0.001
               ? "PASS" : "FAIL");
    printf("pid351:   audio    %s  (no xrun after the first 5 s)\n",
           aud_xruns() <= 1 ? "PASS" : "FAIL");
    printf("pid351:   ring     %s  (never starved)\n",
           ring_lo != INT32_MAX && ring_lo > 0 ? "PASS" : "FAIL");
    printf("pid351:   thermal  %s  (peak under 70 C)\n",
           temp_hi < 70000 ? "PASS" : "FAIL");
    fflush(stdout);
}

static int run_game(const char *rom, int as_init)
{
    if (plat_init() != 0) {
        backlight(BL_ON);
        fprintf(stderr, "%spid351: platform init failed\n",
            plat_is_init() ? "<3>" : "");
        plat_boot_save_log("pid351-fail.log");
        plat_boot_shutdown(1);
        return 1;
    }
    tele_boot("display");
    uint64_t splashed = plat_now_us();
    splash("LOADING");
    tele_boot("splash");
    if (aud_open() != 0)
        printf("pid351: WARN continuing without audio\n");
    tele_boot(aud_rate() > 0 ? "audio" : "no audio");
    if (core_open(rom) != 0) {
        /* Returning from main as PID 1 is a kernel panic, and panic=5 turns
         * that into a reboot loop - so the one failure where a post-mortem
         * matters most would be the one that never writes one. Power off
         * rather than restart, for the same reason: a machine sitting dark
         * with a log on its card can be diagnosed, and one rebooting into the
         * same failure every five seconds cannot even be read. */
        plat_shutdown();
        plat_boot_save_log("pid351-fail.log");
        plat_boot_shutdown(1);
        return 1;
    }

    /* Hold the splash. Asked for, and the request is better than it sounds:
     * loading takes about eight hundred milliseconds, which is long enough to
     * see something appear and too short to read it, so the machine went from
     * a bootloader to Mario via a flicker that looked like a fault.
     *
     * The wait is at the end rather than the beginning, so the loading
     * happens inside it and costs nothing extra - only the remainder is spent
     * waiting. Blocking, not spinning, per the rules.
     *
     * Nothing is played through it. Priming the codec with silence here was
     * tried, to bring the amplifier up behind the wordmark instead of under
     * the game, and it is gone again: it did not move the pop and it cost a
     * second xrun every boot - one in every session before it, two in the
     * session with it. An audible click traded for an inaudible one is a bad
     * trade even when the theory is sound, and this theory was not.
     *
     * The codec is prepared but not started while we sit here, because ALSA's
     * start threshold is half a buffer and nothing has written a frame yet,
     * so no underrun accrues. That sentence was written as an assumption and
     * was false for as long as it stood: aud_open primed the buffer with
     * exactly the threshold, which started the stream here rather than in the
     * loop, and it ran dry forty milliseconds later. The priming is gone and
     * the sentence is now true.
     *
     * This is also why the silence-priming experiment failed and why its
     * conclusion should not be trusted: it was feeding a stream that was
     * already dead, so it bought a second xrun instead of preventing the
     * first. If the pop survives this commit, that experiment is worth
     * running again on a stream that is actually alive. */
    tele_boot("core");
    plat_sleep_until(splashed + SPLASH_HOLD_US);
    tele_boot("hold");

    uint64_t start = plat_now_us(), next = start, mark = start;
    /* A drain curve rather than two endpoints. Over five minutes the gauge
     * barely moves and the endpoints are all there is; over an hour the shape
     * is the answer - a rate that climbs is the backlight or the governor,
     * one that holds flat is the workload, and the difference is invisible
     * from a start and an end. */
    uint64_t batt_mark = start;
    /* The pad read and the flip queue of the iteration before this one; see
     * where they are used for why latency needs both. */
    uint64_t prev_f0 = 0, prev_queue = 0, prev_latch = 0, prev_flip = 0;
    uint64_t prev_iter = 0;
    uint32_t prev_seq = 0;
    int prev_fast = 0, have_seq = 0;
    unsigned frames = 0, win = 0, emu = 0;
    unsigned emu_total = 0;
    uint32_t blit_hi = 0, scale_hi = 0;
    const char *reason = "?";
    uint32_t was = 0;
    /* Battery and temperature at both ends of the session. The exchange rate
     * in docs/hardware.md was measured with synthetic load; this asks the
     * same question of the workload that actually runs. */
    sysinfo_read(&si_start);
    tele_boot("loop");

    for (;;) {
        uint32_t held = plat_input();
        /* Edges, not levels: a state written every frame the button is down
         * would hammer the card and stutter the game. */
        uint32_t hit = held & ~was;
        was = held;

        /* Which of the sixteen controls the session actually used, as edges
         * and as frames held. Sixteen tests a frame is nothing next to a
         * blit, and without it the report cannot say whether a control was
         * exercised at all - which is the first question asked of every input
         * bug this machine has had. */
        for (int b = 0; b < 16; b++) {
            if (hit & (1u << b))
                pad_press[b]++;
            if (held & (1u << b))
                pad_frames[b]++;
        }
        if (held)
            input_frames++;

        if (hit & PAD_L2) {
            printf("pid351: save %s\n", core_state_save() == 0 ? "ok" : "FAILED");
            n_save++;
        }
        if (hit & PAD_L3) {
            printf("pid351: load %s\n", core_state_load() == 0 ? "ok" : "none");
            n_load++;
        }
        /* Undo, on the one control left that no console can ever want. It
         * restores whatever was running immediately before the last load,
         * which is the only way back from pressing load by accident - and
         * core_state_undo has existed and been unreachable since the day
         * savestates went in, which made an accidental load the one action on
         * this machine that could destroy something and not be taken back. */
        if (hit & PAD_R3) {
            printf("pid351: undo %s\n", core_state_undo() == 0 ? "ok" : "none");
            n_undo++;
        }
        /* The codec level at the moment fast mode starts is the whole story
         * of whether it survives the transition, so it is in the log rather
         * than inferred from a five-second window that straddles it. */
        if (hit & PAD_R2)
            printf("pid351: fast on, codec %d frames, ring %d\n",
                   aud_level(), core_audio_level());
        if ((held & PAD_START) && (held & PAD_SELECT)) { reason = "combo"; break; }
        if (plat_should_quit())                        { reason = "quit";  break; }

        /* R2 rather than a stick click: no console we target has a second
         * pair of shoulders, so R2 is as safe as L3/R3 and, unlike a stick
         * click, comfortable to hold down while still playing.
         *
         * Elapsed rather than a deadline, so that a loop already running late
         * still gets its extra frames instead of silently doing nothing at
         * exactly the moment speed was asked for. */
        struct tele_frame tf;
        memset(&tf, 0, sizeof tf);
        uint64_t f0 = plat_now_us();
        int fast = (held & PAD_R2) != 0;

        /* Top of frame to top of frame. Every other timing here measures a
         * piece of the loop; this measures the loop, including the sleep and
         * anything that happened outside our own brackets, so a period that
         * does not match the sum of the pieces is time the machine spent
         * somewhere we are not looking. */
        if (prev_iter && f0 > prev_iter)
            hist_add(&hist[fast][H_PERIOD], (uint32_t)(f0 - prev_iter));
        prev_iter = f0;

        if (fast) {
            /* Before the six frames, not after them. aud_silence fills the
             * codec to 74 ms and a fast frame takes 52, so fast mode looked
             * safe by a wide margin - but it ran from core_audio, at the end
             * of the frame, and the *first* fast frame therefore started from
             * whatever normal play had left in the buffer. That is 40 ms at
             * p50, so entering fast mode ran the codec dry before its own
             * protection had executed once, and the log shows exactly that:
             * one xrun at the transition, then fifteen clean seconds.
             *
             * The call inside core_audio stays. It is a no-op when the buffer
             * is already full, and it is what keeps the later frames fed. */
            aud_silence();
            for (int i = 0; i < FAST_EXTRA; i++) {
                core_skip(held);
                emu++;
                emu_total++;
                fast_emu++;
            }
            fast_panel++;
        }

        int w = 0, h = 0;
        const px_t *fb = core_run(held, &w, &h);
        /* Emulation is the largest single cost in the frame and was the only
         * one never measured - every statement about it so far was solved for
         * from the others, which is how it came to be quoted as 8.7 ms when
         * it is nearer 7.5. */
        tf.emu_us = (uint32_t)(plat_now_us() - f0);
        /* After the run, because the core produces this frame's samples
         * during it, and before the present, because the present blocks on
         * vblank and the codec should not be waiting through that. */
        /* Sampled here, immediately before the top-up rather than after it.
         * The first version read aud_level once per frame after core_audio
         * had already refilled the buffer, which measures the fill target and
         * not the trough - it reported 34 ms of margin through a session
         * where five saves each drained the buffer to an xrun. */
        int al = aud_level();
        if (al >= 0 && frames > 300 && al < aud_lo)
            aud_lo = al;
        if (al >= 0)
            hist_add(&hist[0][H_CODEC], (uint32_t)al);
        uint64_t a0 = plat_now_us();
        core_audio();
        tf.aud_us = (uint32_t)(plat_now_us() - a0);
        if (fb) {
            draw_bar(held);
            plat_present(fb, w, h, barbuf);
        } else {
            null_frames++;
        }

        /* Worst case over the window, not the mean: the question this
         * answers is whether presenting a frame ever fails to fit in the
         * panel period, and an average cannot say. */
        uint32_t pb = 0, ps = 0, pw = 0;
        plat_frame_us(&pb, &pw, &ps);
        if (pb > blit_hi)  blit_hi  = pb;
        if (ps > scale_hi) scale_hi = ps;
        tf.scale_us = ps;
        tf.blit_us  = pb;
        tf.wait_us  = pw;
        tf.work_us  = (uint32_t)(plat_now_us() - f0);
        /* Both places the loop blocks come off. Whatever is left is work
         * this machine actually has to do, and only that can be late. */
        uint32_t idle = tf.wait_us + tf.aud_us;
        tf.busy_us  = tf.work_us > idle ? tf.work_us - idle : 0;
        tf.fast = (uint32_t)fast;
        if (fast)
            fast_us += tf.work_us;
        if (!fast && tf.busy_us > FRAME_US)
            late_frames++;
        tele_add(&tf);

        /* Input latency, measured rather than bounded.
         *
         * The flip queued at the end of this iteration is not latched until
         * the next vblank, and the loop learns when that was from the flip
         * event the following iteration reads - so the pad press it answers
         * is one iteration back, and the pair is carried across rather than
         * guessed at. The first version assumed a whole panel period between
         * queueing and latching and reported the bound as though it were the
         * measurement, which overstated latency by however much of the period
         * was already gone.
         *
         * Attributed to the frame the pad was read on, so a fast frame's
         * latency does not land in the normal distribution. */
        uint64_t latch = plat_flip_us();
        if (latch && latch != prev_latch) {
            prev_latch = latch;

            /* The panel's own clock, sampled at the only place it is
             * observable. Two independent readings of the same event: the
             * interval says how far apart two refreshes were, the sequence
             * says how many refreshes happened in between. A frame the panel
             * showed twice moves the second and not the first, which is why
             * counting the gap is worth the lines.
             *
             * Not seeded from the flip the loop finds already waiting for it.
             * That one is the splash's, from before the two-second hold, so
             * the first interval measured against it spanned the hold: 2.0 s
             * of flip and a hundred and twenty phantom repeats, which was
             * sixteen percent of the count. Excluded exactly - by when the
             * flip happened - rather than by a plausibility threshold, since
             * a threshold would also throw away the real drops it is there to
             * find. */
            if (latch >= start) {
                uint32_t seq  = plat_flip_seq();
                uint32_t step = seq - prev_seq;   /* wraps at 2^32 */

                if (have_seq) {
                    if (latch > prev_flip)
                        hist_add(&hist[prev_fast][H_FLIP],
                                 (uint32_t)(latch - prev_flip));
                    /* Split by mode, because the two are different facts. A
                     * repeat in fast mode is arithmetic - a 52 ms frame on a
                     * 16.7 ms panel must repeat twice - and a repeat in
                     * normal play is a dropped frame. Summed together the
                     * first buries the second. */
                    if (step == 0)
                        seq_same++;
                    else if (step > 1 && step < 1000) {
                        seq_repeat[prev_fast ? 1 : 0] += step - 1;
                        if (!prev_fast && seq_when_n < NSEQ_WHEN)
                            seq_when[seq_when_n++] = frames;
                    } else if (step >= 1000) {
                        seq_jump++;
                    }
                }
                prev_seq  = seq;
                prev_flip = latch;
                have_seq  = 1;
            }
            if (prev_f0 && latch > prev_f0 && latch - prev_f0 < 500000) {
                hist_add(&hist[prev_fast][H_LAT],
                         (uint32_t)(latch - prev_f0));
                if (latch > prev_queue)
                    hist_add(&hist[prev_fast][H_DEAD],
                             (uint32_t)(latch - prev_queue));
            }
        } else if (!latch) {
            /* No timestamp from this backend: fall back to the bound, and
             * say so in the report rather than let the two be confused. */
            lat_bounded = 1;
            hist_add(&hist[fast][H_LAT],
                     (tf.work_us > tf.blit_us ? tf.work_us - tf.blit_us : 0)
                     + FRAME_US);
        }
        prev_f0    = f0;
        prev_queue = plat_now_us();
        prev_fast  = fast;

        int rl = core_audio_level();
        if (rl >= 0) {
            if (rl < ring_lo) ring_lo = rl;
            if (rl > ring_hi) ring_hi = rl;
            hist_add(&hist[0][H_RING], (uint32_t)rl);
        }
        core_state_tick();

        frames++;
        win++;
        emu++;
        emu_total++;
        uint64_t now = plat_now_us();
        /* Once a frame by contract, once a second in practice - see tele.h.
         * After the frame's work rather than before, so the reads land in the
         * part of the period we are about to sleep through anyway. */
        tele_sample(now);

        if (now - mark >= 5000000) {
            double secs = (double)(now - mark) / 1000000.0;
            /* Once per window, alongside a print that already costs more.
             * The peak matters more than either endpoint: thermal throttling
             * would show up as late frames long after the temperature that
             * caused it had fallen back. */
            struct sysinfo_s sw;
            sysinfo_read(&sw);
            if (sw.temp_mc > temp_hi)
                temp_hi = sw.temp_mc;
            if (now - batt_mark >= 60000000) {
                batt_mark = now;
                printf("pid351: batt t=%.0fs %ld%% %ld uAh %ld uA "
                       "%ld.%01ld C %ld MHz\n",
                       (double)(now - start) / 1000000.0, sw.capacity,
                       sw.charge_uah, sw.current_ua,
                       sw.temp_mc / 1000, (sw.temp_mc % 1000) / 100,
                       sw.cpu_khz / 1000);
            }
            /* Emulated frames as well as panel frames: in fast mode they
             * differ, and the ratio is the only honest answer to how fast
             * fast mode actually is on this machine. */
            printf("pid351: %s %.2f fps  emu %.2f (%.2fx)  frames=%u  "
                   "ring=%d  aud=%d x%d  scale=%u blit=%u us\n",
                   core_name(), (double)win / secs, (double)emu / secs,
                   (double)emu / (double)win,
                   frames, core_audio_level(), aud_level(), aud_xruns(),
                   scale_hi, blit_hi);
            fflush(stdout);
            win = 0;
            emu = 0;
            mark = now;
            blit_hi = 0;
            scale_hi = 0;
        }

        next += FRAME_US;
        plat_sleep_until(next);
    }

    tele_boot("exit");
    tele_verdict(reason, frames, emu_total, start);
    fflush(stdout);
    /* The shutdown is staged for the same reason the boot is: it is the half
     * of the session nobody watches, it is where the pop and the fbcon flash
     * both came from, and a stage that hangs here would otherwise be a
     * machine that simply never powers off. */
    core_close();
    tele_boot("core out");
    aud_close();
    tele_boot("audio out");
    /* Before plat_shutdown, which drops DRM master and lets fbcon restore
     * itself onto the panel. Whatever it restores - and it has been the
     * bootloader's leftovers both times anyone looked - is not something to
     * show on the way out. Dark first, then let go. */
    backlight(0);
    tele_boot("dark");
    plat_shutdown();
    tele_boot("drm out");
    if (as_init) {
        /* The last line that can possibly be in the log, since the next call
         * is the one that copies the ring buffer to the card. Everything
         * after it - the sync, the poweroff - is only observable by the
         * machine coming back up. */
        tele_boot("log");
        plat_boot_save_log("pid351-boot.log");
        plat_boot_shutdown(1);
    }
    return 0;
}

int main(int argc, char **argv)
{
    /* Before anything else, including the mounts - so the clock every later
     * stage is measured against starts at the first instruction of userspace
     * rather than at the first one that had somewhere to print. This line
     * itself goes nowhere as PID 1, which is the price of the origin being
     * honest; the stage is still in the timeline at exit. */
    tele_boot("entry");

    /* First, because everything below assumes /dev, /proc and /sys exist,
     * and as PID 1 none of them do. No-op when we are not PID 1. */
    int as_init = plat_boot_init();
    tele_boot(as_init ? "mounts" : "user");

    if (as_init)
        printf("pid351: running as PID 1\n");
    fflush(stdout);

    /* A ROM on the command line skips the demo entirely. */
    if (argc > 1)
        return run_game(argv[1], as_init);

    /* As PID 1 there is no command line, so the ROM comes off the card. One
     * fixed directory and the first file any core claims - not a launcher,
     * and not pretending to be one: it is the smallest thing that makes the
     * console play a game, and the launcher replaces it rather than growing
     * out of it. The demo still runs when the directory is empty or absent,
     * which is what makes an SD card with no ROMs on it a working image
     * rather than a black screen. */
    if (as_init) {
        const char *boot = plat_boot_mount();
        char dir[256], rom[512];
        tele_boot(boot ? "card" : "no card");
        if (boot) {
            snprintf(dir, sizeof dir, "%s/%s", boot, ROM_DIR);
            if (first_rom(dir, rom, sizeof rom) == 0) {
                tele_boot("rom");
                printf("pid351: playing %s\n", rom);
                fflush(stdout);
                return run_game(rom, as_init);
            }
            printf("pid351: no ROM in %s, running the demo\n", dir);
            fflush(stdout);
        }
    }

    if (plat_init() != 0) {
        backlight(BL_ON);
        fprintf(stderr, "%spid351: platform init failed\n",
            plat_is_init() ? "<3>" : "");
        /* A failed display bring-up is exactly the case where the screen
         * cannot tell us anything, so the kernel log has to. */
        plat_boot_save_log("pid351-fail.log");
        plat_boot_shutdown(0);
        return 1;
    }

    canvas_t c = { framebuffer, PANEL_W, PANEL_H };

    /* Present once before anything that can block. SDL does not map the
     * window until the first flip, so a stall anywhere between here and the
     * frame loop shows no window at all rather than a frozen one - and "no
     * window" reads as "the program never started", which sent the wrong
     * question to the wrong subsystem the first time it happened. A frozen
     * splash says the display came up and something after it did not.
     *
     * The same argument holds on the device with more force: the panel keeps
     * whatever the bootloader left there until we flip, so without this a
     * hang looks identical to a machine that never got as far as us. */
    splash("STARTING");

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

    /* Kept as a standing check, not an open question: the mode arithmetic is
     * exact and the flip rate should agree with it to within the alignment
     * error of however many flips we time. A disagreement larger than that
     * would mean the VOP is not running the mode it reports. */
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

    /* Last chance to ask the hardware whether it can rotate for us. If a
     * plane carries a rotation property that takes 90 degrees, everything the
     * blit work above achieved was unnecessary and we should know that before
     * building an image around it. */
    printf("pid351: display properties\n");
    plat_dump_props();

    bench_blit();
    sysinfo_read(&si);
    conditions("after bench ", &si);

    /* Before the sweep, because a sweep with no power numbers is 3.5 minutes
     * of nothing and we would rather know why up front. */
    census();

    /* The OPP sweep. This is the first measurement in the project that could
     * not have been taken on ROCKNIX at all: their device tree deletes every
     * operating point below 1008 MHz, so 816 and 600 simply did not exist as
     * things the machine could be asked to do.
     *
     * 1296 is measured first and last. Identical conditions at the two ends
     * bound the drift over the whole sweep, which is the only way to know
     * whether a 10 mA difference in the middle means anything. */
    long bl0 = si.backlight;

    /* The sweep exists to measure current at each operating point, so with no
     * fuel gauge it is 3.5 minutes producing -1 six times. That matters on the
     * laptop and not on the device: the laptop is the loop everything else is
     * developed in, and PLAN.md values it at about two seconds. Gated on the
     * gauge rather than on which backend is linked, because a device that has
     * somehow lost its gauge cannot produce this measurement either, and
     * would be better off reaching the part of the demo that still works. */
    if (si.current_ua < 0) {
        printf("pid351: no fuel gauge, skipping the OPP sweep - it has "
               "nothing to measure\n");
        fflush(stdout);
    } else {
        static const long khz[] = { 1296000, 1200000, 1008000, 816000,
                                    600000, 1296000 };
        static const char *const label[] = { "1296", "1200", "1008", "816",
                                             "600", "1296-again" };
        enum { NPHASE = (int)(sizeof khz / sizeof khz[0]) };
        struct phase_result pr[NPHASE];

        if (write_governor("performance") != 0)
            printf("pid351: WARN could not select performance governor\n");

        for (int i = 0; i < NPHASE; i++) {
            if (pin_freq(khz[i]) != 0)
                printf("pid351: WARN could not pin %ld kHz (does this DT "
                       "carry that OPP?)\n", khz[i]);
            power_phase(&c, label[i], NULL, 0, i == 0 ? (int)bl0 : -1,
                        35, &pr[i]);
        }

        printf("pid351: OPP SWEEP (current_avg uA, then charge-implied)\n");
        for (int i = 0; i < NPHASE; i++)
            printf("pid351:   %-11s %8ld   %8ld   fps=%.2f work_med=%u us\n",
                   label[i], pr[i].ua_avg, pr[i].ua_implied,
                   pr[i].fps, pr[i].work_med);
        printf("pid351:   trust current_avg over charge-implied: the coulomb "
               "counter only ticks every 6-7 s, which is four steps in a 29 s "
               "window and not enough to resolve 20 mA. The two 1296 phases "
               "bound the drift; anything smaller than their difference is "
               "noise, not a result.\n");
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

    /* After the sweep, not before it, for two independent reasons. The codec
     * draws current, and the sweep exists to measure current - leaving it
     * running would put a constant of unknown size into every phase. And
     * audio is only pumped from the frame loop, so an open stream would sit
     * unfed for the three and a half minutes the sweep takes and underrun,
     * which is exactly what the first run of this did.
     *
     * Not fatal either way. A handheld with no sound still runs games, and
     * the failure path prints what the card would have accepted - on a
     * machine with no serial port that is the only way the answer reaches
     * us. */
    if (aud_open() != 0)
        printf("pid351: WARN continuing without audio\n");
    fflush(stdout);

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

        /* Before any of the early exits below, and before the testcard
         * branch returns to the top: aud_due advances an accumulator, so a
         * frame that skips it silently steals those samples from the next
         * one and the drift it is there to prevent creeps back in. */
        audio_frame(held);

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
            /* An ioctl, so sampled with the other per-second statistics
             * rather than inside the frame it would be timing. */
            aud_lvl = aud_level();
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
            plat_present(framebuffer, src[src_i].w, src[src_i].h, NULL);

            uint32_t bb = 0, ww = 0;
            plat_frame_us(&bb, &ww, NULL);
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

        plat_present(framebuffer, PANEL_W, PANEL_H, NULL);

        uint32_t b_us = 0, w_us = 0;
        plat_frame_us(&b_us, &w_us, NULL);
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
    fflush(stdout);
    aud_close();
    backlight(0);
    plat_shutdown();

    /* Does not return when we are PID 1 - reaching the end of main as PID 1
     * is a kernel panic, so leaving has to be deliberate. */
    plat_boot_save_log("pid351-boot.log");
    plat_boot_shutdown(1);
    return 0;
}
