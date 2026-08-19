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
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "pid351.h"
#include "platform.h"
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
    void p##_retro_unload_game(void); \
    size_t p##_retro_serialize_size(void); \
    bool p##_retro_serialize(void *, size_t); \
    bool p##_retro_unserialize(const void *, size_t)

DECLARE_CORE(nes);

/* A core option we have an opinion about, answered from a compile-time table.
 *
 * Not a config file and not going to become one. The reason this exists at
 * all is that refusing every option does not give a core its own defaults -
 * it gives whatever its code happened to initialise to, which is a different
 * and undocumented thing. fceumm declares its overscan default as 8 and
 * initialises it to 0, so saying nothing produced 256x240 while both we and
 * the core intended 256x224. */
typedef struct { const char *key, *val; } opt_t;

/* Eight scanlines off each end. Every NES pushes garbage into the overscan
 * and every television of the era hid it; more to the point, the scaling
 * policy this project settled on already assumes 256x224 for this console,
 * so cropping is what makes that policy true rather than aspirational. */
static const opt_t nes_opts[] = {
    { "fceumm_overscan_v_top",    "8" },
    { "fceumm_overscan_v_bottom", "8" },
    { "fceumm_overscan_h_left",   "0" },
    { "fceumm_overscan_h_right",  "0" },
    { "fceumm_palette",           "default" },
    { "fceumm_sndquality",        "High" },
    { NULL, NULL },
};

/* Which shell buttons drive which console button, per console.
 *
 * libretro's joypad IDs are SNES-shaped: JOYPAD_B is the bottom of the
 * diamond and JOYPAD_A the right one. A console with fewer buttons than that
 * borrows whichever subset its core picked, so this table is a property of
 * the console rather than of the shell, which is why it sits per core and
 * not in the platform layer.
 *
 * Indexed by RETRO_DEVICE_ID_JOYPAD_*. Entries are masks, so more than one
 * physical button may drive one console button. */
#define PAD_MAP_IDS 16
typedef uint32_t pad_map_t[PAD_MAP_IDS];

/* The NES pad is two buttons side by side, B on the left and A on the right,
 * and B is run while A is jump in the game this was tuned against.
 *
 * Rather than reproduce that pair once, the diamond is split along its
 * diagonal: left and top are run, bottom and right are jump. Every way of
 * holding it then works - Y/B is the SNES diagonal that Super Mario World
 * taught everyone's thumb, Y/A is the true NES horizontal pair, X/A is the
 * upper diagonal and X/B the vertical one. Binding one pair and leaving the
 * others dead was the alternative, and it makes three of the four natural
 * grips silently wrong.
 *
 * The cost is that our B drives the NES's A, so that one label is crossed.
 * Taken deliberately: muscle memory beats silkscreen, and our A still lands
 * on the NES's A.
 *
 * The shoulders stay unbound. A real NES pad has nothing there and turbo was
 * an accessory rather than part of the console, so binding them would invent
 * hardware - and L1/R1 are owned by the GBA and SNES shoulders and by a
 * six-button Genesis pad's top row, which has six face buttons against our
 * four. L2, R2, L3 and R3 are absent for the opposite reason: no console we
 * target has them, which is what frees them for fast mode and the menu. */
static const pad_map_t nes_pad = {
    [RETRO_DEVICE_ID_JOYPAD_B]      = PAD_Y | PAD_X,   /* run  */
    [RETRO_DEVICE_ID_JOYPAD_A]      = PAD_B | PAD_A,   /* jump */
    [RETRO_DEVICE_ID_JOYPAD_UP]     = PAD_UP,
    [RETRO_DEVICE_ID_JOYPAD_DOWN]   = PAD_DOWN,
    [RETRO_DEVICE_ID_JOYPAD_LEFT]   = PAD_LEFT,
    [RETRO_DEVICE_ID_JOYPAD_RIGHT]  = PAD_RIGHT,
    [RETRO_DEVICE_ID_JOYPAD_SELECT] = PAD_SELECT,
    [RETRO_DEVICE_ID_JOYPAD_START]  = PAD_START,
};
typedef struct {
    const char *name;
    const char *exts;          /* space separated, lower case, no dots */
    const opt_t *opts;
    const uint32_t *pad;
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
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
} core_t;

#define CORE_ENTRY(p, nm, ex, op, pd) { \
    nm, ex, op, pd, \
    p##_retro_set_environment, p##_retro_set_video_refresh, \
    p##_retro_set_audio_sample, p##_retro_set_audio_sample_batch, \
    p##_retro_set_input_poll, p##_retro_set_input_state, \
    p##_retro_init, p##_retro_deinit, p##_retro_run, \
    p##_retro_get_system_info, p##_retro_get_system_av_info, \
    p##_retro_load_game, p##_retro_unload_game, \
    p##_retro_serialize_size, p##_retro_serialize, p##_retro_unserialize }

/* Adding a console is one fetch, one blob.sh line and one row here. That is
 * the entire point of the renaming; if it ever takes more, something above
 * has gone wrong. */
static const core_t cores[] = {
    CORE_ENTRY(nes, "fceumm", "nes fds unf unif", nes_opts, nes_pad),
};

#define NCORES ((int)(sizeof cores / sizeof cores[0]))

static const core_t *cur;
static uint32_t pad_held;

/* Set only when a core has explicitly agreed to RGB565.
 *
 * Refusing a format is not enough on its own, which cost an afternoon:
 * fceumm built for 32bpp asks for XRGB8888, and when told no it does not try
 * anything else - it keeps its default XRGB1555 and renders happily into it.
 * The geometry stays perfect and only the colours are wrong, so it reads as a
 * palette bug rather than a format mismatch. A core that never negotiates at
 * all fails the same way. So the agreement is recorded and checked, rather
 * than assumed from the absence of a complaint. */
static int fmt_agreed;

/* ------------------------------------------------------------- video */

/* The core hands back a pointer into its own framebuffer with its own pitch,
 * and the blit wants tight packing. Copying is 123 KB a frame for NES, which
 * measured against a 16.6 ms budget is not worth teaching the blit about
 * pitch for - but it is the first thing to reach for if a core ever lands
 * close to the frame budget. */
static px_t vbuf[PANEL_W * PANEL_H];
static int vw, vh, vnew;

/* Fast mode. Picture and sound are discarded separately because the frame we
 * keep still needs its picture - it is the one reaching the panel - while its
 * samples have to go the same way as the skipped frames'. Anything else
 * leaves one frame of audio in four to be heard, which is the chopped stutter
 * that made fast mode unpleasant rather than merely fast. */
static int discard_v, discard_a, fast_frame;

static void cb_video(const void *data, unsigned width, unsigned height,
                     size_t pitch)
{
    if (discard_v)
        return;

    if (!data || width == 0 || height == 0)
        return;                      /* legal: means "repeat last frame" */
    if ((int)width > PANEL_W || (int)height > PANEL_H) {
        fprintf(stderr, "pid351: core frame %ux%u exceeds panel\n",
                width, height);
        return;
    }

    /* Printed once. Pitch is the only direct evidence of what the core
     * actually decided to render in: 2 bytes per pixel or 4. A format
     * mismatch shows up here as a number, rather than as a picture someone
     * has to look at and judge. */
    static int said;
    if (!said) {
        said = 1;
        printf("pid351: first frame %ux%u pitch=%u (%.1f bytes/pixel)\n",
               width, height, (unsigned)pitch, (double)pitch / width);
        fflush(stdout);
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
    if (discard_a)
        return frames;
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
    /* Fast mode: the resampler is left exactly where it stands - rs_pos,
     * ring_r and ring_w all frozen - and the codec is fed silence by level
     * instead. Freezing rather than running it dry is what lets fast mode end
     * without a click: the ring still holds the samples it held, and the read
     * position is still somewhere inside it. */
    if (fast_frame) {
        aud_silence();
        fast_frame = 0;
        return;
    }

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

        /* Starved. Hold the last sample rather than emit silence: a jump to
         * zero from wherever the waveform was is a step, and a step is a
         * click. Holding is inaudible for the frame or two this can last.
         *
         * The comment here has said "hold the last sample" since the day it
         * was written and the code has always written zeros, which is the
         * kind of disagreement that survives every reading of the file
         * because the comment is what gets read.
         *
         * rs_pos is deliberately not advanced: the samples are not missing,
         * they have not arrived, and the next call should ask for them
         * again. */
        if (idx + 1 >= ring_w) {
            unsigned last = (ring_w - 1) & RING_MASK;
            out[i * 2]     = ring[last * 2];
            out[i * 2 + 1] = ring[last * 2 + 1];
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

int core_fps_milli(void)
{
    return core_fps;
}

/* ------------------------------------------------------------- input */

static void cb_input_poll(void) { }

static int16_t cb_input_state(unsigned port, unsigned device,
                              unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0)
        return 0;
    /* Cores also query RETRO_DEVICE_ID_JOYPAD_MASK (256) when the frontend
     * advertises bitmasks. We do not, so anything past the table is a button
     * this console does not have. */
    if (id >= PAD_MAP_IDS)
        return 0;

    return (pad_held & cur->pad[id]) != 0;
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
        if (f != RETRO_PIXEL_FORMAT_RGB565)
            return false;
        fmt_agreed = 1;
        return true;
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
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = NULL;
        if (!cur || !cur->opts || !v->key)
            return false;
        for (const opt_t *o = cur->opts; o->key; o++)
            if (strcmp(o->key, v->key) == 0) {
                v->value = o->val;
                return true;
            }
        /* No opinion. The core uses its own default, which is the right
         * outcome for the dozens of options we should not be choosing. */
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;    /* compile-time; they never change */
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

/* ------------------------------------------------------- savestates */

/* The only save system pid351 has. Battery-backed cartridge RAM lives inside
 * the state - fceumm registers it with AddExState on every board that has any
 * - so keeping .srm files as well would only buy portability to other
 * emulators, which is not something this machine does.
 *
 * Two buffers rather than one. undo_buf holds whatever was running in the
 * instant before a load, because loading is the only control on the shell
 * that destroys something, and it sits on a stick click that the d-pad alias
 * can trigger by accident. */
static uint8_t *st_buf, *undo_buf;
static size_t st_size;
static int undo_valid;
static char st_path[512];

/* Size is asked for once, after load_game, because it is a property of the
 * cartridge rather than of the console - fceumm's varies with the mapper. */
static int state_alloc(const char *rom_path)
{
    st_size = cur->serialize_size();
    if (st_size == 0) {
        fprintf(stderr, "pid351: %s offers no savestates\n", cur->name);
        return -1;
    }
    st_buf   = malloc(st_size);
    undo_buf = malloc(st_size);
    if (!st_buf || !undo_buf) {
        fprintf(stderr, "pid351: no room for a %zu byte state\n", st_size);
        return -1;
    }
    undo_valid = 0;
    snprintf(st_path, sizeof st_path, "%s.state", rom_path);
    return 0;
}

#ifndef SYNC_FILE_RANGE_WRITE
#define SYNC_FILE_RANGE_WRITE 2
#endif

/* Declared rather than obtained by widening the feature-test macros, which is
 * the same trade plat_drm.c makes for __NR_sync and __NR_syslog and for the
 * same reason: _GNU_SOURCE changes what every other header in the file
 * declares, to get one prototype. */
extern long syscall(long number, ...);

/* Start writeback on a range and return without waiting for it. There is no
 * POSIX call for this, so the syscall directly - see above. */
static void writeback_start(int fd, size_t len)
{
#ifdef __NR_sync_file_range
    syscall(__NR_sync_file_range, fd, (long)0, (long)len,
            SYNC_FILE_RANGE_WRITE);
#else
    (void)fd;
    (void)len;
#endif
}

/* What a state file is. Sixteen bytes in front of the core's own blob.
 *
 * It exists because this no longer calls fsync (see below), so a state file
 * can now be renamed into place before its contents have reached the card.
 * The checksum is what makes that safe: a file that lost the race is
 * detectably wrong rather than quietly wrong, and the previous state is kept
 * beside it to fall back to. Without the checksum, dropping fsync would trade
 * a stutter for silent corruption, which is not a trade.
 *
 * size is the core's serialize_size at the time of writing. A core update
 * that changes it invalidates old states, which is correct - unserialising a
 * blob of the wrong length into a different build is exactly the failure this
 * is here to prevent. */
#define ST_MAGIC   0x31353370u          /* "p351" little endian */
#define ST_VERSION 1u
#define ST_HDR     16

/* Reflected CRC-32, nibble at a time. The table is sixteen entries rather
 * than 256 because it runs on the frame that presses save and the difference
 * between them is a tenth of a millisecond either way, while 1 KB of table
 * would sit in cache the rest of the time doing nothing. */
static uint32_t crc32(const void *buf, size_t len)
{
    static const uint32_t t[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    const uint8_t *p = buf;
    uint32_t c = 0xFFFFFFFFu;

    while (len--) {
        c ^= *p++;
        c = (c >> 4) ^ t[c & 0x0F];
        c = (c >> 4) ^ t[c & 0x0F];
    }
    return c ^ 0xFFFFFFFFu;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Saving, in two parts, because the whole of it does not fit in a frame.
 *
 * Measured: pressing save cost about 93 ms and the panel repeated a frame six
 * times. Splitting it fixed the frame that presses the button - serialise,
 * write, ask for writeback, 808 us - but left fsync costing 63.7 ms a hundred
 * milliseconds later, and every save in two sessions produced exactly one
 * audio xrun and about eleven late frames.
 *
 * Giving it longer to settle was the experiment that decided this. At 100 ms
 * of writeback the fsync took 63.7 ms; at 500 ms it took 76.0 ms. It was
 * never waiting for our 13.8 KB - it is vfat metadata on an SD card, and no
 * amount of waiting shortens it. The rename beside it costs 359 us.
 *
 * So fsync is gone from the play path. Phase two is the rename and nothing
 * else, and durability comes from the checksum above plus keeping the
 * previous state as .bak: a file that was renamed into place before its
 * contents landed fails its own checksum on load and the one before it is
 * still there. Losing power costs at most the newest save, never the game,
 * and never both. core_close syncs, so exiting normally leaves nothing
 * outstanding at all.
 *
 * The rotation order matters and is: write .tmp, drop the old .bak, move
 * .state to .bak, move .tmp to .state. Every interruption of that leaves at
 * least one loadable file on the card.
 */
static int  pend_valid;      /* a save is written but not yet published */
static int  pend_wait;
static char pend_tmp[sizeof st_path + 8];
static uint32_t save_us, rename_us;

uint32_t core_state_save_us(void) { return save_us; }
uint32_t core_state_rename_us(void) { return rename_us; }

/* The half that has to happen on the frame the button was pressed. */
int core_state_save(void)
{
    if (!cur || !st_buf)
        return -1;

    /* A save while one is still in flight finishes that one first rather
     * than abandoning a temp file and its open descriptor. */
    if (pend_valid)
        core_state_sync();

    uint64_t t0 = plat_now_us();
    if (!cur->serialize(st_buf, st_size))
        return -1;

    uint8_t hdr[ST_HDR];
    put32(hdr,      ST_MAGIC);
    put32(hdr + 4,  ST_VERSION);
    put32(hdr + 8,  (uint32_t)st_size);
    put32(hdr + 12, crc32(st_buf, st_size));

    snprintf(pend_tmp, sizeof pend_tmp, "%s.tmp", st_path);
    int fd = open(pend_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    if (write(fd, hdr, ST_HDR) != (ssize_t)ST_HDR
        || write(fd, st_buf, st_size) != (ssize_t)st_size) {
        close(fd);
        unlink(pend_tmp);
        return -1;
    }
    /* Still asked for, even though nothing waits on it now: it costs nothing
     * and it shortens the window in which the checksum has to do its job. */
    writeback_start(fd, ST_HDR + st_size);
    close(fd);
    pend_valid = 1;
    pend_wait  = SAVE_SETTLE_FRAMES;
    save_us   = (uint32_t)(plat_now_us() - t0);
    return 0;
}

/* The durable half. Blocks, so it is called from core_state_tick when the
 * settling frames have passed, and directly at close. */
int core_state_sync(void)
{
    if (!pend_valid)
        return 0;

    uint64_t t0 = plat_now_us();
    char bak[sizeof st_path + 8];
    snprintf(bak, sizeof bak, "%s.bak", st_path);

    pend_valid = 0;
    pend_wait  = 0;

    /* Rotate, then publish. rename over an existing name is atomic enough on
     * vfat for this - it is a directory entry update - and the order means
     * there is never a moment with no loadable state on the card. */
    unlink(bak);
    rename(st_path, bak);
    if (rename(pend_tmp, st_path) != 0) {
        unlink(pend_tmp);
        rename(bak, st_path);
        return -1;
    }
    rename_us = (uint32_t)(plat_now_us() - t0);
    return 0;
}

/* Once per frame. Cheap enough to call unconditionally - it is a load and a
 * branch on every frame that is not settling a save. */
int core_state_tick(void)
{
    if (!pend_valid || --pend_wait > 0)
        return 0;
    return core_state_sync();
}

/* Read one state file into st_buf and say whether it is intact. Everything
 * here is a reason to refuse: wrong magic means it is not ours or predates
 * the header, wrong version or size means it belongs to a different build,
 * and a failed checksum means it was renamed into place before its contents
 * reached the card. */
static int state_read(const char *path)
{
    uint8_t hdr[ST_HDR];
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    if (read(fd, hdr, ST_HDR) != (ssize_t)ST_HDR
        || get32(hdr) != ST_MAGIC
        || get32(hdr + 4) != ST_VERSION
        || get32(hdr + 8) != (uint32_t)st_size
        || read(fd, st_buf, st_size) != (ssize_t)st_size) {
        close(fd);
        return -1;
    }
    close(fd);
    return crc32(st_buf, st_size) == get32(hdr + 12) ? 0 : -1;
}

int core_state_load(void)
{
    if (!cur || !st_buf)
        return -1;

    /* Any save still waiting to be published is published first, so that
     * loading immediately after saving loads what was just saved rather than
     * the one before it. */
    if (pend_valid)
        core_state_sync();

    char bak[sizeof st_path + 8];
    snprintf(bak, sizeof bak, "%s.bak", st_path);

    if (state_read(st_path) != 0) {
        if (state_read(bak) != 0)
            return -1;
        printf("pid351: state failed its checksum, fell back to .bak\n");
        fflush(stdout);
    }

    /* Snapshot before applying, not after: once unserialize returns there is
     * nothing left of the game that was running. */
    undo_valid = cur->serialize(undo_buf, st_size);
    return cur->unserialize(st_buf, st_size) ? 0 : -1;
}

int core_state_undo(void)
{
    if (!cur || !undo_valid)
        return -1;
    undo_valid = 0;
    return cur->unserialize(undo_buf, st_size) ? 0 : -1;
}

int core_accepts(const char *path)
{
    return core_for(path) != NULL;
}

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
    fmt_agreed = 0;
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

    if (!fmt_agreed) {
        fprintf(stderr, "pid351: %s never agreed to RGB565 - it is rendering "
                "in something else and the picture would be wrong. Build it "
                "so it asks for RGB565.\n", cur->name);
        core_close();
        return -1;
    }

    if (state_alloc(rom_path) != 0) {
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

    /* aspect_ratio is the core's own statement of how wide its pixels are,
     * and it is the only opinion on the matter that comes from the emulator
     * rather than from a convention someone repeated. scale.h picks the
     * panel geometry from it, so print it where it can be checked. */
    printf("pid351: core %s, %dx%d, par %.4f, %d.%03d fps, %d Hz -> %d Hz "
           "(stretch %+.3f%%)\n",
           cur->name, (int)av.geometry.base_width,
           (int)av.geometry.base_height,
           (double)av.geometry.aspect_ratio
               * (double)av.geometry.base_height
               / (double)av.geometry.base_width,
           core_fps / 1000, core_fps % 1000,
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
    free(st_buf);
    /* Publish anything outstanding and then force it to the card. This is
     * the one place fsync's cost does not matter, because nothing is being
     * drawn or emulated after it. */
    core_state_sync();
    syscall(__NR_sync);
    free(undo_buf);
    st_buf = undo_buf = NULL;
    st_size = 0;
    undo_valid = 0;
}

const px_t *core_run(uint32_t held, int *w, int *h)
{
    if (!cur)
        return NULL;
    pad_held = held;
    vnew = 0;
    /* The picture is kept; the sound is not, if this frame is one of a fast
     * mode group. */
    discard_a = fast_frame;
    cur->run();
    discard_a = 0;
    if (w) *w = vw;
    if (h) *h = vh;
    return vw && vh ? vbuf : NULL;
}

void core_skip(uint32_t held)
{
    if (!cur)
        return;
    /* Skipped frames run before the kept one so that what reaches the panel
     * is the newest, which is what makes fast mode read as fast motion rather
     * than as dropped frames. */
    pad_held = held;
    discard_v = discard_a = 1;
    cur->run();
    discard_v = discard_a = 0;
    fast_frame = 1;
}

const char *core_name(void)   { return cur ? cur->name : "none"; }
int core_audio_rate(void)     { return core_rate; }
int core_fps_mhz(void)        { return core_fps; }
