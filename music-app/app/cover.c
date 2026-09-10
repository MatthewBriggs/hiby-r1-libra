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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "vendor/jpeg9/jpeglib.h"
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
 * every existing cache entry (they simply become unreachable orphans,
 * cleaned up over time by prune_cache()'s normal LRU pruning). Bump this
 * again any time cover_load()'s pixel output changes. */
#define CACHE_VERSION "2"

static void cache_path(const char *key, int px, char *out, size_t n) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        h = ((h << 5) + h) ^ *p;
    for (const unsigned char *p = (const unsigned char *)CACHE_VERSION; *p; p++)
        h = ((h << 5) + h) ^ *p;
    mkdir(COVER_CACHE_DIR, 0755);
    snprintf(out, n, "%s/%08lx.%d.r565", COVER_CACHE_DIR, h & 0xFFFFFFFFul, px);
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
    return buf;
}

/* The cache is on the card, so it is not a threat to the device, but it still
 * grows by a third of a megabyte per album and nothing was ever removing any
 * of it. Keep a bounded number of the most recently used and drop the rest. */
#define COVER_CACHE_KEEP 120

static void prune_cache(void) {
    DIR *d = opendir(COVER_CACHE_DIR);
    if (!d) return;
    struct { char name[64]; time_t at; } ent[512];
    int n = 0;
    struct dirent *e;
    while (n < (int)(sizeof(ent) / sizeof(ent[0])) && (e = readdir(d))) {
        if (!strstr(e->d_name, ".r565")) continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", COVER_CACHE_DIR, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        snprintf(ent[n].name, sizeof(ent[n].name), "%s", e->d_name);
        ent[n].at = st.st_mtime;
        n++;
    }
    closedir(d);
    if (n <= COVER_CACHE_KEEP) return;

    /* Selection sort by age; n is small and this runs only when the cache is
     * already over its limit. */
    for (int i = 0; i < n - COVER_CACHE_KEEP; i++) {
        int oldest = i;
        for (int j = i + 1; j < n; j++)
            if (ent[j].at < ent[oldest].at) oldest = j;
        if (oldest != i) {
            typeof(ent[0]) t = ent[i]; ent[i] = ent[oldest]; ent[oldest] = t;
        }
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", COVER_CACHE_DIR, ent[i].name);
        unlink(p);
    }
}

static void save_cache(const char *key, int px, const uint16_t *buf) {
    char p[512];
    cache_path(key, px, p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite(buf, sizeof(uint16_t), (size_t)px * px, f);
    fclose(f);
    prune_cache();
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
int cover_downscale_max(const char *jpeg_path, int max_dim) {
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

    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-shrink", jpeg_path);
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

    if (rename(tmp_path, jpeg_path) != 0) { unlink(tmp_path); return -1; }
    return 0;
}
