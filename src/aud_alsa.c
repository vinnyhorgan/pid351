/* pid351 - audio via raw ALSA ioctls
 *
 * See aud.h for what this is and why there is only one of it. This file is
 * the ioctl mechanics.
 *
 * What it deliberately does not do: poll. Nothing here ever waits for the
 * card to become ready, because the video loop is already blocked on vblank
 * once per frame and a second thing to wait on would mean choosing which
 * clock is in charge. The panel is in charge. This just keeps a buffer fed.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>

#include <sound/asound.h>

#include "pid351.h"
#include "platform.h"
#include "aud.h"

/* Card 0, device 0. There is exactly one card on the handheld - the rk817
 * codec off rockchip-i2s - so there is nothing to search for. If this is
 * wrong the open fails and we print what /dev/snd actually holds, which on a
 * machine with no serial port is the only way that answer ever reaches us. */
#define AUD_NODE "/dev/snd/pcmC0D0p"

/* What we ask for. The codec decides; these are the opening bid.
 *
 * 512-frame periods over an 8-period buffer is 85 ms of cushion at 48 kHz and
 * 94 interrupts a second. Battery-first would argue for longer periods and
 * fewer interrupts, and the argument loses: the gamepad's USB bus already
 * costs about 6700 interrupts a second, so 94 more is not measurable, whereas
 * the added latency would be audible. Where a power term is below the noise
 * floor of a term we cannot remove, it is not a power term. */
#define AUD_RATE     48000
#define AUD_CHANNELS 2
#define AUD_PERIOD   512
#define AUD_PERIODS  8

/* The card probes asynchronously and finishes later than the display does,
 * so as PID 1 the node is reliably absent by the time we get here. Only
 * needed when we are init: on the laptop udev has already waited for this,
 * and blocking the development loop for eight seconds to discover a laptop
 * has no sound card would be a poor trade. */
#define AUD_WAIT_MS 8000

static int wait_for_card(void)
{
    if (!plat_is_init() || access(AUD_NODE, F_OK) == 0)
        return access(AUD_NODE, F_OK);
    for (int waited = 0; waited < AUD_WAIT_MS; waited += 20) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (access(AUD_NODE, F_OK) == 0) {
            printf("pid351: audio node appeared after %d ms\n", waited + 20);
            return 0;
        }
    }
    return -1;
}

static int fd = -1;
static unsigned rate, period, buffer;
static int xrun_count;

/* Exact resampling state: acc counts pixel-clock ticks owed, never seconds.
 * See PANEL_FRAME_PX in pid351.h for why it has to be this and not a period
 * in microseconds. */
static uint64_t acc;

/* ------------------------------------------------------ hw_params helpers */

static struct snd_mask *mask_of(struct snd_pcm_hw_params *p, int k)
{
    return &p->masks[k - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval *iv_of(struct snd_pcm_hw_params *p, int k)
{
    return &p->intervals[k - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* "Any": every mask bit set and every interval unbounded. The kernel narrows
 * this down rather than us building up, which is why a refine can report what
 * the card will take instead of only whether our guess was legal. */
static void params_any(struct snd_pcm_hw_params *p)
{
    memset(p, 0, sizeof *p);
    for (int k = SNDRV_PCM_HW_PARAM_FIRST_MASK;
         k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++)
        memset(mask_of(p, k), 0xff, sizeof(struct snd_mask));
    for (int k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
         k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++)
        iv_of(p, k)->max = UINT_MAX;
    p->rmask = ~0u;
    p->info = ~0u;
}

static void set_mask(struct snd_pcm_hw_params *p, int k, unsigned v)
{
    struct snd_mask *m = mask_of(p, k);
    memset(m, 0, sizeof *m);
    m->bits[v >> 5] |= 1u << (v & 31u);
}

static void set_iv(struct snd_pcm_hw_params *p, int k, unsigned v)
{
    struct snd_interval *i = iv_of(p, k);
    i->min = i->max = v;
    i->integer = 1;
}

static unsigned got_iv(struct snd_pcm_hw_params *p, int k)
{
    return iv_of(p, k)->min;
}

/* The format is fixed everywhere else in pid351, so only these three vary. */
static void set_common(struct snd_pcm_hw_params *p)
{
    params_any(p);
    set_mask(p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    set_mask(p, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    set_mask(p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
}

static void report_ranges(void)
{
    struct snd_pcm_hw_params p;
    set_common(&p);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) < 0) {
        printf("pid351: audio refine failed: %s\n", strerror(errno));
        return;
    }
    static const struct { const char *name; int k; } show[] = {
        { "channels",    SNDRV_PCM_HW_PARAM_CHANNELS    },
        { "rate",        SNDRV_PCM_HW_PARAM_RATE        },
        { "period_size", SNDRV_PCM_HW_PARAM_PERIOD_SIZE },
        { "periods",     SNDRV_PCM_HW_PARAM_PERIODS     },
        { "buffer_size", SNDRV_PCM_HW_PARAM_BUFFER_SIZE },
    };
    for (size_t i = 0; i < sizeof show / sizeof show[0]; i++) {
        struct snd_interval *v = iv_of(&p, show[i].k);
        printf("pid351: audio accepts %-12s %u .. %u\n",
               show[i].name, v->min, v->max);
    }
}

/* Only ever called when the open failed, and only exists because the device
 * cannot be asked interactively what it enumerated. */
static void list_snd(void)
{
    DIR *d = opendir("/dev/snd");
    if (!d) {
        printf("pid351: no /dev/snd at all\n");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
            printf("pid351: /dev/snd/%s\n", e->d_name);
    closedir(d);
}

/* ---------------------------------------------------------------- priming */

static int prime(void)
{
    /* Start with the buffer half full rather than empty. The panel and the
     * codec run off independent crystals, so the level will wander no matter
     * how exact our arithmetic is; starting in the middle gives that wander
     * somewhere to go in both directions before it becomes audible. */
    static const int16_t quiet[AUD_PERIOD * AUD_CHANNELS];
    unsigned want = buffer / 2;
    while (want > 0) {
        unsigned n = want < AUD_PERIOD ? want : AUD_PERIOD;
        struct snd_xferi x = { .buf = (void *)quiet, .frames = n, .result = 0 };
        if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0)
            return -1;
        want -= n;
    }
    return 0;
}

int aud_open(void)
{
    if (wait_for_card() != 0)
        printf("pid351: audio node %s never appeared\n", AUD_NODE);

    /* O_NONBLOCK on the open only. A PCM node another process already holds
     * makes open() *wait* rather than fail, with no timeout - and a silent
     * indefinite wait is the one behaviour this program cannot afford. As
     * PID 1 there is no shell to interrupt it from, nothing has been drawn
     * yet, and the boot log is still in a buffer nobody will ever read. It
     * hangs looking exactly like a dead machine.
     *
     * Cleared again immediately, because blocking writes are what let the
     * hardware pace us and are the whole reason aud_write is allowed to
     * wait at all. */
    fd = open(AUD_NODE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        printf("pid351: audio open %s: %s\n", AUD_NODE, strerror(errno));
        list_snd();
        return -1;
    }

    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) {
        printf("pid351: audio could not restore blocking mode: %s\n",
               strerror(errno));
        aud_close();
        return -1;
    }

    /* Printed unconditionally, not only on failure. Every constant above is a
     * guess until a device confirms it, and this is the line that turns the
     * guess into a measurement we can hardcode. */
    report_ranges();

    struct snd_pcm_hw_params hp;
    set_common(&hp);
    set_iv(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, AUD_CHANNELS);
    set_iv(&hp, SNDRV_PCM_HW_PARAM_RATE, AUD_RATE);
    set_iv(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, AUD_PERIOD);
    set_iv(&hp, SNDRV_PCM_HW_PARAM_PERIODS, AUD_PERIODS);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) < 0) {
        printf("pid351: audio hw_params: %s\n", strerror(errno));
        aud_close();
        return -1;
    }

    rate   = got_iv(&hp, SNDRV_PCM_HW_PARAM_RATE);
    period = got_iv(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
    buffer = got_iv(&hp, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);

    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof sw);
    sw.tstamp_mode     = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step     = 1;
    sw.avail_min       = period;
    /* Start as soon as priming finishes, and treat a dry buffer as an error
     * rather than silently restarting: a gap we know about is a number in the
     * boot log, and a gap we do not is a mystery on a machine that cannot be
     * questioned. */
    sw.start_threshold = buffer / 2;
    sw.stop_threshold  = buffer;
    sw.boundary        = buffer;
    while (sw.boundary * 2 <= (unsigned long)LONG_MAX - buffer)
        sw.boundary *= 2;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        printf("pid351: audio sw_params: %s\n", strerror(errno));
        aud_close();
        return -1;
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0) {
        printf("pid351: audio prepare: %s\n", strerror(errno));
        aud_close();
        return -1;
    }

    acc = 0;
    xrun_count = 0;
    if (prime() < 0) {
        printf("pid351: audio prime: %s\n", strerror(errno));
        aud_close();
        return -1;
    }

    printf("pid351: audio %u Hz, period %u, buffer %u (%u.%01u ms), "
           "%d frames/video frame\n",
           rate, period, buffer,
           1000u * buffer / rate, (10000u * buffer / rate) % 10u,
           (int)((uint64_t)rate * PANEL_FRAME_PX / PANEL_PIXEL_HZ));
    return 0;
}

void aud_close(void)
{
    if (fd < 0)
        return;
    ioctl(fd, SNDRV_PCM_IOCTL_DROP, 0);
    close(fd);
    fd = -1;
    rate = period = buffer = 0;
}

int aud_rate(void)
{
    return (int)rate;
}

int aud_xruns(void)
{
    return xrun_count;
}

int aud_due(void)
{
    if (fd < 0)
        return 0;
    /* Exactly rate * 283240 / 17000000 samples per frame, carried in integer
     * pixel-clock ticks so the remainder is never thrown away. At 48 kHz this
     * yields 800, 800, 800, 799, ... and the long-run rate is exact. */
    acc += (uint64_t)rate * PANEL_FRAME_PX;
    int n = (int)(acc / PANEL_PIXEL_HZ);
    acc %= PANEL_PIXEL_HZ;
    return n;
}

/* A dry buffer leaves the stream in SETUP or XRUN and every write after it
 * fails until the stream is prepared again. Re-priming rather than just
 * preparing is deliberate: coming back with an empty buffer means running dry
 * again on the next hiccup, which turns one glitch into a series. */
static int recover(void)
{
    xrun_count++;
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0)
        return -1;
    return prime();
}

int aud_write(const int16_t *frames, int n)
{
    if (fd < 0 || n <= 0)
        return 0;

    int done = 0;
    while (done < n) {
        struct snd_xferi x = {
            .buf    = (void *)(frames + (size_t)done * AUD_CHANNELS),
            .frames = (snd_pcm_uframes_t)(n - done),
            .result = 0,
        };
        if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EPIPE || errno == ESTRPIPE) {
                if (recover() < 0)
                    return -1;
                continue;
            }
            return -1;
        }
        if (x.result <= 0)
            break;
        done += (int)x.result;
    }
    return done;
}

int aud_level(void)
{
    if (fd < 0)
        return -1;
    struct snd_pcm_status st;
    memset(&st, 0, sizeof st);
    if (ioctl(fd, SNDRV_PCM_IOCTL_STATUS, &st) < 0)
        return -1;
    /* avail is room to write, so what is queued is whatever is left. */
    return (int)buffer - (int)st.avail;
}
