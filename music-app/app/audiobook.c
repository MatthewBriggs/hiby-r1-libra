/* audiobook.c — folder walk under /Audiobooks. See audiobook.h for why this
 * is not the SQL index. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audiobook.h"
#include "mp4.h"
#include "tags.h"

/* Same card, same mount point as library.c's SD_ROOT -- not shared via a
 * header because library.c's is file-local and this is the only other place
 * that needs it. */
#define AUDIOBOOK_ROOT "/data/mnt/sd_0/Audiobooks"

static int is_audio_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".m4a") ||
           !strcasecmp(dot, ".m4b") || !strcasecmp(dot, ".flac") ||
           !strcasecmp(dot, ".wav");
}

/* The MP4 family says how long it is in a small box near the end of the file;
 * everything else would need a decode pass, so it is left unknown and filled
 * in by the decoder once the chapter is played. */
static int is_mp4_file(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".m4b") || !strcasecmp(dot, ".m4a"));
}

static int dir_has_audio(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    int have = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !is_audio_file(e->d_name)) continue;
        have = 1;
        break;
    }
    closedir(d);
    return have;
}

static int count_audio(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !is_audio_file(e->d_name)) continue;
        n++;
    }
    closedir(d);
    return n;
}

static int cmp_book(const void *a, const void *b) {
    return strcasecmp(((const ab_book_t *)a)->title, ((const ab_book_t *)b)->title);
}

/* Depth-capped, not because a real card is expected to nest deeply, but
 * because there is no loop protection otherwise and a card is untrusted
 * input. A folder that has its own audio files AND subfolders is unusual but
 * not wrong -- it is still a book, and the subfolders are still walked. */
static int walk(const char *path, ab_book_t *out, int n, int max, int depth) {
    if (n >= max || depth > 6) return n;

    if (dir_has_audio(path)) {
        ab_book_t *b = &out[n];
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(b->title, sizeof(b->title), "%s", base);
        snprintf(b->dir, sizeof(b->dir), "%s", path);
        b->file_count = count_audio(path);
        n++;
    }

    DIR *d = opendir(path);
    if (!d) return n;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[AB_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        n = walk(full, out, n, max, depth + 1);
    }
    closedir(d);
    return n;
}

int ab_scan_books(ab_book_t *out, int max) {
    int n = walk(AUDIOBOOK_ROOT, out, 0, max, 0);
    qsort(out, (size_t)n, sizeof(out[0]), cmp_book);
    return n;
}

/* "track2" before "track10": compares runs of digits numerically rather than
 * character by character, the same ordering a person expects from a folder
 * of chapter files. */
static int natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            long va = strtol(a, &ea, 10);
            long vb = strtol(b, &eb, 10);
            if (va != vb) return va < vb ? -1 : 1;
            a = ea; b = eb;
        } else {
            if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
            a++; b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int cmp_path(const void *a, const void *b) {
    const char *x = (const char *)a, *y = (const char *)b;
    const char *bx = strrchr(x, '/'); bx = bx ? bx + 1 : x;
    const char *by = strrchr(y, '/'); by = by ? by + 1 : y;
    return natural_cmp(bx, by);
}

static void name_of(const char *path, char *out, size_t n) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, n, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* Scratch for one file's embedded chapter list. Static rather than automatic:
 * this runs on the UI thread and 256 of these is 34 KB, too much to put on a
 * stack. */
static mp4_chapter_t g_embedded[AB_MAX_CHAPTERS];

int ab_open_book(const char *dir, ab_book_data_t *bk) {
    memset(bk, 0, sizeof(*bk));
    snprintf(bk->dir, sizeof(bk->dir), "%s", dir);

    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while (bk->file_n < AB_MAX_FILES && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !is_audio_file(e->d_name)) continue;
        char full[AB_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        /* Small enough to rule out audio, generous enough not to reject a
         * real file -- the same floor library.c's folder sweep uses. */
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 16 * 1024)
            continue;
        snprintf(bk->files[bk->file_n], AB_PATH_LEN, "%s", full);
        bk->file_n++;
    }
    closedir(d);
    if (!bk->file_n) return 0;
    qsort(bk->files, (size_t)bk->file_n, AB_PATH_LEN, cmp_path);

    /* One long file: split it by the chapter track it carries itself. */
    if (bk->file_n == 1 && is_mp4_file(bk->files[0])) {
        int nc = mp4_chapters(bk->files[0], g_embedded, AB_MAX_CHAPTERS);
        long total = mp4_duration_ms(bk->files[0]);
        if (nc > 1 && total > 0) {
            for (int i = 0; i < nc; i++) {
                ab_chapter_t *c = &bk->chap[i];
                snprintf(c->title, sizeof(c->title), "%s", g_embedded[i].title);
                c->file          = 0;
                c->file_start_ms = g_embedded[i].start_ms;
                c->book_start_ms = g_embedded[i].start_ms;
                c->dur_ms = ((i + 1 < nc) ? g_embedded[i + 1].start_ms : total)
                          - g_embedded[i].start_ms;
                if (c->dur_ms < 0) c->dur_ms = 0;
            }
            bk->chap_n   = nc;
            bk->total_ms = total;
            return bk->chap_n;
        }
    }

    /* Otherwise a chapter is a file. */
    int64_t at = 0;
    for (int i = 0; i < bk->file_n && i < AB_MAX_CHAPTERS; i++) {
        ab_chapter_t *c = &bk->chap[i];
        if (tag_title(bk->files[i], c->title, sizeof(c->title)) != 0 || !c->title[0])
            name_of(bk->files[i], c->title, sizeof(c->title));
        c->file          = i;
        c->file_start_ms = 0;
        c->book_start_ms = at;
        c->dur_ms        = is_mp4_file(bk->files[i])
                         ? (int64_t)mp4_duration_ms(bk->files[i]) : 0;
        at += c->dur_ms;
        bk->chap_n++;
    }
    /* Titles come from MP3 and MP4 tags too now, not only FLAC's, and many
     * books give every file the book's own name as its title: chapters all
     * called the same say nothing, so those go back to the file names. */
    int same = bk->chap_n > 1;
    for (int i = 1; same && i < bk->chap_n; i++)
        if (strcmp(bk->chap[i].title, bk->chap[0].title)) same = 0;
    for (int i = 0; same && i < bk->chap_n; i++)
        name_of(bk->files[i], bk->chap[i].title, sizeof(bk->chap[i].title));
    bk->total_ms = at;
    return bk->chap_n;
}

/* Used to be one ".position" file per book, living inside that book's own
 * folder on the SD card -- named that way, not a hash of the folder, so it
 * stayed human-readable/eyeball-able there. Moved to a single shared file
 * on internal storage instead, one line per book, same shape and the same
 * crash-safe write-then-rename pattern podcast.c's pod_resume_store()
 * already uses for its own (single, shared) resume file -- mirrored
 * deliberately rather than reinvented. A personal audiobook library is
 * small enough that a plain linear scan costs nothing, and this device's
 * own exFAT card is documented (index.c's own comment) to stall badly
 * under concurrent access; keeping this tiny, frequently-written file off
 * it entirely avoids that class of problem outright rather than depending
 * on call order staying race-free forever (see BG-podresume for exactly
 * that bug, found and fixed on the podcast side of this same file format).
 * The old eyeball-ability the folder-local name gave up: this is now an
 * internal cache like cover.c's own art cache, not something a user is
 * expected to find and read on the card directly. */
#define AB_RESUME_FILE "/usr/data/audiobook_resume.txt"
#define AB_RESUME_KEEP 64

void ab_save_position(const ab_book_data_t *bk, int file_idx, long file_ms) {
    if (!bk->dir[0] || file_idx < 0 || file_idx >= bk->file_n || file_ms < 0) return;

    char (*keep)[AB_PATH_LEN + 32] = malloc(sizeof(*keep) * AB_RESUME_KEEP);
    if (!keep) return;
    int n = 0;
    FILE *f = fopen(AB_RESUME_FILE, "r");
    if (f) {
        char line[AB_PATH_LEN + 32];
        while (n < AB_RESUME_KEEP - 1 && fgets(line, sizeof(line), f)) {
            char probe[AB_PATH_LEN + 32];
            snprintf(probe, sizeof(probe), "%s", line);
            char *nl = strchr(probe, '\n');
            if (nl) *nl = '\0';
            char *t1 = strchr(probe, '\t');
            if (t1) {
                char *t2 = strchr(t1 + 1, '\t');
                if (t2 && strcmp(t2 + 1, bk->dir) == 0) continue;   /* replaced below */
            }
            snprintf(keep[n++], AB_PATH_LEN + 32, "%s", line);
        }
        fclose(f);
    }

    char tmp[sizeof(AB_RESUME_FILE) + 8];
    snprintf(tmp, sizeof(tmp), "%s.new", AB_RESUME_FILE);
    FILE *out = fopen(tmp, "w");
    if (!out) { free(keep); return; }
    int ok = 1;
    /* fputs alone, no fputc('\n') -- fgets above keeps the trailing newline
     * on every line it reads, so keep[i] already ends in one. Adding a
     * second wrote a blank line after every retained entry, and since a
     * blank line is itself read back and retained on the next save (it has
     * no tab, so the skip-this-book test never fires on it), the blanks
     * doubled on every autosave until they filled the AB_RESUME_KEEP budget
     * and started evicting real books. pod_resume_store() -- which this was
     * copied from -- gets this right; the extra fputc was mine. */
    for (int i = 0; i < n && ok; i++)
        if (fputs(keep[i], out) < 0) ok = 0;
    free(keep);
    if (ok && fprintf(out, "%d\t%ld\t%s\n", file_idx, file_ms, bk->dir) < 0) ok = 0;
    /* A partial write on power loss must not resume into garbage: write to a
     * temp name and rename over the old one, which is atomic on the same
     * filesystem, rather than truncate-then-write the file being read. */
    if (fclose(out) != 0) ok = 0;
    if (ok) rename(tmp, AB_RESUME_FILE);
    else unlink(tmp);
}

int ab_load_position(const char *dir, int *file_idx, long *file_ms) {
    FILE *f = fopen(AB_RESUME_FILE, "r");
    if (!f) return -1;
    char line[AB_PATH_LEN + 32];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        if (strcmp(t2 + 1, dir) != 0) continue;
        int fi = atoi(line);
        long ms = atol(t1 + 1);
        if (fi < 0 || ms < 0) continue;
        *file_idx = fi;
        *file_ms = ms;
        found = 0;
        break;
    }
    fclose(f);
    return found;
}
