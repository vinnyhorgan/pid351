/* pid351 - RG351P backend
 *
 * Not implemented yet, on purpose. Rather than guess at ioctls before anyone
 * has looked at what the kernel actually exposes on this unit, plat_init()
 * currently probes the device and reports what it finds, then bails out.
 *
 * That makes the first cross compiled binary useful: it proves the toolchain,
 * proves the transport to the device, and tells us exactly which nodes the
 * real implementation should open.
 *
 * Still to write here:
 *   - DRM/KMS: open card, pick connector + mode, allocate dumb buffers,
 *     double buffer, page flip on vblank (which is also our frame clock).
 *   - RGA (/dev/rga or the v4l2 m2m node): scale + the 90 degree rotate the
 *     portrait panel needs, in hardware, so neither CPU nor GPU wakes up.
 *   - evdev: the internal gamepad, which on the 351M/P is a USB HID device.
 *   - ALSA: RK817 codec, large buffers, few wakeups.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pid351.h"
#include "platform.h"

static void report_dir(const char *path, const char *prefix)
{
    DIR *d = opendir(path);
    if (!d) {
        printf("  %-24s (cannot open)\n", path);
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0)
            printf("  %s/%s\n", path, e->d_name);

    closedir(d);
}

static void report_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return;

    buf[n] = '\0';
    for (size_t i = 0; i < n; i++)
        if (buf[i] == '\n')
            buf[i] = '\0';

    printf("  %-46s %s\n", path, buf);
}

/* Connector state via sysfs rather than ioctls: it needs no libdrm, works the
 * same on a vendor kernel and on mainline, and answers the two questions that
 * decide the display backend - what the connector is called, and whether the
 * mode we are handed is already landscape or still the panel's native
 * portrait (which would mean we owe it a rotation). */
static void report_connectors(void)
{
    DIR *d = opendir("/sys/class/drm");
    if (!d) {
        printf("  /sys/class/drm (cannot open)\n");
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "card", 4) != 0 || strchr(e->d_name, '-') == NULL)
            continue;

        char path[256];
        printf("  connector %s\n", e->d_name);
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", e->d_name);
        report_file(path);
        snprintf(path, sizeof(path), "/sys/class/drm/%s/enabled", e->d_name);
        report_file(path);
        snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", e->d_name);
        report_file(path);
    }

    closedir(d);
}

int plat_init(void)
{
    printf("pid351: reconnaissance build - reporting what this kernel exposes\n\n");

    printf("device:\n");
    report_file("/proc/device-tree/model");
    report_file("/proc/device-tree/compatible");

    printf("\ndrm:\n");
    report_dir("/dev/dri", "card");
    report_dir("/dev/dri", "renderD");
    report_connectors();

    printf("\ninput:\n");
    report_dir("/dev/input", "event");
    report_dir("/dev/input", "js");

    printf("\nrga / v4l2:\n");
    report_dir("/dev", "rga");
    report_dir("/dev", "video");

    printf("\nbacklight:\n");
    report_dir("/sys/class/backlight", "");
    report_file("/sys/class/backlight/backlight/brightness");
    report_file("/sys/class/backlight/backlight/max_brightness");

    printf("\npower:\n");
    report_dir("/sys/class/power_supply", "");
    report_file("/sys/class/power_supply/battery/voltage_now");
    report_file("/sys/class/power_supply/battery/capacity");

    printf("\ncpu:\n");
    report_file("/sys/devices/system/cpu/online");
    report_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    report_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    report_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies");

    printf("\npid351: no display backend yet, exiting.\n");
    return 1;
}

void plat_shutdown(void) { }

uint32_t plat_input(void) { return 0; }

void plat_present(const px_t *fb, int w, int h)
{
    (void)fb; (void)w; (void)h;
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
        return;

    uint64_t delta = deadline_us - now;
    struct timespec ts = {
        .tv_sec  = (time_t)(delta / 1000000u),
        .tv_nsec = (long)((delta % 1000000u) * 1000u),
    };
    nanosleep(&ts, NULL);
}

int plat_should_quit(void) { return 1; }
