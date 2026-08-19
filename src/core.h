/* pid351 - emulator cores
 *
 * One integration surface for all four consoles: libretro, statically linked.
 * Every core we target already has a maintained libretro port, and writing
 * four native integrations instead would mean four places to get frame
 * pacing and audio rate conversion wrong rather than one.
 *
 * Statically linked, not dlopen'd, because there is only ever one process and
 * nothing to load a shared object with. Cores are not written to coexist -
 * every one of them exports the same retro_* names and their internals
 * collide too - so each is turned into a single relocatable object whose only
 * global symbols are a prefixed copy of the libretro API. See cores/blob.sh;
 * no core source is patched, which is what keeps re-fetching one cheap.
 *
 * Deliberately not here: cheats, netplay, disk swapping, rewind, the
 * hardware-render interface, and every environment call that exists to
 * support a configurable frontend. There is no configuration.
 */
#ifndef CORE_H
#define CORE_H

#include "pid351.h"

/* Loads a ROM and starts the core that claims its extension. 0 on success. */
int core_open(const char *rom_path);
void core_close(void);

/* Runs exactly one emulated frame and returns the picture, tightly packed
 * RGB565, with its geometry. NULL means the core produced no new frame -
 * which is legal, and means present the previous one again.
 *
 * Called once per panel frame. The core is not asked to run at its own
 * refresh rate: the panel cannot be retuned and the core can, so the console
 * runs at 60.0186 Hz and its audio is stretched to match. The largest such
 * stretch across the four consoles is NES and SNES at 0.13%, about two cents,
 * which is well below what anyone can hear. */
const px_t *core_run(uint32_t held, int *w, int *h);

/* Emits this frame's audio, already resampled to the codec's rate. Separate
 * from core_run because the core hands over its samples during the run and
 * the amount we owe the codec is decided by the panel, not by the core. */
void core_audio(void);

/* Runs one emulated frame and throws away its picture and its samples, and
 * silences this panel frame's audio. Fast mode is a run of these before the
 * frame that is kept.
 *
 * How many to run is the caller's business, because only the caller knows how
 * much of the frame is left - a fixed multiplier is either below what the
 * machine can do or above it, and above it is stutter rather than speed. */
void core_skip(uint32_t held);

/* Savestates, which are the whole of pid351's save system: cartridge battery
 * RAM is inside the state, so there is nothing a .srm file would add.
 *
 * The state is written next to the ROM. There is no slot selection and no
 * saves directory, because there is no config file in which to name one.
 *
 * core_state_undo restores whatever was running immediately before the last
 * load, which is what makes an accidental load survivable. Its only caller is
 * the menu, and it lands unreachable until the menu does.
 *
 * All three return 0 on success. */
int core_state_save(void);
int core_state_load(void);
int core_state_undo(void);

/* Saving is split: core_state_save does the part that must happen on the
 * frame the button was pressed, core_state_tick finishes it a few frames
 * later, and core_state_sync forces that immediately. Call tick once per
 * frame; nothing else is required. See core.c for why. */
/* Frames of writeback the tick allows before it forces the fsync. Exposed
 * only so the session report can say how long the durable half was deferred
 * rather than have the number appear in two places. */
#define SAVE_SETTLE_FRAMES 6

int core_state_tick(void);
int core_state_sync(void);
uint32_t core_state_save_us(void);
uint32_t core_state_sync_us(void);

/* Whether any core claims this file by extension. The launcher's whole
 * filter, and the reason ROM discovery does not need its own table. */
int core_accepts(const char *path);

const char *core_name(void);
int core_audio_rate(void);   /* the core's own rate, before resampling */
int core_fps_mhz(void);      /* the core's own refresh, millihertz */

/* Ring occupancy in frames, for the same reason aud_level exists: it is where
 * a rate conversion that is slightly wrong shows up first. */
int core_audio_level(void);

/* The core's own frame rate in millihertz - 60100 for NTSC NES. The panel is
 * 60018, and the difference is why anything reporting a speed multiple has to
 * divide by this rather than by 60. */
int core_fps_milli(void);

#endif /* CORE_H */
