#ifndef WAVEFORM_H
#define WAVEFORM_H

#include <stdint.h>

/* The shape of a music track for Now Playing's waveform seek bar: one byte
 * per column, how loud that stretch of the track is on average.
 *
 * Worked out once per track by a background decode at the scheduler's idle
 * class (see waveform.c) and kept in a single file on the card, so it is
 * available on the first play, survives seeks and skips, and has nothing to
 * do with the volume setting. */

/* 144 columns across the bar's 432 px -- see WAVE_BUCKETS's comment in
 * music_hook.c for why exactly this many. Changing it invalidates the cache
 * file, which records the column count and starts again on a mismatch. */
#define WAVEFORM_BUCKETS 144

/* Starts the worker. Safe to call more than once. `log` receives one line per
 * track worked out or found in the cache; NULL for silence. */
void waveform_start(void (*log)(const char *fmt, ...));

/* The track now showing. Returns 1 with out[] filled when its shape is known,
 * 0 while it is being worked out (or never will be: an unreadable file). A
 * different path abandons whatever was in progress; "" wants nothing and
 * lets the worker sleep. Cheap enough to poll every frame: a lock, a string
 * compare, and a copy once ready. */
int waveform_get(const char *path, uint8_t *out);

/* The next two tracks in the queue, in that order. Worked out only once the
 * track on screen is settled, first then second, and dropped the instant that
 * stops being true, so they never delay the shape anybody is waiting for.
 * Results go to the cache rather than to waveform_get(), which finds them
 * there when those tracks actually arrive. Either may be "" for nothing.
 *
 * Two rather than one so a skip forward lands on a shape that is already
 * there: by the time the second track starts, the third is normally done and
 * the fourth has been armed in its place. */
void waveform_prefetch(const char *first, const char *second);

#endif
