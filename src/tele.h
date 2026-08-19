/* pid351 - system telemetry
 *
 * Everything the machine will say about itself, sampled on a clock, from the
 * first instruction of userspace to the last one before the power goes off.
 *
 * This is the half of the instrumentation that is about the machine. The
 * other half - how long a frame took, where the time inside it went - lives
 * in main.c next to the loop that produces it, because it is measured by
 * bracketing code rather than by reading a file. The split is: if it comes
 * out of /proc or /sys, it is here.
 *
 * Three things happen at three rates, which is the whole design:
 *
 *   tele_boot()   at named moments, so a slow start can be attributed to the
 *                 stage that was slow rather than guessed at from the kernel
 *                 log's timestamps.
 *   tele_sample() once a frame, samples on its own schedule. Reading thirty
 *                 files sixty times a second would be a measurement of the
 *                 measurement, so it does it once a second and returns
 *                 immediately the rest of the time.
 *   tele_report() once, at exit, with everything that only means something as
 *                 a difference across the session.
 *
 * Deliberately not here: anything that needs a daemon, a database or a second
 * process. One process, one log, and the log is the kernel ring buffer that
 * plat_boot_save_log copies to the card - which is the only channel off this
 * machine, and is why the volume of it is bounded rather than "everything".
 */
#ifndef TELE_H
#define TELE_H

#include "pid351.h"

/* Records and prints a boot or shutdown milestone with the time since the
 * previous one. Call it at every stage boundary; the cost is a clock read and
 * a printf. The first call also latches the kernel's own uptime, so the whole
 * timeline can be placed against the boot the kernel log describes. */
void tele_boot(const char *stage);

/* Once per frame. Samples the system at TELE_SAMPLE_US and does nothing in
 * between, so the caller does not have to know the rate. */
void tele_sample(uint64_t now_us);

/* The session's system half: totals, deltas, peaks and residencies. Called
 * from the same place as the frame report, before anything is closed, so the
 * numbers describe a running machine rather than one that is shutting down. */
void tele_report(void);

#endif /* TELE_H */
