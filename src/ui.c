/* pid351 - the interface. See ui.h for what belongs here.
 *
 * Laid out in absolute pixels against a 480x320 panel, because there is
 * exactly one panel and a layout engine for a machine with one screen size is
 * a layout engine written for nobody. The constants are named and grouped at
 * the top so that moving something is one edit rather than a search.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pid351.h"
#include "platform.h"
#include "gfx.h"
#include "palette.h"
#include "scale.h"
#include "ui.h"

/* ------------------------------------------------------------ geometry */

#define MARGIN      16
#define HEAD_Y      12          /* ink top of the wordmark */
#define HEAD_SCALE  2
#define RULE_Y      46
#define BODY_Y      58
#define ROW_H       26
#define ROW_SCALE   2
#define ROWS_SHOWN  8
#define ROWS_H      (ROWS_SHOWN * ROW_H)

/* The footer sits on the bottom margin rather than under the last row, so the
 * page has the same air top and bottom whether the card holds three games or
 * thirty. */
#define FOOT_Y      (PANEL_H - MARGIN - FONT_INK_H)
#define FOOT_RULE_Y (FOOT_Y - 12)

/* The rail down the right edge. Two pixels: it is a position readout, not a
 * thing anyone drags. */
#define RAIL_X      (PANEL_W - 6)
#define RAIL_W      3

/* Where a row's ink stops, so a long name is trimmed rather than run under
 * the savestate dot. */
#define ROW_TEXT_X  (MARGIN + 8)
#define ROW_TEXT_R  (PANEL_W - MARGIN - 16)

/* The battery, top right, and the same drawing in the game's side column. */
#define BATT_W      34
#define BATT_H      15

/* How long the brightness readout stays up after the last press. Long enough
 * to see where the level landed, short enough that it is never in the way of
 * the thing being lit. */
#define BRIGHT_SHOW_US 1200000u

#define FRAME_US 16661          /* the panel, same constant main.c paces to */

/* ------------------------------------------------------------- helpers */

/* Text positioned by its ink rather than by its cell. Every y in this file is
 * where the ink starts, which is the only measurement that matches what the
 * eye lines things up on. */
static void text(canvas_t *c, int x, int y, const char *s, int scale, px_t col)
{
    gfx_text(c, x, y - FONT_INK_TOP * scale, s, scale, col);
}

static void text_right(canvas_t *c, int right, int y, const char *s, int scale,
                       px_t col)
{
    text(c, right - gfx_ink_w(s, scale), y, s, scale, col);
}

static long read_long(const char *path, long missing)
{
    FILE *f = fopen(path, "rb");
    long v;

    if (!f)
        return missing;
    if (fscanf(f, "%ld", &v) != 1)
        v = missing;
    fclose(f);
    return v;
}

/* A percentage, or -1 for no gauge. Clamped here rather than at each use:
 * three callers formatting a long into a small buffer is three places for the
 * same bound to be forgotten. */
static int clamp_pct(long v)
{
    if (v < 0)
        return -1;
    return v > 100 ? 100 : (int)v;
}

static int write_long(const char *path, long v)
{
    FILE *f = fopen(path, "w");
    int ok;

    if (!f)
        return -1;
    ok = fprintf(f, "%ld", v) > 0;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

/* --------------------------------------------------------- brightness */

/* The device tree gives this backlight 1666 steps and no table, so the range
 * is ours to choose. Eight stops, geometric rather than linear: perceived
 * brightness goes roughly as the log of the drive, so eight evenly spaced
 * numbers would be seven presses in the dark half and one in the light.
 *
 * The bottom stop is 60 and not 0. A control whose lowest setting is "off"
 * turns the machine into a brick for anyone who taps it one press too far in
 * a dark room, and there is no second screen to recover from.
 */
#define BL_PATH "/sys/class/backlight/backlight/brightness"

static const int bright_step[UI_BRIGHT_STEPS] = {
    60, 110, 200, 350, 590, 900, 1250, 1666
};

void ui_bright_apply(int step)
{
    if (step < 0)
        step = 0;
    if (step >= UI_BRIGHT_STEPS)
        step = UI_BRIGHT_STEPS - 1;
    write_long(BL_PATH, bright_step[step]);
}

void ui_bright_off(void)
{
    write_long(BL_PATH, 0);
}

/* -------------------------------------------------------------- state */

/* One file, two integers, written the same way a savestate is: to a
 * temporary and renamed over the old one, so a power cut during the write
 * leaves the previous contents rather than half of the new ones.
 *
 * CLAUDE.md forbids configuration files and is right to. This is not one.
 * Configuration is what a person edits to change how the machine behaves;
 * this is what the machine writes down so it can remember where it was, the
 * same category as the savestate sitting next to it. Nothing reads it but us,
 * nothing in it is a decision anyone made in a text editor, and deleting it
 * costs a cursor position and a backlight level.
 */
#define STATE_NAME "pid351/state"

void ui_state_load(const char *boot, struct ui_state *st)
{
    char path[512];
    FILE *f;

    st->cursor = 0;
    st->bright = UI_BRIGHT_STEPS / 2;
    if (!boot)
        return;
    snprintf(path, sizeof path, "%s/%s", boot, STATE_NAME);
    f = fopen(path, "rb");
    if (!f)
        return;
    if (fscanf(f, "%d %d", &st->cursor, &st->bright) != 2) {
        st->cursor = 0;
        st->bright = UI_BRIGHT_STEPS / 2;
    }
    fclose(f);
    if (st->bright < 0 || st->bright >= UI_BRIGHT_STEPS)
        st->bright = UI_BRIGHT_STEPS / 2;
    if (st->cursor < 0)
        st->cursor = 0;
}

void ui_state_save(const char *boot, const struct ui_state *st)
{
    char path[512], tmp[512];
    FILE *f;

    if (!boot)
        return;
    snprintf(path, sizeof path, "%s/%s", boot, STATE_NAME);
    snprintf(tmp, sizeof tmp, "%s/%s.tmp", boot, STATE_NAME);
    f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "%d %d\n", st->cursor, st->bright);
    if (fclose(f) == 0)
        rename(tmp, path);
}

/* --------------------------------------------------------- the scan */

/* The name shown is the filename with its extension and its bracketed tags
 * removed: "Kirby's Adventure (USA) (Rev 1).nes" reads as "Kirby's
 * Adventure". No database, no scraper, no per-game file - the tags are a
 * convention every dump follows and stripping them is four lines. A name that
 * genuinely contains a bracket loses the rest of itself, which is a trade
 * worth making against carrying a title database on a machine with one
 * directory of ROMs.
 */
static void display_name(const char *file, char *out, size_t n)
{
    const char *paren = strstr(file, " (");
    const char *dot = strrchr(file, '.');
    size_t len = strlen(file);

    if (paren)
        len = (size_t)(paren - file);
    else if (dot && (size_t)(dot - file) < len)
        len = (size_t)(dot - file);
    if (len >= n)
        len = n - 1;
    memcpy(out, file, len);
    out[len] = 0;
}

static int claimed(const char *file)
{
    const char *dot = strrchr(file, '.');

    /* One core, one extension. When a second core lands this becomes a
     * question for core.c rather than a longer list here. */
    return dot && (!strcmp(dot, ".nes") || !strcmp(dot, ".NES"));
}

static int by_name(const void *a, const void *b)
{
    const struct ui_rom *x = a, *y = b;
    return strcmp(x->name, y->name);
}

int ui_scan(const char *dir, struct ui_rom *out, int max)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) && n < max) {
        char state[600];

        if (e->d_name[0] == '.' || !claimed(e->d_name))
            continue;
        snprintf(out[n].path, sizeof out[n].path, "%s/%s", dir, e->d_name);
        display_name(e->d_name, out[n].name, sizeof out[n].name);
        /* Asked of the filesystem rather than remembered, because the state
         * file is the one thing on the card a person might reasonably delete
         * by hand and the list should agree with what is actually there. */
        snprintf(state, sizeof state, "%s.state", out[n].path);
        out[n].has_state = access(state, R_OK) == 0;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, by_name);
    return n;
}

/* ------------------------------------------------------------ drawing */

static void draw_battery(canvas_t *c, int x, int y, int pct, long ua)
{
    px_t ink = pct < 0  ? UI_DIM
             : ua > 0   ? UI_GOOD          /* charging */
             : pct <= 12 ? UI_WARN
             : pct <= 30 ? DB_TAHITI_GOLD
                         : UI_TEXT;
    int iw = BATT_W - 6;

    gfx_frame(c, x, y, BATT_W, BATT_H, UI_DIM);
    gfx_rect(c, x + BATT_W, y + 4, 3, BATT_H - 8, UI_DIM);
    if (pct >= 0) {
        int fw = iw * pct / 100;
        gfx_rect(c, x + 3, y + 3, fw, BATT_H - 6, ink);
    }
}

/* The brightness readout: eight cells, filled to the level. Drawn as the
 * thing it controls rather than as a number, because "5" means nothing and a
 * row of lit cells means exactly what it looks like. */
static void draw_bright(canvas_t *c, int x, int y, int step, int cell_w)
{
    for (int i = 0; i < UI_BRIGHT_STEPS; i++)
        gfx_rect(c, x + i * (cell_w + 2), y, cell_w, 10,
                 i <= step ? UI_ACCENT : UI_EDGE);
}

/* A name too wide for the row loses its tail to an ellipsis. Trimmed by
 * measuring rather than by counting characters, because ink_w is what decides
 * whether it fits and a character count would be a second, wrong answer. */
static void fit_name(const char *name, char *out, size_t n, int avail)
{
    size_t len = strlen(name);

    if (len >= n)
        len = n - 1;
    memcpy(out, name, len);
    out[len] = 0;
    if (gfx_ink_w(out, ROW_SCALE) <= avail)
        return;
    while (len > 1) {
        out[--len] = '\x05';   /* ellipsis */
        out[len + 1] = 0;
        if (gfx_ink_w(out, ROW_SCALE) <= avail)
            return;
    }
}

static void row_text(canvas_t *c, const struct ui_rom *r, int y, int selected)
{
    px_t ink = selected ? UI_SEL_TEXT : UI_TEXT;
    int ty = y + (ROW_H - FONT_INK_H * ROW_SCALE) / 2;
    char shown[64];

    if (selected) {
        gfx_rect(c, MARGIN - 8, y, PANEL_W - 2 * (MARGIN - 8), ROW_H,
                 UI_SEL_BAR);
        gfx_rect(c, MARGIN - 8, y, 3, ROW_H, UI_ACCENT);
    }
    fit_name(r->name, shown, sizeof shown, ROW_TEXT_R - ROW_TEXT_X);
    text(c, ROW_TEXT_X, ty, shown, ROW_SCALE, ink);
    /* A dot, not the word RESUME: the row is already the game's name and the
     * only thing left to say is whether pressing A continues or starts. */
    if (r->has_state)
        gfx_disc(c, ROW_TEXT_R + 8, y + ROW_H / 2, 4,
                 selected ? UI_SEL_TEXT : UI_ACCENT);
}

static void draw_list(px_t *fb, const struct ui_rom *roms, int n,
                      const struct ui_state *st, int top, int pct, long ua,
                      uint64_t bright_until, uint64_t now)
{
    canvas_t c = { fb, PANEL_W, PANEL_H };
    char buf[16];

    gfx_rect(&c, 0, 0, PANEL_W, PANEL_H, UI_GROUND);

    text(&c, MARGIN, HEAD_Y, "PID351", HEAD_SCALE, UI_ACCENT);
    /* Both status pieces centred on the wordmark's ink, not on its cell: the
     * cell has three rows of ascender space and lining up to it puts the
     * battery visibly low. */
    if (pct >= 0) {
        snprintf(buf, sizeof buf, "%d%%", pct);
        text_right(&c, PANEL_W - MARGIN - BATT_W - 12,
                   HEAD_Y + (FONT_INK_H * HEAD_SCALE - FONT_INK_H) / 2, buf, 1,
                   UI_TEXT);
    }
    draw_battery(&c, PANEL_W - MARGIN - BATT_W,
                 HEAD_Y + (FONT_INK_H * HEAD_SCALE - BATT_H) / 2, pct, ua);
    gfx_rect(&c, MARGIN, RULE_Y, PANEL_W - 2 * MARGIN, 1, UI_EDGE);

    for (int i = 0; i < ROWS_SHOWN && top + i < n; i++)
        row_text(&c, &roms[top + i], BODY_Y + i * ROW_H, top + i == st->cursor);

    /* Only when the list is longer than the window. A rail that is always
     * there and always full is decoration; one that appears exactly when
     * there is more to see is the message. */
    if (n > ROWS_SHOWN) {
        int th = ROWS_H * ROWS_SHOWN / n;
        int ty = ROWS_H * top / n;

        if (th < 8)
            th = 8;
        if (ty > ROWS_H - th)
            ty = ROWS_H - th;
        gfx_rect(&c, RAIL_X, BODY_Y, RAIL_W, ROWS_H, UI_EDGE);
        gfx_rect(&c, RAIL_X, BODY_Y + ty, RAIL_W, th, UI_TEXT);
    }

    gfx_rect(&c, MARGIN, FOOT_RULE_Y, PANEL_W - 2 * MARGIN, 1, UI_EDGE);
    if (now < bright_until) {
        text(&c, MARGIN, FOOT_Y, "BRIGHT", 1, UI_TEXT);
        draw_bright(&c, MARGIN + 46, FOOT_Y - 1, st->bright, 10);
    } else if (n) {
        text(&c, MARGIN, FOOT_Y, "A PLAY", 1, UI_TEXT);
        text(&c, MARGIN + 60, FOOT_Y, "L1 R1 BRIGHT", 1, UI_DIM);
    } else {
        text(&c, MARGIN, FOOT_Y, "NO ROMS ON THE CARD", 1, UI_WARN);
    }
    text_right(&c, PANEL_W - MARGIN, FOOT_Y, "START+SELECT OFF", 1, UI_DIM);
}

/* ---------------------------------------------------------- the list */

int ui_list(px_t *fb, const struct ui_rom *roms, int n, struct ui_state *st)
{
    uint64_t next = plat_now_us();
    uint64_t bright_until = 0, gauge_at = 0;
    /* What the panel is currently showing. Impossible values, so the first
     * pass through the loop always draws - which it must, because what is on
     * the panel when the list is entered is a splash or the last frame of a
     * game. */
    int drawn_top = -1, drawn_cur = -1, drawn_bright = -1, drawn_pct = -2;
    int drawn_show = -1;
    long drawn_ua = -1;
    /* Seeded with what is held right now rather than with zero. The list is
     * entered by releasing A, or by START+SELECT out of a game - and that
     * press is still under the player's thumbs when we get here, a game
     * teardown being a few milliseconds and a human press a few hundred.
     * Starting from zero makes every one of those look like a fresh press on
     * frame 0, which read one START+SELECT as two and powered the machine off
     * on the way back to the list. */
    uint32_t was = plat_input();
    int top = 0;
    int pct = -1;
    long ua = 0;

    if (st->cursor >= n)
        st->cursor = n ? n - 1 : 0;

    for (;;) {
        uint32_t held = plat_input();
        uint32_t hit = held & ~was;
        uint64_t now = plat_now_us();

        was = held;

        /* Both held, and one of them new. The edge is what makes this a
         * decision rather than the tail of the press that got here: held
         * across the entry it never fires, and releasing either one arms it
         * again. */
        if ((held & PAD_START) && (held & PAD_SELECT)
            && (hit & (PAD_START | PAD_SELECT)))
            return -1;
        if (plat_should_quit())
            return -1;

        if (hit & (PAD_UP | PAD_LEFT))
            st->cursor--;
        if (hit & (PAD_DOWN | PAD_RIGHT))
            st->cursor++;
        /* Wraps, because a list of seven on a screen that shows eight has no
         * scrolling to make the ends feel like ends. */
        if (n) {
            if (st->cursor < 0)
                st->cursor = n - 1;
            if (st->cursor >= n)
                st->cursor = 0;
        }
        if (hit & PAD_L1) {
            st->bright--;
            if (st->bright < 0)
                st->bright = 0;
            ui_bright_apply(st->bright);
            bright_until = now + BRIGHT_SHOW_US;
        }
        if (hit & PAD_R1) {
            st->bright++;
            if (st->bright >= UI_BRIGHT_STEPS)
                st->bright = UI_BRIGHT_STEPS - 1;
            ui_bright_apply(st->bright);
            bright_until = now + BRIGHT_SHOW_US;
        }
        if ((hit & PAD_A) && n)
            return st->cursor;

        /* Keep the cursor on screen with a margin at each end rather than
         * scrolling only when it hits the edge, so the next row is always
         * visible before you move onto it. */
        if (st->cursor < top)
            top = st->cursor;
        if (st->cursor >= top + ROWS_SHOWN)
            top = st->cursor - ROWS_SHOWN + 1;
        if (top > n - ROWS_SHOWN)
            top = n - ROWS_SHOWN;
        if (top < 0)
            top = 0;

        /* Two sysfs reads every three seconds, on a screen that is doing
         * nothing else. Per frame it would be ninety times that to animate a
         * figure that moves once a minute. */
        if (now >= gauge_at) {
            pct = clamp_pct(read_long(
                "/sys/class/power_supply/rk817-battery/capacity", -1));
            ua = read_long("/sys/class/power_supply/rk817-battery/current_now",
                           0);
            gauge_at = now + 3000000;
        }

        /* Only when the picture would differ. The list is 300 KB of panel
         * that changes on a keypress and on a gauge read three seconds
         * apart; drawing and flipping it sixty times a second is the whole
         * frame's work to show what is already on the screen, and browsing
         * is a thing people do for minutes at a time.
         *
         * Skipping the present also skips the vblank wait, so the loop paces
         * on the clock instead - still one wakeup a frame to read the pad,
         * which is what keeps a press felt immediately, and nothing else. */
        int show = now < bright_until;

        if (top != drawn_top || st->cursor != drawn_cur
            || st->bright != drawn_bright || show != drawn_show
            || pct != drawn_pct || ua != drawn_ua) {
            draw_list(fb, roms, n, st, top, pct, ua, bright_until, now);
            plat_present(fb, PANEL_W, PANEL_H, NULL);
            drawn_top = top;
            drawn_cur = st->cursor;
            drawn_bright = st->bright;
            drawn_show = show;
            drawn_pct = pct;
            drawn_ua = ua;
        }

        next += FRAME_US;
        plat_sleep_until(next);
    }
}

/* ------------------------------------------------------ the side column */

/* 63 pixels of panel that the 8:7 picture does not reach. It shows the two
 * things worth knowing without pausing - how much battery is left and, for a
 * moment after it changes, how bright the screen is - and the name of what is
 * running, turned on its side because the column is 63 wide and 320 tall.
 *
 * Nothing on it moves during play. A readout that updates every frame in the
 * corner of the eye is a distraction dressed as information, and the input
 * display that used to live here was exactly that.
 */
void ui_bar(px_t *bar, const char *name, int bright_shown)
{
    canvas_t c = { bar, BAR_W, PANEL_H };
    static int pct = -1, last_pct = -2, last_bright = -2;
    static long ua, last_ua = -1;
    static const char *last_name = (const char *)1;
    static uint64_t gauge_at;
    uint64_t now = plat_now_us();

    if (now >= gauge_at) {
        pct = clamp_pct(read_long(
            "/sys/class/power_supply/rk817-battery/capacity", -1));
        ua = read_long("/sys/class/power_supply/rk817-battery/current_now", 0);
        gauge_at = now + 5000000;
    }

    /* Nothing on this column moves between the events below, and the buffer
     * is the caller's and outlives the frame, so redrawing it is 40 KB of
     * writes an unchanged picture. Sixty times a second, on the machine whose
     * first design rule is battery.
     *
     * The name is compared by pointer and that is not a shortcut: it points
     * into the scanned list, which is fixed for the whole power-on, so the
     * same pointer is the same characters. A strcmp here would be reading the
     * string to learn something the address already said. */
    if (pct == last_pct && ua == last_ua && bright_shown == last_bright
        && name == last_name)
        return;
    last_pct = pct;
    last_ua = ua;
    last_bright = bright_shown;
    last_name = name;

    gfx_rect(&c, 0, 0, BAR_W, PANEL_H, UI_LETTERBOX);
    /* A single hairline against the picture, so the pillarbox reads as an edge
     * of the machine rather than as the game failing to fill the screen. It
     * belongs on the seam, which is column zero: the bar is blitted to the
     * right of the picture, so BAR_W - 1 put it down the outside edge of the
     * panel instead - a frame on one side of the screen. */
    gfx_rect(&c, 0, 0, 1, PANEL_H, UI_PANEL);

    draw_battery(&c, (BAR_W - BATT_W - 3) / 2, 14, pct, ua);
    if (pct >= 0) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d%%", pct);
        text(&c, (BAR_W - gfx_ink_w(buf, 1)) / 2, 36, buf, 1, UI_DIM);
    }

    if (bright_shown >= 0) {
        for (int i = 0; i < UI_BRIGHT_STEPS; i++)
            gfx_rect(&c, (BAR_W - 22) / 2, PANEL_H - 30 - i * 12, 22, 8,
                     i <= bright_shown ? UI_ACCENT : UI_EDGE);
    } else if (name && *name) {
        /* Bottom up, so it reads with the machine held normally. */
        int h = gfx_ink_w(name, 1);
        gfx_text_rot(&c, (BAR_W - FONT_ROWS) / 2, PANEL_H / 2 + h / 2, name, 1,
                     UI_DIM);
    }
}
