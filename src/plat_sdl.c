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

    /* Laid out so the keyboard has the shell's geometry rather than the
     * shell's letters. IJKL is a diamond in the same orientation as XYAB, so
     * the pair that is comfortable to roll a finger across on the keyboard is
     * the pair that is comfortable under a thumb on the device - which is the
     * only property of a test rig that matters once the mapping above it
     * starts depending on which buttons sit next to which.
     *
     * YUIOP puts L2 outside L1 and R2 outside R1, matching the way the two
     * shoulder pairs stack on the shell. */
    if (k[SDL_SCANCODE_I])         held |= PAD_X;
    if (k[SDL_SCANCODE_J])         held |= PAD_Y;
    if (k[SDL_SCANCODE_K])         held |= PAD_B;
    if (k[SDL_SCANCODE_L])         held |= PAD_A;

    /* Both, because the device has both and they are indistinguishable from
     * up here: plat_drm.c folds the left stick into the same four bits as the
     * hat, deliberately, so that nothing above the platform layer learns this
     * machine has a stick. A laptop that offered only one of them would be
     * testing something the handheld does not do. */
    if (k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W]) held |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S]) held |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A]) held |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) held |= PAD_RIGHT;

    if (k[SDL_SCANCODE_U])         held |= PAD_L1;
    if (k[SDL_SCANCODE_O])         held |= PAD_R1;
    if (k[SDL_SCANCODE_Y])         held |= PAD_L2;
    if (k[SDL_SCANCODE_P])         held |= PAD_R2;
    if (k[SDL_SCANCODE_N])         held |= PAD_L3;
    if (k[SDL_SCANCODE_M])         held |= PAD_R3;
    if (k[SDL_SCANCODE_RETURN])    held |= PAD_START;
    if (k[SDL_SCANCODE_BACKSPACE]) held |= PAD_SELECT;

    return held;
}

static SDL_Texture *bartex;

static uint32_t present_us;

void plat_present(const px_t *fb, int w, int h, const px_t *bar)
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

    rect_t f = bar ? fit_panel(w, h, PANEL_W, PANEL_H)
                   : (rect_t){ 0, 0, PANEL_W, PANEL_H };
    SDL_FRect dst = { (float)f.x, (float)f.y, (float)f.w, (float)f.h };

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, &dst);

    if (bar && f.w < PANEL_W) {
        if (!bartex) {
            bartex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       PANEL_W - f.w, PANEL_H);
            if (bartex)
                SDL_SetTextureScaleMode(bartex, SDL_SCALEMODE_NEAREST);
        }
        if (bartex) {
            SDL_UpdateTexture(bartex, NULL, bar,
                              (PANEL_W - f.w) * (int)sizeof(px_t));
            SDL_FRect bd = { (float)f.w, 0.0f,
                             (float)(PANEL_W - f.w), (float)PANEL_H };
            SDL_RenderTexture(ren, bartex, NULL, &bd);
        }
    }

    uint64_t t0 = plat_now_us();
    SDL_RenderPresent(ren);
    present_us = (uint32_t)(plat_now_us() - t0);
}

void plat_frame_us(uint32_t *blit_us, uint32_t *wait_us)
{
    /* There is no rotate blit on this backend at all - the panel here is the
     * right way up and the GPU does the scale - so reporting anything but
     * zero would invite comparing a laptop number against the device's. */
    if (blit_us) *blit_us = 0;
    if (wait_us) *wait_us = present_us;
}

int plat_bench(const px_t *src, int src_w, int src_h, int variant, int tile,
               uint32_t *samples, int n)
{
    (void)src; (void)src_w; (void)src_h; (void)variant; (void)tile;
    (void)samples; (void)n;

    /* Refused on purpose. This measurement exists to decide whether to move
     * work off a 1.3 GHz in-order Cortex-A35 with small caches; timing the
     * same loops on an out-of-order laptop core would produce a confident
     * number that says nothing about that question. */
    return -1;
}

int plat_blit_verify(const px_t *src, int src_w, int src_h,
                     int variant, int tile)
{
    (void)src; (void)src_w; (void)src_h; (void)variant; (void)tile;
    return -1;   /* there is no rotate blit on this backend to compare against */
}

int plat_mem_probe(plat_mem_t *out, int iters)
{
    (void)out; (void)iters;
    return -1;   /* no scanout buffer here; the question does not arise */
}

void plat_mode_timing(uint32_t *exact_mhz, uint32_t *clock_khz,
                      uint32_t *htotal, uint32_t *vtotal)
{
    if (exact_mhz) *exact_mhz = 0;
    if (clock_khz) *clock_khz = 0;
    if (htotal)    *htotal    = 0;
    if (vtotal)    *vtotal    = 0;
}

int plat_vblank_probe(int flips, uint32_t *measured_mhz)
{
    (void)flips; (void)measured_mhz;
    return -1;   /* the compositor decides when this presents, not the panel */
}

void plat_dump_props(void)
{
    /* Nothing here belongs to us; the compositor owns the display. */
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

/* No sticks on a keyboard, and adding fake ones would only teach us something
 * about SDL rather than about the handheld. The demo says so on screen. */
int plat_axes(plat_axis_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}


/* A laptop's filesystem is already there and its ROMs come from argv. */
const char *plat_boot_mount(void)
{
    return NULL;
}

/* There is no init to be on a laptop. These exist so main.c can call them
 * unconditionally rather than growing an #ifdef around the boot path. */
int plat_boot_init(void)
{
    return 0;
}

int plat_is_init(void)
{
    return 0;
}

int plat_boot_save_log(const char *name)
{
    (void)name;
    return 0;
}

void plat_boot_shutdown(int power_off)
{
    (void)power_off;
}
