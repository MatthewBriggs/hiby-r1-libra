/* audiobook.h — browsing /Audiobooks on the card. Deliberately not the SQL
 * index (that's library.c/h): the scanner that built it has never heard of
 * this folder, and folding audiobooks into MEDIA_TABLE would mean re-running
 * that scan on every card change. This is a plain filesystem walk, scoped to
 * one folder, same as library.c's own sweep_album_folder() when the index
 * comes up short — just the only path here instead of a fallback.
 */
#ifndef AUDIOBOOK_H
#define AUDIOBOOK_H

#include <stdint.h>

#include "library.h"

#define AB_NAME_LEN     LIB_NAME_LEN
#define AB_PATH_LEN     LIB_PATH_LEN
#define AB_MAX_BOOKS    256
#define AB_MAX_FILES    64
/* BG111: a real 515-chapter audiobook hit this exactly -- was 256, silently
 * truncating (mp4_chapters() respects `max`, so nothing overflowed, the
 * book just stopped existing past chapter 256 as far as this app could
 * see). Both this and g_embedded/chap[] below are file-scope statics, not
 * stack (see g_embedded's own comment), so there is no stack-overflow
 * concern in raising it -- just BSS size, and a few hundred KB of that is
 * nothing on this device. */
#define AB_MAX_CHAPTERS 1024

/* One folder under /Audiobooks that directly contains audio files. Nested
 * layouts (Author/Book) work: a folder with no audio files of its own is
 * never a book, but every folder beneath it is still walked. */
typedef struct {
    char title[AB_NAME_LEN];   /* the folder's own name */
    char dir[AB_PATH_LEN];     /* full path, for ab_open_book() */
    /* Files, not chapters. The chapter count of a one-file book is only
     * knowable by parsing it, which is far too slow to do for every book on
     * the shelf just to fill in a list column — so the list shows this only
     * when it is meaningful, i.e. when a book really is several files. */
    int  file_count;
} ab_book_t;

/* A chapter is a stretch of one file, not necessarily a whole file — which is
 * the only way the two progress bars mean anything for the usual audiobook
 * shape of one eleven-hour .m4b. */
typedef struct {
    char    title[AB_NAME_LEN];
    int     file;              /* index into ab_book_data_t.files */
    int64_t file_start_ms;     /* where in that file the chapter begins */
    int64_t dur_ms;
    int64_t book_start_ms;     /* where in the whole book it begins */
} ab_chapter_t;

typedef struct {
    char         dir[AB_PATH_LEN];        /* the book's own folder, for ab_save_position() */
    char         files[AB_MAX_FILES][AB_PATH_LEN];
    int          file_n;
    ab_chapter_t chap[AB_MAX_CHAPTERS];
    int          chap_n;
    int64_t      total_ms;
} ab_book_data_t;

/* Recursively scans SD_ROOT/Audiobooks, title-sorted. Returns the count
 * written to `out` (capped at max). */
int ab_scan_books(ab_book_t *out, int max);

/* Read one book: its files in natural filename order ("2" before "10"), and
 * its chapters.
 *
 * A book that is a single long file is split by the chapter track the file
 * carries itself — the Audible shape, and the only source of chapter
 * boundaries there is. A book that is a folder of files gets one chapter per
 * file, which is what those layouts mean by a chapter anyway; per-file
 * embedded chapters are not expanded in that case.
 *
 * Returns the chapter count, 0 if the folder holds nothing playable. */
int ab_open_book(const char *dir, ab_book_data_t *bk);

/* Resume position, kept as a small file in the book's own folder rather than
 * a lookup table elsewhere: it stays with the book if the folder moves, needs
 * no key/hash scheme, and a stray extra file beside an eleven-hour .m4b is
 * not a cost worth avoiding. Stored as (file index into ab_open_book's
 * files[], milliseconds into that file) -- the same two numbers ab_follow()
 * already needs to find a chapter from a position, so resuming is finding a
 * chapter the normal way and then correcting the seek. */
void ab_save_position(const ab_book_data_t *bk, int file_idx, long file_ms);
/* Returns 0 and fills file_idx/file_ms if this book has a saved position, -1
 * if it has never been played (a fresh book starts at chapter 1 -- not an
 * error). */
int ab_load_position(const char *dir, int *file_idx, long *file_ms);

#endif
