/* spotify.h — R23: second-choice album art fallback, tried after Last.fm.
 * See spotify.c for why it needs its own token exchange and its own image
 * picker (Spotify orders images the opposite way Last.fm does). */
#ifndef SPOTIFY_H
#define SPOTIFY_H

/* Reads the client ID/secret out of settings.txt once; a no-op on every
 * call after the first, same convention as lastfm_load_config(). */
void spotify_load_config(void);

/* Whether settings.txt had a usable client ID and secret. Calls
 * spotify_load_config() itself if needed. */
int spotify_has_key(void);

/* Client-credentials token exchange, then a catalog search by artist+album,
 * taking the first (largest) image URL from the first result. Blocking --
 * two network round trips via a curl subprocess -- call off the UI thread
 * only. Returns 0 on success, and lastfm_fetch_cover()'s -1 (answered, no
 * matching album) or -2 (could not ask, or no proper answer). */
#ifndef COVER_FETCH_NONE
#define COVER_FETCH_NONE  (-1)
#define COVER_FETCH_ERROR (-2)
#endif
int spotify_fetch_cover(const char *artist, const char *album, const char *dest_jpg);

/* Artist page's photo fallback, tried after Last.fm the same way
 * spotify_fetch_cover() backs up lastfm_fetch_cover() -- a type=artist
 * search instead of type=album, same client-credentials token, same
 * largest-first image order. Spotify's Web API has no artist bio of any
 * kind (removed years ago, never replaced), so this is image-only; the
 * bio side of the artist page is Last.fm-only. Returns 0 on success, -1
 * on any failure. */
int spotify_fetch_artist_image(const char *artist, const char *dest_jpg);

#endif
