/* radio.c — the station list.
 *
 * Stations live in a plain "Name | URL" text file rather than being compiled
 * in, because which stations work is not something this app can decide. Two
 * kinds play: a direct MP3 stream, fed straight to the decoder, and an HLS
 * playlist, whose segments are demuxed from MPEG-TS and decoded as AAC.
 *
 * On the BBC: their live radio is served through an endpoint that describes
 * itself as part of a content protection system, so this app does not go
 * looking for URLs there. Paste one in and it plays like any other — the
 * player has no opinion about where a URL came from.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>

#include "radio.h"

#define STATIONS_PATH "/usr/data/radio_stations.conf"
#define NRK_CURL_BIN  "/data/mnt/sd_0/.podsync/curl"
#define NRK_CURL_CA   "/data/mnt/sd_0/.podsync/cacert.pem"

/* Written on first run so the file exists to be edited, rather than the user
 * having to guess the format. These are stations that publish a direct stream
 * URL openly. */
static const char *seed =
    "# One station per line:  Name | URL\n"
    "# Direct MP3 streams and HLS playlists (.m3u8) both work.\n"
    "# Lines starting with # are ignored.\n"
    "NRK Klassisk | https://nrk-live-radio-world.akamaized.net/klassisk/muxed.m3u8?adap=audio&aco=aac\n"
    "rbbKultur | https://dispatcher.rndfnk.com/rbb/rbbkultur/live/mp3/high\n"
    "BBC Radio 3 | \n"
    "BBC Radio 4 | \n";

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' '  || s[n - 1] == '\t')) s[--n] = '\0';
}

int radio_load(radio_station_t *out, int max) {
    FILE *f = fopen(STATIONS_PATH, "r");
    if (!f) {
        f = fopen(STATIONS_PATH, "w");
        if (f) { fputs(seed, f); fclose(f); }
        f = fopen(STATIONS_PATH, "r");
        if (!f) return 0;
    }

    char line[768];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *bar = strchr(line, '|');
        if (!bar) continue;
        *bar = '\0';
        char *url = bar + 1;
        trim(line);
        trim(url);
        /* A station with no URL yet is kept and shown greyed rather than
         * dropped — it is a slot the user has left themselves. */
        if (!line[0]) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", line);
        snprintf(out[n].url,  sizeof(out[n].url),  "%s", url);
        n++;
    }
    fclose(f);
    return n;
}

/* NRK's own live-channel API gives the current program block's title and
 * real artwork -- checked directly against psapi.nrk.no rather than
 * assumed: it does NOT give per-track/composer "now playing" info for a
 * live channel (that field, indexPoints, is only ever populated for their
 * on-demand catalog) -- see radio_fetch_nrk_art()'s own comment for the
 * exact endpoint. Channel id is derived from the station name ("NRK
 * Klassisk" -> "klassisk") since every NRK channel name in the wild follows
 * that same "NRK <Name>" convention; a non-NRK station is simply not
 * offered this (radio_fetch_nrk_art() returns -1 immediately). */
static int nrk_run_curl(const char *url, const char *out_path) {
    unlink(out_path);
    pid_t pid = fork();
    if (pid == 0) {
        execl(NRK_CURL_BIN, NRK_CURL_BIN, "-fsSL", "--cacert", NRK_CURL_CA,
              "--connect-timeout", "10", "--max-time", "20",
              "-o", out_path, url, (char *)NULL);
        execlp("curl", "curl", "-fsSL",
               "--connect-timeout", "10", "--max-time", "20",
               "-o", out_path, url, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status;
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    struct stat st;
    if (stat(out_path, &st) != 0 || st.st_size <= 0) return -1;
    return 0;
}

/* The smallest image at least NRK_ART_MIN_PX wide, not the largest -- NRK's
 * image arrays list ascending sizes (each entry a "url" immediately
 * followed by its own "width"), running up to 1920px, and cover.c's own
 * progressive-JPEG memory budget (COVER_MEM_BUDGET, 14MB) rejects a large
 * one outright: NRK's images are progressive, and 1920x1920 needs ~22MB of
 * coefficients to decode. Confirmed live -- radio_art_worker's own debug
 * log showed a successful fetch (rc=0, real title) immediately followed by
 * cover_load_fresh() returning NULL for exactly this reason. The display
 * only ever wants RADIO_ART_PX (music_hook.c) or ART_PX square pixels
 * anyway, so there was never a reason to ask for the largest one -- cover_
 * load() rejects anything *smaller* than the requested px (its own "don't
 * upscale a thumbnail" rule), which is what NRK_ART_MIN_PX guards against
 * picking too small. Bounded to at most `end` of buf searched, so a title/
 * image block belonging to a *later* entries[] element can never be
 * mistaken for this one's. */
#define NRK_ART_MIN_PX 480
static int nrk_best_url_in(const char *from, const char *end, char *out, size_t out_n) {
    out[0] = '\0';
    char last_seen[600] = "";
    const char *p = from;
    while (p < end) {
        const char *k = strstr(p, "\"url\"");
        if (!k || k >= end) break;
        const char *c = strchr(k, ':');
        if (!c || c >= end) break;
        const char *q1 = strchr(c, '"');
        if (!q1 || q1 >= end) break;
        const char *q2 = strchr(q1 + 1, '"');
        if (!q2) break;
        size_t len = (size_t)(q2 - q1 - 1);
        if (len > 0 && len < sizeof(last_seen)) {
            memcpy(last_seen, q1 + 1, len);
            last_seen[len] = '\0';
        }
        const char *w = strstr(q2, "\"width\"");
        int width = (w && w < end) ? atoi(strchr(w, ':') + 1) : 0;
        if (width >= NRK_ART_MIN_PX && last_seen[0]) {
            snprintf(out, out_n, "%s", last_seen);
            return 0;   /* smallest qualifying one found -- ascending order, stop here */
        }
        p = q2 + 1;
    }
    /* Nothing reached the minimum (an oddly small image set): fall back to
     * the largest available rather than nothing at all. */
    if (last_seen[0]) { snprintf(out, out_n, "%s", last_seen); return 0; }
    return -1;
}

int radio_fetch_nrk_art(const char *station_name, const char *dest_jpg,
                        char *title_out, size_t title_n) {
    title_out[0] = '\0';
    if (strncmp(station_name, "NRK ", 4) != 0) return -1;

    char channel_id[64];
    size_t j = 0;
    for (const char *p = station_name + 4; *p && j + 1 < sizeof(channel_id); p++)
        channel_id[j++] = (char)tolower((unsigned char)*p);
    channel_id[j] = '\0';
    if (!channel_id[0]) return -1;

    char url[200];
    snprintf(url, sizeof(url), "https://psapi.nrk.no/radio/channels/livebuffer/%s", channel_id);

    char meta_path[64];
    snprintf(meta_path, sizeof(meta_path), "/tmp/.nrk_live_%d.json", (int)getpid());
    if (nrk_run_curl(url, meta_path) != 0) { unlink(meta_path); return -1; }

    FILE *f = fopen(meta_path, "rb");
    if (!f) { unlink(meta_path); return -1; }
    char *buf = malloc(65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    fclose(f);
    unlink(meta_path);
    if (!buf) return -1;
    buf[n] = '\0';

    int rc = -1;
    const char *entries = strstr(buf, "\"entries\"");
    const char *arr = entries ? strchr(entries, '[') : NULL;
    const char *e0 = NULL, *e_end = NULL;
    if (arr) {
        /* "livebuffer" means exactly what it says: this array lists program
         * blocks oldest-first, ending with whichever one is airing *right
         * now* -- it is a record of what already played, not a schedule of
         * what's coming. Confirmed against a live response: at 09:03 UTC
         * the array ran Morgenstemning (06:03) ... Nyheter 11:00 (09:00) ...
         * ending with the block that was actually current. Taking entries[0]
         * (the old code) instead showed a program from hours earlier -- this
         * walks every top-level {...} object in the array and keeps only
         * the last one, bounded by the array's own closing ']' via a
         * bracket-depth counter so a nested "images":[...] array inside an
         * entry can't be mistaken for the end of the whole list. */
        int brace_depth = 0, bracket_depth = 0;
        const char *p = arr + 1;
        const char *cur_start = NULL;
        for (; *p; p++) {
            if (*p == '[') {
                bracket_depth++;
            } else if (*p == ']') {
                if (bracket_depth == 0) break;
                bracket_depth--;
            } else if (*p == '{') {
                if (brace_depth == 0 && bracket_depth == 0 && !cur_start) cur_start = p;
                brace_depth++;
            } else if (*p == '}') {
                brace_depth--;
                if (brace_depth == 0 && bracket_depth == 0 && cur_start) {
                    e0 = cur_start;
                    e_end = p + 1;
                    cur_start = NULL;
                }
            }
        }
    }
    if (e0) {
        const char *t = strstr(e0, "\"title\"");
        if (t && t < e_end) {
            const char *c = strchr(t, ':');
            const char *q1 = c ? strchr(c, '"') : NULL;
            const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q2) {
                size_t len = (size_t)(q2 - q1 - 1);
                if (len >= title_n) len = title_n - 1;
                memcpy(title_out, q1 + 1, len);
                title_out[len] = '\0';
            }
        }

        const char *sq = strstr(e0, "\"squareImage\"");
        if (sq && sq < e_end) {
            char img_url[600];
            if (nrk_best_url_in(sq, e_end, img_url, sizeof(img_url)) == 0) {
                char tmp_jpg[300];
                snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.part", dest_jpg);
                if (nrk_run_curl(img_url, tmp_jpg) == 0 && rename(tmp_jpg, dest_jpg) == 0)
                    rc = 0;
                else
                    unlink(tmp_jpg);
            }
        }
    }

    free(buf);
    return rc;
}

static int rec_cmp_newest(const void *a, const void *b) {
    const radio_recording_t *ra = a, *rb = b;
    return (rb->mtime > ra->mtime) - (rb->mtime < ra->mtime);
}

int radio_recordings_load(radio_recording_t *out, int max) {
    DIR *d = opendir(RADIO_REC_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        snprintf(out[n].path, sizeof(out[n].path), "%s/%s", RADIO_REC_DIR, e->d_name);
        struct stat st;
        if (stat(out[n].path, &st) != 0) continue;
        out[n].size_bytes = (long)st.st_size;
        out[n].mtime = (long)st.st_mtime;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(*out), rec_cmp_newest);
    return n;
}

void radio_recording_new_path(const char *station_name, const char *ext,
                              char *out, size_t n) {
    mkdir(RADIO_REC_DIR, 0755);
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    /* Station name kept as-is (no illegal FAT/exFAT characters expected in
     * any real station name in the seed file), just given a fixed-format
     * timestamp so a folder of these sorts sensibly and each one's date is
     * readable without opening it. */
    snprintf(out, n, "%s/%s %04d-%02d-%02d %02d-%02d-%02d.%s",
             RADIO_REC_DIR, station_name,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ext);
}
