/* pid351 - skeleton main loop
 *
 * For now this draws a test pattern at GBA native resolution, which is the
 * whole reason the panel geometry is a gift: 240x160 doubled is exactly
 * 480x320. Pixel perfect, no filtering, no scaler. Once the platform layer is
 * proven on both backends this is where the cores get driven instead.
 */
#include <stdio.h>
#include <string.h>

#include "pid351.h"
#include "platform.h"

#define CORE_W 240
#define CORE_H 160

/* GBA is 59.727 Hz, not 60. Emulating it at 60 is what makes every general
 * purpose frontend either drop a frame periodically or resample audio. We are
 * not a general purpose frontend. */
#define FRAME_US 16743

static px_t fb[CORE_W * CORE_H];

static void fill_rect(int x, int y, int w, int h, px_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > CORE_W) w = CORE_W - x;
    if (y + h > CORE_H) h = CORE_H - y;

    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            fb[row * CORE_W + col] = c;
}

static void draw_test_pattern(unsigned frame, int box_x, int box_y)
{
    /* Scrolling gradient, so a frozen frame is obvious at a glance. */
    for (int y = 0; y < CORE_H; y++) {
        for (int x = 0; x < CORE_W; x++) {
            int v = (x + (int)frame) & 0xFF;
            fb[y * CORE_W + x] = RGB565(v, y * 255 / CORE_H, 64);
        }
    }

    /* One pixel border: if any edge is missing on the device, something is
     * cropping or overscanning and needs to be found before anything else. */
    fill_rect(0, 0, CORE_W, 1, RGB565(255, 255, 255));
    fill_rect(0, CORE_H - 1, CORE_W, 1, RGB565(255, 255, 255));
    fill_rect(0, 0, 1, CORE_H, RGB565(255, 255, 255));
    fill_rect(CORE_W - 1, 0, 1, CORE_H, RGB565(255, 255, 255));

    /* Asymmetric orientation markers. The panel is physically portrait and
     * rotated, so on the device these immediately show whether the rotation
     * went the right way: big red square top left, small green bar top right. */
    fill_rect(4, 4, 24, 24, RGB565(255, 0, 0));
    fill_rect(CORE_W - 20, 4, 16, 6, RGB565(0, 255, 0));

    /* Driven by the d-pad, so input is visible without reading the log. */
    fill_rect(box_x, box_y, 16, 16, RGB565(255, 255, 255));
}

int main(void)
{
    if (plat_init() != 0)
        return 1;

    unsigned frame = 0;
    int box_x = CORE_W / 2, box_y = CORE_H / 2;
    uint32_t prev_held = 0;
    uint64_t deadline = plat_now_us();
    const char *reason = "?";

    for (;;) {
        uint32_t held = plat_input();

        if (held != prev_held) {
            printf("input: %04x%s%s%s%s%s%s\n", held,
                   (held & PAD_A) ? " A" : "", (held & PAD_B) ? " B" : "",
                   (held & PAD_X) ? " X" : "", (held & PAD_Y) ? " Y" : "",
                   (held & PAD_START) ? " START" : "",
                   (held & PAD_SELECT) ? " SELECT" : "");
            fflush(stdout);
            prev_held = held;
        }

        if (held & PAD_MENU)                                 { reason = "menu";  break; }
        if ((held & PAD_START) && (held & PAD_SELECT))       { reason = "combo"; break; }
        if (plat_should_quit())                              { reason = "quit";  break; }

        if (held & PAD_LEFT)  box_x -= 2;
        if (held & PAD_RIGHT) box_x += 2;
        if (held & PAD_UP)    box_y -= 2;
        if (held & PAD_DOWN)  box_y += 2;

        draw_test_pattern(frame++, box_x, box_y);
        plat_present(fb, CORE_W, CORE_H);

        deadline += FRAME_US;
        plat_sleep_until(deadline);
    }

    plat_shutdown();
    printf("pid351: exit (%s) after %u frames\n", reason, frame);
    return 0;
}
