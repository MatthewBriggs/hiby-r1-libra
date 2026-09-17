/* cover.c — feed cover art: JPEG decode to RGB565, cached on the card.
 *
 * The device ships libjpeg 9 (/usr/lib/libjpeg.so.9) but no headers, and it is
 * dlopen'd rather than linked so a missing or mismatched library degrades to
 * "no cover" instead of failing to load the app. That means the struct layout
 * has to be described here, which is why libjpeg 9's own headers are vendored:
 * jpeg_CreateDecompress validates sizeof(struct jpeg_decompress_struct) and
 * refuses to run if it disagrees, so a wrong header fails safely but silently.
 *
 * Decoding a 3000x3000 cover on a 56 MB device is not free, so libjpeg's
 * scale_denom does the downscale during decode (it can skip most of the IDCT
 * work), and the result is cached next to the cover as a raw RGB565 blob.
 *
 * Cover art is untrusted input — whatever the podcast host chose to publish —
 * and a 3000x3000 *progressive* cover was enough to get hiby_player killed by
 * the OOM killer. Scanline streaming below keeps baseline JPEGs cheap at any
 * size, but progressive decoding cannot stream: jpeg_start_decompress builds
 * the whole coefficient array before it will yield a single line, and
 * scale_denom shrinks only the output, not that array. At 3000x3000x3 that is
 * ~54 MB on a device with about 18 MB free. So the header is checked first and
 * anything whose working set would not fit is declined; the feed loses its
 * thumbnail instead of the player losing its life.
 */

#include <dlfcn.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>          /* AT_FDCWD, for the atime use-clock below */
#include <sys/syscall.h>    /* SYS_gettid, for per-thread shrink temp names */
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vendor/jpeg9/jpeglib.h"
#include "vendor/miniz/miniz.h"
#include "cover.h"

typedef void (*pfn_create)(j_decompress_ptr, int, size_t);
typedef struct jpeg_error_mgr *(*pfn_std_error)(struct jpeg_error_mgr *);

static void *g_lib;
static pfn_create    x_create;
static pfn_std_error x_std_error;
static void (*x_stdio_src)(j_decompress_ptr, FILE *);
static int  (*x_read_header)(j_decompress_ptr, boolean);
static boolean (*x_start)(j_decompress_ptr);
static JDIMENSION (*x_read_scanlines)(j_decompress_ptr, JSAMPARRAY, JDIMENSION);
static boolean (*x_finish)(j_decompress_ptr);
static void (*x_destroy)(j_decompress_ptr);
static void (*x_calc_dims)(j_decompress_ptr);

/* BG104: compress-side symbols, loaded separately from (and lazily after)
 * the decompress ones above -- cover_load() must keep working even on a
 * hypothetical build whose libjpeg.so.9 is missing these, and tying them
 * into the same all-or-nothing load_lib() would make a compress-only
 * problem take decoding down with it for no reason. */
static void (*xc_create)(j_compress_ptr, int, size_t);
static void (*xc_destroy)(j_compress_ptr);
static void (*xc_stdio_dest)(j_compress_ptr, FILE *);
static void (*xc_set_defaults)(j_compress_ptr);
static void (*xc_set_quality)(j_compress_ptr, int, boolean);
static void (*xc_start)(j_compress_ptr, boolean);
static JDIMENSION (*xc_write_scanlines)(j_compress_ptr, JSAMPARRAY, JDIMENSION);
static void (*xc_finish)(j_compress_ptr);

/* Was 8 MB, which rejected a perfectly ordinary 1400x1400 progressive cover
 * (Apple's own documented *minimum* recommended podcast artwork size) --
 * its coefficient array alone is 1400*1400*3*sizeof(JCOEF) = ~11.2 MB,
 * comfortably over the old budget. 14 MB admits that (and a bit more
 * headroom for slight variations) while staying nowhere near the
 * documented OOM case this budget exists to prevent: 3000x3000 needs
 * ~54 MB, still refused by a wide margin. Still roughly half the free
 * memory on an idle device, so a decode cannot crowd out the player even
 * at the worst moment; anything larger loses its thumbnail. */
#define COVER_MEM_BUDGET  (14 * 1024 * 1024)
#define COVER_MAX_DIM     8000

static int g_tried;

static int load_lib(void) {
    if (g_tried) return g_lib != NULL;
    g_tried = 1;
    g_lib = dlopen("libjpeg.so.9", RTLD_NOW);
    if (!g_lib) g_lib = dlopen("/usr/lib/libjpeg.so.9", RTLD_NOW);
    if (!g_lib) g_lib = dlopen("libjpeg.so", RTLD_NOW);
    if (!g_lib) return 0;

    x_create        = (pfn_create)dlsym(g_lib, "jpeg_CreateDecompress");
    x_std_error     = (pfn_std_error)dlsym(g_lib, "jpeg_std_error");
    x_stdio_src     = (void (*)(j_decompress_ptr, FILE *))dlsym(g_lib, "jpeg_stdio_src");
    x_read_header   = (int (*)(j_decompress_ptr, boolean))dlsym(g_lib, "jpeg_read_header");
    x_start         = (boolean (*)(j_decompress_ptr))dlsym(g_lib, "jpeg_start_decompress");
    x_read_scanlines= (JDIMENSION (*)(j_decompress_ptr, JSAMPARRAY, JDIMENSION))dlsym(g_lib, "jpeg_read_scanlines");
    x_finish        = (boolean (*)(j_decompress_ptr))dlsym(g_lib, "jpeg_finish_decompress");
    x_destroy       = (void (*)(j_decompress_ptr))dlsym(g_lib, "jpeg_destroy_decompress");
    x_calc_dims     = (void (*)(j_decompress_ptr))dlsym(g_lib, "jpeg_calc_output_dimensions");

    if (!x_create || !x_std_error || !x_stdio_src || !x_read_header ||
        !x_start || !x_read_scanlines || !x_finish || !x_destroy) {
        dlclose(g_lib);
        g_lib = NULL;
        return 0;
    }
    return 1;
}

static int g_compress_tried;

/* Off the same g_lib handle load_lib() already opened -- calling load_lib()
 * first both ensures g_lib exists and reuses its own "is the library there
 * at all" failure path, rather than duplicating a second dlopen. */
static int load_lib_compress(void) {
    if (g_compress_tried) return xc_create != NULL;
    g_compress_tried = 1;
    if (!load_lib()) return 0;

    xc_create        = (void (*)(j_compress_ptr, int, size_t))dlsym(g_lib, "jpeg_CreateCompress");
    xc_destroy       = (void (*)(j_compress_ptr))dlsym(g_lib, "jpeg_destroy_compress");
    xc_stdio_dest    = (void (*)(j_compress_ptr, FILE *))dlsym(g_lib, "jpeg_stdio_dest");
    xc_set_defaults  = (void (*)(j_compress_ptr))dlsym(g_lib, "jpeg_set_defaults");
    xc_set_quality   = (void (*)(j_compress_ptr, int, boolean))dlsym(g_lib, "jpeg_set_quality");
    xc_start         = (void (*)(j_compress_ptr, boolean))dlsym(g_lib, "jpeg_start_compress");
    xc_write_scanlines = (JDIMENSION (*)(j_compress_ptr, JSAMPARRAY, JDIMENSION))dlsym(g_lib, "jpeg_write_scanlines");
    xc_finish        = (void (*)(j_compress_ptr))dlsym(g_lib, "jpeg_finish_compress");

    if (!xc_create || !xc_destroy || !xc_stdio_dest || !xc_set_defaults ||
        !xc_set_quality || !xc_start || !xc_write_scanlines || !xc_finish) {
        xc_create = NULL;   /* the load_lib_compress() != NULL check above */
        return 0;
    }
    return 1;
}

/* libjpeg's default error handler calls exit(); jump out instead. */
struct jump_err {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void on_error(j_common_ptr cinfo) {
    struct jump_err *e = (struct jump_err *)cinfo->err;
    longjmp(e->jump, 1);
}

static void on_message(j_common_ptr cinfo) { (void)cinfo; }   /* stay quiet */

/* The podcast app caches the scaled bitmap beside the JPEG it came from. That
 * will not do here: the JPEGs are in the user's own album folders, and leaving
 * a .r565 in every one of them is vandalism.
 *
 * Nor can the cache live on /usr/data, which is a 36 MB partition with about
 * 27 MB free. One bitmap is a third of a megabyte and this library has 293
 * albums; caching them internally would fill the device. It goes on the card,
 * which is where the music is — if the card is out there is nothing to play
 * anyway. */
#define COVER_CACHE_DIR "/data/mnt/sd_0/.music_covers"

/* Folded into the hash below so a change to what cover_load() actually
 * produces -- like BG46's center-crop fix -- can't go on serving stale
 * pre-change bitmaps forever. The staleness check in cover_cached()/
 * load_cache() only compares against the source JPEG/folder's own mtime,
 * which has no way to know the *code* that turned those bytes into pixels
 * changed underneath it; a bumped version here is what actually invalidates
 * every existing cache entry (they simply become unreachable orphans).
 * Reclaiming the space those orphans take is cache_version_sweep()'s job --
 * it used to fall out of prune_cache() evicting everything within a few days,
 * which stopped being true once the cap was raised to cover the library. Bump
 * this again any time cover_load()'s pixel output changes. */
#define CACHE_VERSION "2"

/* Bumping CACHE_VERSION used to rely on prune_cache() to clear out the entries
 * it orphaned, which worked only because the cap was small enough that
 * everything got evicted within a few days. With the cap now set to cover the
 * whole library (see COVER_CACHE_KEEP) eviction is meant never to fire, so
 * those orphans would sit on the card forever -- hundreds of megabytes of
 * bitmaps nothing can ever reach again. A stamp file makes the version
 * explicit: when it disagrees with the running build, the directory is cleared
 * once and the stamp rewritten.
 *
 * Racing art workers can both see a stale stamp and both wipe; the cost is one
 * extra decode, not corruption, and only on the first access after an update. */
static void cache_version_sweep(void) {
    static int done;
    if (done) return;
    done = 1;

    char stamp[512];
    snprintf(stamp, sizeof(stamp), "%s/.version", COVER_CACHE_DIR);

    char have[32] = "";
    FILE *f = fopen(stamp, "r");
    int stamped = 0;
    if (f) {
        stamped = fgets(have, sizeof(have), f) != NULL;
        fclose(f);
    }
    have[strcspn(have, "\r\n")] = '\0';
    if (stamped && strcmp(have, CACHE_VERSION) == 0) return;

    /* No stamp at all means a cache written before this check existed, not a
     * stale one: its entries were hashed with whatever CACHE_VERSION was
     * current when they were written, so any that disagree with this build are
     * already unreachable by construction and the rest are still good. Adopt
     * the version instead of wiping -- a wipe here would throw away a full
     * cache of valid bitmaps and re-decode every one of them for nothing. */
    if (stamped) {
        DIR *d = opendir(COVER_CACHE_DIR);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strstr(e->d_name, ".r565")) continue;
                char p[512];
                snprintf(p, sizeof(p), "%s/%s", COVER_CACHE_DIR, e->d_name);
                unlink(p);
            }
            closedir(d);
        }
    }
    if ((f = fopen(stamp, "w"))) {
        fputs(CACHE_VERSION "\n", f);
        fclose(f);
    }
}

static void cache_path(const char *key, int px, char *out, size_t n) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        h = ((h << 5) + h) ^ *p;
    for (const unsigned char *p = (const unsigned char *)CACHE_VERSION; *p; p++)
        h = ((h << 5) + h) ^ *p;
    mkdir(COVER_CACHE_DIR, 0755);
    cache_version_sweep();
    snprintf(out, n, "%s/%08lx.%d.r565", COVER_CACHE_DIR, h & 0xFFFFFFFFul, px);
}

/* Eviction order used to come from the cache entry's own mtime, which is its
 * *write* time and never moves again -- so "keep the most recently used" was
 * really FIFO, and a cover opened every day could be dropped in favour of one
 * seen once. mtime cannot double as the use clock, because load_cache() and
 * cover_cached() compare it against the *source's* mtime to notice a
 * hand-replaced cover.jpg (the documented way to fix art the decoder refuses).
 * So the use clock rides on atime, which nothing else here reads.
 *
 * Measured on this device's exFAT mount, with a test binary doing exactly the
 * call below: atime moves and mtime holds, so the driver really does keep the
 * two apart (busybox `touch -a` moving both is a busybox limitation, not a
 * filesystem one). It is still checked at runtime rather than trusted, because
 * the failure mode is nasty: if setting atime dragged mtime forward, every
 * entry would look newer than its source and a stale cover would be pinned
 * forever -- far worse than the FIFO eviction this replaces. On the first call
 * the old mtime is read back, and if it moved it is put back and the use clock
 * is abandoned for the rest of the session.
 *
 * What this mount does *not* do is persist atime across a remount: entries
 * written before a reboot come back reading 1980 while their mtime survives
 * intact. So this is a within-session use clock, not a durable one, which is
 * why prune_cache() sorts on whichever of the two timestamps is later rather
 * than on atime alone. */
static int atime_lru = -1;   /* -1 = not yet probed, 1 = usable, 0 = abandoned */

static void touch_used(const char *path) {
    if (atime_lru == 0) return;

    struct timespec ts[2];
    ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_NOW;    /* atime := now */
    ts[1].tv_sec = 0; ts[1].tv_nsec = UTIME_OMIT;   /* mtime untouched */

    if (atime_lru == 1) {
        utimensat(AT_FDCWD, path, ts, 0);
        return;
    }

    struct stat before, after;
    if (stat(path, &before) != 0) return;
    if (utimensat(AT_FDCWD, path, ts, 0) != 0) { atime_lru = 0; return; }
    if (stat(path, &after) == 0 && after.st_mtime != before.st_mtime) {
        struct timespec fix[2];
        fix[0].tv_sec = 0;               fix[0].tv_nsec = UTIME_OMIT;
        fix[1].tv_sec = before.st_mtime; fix[1].tv_nsec = 0;
        utimensat(AT_FDCWD, path, fix, 0);
        atime_lru = 0;
        return;
    }
    atime_lru = 1;
}

static uint16_t *load_cache(const char *jpg, const char *key, int px) {
    char p[512];
    cache_path(key, px, p, sizeof(p));

    /* Replacing cover.jpg by hand is the documented way to give a feed art the
     * decoder refuses (see the progressive-JPEG limit), so a cache older than
     * the file it came from has to lose. */
    /* Only meaningful for artwork that lives somewhere permanent. Art
     * extracted from inside a music file is decoded from a scratch file in
     * /tmp that is rewritten every time, and comparing against that would
     * throw the cache away on every single play. */
    struct stat cs, js;
    if (strncmp(jpg, "/tmp/", 5) != 0 &&
        stat(p, &cs) == 0 && stat(jpg, &js) == 0 && js.st_mtime > cs.st_mtime) {
        unlink(p);
        return NULL;
    }

    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t want = (size_t)px * px;
    uint16_t *buf = malloc(want * sizeof(uint16_t));
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, sizeof(uint16_t), want, f);
    fclose(f);
    if (got != want) { free(buf); return NULL; }
    touch_used(p);
    return buf;
}

/* The cache is on the card, so it is not a threat to the device, but it still
 * grows by a third of a megabyte per album and nothing was ever removing any
 * of it. Keep a bounded number of the most recently used and drop the rest.
 *
 * 120 was far too tight, and measurably so: every entry is a 480px square
 * (ART_PX is FB_W -- the only size anything here ever asks for) at 450 KB, the
 * cache sat pinned at exactly 120 files / 60.3 MB, and the card holds ~600
 * media directories. So roughly a fifth of the library fit and the rest
 * evicted continuously -- and a miss is expensive: re-find the artwork, which
 * for embedded art means parsing the audio file again and spilling ~1.5 MB to
 * /tmp, then a full JPEG decode. That is exactly the "loads artwork a lot"
 * this cache exists to prevent, caused by the cache rather than survived by
 * it.
 *
 * The honest budget here is card space, not memory: the bitmaps never sit in
 * RAM together, and the cache lives on the music card by design (see
 * COVER_CACHE_DIR). 2000 entries is ~900 MB of a 477 GB card -- 0.2% -- and
 * comfortably more than any library this device can hold the music for. Sized
 * to cover the library rather than to be small, because an eviction that never
 * fires is the cheapest eviction there is; the cap stays only as a backstop
 * against genuinely unbounded growth. */
#define COVER_CACHE_KEEP 2000

/* Has to exceed COVER_CACHE_KEEP, or the readdir loop below fills the array
 * with the first COVER_PRUNE_MAX names it happens to see, finds that no more
 * than the cap were collected, and returns having deleted nothing -- the cache
 * would then grow without limit and silently. Kept well clear of the cap so
 * the overshoot COVER_PRUNE_EVERY allows still leaves room. */
#define COVER_PRUNE_MAX  4096

/* Sweeping the directory and stat()ing every entry was affordable on each save
 * at 120 files; at 2000 it is 2000 stat()s on a single-core MIPS to usually
 * discover there is nothing to do. The cap is a disk-space bound, not a
 * correctness one, so overshooting it by a few dozen entries between sweeps
 * costs nothing that matters. */
#define COVER_PRUNE_EVERY 64

static void prune_cache(void) {
    DIR *d = opendir(COVER_CACHE_DIR);
    if (!d) return;

    /* ~290 KB, so it is malloc'd rather than put on an art worker's stack. */
    struct ent { char name[64]; time_t at; };
    struct ent *ent = malloc((size_t)COVER_PRUNE_MAX * sizeof(*ent));
    if (!ent) { closedir(d); return; }

    int n = 0;
    struct dirent *e;
    while (n < COVER_PRUNE_MAX && (e = readdir(d))) {
        if (!strstr(e->d_name, ".r565")) continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", COVER_CACHE_DIR, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        snprintf(ent[n].name, sizeof(ent[n].name), "%s", e->d_name);
        /* Whichever is later, for the reason in touch_used(): atime is the use
         * clock but does not survive a remount, so an entry not read since
         * boot reads as 1980 and mtime (its write time) is the better signal
         * for it. Taking the max means a read this session always wins, and
         * anything else falls back to the write-order this used to use --
         * never worse than the old behaviour, whatever the mount has done to
         * the atimes. */
        ent[n].at = st.st_atime > st.st_mtime ? st.st_atime : st.st_mtime;
        n++;
    }
    closedir(d);
    if (n <= COVER_CACHE_KEEP) { free(ent); return; }

    /* Selection sort by age. Only the entries actually being dropped are
     * selected, so this is (n - KEEP) passes, not a full sort, and it runs
     * only once the cache is already over its limit. */
    for (int i = 0; i < n - COVER_CACHE_KEEP; i++) {
        int oldest = i;
        for (int j = i + 1; j < n; j++)
            if (ent[j].at < ent[oldest].at) oldest = j;
        if (oldest != i) {
            struct ent t = ent[i]; ent[i] = ent[oldest]; ent[oldest] = t;
        }
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", COVER_CACHE_DIR, ent[i].name);
        unlink(p);
    }
    free(ent);
}

static void save_cache(const char *key, int px, const uint16_t *buf) {
    char p[512];
    cache_path(key, px, p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite(buf, sizeof(uint16_t), (size_t)px * px, f);
    fclose(f);

    static int saves;
    if (++saves >= COVER_PRUNE_EVERY) {
        saves = 0;
        prune_cache();
    }
}

uint16_t *cover_cached(const char *cache_key, const char *dir, int px) {
    if (px <= 0 || px > 512 || !cache_key) return NULL;
    char p[512];
    cache_path(cache_key, px, p, sizeof(p));

    /* Same staleness rule load_cache() applies to a JPEG, against the folder
     * instead: adding or replacing artwork in an album directory bumps the
     * directory's own mtime, so a cache entry older than the folder is thrown
     * away rather than pinning yesterday's cover forever. Replacing a file
     * fully in place on exFAT may not move the folder's mtime -- that case
     * still resolves on the next cache prune, or by deleting the entry. */
    struct stat cs, ds;
    if (stat(p, &cs) != 0) return NULL;
    if (dir && stat(dir, &ds) == 0 && ds.st_mtime > cs.st_mtime) {
        unlink(p);
        return NULL;
    }

    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t want = (size_t)px * px;
    uint16_t *buf = malloc(want * sizeof(uint16_t));
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, sizeof(uint16_t), want, f);
    fclose(f);
    if (got != want) { free(buf); return NULL; }
    touch_used(p);
    return buf;
}

/* use_cache == 0 is the "refresh this cover" path: decode from the source
 * again and overwrite whatever the cache holds. The cache is otherwise
 * authoritative and has no expiry beyond its mtime check, so a bad entry --
 * one written before a decode bug was fixed, say -- would otherwise be
 * pinned forever with no way for the reader to say "no, fetch it again". */
static uint16_t *cover_load_ex(const char *jpeg_path, const char *cache_key,
                               int px, int use_cache) {
    if (px <= 0 || px > 512) return NULL;
    if (!cache_key) cache_key = jpeg_path;

    uint16_t *cached = use_cache ? load_cache(jpeg_path, cache_key, px) : NULL;
    if (cached) return cached;
    if (!load_lib()) return NULL;

    FILE *f = fopen(jpeg_path, "rb");
    if (!f) return NULL;

    struct jpeg_decompress_struct cinfo;
    struct jump_err jerr;
    /* volatile: both are assigned after the setjmp() below and freed in
     * its handler, and a non-volatile local modified between setjmp()
     * and longjmp() has an indeterminate value once the jump lands
     * (C99 7.13.2.1) -- the compiler is free to keep it in a register
     * that longjmp() restores to what it held at setjmp() time. The
     * handler would then free a stale NULL and leak the buffers, or
     * free something indeterminate. Not academic here: the zero-scanline
     * rejection added for this device's libjpeg defect longjmps on
     * purpose, so this path runs whenever a cover fails to decode, and
     * `out` is px*px*2 bytes -- 460 KB at ART_PX. */
    uint16_t * volatile out = NULL;
    JSAMPLE * volatile row = NULL;

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = x_std_error(&jerr.pub);
    jerr.pub.error_exit = on_error;
    jerr.pub.output_message = on_message;

    if (setjmp(jerr.jump)) {
        if (x_destroy) x_destroy(&cinfo);
        free(row);
        free(out);
        fclose(f);
        return NULL;
    }

    x_create(&cinfo, JPEG_LIB_VERSION, sizeof(struct jpeg_decompress_struct));
    x_stdio_src(&cinfo, f);
    x_read_header(&cinfo, TRUE);

    /* Refuse what will not fit before libjpeg tries to allocate it. A corrupt
     * or absurd SOF is caught by the dimension cap; a progressive image is
     * charged for its full coefficient array, which is the cost scale_denom
     * cannot avoid. */
    if (cinfo.image_width  > COVER_MAX_DIM ||
        cinfo.image_height > COVER_MAX_DIM ||
        cinfo.image_width == 0 || cinfo.image_height == 0)
        longjmp(jerr.jump, 1);

    /* BG46 follow-up: decline a source smaller than the target in either
     * dimension rather than upscale it. Found live on a real file (a
     * podcast episode's 320x320 embedded thumbnail, blown up to this
     * screen's 480x480 Now Playing art): confirmed via direct instrumented
     * logging of raw decoded bytes that this device's libjpeg9 returned
     * literal zero-byte scanlines for whole interior row-bands on that
     * file, with jpeg_read_scanlines still reporting success throughout
     * and no warning raised -- a genuine device-library decode defect, not
     * a bug in this file's own box-filter math (which was separately
     * audited and fixed for a real off-by-something in the same session,
     * but did not explain this). Declining it here falls through to
     * art_candidate()'s next slot -- typically the feed's own folder
     * image, usually higher-resolution than an embedded thumbnail anyway
     * -- with no extra wiring needed, since the caller's retry loop
     * already treats a NULL cover_load() as "try the next candidate". A
     * blown-up-1.5x thumbnail was never going to look sharp regardless of
     * the device bug, so this is the right call on image-quality grounds
     * even for a file that wouldn't have hit it. */
    if ((int)cinfo.image_width < px || (int)cinfo.image_height < px)
        longjmp(jerr.jump, 1);

    if (cinfo.progressive_mode) {
        int64_t coeffs = (int64_t)cinfo.image_width * cinfo.image_height *
                         (cinfo.num_components > 0 ? cinfo.num_components : 3) *
                         (int64_t)sizeof(JCOEF);
        if (coeffs > COVER_MEM_BUDGET) longjmp(jerr.jump, 1);
    }

    /* Ask for the smallest decode that still covers the target size; libjpeg
     * supports N/8 scaling and skips most of the work for small denominators. */
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    for (int d = 8; d >= 1; d--) {
        if ((int)(cinfo.image_width / d) >= px && (int)(cinfo.image_height / d) >= px) {
            cinfo.scale_denom = d;
            break;
        }
    }
    cinfo.out_color_space = JCS_RGB;
    cinfo.do_fancy_upsampling = FALSE;
    if (x_calc_dims) x_calc_dims(&cinfo);

    x_start(&cinfo);

    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    int comps = cinfo.output_components;
    if (w <= 0 || h <= 0 || comps < 1) longjmp(jerr.jump, 1);

    out = malloc((size_t)px * px * sizeof(uint16_t));
    row = malloc((size_t)w * comps);
    if (!out || !row) longjmp(jerr.jump, 1);

    /* Box-filtered (area-average) down to px*px as scanlines arrive, so the
     * full decoded image never has to be held in memory -- only the source
     * rows contributing to the output row currently being built. This used
     * to be nearest-neighbour (one sample picked per destination pixel,
     * mislabelled as "box" in this same comment) -- cheap, but it visibly
     * aliased fine text and sharp graphic edges on covers, exactly the kind
     * of high-contrast content typography-heavy album art has plenty of,
     * where a photo would have hidden the same shortcut. */
    long *racc = malloc((size_t)w * sizeof(long));
    long *gacc = malloc((size_t)w * sizeof(long));
    long *bacc = malloc((size_t)w * sizeof(long));
    if (!racc || !gacc || !bacc) {
        free(racc); free(gacc); free(bacc);
        longjmp(jerr.jump, 1);
    }

    /* BG46: center-crop to a square before the box-filter runs, rather than
     * mapping source width and source height independently to px*px, which
     * stretched each axis by a different factor whenever the source wasn't
     * already square. Album/book covers are conventionally square so this
     * never showed; podcast artwork routinely isn't. y_off rows of top
     * margin are read and discarded (scanlines can't be seeked backward);
     * x_off is folded into the column mapping below, no row cost either way. */
    int side = w < h ? w : h;
    int y_off = (h - side) / 2;
    int x_off = (w - side) / 2;
    for (int i = 0; i < y_off && cinfo.output_scanline < cinfo.output_height; i++) {
        JSAMPROW rp = row;
        x_read_scanlines(&cinfo, &rp, 1);
    }

    int next_src_row = 0;
    int rows = 0;        /* source rows currently folded into racc/gacc/bacc */
    int have_rows = 0;   /* whether racc/gacc/bacc hold anything real yet */
    /* BG46 follow-up 2: the same device-libjpeg defect documented above --
     * "literal zero-byte scanlines for whole interior row-bands...
     * jpeg_read_scanlines still reporting success throughout" -- but for a
     * source that passes the size check, so that guard alone doesn't
     * catch it. Confirmed live on a real file (Elbow's "Audio Vertigo"
     * cover): the top ~55% of Now Playing's art decoded correctly, the
     * rest sat at the plain grey placeholder colour, in a clean horizontal
     * split -- exactly what "a whole row-band came back as raw zero
     * bytes" would produce once box-filtered and blitted.
     *
     * Can't reject on "this row is black": a great many real covers
     * legitimately have large black regions (confirmed elsewhere this
     * session -- Elbow's own cover among them), and rejecting the first
     * such row would drop good decodes far more often than it catches bad
     * ones. What a real decode essentially never produces, even over a
     * near-black region, is *many consecutive scanlines byte-for-byte
     * identical to all-zero* -- DCT/quantization noise means genuine dark
     * content still varies pixel to pixel and row to row. A defect that
     * leaves the decoder's own output buffer untouched, on the other
     * hand, is uniformly, exactly zero with no noise at all. Long consecutive
     * run of that specific shape is treated as the defect and fails the
     * whole decode -- cover_load() returns NULL, and the caller already
     * treats that as "try the next art candidate" (a network-fetched cover,
     * typically), the same fallback BG46's own size-decline case above
     * already relies on. */
    /* BG46 follow-up 3: reported live -- a real photo (two people, both in
     * mostly-black clothing, against a dark wood background: Lisa
     * Batiashvili/Daniel Barenboim's "Tchaikovsky, Sibelius: Violin
     * Concertos") got wrongly rejected by this same check. 8 consecutive
     * rows is a trivial fraction of any real decode (under 2% of a 480px
     * one) -- a dark suit jacket or a shadow can easily span that with no
     * lighter pixel anywhere in it, with nothing wrong with the decode at
     * all. Scaled to the source instead of a small fixed count: the
     * confirmed real defect (Elbow's cover) blanked roughly half the
     * image, so a run has to cover a full third of the source's own
     * height before it's treated as the defect rather than legitimate
     * content -- high enough that genuine dark clothing/shadow, which
     * still has *some* lighter interruption (a collar, a button, a fold)
     * over that large a span in virtually any real photo, doesn't trip
     * it, while a true "this whole band came back blank" defect still
     * does. */
    int zero_run = 0;
    int zero_run_limit = side / 3;
    if (zero_run_limit < 40) zero_run_limit = 40;   /* floor for a small/thumbnail source */
    for (int y = 0; y < px; y++) {
        int row_end = (int)((int64_t)(y + 1) * side / px);
        if (row_end > side) row_end = side;

        /* Upscaling (side < px, an embedded thumbnail smaller than the
         * target size -- routine for a podcast episode's own art, unlike
         * the large album covers this box-filter was written for) means
         * row_end often does not advance past next_src_row for several
         * consecutive output rows in a row: several output rows legitimately
         * share the same single source row. The old code forced at least
         * one new source row to be read on *every* output row regardless
         * ("upscaling: >=1 row"), which is wrong -- it drained the source
         * roughly px/side times faster than it should, so the source ran
         * out with a third or more of the output still unwritten, and
         * those remaining rows silently divided a freshly-zeroed
         * accumulator by a forced rows=1, i.e. rendered solid black. The
         * fix: only start a new accumulation window (and only force a
         * single row's worth of real data) when there is genuinely new
         * source data to fold in, or nothing has been read yet at all;
         * otherwise simply reuse racc/gacc/bacc/rows as they already are
         * from the last output row that did read something. */
        if (row_end > next_src_row || !have_rows) {
            if (row_end <= next_src_row) row_end = next_src_row + 1;
            if (row_end > side) row_end = side;

            memset(racc, 0, (size_t)w * sizeof(long));
            memset(gacc, 0, (size_t)w * sizeof(long));
            memset(bacc, 0, (size_t)w * sizeof(long));
            rows = 0;
            while (next_src_row < row_end && cinfo.output_scanline < cinfo.output_height) {
                JSAMPROW rp = row;
                x_read_scanlines(&cinfo, &rp, 1);
                {
                    int all_zero = 1;
                    for (size_t i = 0; i < (size_t)w * comps; i++)
                        if (row[i] != 0) { all_zero = 0; break; }
                    zero_run = all_zero ? zero_run + 1 : 0;
                    if (zero_run >= zero_run_limit) {
                        free(racc); free(gacc); free(bacc);
                        longjmp(jerr.jump, 1);
                    }
                }
                for (int sx = 0; sx < w; sx++) {
                    const JSAMPLE *p = row + (size_t)sx * comps;
                    racc[sx] += p[0];
                    gacc[sx] += comps > 1 ? p[1] : p[0];
                    bacc[sx] += comps > 2 ? p[2] : p[0];
                }
                next_src_row++;
                rows++;
            }
            if (rows == 0) rows = 1;   /* source exhausted; average of nothing is 0, not a crash */
            have_rows = 1;
        }
        /* else: no new source row for this output row -- racc/gacc/bacc/
         * rows are exactly what the last output row that did read left
         * them as, which is exactly what an upscaled row should show. */

        for (int x = 0; x < px; x++) {
            int col_start = x_off + (int)((int64_t)x * side / px);
            int col_end = x_off + (int)((int64_t)(x + 1) * side / px);
            if (col_end <= col_start) col_end = col_start + 1;
            if (col_end > x_off + side) col_end = x_off + side;
            long rs = 0, gs = 0, bs = 0;
            for (int sx = col_start; sx < col_end; sx++) {
                rs += racc[sx]; gs += gacc[sx]; bs += bacc[sx];
            }
            int n = (col_end - col_start) * rows;
            int r = (int)(rs / n), g = (int)(gs / n), b = (int)(bs / n);
            out[(size_t)y * px + x] =
                (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
    free(racc); free(gacc); free(bacc);

    /* Drain anything left so finish_decompress does not complain. */
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rp = row;
        x_read_scanlines(&cinfo, &rp, 1);
    }
    x_finish(&cinfo);
    x_destroy(&cinfo);
    free(row);
    fclose(f);

    save_cache(cache_key, px, out);
    return out;
}

uint16_t *cover_load(const char *jpeg_path, const char *cache_key, int px) {
    return cover_load_ex(jpeg_path, cache_key, px, 1);
}

uint16_t *cover_load_fresh(const char *jpeg_path, const char *cache_key, int px) {
    return cover_load_ex(jpeg_path, cache_key, px, 0);
}

/* BG104: see cover.h's own comment -- shrink a network-fetched cover before
 * it ever reaches cover_load()'s own box filter, rather than trying to make
 * that filter itself behave better at a large reduction ratio. Two
 * independent decode/encode stages, each with its own setjmp scope: sharing
 * one across both halves would need extra bookkeeping to stop the shared
 * error handler from re-destroying/re-closing whichever half had already
 * finished cleanly by the time the other one failed. */
/* dst_path NULL shrinks jpeg_path in place (write-then-rename over the top of
 * it); otherwise the shrunk copy is written to dst_path and the source is left
 * exactly as it was.
 *
 * The second form exists because the caller's alternative was far worse:
 * cover_load_capped() used to copy a *full-size* cover.jpg byte-for-byte into
 * /tmp purely so it had something of its own to shrink in place -- and /tmp
 * here is tmpfs, so that copy is RAM, on a 57 MB device with a documented OOM
 * history (docs/06) and a 27.9 MB /tmp to fit it in. An oversized original
 * could therefore cost several megabytes of RAM before a single pixel was
 * decoded. Decoding straight to the destination skips that entirely: the only
 * thing that ever lands in /tmp is the already-shrunk result.
 *
 * Returns 1 if an output was written, 0 if the source was already within
 * max_dim (nothing written, dst_path untouched), -1 on error. */
static int downscale_impl(const char *jpeg_path, const char *dst_path, int max_dim) {
    if (max_dim <= 0) return -1;
    if (!load_lib()) return -1;

    FILE *f = fopen(jpeg_path, "rb");
    if (!f) return -1;

    struct jpeg_decompress_struct cinfo;
    struct jump_err jerr;
    /* volatile for the same reason as cover_load()'s own buffers above:
     * assigned after setjmp(), freed in its handler. */
    JSAMPLE * volatile row = NULL;
    JSAMPLE * volatile outbuf = NULL;
    long * volatile racc = NULL, * volatile gacc = NULL, * volatile bacc = NULL;
    int target_w = 0, target_h = 0;

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = x_std_error(&jerr.pub);
    jerr.pub.error_exit = on_error;
    jerr.pub.output_message = on_message;

    if (setjmp(jerr.jump)) {
        if (x_destroy) x_destroy(&cinfo);
        free(row); free(outbuf); free(racc); free(gacc); free(bacc);
        fclose(f);
        return -1;
    }

    x_create(&cinfo, JPEG_LIB_VERSION, sizeof(struct jpeg_decompress_struct));
    x_stdio_src(&cinfo, f);
    x_read_header(&cinfo, TRUE);

    if (cinfo.image_width > COVER_MAX_DIM || cinfo.image_height > COVER_MAX_DIM ||
        cinfo.image_width == 0 || cinfo.image_height == 0)
        longjmp(jerr.jump, 1);

    int src_w = (int)cinfo.image_width, src_h = (int)cinfo.image_height;
    if (src_w <= max_dim && src_h <= max_dim) {
        x_destroy(&cinfo);
        fclose(f);
        return 0;   /* already within bounds -- not an error, nothing to do */
    }

    if (cinfo.progressive_mode) {
        int64_t coeffs = (int64_t)src_w * src_h *
                         (cinfo.num_components > 0 ? cinfo.num_components : 3) *
                         (int64_t)sizeof(JCOEF);
        if (coeffs > COVER_MEM_BUDGET) longjmp(jerr.jump, 1);
    }

    /* Longer side becomes max_dim; the other keeps the source's own ratio --
     * unlike cover_load()'s square thumbnail, this is the actual cover.jpg
     * on disk, and cropping it on the way in would throw away real image
     * content every future viewer of this file gets, not just this app. */
    if (src_w >= src_h) {
        target_w = max_dim;
        target_h = (int)((int64_t)src_h * max_dim / src_w);
    } else {
        target_h = max_dim;
        target_w = (int)((int64_t)src_w * max_dim / src_h);
    }
    if (target_w < 1) target_w = 1;
    if (target_h < 1) target_h = 1;

    /* Same cheap pre-scale idea as cover_load(): ask libjpeg to skip most of
     * the IDCT work rather than decode at full size only to shrink it right
     * back down in the box filter below. */
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    for (int d = 8; d >= 1; d--) {
        if (src_w / d >= target_w && src_h / d >= target_h) {
            cinfo.scale_denom = d;
            break;
        }
    }
    cinfo.out_color_space = JCS_RGB;
    cinfo.do_fancy_upsampling = FALSE;
    if (x_calc_dims) x_calc_dims(&cinfo);
    x_start(&cinfo);

    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    int comps = cinfo.output_components;
    if (w <= 0 || h <= 0 || comps < 1) longjmp(jerr.jump, 1);

    row    = malloc((size_t)w * (size_t)comps);
    outbuf = malloc((size_t)target_w * (size_t)target_h * 3);
    racc   = malloc((size_t)w * sizeof(long));
    gacc   = malloc((size_t)w * sizeof(long));
    bacc   = malloc((size_t)w * sizeof(long));
    if (!row || !outbuf || !racc || !gacc || !bacc) longjmp(jerr.jump, 1);

    /* Same box-filter accumulation shape as cover_load() above, generalized
     * to an independent target_w/target_h rather than one shared px (and no
     * x_off/y_off crop -- see the comment above target_w/target_h). */
    int next_src_row = 0, rows = 0, have_rows = 0;
    for (int y = 0; y < target_h; y++) {
        int row_end = (int)((int64_t)(y + 1) * h / target_h);
        if (row_end > h) row_end = h;
        if (row_end > next_src_row || !have_rows) {
            if (row_end <= next_src_row) row_end = next_src_row + 1;
            if (row_end > h) row_end = h;
            memset(racc, 0, (size_t)w * sizeof(long));
            memset(gacc, 0, (size_t)w * sizeof(long));
            memset(bacc, 0, (size_t)w * sizeof(long));
            rows = 0;
            while (next_src_row < row_end && cinfo.output_scanline < cinfo.output_height) {
                JSAMPROW rp = row;
                x_read_scanlines(&cinfo, &rp, 1);
                for (int sx = 0; sx < w; sx++) {
                    const JSAMPLE *p = row + (size_t)sx * comps;
                    racc[sx] += p[0];
                    gacc[sx] += comps > 1 ? p[1] : p[0];
                    bacc[sx] += comps > 2 ? p[2] : p[0];
                }
                next_src_row++;
                rows++;
            }
            if (rows == 0) rows = 1;
            have_rows = 1;
        }
        JSAMPLE *orow = outbuf + (size_t)y * (size_t)target_w * 3;
        for (int x = 0; x < target_w; x++) {
            int col_start = (int)((int64_t)x * w / target_w);
            int col_end = (int)((int64_t)(x + 1) * w / target_w);
            if (col_end <= col_start) col_end = col_start + 1;
            if (col_end > w) col_end = w;
            long rs = 0, gs = 0, bs = 0;
            for (int sx = col_start; sx < col_end; sx++) {
                rs += racc[sx]; gs += gacc[sx]; bs += bacc[sx];
            }
            int n = (col_end - col_start) * rows;
            orow[x * 3 + 0] = (JSAMPLE)(rs / n);
            orow[x * 3 + 1] = (JSAMPLE)(gs / n);
            orow[x * 3 + 2] = (JSAMPLE)(bs / n);
        }
    }
    free(racc); free(gacc); free(bacc);
    racc = gacc = bacc = NULL;

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rp = row;
        x_read_scanlines(&cinfo, &rp, 1);
    }
    x_finish(&cinfo);
    x_destroy(&cinfo);
    fclose(f);
    free(row);
    row = NULL;

    /* ---- encode outbuf as a fresh baseline JPEG, write-then-rename ---- */
    if (!load_lib_compress()) { free(outbuf); return -1; }

    /* In place: via a sibling temp file so a failure or a kill part-way through
     * cannot leave a truncated JPEG where a good one was. Writing to a
     * caller-supplied destination needs none of that -- it is that caller's own
     * scratch path, nothing else reads it, and a -1 return tells them not to
     * use it.
     *
     * The thread id in the temp name is not decoration. Once this runs on art
     * in the user's own album folder, two art workers can be shrinking two
     * covers in the same directory at once -- or, on a track change, the same
     * cover twice -- and a single shared temp name would have them writing one
     * file while the other renamed it away. That is exactly the shared-scratch
     * collision that produced "art correct down to some horizontal line and
     * flat grey below it" (see cover_load_capped()'s own comment), except this
     * time the casualty would be a file of the user's rather than a scratch
     * copy. One temp path per thread cannot collide. */
    char tmp_path[600];
    if (dst_path) snprintf(tmp_path, sizeof(tmp_path), "%s", dst_path);
    else snprintf(tmp_path, sizeof(tmp_path), "%s.shrink%ld.tmp",
                  jpeg_path, (long)syscall(SYS_gettid));
    FILE *out_f = fopen(tmp_path, "wb");
    if (!out_f) { free(outbuf); return -1; }

    struct jpeg_compress_struct cout;
    struct jump_err jerr2;
    memset(&cout, 0, sizeof(cout));
    cout.err = x_std_error(&jerr2.pub);
    jerr2.pub.error_exit = on_error;
    jerr2.pub.output_message = on_message;

    if (setjmp(jerr2.jump)) {
        if (xc_destroy) xc_destroy(&cout);
        fclose(out_f);
        unlink(tmp_path);
        free(outbuf);
        return -1;
    }

    xc_create(&cout, JPEG_LIB_VERSION, sizeof(struct jpeg_compress_struct));
    xc_stdio_dest(&cout, out_f);
    cout.image_width = (JDIMENSION)target_w;
    cout.image_height = (JDIMENSION)target_h;
    cout.input_components = 3;
    cout.in_color_space = JCS_RGB;
    xc_set_defaults(&cout);
    xc_set_quality(&cout, 85, TRUE);
    xc_start(&cout, TRUE);
    while (cout.next_scanline < cout.image_height) {
        JSAMPROW rp = outbuf + (size_t)cout.next_scanline * (size_t)target_w * 3;
        xc_write_scanlines(&cout, &rp, 1);
    }
    xc_finish(&cout);
    xc_destroy(&cout);
    fclose(out_f);
    free(outbuf);

    if (!dst_path && rename(tmp_path, jpeg_path) != 0) { unlink(tmp_path); return -1; }
    return 1;
}

int cover_downscale_max(const char *jpeg_path, int max_dim) {
    int rc = downscale_impl(jpeg_path, NULL, max_dim);
    return rc < 0 ? -1 : 0;   /* no caller distinguishes "wrote" from "no-op" */
}

int cover_downscale_to(const char *src_path, const char *dst_path, int max_dim) {
    if (!src_path || !dst_path) return -1;
    return downscale_impl(src_path, dst_path, max_dim);
}

/* ---- PNG -> JPEG: just enough PNG to handle a station/album logo -------- */

#define PNG_MAX_FILE (8 * 1024 * 1024)
#define PNG_MAX_DIM  4000

/* Decodes exactly what's needed here and nothing more: 8-bit depth, color
 * type 2 (RGB) or 6 (RGBA), non-interlaced. Any real photo/logo a host
 * publishes for this purpose is one of those; a 16-bit, palette/greyscale,
 * or Adam7-interlaced PNG is declined rather than half-supported. IDAT
 * chunks are concatenated (a PNG encoder is free to split the compressed
 * stream across more than one) before the single zlib inflate -- mz_
 * uncompress() needs the whole stream at once, unlike tinfl's streaming
 * API, but these files are small enough (PNG_MAX_FILE) that holding all of
 * it is not a concern the way it would be for cover.c's own JPEG covers.
 * CRC32 checks are skipped: this app already trusts curl's own transport
 * security for these URLs, and a truncated/corrupt download fails anyway,
 * either at mz_uncompress() (wrong length) or by decoding to visible
 * garbage no worse than a bad photo would. */
static int png_decode_rgb(const unsigned char *buf, size_t len,
                          unsigned char **out_rgb, int *out_w, int *out_h) {
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (len < 8 || memcmp(buf, sig, 8) != 0) return -1;

    size_t p = 8;
    int width = 0, height = 0, bit_depth = 0, color_type = 0, interlace = 0, have_ihdr = 0;
    unsigned char *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    int rc = -1;

    while (p + 12 <= len) {
        uint32_t clen = ((uint32_t)buf[p] << 24) | ((uint32_t)buf[p + 1] << 16) |
                        ((uint32_t)buf[p + 2] << 8) | buf[p + 3];
        const unsigned char *ctype = buf + p + 4;
        size_t cstart = p + 8;
        if (clen > len || cstart + clen + 4 > len) break;

        if (!memcmp(ctype, "IHDR", 4) && clen >= 13) {
            width  = (int)(((uint32_t)buf[cstart] << 24) | ((uint32_t)buf[cstart + 1] << 16) |
                           ((uint32_t)buf[cstart + 2] << 8) | buf[cstart + 3]);
            height = (int)(((uint32_t)buf[cstart + 4] << 24) | ((uint32_t)buf[cstart + 5] << 16) |
                           ((uint32_t)buf[cstart + 6] << 8) | buf[cstart + 7]);
            bit_depth  = buf[cstart + 8];
            color_type = buf[cstart + 9];
            interlace  = buf[cstart + 12];
            have_ihdr = 1;
        } else if (!memcmp(ctype, "IDAT", 4)) {
            if (idat_len + clen > idat_cap) {
                size_t ncap = idat_cap ? idat_cap * 2 : 65536;
                while (ncap < idat_len + clen) ncap *= 2;
                unsigned char *n = realloc(idat, ncap);
                if (!n) goto out;
                idat = n; idat_cap = ncap;
            }
            memcpy(idat + idat_len, buf + cstart, clen);
            idat_len += clen;
        } else if (!memcmp(ctype, "IEND", 4)) {
            break;
        }
        p = cstart + clen + 4;
    }

    if (!have_ihdr || !idat || width <= 0 || height <= 0 ||
        width > PNG_MAX_DIM || height > PNG_MAX_DIM ||
        bit_depth != 8 || interlace != 0 ||
        (color_type != 2 && color_type != 6))
        goto out;

    {
        int channels = (color_type == 6) ? 4 : 3;
        size_t stride = (size_t)width * (size_t)channels + 1;
        size_t raw_len = stride * (size_t)height;
        unsigned char *raw = malloc(raw_len);
        if (!raw) goto out;

        mz_ulong dest_len = (mz_ulong)raw_len;
        int urc = mz_uncompress(raw, &dest_len, idat, (mz_ulong)idat_len);
        if (urc != MZ_OK || dest_len != raw_len) { free(raw); goto out; }

        /* PNG defilter, per-scanline, in place -- the standard five filter
         * types (None/Sub/Up/Average/Paeth), each referencing the already-
         * unfiltered byte to its left (a) and the row above (b, c). */
        unsigned char *prev = NULL;
        for (int y = 0; y < height; y++) {
            unsigned char *line = raw + (size_t)y * stride;
            int filt = line[0];
            unsigned char *cur = line + 1;
            size_t rowbytes = (size_t)width * (size_t)channels;
            for (size_t x = 0; x < rowbytes; x++) {
                int a = (x >= (size_t)channels) ? cur[x - (size_t)channels] : 0;
                int b = prev ? prev[x] : 0;
                int c = (prev && x >= (size_t)channels) ? prev[x - (size_t)channels] : 0;
                int v = cur[x];
                switch (filt) {
                    case 0: break;
                    case 1: v += a; break;
                    case 2: v += b; break;
                    case 3: v += (a + b) / 2; break;
                    case 4: {
                        int pp = a + b - c;
                        int pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c);
                        v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                        break;
                    }
                    default: free(raw); goto out;
                }
                cur[x] = (unsigned char)(v & 0xFF);
            }
            prev = cur;
        }

        unsigned char *rgb = malloc((size_t)width * (size_t)height * 3);
        if (!rgb) { free(raw); goto out; }
        for (int y = 0; y < height; y++) {
            const unsigned char *line = raw + (size_t)y * stride + 1;
            unsigned char *orow = rgb + (size_t)y * (size_t)width * 3;
            for (int x = 0; x < width; x++) {
                orow[x * 3 + 0] = line[x * channels + 0];
                orow[x * 3 + 1] = line[x * channels + 1];
                orow[x * 3 + 2] = line[x * channels + 2];
            }
        }
        free(raw);
        *out_rgb = rgb; *out_w = width; *out_h = height;
        rc = 0;
    }

out:
    free(idat);
    return rc;
}

int cover_png_to_jpeg(const char *png_path, const char *jpeg_path) {
    FILE *f = fopen(png_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > PNG_MAX_FILE) { fclose(f); return -1; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return -1; }

    unsigned char *rgb = NULL;
    int w = 0, h = 0;
    int rc = png_decode_rgb(buf, (size_t)sz, &rgb, &w, &h);
    free(buf);
    if (rc != 0) return -1;

    if (!load_lib_compress()) { free(rgb); return -1; }

    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-png2jpg", jpeg_path);
    FILE *out_f = fopen(tmp_path, "wb");
    if (!out_f) { free(rgb); return -1; }

    struct jpeg_compress_struct cout;
    struct jump_err jerr2;
    memset(&cout, 0, sizeof(cout));
    cout.err = x_std_error(&jerr2.pub);
    jerr2.pub.error_exit = on_error;
    jerr2.pub.output_message = on_message;

    if (setjmp(jerr2.jump)) {
        if (xc_destroy) xc_destroy(&cout);
        fclose(out_f);
        unlink(tmp_path);
        free(rgb);
        return -1;
    }

    xc_create(&cout, JPEG_LIB_VERSION, sizeof(struct jpeg_compress_struct));
    xc_stdio_dest(&cout, out_f);
    cout.image_width = (JDIMENSION)w;
    cout.image_height = (JDIMENSION)h;
    cout.input_components = 3;
    cout.in_color_space = JCS_RGB;
    xc_set_defaults(&cout);
    xc_set_quality(&cout, 90, TRUE);
    xc_start(&cout, TRUE);
    while (cout.next_scanline < cout.image_height) {
        JSAMPROW rp = rgb + (size_t)cout.next_scanline * (size_t)w * 3;
        xc_write_scanlines(&cout, &rp, 1);
    }
    xc_finish(&cout);
    xc_destroy(&cout);
    fclose(out_f);
    free(rgb);

    if (rename(tmp_path, jpeg_path) != 0) { unlink(tmp_path); return -1; }
    return 0;
}
