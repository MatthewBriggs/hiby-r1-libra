/* cover.h — feed cover art as an RGB565 square, or NULL. Caller frees. */
#ifndef COVER_H
#define COVER_H
#include <stdint.h>
/* cache_key identifies the artwork independently of where the JPEG happens to
 * live — extracted art is decoded from a scratch file that is the same path
 * every time, so the path cannot be the key. */
uint16_t *cover_load(const char *jpeg_path, const char *cache_key, int px);
/* As cover_load(), but ignores any cached bitmap and re-decodes from source,
 * replacing the cache entry. Backs the "hold the artwork to refresh it"
 * gesture, which exists so a cover cached wrongly can be corrected without
 * deleting files by hand. */
uint16_t *cover_load_fresh(const char *jpeg_path, const char *cache_key, int px);

/* The cached bitmap for `cache_key`, or NULL if there isn't one. Lets a caller
 * skip finding a JPEG at all when the decoded answer is already on the card —
 * which matters because the cheapest-looking candidate search is not cheap:
 * embedded art has to parse the audio file and spill it to /tmp before
 * cover_load() gets a chance to say "already cached". `dir` is the album
 * folder, or NULL to skip the staleness check; a cache entry older than the
 * folder loses, so dropping a cover.jpg in by hand still takes effect. */
uint16_t *cover_cached(const char *cache_key, const char *dir, int px);

/* BG104: shrinks jpeg_path in place (write-then-rename) if either dimension
 * exceeds max_dim, preserving aspect ratio -- a no-op, not an error, if the
 * file is already within bounds. Meant for a freshly network-fetched cover,
 * whose resolution is whatever the host chose to publish: a large one has
 * been confirmed live to cause a visible flicker on this device's panel
 * (see BACKLOG.md), most likely a downsampling artifact in cover_load()'s
 * own box filter that a large reduction ratio makes bad enough to be
 * visible. Returns 0 on success (shrunk or already fine), -1 on any
 * failure -- the original file is left untouched either way. */
int cover_downscale_max(const char *jpeg_path, int max_dim);
#endif
