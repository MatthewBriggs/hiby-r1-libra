/* tags.c — the track number, read from the file rather than guessed.
 *
 * The number was previously taken from the filename, which works for the half
 * of this library that is named "04 Title" and fails for the half that is not.
 * It matters more than a missing column suggests: it drives the sort order, so
 * a wrong guess does not leave an album unnumbered, it leaves it out of order.
 *
 * Four containers, each read in one pass for every field at once:
 *
 *   FLAC  metadata block type 4, VORBIS_COMMENT, "TRACKNUMBER=7" (or "7/12")
 *   Ogg   Opus and Vorbis: the same comments, in the stream's second packet
 *   MP4   moov/udta/meta/ilst items (trkn, disk, (c)nam...), wherever moov is
 *   MP3   ID3v2 frames, TRCK/TPOS text likewise possibly "7/12"
 *
 * Besides track, disc and title: the disc's own name (DISCSUBTITLE, ID3
 * TSST), and artist/album/album artist/genre for the scanner. Never the
 * audio: a picture or other long value is skipped, not read.
 *
 * tag_read() reads track, disc and (FLAC only) title in one file open and one
 * pass over the tag data, rather than three separate opens each re-scanning
 * the same block for a different key — found live scanning a 183-track box
 * set, where the per-open cost is what made opening the album slow. The
 * single-field tag_track_number()/tag_disc_number()/tag_title() wrappers
 * below stay for callers that only want one field.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tags.h"

/* "7" and "7/12" both mean seven — and so does "7" written as UTF-16, which is
 * what ID3v2.4 frequently uses: the text arrives as a BOM followed by digits
 * interleaved with NULs. Skipping those rather than stopping at the first
 * non-digit is the difference between reading the number and reading nothing. */
static int parse_num(const char *s, int len) {
    int v = -1;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x00 || c == 0xFF || c == 0xFE) continue;   /* NUL, or a BOM */
        if (c >= '0' && c <= '9') {
            if (v < 0) v = 0;
            v = v * 10 + (c - '0');
        } else if (v >= 0) {
            break;                                            /* "7/12" -> 7 */
        } else if (c != ' ') {
            break;                                            /* leading junk */
        }
    }
    return v;
}

static uint32_t rd32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t rd32le(const unsigned char *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

static uint32_t syncsafe(const unsigned char *p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

/* Everything one pass can fill. Any pointer may be NULL (or its size 0) to
 * leave that field alone; numbers start at -1 and strings empty, and the
 * first value a file states for each wins. */
typedef struct {
    int *track, *disc;
    char *title;      unsigned title_n;
    char *disc_title; unsigned dt_n;
    char *artist;     unsigned an;
    char *album;      unsigned bn;
    char *aartist;    unsigned aan;
    char *genre;      unsigned gn;
} tagset_t;

static void set_field(char *out, unsigned n, const char *s, int len) {
    if (!out || n < 2 || len <= 0 || out[0]) return;
    unsigned take = (unsigned)len < n - 1 ? (unsigned)len : n - 1;
    memcpy(out, s, take);
    out[take] = '\0';
}

static int key_is(const char *e, uint32_t len, const char *key) {
    size_t k = strlen(key);
    return len > k && !strncasecmp(e, key, k);
}

/* One "KEY=value" comment -- FLAC's VORBIS_COMMENT block and Ogg's comment
 * packet (Vorbis, Opus) share the format. The '=' in each key keeps ALBUM
 * from matching ALBUMARTIST. DISCSUBTITLE names a disc ("Pop", "Club"),
 * as Symphonium and foobar2000 show it. */
static void vc_entry(tagset_t *ts, const char *e, uint32_t len) {
    if (ts->track && *ts->track < 0 && key_is(e, len, "TRACKNUMBER="))
        *ts->track = parse_num(e + 12, (int)len - 12);
    else if (ts->disc && *ts->disc < 0 && key_is(e, len, "DISCNUMBER="))
        *ts->disc = parse_num(e + 11, (int)len - 11);
    else if (key_is(e, len, "TITLE="))
        set_field(ts->title, ts->title_n, e + 6, (int)len - 6);
    else if (key_is(e, len, "DISCSUBTITLE="))
        set_field(ts->disc_title, ts->dt_n, e + 13, (int)len - 13);
    else if (key_is(e, len, "ARTIST="))
        set_field(ts->artist, ts->an, e + 7, (int)len - 7);
    else if (key_is(e, len, "ALBUM="))
        set_field(ts->album, ts->bn, e + 6, (int)len - 6);
    else if (key_is(e, len, "ALBUMARTIST="))
        set_field(ts->aartist, ts->aan, e + 12, (int)len - 12);
    else if (key_is(e, len, "ALBUM ARTIST="))
        set_field(ts->aartist, ts->aan, e + 13, (int)len - 13);
    else if (key_is(e, len, "GENRE="))
        set_field(ts->genre, ts->gn, e + 6, (int)len - 6);
}

static void flac_all(FILE *f, tagset_t *ts) {
    unsigned char h[4];
    if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return;
    for (int guard = 0; guard < 64; guard++) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) return;
        int last = b[0] & 0x80, type = b[0] & 0x7F;
        uint32_t len = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        if (type == 4) {                       /* VORBIS_COMMENT */
            if (len > 1 << 20 || len < 4) return;
            unsigned char *v = malloc(len);
            if (!v) return;
            if (fread(v, 1, len, f) != len) { free(v); return; }
            uint32_t off = 4 + rd32le(v);      /* vendor string, then a count */
            if (off + 4 <= len) {
                uint32_t n = rd32le(v + off);
                off += 4;
                for (uint32_t i = 0; i < n && off + 4 <= len; i++) {
                    uint32_t clen = rd32le(v + off);
                    off += 4;
                    if (clen > len - off) break;
                    vc_entry(ts, (char *)v + off, clen);
                    off += clen;
                }
            }
            free(v);
            return;
        }
        if (last) return;
        if (fseek(f, (long)len, SEEK_CUR)) return;
    }
}

/* ---- Ogg: Opus and Vorbis ----------------------------------------------
 * The comment header is the stream's second packet, starting on its second
 * page and running on across as many pages as it needs -- an embedded
 * picture (METADATA_BLOCK_PICTURE) can make it hundreds of KB. Read as a
 * byte stream over the pages' payloads, so the short comments are read and
 * a long one is skipped without ever being held. Opus and Vorbis files were
 * not read at all before (2026-10-19): a box set's disc numbers and titles
 * all came back unknown. */
typedef struct {
    FILE *f;
    uint32_t serial;
    unsigned char buf[255 * 255];
    unsigned len, pos;
    int pages;
} ogg_rd_t;

static int ogg_page(ogg_rd_t *o) {
    for (;;) {
        unsigned char h[27];
        if (++o->pages > 4096 || fread(h, 1, 27, o->f) != 27 || memcmp(h, "OggS", 4)) return -1;
        int nseg = h[26];
        unsigned char seg[255];
        if (fread(seg, 1, (size_t)nseg, o->f) != (size_t)nseg) return -1;
        unsigned len = 0;
        for (int i = 0; i < nseg; i++) len += seg[i];
        uint32_t serial = rd32le(h + 14);
        if (o->pages > 1 && serial != o->serial) {     /* another stream's page */
            if (fseek(o->f, (long)len, SEEK_CUR)) return -1;
            continue;
        }
        o->serial = serial;
        if (fread(o->buf, 1, len, o->f) != len) return -1;
        o->len = len;
        o->pos = 0;
        return 0;
    }
}

/* n bytes of the stream into dst, or skipped when dst is NULL. */
static int ogg_take(ogg_rd_t *o, unsigned char *dst, uint32_t n) {
    while (n) {
        if (o->pos == o->len && ogg_page(o) != 0) return -1;
        unsigned k = o->len - o->pos;
        if (k > n) k = n;
        if (dst) { memcpy(dst, o->buf + o->pos, k); dst += k; }
        o->pos += k;
        n -= k;
    }
    return 0;
}

static void ogg_all(FILE *f, tagset_t *ts) {
    ogg_rd_t *o = calloc(1, sizeof(*o));
    if (!o) return;
    o->f = f;
    /* Page one is the identification header and nothing else. */
    if (ogg_page(o) != 0) { free(o); return; }
    o->pos = o->len;
    unsigned char m[8];
    if (ogg_take(o, m, 7) != 0) { free(o); return; }
    int ok = 0;
    if (!memcmp(m, "\x03vorbis", 7)) ok = 1;
    else if (!memcmp(m, "OpusTag", 7) && ogg_take(o, m + 7, 1) == 0 && m[7] == 's') ok = 1;
    unsigned char b4[4];
    if (ok && ogg_take(o, b4, 4) == 0 && ogg_take(o, NULL, rd32le(b4)) == 0 &&
        ogg_take(o, b4, 4) == 0) {
        uint32_t n = rd32le(b4);
        char e[1024];
        for (uint32_t i = 0; i < n && i < 10000; i++) {
            if (ogg_take(o, b4, 4) != 0) break;
            uint32_t clen = rd32le(b4);
            if (clen < sizeof(e)) {
                if (ogg_take(o, (unsigned char *)e, clen) != 0) break;
                vc_entry(ts, e, clen);
            } else if (ogg_take(o, NULL, clen) != 0) {   /* a picture, or the like */
                break;
            }
        }
    }
    free(o);
}

/* ---- MP4 ------------------------------------------------------------------
 * Tags live in moov/udta/meta/ilst, and moov is often at the END of the file
 * -- after the audio, as most encoders write it unless asked for
 * "faststart". This used to search only the first 256 KB, so such a file
 * gave no artist, album, track or disc at all (Joy Division's Heart & Soul,
 * listed under its folder's name, 2026-10-19). Now the atoms are walked,
 * seeking past the audio, and only ilst's small items are read. */
static int mp4_child(FILE *f, long from, long to, const char *want, long *body, long *end) {
    long at = from;
    while (at + 8 <= to) {
        unsigned char h[16];
        if (fseek(f, at, SEEK_SET) || fread(h, 1, 8, f) != 8) return -1;
        uint64_t size = rd32be(h);
        long hdr = 8;
        if (size == 1) {
            if (fread(h + 8, 1, 8, f) != 8) return -1;
            size = ((uint64_t)rd32be(h + 8) << 32) | rd32be(h + 12);
            hdr = 16;
        } else if (size == 0) {
            size = (uint64_t)(to - at);
        }
        if (size < (uint64_t)hdr || at + (long)size > to || at + (long)size < at) return -1;
        if (!memcmp(h + 4, want, 4)) {
            *body = at + hdr;
            *end = at + (long)size;
            return 0;
        }
        at += (long)size;
    }
    return -1;
}

static void mp4_item(FILE *f, tagset_t *ts, const unsigned char *name, long body, long end) {
    static const unsigned char T_NAM[4] = { 0xA9, 'n', 'a', 'm' };
    static const unsigned char T_ART[4] = { 0xA9, 'A', 'R', 'T' };
    static const unsigned char T_ALB[4] = { 0xA9, 'a', 'l', 'b' };
    static const unsigned char T_GEN[4] = { 0xA9, 'g', 'e', 'n' };
    long len = end - body;
    if (len <= 0 || len > 4096) return;         /* covr and the like: not wanted */
    unsigned char v[4096];
    if (fseek(f, body, SEEK_SET) || fread(v, 1, (size_t)len, f) != (size_t)len) return;
    /* The value: a 'data' box (8 header, 4 type, 4 locale, then the payload).
     * A freeform '----' item names itself first in 'mean' and 'name' boxes. */
    char fname[64] = "";
    long off = 0;
    while (off + 8 <= len) {
        uint32_t bs = rd32be(v + off);
        if (bs < 8 || off + (long)bs > len) return;
        if (!memcmp(v + off + 4, "name", 4) && bs > 12) {
            uint32_t n = bs - 12;
            if (n >= sizeof(fname)) n = sizeof(fname) - 1;
            memcpy(fname, v + off + 12, n);
            fname[n] = '\0';
        } else if (!memcmp(v + off + 4, "data", 4) && bs >= 16) {
            const char *p = (const char *)v + off + 16;
            int plen = (int)bs - 16;
            if (!memcmp(name, "trkn", 4) || !memcmp(name, "disk", 4)) {
                if (plen >= 4) {
                    int num = ((unsigned char)p[2] << 8) | (unsigned char)p[3];
                    int *out = name[0] == 't' ? ts->track : ts->disc;
                    if (out && *out < 0 && num > 0) *out = num;
                }
            } else if (!memcmp(name, T_NAM, 4)) set_field(ts->title, ts->title_n, p, plen);
            else if (!memcmp(name, T_ART, 4))   set_field(ts->artist, ts->an, p, plen);
            else if (!memcmp(name, T_ALB, 4))   set_field(ts->album, ts->bn, p, plen);
            else if (!memcmp(name, "aART", 4))  set_field(ts->aartist, ts->aan, p, plen);
            else if (!memcmp(name, T_GEN, 4))   set_field(ts->genre, ts->gn, p, plen);
            else if (!memcmp(name, "----", 4) && !strcasecmp(fname, "DISCSUBTITLE"))
                set_field(ts->disc_title, ts->dt_n, p, plen);
            return;
        }
        off += bs;
    }
}

static void mp4_all(FILE *f, tagset_t *ts) {
    if (fseek(f, 0, SEEK_END)) return;
    long size = ftell(f);
    long mb, me, ub, ue, eb, ee, ib, ie;
    if (mp4_child(f, 0, size, "moov", &mb, &me) != 0) return;
    if (mp4_child(f, mb, me, "udta", &ub, &ue) != 0) return;
    if (mp4_child(f, ub, ue, "meta", &eb, &ee) != 0) return;
    eb += 4;                                    /* a full box: version and flags */
    if (mp4_child(f, eb, ee, "ilst", &ib, &ie) != 0) return;
    long at = ib;
    for (int guard = 0; guard < 512 && at + 8 <= ie; guard++) {
        unsigned char h[8];
        if (fseek(f, at, SEEK_SET) || fread(h, 1, 8, f) != 8) return;
        uint32_t s = rd32be(h);
        if (s < 8 || at + (long)s > ie) return;
        mp4_item(f, ts, h + 4, at + 8, at + (long)s);
        at += s;
    }
}

/* ---- MP3: ID3v2 ------------------------------------------------------------ */

/* ID3v2 text frames carry an encoding byte (0=ISO-8859-1, 1=UTF-16+BOM,
 * 2=UTF-16BE no BOM, 3=UTF-8) ahead of the text itself -- unlike the numeric
 * frames above, this can't just skip non-digits, the bytes have to be
 * converted or a Latin-1 accented character comes out as two mangled bytes
 * where the renderer expects one UTF-8 codepoint. TCON's legacy "(17)"
 * numeric-genre-ID form (ID3v1's 148-entry table) is deliberately not
 * decoded -- passed through as the literal digits -- since virtually
 * everything written in the last two decades already stores plain text. */
static void id3_text_out(const unsigned char *v, uint32_t len, char *out, unsigned outn) {
    if (!out || outn < 2 || len == 0) { if (out && outn) out[0] = '\0'; return; }
    unsigned char enc = v[0];
    const unsigned char *s = v + 1;
    uint32_t n = len - 1;
    unsigned o = 0;
    if (enc == 0 || enc == 3) {
        for (uint32_t i = 0; i < n && o < outn - 1; i++) {
            unsigned char c = s[i];
            if (c == 0) break;
            if (enc == 0 && c >= 0x80) {
                if (o + 2 >= outn) break;
                out[o++] = (char)(0xC0 | (c >> 6));
                out[o++] = (char)(0x80 | (c & 0x3F));
            } else out[o++] = (char)c;
        }
    } else {
        int be = (n >= 2 && s[0] == 0xFE && s[1] == 0xFF);
        uint32_t i = (n >= 2 && ((s[0] == 0xFF && s[1] == 0xFE) || (s[0] == 0xFE && s[1] == 0xFF))) ? 2 : 0;
        for (; i + 1 < n && o < outn - 1; i += 2) {
            unsigned cp = be ? ((unsigned)(s[i] << 8) | s[i + 1]) : ((unsigned)(s[i + 1] << 8) | s[i]);
            if (cp == 0) break;
            if (cp >= 0xD800 && cp <= 0xDFFF) break;   /* surrogate pair -- rare in tags, not chased */
            if (cp < 0x80) { if (o + 1 >= outn) break; out[o++] = (char)cp; }
            else if (cp < 0x800) {
                if (o + 2 >= outn) break;
                out[o++] = (char)(0xC0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                if (o + 3 >= outn) break;
                out[o++] = (char)(0xE0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
        }
    }
    out[o] = '\0';
}

static void id3_all(FILE *f, tagset_t *ts) {
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return;
    int ver = h[3];
    uint32_t tag_size = syncsafe(h + 6), pos = 0;
    while (pos + 10 < tag_size) {
        unsigned char fh[10];
        if (fread(fh, 1, 10, f) != 10) return;
        if (fh[0] == 0) return;                    /* padding */
        uint32_t fsize = (ver >= 4) ? syncsafe(fh + 4) : rd32be(fh + 4);
        /* A frame body running past the end of the tag is a corrupt size:
         * give up. A merely large one (any APIC) is skipped by the seek. */
        if (fsize == 0 || fsize > tag_size - pos - 10) return;
        int *num = ts->track && *ts->track < 0 && !memcmp(fh, "TRCK", 4) ? ts->track
                 : ts->disc  && *ts->disc  < 0 && !memcmp(fh, "TPOS", 4) ? ts->disc : NULL;
        char *out = NULL; unsigned outn = 0;
        if (!memcmp(fh, "TIT2", 4))      { out = ts->title;      outn = ts->title_n; }
        else if (!memcmp(fh, "TSST", 4)) { out = ts->disc_title; outn = ts->dt_n; }
        else if (!memcmp(fh, "TPE1", 4)) { out = ts->artist;     outn = ts->an; }
        else if (!memcmp(fh, "TALB", 4)) { out = ts->album;      outn = ts->bn; }
        else if (!memcmp(fh, "TPE2", 4)) { out = ts->aartist;    outn = ts->aan; }
        else if (!memcmp(fh, "TCON", 4)) { out = ts->genre;      outn = ts->gn; }
        int txxx = !memcmp(fh, "TXXX", 4) && ts->disc_title && ts->dt_n > 1 && !ts->disc_title[0];
        if (num || (out && outn > 1 && !out[0]) || txxx) {
            unsigned char v[512];
            uint32_t want = fsize < sizeof(v) - 1 ? fsize : sizeof(v) - 1;
            if (fread(v, 1, want, f) != want) return;
            v[want] = 0;
            if (num) *num = parse_num((char *)v + 1, (int)want - 1);   /* first byte: encoding */
            else if (out) id3_text_out(v, want, out, outn);
            else if (txxx && (v[0] == 0 || v[0] == 3) && !strcasecmp((char *)v + 1, "DISCSUBTITLE")) {
                /* TXXX: encoding, a description, NUL, the value. Latin-1 or
                 * UTF-8 only; a UTF-16 TXXX is left alone. */
                size_t dl = strlen((char *)v + 1);
                if (2 + dl < want) {
                    unsigned char tmp[512];
                    tmp[0] = v[0];
                    memcpy(tmp + 1, v + 2 + dl, want - 2 - dl);
                    id3_text_out(tmp, (uint32_t)(want - 1 - dl), ts->disc_title, ts->dt_n);
                }
            }
            if (want < fsize && fseek(f, (long)(fsize - want), SEEK_CUR)) return;
        } else if (fseek(f, (long)fsize, SEEK_CUR)) {
            return;
        }
        pos += 10 + fsize;
    }
}

static void tags_read(const char *path, tagset_t *ts) {
    if (ts->track) *ts->track = -1;
    if (ts->disc)  *ts->disc  = -1;
    char *strs[] = { ts->title, ts->disc_title, ts->artist, ts->album, ts->aartist, ts->genre };
    unsigned ns[] = { ts->title_n, ts->dt_n, ts->an, ts->bn, ts->aan, ts->gn };
    for (int i = 0; i < 6; i++) if (strs[i] && ns[i]) strs[i][0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char m[12];
    if (fread(m, 1, sizeof(m), f) != sizeof(m)) { fclose(f); return; }
    rewind(f);
    if (!memcmp(m, "fLaC", 4))            flac_all(f, ts);
    else if (!memcmp(m, "OggS", 4))       ogg_all(f, ts);
    else if (!memcmp(m + 4, "ftyp", 4))   mp4_all(f, ts);
    else if (!memcmp(m, "ID3", 3))        id3_all(f, ts);
    fclose(f);
}

void tag_read(const char *path, int *track, int *disc, char *title, unsigned title_n) {
    tagset_t ts = { .track = track, .disc = disc, .title = title, .title_n = title ? title_n : 0 };
    tags_read(path, &ts);
}

void tag_read_disc(const char *path, int *track, int *disc, char *title, unsigned title_n,
                   char *disc_title, unsigned dt_n) {
    tagset_t ts = { .track = track, .disc = disc, .title = title, .title_n = title ? title_n : 0,
                    .disc_title = disc_title, .dt_n = disc_title ? dt_n : 0 };
    tags_read(path, &ts);
}

int tag_track_number(const char *path) {
    int v; tag_read(path, &v, NULL, NULL, 0); return v;
}

int tag_disc_number(const char *path) {
    int v; tag_read(path, NULL, &v, NULL, 0); return v;
}

int tag_title(const char *path, char *out, unsigned n) {
    if (!out || n < 2) return -1;
    tag_read(path, NULL, NULL, out, n);
    return out[0] ? 0 : -1;
}

void tag_read_meta(const char *path, char *artist, unsigned artist_n,
                   char *album, unsigned album_n,
                   char *album_artist, unsigned aa_n,
                   char *genre, unsigned genre_n) {
    tagset_t ts = { .artist = artist, .an = artist ? artist_n : 0,
                    .album = album, .bn = album ? album_n : 0,
                    .aartist = album_artist, .aan = album_artist ? aa_n : 0,
                    .genre = genre, .gn = genre ? genre_n : 0 };
    tags_read(path, &ts);
}
