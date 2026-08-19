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
#include "ui.h"
#include "palette.h"

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


static px_t framebuffer[PANEL_W * PANEL_H];
/* The pillarbox beside an 8:7 picture, drawn separately because the blit
 * takes the game and the column as two sources rather than compositing them
 * into one buffer it would then have to read back. */
static px_t barbuf[BAR_W * PANEL_H];

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

/* Composed on the ink, not the cell. The wordmark and the line are centred
 * as one block: centring the wordmark on the panel and hanging the line off
 * it puts the visual weight above the middle, which is what "not quite
 * centred" looked like before anyone measured it.
 *
 * The rule under the wordmark is the width of the wordmark rather than of the
 * panel, so it reads as an underline and not as a divider between two halves
 * of nothing. */
static void splash(const char *line, int bright)
{
    canvas_t c = { framebuffer, PANEL_W, PANEL_H };
    const int mark = 4;
    const int sub = 2;
    const int gap = 14;
    int mw = gfx_ink_w("PID351", mark);
    int lw = gfx_ink_w(line, sub);
    int block = FONT_INK_H * mark + gap + 3 + gap + FONT_INK_H * sub;
    int y = (PANEL_H - block) / 2;

    gfx_rect(&c, 0, 0, PANEL_W, PANEL_H, UI_GROUND);
    gfx_text(&c, (PANEL_W - mw) / 2, y - FONT_INK_TOP * mark,
             "PID351", mark, UI_ACCENT);
    gfx_rect(&c, (PANEL_W - mw) / 2, y + FONT_INK_H * mark + gap, mw, 3,
             UI_EDGE);
    gfx_text(&c, (PANEL_W - lw) / 2,
             y + block - FONT_INK_H * sub - FONT_INK_TOP * sub, line, sub,
             UI_DIM);
    plat_present(framebuffer, PANEL_W, PANEL_H, NULL);
    /* After the flip, never before: the whole point of starting dark is that
     * the backlight comes up on our first frame and not on whatever was on
     * the panel beforehand. */
    ui_bright_apply(bright);
}

/* How many extra core frames a held R2 runs per panel frame.
 *
 * Be clear about what this does and does not buy. The machine emulates about
 * 115 NES frames a second, so wall clock speed tops out near 1.9x however
 * many frames are asked for; raising the multiple does not make the game
 * faster, it spends the whole CPU on emulation and lets the panel fall to
 * whatever is left. The session report quotes the multiple actually achieved
 * so the difference is never a guess. */
#define FAST_EXTRA 5

/* Where ROMs live on the card, relative to the FAT partition's root. A
 * compile-time constant because there is no config file and never will be
 * one; the card layout is as fixed as the hardware. */
#define ROM_DIR "pid351/roms"

/* The host build has no card to mount, so it reads the working directory's
 * roms/ - the same one tools/install-image.sh stages onto the card. Running
 * the list on a laptop is the only way to look at it without a reflash. */
#define ROM_DIR_HOST "roms"

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
/* Totals for the whole power-on rather than for one game. The report is a
 * statement about the machine, and a machine that played four games this
 * evening has one frame distribution, not four - the histograms were always
 * cumulative and these make the headline numbers agree with them. */
static unsigned session_frames, session_emu;
static uint64_t session_start;
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
static void tele_verdict(int games, uint64_t now)
{
    struct sysinfo_s si;
    unsigned frames = session_frames, emu = session_emu;
    double secs = (double)(now - session_start) / 1000000.0;
    unsigned normal = frames > fast_panel ? frames - fast_panel : 0;
    double core_hz = (double)core_fps_milli() / 1000.0;

    sysinfo_read(&si);
    if (si.temp_mc > temp_hi)
        temp_hi = si.temp_mc;

    printf("\npid351: ==== session report ====\n");
    printf("pid351: powered on %.1f s, %d game(s), %u panel frames, "
           "%u emulated\n", secs, games, frames, emu);
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

    /* Thresholds fixed in advance. See the comment above. Skipped entirely
     * when nothing was played: a power-on that went list, look, off has no
     * frames to judge, and printing FAIL against an empty measurement is how
     * a report teaches its reader to stop believing it. */
    if (!normal) {
        printf("pid351: verdict: nothing was played, nothing to judge\n");
        fflush(stdout);
        return;
    }
    printf("pid351: verdict:\n");
    printf("pid351:   pacing   %s  (late frames under 0.1%%)\n",
           (double)late_frames / (double)normal < 0.001 ? "PASS" : "FAIL");
    printf("pid351:   audio    %s  (no xrun after the first 5 s)\n",
           aud_xruns() <= 1 ? "PASS" : "FAIL");
    printf("pid351:   ring     %s  (never starved)\n",
           ring_lo != INT32_MAX && ring_lo > 0 ? "PASS" : "FAIL");
    printf("pid351:   thermal  %s  (peak under 70 C)\n",
           temp_hi < 70000 ? "PASS" : "FAIL");
    fflush(stdout);
}

/* One game, from load to the moment the player asks for the list back.
 *
 * It owns the core and nothing else. The display, the codec and the panel
 * belong to main and outlive it, which is what lets a game be swapped without
 * the audio path being torn down and rebuilt - and the audio path being torn
 * down and rebuilt is what the click at boot turned out to be.
 *
 * Returns 0 having put the player back where they came from, or -1 if the
 * core would not take the ROM, which is a fact about that file and not about
 * the machine: the list is still there and the other six still work. */
static int run_game(const struct ui_rom *game, struct ui_state *ui)
{
    if (core_open(game->path) != 0) {
        printf("pid351: %s would not load\n", game->name);
        fflush(stdout);
        return -1;
    }
    /* Resume, if there is anything to resume. One slot a game, written on the
     * way out, so the slot is not a snapshot the player has to remember to
     * take - it is simply where they were. */
    if (game->has_state) {
        printf("pid351: resume %s\n",
               core_state_load() == 0 ? "ok" : "FAILED");
        n_load++;
    }
    fflush(stdout);


    uint64_t start = plat_now_us(), next = start, mark = start;
    /* Until when the brightness readout is up in the side column. */
    uint64_t bright_until = 0;
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
    /* Seeded, for the same reason ui_list seeds it: A is still held from the
     * press that launched this game, and so is whatever shoulder was being
     * used a moment earlier. Zero would make all of them fresh presses on
     * frame 0 and step the backlight again on the way in. */
    uint32_t was = plat_input();
    /* Battery and temperature at both ends of the session. The exchange rate
     * in docs/hardware.md was measured with synthetic load; this asks the
     * same question of the workload that actually runs. */
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
        /* The shoulders, with no modifier. They were read every frame and
         * did nothing, and the backlight is both the largest power lever on
         * the machine and the only setting a person actually wants to change
         * while playing. */
        if (hit & PAD_L1) {
            if (ui->bright > 0)
                ui->bright--;
            ui_bright_apply(ui->bright);
            bright_until = plat_now_us() + 1200000u;
        }
        if (hit & PAD_R1) {
            if (ui->bright < UI_BRIGHT_STEPS - 1)
                ui->bright++;
            ui_bright_apply(ui->bright);
            bright_until = plat_now_us() + 1200000u;
        }
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
        if ((held & PAD_START) && (held & PAD_SELECT)) { reason = "list";  break; }
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
        uint64_t now_us = plat_now_us();
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
            ui_bar(barbuf, game->name,
                   now_us < bright_until ? ui->bright : -1);
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

    /* Where they were, written down before the core is torn down. This
     * overwrites the manual slot, and that is the whole point: there is one
     * state a game, it means "here", and the machine keeping it up to date is
     * better than the player remembering to. */
    printf("pid351: leaving %s after %.1f s, %u frames (%s); state %s\n",
           game->name, (double)(plat_now_us() - start) / 1000000.0, frames,
           reason, core_state_save() == 0 ? "written" : "FAILED");
    n_save++;
    core_state_sync();
    core_close();
    tele_boot("game out");
    fflush(stdout);
    session_frames += frames;
    session_emu += emu_total;
    return 0;
}

int main(int argc, char **argv)
{
    struct ui_rom roms[UI_MAX_ROMS];
    struct ui_state ui;
    const char *boot = NULL;
    char dir[256];
    uint64_t splashed;
    int n = 0, as_init, played = 0;
    struct ui_rom one;

    /* Before anything else, including the mounts - so the clock every later
     * stage is measured against starts at the first instruction of userspace
     * rather than at the first one that had somewhere to print. This line
     * itself goes nowhere as PID 1, which is the price of the origin being
     * honest; the stage is still in the timeline at exit. */
    tele_boot("entry");

    /* First, because everything below assumes /dev, /proc and /sys exist,
     * and as PID 1 none of them do. No-op when we are not PID 1. */
    as_init = plat_boot_init();
    tele_boot(as_init ? "mounts" : "user");
    if (as_init)
        printf("pid351: running as PID 1\n");
    fflush(stdout);

    if (as_init) {
        boot = plat_boot_mount();
        tele_boot(boot ? "card" : "no card");
    }
    ui_state_load(boot, &ui);

    if (plat_init() != 0) {
        /* Light the panel anyway. Whatever is on it is not ours, but a dark
         * screen and a broken one are the same object from the outside. */
        ui_bright_apply(ui.bright);
        fprintf(stderr, "%spid351: platform init failed\n",
                plat_is_init() ? "<3>" : "");
        plat_boot_save_log("pid351-fail.log");
        plat_boot_shutdown(1);
        return 1;
    }
    tele_boot("display");

    splashed = plat_now_us();
    splash("LOADING", ui.bright);
    tele_boot("splash");

    /* Opened once for the whole power-on and closed once at the end. Not per
     * game: tearing the codec down and building it back up between games
     * would run the stream-start path seven times an evening, and the stream
     * start is exactly what the boot click turned out to be. */
    if (aud_open() != 0)
        printf("pid351: WARN continuing without audio\n");
    tele_boot(aud_rate() > 0 ? "audio" : "no audio");

    /* A ROM on the command line skips the list entirely - the host's way in,
     * and the only reason argc is looked at at all. */
    if (argc > 1) {
        char st[600];

        memset(&one, 0, sizeof one);
        snprintf(one.path, sizeof one.path, "%s", argv[1]);
        snprintf(one.name, sizeof one.name, "%s", argv[1]);
        /* Same rule as the list's, so the two ways in resume the same game at
         * the same place rather than the command line always starting over. */
        snprintf(st, sizeof st, "%s.state", one.path);
        one.has_state = access(st, R_OK) == 0;
        n = -1;                  /* skip the list loop, go straight to exit */
    } else {
        /* With a card the ROMs are under its mount point; without one - the
         * host build always, the device only when the mount failed - "roms"
         * beside the binary. Both paths end in the list rather than in an
         * early exit, because a machine that switches itself off half a
         * second after you switch it on has told you nothing about why. The
         * list says there are no ROMs and waits for START+SELECT. */
        if (boot)
            snprintf(dir, sizeof dir, "%s/%s", boot, ROM_DIR);
        else
            snprintf(dir, sizeof dir, "%s", ROM_DIR_HOST);
        n = ui_scan(dir, roms, UI_MAX_ROMS);
        printf("pid351: %d ROM(s) under %s\n", n, dir);
        fflush(stdout);
        tele_boot("scan");
    }

    /* Hold the splash. Asked for, and the request is better than it sounds:
     * the machine is ready in under a second, which is long enough to see
     * something appear and too short to read it, so it went from a bootloader
     * to a game via a flicker that looked like a fault.
     *
     * The wait is at the end rather than the beginning, so everything above
     * happens inside it and costs nothing - only the remainder is spent
     * waiting, and it is spent blocking rather than spinning.
     *
     * Nothing is played through it. Priming the codec with silence here was
     * tried, to bring the amplifier up behind the wordmark instead of under
     * the game, and it is gone: it did not move the click and it cost a
     * second xrun every boot. The click was ours, in aud_open, and the fix
     * was deleting a call rather than adding one. */
    plat_sleep_until(splashed + SPLASH_HOLD_US);
    tele_boot("hold");

    /* The session's clock and its battery reading start here, after the hold
     * and before the first game, so the drain figure covers the machine doing
     * what it is for rather than the machine coming up. Both ways in are
     * below it, which is the reason the hold was hoisted out of them. */
    session_start = plat_now_us();
    sysinfo_read(&si_start);

    if (n < 0) {
        played++;
        run_game(&one, &ui);
    }

    /* The console proper. The list owns the machine between games; a game
     * owns it while it runs and hands it back on START+SELECT. Power off is
     * the list's decision alone, so quitting a game can never be one press
     * away from quitting the machine. */
    while (n >= 0) {
        int pick = ui_list(framebuffer, roms, n, &ui);

        if (pick < 0)
            break;
        tele_boot(played ? "game" : "first game");
        played++;
        if (run_game(&roms[pick], &ui) == 0)
            roms[pick].has_state = 1;
        ui_state_save(boot, &ui);
    }

    tele_boot("exit");
    tele_verdict(played, plat_now_us());
    fflush(stdout);
    ui_state_save(boot, &ui);
    aud_close();
    tele_boot("audio out");
    /* Before plat_shutdown, which drops DRM master and lets fbcon restore
     * itself onto the panel. Whatever it restores - and it has been the
     * bootloader's leftovers both times anyone looked - is not something to
     * show on the way out. Dark first, then let go. */
    ui_bright_off();
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
