/* lastfm.c — see lastfm.h. R23: album art fallback when nothing local
 * (embedded picture, folder image) exists for an album, matching the same
 * shape Navidrome's own Last.fm agent uses: an unauthenticated
 * album.getinfo call by artist+album, taking the largest available image
 * size from the response. No request signing needed -- that's only
 * required for authenticated calls (scrobbling), not a plain read.
 *
 * Network access goes through the same bundled, TLS-capable curl the
 * podcast sync script already carries (busybox's own wget/curl has TLS too
 * old for HTTPS -- see podcast.c's own note on this), so this adds no new
 * dependency; if that curl isn't present (podcasts never set up), a system
 * curl on PATH is tried as a fallback, and failing that this just quietly
 * does nothing, the same as any other missing-art case. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lastfm.h"

/* Thread id, for temp-file names. Cover lookups run on several threads at
 * once (the playing track's, the browsed album's, the artist page's), and
 * getpid() is the same in all of them -- a name per process had two lookups
 * writing and reading one JSON response, or one .part image, between them. */
static int tid(void) { return (int)syscall(SYS_gettid); }

#define SETTINGS_PATH   "/data/mnt/sd_0/settings.txt"
#define CURL_BIN        "/data/mnt/sd_0/.podsync/curl"
#define CURL_CA         "/data/mnt/sd_0/.podsync/cacert.pem"

static char g_api_key[64];
static char g_secret[64];   /* not currently used -- getinfo needs no signing -- kept for a future authenticated call */
static int  g_loaded;

void lastfm_load_config(void) {
    if (g_loaded) return;
    g_loaded = 1;
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[256];
    int in_section = 0, want = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!in_section) {
            if (!strcmp(s, "LastFM")) in_section = 1;
            continue;
        }
        if (s[0] == '\0' || s[0] == '#') continue;
        if (want == 0) { snprintf(g_api_key, sizeof(g_api_key), "%s", s); want = 1; }
        else { snprintf(g_secret, sizeof(g_secret), "%s", s); break; }
    }
    fclose(f);
}

int lastfm_has_key(void) {
    lastfm_load_config();
    return g_api_key[0] != '\0';
}

static void url_encode(const char *in, char *out, size_t out_n) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < out_n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            snprintf(out + j, out_n - j, "%%%02X", c);
            j += 3;
        }
    }
    out[j] = '\0';
}

/* Fetches `url` to `out_path` via curl, bundled copy first. A plain
 * fork+exec with the URL as one argv element -- never a shell, so nothing
 * in an artist or album name (quotes, $, backticks, whatever) can be
 * interpreted as anything but literal bytes of the URL. */
static int run_curl(const char *url, const char *out_path) {
    unlink(out_path);
    pid_t pid = fork();
    if (pid == 0) {
        execl(CURL_BIN, CURL_BIN, "-fsSL", "--cacert", CURL_CA,
              "--connect-timeout", "10", "--max-time", "20",
              "-o", out_path, url, (char *)NULL);
        /* Bundled curl missing (podcasts never set up on this device) --
         * try whatever's on PATH before giving up. */
        execlp("curl", "curl", "-fsSL",
               "--connect-timeout", "10", "--max-time", "20",
               "-o", out_path, url, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return -1;
    int status;
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    struct stat st;
    if (stat(out_path, &st) != 0 || st.st_size <= 0) return -1;
    return 0;
}

/* Last.fm's own image array runs small -> mega, and a size that wasn't
 * generated comes back as an empty "#text" rather than being omitted, so
 * the last non-empty #text within the array is the largest one actually
 * available -- scanned directly rather than through a JSON parser, the
 * same "just enough" approach this codebase already uses for MP4 atoms
 * and ID3 frames. Stops at the array's own closing ']' so a later,
 * unrelated field can never be mistaken for another image entry. */
static int extract_best_image_url(const char *buf, char *out, size_t out_n) {
    const char *arr = strstr(buf, "\"image\"");
    if (!arr) return -1;
    arr = strchr(arr, '[');
    if (!arr) return -1;
    const char *end = strchr(arr, ']');
    if (!end) end = arr + strlen(arr);
    out[0] = '\0';
    const char *p = arr;
    while (p < end) {
        const char *k = strstr(p, "\"#text\"");
        if (!k || k >= end) break;
        const char *c = strchr(k, ':');
        if (!c || c >= end) break;
        const char *q1 = strchr(c, '"');
        if (!q1 || q1 >= end) break;
        const char *q2 = strchr(q1 + 1, '"');
        if (!q2) break;
        size_t len = (size_t)(q2 - q1 - 1);
        if (len > 0 && len < out_n) {
            memcpy(out, q1 + 1, len);
            out[len] = '\0';
        }
        p = q2 + 1;
    }
    return out[0] ? 0 : -1;
}

/* Finds `"key"` within [from, end) and extracts its quoted string value,
 * JSON-unescaped (\", \\, \/, \n, \t, \uXXXX -- the escapes Last.fm's bio
 * text actually contains; \uXXXX above the Latin-1 range is written out as
 * '?' rather than real UTF-8, since this app's font (see text.c) has no
 * broader coverage to render it with anyway). Unlike extract_best_image_
 * url()/extract_json_string() elsewhere, this one has to unescape: an
 * image URL never contains a literal quote or newline, but bio prose is
 * full of both. */
static int extract_json_string_unescaped(const char *from, const char *end,
                                         const char *key, char *out, size_t out_n) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(from, pat);
    if (!k || (end && k >= end)) return -1;
    const char *c = strchr(k, ':');
    if (!c || (end && c >= end)) return -1;
    const char *q1 = strchr(c, '"');
    if (!q1 || (end && q1 >= end)) return -1;
    size_t j = 0;
    const char *p = q1 + 1;
    while (*p && *p != '"' && j + 1 < out_n) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[j++] = '\n'; break;
                case 't': out[j++] = ' ';  break;
                case '"': out[j++] = '"';  break;
                case '\\': out[j++] = '\\'; break;
                case '/': out[j++] = '/';  break;
                case 'u': {
                    /* Four hex digits; only the ones that fit in one byte
                     * are rendered, everything else becomes '?' -- see the
                     * function comment. */
                    unsigned v = 0;
                    for (int i = 1; i <= 4 && p[i]; i++) {
                        char h = p[i];
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    }
                    out[j++] = (v < 128) ? (char)v : '?';
                    p += 4;
                    break;
                }
                default: out[j++] = *p; break;
            }
            p++;
        } else {
            out[j++] = *p++;
        }
    }
    out[j] = '\0';
    return 0;
}

/* Last.fm's bio.summary always ends with a "Read more on Last.fm" link
 * (plain HTML, e.g. `<a href="https://...">Read more on Last.fm</a>.`) --
 * meaningless once the URL itself is gone (this app has no browser), so
 * cut the text off at the first '<' rather than render raw markup or a
 * dangling sentence fragment. Whatever, if anything, precedes it is prose
 * with no HTML of its own in every real response checked, so no broader
 * tag-stripping is attempted -- a stray '<' would just truncate slightly
 * early, not corrupt the display. Trailing whitespace left by the cut is
 * trimmed too. */
static void strip_read_more(char *s) {
    char *a = strchr(s, '<');
    if (a) *a = '\0';
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = '\0';
}

int lastfm_fetch_artist(const char *artist, const char *dest_jpg,
                        char *bio_out, size_t bio_n) {
    lastfm_load_config();
    bio_out[0] = '\0';
    if (!g_api_key[0] || !artist[0]) return -1;

    char ea[300];
    url_encode(artist, ea, sizeof(ea));

    char url[700];
    snprintf(url, sizeof(url),
             "https://ws.audioscrobbler.com/2.0/?method=artist.getinfo"
             "&api_key=%s&artist=%s&format=json",
             g_api_key, ea);

    char meta_path[64];
    snprintf(meta_path, sizeof(meta_path), "/tmp/.lastfm_artist_%d.json", tid());
    if (run_curl(url, meta_path) != 0) { unlink(meta_path); return -1; }

    FILE *f = fopen(meta_path, "rb");
    if (!f) { unlink(meta_path); return -1; }
    char *buf = malloc(65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    fclose(f);
    unlink(meta_path);
    if (!buf) return -1;
    buf[n] = '\0';

    int result = 0;

    char img_url[600];
    if (extract_best_image_url(buf, img_url, sizeof(img_url)) == 0) {
        char tmp_jpg[600];
        snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.part%d", dest_jpg, tid());
        if (run_curl(img_url, tmp_jpg) == 0 && rename(tmp_jpg, dest_jpg) == 0)
            result |= 1;
        else
            unlink(tmp_jpg);
    }

    /* bio.content, not bio.summary -- checked live against a real response
     * (Queen's): summary is Last.fm's own pre-truncated preview, and nothing
     * guarantees it ends on a sentence boundary -- Queen's genuinely stops
     * mid-clause ("...Mercury, a fan of Smile") right before the "Read more"
     * link, which is Last.fm's own data, not a bug in the extraction here.
     * content is the real, complete biography (multiple paragraphs, ~3KB
     * for Queen against summary's ~250 bytes) and ends the same way, so the
     * same strip_read_more() cut still applies.
     *
     * Confirmed globally unique in a real response (exactly one "content"
     * key in the whole document), so no bounding is needed for correctness.
     * An earlier version of this code tried to bound the search to the
     * "bio" object by assuming "similar" was the next sibling key after it
     * -- checked live against the real field order and that assumption was
     * simply wrong (similar appears *before* bio, near the top of the
     * response, not after), which made that bound a no-op (strstr search-
     * ing forward from bio_obj for something that only exists earlier
     * always returns NULL) rather than actively harmful -- removed rather
     * than fixed, since a real key that's confirmed unique needs no bound
     * at all. */
    if (extract_json_string_unescaped(buf, NULL, "content", bio_out, bio_n) == 0
        && bio_out[0]) {
        strip_read_more(bio_out);
        if (bio_out[0]) result |= 2;
    }

    free(buf);
    return result;
}

int lastfm_fetch_cover(const char *artist, const char *album, const char *dest_jpg) {
    lastfm_load_config();
    if (!g_api_key[0] || !artist[0] || !album[0]) return -1;

    char ea[300], eb[300];
    url_encode(artist, ea, sizeof(ea));
    url_encode(album, eb, sizeof(eb));

    char url[900];
    snprintf(url, sizeof(url),
             "https://ws.audioscrobbler.com/2.0/?method=album.getinfo"
             "&api_key=%s&artist=%s&album=%s&format=json",
             g_api_key, ea, eb);

    char meta_path[64];
    snprintf(meta_path, sizeof(meta_path), "/tmp/.lastfm_meta_%d.json", tid());
    if (run_curl(url, meta_path) != 0) { unlink(meta_path); return -1; }

    FILE *f = fopen(meta_path, "rb");
    if (!f) { unlink(meta_path); return -1; }
    char *buf = malloc(65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    fclose(f);
    unlink(meta_path);
    if (!buf) return -1;
    buf[n] = '\0';

    char img_url[600];
    int rc = extract_best_image_url(buf, img_url, sizeof(img_url));
    free(buf);
    if (rc != 0) return -1;

    char tmp_jpg[600];
    snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.part%d", dest_jpg, tid());
    if (run_curl(img_url, tmp_jpg) != 0) { unlink(tmp_jpg); return -1; }
    if (rename(tmp_jpg, dest_jpg) != 0) { unlink(tmp_jpg); return -1; }
    return 0;
}
