/* spotify.c — see spotify.h. R23's second-choice album art source, tried
 * when Last.fm's own album.getinfo has no match -- Spotify's catalog and
 * search matching cover a lot of the live/bootleg/reissue titles Last.fm's
 * fan-tagged database doesn't.
 *
 * Needs its own OAuth2 client-credentials exchange first (a plain read
 * from Last.fm needs no auth at all; every Spotify Web API call does),
 * which is the whole reason this isn't just another branch in lastfm.c --
 * the token fetch, the header-authenticated search, and the image-picking
 * order (largest-first here, smallest-first there) are different enough
 * in shape that sharing the file would mean more conditionals than code
 * saved. Same bundled curl as lastfm.c and podcast.c, same reasoning. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "spotify.h"

/* Thread id, for temp-file names. Cover lookups run on several threads at
 * once (the playing track's, the browsed album's, the artist page's), and
 * getpid() is the same in all of them -- a name per process had two lookups
 * writing and reading one JSON response, or one .part image, between them. */
static int tid(void) { return (int)syscall(SYS_gettid); }

#define SETTINGS_PATH "/data/mnt/sd_0/settings.txt"
#define CURL_BIN      "/data/mnt/sd_0/.podsync/curl"
#define CURL_CA       "/data/mnt/sd_0/.podsync/cacert.pem"

static char g_client_id[64];
static char g_client_secret[64];
static int  g_loaded;

void spotify_load_config(void) {
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
            if (!strcmp(s, "Spotify")) in_section = 1;
            continue;
        }
        if (s[0] == '\0' || s[0] == '#') continue;
        if (want == 0) { snprintf(g_client_id, sizeof(g_client_id), "%s", s); want = 1; }
        else { snprintf(g_client_secret, sizeof(g_client_secret), "%s", s); break; }
    }
    fclose(f);
}

int spotify_has_key(void) {
    spotify_load_config();
    return g_client_id[0] != '\0' && g_client_secret[0] != '\0';
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

/* Runs curl with a caller-built argv (needed here, unlike lastfm.c's
 * run_curl(): the token exchange needs -u and -d, the search needs a
 * bearer -H, neither is just "one URL, one output file"). Bundled copy
 * first, a system curl on PATH as fallback, same as everywhere else this
 * pattern appears. argv[0] is filled in by the caller as CURL_BIN or
 * "curl" to match whichever exec actually runs -- harmless either way,
 * it's just what the child sees as its own name. */
static int run_curl_argv(char *const argv[], const char *out_path) {
    unlink(out_path);
    pid_t pid = fork();
    if (pid == 0) {
        execv(CURL_BIN, argv);
        execvp("curl", argv);
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

static int extract_json_string(const char *buf, const char *key, char *out, size_t out_n) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(buf, pat);
    if (!k) return -1;
    const char *c = strchr(k, ':');
    if (!c) return -1;
    const char *q1 = strchr(c, '"');
    if (!q1) return -1;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return -1;
    size_t len = (size_t)(q2 - q1 - 1);
    if (len >= out_n) len = out_n - 1;
    memcpy(out, q1 + 1, len);
    out[len] = '\0';
    return 0;
}

static int get_access_token(char *token, size_t token_n) {
    char auth[160];
    snprintf(auth, sizeof(auth), "%s:%s", g_client_id, g_client_secret);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/.spotify_token_%d.json", tid());

    char *argv[] = {
        (char *)CURL_BIN, "-fsSL", "--cacert", (char *)CURL_CA,
        "--connect-timeout", "10", "--max-time", "15",
        "-u", auth, "-d", "grant_type=client_credentials",
        "-o", tmp, "https://accounts.spotify.com/api/token",
        NULL
    };
    int rc = run_curl_argv(argv, tmp);
    if (rc != 0) { unlink(tmp); return -1; }

    FILE *f = fopen(tmp, "rb");
    unlink(tmp);
    if (!f) return -1;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    return extract_json_string(buf, "access_token", token, token_n);
}

/* Spotify orders an album's images largest-first (the opposite of
 * Last.fm's smallest-first array), so the *first* "url" in the *first*
 * result's image array is the biggest available -- no need to walk the
 * whole array picking the last non-empty one the way lastfm.c's picker
 * does. */
/* Pulls one quoted JSON string value for `key`, searching from `from`
 * onward, writing the byte just past its closing quote to *after (so the
 * caller can resume scanning past whatever it just read). */
static int extract_next_string(const char *from, const char *key,
                               char *out, size_t out_n, const char **after) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(from, pat);
    if (!k) return -1;
    const char *c = strchr(k, ':');
    if (!c) return -1;
    const char *q1 = strchr(c, '"');
    if (!q1) return -1;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return -1;
    size_t len = (size_t)(q2 - q1 - 1);
    if (len >= out_n) len = out_n - 1;
    memcpy(out, q1 + 1, len);
    out[len] = '\0';
    if (after) *after = q2 + 1;
    return 0;
}

/* Free-text search is loose enough to confidently return the *wrong*
 * release by the right artist -- checked live: searching for "The Velvet
 * Underground" + "1969: Velvet Underground Live With Lou Reed, Volume 1"
 * (a live bootleg) matched their self-titled 1969 studio album instead,
 * a completely different, more prominent release by the same artist.
 * Rather than trust the top hit outright, require most of the searched
 * album title's own distinguishing words to actually appear in what
 * matched -- catches exactly this shape of mismatch (the wrong result
 * shares "velvet"/"underground" but has none of "1969", "live", "reed",
 * or "volume") while still tolerating minor real-world differences (a
 * missing "Volume 1" if the catalog just doesn't split it that way,
 * different punctuation, and so on). Deliberately not requiring *every*
 * word -- that would reject good matches for exactly the same kind of
 * small wording difference this is trying to tolerate. */
static int titles_resemble(const char *searched, const char *found) {
    char a[300], b[300];
    size_t ai = 0, bi = 0;
    for (size_t i = 0; searched[i] && ai + 1 < sizeof(a); i++)
        a[ai++] = (char)tolower((unsigned char)searched[i]);
    a[ai] = '\0';
    for (size_t i = 0; found[i] && bi + 1 < sizeof(b); i++)
        b[bi++] = (char)tolower((unsigned char)found[i]);
    b[bi] = '\0';

    int total = 0, hit = 0;
    char *p = a;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        char *ws = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        size_t wlen = (size_t)(p - ws);
        if (wlen < 3) continue;   /* "a", "of", "the" -- too common to mean anything */
        total++;
        char word[64];
        size_t n = wlen < sizeof(word) - 1 ? wlen : sizeof(word) - 1;
        memcpy(word, ws, n);
        word[n] = '\0';
        if (strstr(b, word)) hit++;
    }
    if (total == 0) return 1;   /* nothing meaningful to check -- don't block on it */
    return hit * 100 >= total * 55;   /* >= 55% of significant words present */
}

int spotify_fetch_cover(const char *artist, const char *album, const char *dest_jpg) {
    spotify_load_config();
    if (!g_client_id[0] || !g_client_secret[0] || !artist[0] || !album[0]) return -1;

    char token[512];
    if (get_access_token(token, sizeof(token)) != 0) return -1;

    /* Plain free-text, not the field-restricted `album:X artist:Y` syntax
     * this started as -- checked live against a real messy title ("1969:
     * Velvet Underground Live With Lou Reed, Volume 1"): the field-
     * restricted form found nothing at all, while the exact same words as
     * a plain query matched correctly on the first try. Real tag data
     * carries colons, "Volume N" suffixes and other punctuation that the
     * strict field syntax apparently can't absorb but free-text search's
     * own relevance ranking handles fine. Composed raw, then the *whole*
     * string encoded in one pass -- encoding artist/album separately and
     * splicing a literal space in afterward would leave that space
     * unencoded, which Spotify's search tolerates unreliably. */
    char raw_q[600];
    snprintf(raw_q, sizeof(raw_q), "%s %s", artist, album);
    char eq[900];
    url_encode(raw_q, eq, sizeof(eq));

    /* limit=5, not 1: free-text search's top hit can be confidently wrong
     * (see titles_resemble()'s own comment for the real case this was
     * checked against), so this walks results in relevance order and
     * takes the first one that actually resembles what was searched for,
     * rather than trusting position 1 outright. */
    char url[1000];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/search?q=%s&type=album&limit=5", eq);

    char auth_hdr[560];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", token);

    char meta_path[64];
    snprintf(meta_path, sizeof(meta_path), "/tmp/.spotify_search_%d.json", tid());
    char *argv[] = {
        (char *)CURL_BIN, "-fsSL", "--cacert", (char *)CURL_CA,
        "--connect-timeout", "10", "--max-time", "15",
        "-H", auth_hdr, "-o", meta_path, url,
        NULL
    };
    if (run_curl_argv(argv, meta_path) != 0) { unlink(meta_path); return -1; }

    FILE *f = fopen(meta_path, "rb");
    unlink(meta_path);
    if (!f) return -1;
    char *buf = malloc(65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    fclose(f);
    if (!buf) return -1;
    buf[n] = '\0';

    /* Each result item carries exactly one "images" array; "name" (the
     * item's own, confirmed against real responses to sit right after
     * images in Spotify's field order -- see titles_resemble()'s sibling
     * extract_next_string() calls below) follows it. Walking successive
     * "images" occurrences therefore walks successive result items, in
     * the same relevance order Spotify returned them. */
    char img_url[600] = "";
    const char *p = buf;
    for (int i = 0; i < 5; i++) {
        const char *arr = strstr(p, "\"images\"");
        if (!arr) break;
        arr = strchr(arr, '[');
        if (!arr) break;
        const char *end = strchr(arr, ']');
        if (!end) break;

        char this_url[600] = "", this_name[300] = "";
        const char *after_url = NULL, *after_name = NULL;
        extract_next_string(arr, "url", this_url, sizeof(this_url), &after_url);
        extract_next_string(end, "name", this_name, sizeof(this_name), &after_name);
        p = after_name ? after_name : end + 1;

        if (this_url[0] && this_name[0] && titles_resemble(album, this_name)) {
            snprintf(img_url, sizeof(img_url), "%s", this_url);
            break;
        }
    }
    free(buf);
    if (!img_url[0]) return -1;

    char tmp_jpg[600];
    snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.part%d", dest_jpg, tid());
    char *dl_argv[] = {
        (char *)CURL_BIN, "-fsSL", "--cacert", (char *)CURL_CA,
        "--connect-timeout", "10", "--max-time", "20",
        "-o", tmp_jpg, img_url,
        NULL
    };
    if (run_curl_argv(dl_argv, tmp_jpg) != 0) { unlink(tmp_jpg); return -1; }
    if (rename(tmp_jpg, dest_jpg) != 0) { unlink(tmp_jpg); return -1; }
    return 0;
}

/* Artist page's photo fallback -- same shape as spotify_fetch_cover() just
 * above (type=artist instead of type=album, matched against the artist's
 * own name via titles_resemble() instead of an album title), since a
 * result item's "images" array followed by "name" is the identical field
 * order for both object types. */
int spotify_fetch_artist_image(const char *artist, const char *dest_jpg) {
    spotify_load_config();
    if (!g_client_id[0] || !g_client_secret[0] || !artist[0]) return -1;

    char token[512];
    if (get_access_token(token, sizeof(token)) != 0) return -1;

    char eq[900];
    url_encode(artist, eq, sizeof(eq));

    char url[1000];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/search?q=%s&type=artist&limit=5", eq);

    char auth_hdr[560];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", token);

    char meta_path[64];
    snprintf(meta_path, sizeof(meta_path), "/tmp/.spotify_artist_%d.json", tid());
    char *argv[] = {
        (char *)CURL_BIN, "-fsSL", "--cacert", (char *)CURL_CA,
        "--connect-timeout", "10", "--max-time", "15",
        "-H", auth_hdr, "-o", meta_path, url,
        NULL
    };
    if (run_curl_argv(argv, meta_path) != 0) { unlink(meta_path); return -1; }

    FILE *f = fopen(meta_path, "rb");
    unlink(meta_path);
    if (!f) return -1;
    char *buf = malloc(65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    fclose(f);
    if (!buf) return -1;
    buf[n] = '\0';

    char img_url[600] = "";
    const char *p = buf;
    for (int i = 0; i < 5; i++) {
        const char *arr = strstr(p, "\"images\"");
        if (!arr) break;
        arr = strchr(arr, '[');
        if (!arr) break;
        const char *end = strchr(arr, ']');
        if (!end) break;

        char this_url[600] = "", this_name[300] = "";
        const char *after_url = NULL, *after_name = NULL;
        extract_next_string(arr, "url", this_url, sizeof(this_url), &after_url);
        extract_next_string(end, "name", this_name, sizeof(this_name), &after_name);
        p = after_name ? after_name : end + 1;

        if (this_url[0] && this_name[0] && titles_resemble(artist, this_name)) {
            snprintf(img_url, sizeof(img_url), "%s", this_url);
            break;
        }
    }
    free(buf);
    if (!img_url[0]) return -1;

    char tmp_jpg[600];
    snprintf(tmp_jpg, sizeof(tmp_jpg), "%s.part%d", dest_jpg, tid());
    char *dl_argv[] = {
        (char *)CURL_BIN, "-fsSL", "--cacert", (char *)CURL_CA,
        "--connect-timeout", "10", "--max-time", "20",
        "-o", tmp_jpg, img_url,
        NULL
    };
    if (run_curl_argv(dl_argv, tmp_jpg) != 0) { unlink(tmp_jpg); return -1; }
    if (rename(tmp_jpg, dest_jpg) != 0) { unlink(tmp_jpg); return -1; }
    return 0;
}
