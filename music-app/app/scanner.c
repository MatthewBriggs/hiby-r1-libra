/* scanner.c — RP6: a library index built by walking the SD card directly.
 *
 * library.c reads /usr/data/usrlocal_media.db, hiby_player's own stock
 * index -- fine while hiby_player is still around to build and maintain it,
 * but a dead end for the "replace hiby_player" plan (BACKLOG.md RP6): once
 * hiby_player is gone, nothing ever scans the card again and the library
 * stays frozen at whatever it last saw.
 *
 * This is that replacement scanner. It owns a database of its own --
 * SCANNER_DB_PATH, never the stock file -- shaped with the same MEDIA_TABLE
 * name and column set the stock one has, so every one of library.c's 15
 * existing raw-SQL query sites can run against it completely unchanged;
 * see lib_open()'s own comment for how the two databases are chosen
 * between. Rows carry the columns those queries actually read (name, path,
 * format, bit, sample_rate, bit_rate, end_time, artist, album,
 * album_artist, genre, ctime) plus one this database alone needs, `mtime`,
 * the change-detection key that makes a second pass over an unchanged file
 * a single indexed lookup rather than a re-read of its tags.
 *
 * Deliberately does NOT touch the stock database or run while it's live and
 * populated: while hiby_player still owns the card's index, this scanner
 * building its own in parallel would just be wasted SD I/O for a database
 * library.c won't prefer over the stock one anyway (see lib_open()). It is
 * still reachable today, same as index.c's "Rebuild library index" row --
 * useful for testing this file ahead of actually dropping hiby_player, and
 * harmless meanwhile: it only ever writes to SCANNER_DB_PATH, never to
 * anything hiby_player itself reads.
 *
 * Two traps this scanner does NOT reproduce, on purpose:
 *  - The stock database's values arrive NUL-terminated *inside the string*
 *    (see library.c's own header comment). That is a quirk of however
 *    hiby_player's own scanner encodes strings, not something SQLite or
 *    this app's queries require -- copy_text() strips it defensively but
 *    doesn't need it present. Rows written here carry plain, cleanly
 *    NUL-terminated strings, which copy_text() handles identically (its
 *    trim loop just finds nothing to trim).
 *  - Track number: the stock schema drops it entirely (real_path()'s own
 *    header comment: "no track number... only track_gain and track_peak").
 *    Ordering is recovered elsewhere, from the filename or the file's own
 *    tags (track_no_from_path(), tag_read()) -- this database doesn't need
 *    to carry one either.
 *  - end_time: populated by the stock scanner only for a cue-sheet split
 *    track sharing one physical file across several rows (see
 *    tracks_query()'s own comment) -- an ordinary track is left -1
 *    indefinitely even there. This scanner never splits a file across rows,
 *    so every row here carries -1, unconditionally.
 *
 * Threading and SQLite pragmas below mirror index.c's own scan_worker
 * exactly, including the SD-card exFAT caution in its header comment --
 * this file writes to UBIFS (/usr/data) for the identical reason index.c's
 * database does, not the card itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "vendor/sqlite3.h"
#include "scanner.h"
#include "library.h"
#include "tags.h"
#include "audio.h"
#include "index.h"

/* Same volume this app's own tracks live under -- see library.c's
 * real_path()/SD_ROOT comment. Duplicated rather than exported for the
 * same reason index.c duplicates PODCAST_EXCL/AUDIOBOOK_EXCL: a one-line
 * constant, and exporting it would mean library.c committing to a public
 * API around what is really an implementation detail of both files
 * independently. */
#define SD_ROOT "/data/mnt/sd_0/"

static void slog(const char *fmt, ...) {
    char b[200];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[240];
    int m = snprintf(line, sizeof(line), "[%6ld.%03ld] %s",
                     (long)ts.tv_sec, ts.tv_nsec / 1000000L, b);
    if (m <= 0) return;
    FILE *f = fopen("/usr/data/music.log", "a");
    if (!f) return;
    fwrite(line, 1, (size_t)m, f);
    fclose(f);
}

static const char *SCHEMA_SQL =
    "create table if not exists MEDIA_TABLE ("
    " path text primary key,"
    " mtime integer not null,"
    " name text, format integer, bit integer, sample_rate integer,"
    " bit_rate integer, end_time integer,"
    " artist text, album text, album_artist text, genre text,"
    " ctime integer"
    ")";

static volatile int g_scan_started, g_scan_running;
static volatile int g_scanned, g_written;
static volatile int g_kick;
static volatile int g_usb_paused;

int scanner_scan_running(void) { return g_scan_running; }

int scanner_scan_progress(int *scanned, int *written) {
    if (scanned) *scanned = g_scanned;
    if (written) *written = g_written;
    return g_scan_started;
}

static void set_low_priority(void) {
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10) != 0)
        slog("[scanner] could not lower scan priority\n");
}

/* Same six extensions podcast.c's is_audio_ext() covers, plus AIFF/AIFC --
 * library.c's track_format_name() already special-cases the .aif/.aiff
 * extension against format=1 (see its own comment, BG25), so those are
 * scanned as first-class tracks rather than skipped. Duplicated rather
 * than shared with podcast.c for the same reason index.c duplicates its
 * own exclusion macros: a handful of case labels, not worth a shared
 * header two unrelated scanners would both have to agree to change. */
static int is_audio_ext(const char *name) {
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    return !strcasecmp(d, ".mp3") || !strcasecmp(d, ".m4a") ||
          !strcasecmp(d, ".ogg") || !strcasecmp(d, ".opus") ||
          !strcasecmp(d, ".wav") || !strcasecmp(d, ".flac") ||
          !strcasecmp(d, ".aif") || !strcasecmp(d, ".aiff");
}

/* lib_format_name()'s own code table (library.c): 0/61868=FLAC, 85=MP3,
 * 278/41388=AAC, 1=WAV (and, by track_format_name()'s extension check,
 * AIFF). Anything else falls through to "?", which the UI already turns
 * into the plain file extension (track_format_name()'s own fallback) --
 * so Ogg/Opus getting no code here costs a display label, not a browsing
 * or playback failure. */
static int format_code(const char *path) {
    const char *d = strrchr(path, '.');
    if (!d) return 2;
    if (!strcasecmp(d, ".flac")) return 0;
    if (!strcasecmp(d, ".mp3")) return 85;
    if (!strcasecmp(d, ".m4a")) return 278;
    if (!strcasecmp(d, ".wav") || !strcasecmp(d, ".aif") || !strcasecmp(d, ".aiff")) return 1;
    return 2;
}

/* Inverse of library.c's real_path() -- identical to lib_track_by_path()'s
 * own inline version of this, duplicated rather than exported for the same
 * reason as SD_ROOT above: it's the natural counterpart to a constant this
 * file already keeps its own copy of. */
static void stored_path(char *dst, size_t n, const char *real) {
    const char *p = real;
    if (!strncmp(p, SD_ROOT, strlen(SD_ROOT))) p += strlen(SD_ROOT);
    snprintf(dst, n, "a:\\%s", p);
    for (char *q = dst; *q; q++) if (*q == '/') *q = '\\';
}

/* One file's worth of work: skip if the index already has it at this mtime,
 * else read its tags and container header once and upsert. Returns 1 if a
 * write happened, 0 if skipped -- same convention index.c's scan_one()
 * uses, for the same Settings-row progress readout. */
static int scan_one(sqlite3 *widb, const char *real, const struct stat *st) {
    char path[LIB_PATH_LEN];
    stored_path(path, sizeof(path), real);

    static const char *check_sql = "select mtime from MEDIA_TABLE where path = ?";
    sqlite3_stmt *cst;
    if (sqlite3_prepare_v2(widb, check_sql, -1, &cst, NULL) == SQLITE_OK) {
        sqlite3_bind_text(cst, 1, path, -1, SQLITE_TRANSIENT);
        int current = sqlite3_step(cst) == SQLITE_ROW &&
                     (time_t)sqlite3_column_int64(cst, 0) == st->st_mtime;
        sqlite3_finalize(cst);
        if (current) return 0;
    }

    char name[LIB_NAME_LEN], artist[LIB_NAME_LEN], album[LIB_NAME_LEN];
    char album_artist[LIB_NAME_LEN], genre[LIB_NAME_LEN];
    tag_read_meta(real, artist, sizeof(artist), album, sizeof(album),
                 album_artist, sizeof(album_artist), genre, sizeof(genre));
    /* R91: a loose file with no album tag at all (a stray radio-show rip
     * sitting at the card root, reported live as "won't show up in Music")
     * indexed with album="", which groups (correctly, by SQL's own rules)
     * with every other untagged file into an entry with no visible label --
     * sorts first alphabetically and is trivially easy to mistake for
     * nothing being there, which is exactly what happened. A named bucket
     * is reachable and self-explanatory the same way "no album" fallbacks
     * work in most other music apps. */
    if (!album[0]) snprintf(album, sizeof(album), "Unknown Album");
    /* Title tag when the container states one (currently FLAC only -- see
     * tag_title()'s own comment); the filename otherwise, same fallback
     * library.c itself reaches for reading a track the index never saw. */
    if (tag_title(real, name, sizeof(name)) != 0) {
        const char *base = strrchr(real, '/');
        base = base ? base + 1 : real;
        snprintf(name, sizeof(name), "%s", base);
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';
    }

    int bits = 0, rate = 0, bitrate = 0, dur_ms = 0;
    audio_probe_format(real, &bits, &rate, &bitrate, &dur_ms);

    static const char *up_sql =
        "insert or replace into MEDIA_TABLE "
        "(path, mtime, name, format, bit, sample_rate, bit_rate, end_time, "
        " artist, album, album_artist, genre, ctime) "
        "values (?, ?, ?, ?, ?, ?, ?, -1, ?, ?, ?, ?, ?)";
    sqlite3_stmt *ust;
    if (sqlite3_prepare_v2(widb, up_sql, -1, &ust, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(ust, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ust, 2, (sqlite3_int64)st->st_mtime);
    sqlite3_bind_text(ust, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ust, 4, format_code(real));
    sqlite3_bind_int(ust, 5, bits);
    sqlite3_bind_int(ust, 6, rate);
    sqlite3_bind_int(ust, 7, bitrate);
    sqlite3_bind_text(ust, 8, artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ust, 9, album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ust, 10, album_artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ust, 11, genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ust, 12, (sqlite3_int64)st->st_mtime);
    int rc = sqlite3_step(ust);
    sqlite3_finalize(ust);
    if (rc != SQLITE_DONE) {
        static int logged;
        if (!logged++) slog("[scanner] insert failed rc=%d: %s\n", rc, sqlite3_errmsg(widb));
        return 0;
    }
    return 1;
}

/* Recursive directory walk, depth-first, skipping the two folders that
 * already have their own proper subsystems -- Podcasts (podcast.c) and
 * Audiobooks (audiobook.h) -- matching PODCAST_EXCL/AUDIOBOOK_EXCL's own
 * exclusion so a file never ends up double-indexed under two different
 * models. Matched case-insensitively and at any depth, not just the SD
 * card's root, since the exclusion exists to avoid a category mismatch
 * (an episode or chapter showing up as a plain "track"), not to describe
 * one specific folder layout. */
static void scan_dir(sqlite3 *widb, const char *dir, int *batch) {
    if (g_usb_paused) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (g_usb_paused) break;
        if (e->d_name[0] == '.') continue;
        char full[LIB_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (!strcasecmp(e->d_name, "Podcasts") || !strcasecmp(e->d_name, "Audiobooks"))
                continue;
            scan_dir(widb, full, batch);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_audio_ext(e->d_name)) continue;

        g_written += scan_one(widb, full, &st);
        g_scanned++;

        /* Same batching/throttling rationale as index.c's own scan loop --
         * see its comment on set_low_priority(): the short sleep, not the
         * batch size, is what actually keeps this off the UI thread's way
         * on a single core doing real file I/O. */
        if (++*batch >= 30) {
            sqlite3_exec(widb, "commit", NULL, NULL, NULL);
            sqlite3_exec(widb, "begin", NULL, NULL, NULL);
            *batch = 0;
            usleep(50000);
        }
    }
    closedir(d);
}

static void *scan_worker(void *arg) {
    (void)arg;
    set_low_priority();

    sqlite3 *widb = NULL;
    int orc = sqlite3_open_v2(SCANNER_DB_PATH, &widb,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix-none");
    if (orc != SQLITE_OK) {
        slog("[scanner] cannot open %s rc=%d: %s\n", SCANNER_DB_PATH, orc,
             widb ? sqlite3_errmsg(widb) : sqlite3_errstr(orc));
        if (widb) sqlite3_close(widb);
        return NULL;
    }
    sqlite3_busy_timeout(widb, 500);
    sqlite3_exec(widb, "pragma journal_mode = off", NULL, NULL, NULL);
    sqlite3_exec(widb, "pragma synchronous = off", NULL, NULL, NULL);
    char *errmsg = NULL;
    if (sqlite3_exec(widb, SCHEMA_SQL, NULL, NULL, &errmsg) != SQLITE_OK) {
        slog("[scanner] schema create failed: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        sqlite3_close(widb);
        return NULL;
    }
    slog("[scanner] db opened, schema ready\n");

    for (;;) {
        g_scan_running = 1;
        g_scanned = 0; g_written = 0;
        int batch = 0;
        sqlite3_exec(widb, "begin", NULL, NULL, NULL);
        /* The whole card, not just a Music folder -- this device's own
         * default download/import paths don't enforce one, and scan_dir()'s
         * own Podcasts/Audiobooks exclusion already keeps those two
         * subsystems' own files out of this table regardless of where they
         * sit. */
        scan_dir(widb, SD_ROOT, &batch);
        sqlite3_exec(widb, "commit", NULL, NULL, NULL);
        g_scan_running = 0;
        slog("[scanner] scan pass done: %d files seen, %d (re)written\n", g_scanned, g_written);

        /* Manual only: waits here indefinitely for scanner_rescan_now() (the
         * Settings row), no periodic re-run -- see index.c's own identical
         * change for why. Auto-rescanning every 30 minutes made sense back
         * when this ran quietly alongside hiby_player's own live scanner;
         * now that this is the sole source of what files exist at all, a
         * manual "check now" (which the user actually controls, e.g. right
         * after copying new music onto the card) is both cheaper and more
         * predictable than a timer neither the UI nor the user can see
         * coming. */
        while (!g_kick) sleep(1);
        g_kick = 0;
    }
    return NULL;
}

void scanner_scan_start(void) {
    if (g_scan_started) return;
    g_scan_started = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, scan_worker, NULL) == 0) pthread_detach(t);
    else { slog("[scanner] could not start scan thread\n"); g_scan_started = 0; }
}

/* The one Settings button now behind both scans (music_hook.c dropped its
 * separate "Rebuild library index" row -- see that screen's own comment).
 * Kicked together because they answer different halves of the same
 * question a manual "check now" tap is actually asking: this discovers
 * what files exist at all, index_rescan_now() fills in per-track detail
 * (track/disc/duration) for what this pass just found. Running index's
 * pass unconditionally, even when this one finds nothing new, is
 * deliberate and cheap -- see index.c's own scan_one(), an unchanged file
 * is one indexed lookup, not a re-read. */
void scanner_rescan_now(void) {
    if (g_usb_paused) return;
    scanner_scan_start();
    g_kick = 1;
    index_rescan_now();
}

void scanner_pause_for_usb(int paused) {
    g_usb_paused = paused != 0;
}
