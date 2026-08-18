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

static void info_line(canvas_t *c, int x, int y, const char *label,
                      const char *value, px_t value_col)
{
    gfx_text(c, x, y, label, 1, C_DIM);
    gfx_text(c, x + 66, y, value, 1, value_col);
}

static void draw_info(canvas_t *c, int x, int y, const struct sysinfo_s *s,
                      double fps, unsigned frames, uint32_t held)
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

    snprintf(v, sizeof v, "%04X", held);
    info_line(c, x, y, "BUTTONS", v, held ? C_LIT : C_DIM);        y += lh;

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

    uint64_t start = plat_now_us();
    uint64_t next  = start;
    uint64_t last_refresh = start;
    uint64_t fps_mark = start;
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
        }

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
        draw_info(&c, 252, 24, &si, fps, frames, held);

        plat_present(framebuffer, PANEL_W, PANEL_H);
        frames++;
        fps_frames++;

        next += FRAME_US;
        plat_sleep_until(next);
    }

    printf("pid351: exit (%s) after %u frames, %.2f fps\n",
           reason, frames, fps);
    plat_shutdown();
    return 0;
}
