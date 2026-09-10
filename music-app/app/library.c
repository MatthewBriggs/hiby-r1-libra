/* library.c — browsing the music library.
 *
 * Reads the stock index at /usr/data/usrlocal_media.db rather than scanning the
 * card again: it has already walked 176 GB, and it carries album_artist on
 * every row. What the stock browsing UI does with that is another matter — it
 * lists an album artist's individual tracks instead of their albums, which is
 * the reason this app exists.
 *
 * Two traps in that database, both found the hard way:
 *
 *  - Every string is NUL-terminated *inside the value*. Rows come back as
 *    'Ceremony\0'. A WHERE clause against 'Ceremony' matches nothing, and the
 *    stray byte renders as a box. Everything is trimmed on the way out and
 *    every lookup key is matched with LIKE 'x%' rather than =.
 *  - There is no track number. Only track_gain and track_peak, which are
 *    ReplayGain. The natural row order is alphabetical by title, so an album
 *    comes back in the wrong order. Ordering is recovered from the filename
 *    where it carries a number, and from the file's tags otherwise.
 *
 * Opened read-only: the stock scanner owns this file and may rewrite it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "vendor/sqlite3.h"
#include "library.h"
#include "tags.h"
#include "audio.h"
#include "index.h"
#include "scanner.h"

#define DB_PATH "/usr/data/usrlocal_media.db"

static sqlite3 *g_db;

/* RP6/RP1: originally written the other way round -- prefer DB_PATH
 * (hiby_player's own scanner) and fall back to scanner.c's database only
 * when DB_PATH was empty. Flipped once RP1 actually shipped (2026-08-23,
 * v0.32, the standalone no-hiby_player release): with hiby_player gone for
 * good, DB_PATH stops being written forever but does NOT go empty -- it
 * freezes at whatever it last knew, fully populated, so the old
 * empty-vs-populated check never triggered the fallback at all. A device
 * on the standalone release would have silently stopped seeing new music
 * forever, still reading a snapshot from whenever hiby_player last ran.
 * scanner.c's own database is the one still being kept current (see its
 * own header comment on the 30-minute rescan loop), so it is now the
 * default, with DB_PATH only a fallback for the gap before a device's
 * first scan pass completes. Harmless on the still-hooked (non-standalone)
 * build too: has_rows() still gates it, so a device that has never run the
 * new scanner just falls through to DB_PATH exactly as before. */
static int has_rows(sqlite3 *db) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "select count(*) from MEDIA_TABLE limit 1", -1, &st, NULL) != SQLITE_OK)
        return 0;
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n > 0;
}

/* BG-revolver follow-up: audio_probe_format() reaches dec_open(), and for
 * an .m4a that primes the first access unit through the AAC/ALAC decoder
 * into audio.c's single shared g_aacpcm buffer -- the same buffer the
 * playback worker writes into for a track playing right now. audio.c's own
 * audio_probe_dur_ms() documents that as "a real data race, not a
 * hypothetical one, since this can run from the UI thread while a track is
 * playing", and sniffs for ftyp to route M4A away from dec_open() entirely.
 * These two call sites are on the UI thread (album open, single-track
 * lookup), so they need the same care. The miss being repaired here is
 * MP3-only by construction -- it came from mp3_probe() not seeing past a
 * large ID3v2 tag -- so restricting the probe to .mp3 both fixes what it
 * was meant to fix and keeps the decoder-priming path off the UI thread.
 * Deliberately an extension test, not the database's own format column:
 * the column is what a stale row might have got wrong in the first place. */
static int is_mp3_path(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".mp3") == 0;
}

int lib_open(void) {
    if (g_db) return 0;
    /* nomutex: only the UI thread touches this handle. */
    int rc = sqlite3_open_v2(SCANNER_DB_PATH, &g_db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc == SQLITE_OK && has_rows(g_db)) return 0;
    if (g_db) { sqlite3_close(g_db); g_db = NULL; }

    rc = sqlite3_open_v2(DB_PATH, &g_db,
                         SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) {
        if (g_db) { sqlite3_close(g_db); g_db = NULL; }
        return -1;
    }
    return 0;
}

void lib_close(void) {
    if (g_db) { sqlite3_close(g_db); g_db = NULL; }
}

/* Copy a column, dropping the embedded NUL and any trailing blanks. Some rows
 * carry a name that is nothing but the NUL — those are the ~548 tracks with no
 * album artist, and they need a label rather than an empty row. */
static void copy_text(char *dst, size_t n, const unsigned char *src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (src[i] && i < n - 1) { dst[i] = (char)src[i]; i++; }
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\t')) i--;
    dst[i] = '\0';
}

static int is_blank(const char *s) {
    for (; *s; s++) if (!isspace((unsigned char)*s)) return 0;
    return 1;
}

/* BG15: LIB_UNKNOWN_MARK stands in for "the blank group", never for its
 * display label -- a real tag value never starts with this byte. */
static int is_blank_mark(const char *v) {
    return v && v[0] == LIB_UNKNOWN_MARK[0] && v[1] == '\0';
}

/* BG45: the stock scanner indexes /Podcasts along with everything else, so
 * every feed turned up in the Music section as an "album" -- 43 episodes
 * across 5 feeds on the live database, sorted alphabetically, carrying no
 * download state or resume markers, duplicating what the Podcasts section
 * already shows properly.
 *
 * No ESCAPE clause: backslash is not special to SQLite's LIKE unless one is
 * declared, so the separators are literal and only the trailing % is a
 * wildcard. The prefix contains no _ either, which would otherwise match any
 * single character. LIKE is ASCII-case-insensitive by default, so a drive
 * letter or folder in another case still matches. This is the exact prefix
 * the count in BG45 was confirmed against.
 *
 * Two spellings, because the trailing LIKE wildcard is also printf's
 * conversion character and almost every query here is assembled with
 * snprintf. Use _FMT when the text is pasted into a *format string*, and _SQL
 * when it reaches SQLite as-is (a bare string literal, or an argument to a
 * %s). Getting this backwards is not a silent style problem: as a format it
 * eats the following character as a conversion and emits nonsense SQL, and as
 * a literal it leaves a doubled %% that matches nothing. The compiler catches
 * the first case with -Wformat; nothing catches the second. */
#define PODCAST_EXCL_SQL "path not like 'a:\\Podcasts\\%'"
#define PODCAST_EXCL_FMT "path not like 'a:\\Podcasts\\%%'"

/* Same story, same fix, for /Audiobooks -- confirmed 18 rows across 3 books
 * on the live database, indexed by the stock scanner exactly like Podcasts
 * was in BG45. Scoped to the lib_albums_recent_*() pair below for now, since that is
 * what surfaced it (R30); the general Music-browsing queries above have the
 * identical gap and are not touched here -- worth its own BG entry, not
 * folded silently into an unrelated feature's diff. */
#define AUDIOBOOK_EXCL_SQL "path not like 'a:\\Audiobooks\\%'"
#define AUDIOBOOK_EXCL_FMT "path not like 'a:\\Audiobooks\\%%'"

/* Builds the WHERE-clause body (just the boolean expression, no leading
 * "where ") for a facet filter, and reports whether a LIKE pattern still
 * needs binding. The blank-group sentinel can't be matched with LIKE 'x%'
 * like a real value -- the column is empty or NUL-led, not the text shown in
 * the UI -- so it gets its own fragment and binds nothing.
 *
 * Always emits at least the podcast exclusion, so there is no longer an
 * "unfiltered" case that produces an empty string: every caller now writes an
 * unconditional "where ". */
static void filter_clause(const char *column, const char *value, int filtered,
                          char *frag, size_t fraglen, int *need_bind) {
    *need_bind = 0;
    frag[0] = '\0';
    if (!filtered) { snprintf(frag, fraglen, "%s", PODCAST_EXCL_SQL); return; }
    if (is_blank_mark(value))
        /* The stored value is a single 0x00 byte, not a true empty string --
         * confirmed against the device's own database. SQLite's length() and
         * substr() both report it as length 0 for this value (unlike a real
         * multi-byte NUL-terminated string, where they at least see the
         * leading byte), so only a direct char(0) comparison catches it;
         * substr(column,1,1)=char(0) matches nothing. `= ''` is kept beside
         * it for a column that is a genuinely empty string with no byte at
         * all, which is a different, equally real case. */
        snprintf(frag, fraglen, "(%s = '' or %s = char(0)) and " PODCAST_EXCL_FMT,
                 column, column);
    else {
        snprintf(frag, fraglen, "%s like ? escape '\\' and " PODCAST_EXCL_FMT, column);
        *need_bind = 1;
    }
}

/* A column name cannot be bound as a parameter, so it is pasted into the SQL
 * and therefore has to come off a fixed list. */
static int allowed_column(const char *c) {
    return c && (!strcmp(c, "album_artist") || !strcmp(c, "artist") ||
                 !strcmp(c, "genre"));
}

/* ---- grouped lists (album artists, artists, genres) ---------------------- */
int lib_group(const char *column, lib_row_t *out, int max, int offset) {
    if (!g_db || !allowed_column(column)) return 0;

    /* Same cache/fallback pattern as lib_albums() -- see its own comment.
     * 0 means "never scanned yet", not "empty list", and falls through to
     * the live query below unchanged. */
    {
        int n = index_group(column, out, max, offset);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                int blank = is_blank(out[i].name);
                if (blank) snprintf(out[i].name, sizeof(out[i].name), "Unknown");
                snprintf(out[i].owner, sizeof(out[i].owner), blank ? LIB_UNKNOWN_MARK : "");
            }
            return n;
        }
    }

    /* The album count is what makes these lists worth reading, and for the
     * album-artist list it is the whole point of the app: the stock browser
     * shows an album artist's tracks rather than their albums. */
    char sql[256];
    snprintf(sql, sizeof(sql),
             "select %s, count(distinct album) from MEDIA_TABLE "
             "where " PODCAST_EXCL_FMT " "
             "group by %s order by %s limit ? offset ?", column, column, column);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max);
    sqlite3_bind_int(st, 2, offset);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        copy_text(out[n].name, sizeof(out[n].name), sqlite3_column_text(st, 0));
        /* BG15: owner is otherwise unused on a group row -- reused here to
         * flag that "Unknown" is a display label, not real filter data. */
        int blank = is_blank(out[n].name);
        if (blank) snprintf(out[n].name, sizeof(out[n].name), "Unknown");
        out[n].count = sqlite3_column_int(st, 1);
        snprintf(out[n].owner, sizeof(out[n].owner), blank ? LIB_UNKNOWN_MARK : "");
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int lib_group_count(const char *column) {
    if (!g_db || !allowed_column(column)) return 0;

    {
        int n = index_group_count(column);
        if (n > 0) return n;
    }

    char sql[192];
    snprintf(sql, sizeof(sql),
             "select count(distinct %s) from MEDIA_TABLE where " PODCAST_EXCL_FMT,
             column);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

/* ---- albums, optionally within one facet value --------------------------- */
/* max(album_artist) comes back with each album so that picking it can query
 * tracks by album *and* artist. Two different artists with an album of the
 * same name is common enough — "Greatest Hits" — that matching on the title
 * alone would splice them together. */
int lib_albums(const char *column, const char *value,
               lib_row_t *out, int max, int offset) {
    if (!g_db) return 0;
    int filtered = allowed_column(column) && value;

    /* Unfiltered (whole-library) browsing is the common case -- the Albums
     * menu entry, not a facet drill-down -- and the one index.c's scan
     * pass keeps a cache for. GROUP BY/ORDER BY over MEDIA_TABLE with no
     * index on `album` meant re-sorting the whole table on every single
     * visit; a facet-filtered view is a smaller slice and reruns live
     * unchanged. 0 rows back from the cache means "never scanned yet", not
     * "empty library" -- falls through to the live query below exactly as
     * before. */
    if (!filtered) {
        int n = index_albums(out, max, offset);
        if (n > 0) {
            for (int i = 0; i < n; i++)
                if (is_blank(out[i].name))
                    snprintf(out[i].name, sizeof(out[i].name), "Unknown album");
            return n;
        }
    }

    char frag[128]; int need_bind;
    filter_clause(column, value, filtered, frag, sizeof(frag), &need_bind);
    char sql[320];
    snprintf(sql, sizeof(sql),
             "select album, count(*), max(album_artist) from MEDIA_TABLE %s%s "
             "group by album order by album limit ? offset ?",
             "where ", frag);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    int a = 1;
    char pat[LIB_NAME_LEN + 2];
    if (need_bind) {
        snprintf(pat, sizeof(pat), "%s%%", value);
        sqlite3_bind_text(st, a++, pat, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st, a++, max);
    sqlite3_bind_int(st, a++, offset);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        copy_text(out[n].name, sizeof(out[n].name), sqlite3_column_text(st, 0));
        if (is_blank(out[n].name))
            snprintf(out[n].name, sizeof(out[n].name), "Unknown album");
        out[n].count = sqlite3_column_int(st, 1);
        copy_text(out[n].owner, sizeof(out[n].owner), sqlite3_column_text(st, 2));
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int lib_albums_count(const char *column, const char *value) {
    if (!g_db) return 0;
    int filtered = allowed_column(column) && value;

    /* Same cache lib_albums() itself uses; see its own comment. */
    if (!filtered) {
        int n = index_albums_count();
        if (n > 0) return n;
    }

    char frag[128]; int need_bind;
    filter_clause(column, value, filtered, frag, sizeof(frag), &need_bind);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "select count(distinct album) from MEDIA_TABLE %s%s",
             "where ", frag);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    char pat[LIB_NAME_LEN + 2];
    if (need_bind) {
        snprintf(pat, sizeof(pat), "%s%%", value);
        sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);
    }
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

/* R30. Every album's own add-time in one pass -- ctime is per-track; an
 * album's is the newest track it has, same reasoning as lib_albums() using
 * max(album_artist): a compilation added to piecemeal should count as
 * "added" when its most recent track arrived, not its oldest.
 * ALBUMS_RECENT_SCAN caps how many distinct albums this reads before
 * ranking -- generous past the ~300 in the library this was built against
 * (audiobook.h's AB_MAX_BOOKS is 256, the closest precedent in this codebase
 * for "how big a library gets"), not a hard assumption it stays that size.
 * Kept well short of a size that would matter for this device's memory
 * budget: each entry is under 400 bytes, so this is under 200KB static.
 *
 * Excludes both Podcasts (BG45) and Audiobooks: both are indexed by the
 * stock scanner into this same table alongside real music, and neither
 * belongs in a Music-menu list -- Podcasts because it duplicates that
 * section's own proper listing, Audiobooks because it has no download
 * state, no resume marker, and no chapter structure once flattened into an
 * ordinary "album" row here, the exact complaint BG45 made about Podcasts,
 * confirmed the same way: 18 rows across 3 books on the live database. */
#define ALBUMS_RECENT_SCAN 512
typedef struct { char album[LIB_NAME_LEN]; char artist[LIB_NAME_LEN];
                 int count; long long added; } recent_scan_row_t;

static int recent_scan(recent_scan_row_t *scan, int max_scan) {
    if (!g_db) return 0;
    int n = 0;
    const char *sql =
        "select album, max(album_artist), count(*), max(ctime) from MEDIA_TABLE "
        "where " PODCAST_EXCL_SQL " and " AUDIOBOOK_EXCL_SQL " group by album";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    while (n < max_scan && sqlite3_step(st) == SQLITE_ROW) {
        copy_text(scan[n].album, sizeof(scan[n].album), sqlite3_column_text(st, 0));
        if (is_blank(scan[n].album))
            snprintf(scan[n].album, sizeof(scan[n].album), "Unknown album");
        copy_text(scan[n].artist, sizeof(scan[n].artist), sqlite3_column_text(st, 1));
        scan[n].count = sqlite3_column_int(st, 2);
        scan[n].added = sqlite3_column_int64(st, 3);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

/* Partial selection sort by whatever `key[i]` holds: only the top `max` are
 * ever read, and max is a top-10 list here, so there is no reason to fully
 * sort a few hundred rows to get them. Rows with key <= 0 are excluded
 * entirely rather than sorted to the bottom -- the caller passing heard
 * timestamps relies on this so an unheard album never appears in "recently
 * heard" just to pad out a short list; keys from ctime never hit this,
 * since every row has a real add time. */
static int recent_take(recent_scan_row_t *scan, long long *key, int n,
                       lib_row_t *out, int max) {
    int take = 0;
    for (int i = 0; i < n && take < max; i++) {
        int best = -1;
        for (int j = i; j < n; j++)
            if (key[j] > 0 && (best < 0 || key[j] > key[best])) best = j;
        if (best < 0) break;   /* nothing left with a real timestamp */
        if (best != i) {
            recent_scan_row_t ts = scan[i]; scan[i] = scan[best]; scan[best] = ts;
            long long tk = key[i]; key[i] = key[best]; key[best] = tk;
        }
        snprintf(out[take].name, sizeof(out[take].name), "%s", scan[i].album);
        snprintf(out[take].owner, sizeof(out[take].owner), "%s", scan[i].artist);
        out[take].count = scan[i].count;
        take++;
    }
    return take;
}

int lib_albums_recent_added(lib_row_t *out, int max) {
    if (max <= 0) return 0;
    static recent_scan_row_t scan[ALBUMS_RECENT_SCAN];
    static long long key[ALBUMS_RECENT_SCAN];
    int n = recent_scan(scan, ALBUMS_RECENT_SCAN);
    for (int i = 0; i < n; i++) key[i] = scan[i].added;
    return recent_take(scan, key, n, out, max);
}

int lib_albums_recent_heard(long long (*heard_ts)(const char *album),
                            lib_row_t *out, int max) {
    if (!heard_ts || max <= 0) return 0;
    static recent_scan_row_t scan[ALBUMS_RECENT_SCAN];
    static long long key[ALBUMS_RECENT_SCAN];
    int n = recent_scan(scan, ALBUMS_RECENT_SCAN);
    for (int i = 0; i < n; i++) key[i] = heard_ts(scan[i].album);
    return recent_take(scan, key, n, out, max);
}

/* The index stores paths as the player's own volume notation — every row is
 * "a:\Artist\Album\01 Title.flac", drive letter and backslashes. Nothing will
 * open until that is turned back into a real path; a: is the card. */
#define SD_ROOT "/data/mnt/sd_0/"

void real_path(char *dst, size_t n, const char *stored) {
    const char *p = stored;
    if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':' &&
        (p[2] == '\\' || p[2] == '/'))
        p += 3;
    snprintf(dst, n, "%s%s", SD_ROOT, p);
    for (char *q = dst; *q; q++) if (*q == '\\') *q = '/';
}

/* ---- tracks on an album -------------------------------------------------- */
/* Pull the leading number out of a filename: "04. New Order - ...", "11 - x",
 * "Disc 1 - 11 - x". Returns -1 when there is nothing to read, which is the
 * signal to fall back to the file's own tag. */
static int track_no_from_path(const char *path) {
    const char *base = strrchr(path, '/');
    const char *b2 = strrchr(path, '\\');
    if (b2 > base) base = b2;
    base = base ? base + 1 : path;

    /* A "Disc 1 - 11 - title" name has two numbers; the track is the last one
     * before the title, so skip a leading disc marker if present. */
    if (!strncasecmp(base, "disc", 4)) {
        const char *dash = strchr(base, '-');
        if (dash) base = dash + 1;
        while (*base == ' ') base++;
    }
    if (!isdigit((unsigned char)*base)) return -1;
    int v = 0, digits = 0;
    while (isdigit((unsigned char)*base) && digits < 3) { v = v * 10 + (*base++ - '0'); digits++; }
    /* Require a separator so "1999 - song.flac" is not read as track 199. */
    if (*base != '.' && *base != ' ' && *base != '-' && *base != '_' && *base != ')')
        return -1;
    return v;
}

/* Files here are often named "01-03 Title" — disc, then track. Where the tags
 * do not carry a disc number, that prefix is the only thing separating one
 * disc of a boxed set from the next. */
static int disc_no_from_path(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    /* Skip any album-title prefix and take the last "NN-NN" in the name. */
    const char *best = NULL;
    for (const char *p = base; *p; p++) {
        if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) &&
            p[2] == '-' && isdigit((unsigned char)p[3]) && isdigit((unsigned char)p[4]) &&
            (p == base || !isdigit((unsigned char)p[-1])))
            best = p;
    }
    if (!best) return -1;
    int d = (best[0] - '0') * 10 + (best[1] - '0');
    return d > 0 && d < 100 ? d : -1;
}

static int cmp_track(const void *a, const void *b) {
    const lib_track_t *x = a, *y = b;
    /* Disc first, or a boxed set interleaves: every disc's track 1, then
     * every track 2. Unstated counts as disc 1 so single-disc albums and
     * stray files sort together. */
    int dx = x->disc > 0 ? x->disc : 1, dy = y->disc > 0 ? y->disc : 1;
    if (dx != dy) return dx - dy;
    if (x->track != y->track) {
        if (x->track < 0) return 1;          /* unnumbered sink to the bottom */
        if (y->track < 0) return -1;
        return x->track - y->track;
    }
    return strcmp(x->name, y->name);
}

static int is_audio(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".flac") || !strcasecmp(dot, ".mp3") ||
           !strcasecmp(dot, ".m4a")  || !strcasecmp(dot, ".wav") ||
           !strcasecmp(dot, ".aiff") || !strcasecmp(dot, ".aif") ||
           !strcasecmp(dot, ".ogg")  || !strcasecmp(dot, ".oga") ||
           !strcasecmp(dot, ".opus");
}

/* Strip a leading "01-04 " or "04 " and the extension, for a name to show. */
static void name_from_file(const char *file, char *out, size_t n) {
    const char *p = file;
    while (*p && (isdigit((unsigned char)*p) || *p == '-')) p++;
    while (*p == ' ' || *p == '.' || *p == '_') p++;
    if (!*p) p = file;
    snprintf(out, n, "%s", p);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* seed_path only needs to be some path that once lived in this album's
 * folder -- used purely to find the folder, not read as a track. Passed
 * explicitly rather than read from out[0] so this still works when every
 * indexed row for the album is stale and n is 0 (see the BG11 fix below):
 * out[0] is never populated in that case, but the folder is still knowable
 * from the row that failed its stat() check. */
static int sweep_album_folder(lib_track_t *out, int n, int max,
                              const char *seed_path, const char *artist_fallback) {
    char dir[LIB_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", seed_path);
    char *slash = strrchr(dir, '/');
    if (!slash) return n;
    *slash = '\0';

    DIR *d = opendir(dir);
    if (!d) return n;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        /* "._Name.flac" is a macOS AppleDouble sidecar — a few KB of resource
         * fork, not music. There are 99 of them on this card and the sweep
         * would otherwise offer every one as a track. The leading-dot test
         * catches them, and the size test catches anything else too small to
         * be audio. */
        if (e->d_name[0] == '.' || !is_audio(e->d_name)) continue;
        char full[LIB_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || st.st_size < 64 * 1024) continue;

        int known = 0;
        for (int i = 0; i < n; i++)
            if (!strcmp(out[i].path, full)) { known = 1; break; }
        if (known) continue;

        lib_track_t *t = &out[n];
        memset(t, 0, sizeof(*t));
        snprintf(t->path, sizeof(t->path), "%s", full);
        /* One open for track+disc+title instead of three separate ones. */
        tag_read(full, &t->track, &t->disc, t->name, sizeof(t->name));
        /* Prefer what the file calls itself; these names are long and the
         * filename carries the whole album title as a prefix. */
        if (!t->name[0])
            name_from_file(e->d_name, t->name, sizeof(t->name));
        /* artist_fallback, not out[0].artist: out[0] is never written when n
         * starts at 0 (BG11 — every indexed row for this album was stale),
         * so reading it here would be uninitialized caller memory. */
        snprintf(t->artist, sizeof(t->artist), "%s", artist_fallback);
        if (t->track <= 0) t->track = track_no_from_path(full);
        if (t->disc <= 0) t->disc = disc_no_from_path(full);
        n++;
    }
    closedir(d);
    return n;
}

/* BG11's actual cause, found from the device's own database, not guessed:
 * "In Time: The Best of R.E.M." has album_artist empty (a bare NUL, no
 * scanner-written tag at all — a real compilation-album gap, not
 * corruption) on every one of its 33 rows, while `artist` is correctly
 * "R.E.M." per track. `album_artist LIKE 'R.E.M.%'` then matches nothing —
 * a real artist name can never match an empty field — so the query below
 * returns zero rows, not 33 with stale paths, and the sweep fallback above
 * (n==0 && seed_path[0]) never gets a folder to sweep either. The only
 * reason this ever looked like it worked was an unrelated bug: reaching
 * this album via Albums directly left `cur_artist` empty (a `row->owner`
 * that is itself the same bare NUL), and an empty artist parameter turns
 * into a `%` wildcard that happens to match anything, including a blank
 * album_artist. That is a coincidence a stale cur_artist should not be
 * relied on for, not a fix.
 *
 * use_artist=1 tries the disambiguating filter first — needed when two
 * different artists share an album name and album_artist is populated.
 * use_artist=0 is the retry once that comes back with literally nothing to
 * work with, dropping the filter and matching by album alone.
 *
 * The artist match itself is deliberately two-directional, not a plain
 * "column starts with the tapped artist" LIKE: found live on a Schoenberg
 * box set where 34 of 37 tracks carry album_artist "Berliner Philharmoniker,
 * Kirill Petrenko" and the 3 Violin Concerto tracks add the soloist,
 * "...Kirill Petrenko, Patricia Kopatchinskaja". lib_albums()'s max(album_
 * artist) picks the longer string (lexicographically greater), so the row
 * tapped from Albums always carries the soloist's name -- and the old
 * one-directional "album_artist like 'tapped-value%'" then matched only
 * the 3 rows that actually start with that longer string, silently
 * dropping the other 34. The second clause below catches the reverse
 * case -- the tapped value starting with the row's own (shorter)
 * album_artist -- without weakening the filter for two genuinely
 * different artists sharing an album title (their names don't prefix one
 * another, so neither clause matches across them).
 *
 * substr(album_artist, 1, length(album_artist)-1), not album_artist itself,
 * on the right side of that second clause: this file's own header comment
 * on the stock DB's traps says every string carries a NUL *inside* the
 * value, not just as a terminator -- confirmed live, hex-dumped directly
 * off the Schoenberg rows: the 40-byte "short" album_artist is 39 real
 * bytes plus a trailing 0x00 (the 65-byte "long" one is 64 real bytes
 * plus the same). Appending '%' straight onto the raw column therefore
 * appended it *after* that embedded NUL, so the pattern demanded the
 * literal byte sequence "...Petrenko\0%" -- which the actual searched
 * value (a plain C string with no embedded NUL of its own) can never
 * contain, so the clause matched nothing at all despite being logically
 * correct. Dropping the last byte before appending '%' fixes it; verified
 * directly against a pulled copy of the real device database, 3 rows to
 * 37. (char(0)/rtrim(x, char(0)) were tried first and don't work here --
 * this build's char(0) reports length() 0, not 1, so neither rtrim() nor
 * replace() ever had a real NUL byte to strip.) */
/* BG91: tapping "Substance" (Joy Division) also played tracks from
 * "Substance 1987" (New Order), interleaved -- reported live, and visible
 * directly in music.log's own play sequence. Root cause: album was matched
 * with the exact same trailing-wildcard LIKE pattern (`album || '%'`)
 * artist/genre facets use deliberately, for a reason that doesn't apply to
 * album at all -- facets use a prefix match so a compound value like
 * "Berliner Philharmoniker, Kirill Petrenko" still matches on the shorter
 * "Berliner Philharmoniker" (see tracks_query()'s own header comment on the
 * album_artist half of this same function). An album title has no such
 * legitimate shorter/longer relationship to itself; "Substance" is simply a
 * different, unrelated album from "Substance 1987", and the wildcard
 * matched both. Fixed with exact equality instead of a prefix match, still
 * tolerating the stock DB's embedded-trailing-NUL quirk (this file's own
 * header comment) by comparing against both the plain value and the value
 * with one real NUL byte appended -- built in C with an explicit bind
 * length rather than SQL's char(0), which the album_artist comment above
 * already found doesn't behave as a real NUL byte in this SQLite build. */
static int tracks_query(const char *artist, const char *album, int use_artist,
                        lib_track_t *out, int max, char *seed_path, size_t seed_len) {
    const char *sql = use_artist
        ? "select name, path, format, bit, sample_rate, bit_rate, end_time, artist "
          "from MEDIA_TABLE where (album = ? or album = ?) "
          "and (album_artist like ? escape '\\' "
          "or ? like (substr(album_artist, 1, length(album_artist) - 1) || '%')) "
          "and " PODCAST_EXCL_SQL " limit ?"
        : "select name, path, format, bit, sample_rate, bit_rate, end_time, artist "
          "from MEDIA_TABLE where (album = ? or album = ?) "
          "and " PODCAST_EXCL_SQL " limit ?";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    char pa[LIB_NAME_LEN + 2], pb[LIB_NAME_LEN + 2];
    int alen = (int)strlen(album);
    if (alen > LIB_NAME_LEN) alen = LIB_NAME_LEN;
    memcpy(pa, album, (size_t)alen);
    pa[alen] = '\0';
    int a = 1;
    sqlite3_bind_text(st, a++, pa, alen, SQLITE_TRANSIENT);        /* exact */
    sqlite3_bind_text(st, a++, pa, alen + 1, SQLITE_TRANSIENT);    /* exact + trailing NUL */
    if (use_artist) {
        snprintf(pb, sizeof(pb), "%s%%", artist);
        sqlite3_bind_text(st, a++, pb, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, a++, artist, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st, a++, max);
    int n = 0;
    /* BG11: seed_path is kept even when a row's file is gone, so there is
     * still a folder to sweep if every single row turns out stale -- an
     * album where every path was renamed used to come back with n stuck at
     * 0 and out[0] never written, which meant no folder to derive and no
     * way for the caller's sweep to ever run. One renamed path is as good
     * as any other for finding the folder; it is never read as a track. */
    seed_path[0] = '\0';
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        lib_track_t *t = &out[n];
        copy_text(t->name, sizeof(t->name), sqlite3_column_text(st, 0));
        char stored[LIB_PATH_LEN];
        copy_text(stored, sizeof(stored), sqlite3_column_text(st, 1));
        real_path(t->path, sizeof(t->path), stored);
        if (!seed_path[0]) snprintf(seed_path, seed_len, "%s", t->path);
        t->format = sqlite3_column_int(st, 2);
        t->bits = sqlite3_column_int(st, 3);
        t->rate = sqlite3_column_int(st, 4);
        t->bitrate = sqlite3_column_int(st, 5);
        t->dur_ms = sqlite3_column_int(st, 6);
        /* R28: end_time is only ever populated for a cue-sheet split track
         * (one physical file shared across several rows) -- an ordinary
         * track, which is nearly everything, is left at -1 indefinitely.
         * See audio_probe_dur_ms()'s own comment for why this is safe to
         * call here, including while something else is playing. */
        /* Rows can outlive their files — renaming anything leaves the index
         * pointing at a name that is gone, and a track that cannot be opened
         * is worse than one that is absent. The folder sweep below picks the
         * real file up under its new name. One stat() here does double duty:
         * this existence check, and (below) the mtime our own track_index
         * cache is keyed against, rather than stat()ing the same path twice. */
        struct stat fst;
        if (stat(t->path, &fst) != 0) continue;

        /* Our own background scanner (index.c) already read this file's tags
         * once; a hit here skips tag_read() and audio_probe_dur_ms() below
         * entirely -- the whole reason an album with hundreds of tracks
         * doesn't have to pay hundreds of file opens on every single open. */
        int idx_track, idx_disc, idx_dur;
        int cached = index_lookup(t->path, fst.st_mtime, &idx_track, &idx_disc, &idx_dur);
        if (cached && idx_dur > 0) t->dur_ms = idx_dur;

        /* BG-revolver: bitrate can be genuinely 0 in the database from
         * before audio.c's mp3_probe() fix (a large ID3v2 tag -- embedded
         * cover art routinely pushes one past a few hundred KB -- used to
         * push the sync scan past the only bytes actually read, silently
         * leaving bitrate unprobed). A plain rescan won't self-heal this:
         * scanner.c skips a file whose mtime hasn't changed, and nothing
         * here touches the file itself. Same live-probe-on-miss shape
         * R28's dur_ms fallback below already uses, and for the same
         * reason: cheap per file (a header parse, not a decode), so paying
         * it only for the rows that actually need it costs nothing for a
         * library that's already fine. */
        if (t->bitrate <= 0 && is_mp3_path(t->path)) {
            int pbits, prate, pbitrate, pdur;
            if (audio_probe_format(t->path, &pbits, &prate, &pbitrate, &pdur) == 0 && pbitrate > 0)
                t->bitrate = pbitrate;
        }
        if (t->dur_ms <= 0) t->dur_ms = audio_probe_dur_ms(t->path, t->bitrate);
        copy_text(t->artist, sizeof(t->artist), sqlite3_column_text(st, 7));
        /* Ask the file first. The filename is a guess that happens to be right
         * about half the time here, and a wrong guess does not leave a track
         * unnumbered — it puts the album in the wrong order. */
        if (cached) {
            t->track = idx_track;
            t->disc = idx_disc;
        } else {
            tag_read(t->path, &t->track, &t->disc, NULL, 0);
        }
        if (t->track <= 0) t->track = track_no_from_path(t->path);
        if (t->disc <= 0) t->disc = disc_no_from_path(t->path);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int lib_tracks_for_album(const char *artist, const char *album,
                         lib_track_t *out, int max, int expected) {
    if (!g_db) return 0;
    char seed_path[LIB_PATH_LEN];
    int n = tracks_query(artist, album, 1, out, max, seed_path, sizeof(seed_path));

    /* Nothing matched at the SQL level at all -- not "matched but every
     * path is stale" (that still sets seed_path; see tracks_query's own
     * comment), but zero rows, full stop. album_artist is empty on every
     * row for a real, fairly common reason (no album-artist tag ever
     * written, common on compilations) and a real artist name can never
     * match an empty field. Retry once without the filter, by album name
     * alone. This trades the artist-disambiguation the filter exists for
     * (two different artists sharing an album name) for actually finding
     * the album at all, which only matters in the disambiguation's own
     * edge case -- and only once the first, more specific attempt has
     * already found nothing to work with. */
    if (n == 0 && !seed_path[0])
        n = tracks_query(artist, album, 0, out, max, seed_path, sizeof(seed_path));

    /* The stock index is not complete. A Beethoven 7 on this card has four
     * movements on disk and three in the database — the scanner simply missed
     * one, and an album quietly short of a track is worse than a slow open.
     * Sweep the folder the album came from and take anything it did not know
     * about. Runs even at n==0: the index can show a track count in the
     * album list yet have every one of those rows' paths gone stale.
     *
     * `expected` (the Albums list's own count for this row) was briefly used
     * to skip this sweep once n reached it -- wrong, and reverted: found live
     * on "Arnold Schoenberg", 37 tracks on disk, 3 in the database. The
     * Albums list's count comes from the *same* incomplete SQL side as
     * tracks_query() above, so n reaching `expected` only proves the two
     * agree with each other, never that the filesystem doesn't have more --
     * which is exactly the case this sweep exists to catch. `expected` is
     * kept as a parameter (some future, actually-independent count might
     * use it safely) but no longer read here. */
    (void)expected;
    if (n < max && seed_path[0])
        n = sweep_album_folder(out, n, max, seed_path, artist);

    /* The index carries the same file twice for some albums, which showed as a
     * doubled movement. Drop repeats whatever their source. */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; ) {
            if (!strcmp(out[i].path, out[j].path)) {
                memmove(&out[j], &out[j + 1], sizeof(out[0]) * (size_t)(n - j - 1));
                n--;
            } else j++;
        }

    qsort(out, (size_t)n, sizeof(out[0]), cmp_track);
    return n;
}

/* The counts below must agree with the ORDER BY the lists use, or a jump lands
 * in the wrong place. Both use the column's natural ordering, so both use a
 * plain `<` against the letter rather than anything cleverer. */
int lib_group_offset(const char *column, const char *key) {
    if (!g_db || !allowed_column(column)) return 0;
    char sql[256];
    snprintf(sql, sizeof(sql),
             "select count(*) from (select distinct %s as v from MEDIA_TABLE "
             "where " PODCAST_EXCL_FMT ") "
             "where v < ?", column);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

int lib_albums_offset(const char *column, const char *value, const char *key) {
    if (!g_db) return 0;
    int filtered = allowed_column(column) && value;
    char frag[128]; int need_bind;
    filter_clause(column, value, filtered, frag, sizeof(frag), &need_bind);
    char sql[320];
    snprintf(sql, sizeof(sql),
             "select count(*) from (select distinct album as v from MEDIA_TABLE %s%s) "
             "where v < ?",
             "where ", frag);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    int a = 1;
    char pat[LIB_NAME_LEN + 2];
    if (need_bind) {
        snprintf(pat, sizeof(pat), "%s%%", value);
        sqlite3_bind_text(st, a++, pat, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(st, a, key, -1, SQLITE_TRANSIENT);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

/* The A-Z strip needs both the start of the touched letter and the start of
 * the next one. Asking lib_*_offset() for those values while a thumb crosses
 * the strip means many complete distinct-list scans. Do the same comparisons
 * in one aggregate query instead: SQLite still decides collation and ordering,
 * so this remains exactly in step with the single-key helpers. */
int lib_letter_offsets(const char *column, const char *value, int albums,
                       int out[LIB_INDEX_LETTERS]) {
    if (!out || !g_db) return -1;
    for (int i = 0; i < LIB_INDEX_LETTERS; i++) out[i] = 0;

    int filtered = albums && allowed_column(column) && value;
    if (!albums && !allowed_column(column)) return -1;

    char frag[128]; int need_bind;
    filter_clause(column, value, filtered, frag, sizeof(frag), &need_bind);

    char sql[1024];
    int used = snprintf(sql, sizeof(sql), "select ");
    for (int i = 0; i < LIB_INDEX_LETTERS && used > 0 && used < (int)sizeof(sql); i++)
        used += snprintf(sql + used, sizeof(sql) - (size_t)used,
                         "%ssum(v < ?)", i ? ", " : "");
    if (used <= 0 || used >= (int)sizeof(sql)) return -1;

    if (albums)
        used += snprintf(sql + used, sizeof(sql) - (size_t)used,
                         " from (select distinct album as v from MEDIA_TABLE %s%s)",
                         "where ", frag);
    else
        used += snprintf(sql + used, sizeof(sql) - (size_t)used,
                         " from (select distinct %s as v from MEDIA_TABLE "
                         "where " PODCAST_EXCL_FMT ")", column);
    if (used <= 0 || used >= (int)sizeof(sql)) return -1;

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int bind = 1;
    for (int i = 0; i < LIB_INDEX_LETTERS; i++) {
        char key[2] = { (char)('A' + i), '\0' };
        sqlite3_bind_text(st, bind++, key, -1, SQLITE_TRANSIENT);
    }
    if (need_bind) {
        char pat[LIB_NAME_LEN + 2];
        snprintf(pat, sizeof(pat), "%s%%", value);
        sqlite3_bind_text(st, bind++, pat, -1, SQLITE_TRANSIENT);
    }

    int rc = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        for (int i = 0; i < LIB_INDEX_LETTERS; i++)
            out[i] = sqlite3_column_int(st, i);
        rc = 0;
    }
    sqlite3_finalize(st);
    return rc;
}

/* The inverse of real_path: back to "a:\Artist\Album\file.flac" so the row
 * can be found. Doing it this way rather than storing the index's own paths in
 * the playlist keeps the .m3u readable by anything else.
 *
 * Deliberately carries no PODCAST_EXCL, unlike every browsing query above.
 * This resolves one already-known path that something else decided to play --
 * a playlist entry or a resume position -- rather than deciding what to offer.
 * Filtering here would not tidy a listing, it would make an episode
 * unresolvable and break resuming it. */
int lib_track_by_path(const char *real, lib_track_t *out) {
    if (!g_db) return -1;
    const char *rel = real;
    if (!strncmp(real, SD_ROOT, strlen(SD_ROOT))) rel = real + strlen(SD_ROOT);

    char stored[LIB_PATH_LEN];
    snprintf(stored, sizeof(stored), "a:\\%s", rel);
    for (char *q = stored; *q; q++) if (*q == '/') *q = '\\';

    static const char *sql =
        "select name, path, format, bit, sample_rate, bit_rate, end_time, artist "
        "from MEDIA_TABLE where path like ? escape '\x01' limit 1";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    char pat[LIB_PATH_LEN + 2];
    snprintf(pat, sizeof(pat), "%s%%", stored);
    sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);

    int rc = -1;
    int stepped = sqlite3_step(st);
    if (stepped != SQLITE_ROW) {
        /* BG-playlist: an old scanner.c briefly wrote "a:\\\\" (a doubled
         * backslash after the drive letter) instead of "a:\\" -- real_path()
         * silently absorbs that extra separator on the way back out (its own
         * backslash-to-slash pass turns it into a harmless doubled slash), so
         * nothing else ever noticed, but this LIKE prefix is byte-exact and
         * never matched those rows. Files re-tagged since the fix landed are
         * fine; anything scanned before it and never touched since (an
         * unchanged mtime skips re-deriving the path) is still stuck in the
         * old form, which is common enough that a plain lookup miss is worth
         * one retry against it rather than leaving those tracks unresolvable
         * forever -- confirmed live: this is why a playlist entry added from
         * one of those older files came back empty. */
        char stored2[LIB_PATH_LEN];
        snprintf(stored2, sizeof(stored2), "a:\\\\%s", rel);
        for (char *q = stored2 + 3; *q; q++) if (*q == '/') *q = '\\';
        char pat2[LIB_PATH_LEN + 2];
        snprintf(pat2, sizeof(pat2), "%s%%", stored2);
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, pat2, -1, SQLITE_TRANSIENT);
        stepped = sqlite3_step(st);
    }
    if (stepped == SQLITE_ROW) {
        copy_text(out->name, sizeof(out->name), sqlite3_column_text(st, 0));
        char raw[LIB_PATH_LEN];
        copy_text(raw, sizeof(raw), sqlite3_column_text(st, 1));
        real_path(out->path, sizeof(out->path), raw);
        out->format  = sqlite3_column_int(st, 2);
        out->bits    = sqlite3_column_int(st, 3);
        out->rate    = sqlite3_column_int(st, 4);
        out->bitrate = sqlite3_column_int(st, 5);
        out->dur_ms  = sqlite3_column_int(st, 6);
        copy_text(out->artist, sizeof(out->artist), sqlite3_column_text(st, 7));
        struct stat fst;
        int idx_track, idx_disc, idx_dur;
        int cached = stat(out->path, &fst) == 0 &&
                     index_lookup(out->path, fst.st_mtime, &idx_track, &idx_disc, &idx_dur);
        if (out->dur_ms <= 0 && cached && idx_dur > 0) out->dur_ms = idx_dur;
        /* Same live-probe-on-miss fallback as the sibling query above --
         * see its own comment (BG-revolver). */
        if (out->bitrate <= 0 && is_mp3_path(out->path)) {
            int pbits, prate, pbitrate, pdur;
            if (audio_probe_format(out->path, &pbits, &prate, &pbitrate, &pdur) == 0 && pbitrate > 0)
                out->bitrate = pbitrate;
        }
        if (out->dur_ms <= 0) out->dur_ms = audio_probe_dur_ms(out->path, out->bitrate);
        if (cached) {
            out->track = idx_track;
            out->disc = idx_disc;
        } else {
            tag_read(out->path, &out->track, &out->disc, NULL, 0);
        }
        if (out->track <= 0) out->track = track_no_from_path(out->path);
        if (out->disc <= 0) out->disc = disc_no_from_path(out->path);
        rc = 0;
    }
    sqlite3_finalize(st);
    return rc;
}

/* The format column is a code, not a key into FORMAT_TABLE — that table's
 * first column is a count. These were read off the library directly. */

/* Every two-character prefix in the list, in sort order, in one pass. The
 * per-letter version of this ran a full scan on each letter the thumb crossed
 * — twenty-six of them on one drag down the strip, on a device that has to
 * read them off a card. Gathered once per list instead, so the drag itself
 * costs no query at all. `out` is 2 bytes per prefix, unterminated. */
int lib_prefixes(const char *column, const char *value, int albums,
                 char *out, int max) {
    if (!g_db || max <= 0) return 0;
    int filtered = albums && allowed_column(column) && value;
    char frag[128]; int need_bind = 0;
    char sql[420];
    if (albums) {
        filter_clause(column, value, filtered, frag, sizeof(frag), &need_bind);
        snprintf(sql, sizeof(sql),
                 "select distinct substr(album,1,2) from MEDIA_TABLE %s%s "
                 "order by 1",
                 "where ", frag);
    } else {
        if (!allowed_column(column)) return 0;
        snprintf(sql, sizeof(sql),
                 "select distinct substr(%s,1,2) from MEDIA_TABLE "
                 "where " PODCAST_EXCL_FMT " order by 1",
                 column);
    }
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    if (need_bind) {
        char vpat[LIB_NAME_LEN + 2];
        snprintf(vpat, sizeof(vpat), "%s%%", value);
        sqlite3_bind_text(st, 1, vpat, -1, SQLITE_TRANSIENT);
    }
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        if (!t || !t[0] || !t[1]) continue;
        /* Accented second letters are multi-byte and get no band of their
         * own; the plain letter still reaches them. */
        if (!isalpha((int)t[1])) continue;
        char a = (char)t[0], b = (char)tolower((int)t[1]);
        int dup = 0;
        for (int i = 0; i < n; i++)
            if (out[2 * i] == a && out[2 * i + 1] == b) dup = 1;
        if (!dup) { out[2 * n] = a; out[2 * n + 1] = b; n++; }
    }
    sqlite3_finalize(st);
    return n;
}

const char *lib_format_name(int code) {
    switch (code) {
        case 61868: case 0: return "FLAC";
        case 85:            return "MP3";
        case 278: case 41388: return "AAC";
        case 1:             return "WAV";
        default:            return "?";
    }
}
