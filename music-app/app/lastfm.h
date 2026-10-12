/* lastfm.h — R23: fall back to Last.fm for an album's cover art when
 * nothing local (embedded picture, folder image) was found. See lastfm.c
 * for the matching logic and why it's shaped the way it is. */
#ifndef LASTFM_H
#define LASTFM_H

/* Reads the API key/secret out of settings.txt once; a no-op on every call
 * after the first (whether or not a key was actually found -- a missing
 * key is a valid, stable answer, not a reason to keep re-reading the
 * file). Safe to call from any thread; only ever actually reads the file
 * once regardless of how many threads call it. */
void lastfm_load_config(void);

/* Whether settings.txt had a usable API key. Calls lastfm_load_config()
 * itself if that hasn't happened yet, so callers don't need to sequence
 * the two. */
int lastfm_has_key(void);

/* Looks up `artist`/`album` (the same album.getinfo call, and the same
 * "largest available image size" pick, Navidrome's Last.fm agent uses)
 * and saves the result to `dest_jpg` on success. Blocking -- does two
 * network round trips via a curl subprocess -- so only ever call this off
 * the UI thread. Returns 0 on success, -1 when Last.fm answered and has no
 * cover (album unknown, or known without an image, or no key to ask with),
 * and -2 when it could not be asked or did not answer properly: no network,
 * a timeout, one of its own 5xx replies, the image download failing. Only
 * -1 is worth remembering as "no cover"; -2 is "try again some other time".
 * cover_load() is what validates the saved file the next time it's read. */
#ifndef COVER_FETCH_NONE
#define COVER_FETCH_NONE  (-1)
#define COVER_FETCH_ERROR (-2)
#endif
int lastfm_fetch_cover(const char *artist, const char *album, const char *dest_jpg);

/* Artist page: artist.getinfo instead of album.getinfo, one call for both
 * a photo (same "largest image" pick as lastfm_fetch_cover()) and a bio
 * (bio.summary, HTML-unescaped and with the trailing "Read more on
 * Last.fm" link stripped). Either half can come back empty on its own --
 * an artist with a photo but no written bio is common -- so success/failure
 * is reported per-half rather than as one combined result: returns 1 if
 * dest_jpg was written, 2 if bio_out was filled, 3 if both were, 0 if the
 * call reached Last.fm but got neither, -1 if the call itself failed (no
 * key, no network, no match at all). Same blocking-network-call contract
 * as lastfm_fetch_cover() -- call off the UI thread only. */
int lastfm_fetch_artist(const char *artist, const char *dest_jpg,
                        char *bio_out, size_t bio_n);

#endif
