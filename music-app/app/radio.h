#ifndef RADIO_H
#define RADIO_H
#include "library.h"

#define RADIO_MAX 32
typedef struct {
    char name[LIB_NAME_LEN];
    char url[512];
} radio_station_t;

/* Loads /usr/data/radio_stations.conf, writing a starter file if absent.
 * Returns how many stations are available. */
int radio_load(radio_station_t *out, int max);

/* Recordings: raw copies of whatever a station actually sent (see
 * radio_buffer.h), saved by the user for as long as they chose to record. */
#define RADIO_REC_DIR "/data/mnt/sd_0/.radio_recordings"
#define RADIO_REC_MAX 200

typedef struct {
    char name[160];   /* filename without the directory, e.g. "NRK Klassisk 2026-09-13 10-05-00.aac" */
    char path[700];
    long size_bytes;
    long mtime;
} radio_recording_t;

/* Newest first. Returns how many were found. */
int radio_recordings_load(radio_recording_t *out, int max);

/* NRK's live-channel API gives the current program block's title and real
 * artwork (see radio.c's own comment on radio_fetch_nrk_art() for the exact
 * endpoint and why this is NOT per-track/composer info -- NRK simply does
 * not publish that for a live channel). station_name must start with
 * "NRK " (checked against the seed station list's own naming) or this
 * returns -1 immediately without any network access. On success, writes
 * the artwork to dest_jpg and the program title to title_out. Real network
 * I/O -- call from a worker thread, never the UI thread. */
int radio_fetch_nrk_art(const char *station_name, const char *dest_jpg,
                        char *title_out, size_t title_n);

/* Same shape, for Deutschlandfunk/Deutschlandfunk Kultur/Deutschlandfunk
 * Nova (see radio_fetch_dlf_broadcast()'s own comment in radio.c). Unlike
 * NRK, the artwork here is a fixed per-station logo rather than something
 * that changes with the programme, and Nova gets no title at all -- no live
 * "now playing" endpoint was found for its site. station_name must be
 * "Deutschlandfunk", "Deutschlandfunk Kultur" or "Deutschlandfunk Nova"
 * exactly (the seed list's own naming) or this returns -1 immediately. */
int radio_fetch_dlf_broadcast(const char *station_name, const char *dest_jpg,
                              char *title_out, size_t title_n);

/* Real per-track composer/piece info for BBC Radio 3 (see radio_fetch_bbc_
 * track()'s own comment in radio.c) -- unlike NRK/DLF, this can give an
 * actual "artist - track" line, not just a programme block, when the
 * current programme is playing music. Falls back to the programme title
 * for Radio 4 (all speech, no track list ever) and for Radio 3 itself
 * outside a music segment. station_name must be "BBC Radio 3" or
 * "BBC Radio 4" exactly (the seed list's own naming) or this returns -1
 * immediately. */
int radio_fetch_bbc_track(const char *station_name, const char *dest_jpg,
                          char *title_out, size_t title_n);

/* Builds a fresh path for a new recording of this station, named by station
 * and the current local time so a folder of them sorts and reads sensibly
 * without opening each one. ext is "mp3" or "aac" (radio_buffer.h's
 * RB_KIND_MP3/RB_KIND_ADTS -- the caller already knows which, from
 * rb_current_kind()), no leading dot. Creates RADIO_REC_DIR if it does not
 * exist yet. */
void radio_recording_new_path(const char *station_name, const char *ext,
                              char *out, size_t n);
#endif
