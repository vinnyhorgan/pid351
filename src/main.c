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
static unsigned late_when[NSEQ_WHEN], late_when_n;
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
    if (late_when_n) {
        printf("pid351: pacing: over budget on frame");
        for (unsigned i = 0; i < late_when_n; i++)
            printf(" %u", late_when[i]);
        printf("%s\n", late_frames > late_when_n ? " ..." : "");
    }

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
        /* Not on the very first frame. The sample is taken before the
         * top-up, deliberately, to catch the trough - but on frame zero
         * nothing has been written to the codec at all, so it reads a
         * perfectly correct zero that then sits in the report as the
         * session's minimum and reads exactly like an xrun. */
        if (al >= 0 && frames > 0)
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
        if (!fast && tf.busy_us > FRAME_US) {
            /* Which frames, for the same reason the panel repeats are
             * located: one late frame in a short session fails a threshold
             * written for a long one, and "it was frame 1" and "it was frame
             * 4000" are not the same fact. */
            if (late_when_n < NSEQ_WHEN)
                late_when[late_when_n++] = frames;
            late_frames++;
        }
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
            }
        }
        /* No card, or a card with nothing on it. Said on the panel rather
         * than powered off into a black screen: "nothing on the card" and
         * "never booted at all" look identical from the outside, and telling
         * those two apart is the single thing this project has spent the most
         * time being unable to do. */
        printf("pid351: no ROM under %s\n", ROM_DIR);
        fflush(stdout);
        if (plat_init() == 0)
            splash("NO ROM");
        else
            backlight(BL_ON);
        plat_sleep_until(plat_now_us() + 5000000);
        backlight(0);
        plat_shutdown();
        plat_boot_save_log("pid351-boot.log");
        plat_boot_shutdown(1);
        return 1;
    }

    /* Not PID 1 and no argument: there is nothing to run. The demo that used
     * to live here - the census, the blit benchmark, the OPP sweep, the pad
     * and testcard screens - is gone. It existed to bring the platform up
     * blind, every one of its measurements is written into docs/hardware.md,
     * and a thousand lines kept for what they once proved is exactly the
     * weight this project says it will not carry. git has them. */
    fprintf(stderr, "pid351: usage: pid351 <rom>\n");
    return 1;
}

