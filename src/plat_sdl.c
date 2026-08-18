/* pid351 - laptop backend (SDL3)
 *
 * This exists so that the frontend, the menu, the save state handling and the
 * core integration can all be written and debugged at native speed with gdb,
 * without the handheld being involved at all. It is not shipped to the device
 * and never will be.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "pid351.h"
#include "platform.h"
#include "scale.h"

/* The panel is small; on a laptop display show it at 2x so it is usable. */
#define WINDOW_SCALE 2

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static int tex_w, tex_h;
static int quit_requested;

int plat_init(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "pid351: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    win = SDL_CreateWindow("pid351", PANEL_W * WINDOW_SCALE,
                           PANEL_H * WINDOW_SCALE, 0);
    if (!win) {
        fprintf(stderr, "pid351: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {
        fprintf(stderr, "pid351: SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }

    /* Render in panel coordinates regardless of window size, so the scaling
     * policy here is identical to the one the device will run. */
    SDL_SetRenderLogicalPresentation(ren, PANEL_W, PANEL_H,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    return 0;
}

void plat_shutdown(void)
{
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}

uint32_t plat_input(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        if (ev.type == SDL_EVENT_QUIT)
            quit_requested = 1;

    const bool *k = SDL_GetKeyboardState(NULL);
    uint32_t held = 0;

    if (k[SDL_SCANCODE_X])         held |= PAD_A;
    if (k[SDL_SCANCODE_Z])         held |= PAD_B;
    if (k[SDL_SCANCODE_S])         held |= PAD_X;
    if (k[SDL_SCANCODE_A])         held |= PAD_Y;
    if (k[SDL_SCANCODE_UP])        held |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN])      held |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT])      held |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT])     held |= PAD_RIGHT;
    if (k[SDL_SCANCODE_Q])         held |= PAD_L1;
    if (k[SDL_SCANCODE_W])         held |= PAD_R1;
    if (k[SDL_SCANCODE_1])         held |= PAD_L2;
    if (k[SDL_SCANCODE_2])         held |= PAD_R2;
    if (k[SDL_SCANCODE_RETURN])    held |= PAD_START;
    if (k[SDL_SCANCODE_BACKSPACE]) held |= PAD_SELECT;
    if (k[SDL_SCANCODE_ESCAPE])    held |= PAD_MENU;

    return held;
}

void plat_present(const px_t *fb, int w, int h)
{
    if (!tex || w != tex_w || h != tex_h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                                SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!tex) {
            fprintf(stderr, "pid351: SDL_CreateTexture: %s\n", SDL_GetError());
            return;
        }
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);  /* never blur */
        tex_w = w;
        tex_h = h;
    }

    SDL_UpdateTexture(tex, NULL, fb, w * (int)sizeof(px_t));

    rect_t f = fit_integer(w, h, PANEL_W, PANEL_H);
    SDL_FRect dst = { (float)f.x, (float)f.y, (float)f.w, (float)f.h };

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, &dst);
    SDL_RenderPresent(ren);
}

uint64_t plat_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void plat_sleep_until(uint64_t deadline_us)
{
    uint64_t now = plat_now_us();
    if (now >= deadline_us)
        return;                       /* running late: do not try to catch up */

    uint64_t delta = deadline_us - now;
    struct timespec ts = {
        .tv_sec  = (time_t)(delta / 1000000u),
        .tv_nsec = (long)((delta % 1000000u) * 1000u),
    };
    nanosleep(&ts, NULL);
}

int plat_should_quit(void)
{
    return quit_requested;
}
