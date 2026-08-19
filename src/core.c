/* pid351 - libretro host
 *
 * See core.h for why libretro and why static. This file is the callbacks and
 * the rate conversion.
 *
 * The rate conversion is the whole of the difficulty. Three independent clock
 * mismatches stack up between a core and the speaker: the core's sample rate
 * is not the codec's, the console's refresh is not the panel's, and the
 * panel's crystal is not the codec's. Rather than correct three things, the
 * resampler runs off one ring and one feedback term - the ring level is the
 * only observable that all three disturb, so holding it steady holds all
 * three. That also means the panel-versus-codec drift measured separately in
 * docs/hardware.md needs no correction of its own here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pid351.h"
#include "core.h"
#include "aud.h"
#include "../cores/libretro.h"

/* Each core is one relocatable object exporting a prefixed copy of the API.
 * Declared rather than included: there is no header for a renamed core, and
 * writing the prototypes out is what makes the renaming visible at the only
 * place it matters. */
#define DECLARE_CORE(p) \
    void p##_retro_set_environment(retro_environment_t); \
    void p##_retro_set_video_refresh(retro_video_refresh_t); \
    void p##_retro_set_audio_sample(retro_audio_sample_t); \
    void p##_retro_set_audio_sample_batch(retro_audio_sample_batch_t); \
    void p##_retro_set_input_poll(retro_input_poll_t); \
    void p##_retro_set_input_state(retro_input_state_t); \
    void p##_retro_init(void); \
    void p##_retro_deinit(void); \
    void p##_retro_run(void); \
    void p##_retro_get_system_info(struct retro_system_info *); \
    void p##_retro_get_system_av_info(struct retro_system_av_info *); \
    bool p##_retro_load_game(const struct retro_game_info *); \
    void p##_retro_unload_game(void)

DECLARE_CORE(nes);

typedef struct {
    const char *name;
    const char *exts;          /* space separated, lower case, no dots */
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    void (*run)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
} core_t;

#define CORE_ENTRY(p, nm, ex) { \
    nm, ex, \
    p##_retro_set_environment, p##_retro_set_video_refresh, \
    p##_retro_set_audio_sample, p##_retro_set_audio_sample_batch, \
    p##_retro_set_input_poll, p##_retro_set_input_state, \
    p##_retro_init, p##_retro_deinit, p##_retro_run, \
    p##_retro_get_system_info, p##_retro_get_system_av_info, \
    p##_retro_load_game, p##_retro_unload_game }

/* Adding a console is one fetch, one blob.sh line and one row here. That is
 * the entire point of the renaming; if it ever takes more, something above
 * has gone wrong. */
static const core_t cores[] = {
    CORE_ENTRY(nes, "fceumm", "nes fds unf unif"),
};

#define NCORES ((int)(sizeof cores / sizeof cores[0]))

static const core_t *cur;
static uint32_t pad_held;

/* ------------------------------------------------------------- video */

/* The core hands back a pointer into its own framebuffer with its own pitch,
 * and the blit wants tight packing. Copying is 123 KB a frame for NES, which
 * measured against a 16.6 ms budget is not worth teaching the blit about
 * pitch for - but it is the first thing to reach for if a core ever lands
 * close to the frame budget. */
static px_t vbuf[PANEL_W * PANEL_H];
static int vw, vh, vnew;

static void cb_video(const void *data, unsigned width, unsigned height,
                     size_t pitch)
{
    if (!data || width == 0 || height == 0)
        return;                      /* legal: means "repeat last frame" */
    if ((int)width > PANEL_W || (int)height > PANEL_H) {
        fprintf(stderr, "pid351: core frame %ux%u exceeds panel\n",
                width, height);
        return;
    }

    const unsigned char *src = data;
    for (unsigned y = 0; y < height; y++)
        memcpy(vbuf + (size_t)y * width, src + (size_t)y * pitch,
               (size_t)width * sizeof(px_t));

    vw = (int)width;
    vh = (int)height;
    vnew = 1;
}

/* ------------------------------------------------------------- audio */

/* Power of two so the wrap is a mask. Eight thousand frames is a sixth of a
 * second, which is far more than the conversion needs and exists so a core
 * that stalls for a few frames does not become an underrun. */
#define RING_FRAMES 8192
#define RING_MASK   (RING_FRAMES - 1)

/* About one video frame of slack, on top of the codec's own 85 ms. Enough to
 * absorb a core that runs long on one frame, and no more: this sits directly
 * in front of the speaker, so every frame of it is latency between pressing a
 * button and hearing the jump. */
#define RING_TARGET 1024

static int16_t ring[RING_FRAMES * 2];
static unsigned ring_w, ring_r;       /* frame counters, free running */
static uint64_t rs_pos;               /* 32.32 read position, fractional */
static uint64_t rs_step;              /* input frames per output frame */
static int core_rate, core_fps;       /* fps in millihertz */

static unsigned ring_level(void)
{
    return ring_w - ring_r;
}

static size_t cb_audio_batch(const int16_t *data, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        if (ring_level() >= RING_FRAMES - 2)
            break;                    /* full: dropping beats corrupting */
        unsigned s = ring_w & RING_MASK;
        ring[s * 2]     = data[i * 2];
        ring[s * 2 + 1] = data[i * 2 + 1];
        ring_w++;
    }
    return frames;
}

static void cb_audio_sample(int16_t l, int16_t r)
{
    int16_t pair[2] = { l, r };
    cb_audio_batch(pair, 1);
}

void core_audio(void)
{
    int n = aud_due();
    if (n <= 0 || !cur)
        return;

    static int16_t out[4096 * 2];
    if (n > (int)(sizeof out / sizeof out[0]) / 2)
        n = (int)(sizeof out / sizeof out[0]) / 2;

    /* Feedback on ring level. The nominal step already accounts for the two
     * mismatches we know exactly; this absorbs the one we do not, which is
     * two crystals running at their own pace. Deliberately weak: a fast
     * correction is audible as pitch wobble, and there is nothing here that
     * changes quickly. */
    unsigned lvl = ring_level();
    unsigned target = RING_TARGET;
    int64_t err = (int64_t)lvl - (int64_t)target;
    if (err >  (int64_t)target) err =  (int64_t)target;
    if (err < -(int64_t)target) err = -(int64_t)target;
    uint64_t step = (uint64_t)((int64_t)rs_step + err * 1024);

    for (int i = 0; i < n; i++) {
        unsigned idx  = (unsigned)(rs_pos >> 32);
        uint32_t frac = (uint32_t)(rs_pos & 0xffffffffu);

        if (idx + 1 >= ring_w) {        /* starved: hold the last sample */
            out[i * 2] = out[i * 2 + 1] = 0;
            continue;
        }

        unsigned a = idx & RING_MASK, b = (idx + 1) & RING_MASK;
        /* Linear is enough here and cubic is not: the conversion ratio sits
         * within a fraction of a percent of 1:1, where linear interpolation
         * error falls away entirely. It would matter for 32768 to 48000. */
        for (unsigned c = 0; c < 2; c++) {
            int32_t s0 = ring[a * 2 + c], s1 = ring[b * 2 + c];
            int64_t v = s0 + (((int64_t)(s1 - s0) * frac) >> 32);
            out[(unsigned)i * 2 + c] = (int16_t)v;
        }
        rs_pos += step;
    }

    /* Retire whole frames that the read position has passed. */
    unsigned consumed = (unsigned)(rs_pos >> 32);
    if (consumed > ring_r) {
        ring_r = consumed;
        if (ring_r > ring_w)
            ring_r = ring_w;
    }

    aud_write(out, n);
}

int core_audio_level(void)
{
    return cur ? (int)ring_level() : -1;
}

/* ------------------------------------------------------------- input */

static void cb_input_poll(void) { }

static int16_t cb_input_state(unsigned port, unsigned device,
                              unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0)
        return 0;

    /* Named against the shell, not against any one console; per-core
     * remapping is a frontend problem and does not exist yet. */
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_A:      return (pad_held & PAD_A)      != 0;
    case RETRO_DEVICE_ID_JOYPAD_B:      return (pad_held & PAD_B)      != 0;
    case RETRO_DEVICE_ID_JOYPAD_X:      return (pad_held & PAD_X)      != 0;
    case RETRO_DEVICE_ID_JOYPAD_Y:      return (pad_held & PAD_Y)      != 0;
    case RETRO_DEVICE_ID_JOYPAD_UP:     return (pad_held & PAD_UP)     != 0;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (pad_held & PAD_DOWN)   != 0;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (pad_held & PAD_LEFT)   != 0;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (pad_held & PAD_RIGHT)  != 0;
    case RETRO_DEVICE_ID_JOYPAD_L:      return (pad_held & PAD_L1)     != 0;
    case RETRO_DEVICE_ID_JOYPAD_R:      return (pad_held & PAD_R1)     != 0;
    case RETRO_DEVICE_ID_JOYPAD_L2:     return (pad_held & PAD_L2)     != 0;
    case RETRO_DEVICE_ID_JOYPAD_R2:     return (pad_held & PAD_R2)     != 0;
    case RETRO_DEVICE_ID_JOYPAD_START:  return (pad_held & PAD_START)  != 0;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: return (pad_held & PAD_SELECT) != 0;
    default:                            return 0;
    }
}

/* ------------------------------------------------------- environment */

static void cb_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level; (void)fmt;
    /* Swallowed on purpose. Cores log per frame, and on the device stdout is
     * /dev/kmsg, where that is both a rate limit and a power cost. */
}

static bool cb_environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        /* The only format we accept. It is what the VOP scans out and what
         * the blit is written against, so a core that cannot produce it
         * would need a conversion pass we are not going to write. */
        return f == RETRO_PIXEL_FORMAT_RGB565;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;         /* we can repeat a frame */
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = ".";
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = cb_log;
        return true;
    default:
        /* Everything else is a frontend feature we do not have. Saying so is
         * correct: a core that asks is required to cope with "no". */
        return false;
    }
}

/* ------------------------------------------------------------- open */

static const core_t *core_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return NULL;
    char ext[16];
    size_t n = strlen(dot + 1);
    if (n == 0 || n >= sizeof ext)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        char ch = dot[1 + i];
        ext[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    ext[n] = 0;

    for (int i = 0; i < NCORES; i++) {
        const char *p = cores[i].exts;
        while (*p) {
            const char *sp = strchr(p, ' ');
            size_t len = sp ? (size_t)(sp - p) : strlen(p);
            if (len == n && memcmp(p, ext, n) == 0)
                return &cores[i];
            p = sp ? sp + 1 : p + len;
        }
    }
    return NULL;
}

static void *rom_data;
static size_t rom_size;

int core_open(const char *rom_path)
{
    cur = core_for(rom_path);
    if (!cur) {
        fprintf(stderr, "pid351: no core claims %s\n", rom_path);
        return -1;
    }

    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "pid351: cannot open %s\n", rom_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    rom_size = (size_t)sz;
    rom_data = malloc(rom_size);
    if (!rom_data || fread(rom_data, 1, rom_size, f) != rom_size) {
        fclose(f);
        fprintf(stderr, "pid351: cannot read %s\n", rom_path);
        return -1;
    }
    fclose(f);

    /* set_environment before init, per the libretro contract: a core is
     * entitled to make environment calls from inside retro_init and several
     * of them do. */
    cur->set_environment(cb_environment);
    cur->set_video_refresh(cb_video);
    cur->set_audio_sample(cb_audio_sample);
    cur->set_audio_sample_batch(cb_audio_batch);
    cur->set_input_poll(cb_input_poll);
    cur->set_input_state(cb_input_state);
    cur->init();

    struct retro_game_info gi;
    memset(&gi, 0, sizeof gi);
    gi.path = rom_path;
    gi.data = rom_data;
    gi.size = rom_size;
    if (!cur->load_game(&gi)) {
        fprintf(stderr, "pid351: %s refused %s\n", cur->name, rom_path);
        core_close();
        return -1;
    }

    struct retro_system_av_info av;
    memset(&av, 0, sizeof av);
    cur->get_system_av_info(&av);
    core_rate = (int)(av.timing.sample_rate + 0.5);
    core_fps  = (int)(av.timing.fps * 1000.0 + 0.5);

    /* Input frames per output frame, 32.32. The core is run once per panel
     * frame, so it produces core_rate/core_fps samples in the time the codec
     * consumes aud_rate/panel_fps - and the ratio of those two is the whole
     * of the nominal conversion. Both fps values are exact rationals, which
     * is why neither is rounded to a period first. */
    int arate = aud_rate();
    if (arate <= 0) arate = 48000;
    rs_step = (uint64_t)(((double)core_rate / (double)arate)
                         * ((double)PANEL_PIXEL_HZ / (double)PANEL_FRAME_PX)
                         / ((double)core_fps / 1000.0)
                         * 4294967296.0);
    rs_pos = 0;
    ring_w = ring_r = 0;

    /* Prime the ring before anything reads from it, for the same reason the
     * PCM buffer is primed: the core produces almost exactly what we consume
     * each frame, so starting empty means running on the edge of starvation
     * and letting the feedback term crawl up from nothing over minutes. Two
     * frames of game state are discarded to buy it, which is no worse than
     * the emulator having started two frames earlier. */
    while (ring_level() < RING_TARGET && cur)
        cur->run();
    vnew = 0;

    printf("pid351: core %s, %dx%d, %d.%03d fps, %d Hz -> %d Hz "
           "(stretch %+.3f%%)\n",
           cur->name, (int)av.geometry.base_width,
           (int)av.geometry.base_height, core_fps / 1000, core_fps % 1000,
           core_rate, arate,
           ((double)PANEL_PIXEL_HZ / (double)PANEL_FRAME_PX)
               / ((double)core_fps / 1000.0) * 100.0 - 100.0);
    fflush(stdout);
    return 0;
}

void core_close(void)
{
    if (cur) {
        cur->unload_game();
        cur->deinit();
        cur = NULL;
    }
    free(rom_data);
    rom_data = NULL;
    rom_size = 0;
}

const px_t *core_run(uint32_t held, int *w, int *h)
{
    if (!cur)
        return NULL;
    pad_held = held;
    vnew = 0;
    cur->run();
    if (w) *w = vw;
    if (h) *h = vh;
    return vw && vh ? vbuf : NULL;
}

const char *core_name(void)   { return cur ? cur->name : "none"; }
int core_audio_rate(void)     { return core_rate; }
int core_fps_mhz(void)        { return core_fps; }
