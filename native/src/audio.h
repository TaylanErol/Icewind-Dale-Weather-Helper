/* audio.h - wind ambience (port of js/audio.js) on winmm waveOut.
 *
 * Model (audio.js): brown noise
 *   last = (last + 0.02 * white) / 1.02;  sample = last * 3.5
 * through a band-pass biquad, a wind gain and a master gain:
 *   gust      = 0.65 + 0.35 * sin(phase) * sin(phase * 0.37 + 1.3),
 *               phase += dt * (0.25 + wind * 0.6)
 *   gain      -> (0.03 + wind * 0.42) * gust          (time constant 0.5 s)
 *   centre Hz -> 220 + wind * 800 + gust * 260        (0.6 s)
 *   Q         -> 0.6 + wind * 1.4                     (0.8 s)
 *   master    -> enabled ? 0.9 : 0                    (0.4 s; fade out, then stop)
 * Thunder (lightning events): the noise at half speed through a 140 Hz low-pass with an
 * envelope 0 -> 1.4*s in 80 ms, -> 0.4*s at 0.7 s, -> 0.001 at 3.2 s, delayed
 * 0.3 + (1 - s) * 1.4 s, strength s in [0.5, 1].
 * The native synthesis keeps this model with gusts whose depth grows with the wind
 * (0.63..0.79 in calm air, 0.42..1 at full wind, same mean), a band from 300 Hz, a calm
 * floor of 0.065 and live (unlooped) noise, so quiet weather never dips toward silence;
 * the exact deviations are listed at the top of audio.c.
 *
 * Interior (native only): the sound as heard from inside a closed building - wind and
 * thunder through one overdamped low-pass (about -6 dB/oct from 250 Hz, -12 dB/oct above
 * 2 kHz, like a wall), 5..9 dB quieter in all with the howl still faintly audible,
 * shallower gusts and a faint low rumble. Toggling cross-fades over about 0.8 s. Sound
 * only: never touches the weather.
 *
 * Native implementation notes (audio.c):
 *   - 44100 Hz mono 16-bit; a worker thread woken by the waveOut CALLBACK_EVENT keeps
 *     8 blocks of 35 ms (279 ms) queued and refills them in order. It runs at high
 *     priority, joins MMCSS "Audio" when avrt.dll exists and opts out of power
 *     throttling (both resolved at run time). All smoothing is done per block on that
 *     thread, so the UI thread never runs a timer for audio and a busy or dragged window
 *     cannot starve it. The thread and device exist only while enabled (plus the fade-out);
 *     disabled = zero CPU.
 *   - Parameters are written by the UI thread and read by the audio thread through
 *     atomics; every function here is cheap and non-blocking except audio_shutdown(),
 *     which joins the thread.
 *   - The wind level comes from the event of the viewed hour: the UI passes
 *     iwd_look(id)->wind (0..1, scene.js LOOKS). Thunder is optional: when the viewed hour
 *     is Lightning Storm (id 10) the UI may call audio_thunder() at random intervals of
 *     0.8..4.3 s only while the sound is on.
 *   - Links only winmm.dll (system DLL).
 */
#ifndef IWD_AUDIO_H
#define IWD_AUDIO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prepare the module (no device is opened yet). Always succeeds; cheap. */
void audio_init(void);

/* Turn the ambience on/off (sound toggle). Turning on opens the default wave device and
 * starts the fill thread; returns false when no audio device can be opened (the UI shows
 * "No audio" on the toggle, as the web does). Turning off fades out over ~0.4 s, then
 * closes the device. Returns the resulting enabled state's success. */
bool audio_set_enabled(bool on);
bool audio_is_enabled(void);

/* Whether any wave output device exists (waveOutGetNumDevs() > 0). Cheap, opens nothing;
 * the UI probes it once at start-up so the sound chip can say "No audio" from the first
 * paint instead of only after the first toggle. */
bool audio_device_present(void);

/* Wind strength 0..1 (clamped). The audio thread eases toward it with the time constants
 * above, so the UI may call this whenever the viewed hour/event changes. */
void audio_set_wind(double wind);

/* Interior muffling on/off (see above). May be set at any time, also while the sound is
 * off (it then applies from the next start). Cheap; the audio thread cross-fades. */
void audio_set_interior(bool on);
bool audio_interior(void);

/* Times the device queue ran dry since start-up (diagnostics; 0 on a healthy system). */
unsigned audio_underruns(void);

/* One distant thunder clap, strength 0.5..1 (clamped). Ignored while disabled. */
void audio_thunder(double strength);

/* Stop everything and release the device/thread. Safe to call more than once. */
void audio_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IWD_AUDIO_H */
