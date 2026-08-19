/* pid351 - system telemetry. See tele.h for what belongs here.
 *
 * Every reader in this file returns a sentinel rather than failing, and every
 * caller is expected to print the sentinel rather than hide it. A telemetry
 * module that quietly drops the fields it could not read produces a log that
 * looks complete and is not, which is the specific failure this project has
 * already been bitten by twice - once by a rate-limited /dev/kmsg and once by
 * a codec that was never open.
 *
 * The file readers are a copy of the three in main.c rather than a shared
 * header. That is a deliberate twenty lines of duplication: the alternative
 * is a util.h, and a util.h is where a project like this starts accumulating
 * a standard library it did not need. If a third caller ever wants them, that
 * is when they move.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pid351.h"
#include "platform.h"
#include "tele.h"

/* Once a second. Fast enough that a governor ramp, a thermal step or a
 * charge-current change is visible as a shape rather than as two endpoints;
 * slow enough that the sampling itself is nowhere in the frame budget. Thirty
 * small reads out of procfs at 1 Hz is microseconds per second. */
#define TELE_SAMPLE_US 1000000u

#define NCPU      4
#define NIRQ      48
#define NZONE     8
#define NIDLE     4
#define NSTAGE    24

/* ------------------------------------------------------------- readers */

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

static int read_text(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "rb");
    size_t got;

    if (!f)
        return 0;
    got = fread(out, 1, n - 1, f);
    fclose(f);
    out[got] = 0;
    while (got && (out[got - 1] == '\n' || out[got - 1] == ' '))
        out[--got] = 0;
    return got > 0;
}

/* One named field out of a "Key: value" file - meminfo, vmstat, self/status.
 * Returns missing when the key is absent, which on a kernel we configured
 * ourselves is information rather than an error. */
static long read_field(const char *path, const char *key, long missing)
{
    FILE *f = fopen(path, "rb");
    char line[256];
    size_t klen = strlen(key);
    long v = missing;

    if (!f)
        return missing;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, klen) &&
            (line[klen] == ':' || line[klen] == ' ')) {
            v = strtol(line + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

/* ---------------------------------------------------------- the snapshot */

/* One reading of everything that moves. Kept as a plain struct of longs so
 * that a sample can be differenced against another sample with no special
 * cases: a field nobody could read is -1 at both ends and its difference is
 * meaningless in a way that is visible rather than silent. */
struct snap {
    uint64_t at_us;

    /* /proc/stat, per core, in USER_HZ jiffies */
    long cpu_user[NCPU], cpu_nice[NCPU], cpu_sys[NCPU];
    long cpu_idle[NCPU], cpu_iowait[NCPU], cpu_irq[NCPU], cpu_softirq[NCPU];
    long ctxt, intr, softirq_total, procs_running, procs_blocked, forks;

    /* /proc/interrupts, summed across cores */
    long irq[NIRQ];

    /* us, out of cpuidle - the only direct measure of whether we sleep */
    long idle_time[NCPU][NIDLE], idle_usage[NCPU][NIDLE];

    /* ourselves */
    long minflt, majflt, utime, stime, threads, rss_pages;
    long vm_rss_kb, vm_hwm_kb, vol_ctxt, nonvol_ctxt;
    long sched_run_ns, sched_wait_ns, sched_slices;

    /* memory */
    long mem_free_kb, mem_avail_kb, buffers_kb, cached_kb;
    long slab_kb, dirty_kb, writeback_kb;
    long pgfault, pgmajfault, pgpgin, pgpgout;

    /* clocks and heat */
    long cpu_khz, gpu_hz, ddr_hz;
    long zone_mc[NZONE];

    /* power */
    long capacity, voltage_uv, current_ua, charge_uah, charge_full_uah;
    long backlight, bl_power;

    /* the card, because savestates land on it */
    long mmc_reads, mmc_writes, mmc_rsect, mmc_wsect, mmc_io_ms;

    long uptime_cs;
    long load_milli;
};

/* Names discovered once, because they do not change and parsing them is the
 * expensive part of reading /proc/interrupts. */
static char irq_name[NIRQ][32];
static int  irq_n;
static char zone_name[NZONE][24];
static int  zone_n;
static char idle_name[NIDLE][16];
static int  idle_n;
static const char *supply_dir;

static struct snap first, prev, cur;
static int have_first;
static uint64_t next_sample_us;
static unsigned samples;

/* Peaks, which a difference of two snapshots cannot recover. */
static long peak_zone_mc[NZONE];
static long peak_rss_kb, peak_cpu_khz, peak_current_ua;
static long min_capacity = -1, min_mem_avail_kb = -1;
static long peak_load_milli;

/* --------------------------------------------------------------- pieces */

static void read_stat(struct snap *s)
{
    FILE *f = fopen("/proc/stat", "rb");
    char line[512];

    s->ctxt = s->intr = s->softirq_total = -1;
    s->procs_running = s->procs_blocked = s->forks = -1;
    for (int i = 0; i < NCPU; i++)
        s->cpu_user[i] = s->cpu_nice[i] = s->cpu_sys[i] = s->cpu_idle[i] =
        s->cpu_iowait[i] = s->cpu_irq[i] = s->cpu_softirq[i] = -1;
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "cpu", 3) && line[3] >= '0' && line[3] <= '9') {
            int c = line[3] - '0';
            if (c < NCPU)
                sscanf(line + 4, "%ld %ld %ld %ld %ld %ld %ld",
                       &s->cpu_user[c], &s->cpu_nice[c], &s->cpu_sys[c],
                       &s->cpu_idle[c], &s->cpu_iowait[c], &s->cpu_irq[c],
                       &s->cpu_softirq[c]);
        } else if (!strncmp(line, "ctxt ", 5)) {
            s->ctxt = strtol(line + 5, NULL, 10);
        } else if (!strncmp(line, "intr ", 5)) {
            s->intr = strtol(line + 5, NULL, 10);
        } else if (!strncmp(line, "softirq ", 8)) {
            s->softirq_total = strtol(line + 8, NULL, 10);
        } else if (!strncmp(line, "processes ", 10)) {
            s->forks = strtol(line + 10, NULL, 10);
        } else if (!strncmp(line, "procs_running ", 14)) {
            s->procs_running = strtol(line + 14, NULL, 10);
        } else if (!strncmp(line, "procs_blocked ", 14)) {
            s->procs_blocked = strtol(line + 14, NULL, 10);
        }
    }
    fclose(f);
}

/* Every interrupt line, summed over the four cores. The names are taken on
 * the first pass and the order is assumed stable afterwards, which it is: the
 * table only grows when a driver probes, and every driver on this machine has
 * probed before the first frame. */
static void read_irqs(struct snap *s)
{
    FILE *f = fopen("/proc/interrupts", "rb");
    char line[512];
    int n = 0;

    for (int i = 0; i < NIRQ; i++)
        s->irq[i] = -1;
    if (!f)
        return;
    if (!fgets(line, sizeof line, f)) {      /* the column header */
        fclose(f);
        return;
    }
    while (fgets(line, sizeof line, f) && n < NIRQ) {
        char *p = line, *colon = strchr(line, ':');
        long total = 0;

        if (!colon)
            continue;
        p = colon + 1;
        for (int c = 0; c < NCPU; c++) {
            char *end;
            long v = strtol(p, &end, 10);
            if (end == p)
                break;
            total += v;
            p = end;
        }
        if (!irq_name[n][0]) {
            /* The trailing name, which is the useful part: "ff460000.vop"
             * says more than "27" ever will. Falls back to the number when
             * the line has no name, as the per-cpu IPI lines do not. */
            char *name = strrchr(p, ' ');
            char *nl;
            if (name && name[1]) {
                snprintf(irq_name[n], sizeof irq_name[n], "%s", name + 1);
            } else {
                *colon = 0;
                while (*line == ' ')
                    memmove(line, line + 1, strlen(line));
                snprintf(irq_name[n], sizeof irq_name[n], "%s", line);
            }
            nl = strchr(irq_name[n], '\n');
            if (nl)
                *nl = 0;
        }
        s->irq[n++] = total;
    }
    if (n > irq_n)
        irq_n = n;
    fclose(f);
}

static void read_idle(struct snap *s)
{
    for (int c = 0; c < NCPU; c++)
        for (int i = 0; i < NIDLE; i++)
            s->idle_time[c][i] = s->idle_usage[c][i] = -1;

    for (int c = 0; c < NCPU; c++) {
        for (int i = 0; i < NIDLE; i++) {
            char path[128];
            snprintf(path, sizeof path,
                     "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/time",
                     c, i);
            s->idle_time[c][i] = read_long(path, -1);
            snprintf(path, sizeof path,
                     "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/usage",
                     c, i);
            s->idle_usage[c][i] = read_long(path, -1);
            if (c == 0 && s->idle_time[c][i] >= 0) {
                snprintf(path, sizeof path,
                         "/sys/devices/system/cpu/cpu0/cpuidle/state%d/name",
                         i);
                if (!idle_name[i][0] &&
                    !read_text(path, idle_name[i], sizeof idle_name[i]))
                    snprintf(idle_name[i], sizeof idle_name[i], "s%d", i);
                if (i + 1 > idle_n)
                    idle_n = i + 1;
            }
        }
    }
}

static void read_self(struct snap *s)
{
    FILE *f = fopen("/proc/self/stat", "rb");

    s->minflt = s->majflt = s->utime = s->stime = -1;
    s->threads = s->rss_pages = -1;
    if (f) {
        char buf[1024];
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        buf[got] = 0;
        fclose(f);
        /* Field 2 is the command name in parentheses and can contain spaces,
         * so the scan starts after the last close paren rather than at the
         * beginning. This is the documented way to parse this file and the
         * only reason it is not a one-line sscanf. */
        char *p = strrchr(buf, ')');
        if (p) {
            long d;
            /* state, ppid, pgrp, session, tty, tpgid, flags */
            int n = sscanf(p + 2, "%*c %ld %ld %ld %ld %ld %*u "
                                  "%ld %*u %ld %*u %ld %ld",
                           &d, &d, &d, &d, &d,
                           &s->minflt, &s->majflt, &s->utime, &s->stime);
            (void)n;
        }
    }
    s->vm_rss_kb    = read_field("/proc/self/status", "VmRSS", -1);
    s->vm_hwm_kb    = read_field("/proc/self/status", "VmHWM", -1);
    s->threads      = read_field("/proc/self/status", "Threads", -1);
    s->vol_ctxt     = read_field("/proc/self/status",
                                 "voluntary_ctxt_switches", -1);
    s->nonvol_ctxt  = read_field("/proc/self/status",
                                 "nonvoluntary_ctxt_switches", -1);

    s->sched_run_ns = s->sched_wait_ns = s->sched_slices = -1;
    f = fopen("/proc/self/schedstat", "rb");
    if (f) {
        if (fscanf(f, "%ld %ld %ld", &s->sched_run_ns, &s->sched_wait_ns,
                   &s->sched_slices) != 3)
            s->sched_run_ns = -1;
        fclose(f);
    }
}

static void read_mem(struct snap *s)
{
    s->mem_free_kb  = read_field("/proc/meminfo", "MemFree", -1);
    s->mem_avail_kb = read_field("/proc/meminfo", "MemAvailable", -1);
    s->buffers_kb   = read_field("/proc/meminfo", "Buffers", -1);
    s->cached_kb    = read_field("/proc/meminfo", "Cached", -1);
    s->slab_kb      = read_field("/proc/meminfo", "Slab", -1);
    s->dirty_kb     = read_field("/proc/meminfo", "Dirty", -1);
    s->writeback_kb = read_field("/proc/meminfo", "Writeback", -1);
    s->pgfault      = read_field("/proc/vmstat", "pgfault", -1);
    s->pgmajfault   = read_field("/proc/vmstat", "pgmajfault", -1);
    s->pgpgin       = read_field("/proc/vmstat", "pgpgin", -1);
    s->pgpgout      = read_field("/proc/vmstat", "pgpgout", -1);
}

static void read_thermal(struct snap *s)
{
    for (int i = 0; i < NZONE; i++) {
        char path[96];

        snprintf(path, sizeof path,
                 "/sys/class/thermal/thermal_zone%d/temp", i);
        s->zone_mc[i] = read_long(path, -1);
        if (s->zone_mc[i] == -1)
            continue;
        if (i + 1 > zone_n)
            zone_n = i + 1;
        if (!zone_name[i][0]) {
            snprintf(path, sizeof path,
                     "/sys/class/thermal/thermal_zone%d/type", i);
            if (!read_text(path, zone_name[i], sizeof zone_name[i]))
                snprintf(zone_name[i], sizeof zone_name[i], "zone%d", i);
        }
    }
}

/* The battery is under one of two names depending on whose driver is loaded,
 * which cost this project two boots once already. Settled once here rather
 * than tried on every field of every sample. */
static void find_supply(void)
{
    static const char *const cand[] = {
        "/sys/class/power_supply/rk817-battery",
        "/sys/class/power_supply/battery",
        "/sys/class/power_supply/BAT0", NULL };
    char path[160];

    for (int i = 0; cand[i]; i++) {
        snprintf(path, sizeof path, "%s/capacity", cand[i]);
        if (read_long(path, -1) != -1) {
            supply_dir = cand[i];
            return;
        }
    }
}

static long supply_long(const char *leaf)
{
    char path[192];

    if (!supply_dir)
        return -1;
    snprintf(path, sizeof path, "%s/%s", supply_dir, leaf);
    return read_long(path, -1);
}

static void read_power(struct snap *s)
{
    s->capacity        = supply_long("capacity");
    s->voltage_uv      = supply_long("voltage_avg");
    if (s->voltage_uv < 0)
        s->voltage_uv  = supply_long("voltage_now");
    s->current_ua      = supply_long("current_avg");
    if (s->current_ua == -1)
        s->current_ua  = supply_long("current_now");
    s->charge_uah      = supply_long("charge_now");
    s->charge_full_uah = supply_long("charge_full");
    s->backlight = read_long("/sys/class/backlight/backlight/brightness", -1);
    s->bl_power  = read_long("/sys/class/backlight/backlight/bl_power", -1);
}

static void read_disk(struct snap *s)
{
    FILE *f = fopen("/proc/diskstats", "rb");
    char line[256];

    s->mmc_reads = s->mmc_writes = s->mmc_rsect = s->mmc_wsect = -1;
    s->mmc_io_ms = -1;
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char name[32];
        long r, rm, rs, rt, w, wm, ws, wt, inflight, io_ms;

        if (sscanf(line, "%*d %*d %31s %ld %ld %ld %ld %ld %ld %ld %ld "
                         "%ld %ld",
                   name, &r, &rm, &rs, &rt, &w, &wm, &ws, &wt,
                   &inflight, &io_ms) != 11)
            continue;
        if (strcmp(name, "mmcblk0"))
            continue;
        s->mmc_reads  = r;
        s->mmc_writes = w;
        s->mmc_rsect  = rs;
        s->mmc_wsect  = ws;
        s->mmc_io_ms  = io_ms;
        break;
    }
    fclose(f);
}

static void read_freq(struct snap *s)
{
    s->cpu_khz = read_long(
        "/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq", -1);
    s->gpu_hz  = read_long("/sys/class/devfreq/ff400000.gpu/cur_freq", -1);
    s->ddr_hz  = read_long("/sys/class/devfreq/dmc/cur_freq", -1);
}

static void snap_take(struct snap *s, uint64_t now_us)
{
    char buf[128];

    memset(s, 0, sizeof *s);
    s->at_us = now_us;
    read_stat(s);
    read_irqs(s);
    read_idle(s);
    read_self(s);
    read_mem(s);
    read_thermal(s);
    read_power(s);
    read_disk(s);
    read_freq(s);

    s->uptime_cs = -1;
    if (read_text("/proc/uptime", buf, sizeof buf)) {
        double up = strtod(buf, NULL);
        s->uptime_cs = (long)(up * 100.0);
    }
    s->load_milli = -1;
    if (read_text("/proc/loadavg", buf, sizeof buf))
        s->load_milli = (long)(strtod(buf, NULL) * 1000.0);
}

static void peaks_update(const struct snap *s)
{
    for (int i = 0; i < zone_n; i++)
        if (s->zone_mc[i] > peak_zone_mc[i])
            peak_zone_mc[i] = s->zone_mc[i];
    if (s->vm_rss_kb > peak_rss_kb)   peak_rss_kb = s->vm_rss_kb;
    if (s->cpu_khz > peak_cpu_khz)    peak_cpu_khz = s->cpu_khz;
    if (s->load_milli > peak_load_milli) peak_load_milli = s->load_milli;
    /* Current is negative while discharging on this gauge, so the peak draw
     * is the most negative reading. Signs on this chip have been got wrong
     * before; the report states which way round it is rather than assume the
     * reader remembers. */
    if (s->current_ua < peak_current_ua) peak_current_ua = s->current_ua;
    if (s->capacity >= 0 && (min_capacity < 0 || s->capacity < min_capacity))
        min_capacity = s->capacity;
    if (s->mem_avail_kb >= 0 &&
        (min_mem_avail_kb < 0 || s->mem_avail_kb < min_mem_avail_kb))
        min_mem_avail_kb = s->mem_avail_kb;
}

/* ------------------------------------------------------------- the line */

/* Busy percent for one core between two snapshots, in tenths. Idle and iowait
 * both count as not-us; on a machine with one process and no disk in the
 * frame loop the second is nearly always zero, and when it is not that is
 * exactly the sort of thing this exists to catch. */
static int cpu_busy_tenths(const struct snap *a, const struct snap *b, int c)
{
    long busy, idle, total;

    if (a->cpu_user[c] < 0 || b->cpu_user[c] < 0)
        return -1;
    busy = (b->cpu_user[c] - a->cpu_user[c])
         + (b->cpu_nice[c] - a->cpu_nice[c])
         + (b->cpu_sys[c] - a->cpu_sys[c])
         + (b->cpu_irq[c] - a->cpu_irq[c])
         + (b->cpu_softirq[c] - a->cpu_softirq[c]);
    idle = (b->cpu_idle[c] - a->cpu_idle[c])
         + (b->cpu_iowait[c] - a->cpu_iowait[c]);
    total = busy + idle;
    if (total <= 0)
        return -1;
    return (int)(busy * 1000 / total);
}

static long d(long a, long b)
{
    return (a < 0 || b < 0) ? -1 : b - a;
}

/* One dense line a second, key=value, raw units. Deliberately ugly and
 * deliberately greppable: this is the series a plot gets made from later, and
 * every prettification is a place for a unit conversion to be wrong in a way
 * nobody can check afterwards. */
static void print_line(void)
{
    double dt = (double)(cur.at_us - prev.at_us) / 1000000.0;
    char cpus[64] = "";
    char zones[96] = "";
    int off = 0;

    for (int c = 0; c < NCPU; c++) {
        int t = cpu_busy_tenths(&prev, &cur, c);
        off += snprintf(cpus + off, sizeof cpus - (size_t)off, "%s%d.%d",
                        c ? "/" : "", t < 0 ? 0 : t / 10, t < 0 ? 0 : t % 10);
    }
    off = 0;
    for (int i = 0; i < zone_n; i++)
        off += snprintf(zones + off, sizeof zones - (size_t)off, "%s%ld",
                        i ? "/" : "", cur.zone_mc[i]);

    printf("pid351: T t=%.1f up=%ld.%02ld cpu=%s%% khz=%ld gpu=%ld ddr=%ld "
           "temp=%s load=%ld ctxt=%ld intr=%ld sirq=%ld run=%ld blk=%ld "
           "rss=%ld free=%ld avail=%ld dirty=%ld wb=%ld slab=%ld "
           "flt=%ld/%ld vctx=%ld nvctx=%ld schedwait=%ld "
           "cap=%ld uv=%ld ua=%ld uah=%ld bl=%ld "
           "mmcr=%ld mmcw=%ld mmcms=%ld\n",
           (double)(cur.at_us - first.at_us) / 1000000.0,
           cur.uptime_cs / 100, cur.uptime_cs % 100,
           cpus, cur.cpu_khz, cur.gpu_hz, cur.ddr_hz, zones, cur.load_milli,
           (long)((double)d(prev.ctxt, cur.ctxt) / dt),
           (long)((double)d(prev.intr, cur.intr) / dt),
           (long)((double)d(prev.softirq_total, cur.softirq_total) / dt),
           cur.procs_running, cur.procs_blocked,
           cur.vm_rss_kb, cur.mem_free_kb, cur.mem_avail_kb,
           cur.dirty_kb, cur.writeback_kb, cur.slab_kb,
           d(prev.minflt, cur.minflt), d(prev.majflt, cur.majflt),
           d(prev.vol_ctxt, cur.vol_ctxt),
           d(prev.nonvol_ctxt, cur.nonvol_ctxt),
           d(prev.sched_wait_ns, cur.sched_wait_ns) / 1000,
           cur.capacity, cur.voltage_uv, cur.current_ua, cur.charge_uah,
           cur.backlight,
           d(prev.mmc_reads, cur.mmc_reads),
           d(prev.mmc_writes, cur.mmc_writes),
           d(prev.mmc_io_ms, cur.mmc_io_ms));
}

/* -------------------------------------------------------------- timeline */

static struct {
    const char *name;
    uint64_t at_us;
} stage[NSTAGE];
static int stage_n;
static uint64_t stage_first_us;
static long stage_first_uptime_cs = -1;
static int  uptime_stage = -1;

void tele_boot(const char *stage_name)
{
    uint64_t now = plat_now_us();
    char buf[64];

    /* The clock is latched on the very first call and the files are retried
     * on every call until they answer. Both halves matter: the first call
     * happens before PID 1 has mounted /proc, so a one-shot latch would leave
     * the kernel's uptime and the battery unread for the whole session - and
     * moving the first call later to avoid that would put the origin of every
     * timestamp after the part of the boot most worth measuring. */
    if (!stage_n)
        stage_first_us = now;
    if (stage_first_uptime_cs < 0 && read_text("/proc/uptime", buf, sizeof buf)) {
        stage_first_uptime_cs = (long)(strtod(buf, NULL) * 100.0);
        uptime_stage = stage_n;
    }
    if (!supply_dir)
        find_supply();
    if (stage_n < NSTAGE) {
        stage[stage_n].name  = stage_name;
        stage[stage_n].at_us = now;
        stage_n++;
    }
    /* Printed as it happens as well as collected, because a stage that hangs
     * never reaches the report and the last line printed is then the whole
     * of the diagnosis. */
    printf("pid351: stage %-12s +%6.3f s (%.3f s in)\n", stage_name,
           stage_n > 1 ? (double)(now - stage[stage_n - 2].at_us) / 1e6 : 0.0,
           (double)(now - stage_first_us) / 1e6);
}

/* ---------------------------------------------------------------- public */

void tele_sample(uint64_t now_us)
{
    if (!have_first) {
        snap_take(&first, now_us);
        peaks_update(&first);
        prev = first;
        have_first = 1;
        next_sample_us = now_us + TELE_SAMPLE_US;
        return;
    }
    if (now_us < next_sample_us)
        return;
    next_sample_us = now_us + TELE_SAMPLE_US;
    snap_take(&cur, now_us);
    peaks_update(&cur);
    print_line();
    samples++;
    prev = cur;
}

void tele_report(void)
{
    struct snap last;
    double secs;

    snap_take(&last, plat_now_us());
    peaks_update(&last);
    if (!have_first)
        first = last;
    secs = (double)(last.at_us - first.at_us) / 1000000.0;
    if (secs <= 0.0)
        secs = 1e-6;

    printf("pid351: ---- system ----\n");
    printf("pid351: sampled %u times over %.1f s at %u us\n",
           samples, secs, (unsigned)TELE_SAMPLE_US);

    printf("pid351: boot timeline (kernel was %ld.%02ld s in at stage %s):\n",
           stage_first_uptime_cs / 100, stage_first_uptime_cs % 100,
           uptime_stage >= 0 && uptime_stage < stage_n
               ? stage[uptime_stage].name : "?");
    for (int i = 0; i < stage_n; i++)
        printf("pid351:   %-12s +%7.3f s  (%7.3f s in)\n", stage[i].name,
               i ? (double)(stage[i].at_us - stage[i - 1].at_us) / 1e6 : 0.0,
               (double)(stage[i].at_us - stage_first_us) / 1e6);

    printf("pid351: cpu busy over the session:");
    for (int c = 0; c < NCPU; c++) {
        int t = cpu_busy_tenths(&first, &last, c);
        printf(" cpu%d=%d.%d%%", c, t < 0 ? 0 : t / 10, t < 0 ? 0 : t % 10);
    }
    printf("\n");

    /* Our own time on a CPU against the wall clock, which is the honest
     * answer to "how much of this machine does pid351 use" - the per-core
     * figures above include the kernel's own work on our behalf. */
    printf("pid351: process: utime %ld jiffies, stime %ld, on cpu %.3f s, "
           "waiting to run %.3f s over %ld slices\n",
           d(first.utime, last.utime), d(first.stime, last.stime),
           (double)d(first.sched_run_ns, last.sched_run_ns) / 1e9,
           (double)d(first.sched_wait_ns, last.sched_wait_ns) / 1e9,
           d(first.sched_slices, last.sched_slices));
    printf("pid351: process: rss %ld kB (peak %ld), threads %ld, "
           "faults %ld minor %ld major, ctxt %ld voluntary %ld not\n",
           last.vm_rss_kb, last.vm_hwm_kb > peak_rss_kb ? last.vm_hwm_kb
                                                        : peak_rss_kb,
           last.threads, d(first.minflt, last.minflt),
           d(first.majflt, last.majflt),
           d(first.vol_ctxt, last.vol_ctxt),
           d(first.nonvol_ctxt, last.nonvol_ctxt));

    /* Idle residency, per core, per state. The one number that says whether
     * "never busy-wait" is being obeyed by the machine and not just by the
     * source: a core that never enters an idle state is a core spinning
     * somewhere, and no amount of reading the loop will show that. */
    for (int i = 0; i < idle_n; i++) {
        long tot = 0;
        int any = 0;
        printf("pid351: idle %-8s", idle_name[i]);
        for (int c = 0; c < NCPU; c++) {
            long us = d(first.idle_time[c][i], last.idle_time[c][i]);
            long n  = d(first.idle_usage[c][i], last.idle_usage[c][i]);
            if (us < 0) {
                printf(" cpu%d=-", c);
                continue;
            }
            any = 1;
            tot += us;
            printf(" cpu%d=%.1f%%/%ldx", c,
                   (double)us / 10000.0 / secs, n);
        }
        if (any)
            printf("  total %.2f core-seconds", (double)tot / 1e6);
        printf("\n");
    }

    /* The low water is on available rather than on free, because free is not
     * the number that runs out: the kernel will evict page cache before it
     * fails an allocation, so a free that dips to nothing while available
     * holds is a working machine and the reverse is not. */
    printf("pid351: memory: free %ld kB, available %ld (low water %ld), "
           "cached %ld, slab %ld, dirty %ld, writeback %ld\n",
           last.mem_free_kb, last.mem_avail_kb, min_mem_avail_kb,
           last.cached_kb, last.slab_kb, last.dirty_kb, last.writeback_kb);
    printf("pid351: paging: %ld faults, %ld major, %ld kB in, %ld kB out\n",
           d(first.pgfault, last.pgfault),
           d(first.pgmajfault, last.pgmajfault),
           d(first.pgpgin, last.pgpgin), d(first.pgpgout, last.pgpgout));

    printf("pid351: switching: %ld context switches (%.0f/s), %ld interrupts "
           "(%.0f/s), %ld softirqs (%.0f/s), %ld forks\n",
           d(first.ctxt, last.ctxt),
           (double)d(first.ctxt, last.ctxt) / secs,
           d(first.intr, last.intr),
           (double)d(first.intr, last.intr) / secs,
           d(first.softirq_total, last.softirq_total),
           (double)d(first.softirq_total, last.softirq_total) / secs,
           d(first.forks, last.forks));

    /* Every interrupt that fired, by name. The vblank line should land within
     * a hair of the panel rate and the i2s line within a hair of the codec
     * period; anything else moving fast is something we did not know was
     * running, which on a machine with one process is worth knowing. */
    printf("pid351: interrupts over the session:\n");
    for (int i = 0; i < irq_n; i++) {
        long n = d(first.irq[i], last.irq[i]);
        if (n <= 0)
            continue;
        printf("pid351:   %-20s %8ld  %8.2f/s\n",
               irq_name[i][0] ? irq_name[i] : "?", n, (double)n / secs);
    }

    printf("pid351: thermal:");
    for (int i = 0; i < zone_n; i++)
        printf(" %s=%ld.%01ld(peak %ld.%01ld)", zone_name[i],
               last.zone_mc[i] / 1000, (last.zone_mc[i] % 1000) / 100,
               peak_zone_mc[i] / 1000, (peak_zone_mc[i] % 1000) / 100);
    printf(" C\n");

    printf("pid351: clocks: cpu %ld kHz (peak %ld), gpu %ld Hz, ddr %ld Hz, "
           "load %ld milli (peak %ld)\n",
           last.cpu_khz, peak_cpu_khz, last.gpu_hz, last.ddr_hz,
           last.load_milli, peak_load_milli);

    printf("pid351: card: %ld reads, %ld writes, %ld sectors read, "
           "%ld written, %ld ms of io\n",
           d(first.mmc_reads, last.mmc_reads),
           d(first.mmc_writes, last.mmc_writes),
           d(first.mmc_rsect, last.mmc_rsect),
           d(first.mmc_wsect, last.mmc_wsect),
           d(first.mmc_io_ms, last.mmc_io_ms));

    printf("pid351: gauge: %s, capacity %ld%% (low %ld), %ld uV, %ld uA "
           "(peak draw %ld, negative is out), charge %ld of %ld uAh, "
           "backlight %ld\n",
           supply_dir ? supply_dir : "no battery found",
           last.capacity, min_capacity, last.voltage_uv, last.current_ua,
           peak_current_ua, last.charge_uah, last.charge_full_uah,
           last.backlight);
}
