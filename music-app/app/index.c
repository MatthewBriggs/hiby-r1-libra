/* index.c — a track-detail cache this app owns.
 *
 * Every album open used to re-derive track number, disc number and duration
 * live: tag_track_number()/tag_disc_number() each opened the file
 * independently (fixed since, see tags.c), and duration is unpopulated for
 * ~99.7% of the stock index (R28) so audio_probe_dur_ms() opened it again.
 * Nothing was ever cached, so this cost was paid on *every* open, forever,
 * scaling with track count -- a 183-track box set meant hundreds of
 * synchronous file opens just to draw its tracklist.
 *
 * This owns a small SQLite database of its own (never the stock read-only
 * one library.c reads) and a background thread that walks the library once,
 * reading each file's tags exactly once, so a later album open is a plain
 * SQL lookup keyed by path. A row is valid only while its stored mtime
 * matches the file's current mtime -- a retag or replace just falls back to
 * a live read (unchanged from today) until the next scan pass catches it.
 *
 * Threading: build.sh compiles SQLite with SQLITE_THREADSAFE=2 (multi-thread
 * mode) specifically for this file -- safe for different connections to be
 * used concurrently from different threads, unsafe only if a single
 * connection were shared across threads, which nothing here does. The
 * scanner thread opens its own connections (one read-only to the stock DB,
 * one read-write to this file); index_lookup() below opens and reuses its
 * own separate read-only connection, called only from the UI thread, same
 * as every other database access in this app.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "vendor/sqlite3.h"
#include "index.h"
#include "library.h"
#include "tags.h"
#include "audio.h"
#include "scanner.h"

#define STOCK_DB_PATH "/usr/data/usrlocal_media.db"
/* Internal UBIFS storage, not the SD card: /usr/data/usrlocal_media.db (the
 * stock scanner's own index) and music.log already live here reliably all
 * session. The SD card's exFAT driver, by contrast, does not -- confirmed
 * live: SQLITE_READONLY, then SQLITE_IOERR, then a main-thread stall in
 * __lock_page_killable under write load, and the file's own directory
 * entry not surviving a clean reboot despite the data having been read
 * back correctly within the same boot (a delayed-metadata quirk, not a
 * logic bug here). UBIFS is real flash-native filesystem with proper
 * locking and journaling, the reason a database lives here at all rather
 * than a flat file like this app's other SD-card state. Only ~1.3 MB for
 * the full library, comfortably inside the ~22 MB this partition has free. */
#define INDEX_DB_PATH "/usr/data/music_index.db"

/* Same exclusions library.c's tracks_query() applies -- Podcasts/Audiobooks
 * are indexed by the stock scanner too, but have no business in a music
 * track cache. Duplicated rather than shared: one-line macros, and sharing
 * them would mean exporting library.c's printf-vs-literal distinction for
 * no real benefit here (this file only ever pastes them as plain SQL). */
#define PODCAST_EXCL   "path not like 'a:\\Podcasts\\%'"
#define AUDIOBOOK_EXCL "path not like 'a:\\Audiobooks\\%'"
/* Same text, %% instead of % -- for pasting into a string that itself goes
 * through snprintf's own format parsing (library.c's tracks_query() draws
 * exactly this _SQL/_FMT distinction, for the same reason: fed to snprintf
 * as a literal, the bare % above is read as a conversion specifier, not a
 * SQL wildcard, and either eats the next argument or -- with none left, as
 * here -- is simply undefined). */
#define PODCAST_EXCL_FMT   "path not like 'a:\\Podcasts\\%%'"

static void ilog(const char *fmt, ...) {
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
    "create table if not exists track_index ("
    " path text primary key,"
    " mtime integer not null,"
    " track integer,"
    " disc integer,"
    " title text,"
    " dur_ms integer"
    ");"
    /* `seq` is insertion order (0..N-1, already sorted by album name --
     * see the populate query below), so a page read is a plain seq range,
     * never a re-sort of this table itself. */
    "create table if not exists album_cache ("
    " seq integer primary key,"
    " name text not null,"
    " count integer,"
    " owner text"
    ");"
    /* The other three top-level Music lists -- Album artists, Artists,
     * Genres -- keyed by which column they group (composite primary key,
     * same seq-is-sort-order trick as album_cache above, per column). */
    "create table if not exists group_cache ("
    " column text not null,"
    " seq integer not null,"
    " name text not null,"
    " count integer,"
    " primary key (column, seq)"
    ")";

static int g_scan_started;
/* Live status for the Settings row -- plain ints, read from the UI thread
 * with no lock, same convention audio.c's g_out_kind already uses for a
 * cross-thread flag nothing here needs stronger than eventual consistency
 * for (a progress counter one tick stale is invisible at 30fps). */
static volatile int g_scan_running;
static volatile int g_scanned, g_written;
static volatile int g_kick;          /* set by index_rescan_now(), cleared once seen */
static volatile int g_usb_paused;

/* ---- lookup, UI thread only ----------------------------------------- */

static sqlite3 *g_read_db;

static int ensure_read_db(void) {
    if (g_read_db) return 0;
    /* Retried every call, not cached as a permanent failure: the scanner
     * creates this file itself moments after process load, so a lookup
     * that lands before the file exists yet must not lock itself out of
     * ever trying again for the rest of the process's life. Opening a
     * missing file read-only is a cheap failed syscall, not worth avoiding
     * at the cost of correctness. */
    /* "unix-none": see the write connection's own comment on why locking is
     * disabled for this file. */
    if (sqlite3_open_v2(INDEX_DB_PATH, &g_read_db,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, "unix-none") != SQLITE_OK) {
        if (g_read_db) { sqlite3_close(g_read_db); g_read_db = NULL; }
        return -1;
    }
    sqlite3_busy_timeout(g_read_db, 200);
    return 0;
}

int index_lookup(const char *path, time_t mtime, int *track, int *disc, int *dur_ms) {
    if (ensure_read_db() != 0) return 0;
    static const char *sql =
        "select mtime, track, disc, dur_ms from track_index where path = ?";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_read_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    int hit = 0;
    if (sqlite3_step(st) == SQLITE_ROW &&
        (time_t)sqlite3_column_int64(st, 0) == mtime) {
        if (track)  *track  = sqlite3_column_int(st, 1);
        if (disc)   *disc   = sqlite3_column_int(st, 2);
        if (dur_ms) *dur_ms = sqlite3_column_int(st, 3);
        hit = 1;
    }
    sqlite3_finalize(st);
    return hit;
}

int index_albums_count(void) {
    if (ensure_read_db() != 0) return 0;
    static const char *sql = "select count(*) from album_cache";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_read_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

int index_group_count(const char *column) {
    if (ensure_read_db() != 0) return 0;
    static const char *sql = "select count(*) from group_cache where column = ?";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_read_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, column, -1, SQLITE_TRANSIENT);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

int index_group(const char *column, lib_row_t *out, int max, int offset) {
    if (ensure_read_db() != 0) return 0;
    static const char *sql =
        "select name, count from group_cache where column = ? "
        "order by seq limit ? offset ?";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_read_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, column, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, max);
    sqlite3_bind_int(st, 3, offset);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 0);
        snprintf(out[n].name, sizeof(out[n].name), "%s", name ? (const char *)name : "");
        out[n].count = sqlite3_column_int(st, 1);
        out[n].owner[0] = '\0';
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int index_albums(lib_row_t *out, int max, int offset) {
    if (ensure_read_db() != 0) return 0;
    static const char *sql =
        "select name, count, owner from album_cache order by seq limit ? offset ?";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_read_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max);
    sqlite3_bind_int(st, 2, offset);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 0);
        const unsigned char *owner = sqlite3_column_text(st, 2);
        snprintf(out[n].name, sizeof(out[n].name), "%s", name ? (const char *)name : "");
        out[n].count = sqlite3_column_int(st, 1);
        snprintf(out[n].owner, sizeof(out[n].owner), "%s", owner ? (const char *)owner : "");
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

/* ---- scan, background thread only ------------------------------------ */

static void set_low_priority(void) {
    /* Opposite end of audio.c's apply_decode_priority(): this has no
     * deadline at all, so it should lose every scheduling tie rather than
     * win one, on a single-core device where a decode thread already runs
     * as low as nice -8. */
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10) != 0)
        ilog("[index] could not lower scan priority\n");
}

/* One row's worth of work: skip if already current, else read tags and
 * duration once and upsert. Returns 1 if a write happened, 0 if skipped. */
static int scan_one(sqlite3 *widb, const char *real, int bitrate_bps) {
    struct stat st;
    if (stat(real, &st) != 0) return 0;

    static const char *check_sql = "select mtime from track_index where path = ?";
    sqlite3_stmt *cst;
    if (sqlite3_prepare_v2(widb, check_sql, -1, &cst, NULL) == SQLITE_OK) {
        sqlite3_bind_text(cst, 1, real, -1, SQLITE_TRANSIENT);
        int current = sqlite3_step(cst) == SQLITE_ROW &&
                     (time_t)sqlite3_column_int64(cst, 0) == st.st_mtime;
        sqlite3_finalize(cst);
        if (current) return 0;
    }

    int track, disc;
    char title[LIB_NAME_LEN];
    tag_read(real, &track, &disc, title, sizeof(title));
    int dur_ms = audio_probe_dur_ms(real, bitrate_bps);

    static const char *up_sql =
        "insert or replace into track_index (path, mtime, track, disc, title, dur_ms) "
        "values (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *ust;
    if (sqlite3_prepare_v2(widb, up_sql, -1, &ust, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(ust, 1, real, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ust, 2, (sqlite3_int64)st.st_mtime);
    sqlite3_bind_int(ust, 3, track);
    sqlite3_bind_int(ust, 4, disc);
    sqlite3_bind_text(ust, 5, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ust, 6, dur_ms);
    int rc = sqlite3_step(ust);
    sqlite3_finalize(ust);
    if (rc != SQLITE_DONE) {
        static int logged;
        if (!logged++) ilog("[index] insert failed rc=%d: %s\n", rc, sqlite3_errmsg(widb));
        return 0;
    }
    return 1;
}

/* Same grouping lib_albums(NULL, NULL, ...) itself uses for the whole
 * unfiltered library -- PODCAST_EXCL only, no AUDIOBOOK_EXCL, since that's
 * exactly what the live query filters (library.c's own filter_clause()
 * always emits just the podcast exclusion when unfiltered; audiobooks get
 * their own screen, not a Music-menu facet, so they were never excluded
 * from this specific query to begin with). Read once per pass and replace
 * the cache wholesale -- 286 albums is nothing to rewrite outright, and
 * doing so avoids reconciling stale entries for albums that were removed. */
static void populate_album_cache(sqlite3 *rodb, sqlite3 *widb) {
    sqlite3_exec(widb, "delete from album_cache", NULL, NULL, NULL);
    static const char *sql =
        "select album, count(*), max(album_artist) from MEDIA_TABLE where "
        PODCAST_EXCL " group by album order by album";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(rodb, sql, -1, &st, NULL) != SQLITE_OK) return;
    static const char *ins_sql =
        "insert into album_cache (seq, name, count, owner) values (?, ?, ?, ?)";
    sqlite3_stmt *ist;
    if (sqlite3_prepare_v2(widb, ins_sql, -1, &ist, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return;
    }
    sqlite3_exec(widb, "begin", NULL, NULL, NULL);
    int seq = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        /* Either text column can be a genuine SQL NULL (a blank album name,
         * or every track lacking album_artist) -- sqlite3_column_text()
         * then returns NULL, and binding that as a C string would be
         * undefined. Empty string in its place, same as copy_text()
         * elsewhere in this codebase treats a NULL column. */
        const unsigned char *name = sqlite3_column_text(st, 0);
        const unsigned char *owner = sqlite3_column_text(st, 2);
        sqlite3_bind_int(ist, 1, seq++);
        sqlite3_bind_text(ist, 2, name ? (const char *)name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ist, 3, sqlite3_column_int(st, 1));
        sqlite3_bind_text(ist, 4, owner ? (const char *)owner : "", -1, SQLITE_TRANSIENT);
        sqlite3_step(ist);
        sqlite3_reset(ist);
    }
    sqlite3_exec(widb, "commit", NULL, NULL, NULL);
    sqlite3_finalize(ist);
    sqlite3_finalize(st);
    ilog("[index] album cache: %d albums\n", seq);
}

/* Same idea for the other three top-level Music lists -- Album artists,
 * Artists, Genres -- which each pay their own uncached GROUP BY/ORDER BY
 * over MEDIA_TABLE on every visit today, same cost lib_albums() itself
 * had before album_cache. count(distinct album), not count(*), matching
 * lib_group()'s own live query exactly: these lists count albums per
 * artist/genre, not tracks. */
static void populate_group_cache(sqlite3 *rodb, sqlite3 *widb) {
    static const char *columns[] = { "album_artist", "artist", "genre" };
    sqlite3_exec(widb, "delete from group_cache", NULL, NULL, NULL);
    static const char *ins_sql =
        "insert into group_cache (column, seq, name, count) values (?, ?, ?, ?)";
    sqlite3_stmt *ist;
    if (sqlite3_prepare_v2(widb, ins_sql, -1, &ist, NULL) != SQLITE_OK) return;
    sqlite3_exec(widb, "begin", NULL, NULL, NULL);
    for (unsigned c = 0; c < sizeof(columns) / sizeof(columns[0]); c++) {
        char sql[192];
        snprintf(sql, sizeof(sql),
                 "select %s, count(distinct album) from MEDIA_TABLE where "
                 PODCAST_EXCL_FMT " group by %s order by %s",
                 columns[c], columns[c], columns[c]);
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(rodb, sql, -1, &st, NULL) != SQLITE_OK) continue;
        int seq = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 0);
            sqlite3_bind_text(ist, 1, columns[c], -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ist, 2, seq++);
            sqlite3_bind_text(ist, 3, name ? (const char *)name : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ist, 4, sqlite3_column_int(st, 1));
            sqlite3_step(ist);
            sqlite3_reset(ist);
        }
        sqlite3_finalize(st);
        ilog("[index] group cache: %d %s\n", seq, columns[c]);
    }
    sqlite3_exec(widb, "commit", NULL, NULL, NULL);
    sqlite3_finalize(ist);
}

static void *scan_worker(void *arg) {
    (void)arg;
    set_low_priority();

    /* "unix-none": both this and journal_mode=off below were worked out
     * against the SD card, whose exFAT driver doesn't honour POSIX advisory
     * locks properly (confirmed live: SQLITE_READONLY, then SQLITE_IOERR).
     * Kept now that this lives on UBIFS instead -- a real flash filesystem
     * that shouldn't need either workaround -- because they're still
     * correct and harmless for a single-writer, single-process, disposable
     * cache: no reason to pay for locking or journal overhead this file
     * never needs, on any filesystem. */
    sqlite3 *widb = NULL;
    int orc = sqlite3_open_v2(INDEX_DB_PATH, &widb,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix-none");
    if (orc != SQLITE_OK) {
        ilog("[index] cannot open %s rc=%d: %s\n", INDEX_DB_PATH, orc,
             widb ? sqlite3_errmsg(widb) : sqlite3_errstr(orc));
        if (widb) sqlite3_close(widb);
        return NULL;
    }
    sqlite3_busy_timeout(widb, 500);
    /* journal_mode=OFF: the rollback journal is a second sidecar file
     * SQLite creates, writes and deletes/renames around every transaction
     * -- still failed (SQLITE_IOERR on the first insert) even with locking
     * disabled above, pointing at that file dance itself, not just
     * locking, as something this exFAT driver mishandles. No durability
     * loss that matters here: this file is a rebuildable cache with its
     * own self-healing mtime check (see the header comment) -- a torn
     * write in a power-loss scenario just means that one row gets rescanned
     * next pass, not corruption anything downstream trusts blindly. */
    sqlite3_exec(widb, "pragma journal_mode = off", NULL, NULL, NULL);
    sqlite3_exec(widb, "pragma synchronous = off", NULL, NULL, NULL);
    char *errmsg = NULL;
    if (sqlite3_exec(widb, SCHEMA_SQL, NULL, NULL, &errmsg) != SQLITE_OK) {
        ilog("[index] schema create failed: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        sqlite3_close(widb);
        return NULL;
    }
    ilog("[index] db opened, schema ready\n");

    ilog("[index] scan thread started\n");

    for (;;) {
        /* RBR/BG95: this used to open STOCK_DB_PATH unconditionally, same
         * as library.c's own lib_open() did before RP6 -- and like that
         * function, it needed the identical fix. hiby_player's own scanner
         * is gone for good once RP1 is committed to, so STOCK_DB_PATH is
         * frozen: a file scanner.c discovers that the frozen stock table
         * never knew about (a box set added to the card after hiby_player
         * stopped scanning, in the case that surfaced this) was invisible
         * to this scan pass forever, no matter how many times "Scan
         * library" ran -- every browse of it kept paying the full,
         * uncached tag_read()+audio_probe_dur_ms() cost, live, since this
         * cache never had a row for it to serve instead. Same preference
         * order as lib_open(): scanner.c's own database first (the one
         * actually being kept current -- see its own header comment on
         * the manual rescan), stock only as a fallback for a device that
         * hasn't run a scan yet. */
        sqlite3 *rodb = NULL;
        int rc = sqlite3_open_v2(SCANNER_DB_PATH, &rodb,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_stmt *cst;
            int has_rows = 0;
            if (sqlite3_prepare_v2(rodb, "select 1 from MEDIA_TABLE limit 1", -1, &cst, NULL) == SQLITE_OK) {
                has_rows = sqlite3_step(cst) == SQLITE_ROW;
                sqlite3_finalize(cst);
            }
            if (!has_rows) { sqlite3_close(rodb); rodb = NULL; }
        } else if (rodb) {
            sqlite3_close(rodb);   /* open_v2 can return a non-NULL handle even on failure */
            rodb = NULL;
        }
        if (!rodb && sqlite3_open_v2(STOCK_DB_PATH, &rodb,
                                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
            if (rodb) sqlite3_close(rodb);
            ilog("[index] neither scanner nor stock db ready, retrying in 30s\n");
            sleep(30);
            continue;
        }

        g_scan_running = 1;
        g_scanned = 0; g_written = 0;
        static const char *sql =
            "select path, bit_rate from MEDIA_TABLE where "
            PODCAST_EXCL " and " AUDIOBOOK_EXCL;
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(rodb, sql, -1, &st, NULL) == SQLITE_OK) {
            int batch = 0;
            sqlite3_exec(widb, "begin", NULL, NULL, NULL);
            while (!g_usb_paused && sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char *raw_col = sqlite3_column_text(st, 0);
                if (!raw_col) continue;
                char stored[LIB_PATH_LEN], real[LIB_PATH_LEN];
                snprintf(stored, sizeof(stored), "%s", raw_col);
                real_path(real, sizeof(real), stored);
                int bitrate = sqlite3_column_int(st, 1);

                g_written += scan_one(widb, real, bitrate);
                g_scanned++;

                /* Batched commits limit fsync overhead; the short sleep,
                 * not the batch size, is what keeps this off the UI
                 * thread's and the decode thread's way -- see
                 * set_low_priority()'s own comment for why niceness alone
                 * isn't enough on a single core doing real file I/O. */
                if (++batch >= 30) {
                    sqlite3_exec(widb, "commit", NULL, NULL, NULL);
                    sqlite3_exec(widb, "begin", NULL, NULL, NULL);
                    batch = 0;
                    usleep(50000);
                }
            }
            sqlite3_exec(widb, "commit", NULL, NULL, NULL);
            sqlite3_finalize(st);
        } else {
            ilog("[index] stock db query prepare failed\n");
        }
        populate_album_cache(rodb, widb);
        populate_group_cache(rodb, widb);
        sqlite3_close(rodb);
        g_scan_running = 0;
        ilog("[index] scan pass done: %d tracks seen, %d (re)written\n", g_scanned, g_written);

        /* Manual only: waits here indefinitely for index_rescan_now() (the
         * Settings row), no periodic re-run. Was a 30-minute timer -- with
         * scanner.c now the sole source of what files exist at all (RP1:
         * hiby_player's own scanner, the only other thing that ever found
         * new files, is gone for good), the two are now always kicked
         * together (see scanner_rescan_now()'s own comment), so a separate
         * timer here just meant redundant passes between manual scans. A
         * kick that arrived *during* the pass just finished is intentionally
         * still honoured here rather than treated as already satisfied --
         * it asked for the freshest possible pass, not merely "a" pass. */
        while (!g_kick) sleep(1);
        g_kick = 0;
    }
    return NULL;
}

int index_scan_running(void) { return g_scan_running; }

/* For the Settings row: how far the most recent/current pass got. Returns 1
 * (with scanned/written filled) once at least one pass has started, 0
 * before that (nothing to report yet). */
int index_scan_progress(int *scanned, int *written) {
    if (scanned) *scanned = g_scanned;
    if (written) *written = g_written;
    return g_scan_started;
}

void index_rescan_now(void) {
    if (g_usb_paused) return;
    index_scan_start();   /* in case it was never started at all */
    g_kick = 1;
}

void index_pause_for_usb(int paused) {
    g_usb_paused = paused != 0;
}

void index_scan_start(void) {
    if (g_scan_started) return;
    g_scan_started = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, scan_worker, NULL) == 0) pthread_detach(t);
    else { ilog("[index] could not start scan thread\n"); g_scan_started = 0; }
}
