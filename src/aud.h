/* pid351 - audio
 *
 * One implementation for both targets, which is the opposite of how the
 * display works and worth saying why: the laptop and the handheld are both
 * Linux with ALSA, so there is nothing here to abstract. The ioctls that run
 * on the laptop are the ioctls that run on the device, which makes the laptop
 * a real test rig for this subsystem rather than a simulation of one. The
 * only thing that differs is which card answers and what it will accept, and
 * both of those are reported at open rather than guessed.
 *
 * Raw ioctls, no alsa-lib, for the same reason the display talks to DRM
 * directly: the whole path is seven ioctls, and a library would be a
 * dependency, a link-time problem for a static binary, and a layer between us
 * and the errno the kernel actually returned.
 *
 * Deliberately not here: mixing, a callback thread, format conversion, and
 * any notion of a device other than the one playing. One core produces one
 * interleaved stereo stream of s16 and we hand it to the hardware. A mixer
 * would exist to combine sources, and there is only ever one source.
 */
#ifndef AUD_H
#define AUD_H

#include <stdint.h>

/* Opens the PCM and primes it. Returns 0 on success.
 *
 * Never fatal to the caller's judgement: a machine with no sound is still a
 * machine that runs games, so main treats a failure here as a warning. It
 * prints what the card would have accepted either way, because on a device
 * with no serial port the only chance to learn the constraints is the boot
 * log of the run that failed. */
int aud_open(void);
void aud_close(void);

/* Negotiated rate in Hz, or 0 when closed. Not a constant, because what we
 * ask for and what the codec grants are different questions. */
int aud_rate(void);

/* How many stereo frames this video frame is worth. Call exactly once per
 * video frame: it advances an accumulator, and calling it twice steals
 * samples from the next frame.
 *
 * This is the whole of "we pace the panel and resample audio" made concrete.
 * The count alternates between 799 and 800 at 48 kHz in whatever pattern
 * exact arithmetic dictates, and over any interval the error is under one
 * sample rather than accumulating. */
int aud_due(void);

/* Hand over n interleaved stereo frames. Returns frames accepted, or -1.
 *
 * Blocks if the hardware has not drained enough yet, which is intended: it is
 * backpressure from the only other clock in the system, and CLAUDE.md permits
 * exactly two ways to wait for time to pass, of which this is one. In steady
 * state it does not block at all, because aud_due asks for what the hardware
 * is about to consume. */
int aud_write(const int16_t *frames, int n);

/* Frames currently queued in the hardware buffer, or -1.
 *
 * The drift readout. The panel and the codec run off independent crystals, so
 * even exact arithmetic against the panel cannot hold the buffer level
 * steady - it can only stop us adding error of our own. Watching this number
 * over minutes is how we find out what the real correction term is, and that
 * measurement needs the device. */
/* Tops the buffer up with silence, by level rather than by rate, and returns
 * the frames written. For fast mode, where the frame loop is deliberately too
 * slow for aud_due's one-frame-per-call accounting to keep the codec fed. */
int aud_silence(void);

int aud_level(void);

/* Times the buffer has run dry since open. A machine with no serial port
 * needs its faults counted rather than logged as they happen. */
int aud_xruns(void);

#endif /* AUD_H */
