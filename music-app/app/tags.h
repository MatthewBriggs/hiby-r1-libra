#ifndef TAGS_H
#define TAGS_H

/* Track, disc and title, in one file open -- prefer this over
 * the single-field wrappers below when more than one is needed, since each
 * of those opens the file independently. Any output pointer may be NULL to
 * skip that field; track/disc land at -1 and title is left untouched when
 * not found or not applicable to the container. Pass title_n 0 (or a NULL
 * title) to skip title entirely. */
void tag_read(const char *path, int *track, int *disc, char *title, unsigned title_n);
/* The same, plus the disc's own name (DISCSUBTITLE / ID3 TSST, "Pop" on a
 * box set's first disc); empty when the file does not name its disc. */
void tag_read_disc(const char *path, int *track, int *disc, char *title, unsigned title_n,
                   char *disc_title, unsigned dt_n);

/* The track number the file itself claims. Returns -1 when it does not say.
 * Handles FLAC VORBIS_COMMENT, MP4 trkn and ID3v2 TRCK. */
int tag_track_number(const char *path);
/* Disc number, for multi-disc sets. -1 when the file does not say. */
int tag_disc_number(const char *path);
/* The title the file states, for tracks the index never saw. 0 on success. */
int tag_title(const char *path, char *out, unsigned n);

/* Artist/album/album_artist/genre in one open, for the library scanner
 * (scanner.c) populating its own MEDIA_TABLE-shaped row -- track/disc/title
 * are covered by tag_read() above and not repeated here. Any output pointer
 * may be NULL to skip that field; a field left unset by the container (or
 * the container itself unsupported) comes back an empty string, never
 * uninitialized. Every buffer is NUL-terminated UTF-8 on return -- ID3v2's
 * ISO-8859-1/UTF-16 encodings are converted, not passed through raw. */
void tag_read_meta(const char *path, char *artist, unsigned artist_n,
                   char *album, unsigned album_n,
                   char *album_artist, unsigned aa_n,
                   char *genre, unsigned genre_n);
#endif
