/* hls.c — enough HLS to play a live radio stream.
 *
 * Two playlists: a master listing variants at different bitrates, and a media
 * playlist per variant listing the segments currently available. The media
 * playlist is a sliding window — it is re-fetched as it goes, and segments
 * already played are recognised by their media sequence number rather than by
 * their URL, because a stream can legitimately repeat a filename.
 *
 * Fetching goes through the static curl on the card, as elsewhere in this app:
 * the device's own wget cannot complete a modern TLS handshake.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hls.h"

#define CURL_PATH "/data/mnt/sd_0/.podsync/curl"
#define CA_BUNDLE "/data/mnt/sd_0/.podsync/cacert.pem"

static double now_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int fetch(const char *url, unsigned char *buf, int max) {
    char cmd[HLS_URL_MAX + 256];
    snprintf(cmd, sizeof(cmd),
             "%s -sL --max-time 20 --cacert %s '%s' 2>/dev/null",
             CURL_PATH, CA_BUNDLE, url);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    int n = 0;
    while (n < max) {
        size_t got = fread(buf + n, 1, (size_t)(max - n), p);
        if (got == 0) break;
        n += (int)got;
    }
    pclose(p);
    return n;
}

/* Resolve a playlist entry against the playlist it came from: absolute, or
 * host-relative, or a sibling. */
static void resolve(const char *base, const char *ref, char *out, size_t n) {
    if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8)) {
        snprintf(out, n, "%s", ref);
        return;
    }
    if (ref[0] == '/') {
        const char *p = strstr(base, "://");
        p = p ? strchr(p + 3, '/') : NULL;
        int host_len = p ? (int)(p - base) : (int)strlen(base);
        snprintf(out, n, "%.*s%s", host_len, base, ref);
        return;
    }
    const char *slash = strrchr(base, '/');
    int dir = slash ? (int)(slash - base) : (int)strlen(base);
    snprintf(out, n, "%.*s/%s", dir, base, ref);
}

static char *next_line(char **cursor) {
    char *s = *cursor;
    if (!s || !*s) return NULL;
    char *nl = strchr(s, '\n');
    if (nl) { *nl = '\0'; *cursor = nl + 1; } else { *cursor = s + strlen(s); }
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == ' ')) s[--len] = '\0';
    return s;
}

int hls_open(hls_t *h, const char *master_url) {
    memset(h, 0, sizeof(*h));
    h->last_seq = -1;
    h->target_ms = 4000;

    static unsigned char buf[256 * 1024];
    int n = fetch(master_url, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    if (strncmp((char *)buf, "#EXTM3U", 7) != 0) return -1;

    /* A media playlist can be handed to us directly; only chase variants when
     * this is actually a master. */
    if (!strstr((char *)buf, "#EXT-X-STREAM-INF")) {
        snprintf(h->media_url, sizeof(h->media_url), "%s", master_url);
        return 0;
    }

    /* Pick the highest bandwidth on offer. This is a music player attached to
     * a DAC; the point is the good one. */
    long best = -1;
    char *cur = (char *)buf, *line;
    long pending_bw = -1;
    while ((line = next_line(&cur))) {
        if (!strncmp(line, "#EXT-X-STREAM-INF", 17)) {
            const char *b = strstr(line, "BANDWIDTH=");
            pending_bw = b ? strtol(b + 10, NULL, 10) : 0;
        } else if (line[0] && line[0] != '#' && pending_bw >= 0) {
            if (pending_bw > best) {
                best = pending_bw;
                resolve(master_url, line, h->media_url, sizeof(h->media_url));
            }
            pending_bw = -1;
        }
    }
    return h->media_url[0] ? 0 : -1;
}

/* Re-read the media playlist and queue anything newer than what we have.
 *
 * The queue is a sliding window over the *newest* segments seen, and
 * last_seq advances for every segment walked, not only for ones that fit
 * in the queue. Both of those matter, and getting either wrong is not
 * subtle -- measured live (BACKLOG R111): NRK's media playlist lists 2893
 * segments, a roughly three-hour DVR window, not the short live window
 * this originally assumed. The old code stopped *adding* at HLS_QUEUE and
 * only advanced last_seq when it added, so a cold start latched onto
 * segment 8 of 2893 -- about three hours behind live -- and then crawled
 * forward exactly HLS_QUEUE segments per refresh, keeping the last two of
 * each batch and silently dropping the six in between. That produced
 * audio with six-segment holes in it, a "behind live" figure that grew
 * without ever converging, and a buffer filling roughly four times faster
 * than real time (255 seconds of audio fetched in 63 seconds of wall
 * clock, with the fetch loop never once having to wait). Draining the
 * oldest queue entry on overflow instead of refusing to add, and
 * advancing last_seq unconditionally, keeps this pinned to the live edge
 * on any window length. */
static int refresh(hls_t *h) {
    static unsigned char buf[256 * 1024];
    int n = fetch(h->media_url, buf, (int)sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';

    int cold = (h->last_seq < 0);
    long long seq = 0;
    char *cur = (char *)buf, *line;
    while ((line = next_line(&cur))) {
        if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
            seq = strtoll(line + 22, NULL, 10);
        } else if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22)) {
            int t = (int)strtol(line + 22, NULL, 10);
            if (t > 0 && t < 60) h->target_ms = t * 1000;
        } else if (line[0] && line[0] != '#') {
            if (cold || seq > h->last_seq) {
                if (h->n_queue == HLS_QUEUE) {
                    /* Full: drop the oldest so the newest always survives.
                     * The old code stopped adding here instead, which is
                     * what stranded a cold start at the back of a long
                     * window -- see this function's own comment. */
                    memmove(h->queue[0], h->queue[1],
                            (size_t)(HLS_QUEUE - 1) * HLS_URL_MAX);
                    h->n_queue--;
                }
                resolve(h->media_url, line, h->queue[h->n_queue], HLS_URL_MAX);
                h->n_queue++;
                h->last_seq = seq;
            }
            seq++;
        }
    }
    /* Cold start only: begin at the live edge rather than playing out
     * however much of the window this just queued. Deliberately not done
     * on later refreshes -- by then the queue only ever holds genuinely
     * new segments, and dropping any of those would punch a hole in the
     * time-shift buffer for no reason. */
    if (cold && h->n_queue > 2) {
        memmove(h->queue[0], h->queue[h->n_queue - 2], HLS_URL_MAX);
        memmove(h->queue[1], h->queue[h->n_queue - 1], HLS_URL_MAX);
        h->n_queue = 2;
    }
    return h->n_queue;
}

/* Confirmed live (BACKLOG R111): the caller above this (radio_buffer.c's
 * rb_worker_hls()) retries in a tight loop with only a 500ms backoff
 * whenever this returns 0 -- and with no throttle here, an empty queue
 * meant a *full playlist re-fetch* (fork+exec+TLS handshake to the same
 * host) on every single one of those retries, not just once per segment.
 * For a 4-second-target stream, waiting for one new segment could mean
 * roughly eight playlist re-fetches before it appeared, each one real
 * fork/TLS cost on this device's single CPU core -- shown live to be the
 * dominant cause of radio playback measurably falling behind live over
 * time (a direct-MP3 station, one persistent connection, stayed stable
 * under the same conditions where an HLS one drifted badly). A playlist
 * genuinely cannot have new content more often than the server's own
 * declared segment duration (target_ms) allows, so re-fetching faster
 * than that never finds anything new -- it only spends CPU proving it.
 * Skipping the actual network call when called again too soon (returning
 * "nothing new" immediately, exactly what a real too-early refresh would
 * have found anyway) cuts the wasted re-fetches to roughly zero while
 * changing no observable behaviour: the first refresh attempt after
 * target_ms/2 has elapsed still runs on the very next call, so a segment
 * appearing right on schedule is picked up just as promptly as before.
 *
 * last_refresh_mono is stamped only when a refresh *finds nothing new* --
 * not on every attempt. Stamping it unconditionally (tried first) throttles
 * the call immediately after a *successful* refresh too, adding min_gap of
 * dead time to every single segment cycle rather than only to genuine idle
 * waiting -- confirmed live as making the drift measurably worse, not
 * better: a stream producing a new segment every ~3.75s but forced to wait
 * out an unconditional ~2s gap before each check falls behind on every
 * cycle by design, not by accident. Leaving the timestamp untouched after
 * a hit means the next empty check starts its own throttle window fresh,
 * so a live stream producing segments back-to-back never waits at all --
 * only a genuinely idle poll (nothing new since the last check) pays the
 * wait, which is exactly the case this exists to cut down on. */
int hls_next(hls_t *h, unsigned char *buf, int max) {
    if (h->n_queue == 0) {
        double now = now_mono();
        double min_gap = (h->target_ms > 0 ? h->target_ms : 4000) / 2000.0;
        if (h->last_refresh_mono > 0 && now - h->last_refresh_mono < min_gap)
            return 0;
        if (refresh(h) <= 0) { h->last_refresh_mono = now; return 0; }
    }
    if (h->n_queue == 0) return 0;

    int n = fetch(h->queue[0], buf, max);
    memmove(h->queue[0], h->queue[1], (size_t)(HLS_QUEUE - 1) * HLS_URL_MAX);
    h->n_queue--;
    return n > 0 ? n : 0;
}
