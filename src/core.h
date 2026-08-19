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

/* Fast mode: run this many extra emulated frames per panel frame, throwing
 * their picture and their samples away, and output silence while it is on.
 * Set it every frame - there is no state to unwind, and a mode that has to be
 * turned off is a mode that gets left on.
 *
 * Held rather than toggled for the same reason. Fast mode costs battery, and
 * battery is the first priority on this machine; a toggle can be left running
 * in a pocket, a held button cannot. */
void core_fast(int extra);

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

/* Whether any core claims this file by extension. The launcher's whole
 * filter, and the reason ROM discovery does not need its own table. */
int core_accepts(const char *path);

const char *core_name(void);
int core_audio_rate(void);   /* the core's own rate, before resampling */
int core_fps_mhz(void);      /* the core's own refresh, millihertz */

/* Ring occupancy in frames, for the same reason aud_level exists: it is where
 * a rate conversion that is slightly wrong shows up first. */
int core_audio_level(void);

#endif /* CORE_H */
