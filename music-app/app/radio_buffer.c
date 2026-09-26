/* radio_buffer.c -- see radio_buffer.h for the design.
 *
 * Chunk files are named by (sequence number mod RB_MAX_CHUNKS), so the
 * retention window falls out of the naming scheme itself: writing chunk
 * seq N+RB_MAX_CHUNKS overwrites (O_TRUNC) the same file that held chunk N,
 * which is exactly the eviction policy wanted -- no separate delete pass,
 * no manifest compaction, just "the oldest chunk still fitting in the ring
 * of filenames".
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "radio_buffer.h"
#include "hls.h"
#include "ts.h"

#define RB_DIR            "/data/mnt/sd_0/.radio_buffer"
#define RB_CHUNK_SECONDS  20
#define RB_WINDOW_SECONDS (30 * 60)
#define RB_MAX_CHUNKS     ((RB_WINDOW_SECONDS / RB_CHUNK_SECONDS) + 2)  /* +2 slack */

#define RB_CURL_PATH "/data/mnt/sd_0/.podsync/curl"
#define RB_CA_BUNDLE "/data/mnt/sd_0/.podsync/cacert.pem"

typedef struct {
    long long seq;       /* -1 = slot never written this session */
    uint64_t  start_pos;
    size_t    len;
} rb_chunk_meta_t;

static pthread_t       g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_running;   /* background thread told to keep going */
static volatile int    g_active;    /* background thread actually alive */
static rb_kind_t       g_kind;
static char            g_url[1024];

static rb_chunk_meta_t g_chunks[RB_MAX_CHUNKS];
static uint64_t        g_live_pos;      /* total bytes ever written */
static long long       g_cur_seq = -1;  /* chunk currently being written */

/* Recording: a second, permanent sink for the exact same bytes rb_write_
 * bytes() is already writing into chunk files, guarded by the same g_lock
 * everything else here uses. -1 means not recording. */
static int g_rec_fd = -1;

/* The curl fetching an MP3 stream, so rb_stop() can end it: a stalled stream
 * leaves the worker blocked in read() with nothing to wake it, and joining it
 * then hung whoever asked for the stop -- the UI, on a station change. */
static pid_t g_curl_pid = -1;

/* Bytes/sec estimate: exponential moving average over ~4s windows of real
 * arrival, not a nominal bitrate the source is not obliged to honour. */
static double   g_bps;
static uint64_t g_bps_window_bytes;

static void chunk_path(long long seq, char *out, size_t n) {
    snprintf(out, n, "%s/%lld.chunk", RB_DIR, seq % RB_MAX_CHUNKS);
}

static double now_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void rb_note_bytes(size_t n) {
    static double window_start = -1;
    double t = now_mono();
    if (window_start < 0) { window_start = t; g_bps_window_bytes = 0; }
    g_bps_window_bytes += n;
    double elapsed = t - window_start;
    if (elapsed >= 4.0) {
        double measured = (double)g_bps_window_bytes / elapsed;
        g_bps = (g_bps <= 0) ? measured : (g_bps * 0.5 + measured * 0.5);
        window_start = t;
        g_bps_window_bytes = 0;
    }
}

/* Start (or roll to) the chunk for the given global write position. Slot
 * (seq mod RB_MAX_CHUNKS) is reused unconditionally -- whatever chunk lived
 * there before is exactly RB_MAX_CHUNKS chunks old, which is the retention
 * limit, so overwriting it is the eviction. */
static int rb_roll_chunk(long long seq) {
    char path[300];
    chunk_path(seq, path, sizeof(path));
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;
    pthread_mutex_lock(&g_lock);
    g_chunks[seq % RB_MAX_CHUNKS].seq = seq;
    g_chunks[seq % RB_MAX_CHUNKS].start_pos = g_live_pos;
    g_chunks[seq % RB_MAX_CHUNKS].len = 0;
    g_cur_seq = seq;
    pthread_mutex_unlock(&g_lock);
    return fd;
}

static void rb_write_bytes(int *fd, double *chunk_started, const unsigned char *data, size_t n) {
    if (n == 0) return;
    ssize_t w = write(*fd, data, n);
    if (w < 0) w = 0;
    pthread_mutex_lock(&g_lock);
    g_chunks[g_cur_seq % RB_MAX_CHUNKS].len += (size_t)w;
    g_live_pos += (uint64_t)w;
    /* A duplicate, not the descriptor itself: rb_recording_stop() can close
     * g_rec_fd the moment the lock drops, and that number can then be reused
     * by whatever the app opens next -- the write below would land in it. */
    int rec_fd = (g_rec_fd >= 0 && w > 0) ? dup(g_rec_fd) : -1;
    pthread_mutex_unlock(&g_lock);
    rb_note_bytes((size_t)w);

    /* Same bytes, verbatim, into the recording if one is active -- see
     * rb_recording_start()'s own comment on why this is a second sink
     * rather than reusing a chunk file after the fact. Written outside the
     * lock (already released above) since this is real file I/O and
     * nothing else here needs to wait on it. */
    if (rec_fd >= 0) {
        if (write(rec_fd, data, (size_t)w) < 0) { /* the recording loses this chunk */ }
        close(rec_fd);
    }

    if (now_mono() - *chunk_started >= RB_CHUNK_SECONDS) {
        close(*fd);
        *fd = rb_roll_chunk(g_cur_seq + 1);
        *chunk_started = now_mono();
    }
}

/* A live stream's connect can fail on a transient hiccup (early-boot
 * network not fully up yet, a DNS blip, a momentary redirect failure) that
 * has nothing to do with the URL being bad. Reported live: a fresh boot hit
 * exactly this ~15s into boot and the station just sat on "stopped" with an
 * empty buffer directory -- neither worker retried its own initial connect
 * at all, unlike a real radio player. Three tries, 1s apart, covers a
 * genuine hiccup without holding a bad URL's failure report open for long. */
#define RB_CONNECT_RETRIES 3

/* curl straight into a pipe, no shell: the URL comes from a station list and
 * used to sit inside single quotes in a popen() command line. */
static FILE *rb_curl_open(pid_t *pid_out) {
    int fds[2];
    if (pipe(fds) != 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        dup2(fds[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) dup2(nul, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execl(RB_CURL_PATH, RB_CURL_PATH, "-sL", "--no-buffer", "--cacert", RB_CA_BUNDLE,
              g_url, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    FILE *p = fdopen(fds[0], "r");
    if (!p) { close(fds[0]); kill(pid, SIGKILL); waitpid(pid, NULL, 0); return NULL; }
    *pid_out = pid;
    return p;
}
static void rb_curl_close(FILE *p, pid_t pid) {
    fclose(p);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

static void *rb_worker_mp3(void *arg) {
    (void)arg;
    FILE *p = NULL;
    pid_t pid = -1;
    unsigned char probe[1];
    size_t probe_got = 0;
    for (int attempt = 0; g_running && attempt < RB_CONNECT_RETRIES; attempt++) {
        p = rb_curl_open(&pid);
        if (p) {
            pthread_mutex_lock(&g_lock);
            g_curl_pid = pid;
            pthread_mutex_unlock(&g_lock);
            probe_got = fread(probe, 1, 1, p);
            if (probe_got > 0) break;
            pthread_mutex_lock(&g_lock);
            g_curl_pid = -1;
            pthread_mutex_unlock(&g_lock);
            rb_curl_close(p, pid);
            p = NULL;
        }
        if (attempt + 1 < RB_CONNECT_RETRIES && g_running) sleep(1);
    }
    if (!p) { g_active = 0; return NULL; }

    int fd = rb_roll_chunk(0);
    double chunk_started = now_mono();
    if (fd < 0) {
        pthread_mutex_lock(&g_lock); g_curl_pid = -1; pthread_mutex_unlock(&g_lock);
        rb_curl_close(p, pid);
        g_active = 0;
        return NULL;
    }
    if (probe_got > 0) rb_write_bytes(&fd, &chunk_started, probe, probe_got);

    unsigned char buf[8192];
    while (g_running) {
        size_t got = fread(buf, 1, sizeof(buf), p);
        if (got == 0) {
            if (feof(p) || ferror(p)) break;
            continue;
        }
        rb_write_bytes(&fd, &chunk_started, buf, got);
    }
    close(fd);
    pthread_mutex_lock(&g_lock);
    g_curl_pid = -1;
    pthread_mutex_unlock(&g_lock);
    rb_curl_close(p, pid);
    g_active = 0;
    return NULL;
}

static void *rb_worker_hls(void *arg) {
    (void)arg;
    hls_t hls;
    int opened = 0;
    for (int attempt = 0; g_running && attempt < RB_CONNECT_RETRIES; attempt++) {
        if (hls_open(&hls, g_url) == 0) { opened = 1; break; }
        if (attempt + 1 < RB_CONNECT_RETRIES) sleep(1);
    }
    if (!opened) { g_active = 0; return NULL; }

    int fd = rb_roll_chunk(0);
    double chunk_started = now_mono();
    if (fd < 0) { g_active = 0; return NULL; }

    static unsigned char seg[512 * 1024];
    static unsigned char adts[192 * 1024];
    int stalls = 0;
    while (g_running) {
        int n = hls_next(&hls, seg, (int)sizeof(seg));
        if (n <= 0) {
            if (++stalls > 30) break;   /* ~ a minute of nothing new at 4s segments */
            usleep(500 * 1000);
            continue;
        }
        stalls = 0;
        int a = ts_extract_audio(seg, n, adts, (int)sizeof(adts));
        if (a > 0) rb_write_bytes(&fd, &chunk_started, adts, (size_t)a);
    }
    close(fd);
    g_active = 0;
    return NULL;
}

int rb_start(const char *url, rb_kind_t kind) {
    rb_stop();

    mkdir(RB_DIR, 0755);
    /* Wipe the previous session's chunk files -- a different station starts
     * from a clean, empty window, not a tail of whatever the last one left
     * behind under the same (seq mod RB_MAX_CHUNKS) filenames. */
    for (int i = 0; i < RB_MAX_CHUNKS; i++) {
        char path[300];
        snprintf(path, sizeof(path), "%s/%d.chunk", RB_DIR, i);
        unlink(path);
        g_chunks[i].seq = -1;
        g_chunks[i].len = 0;
    }
    g_live_pos = 0;
    g_cur_seq = -1;
    g_bps = 0;
    g_bps_window_bytes = 0;

    snprintf(g_url, sizeof(g_url), "%s", url);
    g_kind = kind;
    g_running = 1;
    g_active = 1;
    void *(*fn)(void *) = (kind == RB_KIND_ADTS) ? rb_worker_hls : rb_worker_mp3;
    if (pthread_create(&g_thread, NULL, fn, NULL) != 0) {
        g_running = 0;
        g_active = 0;
        return -1;
    }
    return 0;
}

void rb_stop(void) {
    rb_recording_stop();   /* switching stations mid-recording should not leak the fd */
    if (!g_running && !g_active) return;
    g_running = 0;
    /* Unblock a worker stuck reading a stalled stream: ending curl closes its
     * end of the pipe, and the read returns. */
    pthread_mutex_lock(&g_lock);
    if (g_curl_pid > 0) kill(g_curl_pid, SIGTERM);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
    g_active = 0;
}

uint64_t rb_live_pos(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_live_pos;
    pthread_mutex_unlock(&g_lock);
    return v;
}

uint64_t rb_oldest_pos(void) {
    pthread_mutex_lock(&g_lock);
    uint64_t oldest = g_live_pos;
    for (int i = 0; i < RB_MAX_CHUNKS; i++) {
        if (g_chunks[i].seq < 0) continue;
        if (g_chunks[i].start_pos < oldest) oldest = g_chunks[i].start_pos;
    }
    pthread_mutex_unlock(&g_lock);
    return oldest;
}

size_t rb_read_at(uint64_t pos, unsigned char *out, size_t len) {
    if (pos >= rb_live_pos()) return 0;
    if (pos < rb_oldest_pos()) return 0;

    pthread_mutex_lock(&g_lock);
    rb_chunk_meta_t chunks[RB_MAX_CHUNKS];
    memcpy(chunks, g_chunks, sizeof(chunks));
    pthread_mutex_unlock(&g_lock);

    /* Find the chunk containing pos. */
    for (int i = 0; i < RB_MAX_CHUNKS; i++) {
        if (chunks[i].seq < 0) continue;
        uint64_t lo = chunks[i].start_pos, hi = lo + chunks[i].len;
        if (pos < lo || pos >= hi) continue;
        char path[300];
        chunk_path(chunks[i].seq, path, sizeof(path));
        int fd = open(path, O_RDONLY);
        if (fd < 0) return 0;
        lseek(fd, (off_t)(pos - lo), SEEK_SET);
        size_t want = len;
        if (want > hi - pos) want = (size_t)(hi - pos);
        ssize_t got = read(fd, out, want);
        close(fd);
        if (got <= 0) return 0;
        size_t total = (size_t)got;
        /* A request spanning into the next chunk: recurse once. Chunks are
         * contiguous in position by construction (each starts exactly where
         * the previous one's live_pos was), so pos+total is the very next
         * byte position with no gap to bridge. */
        if (total < len && pos + total < rb_live_pos())
            total += rb_read_at(pos + total, out + total, len - total);
        return total;
    }
    return 0;
}

double rb_bytes_per_sec(void) {
    pthread_mutex_lock(&g_lock);
    double v = g_bps;
    pthread_mutex_unlock(&g_lock);
    return v;
}

int rb_active(void) { return g_active; }
rb_kind_t rb_current_kind(void) { return g_kind; }

int rb_recording_start(const char *path) {
    if (!g_active) return -1;
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;
    pthread_mutex_lock(&g_lock);
    if (g_rec_fd >= 0) close(g_rec_fd);   /* starting a new one abandons any previous, unclosed one */
    g_rec_fd = fd;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void rb_recording_stop(void) {
    pthread_mutex_lock(&g_lock);
    int fd = g_rec_fd;
    g_rec_fd = -1;
    pthread_mutex_unlock(&g_lock);
    if (fd >= 0) close(fd);
}

int rb_is_recording(void) {
    pthread_mutex_lock(&g_lock);
    int v = g_rec_fd >= 0;
    pthread_mutex_unlock(&g_lock);
    return v;
}
