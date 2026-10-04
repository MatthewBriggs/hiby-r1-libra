/* podcast.c — see podcast.h. Ported from the standalone Podcasts app's
 * podcast_hook.c; drawing, input, volume/lock and the tile-hijack machinery
 * all dropped, since Libra already owns all of that. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "podcast.h"
#include "audio.h"

#define PODCAST_DIR "/data/mnt/sd_0/Podcasts"
#define RESUME_DIR  "/data/mnt/sd_0/.podsync"
/* Internal storage (UBIFS), not RESUME_DIR on the SD card -- this is only
 * ever a few KB (pod_resume_store() caps it at 127 entries), so the space
 * cost is negligible, and reordering pod_play_episode() (resume lookup
 * before audio_play(), not after) already fixed the actual bug this file
 * being on the SD card exposed: a synchronous read racing the worker
 * thread's own concurrent SD-card work. This is belt-and-suspenders on
 * top of that fix, not a replacement for it -- moving the file sidesteps
 * this specific contention entirely rather than depending on call-order
 * staying correct forever, and reads/writes a few KB file without ever
 * touching this exFAT card's own well-documented stall-under-load quirks
 * (see index.c's own comment) at all. */
#define RESUME_FILE "/usr/data/podcast_resume.txt"
#define SYNC_SCRIPT RESUME_DIR "/podsync_once.sh"
#define PODSYNC_CURL RESUME_DIR "/curl"
#define PODSYNC_CA   RESUME_DIR "/cacert.pem"
#define DL_HDR_PATH  "/tmp/.pod_dl_headers"
#define SYNC_LOG     "/tmp/.podsync_run.log"
#define FEEDS_PATH   RESUME_DIR "/feeds.txt"
#define SETTINGS_PATH "/data/mnt/sd_0/settings.txt"

static void sort_feeds(pod_feed_t *items, int n) {
    for (int i = 1; i < n; i++) {
        pod_feed_t tmp = items[i];
        int j = i - 1;
        while (j >= 0 && strcasecmp(items[j].name, tmp.name) > 0) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = tmp;
    }
}

int pod_scan_feeds(pod_feed_t *out, int max) {
    int n = 0;
    DIR *d = opendir(PODCAST_DIR);
    if (!d) return 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[POD_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", PODCAST_DIR, e->d_name);
        DIR *sub = opendir(path);
        if (!sub) continue;
        closedir(sub);
        snprintf(out[n++].name, POD_NAME_LEN, "%s", e->d_name);
    }
    closedir(d);
    sort_feeds(out, n);
    return n;
}

/* ---- resume ---------------------------------------------------------- */

int pod_resume_lookup(const char *path, int *dur_out) {
    if (dur_out) *dur_out = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) return 0;
    char line[POD_PATH_LEN + 48];
    int ms = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = '\0';
        char *rest = t1 + 1;
        char *t2 = strchr(rest, '\t');
        int dur = 0;
        char *pathp;
        if (t2) { *t2 = '\0'; dur = atoi(rest); pathp = t2 + 1; }
        else    { pathp = rest; }
        if (strcmp(pathp, path) == 0) {
            ms = atoi(line);
            if (dur_out) *dur_out = dur;
            break;
        }
    }
    fclose(f);
    return ms;
}

void pod_resume_store(const char *path, int ms, int dur) {
    /* No mkdir here -- RESUME_FILE lives directly under /usr/data, which
     * always exists after boot, unlike its old home inside RESUME_DIR
     * (the SD card's .podsync/, which podsync_once.sh itself creates). */
    char (*keep)[POD_PATH_LEN + 32] = malloc(sizeof(*keep) * 128);
    if (!keep) return;
    int n = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (f) {
        char line[POD_PATH_LEN + 32];
        while (n < 127 && fgets(line, sizeof(line), f)) {
            char probe[POD_PATH_LEN + 32];
            snprintf(probe, sizeof(probe), "%s", line);
            char *nl = strchr(probe, '\n');
            if (nl) *nl = '\0';
            char *t1 = strchr(probe, '\t');
            if (t1) {
                char *rest = t1 + 1;
                char *t2 = strchr(rest, '\t');
                char *pathp = t2 ? t2 + 1 : rest;
                if (strcmp(pathp, path) == 0) continue;   /* replaced below */
            }
            snprintf(keep[n++], POD_PATH_LEN + 32, "%s", line);
        }
        fclose(f);
    }
    /* Write-then-rename, not truncate-in-place -- see podcast.h/the original
     * app's comment: this runs every few seconds while playing, and this
     * device does get its player OOM-killed, so a truncating write here has
     * lost every saved resume position, not just the current one, in the
     * window that used to be entered thousands of times a day. */
    char tmp[sizeof(RESUME_FILE) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", RESUME_FILE);
    f = fopen(tmp, "w");
    if (f) {
        if (ms > 3000 || ms == POD_FINISHED) fprintf(f, "%d\t%d\t%s\n", ms, dur, path);
        for (int i = 0; i < n; i++) fputs(keep[i], f);
        int ok = (fflush(f) == 0);
        fclose(f);
        if (!ok || rename(tmp, RESUME_FILE) != 0) unlink(tmp);
    }
    free(keep);
}

/* R70: swipe-to-remove on the episode list frees the download, not the
 * episode itself -- pod_load_episodes() re-derives "not downloaded" from
 * the file's absence next time it runs (from episodes.tsv if the feed
 * still lists it there, same as any episode never downloaded in the first
 * place). Only touches disk and the resume file; the caller owns clearing
 * its own pod_eps[]/tracks[] entry's path/downloaded/dur_ms fields, same
 * split pod_download_start()/pod_download_poll() already have with the
 * caller for the opposite direction. */
void pod_delete_download(const char *path) {
    if (!path || !path[0]) return;
    unlink(path);
    pod_resume_store(path, 0, 0);
}

/* ---- per-feed intro/outro skip ------------------------------------------- */

/* Seconds to skip at the start and the end of every episode of a feed, set
 * from that podcast's own settings page. "intro<TAB>outro<TAB>feed", one line
 * per feed that has either; a feed with neither has no line. */
#define SKIP_FILE "/usr/data/podcast_skip.txt"

void pod_skip_lookup(const char *feed, int *intro_s, int *outro_s) {
    *intro_s = *outro_s = 0;
    if (!feed || !feed[0]) return;
    FILE *f = fopen(SKIP_FILE, "r");
    if (!f) return;
    char line[POD_NAME_LEN + 48];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *t1 = strchr(line, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2 || strcmp(t2 + 1, feed) != 0) continue;
        *intro_s = atoi(line);
        *outro_s = atoi(t1 + 1);
        break;
    }
    fclose(f);
}

void pod_skip_store(const char *feed, int intro_s, int outro_s) {
    if (!feed || !feed[0]) return;
    /* Rewritten whole, by rename, like the speed file below: a write cut
     * short must not cost every other feed its setting. */
    char (*keep)[POD_NAME_LEN + 48] = malloc(sizeof(*keep) * 128);
    if (!keep) return;
    int n = 0;
    FILE *f = fopen(SKIP_FILE, "r");
    if (f) {
        char line[POD_NAME_LEN + 48];
        while (n < 127 && fgets(line, sizeof(line), f)) {
            char probe[POD_NAME_LEN + 48];
            snprintf(probe, sizeof(probe), "%s", line);
            probe[strcspn(probe, "\n")] = '\0';
            char *t1 = strchr(probe, '\t');
            char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            if (t2 && strcmp(t2 + 1, feed) == 0) continue;   /* replaced below */
            snprintf(keep[n++], POD_NAME_LEN + 48, "%s", line);
        }
        fclose(f);
    }
    char tmp[sizeof(SKIP_FILE) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", SKIP_FILE);
    f = fopen(tmp, "w");
    if (f) {
        if (intro_s > 0 || outro_s > 0) fprintf(f, "%d\t%d\t%s\n", intro_s, outro_s, feed);
        for (int i = 0; i < n; i++) fputs(keep[i], f);
        int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
        ok = fclose(f) == 0 && ok;
        if (!ok || rename(tmp, SKIP_FILE) != 0) unlink(tmp);
    }
    free(keep);
}

/* ---- per-feed playback speed -------------------------------------------- */

#define SPEED_FILE "/usr/data/podcast_speed.txt"

int pod_speed_lookup(const char *feed) {
    if (!feed || !feed[0]) return 0;
    FILE *f = fopen(SPEED_FILE, "r");
    if (!f) return 0;
    char line[POD_NAME_LEN + 32];
    int permille = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = '\0';
        if (strcmp(t1 + 1, feed) == 0) {
            permille = atoi(line);
            break;
        }
    }
    fclose(f);
    return permille;
}

void pod_speed_store(const char *feed, int permille) {
    if (!feed || !feed[0]) return;
    /* Same write-then-rename shape pod_resume_store() uses, and the same
     * reason: an OOM kill mid-write must not cost every other feed's own
     * saved speed, not just the one just changed. Feed count is nothing
     * like episode count, but there's no reason to trust that forever --
     * same 127-entry cap as the resume file, for the same reason. */
    char (*keep)[POD_NAME_LEN + 32] = malloc(sizeof(*keep) * 128);
    if (!keep) return;
    int n = 0;
    FILE *f = fopen(SPEED_FILE, "r");
    if (f) {
        char line[POD_NAME_LEN + 32];
        while (n < 127 && fgets(line, sizeof(line), f)) {
            char probe[POD_NAME_LEN + 32];
            snprintf(probe, sizeof(probe), "%s", line);
            char *nl = strchr(probe, '\n');
            if (nl) *nl = '\0';
            char *t1 = strchr(probe, '\t');
            if (t1 && strcmp(t1 + 1, feed) == 0) continue;   /* replaced below */
            snprintf(keep[n++], POD_NAME_LEN + 32, "%s", line);
        }
        fclose(f);
    }
    char tmp[sizeof(SPEED_FILE) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", SPEED_FILE);
    f = fopen(tmp, "w");
    if (f) {
        if (permille != 1000) fprintf(f, "%d\t%s\n", permille, feed);
        for (int i = 0; i < n; i++) fputs(keep[i], f);
        int ok = (fflush(f) == 0);
        fclose(f);
        if (!ok || rename(tmp, SPEED_FILE) != 0) unlink(tmp);
    }
    free(keep);
}

/* ---- episodes ---------------------------------------------------------- */

/* Cached from the last pod_load_episodes() call, so pod_download_start(idx)
 * can find that episode's name/url/feed-dir without the caller re-passing
 * them -- same shape as the standalone app's single global episodes[]. */
static char g_feed_dir[POD_PATH_LEN];
static char g_feed[POD_NAME_LEN];       /* the feed's own name, g_feed_dir's last part */
static char g_ep_name[POD_MAX_ITEMS][POD_NAME_LEN];
static char g_ep_url[POD_MAX_ITEMS][POD_PATH_LEN];
static long g_ep_mtime[POD_MAX_ITEMS];
static int  g_ep_n;

static long parse_iso_date(const char *s) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday,
               &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec) != 6)
        return 0;
    tmv.tm_year -= 1900;
    tmv.tm_mon  -= 1;
    tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    return (t == (time_t)-1) ? 0 : (long)t;
}

/* BG48/BG49: an episode's duration/seek/bitrate were all silently 0. Root
 * cause: audio.c's MP3 open deliberately leaves the decoded frame count at
 * 0 rather than scan a VBR file whole to get it (see dec_open()'s own
 * comment on that), so audio_dur_ms() never has an answer for MP3 either --
 * a library track gets its duration from audio_probe_dur_ms(path,
 * bitrate_bps), which for MP3 only estimates from filesize/bitrate, and
 * that bitrate comes from the SQL index's own ID3 read at scan time. A
 * podcast episode was never scanned, so there is no bitrate anywhere to
 * pass it. This reads the *first* MPEG frame header directly (skipping any
 * ID3v2 tag first -- routine on a podcast MP3, and often sizeable with
 * embedded cover art, so scanning through it risked a spurious sync-word
 * match in tag data) to get a real bitrate straight from the file, the same
 * "no full decode" trade audio_probe_dur_ms() already documents for its own
 * estimate. CBR-exact; a VBR file's true average can differ from its first
 * frame, same caveat that estimate already carries. */
static int pod_mp3_kbps(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[10];
    long off = 0;
    if (fread(hdr, 1, 10, f) == 10 && !memcmp(hdr, "ID3", 3)) {
        long sz = ((long)(hdr[6] & 0x7f) << 21) | ((long)(hdr[7] & 0x7f) << 14) |
                  ((long)(hdr[8] & 0x7f) << 7)  |  (long)(hdr[9] & 0x7f);
        off = 10 + sz;
    }
    fseek(f, off, SEEK_SET);
    unsigned char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    static const int v1l3[16] = { 0, 32, 40, 48, 56, 64, 80, 96,
                                  112,128,160,192,224,256,320,  0 };
    static const int v2l3[16] = { 0,  8, 16, 24, 32, 40, 48, 56,
                                   64, 80, 96,112,128,144,160,  0 };
    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        int ver   = (buf[i + 1] >> 3) & 3;   /* 3 = MPEG1, 2/0 = MPEG2/2.5 */
        int layer = (buf[i + 1] >> 1) & 3;   /* 1 = Layer III */
        if (layer != 1) continue;
        int bri = (buf[i + 2] >> 4) & 0xF;
        if (bri == 0 || bri == 15) continue;
        int kbps = (ver == 3) ? v1l3[bri] : v2l3[bri];
        if (kbps > 0) return kbps;
    }
    return 0;
}

/* Probes duration once per episode -- cheap (a header read, no full decode;
 * see audio_probe_dur_ms()'s and pod_mp3_kbps()'s own comments) but no
 * reason to redo it on every pod_load_episodes() call once an episode has
 * an answer. */
static void pod_probe_dur(pod_episode_t *e) {
    if (!e->downloaded || e->dur_ms > 0) return;
    const char *dot = strrchr(e->path, '.');
    int kbps = (dot && !strcasecmp(dot, ".mp3")) ? pod_mp3_kbps(e->path) : 0;
    e->dur_ms = audio_probe_dur_ms(e->path, kbps * 1000);
}

static int is_audio_ext(const char *n) {
    const char *d = strrchr(n, '.');
    if (!d) return 0;
    return !strcasecmp(d, ".mp3") || !strcasecmp(d, ".m4a") ||
           !strcasecmp(d, ".m4b") || !strcasecmp(d, ".aac") ||
           !strcasecmp(d, ".ogg") || !strcasecmp(d, ".opus") ||
           !strcasecmp(d, ".wav") || !strcasecmp(d, ".flac");
}

static void sort_episodes(pod_episode_t *items, int n) {
    /* Newest first by mtime -- download order for a local file, pubdate for
     * a manifest-only one. Name-only sorting only looks right when titles
     * happen to start with a number. */
    for (int i = 1; i < n; i++) {
        pod_episode_t tmp = items[i];
        int j = i - 1;
        while (j >= 0 && items[j].mtime < tmp.mtime) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = tmp;
    }
}

/* Opening a feed looked up each downloaded episode's resume position by
 * reopening RESUME_FILE once per episode, and measured each one's length by
 * reading the start of the file -- every time, since a length was only kept
 * once the episode had been played. Together most of a feed's opening time
 * (289 of 386 ms, measured). Now the resume file is read once per opening,
 * and lengths are measured once ever and kept here, "ms<TAB>path". */
#define DUR_FILE "/usr/data/podcast_durations2.txt"   /* "2": the first counted ID3 tags as audio */

typedef struct { char path[POD_PATH_LEN]; int a, b; } pod_kv_t;

static pod_kv_t *g_durs;
static int g_durs_n, g_durs_cap, g_durs_loaded;

static int kv_push(pod_kv_t **arr, int *n, int *cap, const char *path, int a, int b) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        pod_kv_t *na = realloc(*arr, sizeof(**arr) * (size_t)nc);
        if (!na) return -1;
        *arr = na;
        *cap = nc;
    }
    snprintf((*arr)[*n].path, POD_PATH_LEN, "%s", path);
    (*arr)[*n].a = a;
    (*arr)[*n].b = b;
    (*n)++;
    return 0;
}

static int kv_find(const pod_kv_t *arr, int n, const char *path) {
    for (int i = n - 1; i >= 0; i--)       /* newest wins: appended last */
        if (!strcmp(arr[i].path, path)) return i;
    return -1;
}

static void durs_load(void) {
    if (g_durs_loaded) return;
    g_durs_loaded = 1;
    FILE *f = fopen(DUR_FILE, "r");
    if (!f) return;
    char line[POD_PATH_LEN + 32];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        int ms = atoi(line);
        if (ms > 0) kv_push(&g_durs, &g_durs_n, &g_durs_cap, tab + 1, ms, 0);
    }
    fclose(f);
}

static void durs_add(const char *path, int ms) {
    if (ms <= 0) return;
    kv_push(&g_durs, &g_durs_n, &g_durs_cap, path, ms, 0);
    FILE *f = fopen(DUR_FILE, "a");
    if (!f) return;
    fprintf(f, "%d\t%s\n", ms, path);
    fclose(f);
}

/* RESUME_FILE in memory, for one feed opening: path, ms, dur -- the same
 * fields pod_resume_lookup() reads, read once instead of per episode. */
static int resume_table(pod_kv_t **out, int *cap) {
    int n = 0;
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) return 0;
    char line[POD_PATH_LEN + 48];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = '\0';
        char *rest = t1 + 1;
        char *t2 = strchr(rest, '\t');
        int dur = 0;
        char *pathp = rest;
        if (t2) { *t2 = '\0'; dur = atoi(rest); pathp = t2 + 1; }
        kv_push(out, &n, cap, pathp, atoi(line), dur);
    }
    fclose(f);
    return n;
}

/* How long the last pod_load_episodes() spent in each step, in ms: the folder
 * listing, the manifest, the sort, and the per-episode resume and length
 * lookups. Reported live as a feed taking seconds to open; these say where. */
static int g_load_ms[4];
void pod_load_timing(int out[4]) { memcpy(out, g_load_ms, sizeof(g_load_ms)); }

static long pod_ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000L + (t1.tv_nsec - t0->tv_nsec) / 1000000L;
}

int pod_load_episodes(const char *feed, pod_episode_t *out, int max) {
    int n = 0;
    struct timespec tl;
    clock_gettime(CLOCK_MONOTONIC, &tl);
    snprintf(g_feed_dir, sizeof(g_feed_dir), "%s/%s", PODCAST_DIR, feed);
    snprintf(g_feed, sizeof(g_feed), "%s", feed);
    DIR *d = opendir(g_feed_dir);
    if (!d) { g_ep_n = 0; return 0; }
    struct dirent *e;
    /* Nothing is ever deleted automatically, so a long-running feed can
     * exceed `max`. Once full, displace whichever entry is currently oldest
     * rather than stopping -- readdir order on FAT32 is roughly creation
     * order, not date order, so stopping early would hide new episodes
     * behind old ones. */
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!is_audio_ext(e->d_name)) continue;

        char path[POD_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", g_feed_dir, e->d_name);
        struct stat st;
        long mt = (stat(path, &st) == 0) ? (long)st.st_mtime : 0;

        int slot;
        if (n < max) {
            slot = n++;
        } else {
            slot = 0;
            for (int i = 1; i < max; i++)
                if (out[i].mtime < out[slot].mtime) slot = i;
            if (mt <= out[slot].mtime) continue;
        }

        snprintf(out[slot].path, POD_PATH_LEN, "%s", path);
        out[slot].mtime = mt;
        snprintf(out[slot].name, POD_NAME_LEN, "%s", e->d_name);
        char *dot = strrchr(out[slot].name, '.');
        if (dot) *dot = '\0';
        out[slot].downloaded = 1;
        out[slot].url[0] = '\0';
    }
    closedir(d);
    g_load_ms[0] = (int)pod_ms_since(&tl);
    clock_gettime(CLOCK_MONOTONIC, &tl);

    /* R18: fold in episodes.tsv, the manifest podsync_once.sh writes of
     * every episode currently in the feed's recent window, not just the
     * ones fetched. Its `base` field is the same sanitize(title) the
     * download filename uses, so a manifest entry that already has a local
     * file needs no reimplementation of sanitize() in C to detect -- a
     * plain strcmp against the name each local file was just given works. */
    char manifest[POD_PATH_LEN];
    snprintf(manifest, sizeof(manifest), "%s/episodes.tsv", g_feed_dir);
    FILE *mf = fopen(manifest, "r");
    if (mf) {
        char line[1024];
        while (n < max && fgets(line, sizeof(line), mf)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            char *base = line;
            char *url = strchr(base, '\t');
            if (!url) continue;
            *url++ = '\0';
            char *date = strchr(url, '\t');
            if (date) *date++ = '\0';

            int known = 0;
            for (int i = 0; i < n; i++)
                if (!strcmp(out[i].name, base)) { known = 1; break; }
            if (known) continue;

            int slot = n++;
            snprintf(out[slot].name, POD_NAME_LEN, "%s", base);
            out[slot].path[0] = '\0';
            out[slot].mtime = date ? parse_iso_date(date) : 0;
            out[slot].downloaded = 0;
            snprintf(out[slot].url, POD_PATH_LEN, "%s", url);
        }
        fclose(mf);
    }

    g_load_ms[1] = (int)pod_ms_since(&tl);
    clock_gettime(CLOCK_MONOTONIC, &tl);
    sort_episodes(out, n);
    g_load_ms[2] = (int)pod_ms_since(&tl);
    clock_gettime(CLOCK_MONOTONIC, &tl);
    pod_kv_t *res = NULL;
    int res_cap = 0;
    int res_n = resume_table(&res, &res_cap);
    durs_load();
    for (int i = 0; i < n; i++) {
        if (out[i].downloaded) {
            /* Same answer pod_resume_lookup() gives -- the first matching
             * line -- from the table read once above. */
            out[i].resume_ms = 0;
            out[i].dur_ms = 0;
            for (int k = 0; k < res_n; k++)
                if (!strcmp(res[k].path, out[i].path)) {
                    out[i].resume_ms = res[k].a;
                    out[i].dur_ms = res[k].b;
                    break;
                }
            /* The measured length wins over one saved with a resume position:
             * those were the old estimate, which counted ID3 tags as audio. */
            int d = kv_find(g_durs, g_durs_n, out[i].path);
            if (d >= 0) {
                out[i].dur_ms = g_durs[d].a;
            } else {
                out[i].dur_ms = 0;
                pod_probe_dur(&out[i]);
                durs_add(out[i].path, out[i].dur_ms);
            }
        } else {
            out[i].resume_ms = 0; out[i].dur_ms = 0;
        }
        /* Cache for pod_download_start(idx). */
        snprintf(g_ep_name[i], POD_NAME_LEN, "%s", out[i].name);
        snprintf(g_ep_url[i], POD_PATH_LEN, "%s", out[i].url);
        g_ep_mtime[i] = out[i].mtime;
    }
    free(res);
    g_load_ms[3] = (int)pod_ms_since(&tl);
    g_ep_n = n;
    return n;
}

/* ---- on-demand download (R18) -------------------------------------------- */

static pid_t dl_pid = -1;
static int   dl_slot = -1;
static long  dl_bytes;
static long  dl_total;
static char  dl_part_path[POD_PATH_LEN];
static char  dl_final_path[POD_PATH_LEN];
static int   dl_ok;
/* What is downloading, captured when it starts rather than looked up again
 * through g_ep_*[dl_slot] when it ends: g_ep_* is whichever feed was opened
 * last, and browsing to another feed mid-download used to stamp the finished
 * file with the other feed's date for that slot. */
static char  dl_feed[POD_NAME_LEN];
static char  dl_name[POD_NAME_LEN];
static long  dl_mtime;

static const char *ext_for_url(const char *url) {
    static char ext[8];
    ext[0] = '\0';
    const char *q = strpbrk(url, "?#");
    const char *end = q ? q : url + strlen(url);
    const char *dot = NULL;
    for (const char *p = url; p < end; p++) if (*p == '.') dot = p;
    if (dot) {
        size_t n = 0;
        for (const char *p = dot + 1; p < end && n < sizeof(ext) - 1; p++, n++)
            ext[n] = (char)tolower((unsigned char)*p);
        ext[n] = '\0';
    }
    static const char *ok[] = { "mp3", "m4a", "m4b", "aac", "ogg", "oga",
                                 "opus", "wav", "flac", NULL };
    for (int i = 0; ok[i]; i++) if (!strcmp(ext, ok[i])) return ext;
    return "mp3";
}

static int dl_begin(const char *feed, const char *name, const char *url, long mtime, int slot) {
    if (dl_pid > 0 || !url[0]) return -1;
    const char *ext = ext_for_url(url);
    snprintf(dl_final_path, sizeof(dl_final_path), "%s/%s/%s.%s",
             PODCAST_DIR, feed, name, ext);
    snprintf(dl_part_path, sizeof(dl_part_path), "%s.part", dl_final_path);
    unlink(dl_part_path);
    unlink(DL_HDR_PATH);

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);       /* cancellation must include curl descendants */
        execl(PODSYNC_CURL, PODSYNC_CURL, "-fsSL", "--cacert", PODSYNC_CA,
              "--connect-timeout", "20", "--max-time", "900",
              "-D", DL_HDR_PATH, "-o", dl_part_path, url,
              (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    dl_pid = pid;
    dl_slot = slot;
    dl_bytes = 0;
    dl_total = 0;
    dl_mtime = mtime;
    snprintf(dl_feed, sizeof(dl_feed), "%s", feed);
    snprintf(dl_name, sizeof(dl_name), "%s", name);
    return 0;
}

void pod_download_start(int idx) {
    if (idx < 0 || idx >= g_ep_n) return;
    dl_begin(g_feed, g_ep_name[idx], g_ep_url[idx], g_ep_mtime[idx], idx);
}

int pod_download_start_named(const char *feed, const char *name) {
    if (dl_pid > 0 || !feed[0] || !name[0]) return -1;
    char manifest[POD_PATH_LEN];
    snprintf(manifest, sizeof(manifest), "%s/%s/episodes.tsv", PODCAST_DIR, feed);
    FILE *mf = fopen(manifest, "r");
    if (!mf) return -1;
    /* Same parse as pod_load_episodes()'s manifest fold-in. */
    char line[1024];
    int rc = -1;
    while (fgets(line, sizeof(line), mf)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *url = strchr(line, '\t');
        if (!url) continue;
        *url++ = '\0';
        char *date = strchr(url, '\t');
        if (date) *date++ = '\0';
        if (strcmp(line, name) != 0) continue;
        /* Already on the card (fetched some other way meanwhile)? Then there
         * is nothing to do, which counts as done rather than as failure. */
        const char *ext = ext_for_url(url);
        char final_path[POD_PATH_LEN];
        snprintf(final_path, sizeof(final_path), "%s/%s/%s.%s", PODCAST_DIR, feed, name, ext);
        struct stat st;
        if (stat(final_path, &st) == 0) { rc = 1; break; }
        rc = dl_begin(feed, name, url, date ? parse_iso_date(date) : 0, -1);
        break;
    }
    fclose(mf);
    return rc;
}

const char *pod_download_feed(void) { return dl_pid > 0 ? dl_feed : ""; }
const char *pod_download_name(void) { return dl_pid > 0 ? dl_name : ""; }

int pod_download_active(void) { return dl_pid > 0; }
int pod_download_slot(void)   { return dl_slot; }
long pod_download_bytes(void) { return dl_bytes; }
long pod_download_total(void) { return dl_total; }

int pod_download_poll(void) {
    if (dl_pid <= 0) return -1;

    /* BG110: curl's -D dump-header file gets one header block per hop when
     * a URL redirects (common for podcast enclosures, which are routinely
     * fronted by a tracking-redirect host) -- all appended to the same
     * file, in order. Grabbing the first "Content-Length" seen, as this
     * used to, latches onto an early hop's own (often tiny, unrelated)
     * length rather than the final audio file's, and -- gated on
     * `dl_total <= 0` -- stayed wrong for the rest of the download once
     * caught. Reported live as the on-screen percent climbing into six
     * digits then going negative: the real byte count kept growing against
     * that wrong small denominator, and `dl_bytes * 100` eventually
     * overflowed this build's 32-bit `long`. Re-parsed every poll instead
     * of once, resetting on each "HTTP/" status line so only the *last*
     * block's Content-Length -- the actual response being downloaded --
     * is kept; cheap (a small local file, a handful of redirects at most)
     * and self-correcting as curl appends more of the chain. */
    {
        FILE *hf = fopen(DL_HDR_PATH, "r");
        if (hf) {
            char line[256];
            long v, cur = -1;
            while (fgets(line, sizeof(line), hf)) {
                if (!strncmp(line, "HTTP/", 5)) {
                    cur = -1;
                } else if (sscanf(line, "Content-Length: %ld", &v) == 1 ||
                           sscanf(line, "content-length: %ld", &v) == 1) {
                    cur = v;
                }
            }
            if (cur > 0) dl_total = cur;
            fclose(hf);
        }
    }
    struct stat pst;
    if (stat(dl_part_path, &pst) == 0) dl_bytes = (long)pst.st_size;

    int status;
    pid_t r = waitpid(dl_pid, &status, WNOHANG);
    if (r == 0) return 0;

    int ok = (r == dl_pid) && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    struct stat fst;
    if (ok && (stat(dl_part_path, &fst) != 0 || fst.st_size <= 0)) ok = 0;

    if (ok) {
        rename(dl_part_path, dl_final_path);
        if (dl_mtime > 0) {
            struct utimbuf ub;
            ub.actime = ub.modtime = (time_t)dl_mtime;
            utime(dl_final_path, &ub);
        }
    } else {
        unlink(dl_part_path);
    }
    dl_ok = ok;
    dl_pid = -1;
    return 1;
}

/* ---- whole-feed sync ------------------------------------------------- */

/* R64: feeds.txt used to be the only place to manage subscriptions -- hand-
 * edited directly on the card, with no connection to settings.txt at all.
 * Regenerated here from a "Podcasts" section in settings.txt, one URL per
 * line, same blank-line-ends-the-section convention every other section
 * (WiFi/Radio/LastFM/Spotify) already uses -- run right before every sync,
 * so settings.txt is the actual source of truth and feeds.txt becomes a
 * generated file rather than something to hand-edit. podsync_once.sh itself
 * is untouched: it still just reads feeds.txt, one format to trust, rather
 * than teaching a shell script to parse settings.txt's section format too.
 *
 * Deliberately non-destructive when there's nothing to generate from: no
 * settings.txt, or a settings.txt with no "Podcasts" section (or one with a
 * section header but zero URLs under it) at all, means the section is
 * missing rather than genuinely emptied on purpose -- leaves feeds.txt
 * exactly as it already was rather than truncating a subscription list
 * whose real source hasn't migrated yet. */
static void pod_sync_feeds_from_settings(void) {
    FILE *in = fopen(SETTINGS_PATH, "r");
    if (!in) return;
    char line[512];
    int in_section = 0;
    char tmp_path[300];
    snprintf(tmp_path, sizeof(tmp_path), "%s.new", FEEDS_PATH);
    FILE *out = NULL;
    while (fgets(line, sizeof(line), in)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!in_section) {
            if (!strcmp(s, "Podcasts")) in_section = 1;
            continue;
        }
        if (s[0] == '\0') break;      /* blank line: end of section */
        if (s[0] == '#') continue;
        if (!out) {
            out = fopen(tmp_path, "w");
            if (!out) break;
            fprintf(out, "# Generated from settings.txt's Podcasts section -- edit there, not here.\n");
        }
        fprintf(out, "%s\n", s);
    }
    fclose(in);
    if (out) { fclose(out); rename(tmp_path, FEEDS_PATH); }
}

static pid_t update_pid = -1;
static int   update_running_flag;
static int   update_died_flag;

/* The sync script, built into the app (podsync_embed.h, made from
 * podsync/podsync_once.sh at build time). It used to be only whatever had
 * been copied onto the card by hand -- fine while nothing read its output
 * but people, not now the app marks each podcast from its "@" lines. Before
 * every sync the card's copy is replaced if it differs, keeping the one it
 * replaces as .bak. */
#include "podsync_embed.h"

static void pod_install_script(void) {
    const unsigned char *want = podsync_once_sh;
    size_t want_n = podsync_once_sh_len;
    FILE *f = fopen(SYNC_SCRIPT, "rb");
    if (f) {
        int same = 1;
        size_t at = 0;
        unsigned char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (at + n > want_n || memcmp(buf, want + at, n) != 0) { same = 0; break; }
            at += n;
        }
        fclose(f);
        if (same && at == want_n) return;
        char bak[sizeof(SYNC_SCRIPT) + 8];
        snprintf(bak, sizeof(bak), "%s.bak", SYNC_SCRIPT);
        rename(SYNC_SCRIPT, bak);
    }
    char tmp[sizeof(SYNC_SCRIPT) + 8];
    snprintf(tmp, sizeof(tmp), "%s.new", SYNC_SCRIPT);
    f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(want, 1, want_n, f) == want_n;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, SYNC_SCRIPT) != 0) unlink(tmp);
}

/* ---- per-podcast sync status --------------------------------------------- */

/* Read from the script's "@" lines as they arrive: which podcast it is on,
 * and how each one ended. Kept after the sync finishes, until the next. */
typedef struct { char name[POD_NAME_LEN]; int state, new_n; } pod_sst_t;
#define POD_SST_MAX 64
static pod_sst_t g_sst[POD_SST_MAX];
static int  g_sst_n;
static long g_sst_off;               /* how far into SYNC_LOG has been read */

static pod_sst_t *sst_get(const char *name) {
    if (!name[0]) return NULL;
    for (int i = 0; i < g_sst_n; i++) if (!strcmp(g_sst[i].name, name)) return &g_sst[i];
    if (g_sst_n == POD_SST_MAX) return NULL;
    pod_sst_t *e = &g_sst[g_sst_n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->state = POD_SYNC_NONE;
    e->new_n = 0;
    return e;
}

int pod_sync_status_poll(void) {
    FILE *f = fopen(SYNC_LOG, "r");
    if (!f) return 0;
    if (fseek(f, g_sst_off, SEEK_SET) != 0) { fclose(f); return 0; }
    int changed = 0;
    char line[512];
    for (;;) {
        long at = ftell(f);
        if (!fgets(line, sizeof(line), f)) break;
        size_t len = strlen(line);
        if (len == 0 || line[len - 1] != '\n') { g_sst_off = at; fclose(f); return changed; }  /* half a line: next time */
        line[len - 1] = '\0';
        g_sst_off = ftell(f);
        if (line[0] != '@') continue;
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = '\0';
        char *name = t1 + 1, *t2 = strchr(name, '\t');
        int n = 0;
        if (t2) { *t2 = '\0'; n = atoi(t2 + 1); }
        pod_sst_t *e;
        if (!strcmp(line, "@feed") || !strcmp(line, "@name")) {
            e = sst_get(name);
            if (e) { e->state = POD_SYNC_BUSY; e->new_n = 0; }
        } else if (!strcmp(line, "@ok") || !strcmp(line, "@fail")) {
            e = sst_get(name);
            if (e) { e->state = line[1] == 'o' ? POD_SYNC_OK : POD_SYNC_FAIL; e->new_n = n; }
        } else {
            continue;
        }
        changed = 1;
    }
    fclose(f);
    return changed;
}

int pod_sync_status(const char *feed, int *new_n) {
    for (int i = 0; i < g_sst_n; i++)
        if (!strcmp(g_sst[i].name, feed)) {
            if (new_n) *new_n = g_sst[i].new_n;
            return g_sst[i].state;
        }
    return POD_SYNC_NONE;
}

/* A sync this process did not start: one left running when Libra restarted
 * (the script is its own process group, so it outlives the app -- found when
 * a restart mid-sync left it running unseen, the bar saying "Sync" and a tap
 * able to start a second one over it). Found by its command line; watched
 * with kill(pid, 0), since only a parent can waitpid() it. */
static pid_t update_foreign = -1;

static pid_t find_running_sync(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    pid_t self = getpid(), found = -1;
    struct dirent *e;
    while (found < 0 && (e = readdir(d)) != NULL) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid == self || pid == update_pid) continue;
        char p[48], cmd[512];
        snprintf(p, sizeof(p), "/proc/%s/cmdline", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n <= 0) continue;
        for (ssize_t i = 0; i < n; i++) if (!cmd[i]) cmd[i] = ' ';
        cmd[n] = '\0';
        if (strstr(cmd, "podsync_once.sh")) found = pid;
    }
    closedir(d);
    return found;
}

int pod_update_adopt(void) {
    if (update_running_flag) return 0;
    pid_t pid = find_running_sync();
    if (pid <= 0) return 0;
    update_foreign = pid;
    update_running_flag = 1;
    update_died_flag = 0;
    g_sst_n = 0;          /* its progress so far, from the top of its log */
    g_sst_off = 0;
    return 1;
}

void pod_update_start(void) {
    if (update_running_flag) return;
    if (pod_update_adopt()) return;   /* one is already running: that one */
    pod_install_script();
    g_sst_n = 0;
    g_sst_off = 0;
    /* Reported live: crashed the app repeatedly (crash-counted supervisor
     * forced a reboot) right after this was added and the user tried
     * Update feeds. Reverted the call immediately rather than debug it
     * live with the user locked out -- root cause not yet found, the
     * function itself read clean on review. Left in place, unused, so the
     * fix (whatever it turns out to be) doesn't have to be rewritten from
     * scratch. DO NOT re-enable this call until the actual crash is
     * reproduced and understood. */
    unlink(SYNC_LOG);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);       /* the shell starts writers of its own */
        execl("/bin/sh", "sh", SYNC_SCRIPT, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        update_pid = pid;
        update_died_flag = 0;
        update_running_flag = 1;
    }
}

int pod_update_running(void) { return update_running_flag; }
int pod_update_died(void)    { return update_died_flag; }

int pod_update_tail(char out[][POD_NAME_LEN], int max_lines) {
    FILE *f = fopen(SYNC_LOG, "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (!line[0]) continue;
        if (strcmp(line, "__DONE__") == 0) { update_running_flag = 0; continue; }
        if (line[0] == '@') continue;   /* progress for the podcast list, not reading */
        if (n < max_lines) {
            snprintf(out[n++], POD_NAME_LEN, "%s", line);
        } else {
            for (int i = 1; i < max_lines; i++)
                memcpy(out[i - 1], out[i], POD_NAME_LEN);
            snprintf(out[max_lines - 1], POD_NAME_LEN, "%s", line);
        }
    }
    fclose(f);
    return n;
}

/* Whether the run wrote its own completion marker. The caller's poll loop
 * normally consumes "__DONE__" itself (pod_update_tail() clears the running
 * flag on it), but that depends on a poll landing between the marker being
 * written and the process exiting -- microseconds apart. Checked here too so
 * "did it finish?" is answered by what the script actually recorded rather
 * than by which of the two won a race. */
static int sync_log_has_done(void) {
    FILE *f = fopen(SYNC_LOG, "r");
    if (!f) return 0;
    char line[256];
    int done = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strcmp(line, "__DONE__") == 0) { done = 1; break; }
    }
    fclose(f);
    return done;
}

void pod_update_reap(void) {
    if (update_foreign > 0) {
        if (kill(update_foreign, 0) == 0 || errno != ESRCH) return;   /* still going */
        update_foreign = -1;
        if (update_running_flag) {
            update_running_flag = 0;
            if (!sync_log_has_done()) update_died_flag = 1;
        }
        return;
    }
    if (update_pid <= 0) return;
    int status;
    if (waitpid(update_pid, &status, WNOHANG) != update_pid) return;
    update_pid = -1;
    if (update_running_flag) {
        update_running_flag = 0;
        /* Only a run that ended *without* its completion marker actually
         * stopped early -- see sync_log_has_done(). Reporting death purely
         * on "the child is gone" turned every normal finish into a red
         * "Sync stopped early." */
        if (!sync_log_has_done()) update_died_flag = 1;
    }
}

void pod_cancel_io(void) {
    /* Both children write directly onto the SD card.  They must be gone
     * before that block device is handed to the USB host. */
    if (dl_pid > 0) {
        kill(-dl_pid, SIGTERM);
        (void)waitpid(dl_pid, NULL, 0);
        dl_pid = -1;
        unlink(dl_part_path);
    }
    if (update_foreign > 0) {
        kill(-update_foreign, SIGTERM);
        update_foreign = -1;
        update_running_flag = 0;
        update_died_flag = 1;
    }
    if (update_pid > 0) {
        kill(-update_pid, SIGTERM);
        (void)waitpid(update_pid, NULL, 0);
        update_pid = -1;
        update_running_flag = 0;
        update_died_flag = 1;
    }
}

/* ---- show notes ---------------------------------------------------------- */

int pod_load_notes(const char *audio_path, char *out, int max_len) {
    out[0] = '\0';
    char p[POD_PATH_LEN];
    snprintf(p, sizeof(p), "%s", audio_path);
    char *dot = strrchr(p, '.');
    if (!dot) return 0;
    snprintf(dot, sizeof(p) - (size_t)(dot - p), ".txt");

    FILE *f = fopen(p, "r");
    if (!f) return 0;
    size_t n = fread(out, 1, (size_t)max_len - 1, f);
    fclose(f);
    out[n] = '\0';
    return (int)n;
}
