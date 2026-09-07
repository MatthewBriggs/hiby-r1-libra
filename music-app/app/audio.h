/* audio.h — playback. Decoders are dlopen'd from the device; see audio.c. */
#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>
int  audio_play(const char *path);
void audio_stop(void);
void audio_toggle(void);
void audio_seek_ms(int ms);
/* The target of the most recent audio_seek_ms() call, until the worker
 * thread has actually applied it (-1 once caught up or if none is
 * outstanding). audio_pos_ms() alone reads 0 for however long the worker
 * takes to open the decoder/output after audio_play() -- opening a fresh
 * Bluetooth PCM connection especially -- so anything drawing a resumed
 * position right after audio_play()+audio_seek_ms() should prefer this
 * over audio_pos_ms() while it's non-negative, or it shows the wrong spot
 * (usually the very start) until the seek lands. */
int  audio_seek_pending_ms(void);
/* Container-header probe, no decode: bits/rate/bitrate/duration for a
 * scanner populating a database row, not for playback. Any output pointer
 * may be NULL. Returns 0 on success (a format this app can open at all),
 * -1 otherwise. See audio_probe_format()'s own comment in audio.c for what
 * each field costs to get and how MP3 differs from everything else. */
int audio_probe_format(const char *path, int *bits, int *rate,
                       int *bitrate_bps, int *dur_ms);
/* Pitch-preserving playback speed (WSOLA), permille: 1000 = 1.0x, clamped to
 * [800,2000]. 1000 bypasses WSOLA entirely -- exact passthrough, same as
 * before this existed. Applies to the 16-bit output path only; a hires
 * source keeps playing at 1.0x regardless (audiobooks are never hires in
 * practice, so this is not a real-world limitation). */
void audio_set_speed(int permille);
int  audio_speed(void);

/* Gapless: hand over the track to roll into when this one ends. The worker
 * takes it at the boundary without closing the output device.
 * audio_take_advance reports (and clears) how many times that has happened, so
 * the UI can follow along and queue the one after. */
void audio_set_next(const char *path);
int  audio_take_advance(void);
int  audio_is_active(void);
int  audio_is_paused(void);
int  audio_pos_ms(void);
int  audio_dur_ms(void);
/* R29: raw abs-sample peak of the most recent output chunk -- see
 * g_last_peak's own comment in audio.c for why this is deliberately
 * unscaled. For the Music-only waveform seek bar: sample this on a UI-
 * thread poll while a track plays and bucket it by audio_pos_ms(), the
 * same way every other live readout in this app is built from a poll
 * rather than a callback. */
int32_t audio_current_peak(void);
void audio_set_volume(int pct);
int  audio_volume(void);
void audio_volume_step(int delta);   /* routes to the BT mixer or software */
void audio_volume_set(int pct);      /* absolute, for the slider */
int  audio_using_bt(void);
int  audio_using_usb(void);
/* Settings' "disable PEQ, MSEB and Bluetooth when playing over USB" -- skips
 * eq_process_*() outright while on, rather than touching eq_enabled()/the
 * saved profile, so it's a pure runtime override that undoes itself the
 * instant USB output stops, whatever the user's actual EQ settings are. */
void audio_set_usb_bypass(int on);
int  audio_usb_bypass(void);
/* Same "USB Transport Mode" feature: pins the volume at whatever it was set
 * to right before this engaged (100, in the current caller -- a USB DAC/amp
 * is expected to own its own volume, the same reasoning R37's DSP bypass
 * already uses for that output). audio_volume_set()/audio_volume_step()
 * both ignore any change while this is on, so a stray key press or slider
 * drag can't quietly move it out from under the DAC; the caller still uses
 * audio_set_volume() directly (which this does NOT gate) to set the pinned
 * value itself before/after engaging. */
void audio_set_vol_locked(int on);
/* BG90 follow-up: the worker thread's own Bluetooth-reconnect poll (see its
 * comment in audio.c) forks a `bluealsa-cli` subprocess every ~5s while
 * playing over wired output -- real, if brief, contention for the one core
 * against whatever the UI thread is doing right then. The worker has no
 * other way to know the screen is locked (set_locked() calls this so it
 * does), and there is nothing to gain from discovering a Bluetooth
 * reconnect while the user cannot see the screen change anyway -- skipped
 * entirely while locked, resuming from wherever its own tick count left
 * off once unlocked, rather than risk this exact subprocess landing right
 * on top of the wake-up redraw and reading as a slower wake. */
void audio_set_screen_locked(int on);
/* Background-thread only (bt_poll) -- both do blocking D-Bus round-trips via
 * amixer. audio_bt_volume_pending() is the cheap check between polls;
 * audio_bt_volume_service() applies a pending write (from a key or the
 * slider) and always reads back afterward. Neither may run on the thread
 * that also draws frames and reads input -- see the comment above
 * audio_volume_set() in audio.c for why. */
int  audio_bt_volume_pending(void);
void audio_bt_volume_service(void);
const char *audio_codec(void);        /* of the stream now playing */
int  audio_is_exact(void);            /* output opened with no conversion layer */
int  audio_output_lost(void);         /* playback stopped because the device went away */
const char *audio_output(void);   /* "3.5 mm", "USB" or "Bluetooth" */

/* R28: header-only duration probe for a track the library's own database
 * has no duration for (see the comment above its definition in audio.c for
 * why that is the common case, not a rare one). bitrate_bps is only used
 * as an MP3/VBR fallback; pass 0 if unknown. Safe to call from any thread,
 * including while something else is playing. Returns 0 on failure. */
int audio_probe_dur_ms(const char *path, int bitrate_bps);

/* BG106: 1 if path's first MPEG frame carries a Xing/VBRI VBR tag, 0 for
 * CBR/ABR/unreadable/non-MP3 alike -- see audio.c's own comment on why
 * LAME's "Info" tag (CBR/ABR) is deliberately excluded. Cheap (reads at
 * most 8202 bytes), but callers showing this in a UI should still cache
 * it per path rather than re-probe every redraw -- see music_hook.c's own
 * cached wrapper. */
int audio_mp3_is_vbr(const char *path);
#endif
