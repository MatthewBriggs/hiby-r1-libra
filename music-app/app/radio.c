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
#define RADIO_CURL_BIN "/data/mnt/sd_0/.podsync/curl"
#define RADIO_CURL_CA  "/data/mnt/sd_0/.podsync/cacert.pem"

/* Written on first run so the file exists to be edited, rather than the user
 * having to guess the format. These are stations that publish a direct stream
 * URL openly. */
static const char *seed =
    "# One station per line:  Name | URL\n"
    "# Direct MP3 streams and HLS playlists (.m3u8) both work.\n"
    "# Lines starting with # are ignored.\n"
    "NRK Klassisk | https://nrk-live-radio-world.akamaized.net/klassisk/muxed.m3u8?adap=audio&aco=aac\n"
    "rbbKultur | https://dispatcher.rndfnk.com/rbb/rbbkultur/live/mp3/high\n"
    /* MP3, not the AAC variant these stations also publish -- AAC here
     * means a raw ADTS elementary stream over plain HTTP, not HLS, and this
     * app's direct-stream decode path only speaks MP3 (see open_any()'s
     * .m3u8 check in audio.c: anything without that extension goes to the
     * MP3 decoder, which would just fail on ADTS). MP3 needs no new code. */
    "Deutschlandfunk | https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3\n"
    "Deutschlandfunk Kultur | https://st02.sslstream.dlf.de/dlf/02/128/mp3/stream.mp3\n"
    "Deutschlandfunk Nova | https://st03.sslstream.dlf.de/dlf/03/128/mp3/stream.mp3\n"
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

/* Shared by every "now playing" provider below (NRK, Deutschlandfunk) --
 * nothing in it is provider-specific, just fetch a URL to a file. */
static int radio_run_curl(const char *url, const char *out_path) {
    unlink(out_path);
    pid_t pid = fork();
    if (pid == 0) {
        execl(RADIO_CURL_BIN, RADIO_CURL_BIN, "-fsSL", "--cacert", RADIO_CURL_CA,
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
    if (radio_run_curl(url, meta_path) != 0) { unlink(meta_path); return -1; }

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
                if (radio_run_curl(img_url, tmp_jpg) == 0 && rename(tmp_jpg, dest_jpg) == 0)
                    rc = 0;
                else
                    unlink(tmp_jpg);
            }
        }
    }

    free(buf);
    return rc;
}

/* Deutschlandfunk / Deutschlandfunk Kultur / Deutschlandfunk Nova each run a
 * different frontend (checked directly, not assumed): DLF and Kultur share
 * one CMS that exposes "/api/partials/CurrentBroadcast?drsearch:_ajax=1" on
 * the station's own site -- a tiny HTML fragment whose one real element is
 * <div data-broadcast="{&quot;title&quot;:...,&quot;startTime&quot;:...,
 * &quot;producer&quot;:...}">, no JSON API and no auth needed. Nova's site is
 * a completely separate build with no equivalent found (its schedule pages
 * only carry on-demand podcast episodes, not a live "now" marker), so it
 * gets no title here -- radio_fetch_dlf_broadcast() returns -1 for it before
 * any network access, same as radio_fetch_nrk_art() does for a non-NRK name.
 *
 * None of the three publish live per-program artwork the way NRK does, so
 * the image here is a fixed per-station logo, not something that changes
 * with the programme -- ARD Audiothek's own GraphQL API lists one for every
 * ARD live stream (permanentLivestreams { image { url } }, confirmed via
 * its schema introspection), but querying it live for a fixed value is
 * pointless network cost; the three URLs below are exactly what that query
 * returns for these stations' publicationServiceIds today. Landscape (16:9),
 * not square -- cover_load()'s own center-crop (see its "side"/"x_off"
 * logic) handles that the same way it would a landscape local album cover,
 * but it also refuses to *upscale* a thumbnail, and center-cropping a 16:9
 * image to a square uses its short side -- so w=960 here, not the w=480
 * that would be plenty for a square image, since ARD's image service only
 * takes a width and returns proportional height (960x540 clears RADIO_ART_
 * PX's 480px square comfortably; 480x270 did not, and got silently
 * rejected exactly the way an oversized NRK image once was for the
 * opposite reason -- see nrk_best_url_in()'s own comment on that bug). */
static int dlf_site_for(const char *station_name, const char **host) {
    if (!strcmp(station_name, "Deutschlandfunk")) { *host = "www.deutschlandfunk.de"; return 0; }
    if (!strcmp(station_name, "Deutschlandfunk Kultur")) { *host = "www.deutschlandfunkkultur.de"; return 0; }
    return -1;   /* Nova, or anything else -- no CurrentBroadcast endpoint known */
}

static const char *dlf_logo_url_for(const char *station_name) {
    if (!strcmp(station_name, "Deutschlandfunk"))
        return "https://api.ardmediathek.de/image-service/images/urn:ard:image:13599da8686b116c?w=960&ch=aed6513ff66e353d";
    if (!strcmp(station_name, "Deutschlandfunk Kultur"))
        return "https://api.ardmediathek.de/image-service/images/urn:ard:image:653f5af88ec9c219?w=960&ch=46be14b5e31b871c";
    if (!strcmp(station_name, "Deutschlandfunk Nova"))
        return "https://api.ardmediathek.de/image-service/images/urn:ard:image:3ae73f64276f37ad?w=960&ch=dc349460284f607a";
    return NULL;
}

/* Pulls "title" out of data-broadcast="{&quot;title&quot;:&quot;...&quot;,
 * ...}" without ever unescaping the whole blob -- the value never contains
 * a raw '"', so the &quot; markers either side of it are exactly the
 * delimiters needed, no HTML-entity decoding required for this one field. */
static int dlf_title_in(const char *buf, char *out, size_t out_n) {
    const char *attr = strstr(buf, "data-broadcast=\"");
    if (!attr) return -1;
    const char *k = strstr(attr, "title&quot;:&quot;");
    if (!k) return -1;
    const char *v0 = k + strlen("title&quot;:&quot;");
    const char *v1 = strstr(v0, "&quot;");
    if (!v1) return -1;
    size_t len = (size_t)(v1 - v0);
    if (len >= out_n) len = out_n - 1;
    memcpy(out, v0, len);
    out[len] = '\0';
    return out[0] ? 0 : -1;
}

int radio_fetch_dlf_broadcast(const char *station_name, const char *dest_jpg,
                              char *title_out, size_t title_n) {
    title_out[0] = '\0';
    if (strncmp(station_name, "Deutschlandfunk", 15) != 0) return -1;

    int rc = -1;
    const char *host;
    if (dlf_site_for(station_name, &host) == 0) {
        char url[160];
        snprintf(url, sizeof(url), "https://%s/api/partials/CurrentBroadcast?drsearch:_ajax=1", host);
        char frag_path[64];
        snprintf(frag_path, sizeof(frag_path), "/tmp/.dlf_bc_%d.html", (int)getpid());
        if (radio_run_curl(url, frag_path) == 0) {
            FILE *f = fopen(frag_path, "rb");
            if (f) {
                char buf[4096];
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f);
                buf[n] = '\0';
                if (dlf_title_in(buf, title_out, title_n) == 0) rc = 0;
            }
        }
        unlink(frag_path);
    }

    const char *logo = dlf_logo_url_for(station_name);
    if (logo) {
        char tmp_jpg[300];
        snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.part", dest_jpg);
        /* Whatever format the host actually published -- checked live,
         * Deutschlandfunk and Nova's own logos are JPEG but Kultur's are
         * PNG. Format handling lives with the rest of the cover pipeline
         * (music_hook.c's radio_art_worker(), via cover_png_to_jpeg()),
         * not here: this function's job is just getting the bytes. */
        if (radio_run_curl(logo, tmp_jpg) == 0 && rename(tmp_jpg, dest_jpg) == 0)
            rc = 0;   /* a logo with no title is still worth showing */
        else
            unlink(tmp_jpg);
    }
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
