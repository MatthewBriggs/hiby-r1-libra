/* waveform.c -- the loudness shape of a music track, worked out in the
 * background and kept on the card. See waveform.h for the interface.
 *
 * Replaces R29's capture-from-playback, which sampled the peak of whatever
 * the output happened to be playing: that could only show a shape on the
 * second play of a track, threw the capture away after any seek or skip, and
 * was measured after the volume gain. Modelled on Sonix's waveform worker
 * (github.com/Jepl4r/sonix-player, src/system/audio/waveform.c), adapted to
 * this codebase's decoders and to the card's filesystem.
 *
 * The worker runs at SCHED_IDLE: it only gets the core when nothing else --
 * decode, bluealsa's encoder, the UI -- wants it. On one core that is the
 * difference between a background decode that costs nothing and one that
 * competes with playback. It is not stopped when the screen locks: the shape
 * is for the file, not the moment, and a locked screen is exactly when the
 * core is free. */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "waveform.h"

#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif

/* One file for every track rather than a file per track. The card is exFAT
 * at 477 GB, where the smallest file still occupies a whole cluster, and a
 * directory of thousands of entries is a linear search on every open; R29's
 * per-track .wave files paid both. A record here is 152 bytes, so a library
 * of eight thousand tracks is a 1.2 MB file. */
#define WAVE_FILE    "/data/mnt/sd_0/.music_waveforms.dat"
#define WAVE_OLD_DIR "/data/mnt/sd_0/.music_waveforms"   /* R29's, cleared once */
#define WAVE_MAGIC   0x46564157u                          /* "WAVF" */
#define WAVE_VERSION 1u
#define WAVE_MAX     8000u

typedef struct {
    uint32_t magic, version, buckets, count;
} wave_header_t;

typedef struct {
    uint64_t key;
    uint8_t  bars[WAVEFORM_BUCKETS];
} wave_record_t;

/* Tallest column, out of the 255 the draw scales against. A column that
 * reaches the very top of its box reads as clipped audio, which is the one
 * thing the shape of a track must not look like. */
#define WAVE_CEILING 235
/* Fixed-point scale the curve is worked on. */
#define WAVE_SCALE   4096u

static void (*g_log)(const char *fmt, ...);
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;
static int  g_started;
static char g_wanted[512];      /* what the page wants */
static char g_next[512];        /* the queue's next track, worked out when idle */
static char g_pre_done[512];    /* the last prefetch settled, so it is not redone */
static char g_have_path[512];   /* what g_have answers for -- set once settled */
static int  g_have_ok;          /* 1: g_have is a shape; 0: this file has none */
static uint8_t g_have[WAVEFORM_BUCKETS];

/* Every key in the file, in file order, so a lookup is a scan of RAM and one
 * read of the record it lands on rather than a scan of the file on every
 * track change. Guarded by g_lock: the worker appends to it, and waveform_get()
 * reads it on the caller's thread for the reason described there. */
static uint64_t *g_keys;
static uint32_t  g_count;
static int       g_index_loaded;

#define WLOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* Path, size and modification time: a file replaced by different audio under
 * the same name is a different key, so it is worked out again rather than
 * shown with the old shape. 0 if the file is not there. */
static uint64_t path_key(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    uint64_t h = fnv(1469598103934665603ull, path, strlen(path));
    int64_t size = (int64_t)st.st_size, when = (int64_t)st.st_mtime;
    h = fnv(h, &size, sizeof(size));
    h = fnv(h, &when, sizeof(when));
    return h ? h : 1;
}

static int header_ok(const wave_header_t *h) {
    return h->magic == WAVE_MAGIC && h->version == WAVE_VERSION &&
           h->buckets == WAVEFORM_BUCKETS && h->count <= WAVE_MAX;
}

static void load_index(void) {
    if (g_index_loaded) return;
    g_index_loaded = 1;
    g_keys = malloc(WAVE_MAX * sizeof(*g_keys));
    g_count = 0;
    if (!g_keys) return;
    FILE *f = fopen(WAVE_FILE, "rb");
    if (!f) return;
    wave_header_t h;
    wave_record_t r;
    /* header.count, not end of file: a store interrupted between writing a
     * record and bumping the count leaves a record nobody should read. */
    if (fread(&h, sizeof(h), 1, f) == 1 && header_ok(&h)) {
        while (g_count < h.count && fread(&r, sizeof(r), 1, f) == 1)
            g_keys[g_count++] = r.key;
    }
    fclose(f);
}

static int cache_lookup(uint64_t key, uint8_t *out) {
    if (!g_keys) return 0;
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_keys[i] != key) continue;
        FILE *f = fopen(WAVE_FILE, "rb");
        if (!f) return 0;
        wave_record_t r;
        int ok = fseek(f, (long)(sizeof(wave_header_t) + i * sizeof(r)), SEEK_SET) == 0 &&
                 fread(&r, sizeof(r), 1, f) == 1 && r.key == key;
        fclose(f);
        if (ok) memcpy(out, r.bars, WAVEFORM_BUCKETS);
        return ok;
    }
    return 0;
}

/* Appends. Only reached straight after cache_lookup() missed this key, so it
 * is known not to be there already. A full file, or one from a build with a
 * different layout, is started again rather than compacted: losing it costs
 * some tracks being worked out once more, in the background, at idle -- not
 * worth a compactor. */
static void cache_store(uint64_t key, const uint8_t *bars) {
    if (!g_keys) return;
    wave_header_t h;
    FILE *f = fopen(WAVE_FILE, "r+b");
    if (f && (fread(&h, sizeof(h), 1, f) != 1 || !header_ok(&h) || h.count >= WAVE_MAX)) {
        fclose(f);
        f = NULL;
    }
    if (!f) {
        f = fopen(WAVE_FILE, "w+b");   /* truncates */
        if (!f) return;
        h.magic = WAVE_MAGIC; h.version = WAVE_VERSION;
        h.buckets = WAVEFORM_BUCKETS; h.count = 0;
        g_count = 0;
    }
    /* The file is the authority on where the next record goes; the index
     * follows it rather than the other way round. */
    if (h.count != g_count) g_count = h.count < WAVE_MAX ? h.count : 0;

    wave_record_t r;
    memset(&r, 0, sizeof(r));
    r.key = key;
    memcpy(r.bars, bars, WAVEFORM_BUCKETS);
    if (fseek(f, (long)(sizeof(h) + h.count * sizeof(r)), SEEK_SET) == 0 &&
        fwrite(&r, sizeof(r), 1, f) == 1) {
        h.count++;
        if (fseek(f, 0, SEEK_SET) == 0 && fwrite(&h, sizeof(h), 1, f) == 1)
            g_keys[g_count++] = key;
    }
    fclose(f);
}

/* R29 kept one small file per track in its own directory. Nothing reads them
 * any more; they are this app's own cache, so they go, once, from the worker
 * at idle priority. */
static void clear_old_cache(void) {
    DIR *d = opendir(WAVE_OLD_DIR);
    if (!d) return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", WAVE_OLD_DIR, e->d_name);
        if (unlink(p) == 0) n++;
    }
    closedir(d);
    rmdir(WAVE_OLD_DIR);
    WLOG("[wave] removed %d per-track files from the old cache\n", n);
}

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

/* RMS levels to drawable heights. Scaled against the loudest column rather
 * than full scale -- a quiet recording drawn against 0 dBFS is a flat line,
 * and the point is the shape of the track, not its level -- then lifted by a
 * curve halfway between linear and square root (roughly a 0.7 power). Linear
 * alone leaves a quiet verse at a tenth of the chorus and unreadable; the
 * square root alone flattens the dynamics that make it worth looking at. */
static void shape(const uint32_t *level, uint8_t *bars) {
    uint32_t loudest = 1;
    for (int i = 0; i < WAVEFORM_BUCKETS; i++)
        if (level[i] > loudest) loudest = level[i];
    for (int i = 0; i < WAVEFORM_BUCKETS; i++) {
        uint32_t norm = (uint32_t)((uint64_t)level[i] * WAVE_SCALE / loudest);
        uint32_t curved = (norm + isqrt32(norm * WAVE_SCALE)) / 2;
        if (curved > WAVE_SCALE) curved = WAVE_SCALE;
        bars[i] = (uint8_t)(curved * WAVE_CEILING / WAVE_SCALE);
    }
}

/* Asked between chunks. The track on screen is wanted for as long as it is
 * the track on screen; a prefetch only for as long as nothing more urgent has
 * come along -- a new track starting drops it mid-decode, which is the whole
 * reason prefetching is safe to do at all. */
static int still_wanted(void *ctx) {
    const char *path = (const char *)ctx;
    pthread_mutex_lock(&g_lock);
    int ok;
    if (strcmp(g_wanted, path) == 0) {
        ok = 1;
    } else {
        int cur_needs_work = g_wanted[0] && strcmp(g_wanted, g_have_path) != 0;
        ok = !cur_needs_work && strcmp(g_next, path) == 0;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static void be_idle(void) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) != 0) {
        /* Not available: at least the weakest nice level there is. */
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
        WLOG("[wave] SCHED_IDLE refused; running at nice 19\n");
    }
}

static void *worker(void *arg) {
    (void)arg;
    be_idle();
    clear_old_cache();
    /* The index and the cache file are shared with waveform_get(), which now
     * reads them on the caller's thread to answer a cached track at once --
     * so every touch of them, here too, is under g_lock. The file work inside
     * is a header and one 152-byte record, never a decode. */
    pthread_mutex_lock(&g_lock);
    load_index();
    uint32_t cached = g_count;
    pthread_mutex_unlock(&g_lock);
    WLOG("[wave] worker up, %u shapes cached\n", cached);

    for (;;) {
        char path[sizeof(g_wanted)];
        int prefetch;
        pthread_mutex_lock(&g_lock);
        for (;;) {
            /* The track on screen first, always. Only when it is settled --
             * or there is none -- is the queue's next track worth the core. */
            int cur = g_wanted[0] && strcmp(g_wanted, g_have_path) != 0;
            int nxt = !cur && g_next[0] && strcmp(g_next, g_pre_done) != 0 &&
                      strcmp(g_next, g_have_path) != 0;
            if (cur || nxt) { prefetch = !cur; break; }
            pthread_cond_wait(&g_wake, &g_lock);
        }
        snprintf(path, sizeof(path), "%s", prefetch ? g_next : g_wanted);
        pthread_mutex_unlock(&g_lock);

        uint8_t bars[WAVEFORM_BUCKETS];
        uint32_t level[WAVEFORM_BUCKETS];
        int outcome;   /* 1 shape, 0 none ever, -1 abandoned */
        uint64_t key = path_key(path);
        int hit = 0;
        if (key) {
            pthread_mutex_lock(&g_lock);
            hit = cache_lookup(key, bars);
            pthread_mutex_unlock(&g_lock);
        }
        if (key == 0) {
            outcome = 0;
        } else if (hit) {
            outcome = 1;
        } else {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            outcome = audio_envelope(path, level, WAVEFORM_BUCKETS, still_wanted, path);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
            const char *name = strrchr(path, '/');
            name = name ? name + 1 : path;
            if (outcome == 1) {
                shape(level, bars);
                pthread_mutex_lock(&g_lock);
                cache_store(key, bars);
                pthread_mutex_unlock(&g_lock);
                WLOG("[wave] worked out in %ld ms: %s\n", ms, name);
            } else if (outcome == 0) {
                WLOG("[wave] nothing to measure: %s\n", name);
            } else {
                WLOG("[wave] abandoned after %ld ms: %s\n", ms, name);
            }
        }

        pthread_mutex_lock(&g_lock);
        /* An abandoned job leaves no trace at all -- not even the path -- so
         * the track is picked up again if it comes back. A settled answer,
         * shape or none, is recorded so the wait above does not spin on it.
         *
         * A prefetch that finished while the track was still only "next"
         * answers nobody yet: cache_store() above has it, and the ordinary
         * lookup finds it the moment that track starts. All that is recorded
         * here is that it is done, so the loop does not start it again. */
        if (outcome >= 0) {
            if (strcmp(g_wanted, path) == 0) {
                snprintf(g_have_path, sizeof(g_have_path), "%s", path);
                g_have_ok = outcome == 1;
                if (g_have_ok) memcpy(g_have, bars, WAVEFORM_BUCKETS);
            } else if (prefetch) {
                snprintf(g_pre_done, sizeof(g_pre_done), "%s", path);
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void waveform_start(void (*log)(const char *fmt, ...)) {
    pthread_mutex_lock(&g_lock);
    int start = !g_started;
    g_started = 1;
    if (log) g_log = log;
    pthread_mutex_unlock(&g_lock);
    if (!start) return;
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) != 0) {
        WLOG("[wave] no worker thread; waveforms will not appear\n");
        pthread_mutex_lock(&g_lock);
        g_started = 0;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_detach(t);
}

void waveform_prefetch(const char *path) {
    if (!path) path = "";
    pthread_mutex_lock(&g_lock);
    if (strcmp(g_next, path) != 0) {
        snprintf(g_next, sizeof(g_next), "%s", path);
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_lock);
}

int waveform_get(const char *path, uint8_t *out) {
    if (!path) path = "";
    pthread_mutex_lock(&g_lock);
    int changed = strcmp(g_wanted, path) != 0;
    if (changed) {
        snprintf(g_wanted, sizeof(g_wanted), "%s", path);
        pthread_cond_signal(&g_wake);
    }

    /* A track whose shape is already cached has to answer on this call, not on
     * a later poll. Handing the question to the worker and waiting for it to
     * be scheduled -- possibly behind the last chunk of whatever it was
     * decoding -- put the plain progress bar on screen first and the waveform
     * in a moment later, which is a regression against R29: that read its
     * cache file on this thread and so had the shape ready for the first
     * frame of the new track.
     *
     * So the lookup happens here too, but only on the call that changes the
     * track: one stat and, at most, one 152-byte read. Every poll after it
     * costs a string compare, and a track with no cached shape falls through
     * to the worker exactly as before. */
    if (changed && path[0] && strcmp(g_have_path, path) != 0) {
        load_index();
        uint64_t key = path_key(path);
        uint8_t bars[WAVEFORM_BUCKETS];
        if (key && cache_lookup(key, bars)) {
            snprintf(g_have_path, sizeof(g_have_path), "%s", path);
            memcpy(g_have, bars, WAVEFORM_BUCKETS);
            g_have_ok = 1;
        }
    }

    int ready = path[0] && g_have_ok && strcmp(g_have_path, path) == 0;
    if (ready && out) memcpy(out, g_have, WAVEFORM_BUCKETS);
    pthread_mutex_unlock(&g_lock);
    return ready;
}
