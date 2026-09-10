/* index.h — a track-detail cache this app owns, so track/disc numbers and
 * duration don't have to be re-read from every file on every album open.
 * See index.c for why and the scan model. */
#ifndef INDEX_H
#define INDEX_H

#include <time.h>
#include "library.h"   /* lib_row_t, for index_albums() below */

/* Starts the background scanner once (idempotent — a second call is a
 * no-op). Safe to call before lib_open(): the scanner opens its own
 * connections, entirely separate from library.c's g_db. */
void index_scan_start(void);

/* Whether a pass is running right now, for the Settings row. */
int index_scan_running(void);

/* Most recent/current pass's progress, for the Settings row. Returns 0 (and
 * leaves the outputs untouched) if no pass has ever started yet. */
int index_scan_progress(int *scanned, int *written);

/* Settings' "Rebuild library index" row: starts the scanner if it was
 * somehow never started, and wakes it immediately for a fresh pass rather
 * than waiting out its normal idle interval. */
void index_rescan_now(void);
void index_pause_for_usb(int paused);

/* Looks up a path's cached track/disc/duration, valid only if `mtime`
 * (the caller's own fresh stat() of the file) matches what was on disk when
 * it was scanned. Returns 1 and fills all three outputs on a fresh hit, 0
 * on a miss (not yet scanned, or the file changed since) — outputs are
 * untouched on a miss, so the caller's existing fallback path is unchanged.
 * UI-thread only, like every other database access in this app. */
int index_lookup(const char *path, time_t mtime, int *track, int *disc, int *dur_ms);

/* Whole-library album list, cached during the last scan pass -- entering
 * Albums unfiltered used to re-run MEDIA_TABLE's own GROUP BY/ORDER BY on
 * every single visit, with no index on `album` to make that cheap. Return
 * value is the count actually filled; 0 means no cache yet (never scanned,
 * or the scan hasn't reached this point), and the caller should fall back
 * to the live query exactly as before -- this is a pure fast path, not a
 * replacement for it. Rows come out in the same sorted order the live
 * query itself produces, so paging (offset/max) behaves identically. */
int index_albums(lib_row_t *out, int max, int offset);
/* 0 likewise means "no cache", not "zero albums" -- callers already treat
 * an empty library as len(rows)==0 regardless of what count() reports. */
int index_albums_count(void);

/* Same as index_albums()/index_albums_count(), for the other three
 * top-level Music lists: Album artists, Artists, Genres. `column` is one
 * of "album_artist", "artist", "genre" -- same whitelist library.c's
 * allowed_column() checks. Rows come out with owner left empty; the
 * caller applies its own blank-name/"Unknown" substitution the same way
 * regardless of whether a row came from here or the live query. */
int index_group(const char *column, lib_row_t *out, int max, int offset);
int index_group_count(const char *column);

#endif
