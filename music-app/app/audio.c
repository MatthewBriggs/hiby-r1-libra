/* audio.c — decode to ALSA on a worker thread.
 *
 * Decoding is bundled, output is borrowed. The device's libsndfile looked like
 * the obvious answer and is not: it is built without libFLAC — links libc and
 * libm alone — so it rejects FLAC, which is 3943 of the 4722 tracks here. The
 * public-domain dr_libs decoders are compiled in instead, covering FLAC, MP3
 * and WAV, 96% of the library, with no device dependency at all. M4A is the
 * remaining 4%; libfdk-aac is on the device but needs an MP4 demuxer first.
 *
 * ALSA is still dlopen'd, so a missing libasound degrades to "no sound" rather
 * than the whole hook failing to load into hiby_player.
 *
 * Device constraints carried over from the Podcasts app, all learned the hard
 * way there:
 *
 *  - Open "plughw:0,0" so ALSA resamples to whatever discrete rates the
 *    hardware actually accepts; snd_pcm_set_params is unreliable here, so hw
 *    and sw params are set by hand.
 *  - The CS43131's volume registers are not wired: writes report success, read
 *    back 0 and change nothing. Wired output therefore scales samples in
 *    software. Bluetooth is different — bluealsa exposes a real mixer.
 *  - Join the worker on "a thread exists", not on "it is still running". The
 *    worker clears its own running flag as it exits, so keying the join off
 *    that flag skips it exactly when the thread has already finished, and the
 *    stack is then never reclaimed for the life of hiby_player.
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"
#include "wsola.h"
#include "eq.h"

/* Bound the log. Four writers (alog here, mlog in music_hook.c, ilog in index.c,
 * slog in scanner.c) all append to this one file and nothing ever trimmed
 * it: found at 2.76 MB during an audit, on the /usr/data partition that has
 * ~23 MB free -- the tightest storage on the device. One generation is kept
 * (music.log.1) so a crash still leaves recent history to read, which caps
 * total use at roughly 2x LOG_MAX rather than unbounded. stat()ed once every
 * 256 lines rather than per line: this runs on the UI thread, and the point
 * is to catch runaway growth, not to enforce the byte exactly. Any of the
 * four writers rolling the file is enough, since they all share the path. */
#define LOG_MAX (1024 * 1024)
static void log_roll_if_big(const char *path) {
    static unsigned calls;
    if ((calls++ & 0xFF) != 0) return;
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > LOG_MAX) {
        char old[128];
        snprintf(old, sizeof(old), "%s.1", path);
        rename(path, old);
    }
}

static void alog(const char *fmt, ...) {
    char b[200];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    /* Timestamped: without one there is no telling a normal run of short
     * tracks from the player racing through them, which cost a diagnosis. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[240];
    int m = snprintf(line, sizeof(line), "[%6ld.%03ld] %s",
                     (long)ts.tv_sec, ts.tv_nsec / 1000000L, b);
    if (m > 0) {
        /* /usr/data, not /tmp: a reboot wipes /tmp, and the interesting audio
         * failures (underruns, a lost output) are exactly the ones followed by
         * a reboot before anyone can read the log. mlog() was moved for this
         * same reason; alog() was missed at the time, which is why the BG12
         * incident left no audio diagnostics at all. */
        log_roll_if_big("/usr/data/music.log");
        int fd = open("/usr/data/music.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { write(fd, line, (size_t)(m < (int)sizeof(line) ? m : (int)sizeof(line))); close(fd); }
    }
}

static int g_exact;                 /* opened without a conversion layer */
static int g_out_fmt;               /* ALSA format the device actually took */
static int g_out_lost;              /* the output device disappeared mid-play */
static int g_lost_kind = -1;        /* which route it was, so the same one can be waited for */
static const char *g_codec = "";    /* what the current source turned out to be */

/* ---- decoders ------------------------------------------------------------ */
#include "vendor/dr_flac.h"
#include "vendor/dr_mp3.h"
#include "vendor/dr_wav.h"
#include "aac.h"
#include "alac.h"
#include "mp4.h"
#include "ts.h"
#include "hls.h"
#include "status.h"
#include "vorbis_dec.h"
#include "opus_dec.h"

typedef enum { DEC_NONE = 0, DEC_FLAC, DEC_MP3, DEC_WAV, DEC_M4A,
               DEC_VORBIS, DEC_OPUS } dec_kind_t;

typedef struct {
    dec_kind_t kind;
    drflac    *flac;
    drmp3      mp3;
    drwav      wav;
    int        channels;
    unsigned   rate;
    uint64_t   frames;      /* 0 when the container does not say cheaply */
    int        bits;        /* of the source; 16 unless the file says more */
    int        is_stream;
    int        is_hls;
    mp4_t       mp4;
    aac_dec_t  *aac;
    alac_dec_t *alac;   /* .m4a is either AAC or ALAC, never both; mp4.codec says which */
    int        aac_frames, aac_taken;   /* into the shared PCM buffer below, whichever decoder filled it */
    vorbis_dec_t vorbis;
    opus_dec_t   opus;
} dec_t;

/* Only lossless formats above 16 bits have anything worth dithering when
 * forced down to 16 (Bluetooth, always S16_LE) — MP3 and AAC decode to 16
 * either way. Independent of output route on purpose: it says whether the
 * source has extra precision, not whether that precision reaches ALSA. */
static int dec_is_wide(const dec_t *d) {
    return (d->bits > 16) && (d->kind == DEC_FLAC || d->kind == DEC_WAV);
}

/* Holds one decoded access unit, whichever decoder produced it; the buffer is
 * shared because only one file is ever being decoded. Keeping it out of dec_t
 * also keeps it off the worker's stack.
 *
 * Sized by the *largest* frame either decoder emits, not by AAC alone: an AAC
 * frame is at most 2048 samples per channel, but an ALAC frame is 4096 (the
 * frameLength in its magic cookie). Sizing this 2048 is what made every ALAC
 * file fail to open — alac_decode clamps its sample count to what the caller's
 * buffer will hold, so it decoded a 4096-sample frame as if it were 2048 and
 * the bitstream ran long. */
#define DEC_MAX_FRAMES 4096
static short g_aacpcm[DEC_MAX_FRAMES * 8];

/* Worst case for one *compressed* ALAC access unit, not the decoded PCM
 * g_aacpcm above holds: 4096 samples * 2 ch * 16-bit, the size a frame can
 * reach in ALAC's escape mode, where a loud or noisy passage compresses so
 * poorly the encoder just stores it close to raw rather than losing time to
 * a compression pass that will not pay for itself. 8192 (this file's
 * previous size) is exactly half of that -- silently wrong, not obviously
 * wrong: mp4_next() treats an access unit bigger than the caller's buffer as
 * unreadable and skips it rather than erroring (see its own comment, "one
 * unreadable access unit... should cost a click, not the rest of the
 * chapter"), and once 64 in a row fail that way the file reads as finished.
 * Reproduced live on a noisy 1969 Velvet Underground bootleg -- exactly the
 * loud, compression-resistant material escape mode exists for -- where every
 * track past the first ended after a few seconds instead of several minutes.
 * Doubled with headroom, not tuned to the exact 16384-byte theoretical max,
 * since the cost of being wrong here is a silently truncated track, not a
 * cheap buffer a few KB larger than strictly necessary. */
#define M4A_AU_MAX 32768

/* Sniff the magic rather than trust the extension — a wrong decoder is a hang
 * rather than an error. */
static dec_kind_t sniff(const char *path) {
    unsigned char m[96];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return DEC_NONE;
    ssize_t n = read(fd, m, sizeof(m));
    close(fd);
    if (n < 12) return DEC_NONE;
    if (!memcmp(m, "fLaC", 4)) return DEC_FLAC;
    if (!memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WAVE", 4)) return DEC_WAV;
    /* AIFF/AIFC: dr_wav's own container auto-detection (drwav_container_aiff)
     * already handles the big-endian chunk layout once past this sniff, so
     * this reuses DEC_WAV rather than adding a decoder kind of its own. */
    if (!memcmp(m, "FORM", 4) &&
        (!memcmp(m + 8, "AIFF", 4) || !memcmp(m + 8, "AIFC", 4))) return DEC_WAV;
    /* Ogg only says "some Ogg stream lives here" -- Vorbis and Opus both
     * mark their first packet with their own magic right after the page
     * header, close enough to the front that a fixed-size read still reaches
     * it without actually parsing the page (that's ogg_io.c's job, once a
     * kind has been decided here). */
    if (!memcmp(m, "OggS", 4) && n >= 96) {
        for (int i = 27; i < 96 - 8; i++) {
            if (!memcmp(m + i, "\x01vorbis", 7)) return DEC_VORBIS;
            if (!memcmp(m + i, "OpusHead", 8)) return DEC_OPUS;
        }
    }
    if (!memcmp(m, "ID3", 3)) return DEC_MP3;
    /* An MP4 announces itself in the second box field, not the first. */
    if (!memcmp(m + 4, "ftyp", 4)) return DEC_M4A;
    if (m[0] == 0xFF && (m[1] & 0xE0) == 0xE0) return DEC_MP3;
    return DEC_NONE;
}

/* A radio stream is an endless MP3 arriving down a pipe: no file to open, no
 * length, and no seeking.
 *
 * drmp3_init is the obvious entry point and it does not work here. It seeks
 * backwards while it hunts for the first frame, and a pipe cannot rewind, so
 * it fails outright on a perfectly good stream. The low-level frame decoder
 * exists for exactly this: hand it bytes, it hands back one frame and says how
 * many bytes it consumed, and the buffer is ours to refill.
 *
 * TLS is why this shells out to the static curl on the card rather than
 * opening a socket: the device's own wget is too old to complete a modern
 * handshake.
 */
#define STREAM_BUF   (32 * 1024)
#define CURL_PATH    "/data/mnt/sd_0/.podsync/curl"
#define CA_BUNDLE    "/data/mnt/sd_0/.podsync/cacert.pem"

/* HLS: segments are fetched whole, demuxed to ADTS, and handed to the decoder
 * a frame at a time. Unlike a plain MP3 stream there is no continuous socket —
 * the fetch happens in bursts, which is why the ALSA buffer is enlarged for
 * streams so a slow segment does not underrun. */
typedef struct {
    hls_t          hls;
    aac_dec_t     *aac;
    unsigned char  seg[512 * 1024];
    unsigned char  adts[192 * 1024];
    int            adts_len, adts_used;
    short          pcm[2048 * 8];
    int            pcm_frames, pcm_taken;
    int            channels, rate;
} hls_src_t;

static hls_src_t g_hls;

typedef struct {
    FILE          *pipe;
    drmp3dec       dec;
    unsigned char  buf[STREAM_BUF];
    int            len;                    /* bytes held */
    short          pcm[DRMP3_MAX_SAMPLES_PER_FRAME];
    int            pcm_frames, pcm_taken;  /* frames decoded / already handed out */
    int            channels, rate;
} stream_t;

static stream_t g_stream;

static int stream_fill(stream_t *s) {
    if (!s->pipe || s->len >= STREAM_BUF) return 0;
    size_t got = fread(s->buf + s->len, 1, (size_t)(STREAM_BUF - s->len), s->pipe);
    s->len += (int)got;
    return (int)got;
}

/* One decoded frame at a time into s->pcm. 0 means the stream ended. */
static int stream_decode_frame(stream_t *s) {
    for (int attempt = 0; attempt < 64; attempt++) {
        drmp3dec_frame_info info;
        int samples = 0;
        if (s->len > 0)
            samples = drmp3dec_decode_frame(&s->dec, s->buf, s->len, s->pcm, &info);

        if (info.frame_bytes > 0) {
            memmove(s->buf, s->buf + info.frame_bytes, (size_t)(s->len - info.frame_bytes));
            s->len -= info.frame_bytes;
            if (samples > 0) {
                s->channels = info.channels;
                s->rate = info.sample_rate;
                s->pcm_frames = samples;
                s->pcm_taken = 0;
                return samples;
            }
            continue;                      /* header only, e.g. after a tag */
        }
        /* Nothing consumable yet: more bytes, or the stream has stopped. */
        if (stream_fill(s) == 0 && s->len < STREAM_BUF) {
            if (feof(s->pipe) || ferror(s->pipe)) return 0;
        }
    }
    return 0;
}

/* Pull in another segment and demux it. 0 means nothing new was available. */
static int hls_refill(hls_src_t *s) {
    int n = hls_next(&s->hls, s->seg, (int)sizeof(s->seg));
    if (n <= 0) return 0;
    int a = ts_extract_audio(s->seg, n, s->adts, (int)sizeof(s->adts));
    if (a <= 0) return 0;
    s->adts_len = a;
    s->adts_used = 0;
    return a;
}

static int dec_open_hls(dec_t *d, const char *url) {
    memset(d, 0, sizeof(*d));
    memset(&g_hls, 0, sizeof(g_hls));
    if (hls_open(&g_hls.hls, url) != 0) { alog("[audio] hls open failed\n"); return -1; }
    g_hls.aac = aac_open(NULL, 0);           /* ADTS: self-describing */
    if (!g_hls.aac) { alog("[audio] no AAC decoder\n"); return -1; }

    /* Decode a little before reporting success, so the rate and channel count
     * are known and a dead stream fails here rather than as silence. */
    for (int tries = 0; tries < 6; tries++) {
        if (g_hls.adts_used >= g_hls.adts_len && hls_refill(&g_hls) <= 0) continue;
        int took = aac_fill(g_hls.aac, g_hls.adts + g_hls.adts_used,
                            (unsigned)(g_hls.adts_len - g_hls.adts_used));
        if (took > 0) g_hls.adts_used += took;
        int fr = aac_frame(g_hls.aac, g_hls.pcm, 2048);
        if (fr > 0) {
            g_hls.pcm_frames = fr; g_hls.pcm_taken = 0;
            g_hls.rate = aac_rate(g_hls.aac);
            g_hls.channels = aac_channels(g_hls.aac);
            d->kind = DEC_M4A;               /* AAC, just not from a file */
            d->is_stream = 1;
            d->is_hls = 1;
            d->channels = g_hls.channels;
            d->rate = (unsigned)g_hls.rate;
            d->frames = 0;
            g_codec = "AAC";
            alog("[audio] hls %d Hz %d ch\n", g_hls.rate, g_hls.channels);
            return 0;
        }
    }
    aac_close(g_hls.aac);
    g_hls.aac = NULL;
    alog("[audio] hls produced no audio\n");
    return -1;
}

static int dec_open_stream(dec_t *d, const char *url) {
    memset(d, 0, sizeof(*d));
    memset(&g_stream, 0, sizeof(g_stream));
    char cmd[1024];
    /* -L because these stations answer with a redirect to a regional pop, and
     * --no-buffer so playback does not wait on curl filling an output block. */
    snprintf(cmd, sizeof(cmd),
             "%s -sL --no-buffer --cacert %s '%s' 2>/dev/null",
             CURL_PATH, CA_BUNDLE, url);
    g_stream.pipe = popen(cmd, "r");
    if (!g_stream.pipe) { alog("[audio] cannot start fetch\n"); return -1; }

    drmp3dec_init(&g_stream.dec);
    stream_fill(&g_stream);
    /* A playlist handed to the MP3 path decodes as nothing at all, which is
     * indistinguishable from a dead station. Recognise it and say so, and the
     * caller can open it properly. */
    if (g_stream.len >= 7 && !memcmp(g_stream.buf, "#EXTM3U", 7)) {
        pclose(g_stream.pipe);
        g_stream.pipe = NULL;
        alog("[audio] that URL is a playlist, not a stream\n");
        return -2;
    }
    if (stream_decode_frame(&g_stream) <= 0) {
        pclose(g_stream.pipe);
        g_stream.pipe = NULL;
        alog("[audio] no MP3 frames in stream\n");
        return -1;
    }
    d->kind = DEC_MP3;
    d->channels = g_stream.channels;
    d->rate = (unsigned)g_stream.rate;
    d->frames = 0;                            /* live: no length */
    d->is_stream = 1;
    g_codec = "MP3";
    alog("[audio] stream %d Hz %d ch\n", g_stream.rate, g_stream.channels);
    return 0;
}

/* AAC gives nothing back until it has a full frame, so the first access unit
 * or two may decode to silence; prime until it yields, which also proves the
 * file before anything is reported as playable. ALAC never returns 0 (one
 * access unit is always one full frame, no bit-reservoir to fill), so this
 * only loops for AAC in practice but stays generic since both fill the same
 * shared buffer. */
static int m4a_prime(dec_t *d) {
    unsigned char au[M4A_AU_MAX];
    for (int i = 0; i < 8; i++) {
        int len = mp4_next(&d->mp4, au, sizeof(au));
        if (len <= 0) return -1;
        int fr = d->alac
               ? alac_decode(d->alac, au, (unsigned)len, g_aacpcm, DEC_MAX_FRAMES)
               : aac_decode(d->aac, au, (unsigned)len, g_aacpcm, DEC_MAX_FRAMES);
        if (fr < 0) return -1;
        if (fr > 0) { d->aac_frames = fr; d->aac_taken = 0; return 0; }
    }
    return -1;
}

static int dec_open_m4a(dec_t *d, const char *path) {
    if (mp4_open(&d->mp4, path) != 0) return -1;
    if (d->mp4.codec == MP4_CODEC_ALAC) {
        d->alac = alac_open(d->mp4.asc, d->mp4.asc_len);
        if (!d->alac) { mp4_close(&d->mp4); return -1; }
    } else {
        d->aac = aac_open(d->mp4.asc, d->mp4.asc_len);
        if (!d->aac) { mp4_close(&d->mp4); return -1; }
    }
    if (m4a_prime(d) != 0) {
        if (d->alac) alac_close(d->alac); else aac_close(d->aac);
        mp4_close(&d->mp4);
        alog("[audio] no %s frames in %s\n", d->alac ? "ALAC" : "AAC", path);
        return -1;
    }
    d->kind = DEC_M4A;
    d->bits = 16;   /* both decoders hand back s16, whatever the source depth */
    d->channels = d->alac ? alac_channels(d->alac) : aac_channels(d->aac);
    d->rate     = (unsigned)(d->alac ? alac_rate(d->alac) : aac_rate(d->aac));
    /* The track length is in the file's own timescale, which is not always the
     * output rate — HE-AAC decodes to twice the rate in its config. */
    d->frames = (d->mp4.timescale && d->rate)
              ? (uint64_t)d->mp4.duration * d->rate / d->mp4.timescale : 0;
    alog("[audio] m4a %s %u Hz %d ch, %u units\n",
         d->alac ? "ALAC" : "AAC", d->rate, d->channels, d->mp4.n_samples);
    return 0;
}

/* Format-only M4A probe for callers that must not touch g_aacpcm -- notably
 * audio_probe_format() below, which scanner.c calls from its own background
 * scan thread and which can therefore run concurrently with the playback
 * worker thread's own M4A decode. dec_open_m4a() calls m4a_prime(), which
 * decodes into the single shared g_aacpcm buffer; doing that from any thread
 * but the worker races a real in-progress decode. Sample rate and channel
 * count come straight off alac_open()/aac_open()'s own parse of the ASC/
 * magic cookie though, so they're available without ever priming a frame --
 * this stops right there. Bits is always 16 (both decoders hand back s16). */
static int dec_open_m4a_probe(dec_t *d, const char *path) {
    if (mp4_open(&d->mp4, path) != 0) return -1;
    if (d->mp4.codec == MP4_CODEC_ALAC) {
        d->alac = alac_open(d->mp4.asc, d->mp4.asc_len);
        if (!d->alac) { mp4_close(&d->mp4); return -1; }
        d->channels = alac_channels(d->alac);
        d->rate     = (unsigned)alac_rate(d->alac);
        alac_close(d->alac);
        d->alac = NULL;
    } else {
        d->aac = aac_open(d->mp4.asc, d->mp4.asc_len);
        if (!d->aac) { mp4_close(&d->mp4); return -1; }
        d->channels = aac_channels(d->aac);
        d->rate     = (unsigned)aac_rate(d->aac);
        aac_close(d->aac);
        d->aac = NULL;
    }
    d->kind = DEC_M4A;
    d->bits = 16;
    d->frames = (d->mp4.timescale && d->rate)
              ? (uint64_t)d->mp4.duration * d->rate / d->mp4.timescale : 0;
    mp4_close(&d->mp4);
    return 0;
}

static int dec_open(dec_t *d, const char *path) {
    memset(d, 0, sizeof(*d));
    switch (sniff(path)) {
        case DEC_FLAC:
            d->flac = drflac_open_file(path, NULL);
            if (!d->flac) return -1;
            d->kind = DEC_FLAC;
            d->channels = d->flac->channels;
            d->rate = d->flac->sampleRate;
            d->frames = d->flac->totalPCMFrameCount;
            d->bits = d->flac->bitsPerSample;
            return 0;
        case DEC_WAV:
            if (!drwav_init_file(&d->wav, path, NULL)) return -1;
            d->kind = DEC_WAV;
            d->channels = (int)d->wav.channels;
            d->rate = d->wav.sampleRate;
            d->frames = d->wav.totalPCMFrameCount;
            d->bits = (int)d->wav.bitsPerSample;
            return 0;
        case DEC_M4A:
            return dec_open_m4a(d, path);
        case DEC_VORBIS:
            if (vorbis_dec_open(&d->vorbis, path) < 0) return -1;
            d->kind = DEC_VORBIS;
            d->channels = d->vorbis.channels;
            d->rate = d->vorbis.rate;
            d->frames = d->vorbis.total_frames;
            d->bits = 16;
            return 0;
        case DEC_OPUS:
            if (opus_dec_open(&d->opus, path) < 0) return -1;
            d->kind = DEC_OPUS;
            d->channels = d->opus.channels;
            d->rate = d->opus.rate;
            d->frames = d->opus.total_frames;
            d->bits = 16;
            return 0;
        case DEC_MP3:
            if (!drmp3_init_file(&d->mp3, path, NULL)) return -1;
            d->kind = DEC_MP3;
            d->channels = (int)d->mp3.channels;
            d->rate = d->mp3.sampleRate;
            /* Deliberately not drmp3_get_pcm_frame_count: for a VBR file it
             * decodes the whole thing to count, which is seconds of stall on
             * this CPU. The index already knows the duration. */
            d->frames = 0;
            return 0;
        default:
            return -1;
    }
}

/* Only the lossless formats have more than 16 bits to give. MP3 and AAC are
 * decoded to 16 either way, so they never take this path. */
static uint64_t dec_read32(dec_t *d, int32_t *out, uint64_t want) {
    switch (d->kind) {
        case DEC_FLAC: return drflac_read_pcm_frames_s32(d->flac, want, out);
        case DEC_WAV:  return drwav_read_pcm_frames_s32(&d->wav, want, out);
        default:       return 0;
    }
}

static uint64_t dec_read(dec_t *d, short *out, uint64_t want) {
    if (d->is_hls) {
        hls_src_t *s = &g_hls;
        uint64_t done = 0;
        while (done < want) {
            if (s->pcm_taken >= s->pcm_frames) {
                int fr = aac_frame(s->aac, s->pcm, 2048);
                if (fr <= 0) {
                    /* Out of decoded audio: feed it more, fetching another
                     * segment when the current one is spent. */
                    if (s->adts_used >= s->adts_len && hls_refill(s) <= 0) break;
                    int took = aac_fill(s->aac, s->adts + s->adts_used,
                                        (unsigned)(s->adts_len - s->adts_used));
                    if (took <= 0) break;
                    s->adts_used += took;
                    continue;
                }
                s->pcm_frames = fr;
                s->pcm_taken = 0;
            }
            int avail = s->pcm_frames - s->pcm_taken;
            uint64_t take = (uint64_t)avail < (want - done) ? (uint64_t)avail : (want - done);
            memcpy(out + done * (size_t)s->channels,
                   s->pcm + (size_t)s->pcm_taken * (size_t)s->channels,
                   (size_t)take * (size_t)s->channels * sizeof(short));
            s->pcm_taken += (int)take;
            done += take;
        }
        return done;
    }
    if (d->is_stream) {
        stream_t *s = &g_stream;
        uint64_t done = 0;
        while (done < want) {
            if (s->pcm_taken >= s->pcm_frames && stream_decode_frame(s) <= 0) break;
            int avail = s->pcm_frames - s->pcm_taken;
            uint64_t take = (uint64_t)avail < (want - done) ? (uint64_t)avail : (want - done);
            memcpy(out + done * (size_t)s->channels,
                   s->pcm + (size_t)s->pcm_taken * (size_t)s->channels,
                   (size_t)take * (size_t)s->channels * sizeof(short));
            s->pcm_taken += (int)take;
            done += take;
        }
        return done;
    }
    if (d->kind == DEC_M4A) {
        uint64_t done = 0;
        while (done < want) {
            if (d->aac_taken >= d->aac_frames) {
                unsigned char au[M4A_AU_MAX];
                int len = mp4_next(&d->mp4, au, sizeof(au));
                if (len <= 0) break;
                int fr = d->alac
                       ? alac_decode(d->alac, au, (unsigned)len, g_aacpcm, DEC_MAX_FRAMES)
                       : aac_decode(d->aac, au, (unsigned)len, g_aacpcm, DEC_MAX_FRAMES);
                if (fr < 0) break;
                if (fr == 0) continue;          /* wants more input (AAC only) */
                d->aac_frames = fr;
                d->aac_taken = 0;
            }
            int avail = d->aac_frames - d->aac_taken;
            uint64_t take = (uint64_t)avail < (want - done) ? (uint64_t)avail : (want - done);
            memcpy(out + done * (size_t)d->channels,
                   g_aacpcm + (size_t)d->aac_taken * (size_t)d->channels,
                   (size_t)take * (size_t)d->channels * sizeof(short));
            d->aac_taken += (int)take;
            done += take;
        }
        return done;
    }

    switch (d->kind) {
        case DEC_FLAC:   return drflac_read_pcm_frames_s16(d->flac, want, out);
        case DEC_MP3:    return drmp3_read_pcm_frames_s16(&d->mp3, want, out);
        case DEC_WAV:    return drwav_read_pcm_frames_s16(&d->wav, want, out);
        case DEC_VORBIS: return vorbis_dec_read(&d->vorbis, out, want);
        case DEC_OPUS:   return opus_dec_read(&d->opus, out, want);
        default:         return 0;
    }
}

/* Forward declaration -- defined below, reused here for its cheap CBR bitrate
 * read (first valid frame header only, no decode) to support the approximate
 * seek just below. */
static int mp3_probe(const char *path, int *rate_out, int *bitrate_bps_out);

/* dr_mp3 has exactly two ways to seek: an O(1) lookup against a bound seek
 * table, or -- with none bound -- drmp3_seek_to_pcm_frame__brute_force(),
 * which is a "dumb read-and-discard" full decode of every frame from the
 * start of the file up to the target (dr_mp3's own comment, not this app's).
 * mp3_seektable_kickoff() only starts building that table once a track is
 * already open, on its own background thread, and building it costs exactly
 * the same full-file decode pass -- so a resume seek landing anywhere close
 * to the end of a long episode, on the very first open, had no fast option
 * either way: minutes of silent 100%-of-one-core decode before anything was
 * audible, and if the user switched tracks while that was running,
 * audio_stop()'s pthread_join() froze the whole UI thread for the same
 * span, since nothing inside that single blocking call checks g_running.
 *
 * First attempt at fixing this re-initialized d->mp3 on a manually fseek'd
 * FILE* via a custom drmp3_init() call, expecting decode to just resume from
 * wherever the file pointer already was. Reported live: the resume position
 * showed correctly (this app's own g_pos_ms is set from the *target*, not
 * from anything dr_mp3 reports), but the audio itself started from the
 * beginning every time. Root cause, found reading drmp3_init_internal()
 * itself: it unconditionally seeks to the end, then back to byte 0, to hunt
 * for ID3v1/APE/ID3v2 tags before it ever looks at real frame data --
 * completely undoing the pre-seek regardless of where the FILE* was
 * positioned on entry. drmp3_init() is simply not designed to resume
 * mid-stream; nothing publicly exposed makes it skip that dance.
 *
 * This instead reuses the seek-*table* mechanism dr_mp3 already has, which
 * bypasses drmp3_init() entirely: drmp3_seek_to_pcm_frame__seek_table()
 * raw-seeks the byte position from a bound drmp3_seek_point via the same
 * onSeek callback d->mp3 was opened with, drops any buffered read-ahead
 * (drmp3_reset()), discards a couple of leading MP3 frames to let the bit
 * reservoir settle, and is done -- no tag scan, no reset to the start.
 * Building a single synthetic point -- byte offset estimated from the
 * file's average byte rate, claimed PCM frame index equal to the seek
 * target itself -- makes the table-seek land in one step with nothing left
 * over to brute-force: drmp3_seek_to_pcm_frame__seek_table() computes
 * `leftoverFrames = frameIndex - currentPCMFrame`, and since currentPCMFrame
 * is set straight from this point's own claimed index, that difference is
 * zero. mp3_probe() already reads bitrate from the first real frame header
 * elsewhere in this file for exactly this kind of "close enough" estimate;
 * for CBR (the common case for a fixed-quality podcast export) it's exact,
 * and for VBR it's still within a fraction of a second per hour of drift --
 * nowhere near enough to be audible against spoken content. Landing a few
 * frames short or long of the mathematical target is an accepted, universal
 * trade-off of this technique, not a bug, because the alternative is the
 * multi-minute stall above.
 *
 * Only refreshed while seekPointCount <= 1 (nothing bound yet, or still just
 * a previous approximation of ours) -- once mp3_seektable_kickoff()'s
 * background build finishes and binds its real, many-point table, this
 * leaves it alone and every seek after that is both fast and exact. */
static int mp3_seek_approx(dec_t *d, const char *path, uint64_t targetFrame,
                            drmp3_seek_point **bound_points) {
    if (d->mp3.seekPointCount > 1) return 0;   /* a real table is already bound */
    if (!d->rate) return 0;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return 0;
    int bitrate_bps = 0, probe_rate = 0;
    if (mp3_probe(path, &probe_rate, &bitrate_bps) != 0 || bitrate_bps <= 0) return 0;
    /* Scale from streamStartOffset, not byte 0 -- a well-tagged rip's embedded
     * cover art can push the real audio data well into the file (the exact
     * mechanism behind the "Revolver" 0-kbps bug elsewhere in this file), and
     * scaling from the very start would land short by however big that tag
     * is, worse the larger the file. drmp3_init_file() already found this
     * exactly once, on d->mp3, before this function ever runs. */
    double target_ms = (double)targetFrame * 1000.0 / (double)d->rate;
    int64_t data_start = (int64_t)d->mp3.streamStartOffset;
    int64_t byte_off = data_start + (int64_t)(target_ms / 1000.0 * ((double)bitrate_bps / 8.0));
    if (byte_off < data_start) byte_off = data_start;
    if (byte_off > st.st_size - 4096) byte_off = st.st_size > 4096 ? st.st_size - 4096 : data_start;

    drmp3_seek_point *pt = malloc(sizeof(*pt));
    if (!pt) return 0;
    pt->seekPosInBytes = (drmp3_uint64)byte_off;
    pt->pcmFrameIndex = targetFrame;
    pt->mp3FramesToDiscard = 2;   /* DRMP3_SEEK_LEADING_MP3_FRAMES -- same margin
                                    * dr_mp3's own real seek points always carry,
                                    * to let the bit reservoir settle after
                                    * landing somewhere arbitrary */
    pt->pcmFramesToDiscard = 0;
    if (!drmp3_bind_seek_table(&d->mp3, 1, pt)) { free(pt); return 0; }
    /* drmp3_bind_seek_table() never frees the previous table itself (its own
     * doc: "Memory is owned by the client") -- this is the same ownership
     * worker() already tracks via bound_points for the background-built
     * table, reused here so whichever last replaced it is the one that gets
     * freed, by whichever call (this one, the real bind, or worker()'s own
     * final cleanup) replaces or releases it next. */
    free(*bound_points);
    *bound_points = pt;
    return 1;
}

static void dec_seek(dec_t *d, uint64_t frame, const char *path,
                      drmp3_seek_point **bound_points) {
    if (d->kind == DEC_M4A) {
        /* Access units are a fixed number of frames, so the index is just a
         * division; seeking is to the start of the unit containing it. */
        unsigned per = d->aac_frames > 0 ? (unsigned)d->aac_frames : 1024;
        mp4_seek(&d->mp4, (unsigned)(frame / per));
        d->aac_taken = d->aac_frames = 0;
        return;
    }
    switch (d->kind) {
        case DEC_VORBIS: vorbis_dec_seek(&d->vorbis, frame); break;
        case DEC_OPUS:   opus_dec_seek(&d->opus, frame);     break;
        case DEC_FLAC: drflac_seek_to_pcm_frame(d->flac, frame); break;
        case DEC_MP3:
            /* Binds an approximate table when nothing better is bound yet;
             * a no-op once the real background-built one is. Either way,
             * the actual seek is always this same one call. */
            mp3_seek_approx(d, path, frame, bound_points);
            drmp3_seek_to_pcm_frame(&d->mp3, frame);
            break;
        case DEC_WAV:  drwav_seek_to_pcm_frame(&d->wav, frame);  break;
        default: break;
    }
}

static void dec_close(dec_t *d) {
    if (d->is_hls) {
        aac_close(g_hls.aac);
        g_hls.aac = NULL;
        d->kind = DEC_NONE;
        return;
    }
    if (d->is_stream) {
        if (g_stream.pipe) { pclose(g_stream.pipe); g_stream.pipe = NULL; }
        d->kind = DEC_NONE;
        return;
    }
    if (d->kind == DEC_M4A) {
        if (d->alac) alac_close(d->alac); else aac_close(d->aac);
        mp4_close(&d->mp4);
        d->kind = DEC_NONE;
        return;
    }
    switch (d->kind) {
        case DEC_FLAC:   drflac_close(d->flac); break;
        case DEC_MP3:    drmp3_uninit(&d->mp3); break;
        case DEC_WAV:    drwav_uninit(&d->wav); break;
        case DEC_VORBIS: vorbis_dec_close(&d->vorbis); break;
        case DEC_OPUS:   opus_dec_close(&d->opus); break;
        default: break;
    }
    d->kind = DEC_NONE;
}

/* R28: a header-only duration probe for tracks the stock scanner's own
 * database never gives a duration for. MEDIA_TABLE's begin_time/end_time
 * columns turn out to be populated only for cue-sheet split tracks (an
 * audiobook sharing one physical file across several rows); an ordinary
 * one-file-one-track file -- the overwhelming majority of a real library
 * -- is left at -1 indefinitely, which is why durations were blank almost
 * everywhere rather than just for a stale minority.
 *
 * FLAC/WAV/Vorbis/Opus state their total sample count in the header, so
 * dec_open()+dec_close() gets an exact answer with no PCM decode -- safe
 * to call from any thread, since none of those four keep decode state
 * anywhere but the local dec_t this function owns.
 *
 * M4A (AAC or ALAC) deliberately does NOT go through dec_open() here: that
 * path primes the first access unit through alac_decode()/aac_decode()
 * into g_aacpcm, the same buffer the playback worker's own dec_read()
 * writes into for an M4A track that might be playing on another thread
 * right now -- a real data race, not a hypothetical one, since this can
 * run from the UI thread while a track is playing. mp4_duration_ms()
 * (already used by audiobook.c for exactly this reason) answers from the
 * container's own moov atom instead, never touching either decoder.
 *
 * MP3 has no cheap answer for a VBR file: dec_open() deliberately leaves
 * d->frames at 0 rather than decode the whole file to count frames (see
 * its own comment). Estimated instead from file size and the bitrate
 * already in the database -- exact for CBR, close for VBR, which is all
 * a list-row label needs. */
int audio_probe_dur_ms(const char *path, int bitrate_bps) {
    if (!path[0]) return 0;
    unsigned char m4a_check[8];
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, m4a_check, sizeof(m4a_check));
        close(fd);
        if (n == 8 && !memcmp(m4a_check + 4, "ftyp", 4))
            return (int)mp4_duration_ms(path);
    }

    dec_t d;
    if (dec_open(&d, path) != 0) return 0;
    int ms = 0;
    if (d.kind == DEC_MP3) {
        struct stat st;
        if (bitrate_bps > 0 && stat(path, &st) == 0)
            ms = (int)((int64_t)st.st_size * 8000 / bitrate_bps);
    } else if (d.frames && d.rate) {
        ms = (int)(d.frames * 1000 / d.rate);
    }
    dec_close(&d);
    return ms;
}

/* MP3's frame header carries its own bitrate and sample rate directly (a 4-
 * and a 2-bit table index respectively) -- the one piece of format info this
 * container answers without the "decode it to find out" problem d.frames=0
 * exists to avoid (see dec_open()'s own comment). Skips a leading ID3v2 tag
 * first, same size field tags.c's id3_all() already reads, then scans for
 * the sync word rather than assuming the tag's declared size lands exactly
 * on it -- padding and off-by-ones in the wild are common enough that "the
 * next 0xFFE" is the more reliable target. CBR gives an exact bitrate this
 * way; VBR gives whatever the first frame happened to be, which is the same
 * "close enough for a list row" this file already accepts for duration. */
static int mp3_probe(const char *path, int *rate_out, int *bitrate_bps_out) {
    static const int mpeg1_l3_kbps[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
    static const int mpeg2_l3_kbps[16] = { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0 };
    static const int mpeg1_rates[4] = { 44100, 48000, 32000, 0 };
    static const int mpeg2_rates[4] = { 22050, 24000, 16000, 0 };
    static const int mpeg25_rates[4] = { 11025, 12000, 8000, 0 };

    unsigned char hdr[10];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t hn = fread(hdr, 1, sizeof(hdr), f);
    if (hn < 10) { fclose(f); return -1; }

    /* Seek past the ID3v2 tag before reading the scan window, rather than
     * reading a fixed buffer from byte 0 and hoping the tag fits inside it.
     * A well-tagged rip's embedded cover art routinely pushes the tag itself
     * past a few hundred KB -- reported live on "Revolver", whose 0 kbps
     * traced to exactly this: a tag bigger than the old 8192-byte read, so
     * `start` (computed correctly from the tag's own size field) landed
     * past the end of the only bytes actually in memory, and the sync scan
     * below never ran at all. */
    size_t start = 0;
    if (!memcmp(hdr, "ID3", 3)) {
        uint32_t sz = ((uint32_t)(hdr[6] & 0x7F) << 21) | ((uint32_t)(hdr[7] & 0x7F) << 14) |
                     ((uint32_t)(hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
        start = 10 + sz;
    }
    if (fseek(f, (long)start, SEEK_SET) != 0) { fclose(f); return -1; }

    unsigned char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        int ver_bits = (buf[i + 1] >> 3) & 0x03;    /* 00=v2.5, 10=v2, 11=v1 */
        int layer_bits = (buf[i + 1] >> 1) & 0x03;   /* 01=Layer III */
        if (layer_bits != 0x01 || ver_bits == 0x01) continue;
        int br_idx = (buf[i + 2] >> 4) & 0x0F;
        int sr_idx = (buf[i + 2] >> 2) & 0x03;
        if (br_idx == 0 || br_idx == 15 || sr_idx == 3) continue;
        int kbps = (ver_bits == 0x03) ? mpeg1_l3_kbps[br_idx] : mpeg2_l3_kbps[br_idx];
        int rate = (ver_bits == 0x03) ? mpeg1_rates[sr_idx]
                 : (ver_bits == 0x02) ? mpeg2_rates[sr_idx] : mpeg25_rates[sr_idx];
        if (!kbps || !rate) continue;
        if (rate_out) *rate_out = rate;
        if (bitrate_bps_out) *bitrate_bps_out = kbps * 1000;
        return 0;
    }
    return -1;
}

/* BG106: reported live -- a VBR MP3 (a Martha Wainwright "Special Edition"
 * track) showed a flat "128 kbps" as if it were CBR. mp3_probe() above is
 * working as designed (its own comment already says a VBR file "gives
 * whatever the first frame happened to be" as a deliberate, cheap
 * approximation for a list row's duration estimate) -- the actual bug is
 * that nothing anywhere checks whether that number means anything before
 * showing it labelled as if it were the file's one true bitrate.
 *
 * A VBR encoder conventionally writes a "Xing" (or Fraunhofer's own
 * "VBRI") tag into the first frame announcing exactly that -- LAME
 * deliberately writes "Info" instead for a CBR/ABR file it also tags, so
 * "Info" is excluded here on purpose, not just an oversight. Same ID3v2-
 * skip and frame-sync scan as mp3_probe(), reused rather than shared code
 * because the two return genuinely different things (a number vs a yes/no)
 * and mp3_probe() already has three call sites depending on its exact
 * signature. The tag sits right after the frame header's own side-info
 * bytes, whose length depends on MPEG version and channel mode -- a fixed,
 * well-known table, not something derived here. Returns 1 if a VBR tag is
 * found, 0 for CBR/ABR/unreadable/non-MP3 alike -- there is no reliable
 * third answer to give a caller from a single frame. */
int audio_mp3_is_vbr(const char *path) {
    unsigned char hdr[10];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t hn = fread(hdr, 1, sizeof(hdr), f);
    if (hn < 10) { fclose(f); return 0; }

    size_t start = 0;
    if (!memcmp(hdr, "ID3", 3)) {
        uint32_t sz = ((uint32_t)(hdr[6] & 0x7F) << 21) | ((uint32_t)(hdr[7] & 0x7F) << 14) |
                     ((uint32_t)(hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);
        start = 10 + sz;
    }
    if (fseek(f, (long)start, SEEK_SET) != 0) { fclose(f); return 0; }

    unsigned char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
        int ver_bits = (buf[i + 1] >> 3) & 0x03;    /* 00=v2.5, 10=v2, 11=v1 */
        int layer_bits = (buf[i + 1] >> 1) & 0x03;   /* 01=Layer III */
        if (layer_bits != 0x01 || ver_bits == 0x01) continue;
        int br_idx = (buf[i + 2] >> 4) & 0x0F;
        int sr_idx = (buf[i + 2] >> 2) & 0x03;
        if (br_idx == 0 || br_idx == 15 || sr_idx == 3) continue;

        /* A real frame header -- this is the one mp3_probe() would also
         * have picked. The Xing/Info tag sits after the side-info bytes:
         * 32 for MPEG1 stereo/joint/dual, 17 for MPEG1 mono, 17 for
         * MPEG2/2.5 stereo/joint/dual, 9 for MPEG2/2.5 mono. */
        int mono = ((buf[i + 3] >> 6) & 0x03) == 0x03;
        int side_info = (ver_bits == 0x03) ? (mono ? 17 : 32) : (mono ? 9 : 17);
        size_t tag_off = i + 4 + (size_t)side_info;
        if (tag_off + 4 <= n &&
            (!memcmp(buf + tag_off, "Xing", 4) || !memcmp(buf + tag_off, "VBRI", 4)))
            return 1;
        /* VBRI is Fraunhofer's own tag and, unlike Xing/Info, always sits
         * at a fixed offset of 4 + 32 bytes regardless of channel mode --
         * it doesn't share Xing/Info's side-info-relative convention. Only
         * worth a second check when that differs from the one just tried. */
        size_t vbri_off = i + 4 + 32;
        if (vbri_off != tag_off && vbri_off + 4 <= n &&
            !memcmp(buf + vbri_off, "VBRI", 4))
            return 1;
        return 0;   /* a real frame with neither tag -- CBR, ABR, or a
                      * plain "Info"-tagged file; nothing further up this
                      * file scans past the first frame either. */
    }
    return 0;
}

/* Scanner-facing probe: what a database row needs, none of it decoded PCM.
 * FLAC/WAV/AIFF/Vorbis/Opus/M4A all state bits and (except MP3-adjacent
 * concerns don't apply here) an exact frame count via dec_open() alone, so
 * duration is exact and free for them. MP3 gets its rate/bitrate from
 * mp3_probe() above instead, and its duration is left 0 here -- the same
 * file-size/bitrate estimate audio_probe_dur_ms() already does is repeated
 * by whichever caller actually needs it (index.c's own scan pass, which
 * already calls audio_probe_dur_ms() itself; duplicating that estimate here
 * too would just be two places to keep in sync). MP3's bits is hardcoded 16 --
 * dec_open() leaves d.bits at 0 for it deliberately (no wide MP3 exists). */
int audio_probe_format(const char *path, int *bits, int *rate,
                       int *bitrate_bps, int *dur_ms) {
    dec_t d;
    memset(&d, 0, sizeof(d));
    /* M4A goes through dec_open_m4a_probe() instead of dec_open(), which for
     * M4A would call dec_open_m4a() -> m4a_prime() and touch the shared
     * g_aacpcm buffer -- unsafe here since this runs on scanner.c's
     * background scan thread, possibly concurrent with real M4A playback on
     * the worker thread. See dec_open_m4a_probe()'s own comment. */
    int rc = (sniff(path) == DEC_M4A) ? dec_open_m4a_probe(&d, path) : dec_open(&d, path);
    if (rc != 0) return -1;
    dec_kind_t kind = d.kind;
    if (rate) *rate = (int)d.rate;
    if (bits) *bits = (kind == DEC_MP3) ? 16 : d.bits;
    if (dur_ms) *dur_ms = (d.frames && d.rate) ? (int)(d.frames * 1000 / d.rate) : 0;
    if (kind != DEC_M4A) dec_close(&d);

    if (kind == DEC_MP3) {
        int r = 0, bps = 0;
        if (mp3_probe(path, &r, &bps) == 0) {
            if (rate) *rate = r;
            if (bitrate_bps) *bitrate_bps = bps;
        } else if (bitrate_bps) {
            *bitrate_bps = 0;
        }
    } else if (bitrate_bps) {
        struct stat st;
        *bitrate_bps = (dur_ms && *dur_ms > 0 && stat(path, &st) == 0)
                      ? (int)((int64_t)st.st_size * 8000 / *dur_ms) : 0;
    }
    return 0;
}

/* ---- ALSA, dlopen'd ------------------------------------------------------ */
#define SND_PCM_STREAM_PLAYBACK       0
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_FORMAT_S16_LE         2

typedef long  snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

static void *g_alsa;
static int (*x_open)(void **, const char *, int, int);
static int (*x_close)(void *);
static int (*x_drop)(void *);
static int (*x_drain)(void *);
static snd_pcm_sframes_t (*x_writei)(void *, const void *, snd_pcm_uframes_t);
static int (*x_recover)(void *, int, int);
static int (*x_hwp_malloc)(void **);
static void (*x_hwp_free)(void *);
static int (*x_hwp_any)(void *, void *);
static int (*x_hwp_set_access)(void *, void *, int);
static int (*x_hwp_set_format)(void *, void *, int);
static int (*x_hwp_set_channels)(void *, void *, unsigned);
static int (*x_hwp_set_rate_near)(void *, void *, unsigned *, int *);
static int (*x_hwp_set_buffer_time_near)(void *, void *, unsigned *, int *);
static int (*x_hwp_set_period_time_near)(void *, void *, unsigned *, int *);
static int (*x_hwp_apply)(void *, void *);
static int (*x_hwp_get_buffer_size)(void *, unsigned long *);
static int (*x_hwp_get_period_size)(void *, unsigned long *, int *);
static int (*x_prepare)(void *);
static int (*x_swp_malloc)(void **);
static void (*x_swp_free)(void *);
static int (*x_swp_current)(void *, void *);
static int (*x_swp_set_start_threshold)(void *, void *, snd_pcm_uframes_t);
static int (*x_swp_apply)(void *, void *);

#define SYM(h, p, name) do { \
    *(void **)(&p) = dlsym(h, name); \
    if (!p) { alog("[audio] missing %s\n", name); return -1; } \
} while (0)

static int load_libs(void) {
    if (g_alsa) return 0;
    {
        g_alsa = dlopen("libasound.so.2", RTLD_LAZY);
        if (!g_alsa) g_alsa = dlopen("libasound.so", RTLD_LAZY);
        if (!g_alsa) { alog("[audio] no libasound\n"); return -1; }
        SYM(g_alsa, x_open,  "snd_pcm_open");
        SYM(g_alsa, x_close, "snd_pcm_close");
        SYM(g_alsa, x_drop,  "snd_pcm_drop");
        /* Optional: without it a format change clips the tail, which is a
         * great deal better than failing to load. */
        *(void **)(&x_drain) = dlsym(g_alsa, "snd_pcm_drain");
        SYM(g_alsa, x_writei,"snd_pcm_writei");
        SYM(g_alsa, x_recover,"snd_pcm_recover");
        SYM(g_alsa, x_hwp_malloc,"snd_pcm_hw_params_malloc");
        SYM(g_alsa, x_hwp_free,  "snd_pcm_hw_params_free");
        SYM(g_alsa, x_hwp_any,   "snd_pcm_hw_params_any");
        SYM(g_alsa, x_hwp_set_access,  "snd_pcm_hw_params_set_access");
        SYM(g_alsa, x_hwp_set_format,  "snd_pcm_hw_params_set_format");
        SYM(g_alsa, x_hwp_set_channels,"snd_pcm_hw_params_set_channels");
        SYM(g_alsa, x_hwp_set_rate_near,"snd_pcm_hw_params_set_rate_near");
        SYM(g_alsa, x_hwp_set_buffer_time_near, "snd_pcm_hw_params_set_buffer_time_near");
        SYM(g_alsa, x_hwp_set_period_time_near, "snd_pcm_hw_params_set_period_time_near");
        SYM(g_alsa, x_hwp_apply, "snd_pcm_hw_params");
        *(void **)(&x_hwp_get_buffer_size) = dlsym(g_alsa, "snd_pcm_hw_params_get_buffer_size");
        *(void **)(&x_hwp_get_period_size) = dlsym(g_alsa, "snd_pcm_hw_params_get_period_size");
        SYM(g_alsa, x_prepare, "snd_pcm_prepare");
        SYM(g_alsa, x_swp_malloc, "snd_pcm_sw_params_malloc");
        SYM(g_alsa, x_swp_free,   "snd_pcm_sw_params_free");
        SYM(g_alsa, x_swp_current,"snd_pcm_sw_params_current");
        SYM(g_alsa, x_swp_set_start_threshold, "snd_pcm_sw_params_set_start_threshold");
        SYM(g_alsa, x_swp_apply,  "snd_pcm_sw_params");
    }
    return 0;
}

/* ---- state --------------------------------------------------------------- */
static pthread_t       g_thread;
static int             g_thread_valid;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int   g_running, g_active, g_paused;
static int   g_pos_ms, g_dur_ms;
static int   g_vol = 70;
static char  g_path[512];
static char  g_next_path[512];      /* queued by the UI, taken at the boundary */
static int   g_advance;             /* bumped each time the worker rolls on */
/* Set by audio_skip_to(): the worker takes it as an immediate end of track,
 * so a skip reuses the gapless handover instead of closing the device. */
static int   g_skip_now;
static int   g_speed = 1000;        /* permille, WSOLA time-stretch; 1000 = bypass */
/* R29: raw abs-sample peak of the most recent chunk actually written to the
 * output, computed in the decode worker right after the volume gain that
 * already runs there and published under g_lock in the same critical
 * section as g_pos_ms just below it -- no extra locking cost, since that
 * lock is already taken there every chunk regardless. Deliberately not
 * scaled to any fixed 0..N range: the two source paths (16-bit `short`,
 * hires `int32_t` shifted for whatever S24_LE/S32_LE the device opened)
 * have different natural magnitudes, so audio_current_peak() hands back
 * the raw value and callers building a waveform normalize it against their
 * own observed max over a whole track -- scale-invariant, and it doesn't
 * need to know which path produced it. */
static int32_t g_last_peak;

/* ---- output routing ------------------------------------------------------ */
/* An A2DP sink shows up as a bluealsa PCM ending in /sink. */
static int bt_sink_connected(void) {
    FILE *p = popen("bluealsa-cli list-pcms 2>/dev/null", "r");
    if (!p) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), p))
        if (strstr(line, "a2dp") && strstr(line, "/sink")) { found = 1; break; }
    pclose(p);
    return found;
}

/* A USB DAC enumerates as its own ALSA card, so plughw:0,0 — the internal
 * CS43131 — would keep feeding the headphone jack while the amp sits silent.
 *
 * Read /sys/class/sound, not /proc/asound: this kernel is built without ALSA's
 * procfs, so /proc/asound does not exist and a check against it silently never
 * fires. Each card symlinks to its device; the built-in one lands under
 * platform/hiby-hifi-board and anything on USB has "usb" in the path. */
static int usb_card(void) {
    for (int n = 0; n <= 8; n++) {
        char path[64], link[512], pcm[64];
        snprintf(path, sizeof(path), "/sys/class/sound/card%d", n);
        ssize_t k = readlink(path, link, sizeof(link) - 1);
        if (k <= 0) continue;
        link[k] = '\0';
        if (!strstr(link, "usb") && !strstr(link, "USB")) continue;
        snprintf(pcm, sizeof(pcm), "/sys/class/sound/pcmC%dD0p", n);
        if (access(pcm, F_OK) != 0) continue;      /* no playback device */
        return n;
    }
    return -1;
}

static int  g_out_kind;              /* 0 wired, 1 usb, 2 bluetooth */
/* Settings' "disable PEQ, MSEB and Bluetooth when playing over USB" -- the
 * Bluetooth-radio half of that is music_hook.c's job (it owns st_bt_set()
 * and the quick-settings toggle already), this flag is only the DSP half. */
static int  g_usb_bypass;
void audio_set_usb_bypass(int on) { g_usb_bypass = on; }
int  audio_usb_bypass(void)       { return g_usb_bypass; }
static int  g_out_card = -1;
static unsigned g_out_rate;          /* what pcm_open() actually negotiated */

/* BG41: a hi-res source (88.2/96/176.4/192 kHz) opened straight over Bluetooth
 * makes bluealsa's own PCM plugin resample it in software to whatever the a2dp
 * link really carries -- accepted silently, at whatever rate is asked for, so
 * nothing here ever saw an open fail or fall back. That resample runs in this
 * thread (it is a linked-in ALSA I/O plugin, not a separate process) and is
 * exactly the "self-inflicted" cost the pcm_open comment already predicted for
 * plughw; over Bluetooth it lands on top of decode and, if PEQ is on, the
 * EQ cascade too, with the CPU log showing decode alone climbing from the
 * usual sub-10% to ~70% of the single core once a 192 kHz PEQ track has been
 * playing a while -- little enough headroom left that an ordinary hiccup
 * produces the underrun and the audible quality the headset's own adaptive
 * bitrate then reacts to by stepping down. No Bluetooth codec on this device
 * carries more than 96 kHz of real information regardless, so nothing is
 * actually thrown away by not asking for the full source rate.
 *
 * Restricted to a clean 2x/4x divide of a standard 44.1/48 kHz base -- every
 * real hi-res file is one of those four multiples, and an exact integer ratio
 * is what makes the plain box-car decimation in the worker loop correct
 * without needing a general resampler: no fractional-position state to carry
 * between chunks, so no cross-chunk phase or click risk. Anything else (an
 * odd source rate, or already <=48 kHz) passes through unchanged, same as
 * today. */
static unsigned bt_target_rate(unsigned src_rate) {
    if (src_rate == 88200 || src_rate == 176400) return 44100;
    if (src_rate == 96000 || src_rate == 192000) return 48000;
    return src_rate;
}

/* "Headphones" was the wrong word for the wired route: it is the internal DAC
 * feeding the 3.5 mm socket, and it showed even with nothing plugged in, which
 * reads as a claim about the jack rather than about where the stream is going.
 * The socket is the honest name. */
static const char *out_label(void) {
    return g_out_kind == 2 ? "Bluetooth" : g_out_kind == 1 ? "USB" : "3.5 mm";
}

const char *audio_output(void) { return out_label(); }
const char *audio_codec(void)  { return g_codec; }
int audio_is_exact(void) { return g_exact; }
int audio_output_lost(void) { return g_out_lost; }
int audio_using_bt(void)  { return g_out_kind == 2; }
int audio_using_usb(void) { return g_out_kind == 1; }

/* Bluetooth has a real mixer, unlike the wired path where the CS43131's volume
 * registers are not wired up and samples have to be scaled in software. The
 * element is named after the connected device, so it is looked up by name and
 * re-looked whenever it stops answering — a different headset since last
 * time would otherwise leave the volume keys apparently dead. */
static char bt_mixer[64];
/* Backoff state for find_bt_mixer(), reset whenever Bluetooth stops being
 * the output so a different headset next time is probed promptly. See
 * audio_bt_volume_service() for why this is needed at all. */
static int    bt_mixer_misses;
static time_t bt_mixer_next_try;
/* BG93: a single failed bt_read_raw() used to clear bt_mixer immediately,
 * indistinguishable from the headset actually being gone -- and once
 * bt_mixer_misses/next_try above existed, that meant one transient amixer
 * hiccup (a momentary D-Bus timeout, a race with a write that just landed)
 * could throw volume control into the same up-to-60s backoff a real
 * disconnect gets, reading as "stopped working" for however long that
 * lasted. Counted instead of acted on immediately -- see its use below. */
static int    bt_read_fail_count;
#define BT_READ_FAIL_LIMIT 2   /* consecutive failures before treating the
                                 * mixer as actually gone, not just a blip */

/* R90: reported live, twice -- "goes down 5% then back up 1%" on every
 * press, holding a button not ramping smoothly, and the first press after
 * connecting landing as a dramatic *increase*. All three trace to the same
 * root cause: this app tracked Bluetooth volume in percent and asked amixer
 * to translate that to/from the mixer's real 0-127 AVRCP steps on every
 * write *and* every read -- two independent roundings, neither reliably
 * the inverse of the other, so a set-then-read-back round trip routinely
 * didn't return what was just requested. A tolerance band around the
 * expected rounding noise was tried first and still wasn't enough to stop
 * it being visible. Fixed properly instead: track the mixer's own raw
 * integer directly, step by a raw amount, write the raw integer with
 * amixer's plain (non-percent) form, and read the raw integer back --
 * zero rounding anywhere in the loop, so a write and its own readback are
 * now identical by construction, not just close. g_vol (0-100) still
 * exists for the on-screen percentage and every other output path's
 * software-gain use of it; for Bluetooth it is now purely a *display*
 * conversion of bt_vol_raw, never the value actually being stepped. */
static int bt_vol_raw = -1;      /* -1 = not yet read this connection */
static int bt_vol_max  = 127;    /* AVRCP's own range; re-read per mixer in
                                   * case a future device differs */
/* How many equal steps a single volume-key press moves across bt_vol_max --
 * a Settings choice (16/32/64/128), not derived from percent at all. 128 is
 * the mixer's real per-step resolution; the others are coarser/fewer-taps
 * options. Persisted by the caller (music_hook.c), not here. */
static int bt_vol_steps = 32;
void audio_set_bt_vol_steps(int n) {
    if (n != 16 && n != 32 && n != 64 && n != 128) n = 32;
    bt_vol_steps = n;
}
int audio_bt_vol_steps(void) { return bt_vol_steps; }
int audio_bt_vol_raw(void) { return bt_vol_raw; }
int audio_bt_vol_max(void) { return bt_vol_max; }

static int bt_raw_to_pct(int raw) {
    return bt_vol_max > 0 ? (raw * 100 + bt_vol_max / 2) / bt_vol_max : 0;
}
static int bt_pct_to_raw(int pct) {
    return (pct * bt_vol_max + 50) / 100;
}

static void find_bt_mixer(void) {
    bt_mixer[0] = '\0';
    bt_vol_raw = -1;
    FILE *p = popen("amixer -D bluealsa scontrols 2>/dev/null", "r");
    if (!p) return;
    char line[256];
    while (fgets(line, sizeof(line), p)) {
        char *q1 = strchr(line, '\'');
        char *q2 = q1 ? strchr(q1 + 1, '\'') : NULL;
        if (q1 && q2 && q2 > q1 + 1) {
            size_t n = (size_t)(q2 - q1 - 1);
            if (n >= sizeof(bt_mixer)) n = sizeof(bt_mixer) - 1;
            memcpy(bt_mixer, q1 + 1, n);
            bt_mixer[n] = '\0';
            break;
        }
    }
    pclose(p);
    /* BG59: silent before this -- a headset that never negotiates AVRCP
     * absolute volume left no trace anywhere that this ever ran, let alone
     * that it found nothing. Logged once per attempt (this only runs when
     * bt_mixer is already empty, so it can't spam once one is found). */
    if (!bt_mixer[0]) alog("[audio] bt: no AVRCP mixer control found (amixer -D bluealsa scontrols empty)\n");
}

/* Reads the control's own raw integer (not the percent amixer derives from
 * it) -- "Front Left: Playback 65 [51%] ..." -> 65. Also re-reads the
 * control's own advertised range from its "Limits: Playback 0 - 127" line
 * when max_out is given, rather than assuming 127 forever: this is what
 * AVRCP absolute volume specifies and what's been observed live, but
 * nothing stops a future device or bluealsa version reporting something
 * else, and trusting its own stated range costs one more sscanf. */
static int bt_read_raw(int *max_out) {
    if (!bt_mixer[0]) return -1;
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "amixer -D bluealsa sget '%s' 2>/dev/null", bt_mixer);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char line[256];
    int raw = -1, max = -1;
    while (fgets(line, sizeof(line), p)) {
        int lo, hi;
        if (sscanf(line, " Limits: Playback %d - %d", &lo, &hi) == 2 && hi > 0) {
            max = hi;
            /* R90 follow-up: reported live as hardware buttons collapsing
             * volume straight to 0 and staying there, and the on-screen
             * value showing 0%/muted -- this line also contains the
             * substring "Playback ", and sscanf("0 - 127", "Playback %d
             * [", &v) returns 1 (v=0) even though the trailing "[" never
             * matched: a literal mismatch after a successful %d stops
             * scanning, it does not un-assign what %d already wrote or
             * change the return count. raw ended up latched at 0 from
             * *this* line, before the real "Front Left: Playback 65
             * [51%]" line below it was ever reached -- every read came
             * back 0 regardless of the mixer's actual position. Skipping
             * straight to the next line once this one's already been
             * used for the range keeps it from ever reaching that parse
             * at all. */
            continue;
        }
        if (raw < 0) {
            char *b = strstr(line, "Playback ");
            if (b) {
                int v; char bracket = 0;
                /* %c doesn't skip leading whitespace itself, so the
                 * literal space in the format does that job -- requiring
                 * the very next character to be '[' is what tells this
                 * apart from a line shaped like the Limits one above,
                 * regardless of exactly how either is phrased. */
                if (sscanf(b, "Playback %d %c", &v, &bracket) == 2 && bracket == '[')
                    raw = v;
            }
        }
    }
    pclose(p);
    if (max_out && max > 0) *max_out = max;
    return raw;
}

/* One entry point for a volume change, so the caller does not have to know
 * which route is live. */
/* Absolute set, for the slider. The wired path scales samples in software;
 * on Bluetooth the headset has a real mixer, stepped in its own raw units
 * (see this section's own top comment for why).
 *
 * Neither of these talks to bluealsa directly any more. Both `system()` here
 * and the `popen()` readback are blocking D-Bus round-trips, and the volume
 * keys are read on the same thread that draws every frame and processes
 * every touch — so a headset dropping mid-call (see the "Bluetooth
 * disconnects" symptom this was chasing) froze the whole player with the
 * screen already dark and nothing left to recover it but a hard reset. This
 * is the same bug class the codec/battery poll was already moved off the UI
 * thread for; volume control just never got the same treatment. The actual
 * write happens in audio_bt_volume_service(), on bt_poll's background
 * thread — these two only ever set a pending request and apply an optimistic
 * g_vol update so the display still feels instant. */
static pthread_mutex_t bt_vol_lock = PTHREAD_MUTEX_INITIALIZER;
static int bt_vol_pending;            /* 1 = a write is waiting to be applied */
static int bt_vol_pending_raw;

/* USB Transport Mode's volume lock -- see audio_set_vol_locked()'s own
 * comment in audio.h. Deliberately doesn't gate audio_set_volume() itself:
 * that's the low-level setter the lock's own owner uses to establish the
 * pinned value in the first place, so gating it there would make locking
 * unable to set the value it's supposed to hold. */
static int g_vol_locked;
void audio_set_vol_locked(int on) { g_vol_locked = on; }

/* BG90 follow-up -- see audio_set_screen_locked()'s own comment in audio.h.
 * Plain unguarded int: set from the UI thread (set_locked()), read from the
 * worker thread, both single-word accesses -- the same benign-race class
 * already accepted elsewhere in this file (bt_mixer[0], g_out_kind), where
 * a stale read for one tick just means the skip (or the check) catches up
 * next tick, not a real race. */
static int g_screen_locked;
void audio_set_screen_locked(int on) { g_screen_locked = on; }

void audio_volume_set(int pct) {
    if (g_vol_locked) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (!audio_using_bt()) { audio_set_volume(pct); return; }
    int raw = bt_pct_to_raw(pct);
    if (raw < 0) raw = 0; if (raw > bt_vol_max) raw = bt_vol_max;
    bt_vol_raw = raw;
    pthread_mutex_lock(&bt_vol_lock);
    bt_vol_pending = 1; bt_vol_pending_raw = raw;
    pthread_mutex_unlock(&bt_vol_lock);
    /* The slider asked for this exact percentage -- show it directly
     * rather than round-tripping through bt_raw_to_pct(), which could
     * legitimately land one point off given bt_vol_max isn't always a
     * clean divisor of 100. */
    pthread_mutex_lock(&g_lock); g_vol = pct; pthread_mutex_unlock(&g_lock);
    /* BG94: see audio_volume_step()'s matching comment. */
    if (!bt_mixer[0]) bt_mixer_next_try = 0;
}

void audio_volume_step(int delta) {
    if (g_vol_locked) return;
    if (!audio_using_bt()) {
        int v = audio_volume() + delta;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        audio_set_volume(v);
        return;
    }
    /* R90: the step size is bt_vol_max/bt_vol_steps raw units -- a user
     * Settings choice, not derived from `delta` (still ±5, the same call
     * every other output path uses; only its *sign* matters here). A
     * fresh connection with no raw reading yet (-1) starts from whatever
     * g_vol last showed, converted once -- the best available guess until
     * the next mixer discovery corrects it for real (see
     * audio_bt_volume_service()). Base state is bt_vol_raw itself, not a
     * value re-derived from g_vol every press: g_vol accumulating a
     * fractional raw step every time it got converted back and forth was
     * exactly the old bug. */
    int base = bt_vol_raw >= 0 ? bt_vol_raw : bt_pct_to_raw(audio_volume());
    int step = bt_vol_max / bt_vol_steps; if (step < 1) step = 1;
    int raw = base + (delta > 0 ? step : delta < 0 ? -step : 0);
    if (raw < 0) raw = 0; if (raw > bt_vol_max) raw = bt_vol_max;
    bt_vol_raw = raw;
    pthread_mutex_lock(&g_lock);
    g_vol = bt_raw_to_pct(raw);
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&bt_vol_lock);
    bt_vol_pending = 1; bt_vol_pending_raw = raw;
    pthread_mutex_unlock(&bt_vol_lock);
    /* BG94: reported as hardware volume buttons lagging several seconds
     * behind a press, sometimes matching the on-screen slider and sometimes
     * not. audio_bt_volume_service() only re-runs find_bt_mixer() on its own
     * schedule (bt_mixer_next_try, BG93's backoff) -- a pending request that
     * arrives while bt_mixer is still empty (freshly connected, or between
     * scheduled retries) just sits there doing nothing until that timer
     * comes back around on its own, up to the full 5s early-retry interval
     * or 60s once settled. A real button press is exactly the signal worth
     * retrying *now* for, rather than waiting out a schedule that was only
     * ever meant to bound how often an unattended, no-one's-pressing-
     * anything probe repeats. Racing this against audio_bt_volume_service()
     * itself (which only ever runs on the bt_poll thread) is the same
     * class of benign single-word race bt_mixer[0]'s own read elsewhere in
     * this file already accepts -- worst case this write loses a race and
     * next tick's own check catches it anyway. */
    if (!bt_mixer[0]) bt_mixer_next_try = 0;
}

/* True if a write is waiting, so the background thread can check cheaply
 * (no amixer round-trip) between its normal ~5s polling ticks. */
int audio_bt_volume_pending(void) {
    pthread_mutex_lock(&bt_vol_lock);
    int p = bt_vol_pending;
    pthread_mutex_unlock(&bt_vol_lock);
    return p;
}

/* Background-thread only. Applies one pending mixer write if there is one,
 * then always reads back — both because the headset's own buttons move the
 * mixer without a key event reaching this app at all (bluealsa applies
 * --a2dp-volume directly), and because a step or set may have landed between
 * a notch, and g_vol should show what actually took, not the request. Both
 * the write and the readback are the mixer's own raw integer now (see this
 * section's own top comment) -- no percent conversion anywhere in this
 * round trip, so a write this app just made and its own readback are
 * identical by construction, not merely close. */
void audio_bt_volume_service(void) {
    if (!audio_using_bt()) {
        bt_mixer_misses = 0; bt_mixer_next_try = 0; bt_read_fail_count = 0;
        return;
    }

    int pending, raw_val;
    pthread_mutex_lock(&bt_vol_lock);
    pending = bt_vol_pending; raw_val = bt_vol_pending_raw;
    bt_vol_pending = 0;
    pthread_mutex_unlock(&bt_vol_lock);

    /* Back off when there's nothing to find. find_bt_mixer() is a popen()
     * of `amixer -D bluealsa scontrols` -- a fork+exec plus ALSA/D-Bus work
     * -- and this service runs about every 5s for as long as Bluetooth is
     * connected. A headset that simply doesn't do AVRCP absolute volume
     * leaves bt_mixer empty permanently, so the "only runs when bt_mixer is
     * empty" guard the original relied on to prevent spam never fires: it
     * prevents re-probing after *success*, not after persistent failure.
     * Confirmed live -- music.log carried this probe and its "no AVRCP
     * mixer control found" line every 5.1s, indefinitely, competing with
     * decode and (downstream, on the same single core) bluealsa's own
     * encoder. Retry quickly a few times, since a mixer can appear a moment
     * after connect, then settle to once a minute. The backoff state
     * resets whenever Bluetooth isn't the output, so a different headset
     * still gets probed promptly -- which is the case the original comment
     * about re-looking actually cared about. */
    if (!bt_mixer[0]) {
        time_t now = time(NULL);
        if (now >= bt_mixer_next_try) {
            find_bt_mixer();
            if (!bt_mixer[0]) {
                if (bt_mixer_misses < 1000) bt_mixer_misses++;
                bt_mixer_next_try = now + (bt_mixer_misses < 3 ? 5 : 60);
            } else {
                bt_mixer_misses = 0; bt_mixer_next_try = 0;
                bt_read_fail_count = 0;
                /* R90: reported live as the first volume-down press after
                 * connecting a Jabra Elite 4 Active landing as a dramatic
                 * *increase* instead. This control was just discovered --
                 * never read from yet -- and any write already queued while
                 * it was still unknown (bt_vol_pending, e.g. from a button
                 * pressed in the couple of seconds before AVRCP negotiated)
                 * was computed against whatever g_vol happened to be left
                 * over from before this headset connected, not this
                 * headset's own actual current volume. Sync to reality
                 * first and apply the pending write next tick (still
                 * queued, ~100ms away, not lost) against a mixer this
                 * function has actually read from at least once, rather
                 * than writing blind to a control with an unconfirmed
                 * range/current value. */
                int max = bt_vol_max;
                int fresh = bt_read_raw(&max);
                if (fresh >= 0) {
                    bt_vol_max = max;
                    bt_vol_raw = fresh;
                    pthread_mutex_lock(&g_lock);
                    g_vol = bt_raw_to_pct(fresh);
                    pthread_mutex_unlock(&g_lock);
                }
                return;
            }
        }
    }
    if (!bt_mixer[0]) return;

    if (pending) {
        char cmd[224];
        /* Always an absolute target, never a relative step -- see
         * audio_volume_step()'s comment for why a batched relative step
         * against the mixer's own current value could disagree with g_vol
         * by tens of percent. Plain integer, not "N%" -- see this
         * section's own top comment for why. */
        snprintf(cmd, sizeof(cmd), "amixer -D bluealsa sset '%s' %d >/dev/null 2>&1",
                 bt_mixer, raw_val);
        if (system(cmd) == -1) return;
    }

    int max = bt_vol_max;
    int raw = bt_read_raw(&max);
    if (raw < 0) {
        /* BG93: one failed read no longer means "gone" -- only
         * BT_READ_FAIL_LIMIT in a row does, so a lone transient hiccup
         * costs nothing beyond skipping this poll's g_vol readback. */
        if (++bt_read_fail_count >= BT_READ_FAIL_LIMIT) {
            bt_mixer[0] = '\0';
            bt_read_fail_count = 0;
        }
        return;
    }
    bt_read_fail_count = 0;
    bt_vol_max = max;
    bt_vol_raw = raw;
    pthread_mutex_lock(&g_lock);
    g_vol = bt_raw_to_pct(raw);
    pthread_mutex_unlock(&g_lock);
}

/* ALSA's own numbering. */
#define FMT_S16_LE 2
#define FMT_S24_LE 6
#define FMT_S32_LE 10

/* Open the device, asking for the source's own format and rate.
 *
 * The device tried first is hw:, which has no conversion layer at all: what is
 * written is what the DAC receives. plughw: is kept only as a fallback, and it
 * is worth knowing what it costs — it will silently resample and requantise to
 * whatever suits it, so anything opened through it is not bit-perfect however
 * good the file was. The internal DAC accepts S24_LE and every rate from 44.1
 * to 384 kHz, so the fallback should be rare.
 *
 * Bluetooth is the exception: the A2DP transport is S16_LE whatever is asked,
 * so it goes through the plug device as before.
 */
static void *pcm_open(unsigned rate, int channels, int deep, int want_fmt) {
    void *pcm = NULL;
    char exact[24], plug[24];

    /* BG90: priority is USB > Bluetooth > 3.5mm jack -- an external DAC/amp
     * on USB takes precedence over everything, Bluetooth next, the built-in
     * jack (or speaker, if nothing's plugged into it) last as the always-
     * available fallback. Previously the jack outranked both, on the
     * reasoning that a paired headset in the next room shouldn't win over
     * headphones physically plugged in just now -- true as far as it went,
     * but it also meant Bluetooth headphones put on right now lost to a jack
     * nothing was using, which is backwards from what a user reaching for
     * Bluetooth actually wants. No jack check gates USB/Bluetooth anymore;
     * the jack no longer needs asking about at all here, since it never
     * outranks either and it's still exactly what "wired" (kind 0) plays out
     * of whenever neither of the other two applies. */
    int ucard  = usb_card();
    int use_bt = (ucard <= 0) ? bt_sink_connected() : 0;
    g_out_kind = ucard > 0 ? 1 : (use_bt ? 2 : 0);
    g_out_card = ucard;
    if (use_bt) rate = bt_target_rate(rate);   /* BG41 -- see bt_target_rate() */

    int card = ucard > 0 ? ucard : 0;
    snprintf(exact, sizeof(exact), "hw:%d,0", card);
    snprintf(plug,  sizeof(plug),  "plughw:%d,0", card);

    const char *names[4];
    int fmts[4];
    unsigned count = 0;
    if (use_bt) {
        /* Pin the profile. Since HFP was enabled there are three PCMs on the
         * headset — a2dpsrc/sink plus hfpag/sink and /source — and the bare
         * "bluealsa" device can resolve to the HFP one, which expects
         * narrowband mono. Feeding it 44.1 kHz stereo comes out as static. */
        names[count] = "bluealsa:PROFILE=a2dp"; fmts[count++] = FMT_S16_LE;
        names[count] = "bluealsa"; fmts[count++] = FMT_S16_LE;
        /* Advertised but unopenable should not mean silence. */
        names[count] = plug; fmts[count++] = FMT_S16_LE;
    } else {
        names[count] = exact; fmts[count++] = want_fmt;       /* bit-perfect */
        /* A second bit-perfect attempt, in S32_LE, before giving up and
         * converting. The internal DAC takes S24_LE and never reaches this,
         * but a USB DAC often advertises S32_LE and *not* S24_LE -- and when
         * the exact open fails the next candidate is plughw, where libasound
         * requantises every sample in software. At 96 or 192 kHz that is a
         * per-sample cost on a 1 GHz in-order core, which is the shape of
         * BG12: distortion from missed deadlines, a warm device from the
         * sustained load, and a starved UI while the raised-priority audio
         * thread keeps the hardware keys alive. Costs nothing when the first
         * attempt already worked. */
        if (want_fmt == FMT_S24_LE) { names[count] = exact; fmts[count++] = FMT_S32_LE; }
        names[count] = plug;  fmts[count++] = want_fmt;       /* converted */
        names[count] = plug;  fmts[count++] = FMT_S16_LE;     /* last resort */
    }

    for (unsigned i = 0; i < count; i++) {
        if (x_open(&pcm, names[i], SND_PCM_STREAM_PLAYBACK, 0) < 0 || !pcm) {
            pcm = NULL;
            continue;
        }
        void *hw = NULL;
        if (x_hwp_malloc(&hw) < 0 || !hw) { x_close(pcm); pcm = NULL; continue; }
        x_hwp_any(pcm, hw);
        x_hwp_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        int ok = x_hwp_set_format(pcm, hw, fmts[i]) >= 0;
        if (ok) ok = x_hwp_set_channels(pcm, hw, (unsigned)channels) >= 0;
        unsigned r = rate;
        if (ok) ok = x_hwp_set_rate_near(pcm, hw, &r, NULL) >= 0;
        /* An exact open that had to move the rate is not exact. */
        if (ok && i == 0 && !use_bt && r != rate) ok = 0;
        unsigned buf_us = deep ? 2000000 : 500000;
        unsigned per_us = deep ?  250000 : 100000;
        if (ok) x_hwp_set_buffer_time_near(pcm, hw, &buf_us, NULL);
        if (ok) x_hwp_set_period_time_near(pcm, hw, &per_us, NULL);
        if (ok) ok = x_hwp_apply(pcm, hw) >= 0;
        if (!ok) { x_hwp_free(hw); x_close(pcm); pcm = NULL; continue; }

        /* Which *device* was opened, not which index: the exact device now has
         * two candidate formats, so "i == 0" no longer means bit-perfect. Both
         * entries point at the same `exact` buffer, so this compares pointers. */
        g_exact = (!use_bt && names[i] == exact);
        g_out_fmt = fmts[i];
        g_out_rate = r;
        if (use_bt && i > 0 && names[i] != exact) g_out_kind = 0;   /* fell back to the jack */
        alog("[audio] %s %u Hz %d ch %s%s\n", names[i], r, channels,
             fmts[i] == FMT_S32_LE ? "S32_LE" :
             fmts[i] == FMT_S24_LE ? "S24_LE" : "S16_LE",
             g_exact ? " (exact)" : " (converted)");
        x_hwp_free(hw);
        break;
    }
    if (!pcm) { alog("[audio] snd_pcm_open failed\n"); return NULL; }

    /* Sizing the buffer is not optional. Left at the default this opened
     * happily, took about eleven seconds of audio, and then parked forever in
     * wait_for_avail: the stream never started, so nothing ever drained. */

    /* start_threshold 1: begin playing as soon as there is a frame, rather
     * than waiting for the buffer to fill. */
    void *sw = NULL;
    if (x_swp_malloc(&sw) >= 0 && sw) {
        x_swp_current(pcm, sw);
        x_swp_set_start_threshold(pcm, sw, 1);
        x_swp_apply(pcm, sw);
        x_swp_free(sw);
    }
    x_prepare(pcm);
    return pcm;
}

#define CHUNK_FRAMES 2048

/* How long a pause has to last before the playback device is handed back, in
 * 60 ms poll ticks -- about five seconds. Long enough that scrubbing, or a
 * pause to say something, does not audibly cycle the DAC; short enough that a
 * device put down and forgotten stops drawing through the amp. */
#define PCM_IDLE_CLOSE_TICKS 83
/* BG90: bt_sink_connected() forks a `bluealsa-cli` subprocess, unlike the
 * plain readlink usb_card() does, so it is checked once every this-many
 * passes of the ~1s-cadence poll below rather than every pass.
 *
 * Was 5 (~5s). Reported live 2026-09-11 as "takes a second or two to switch
 * from 3.5mm to BT" after a boot reconnect; music.log showed the switch
 * landing on the poll tick, up to 5s after the A2DP PCM appeared:
 *
 *     21.749  bt_sink_connected() took 17 ms
 *     26.867  bt_sink_connected() took 20 ms
 *     31.982  bt_sink_connected() took 21 ms
 *     31.982  output changed, reopening
 *
 * BG90 chose 5 on the assumption the fork was expensive; BG98's own
 * instrumentation (those log lines) since measured it at 17-21ms. At a ~1s
 * cadence that is ~2% of one core, and it is paid only while playing out of
 * the jack -- the whole block is skipped once output is already Bluetooth or
 * USB, and while the screen is locked. Worth 2% to have the headset take over
 * within a second of connecting. Raise this again if decode ever starves. */
#define BT_POLL_EVERY 1

static int    g_seek_to_ms = -1;   /* set by audio_seek_ms, consumed by the worker */

/* File, plain stream or playlist — decided from the path. */
static int open_any(dec_t *d, const char *path) {
    int is_url = !strncmp(path, "http://", 7) || !strncmp(path, "https://", 8);
    /* Look for the extension in the path, not at the end of the whole URL:
     * NRK's playlist ends "...muxed.m3u8?adap=audio&aco=aac", so a check for a
     * trailing .m3u8 sent it down the MP3 path and it decoded to nothing. */
    int is_hls = 0;
    if (is_url) {
        const char *q = strchr(path, '?');
        const char *dot = strstr(path, ".m3u8");
        if (dot && (!q || dot < q)) is_hls = 1;
    }
    int rc = is_hls ? dec_open_hls(d, path)
                    : is_url ? dec_open_stream(d, path)
                             : dec_open(d, path);
    /* Named like a stream but served as a playlist: open it as one. */
    if (rc == -2) rc = dec_open_hls(d, path);
    return rc;
}

/* ---- MP3 seek table (background-built) ------------------------------------
 * BG53: dr_mp3's default seek is brute-force -- with no index it decodes
 * forward from wherever the stream currently sits (or from the very start,
 * for a backward seek) to reach the target, real decode work proportional
 * to how far into the file the target is, run synchronously inside the
 * seek call itself. That's why a deep seek -- including a skip button,
 * which is just a seek relative to the current position -- could stall the
 * worker thread, and with it the position clock and playback both, for
 * several seconds: "the bar jumps and then returns to its original
 * position" was actually the display freezing at the pre-seek value while
 * this ran, not a revert (confirmed by sequential screenshots after one
 * tap: frozen, then landed correctly, then continued forward with no
 * bounce back).
 *
 * dr_mp3 also supports binding a seek table (drmp3_bind_seek_table()) that
 * turns the same call into an O(1) lookup plus a small bounded decode
 * within one segment -- but building it costs a pass through the whole
 * file's frame headers first, cheap per frame but proportional to file
 * length, and a podcast episode runs long. Doing that synchronously at
 * track-open would trade "seeking is occasionally slow" for "starting
 * every track is slow", which is worse: most tracks are listened to start
 * to finish without ever being seeked.
 *
 * So it's built in the background, on its own detached thread, against a
 * *separate* drmp3 instance opened on the same file -- never touching the
 * live playback decoder directly, since dr_mp3 has no internal thread
 * safety of its own and the worker thread is continuously reading from it.
 * The finished table crosses threads as a single pointer handoff through
 * g_lock, and only the worker thread itself ever calls
 * drmp3_bind_seek_table() on its own decoder, from its own main loop,
 * alongside the other per-tick state it already checks there each pass.
 * If the track has changed by the time a build finishes, the result is
 * discarded rather than published (checked against g_path under the same
 * lock), so a background build for a track skipped a second after it
 * started can never clobber whatever the *current* track is doing. */
#define MP3_SEEK_POINTS 400   /* ~1500 points/hour of audio; the residual
                                * decode after a seek-table hit is bounded by
                                * the spacing between points, not by how deep
                                * into the file the target is */

static drmp3_seek_point *g_seek_pending_points;
static drmp3_uint32      g_seek_pending_count;
static int               g_seek_pending_ready;

static void *mp3_seektable_worker(void *arg) {
    char *path = (char *)arg;
    drmp3 tmp;
    if (drmp3_init_file(&tmp, path, NULL)) {
        drmp3_uint32 count = MP3_SEEK_POINTS;
        drmp3_seek_point *points = malloc((size_t)count * sizeof(drmp3_seek_point));
        if (points && drmp3_calculate_seek_points(&tmp, &count, points)) {
            pthread_mutex_lock(&g_lock);
            if (!strcmp(g_path, path)) {
                free(g_seek_pending_points);
                g_seek_pending_points = points;
                g_seek_pending_count = count;
                g_seek_pending_ready = 1;
                points = NULL;   /* ownership transferred */
            }
            pthread_mutex_unlock(&g_lock);
        }
        free(points);   /* NULL-safe: frees only if not transferred above */
        drmp3_uninit(&tmp);
    }
    free(path);
    return NULL;
}

/* Kicks off a background build for `d` if it turned out to be MP3 -- call
 * right after any open_any() that might have opened one, initial track and
 * gapless advance alike. Fire-and-forget: detached, self-validates against
 * g_path before publishing (see mp3_seektable_worker()), so nothing here
 * ever needs joining or cancelling. */
static void mp3_seektable_kickoff(dec_t *d, const char *path) {
    if (d->kind != DEC_MP3) return;
    char *pcopy = malloc(strlen(path) + 1);
    if (!pcopy) return;
    strcpy(pcopy, path);
    pthread_t t;
    if (pthread_create(&t, NULL, mp3_seektable_worker, pcopy) == 0) pthread_detach(t);
    else free(pcopy);
}

/* Two slots, swapped by pointer at a track boundary. The decoders are third
 * party structs; moving one by assignment would work today and quietly stop
 * working the day one of them holds a pointer into itself. */
static dec_t g_dec_slot[2];

/* Speed-changed playback state. File-scope, not a worker() local: wsola_t is
 * ~80 KB (fixed-size, no malloc -- see wsola.h) and this is a MIPS thread
 * stack, not a place to put that. Re-initialised whenever rate, channel count
 * or the requested speed drift from what it was last built for -- one check
 * covers a speed change, a gapless format change, and the very first use
 * (zero-initialised statics start at rate=0, which never matches). */
static wsola_t g_wsola;
static short   g_wsola_out[8192];
/* What g_wsola was last built for. Kept separately rather than read back out
 * of g_wsola, because wsola_init clamps what it is given (rate to 51200,
 * speed to [800,2000]): comparing the request against the clamped value never
 * matches for anything outside those bounds, and the mismatch re-initialised
 * ~80 KB and a 2048-point cosine window on every single chunk. */
static int     g_wsola_rate, g_wsola_ch, g_wsola_speed;

/* Write `frames` interleaved frames to `pcm`, with the same underrun/loss
 * handling the direct write path always had. Shared by the 1.0x passthrough
 * and every WSOLA-drained chunk, so a speed change does not get a second copy
 * of this retry logic to drift out of sync with the first. Returns the pcm
 * handle, or NULL if the output was lost (the caller must stop writing). */
static void *write_pcm_frames(void *pcm, const char *p, snd_pcm_uframes_t left,
                              size_t frame_bytes) {
    while (left > 0) {
        snd_pcm_sframes_t w = x_writei(pcm, p, left);
        if (w < 0) {
            if (w == -32) {
                static unsigned long xruns;
                if (xruns == 0 || (xruns & 63) == 0)
                    alog("[audio] underrun #%lu (buffer ran dry)\n", xruns + 1);
                xruns++;
            } else {
                static int moaned;
                if (moaned < 5) {
                    moaned++;
                    alog("[audio] writei failed rc=%ld\n", (long)w);
                }
            }
            if (x_recover(pcm, (int)w, 1) < 0) {
                alog("[audio] output lost (rc=%ld); pausing\n", (long)w);
                g_lost_kind = g_out_kind;
                x_close(pcm);
                pthread_mutex_lock(&g_lock);
                g_paused = 1;
                pthread_mutex_unlock(&g_lock);
                g_out_lost = 1;
                return NULL;
            }
            usleep(1000);
            continue;
        }
        left -= (snd_pcm_uframes_t)w;
        p += (size_t)w * frame_bytes;
    }
    return pcm;
}

/* The nice -8 above, but only when the output is NOT Bluetooth -- call after
 * every pcm_open(), since only then is g_out_kind known, and the route can
 * change mid-playback (jack plugged in, headset connected).
 *
 * On Bluetooth the encoder is a separate process. bluealsa does the LDAC
 * encode on this same single core, to its own hard realtime deadline, and it
 * sits *downstream* of us: starving it corrupts audio that has already left
 * this process, where nothing here can clamp, dither or otherwise defend it.
 * At nice -8 this thread wins every scheduling tie against that encoder, and
 * every CPU-hungry thing added since (the EQ, WSOLA, the icon blits) takes
 * its share from the encoder's slice rather than ours -- which is exactly
 * the shape of a fault that grows more frequent as the app grows, on
 * Bluetooth only, regardless of whether the EQ is even enabled.
 *
 * Nothing is given up by skipping it there. The boost exists for 96 kHz
 * 24-bit decode running near realtime (BG12: 93% of one core); Bluetooth is
 * rate-capped by bt_target_rate() and forced to S16_LE, and the logs show
 * decode at 3-13% of one core on that path. This was outranking the encoder
 * to protect headroom that was never in short supply. */
static void apply_decode_priority(void) {
    int bt = (g_out_kind == 2);
    int want = bt ? 0 : -8;
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), want) != 0)
        alog("[audio] could not set decode priority %d\n", want);
    else
        alog("[audio] decode priority nice %d (%s)\n", want, bt ? "bt" : "local");
}

/* BG67/BG79: bluealsa resets its own mixer to a default level on every new
 * PCM connection, independent of the AVRCP-adjacent control this app
 * already tracks in bt_mixer/g_vol -- audio_bt_volume_service() always
 * trusts a mixer readback (correct on its own: the headset's own buttons
 * move it without an event ever reaching this app), so without re-pushing
 * first, the very next readback after a fresh connection silently
 * overwrites g_vol with whatever bluealsa reset to.
 *
 * BG67 originally fixed this at the *first* pcm_open() a track makes --
 * reported live as "volume reverts" on every track/album change over
 * Bluetooth. BG79 is the same bug on a different pcm_open(): resuming a
 * paused audiobook after PCM_IDLE_CLOSE_TICKS has released the output
 * device (see worker()'s own comment on why it does that) opens a brand
 * new connection too, and that call site never got the same fix -- the
 * original inline block lived right after the one call site, easy to miss
 * that every *other* successful pcm_open() over Bluetooth needs it just as
 * much. Pulled out into one shared call so that stops being possible. */
static void bt_repush_volume(void) {
    if (g_out_kind != 2) return;
    /* R90: bt_vol_raw is this app's own last-known raw setting -- reuse it
     * directly rather than converting g_vol (percent) back to raw, the
     * same "never round-trip through percent" reasoning as everywhere else
     * in this file now. Falls back to converting g_vol only on a fresh
     * connection that hasn't read a raw value yet at all. */
    int raw = bt_vol_raw >= 0 ? bt_vol_raw : bt_pct_to_raw(audio_volume());
    pthread_mutex_lock(&bt_vol_lock);
    bt_vol_pending = 1; bt_vol_pending_raw = raw;
    pthread_mutex_unlock(&bt_vol_lock);
}

static void *worker(void *arg) {
    (void)arg;

    /* BG12/BG9: decode has to beat the UI to the CPU, not merely share it.
     *
     * This is a single-core MIPS, and 96 kHz/24-bit FLAC (the Tchaikovsky /
     * Sibelius set is 372 MB for one 19.7-minute movement) decodes close
     * enough to realtime that it has very little headroom. The UI thread
     * does full 480x800 software redraws with antialiased text; at default
     * priority the two compete as equals, so a redraw lands squarely on a
     * decode deadline and the buffer runs dry -- audible as the "crunchy"
     * distortion, with the starved main thread showing as frozen on-screen
     * controls while hardware keys (acted on from this thread's state) kept
     * working. Sustained 100% CPU is also the warm case in BG9.
     *
     * Niced up rather than SCHED_FIFO deliberately: this thread's write-retry
     * path can spin if recovery keeps failing, and a spinning realtime thread
     * on one core would lock the device out entirely -- far worse than the
     * bug. A negative nice still gets preempted; it only wins the tie.
     *
     * Applied by apply_decode_priority() after the route is known rather than
     * here, because it must NOT apply on Bluetooth -- see that function. */

    dec_t *d = &g_dec_slot[0];
    int slot = 0;
    /* Discard any pending seek table a previous, since-abandoned session
     * left behind (e.g. a track whose pcm_open() below failed after its
     * table had already started building) -- nothing will ever consume it
     * otherwise, since only this loop, below, ever does. */
    pthread_mutex_lock(&g_lock);
    free(g_seek_pending_points);
    g_seek_pending_points = NULL; g_seek_pending_count = 0; g_seek_pending_ready = 0;
    pthread_mutex_unlock(&g_lock);
    drmp3_seek_point *bound_points = NULL;   /* currently bound to d->mp3, if any; owned here */
    if (open_any(d, g_path) != 0) {
        alog("[audio] cannot decode %s\n", g_path);
        pthread_mutex_lock(&g_lock);
        g_active = 0; g_running = 0;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    mp3_seektable_kickoff(d, g_path);
    int ch = d->channels > 0 ? d->channels : 2;
    unsigned rate = d->rate ? d->rate : 44100;   /* the source's own rate -- seek/duration/polling always key off this, never eff_rate */
    pthread_mutex_lock(&g_lock);
    if (d->frames) g_dur_ms = (int)(d->frames * 1000 / rate);
    pthread_mutex_unlock(&g_lock);

    /* 24-bit sources are opened as S24_LE; everything else is 16 either way. */
    int want_fmt = (d->bits > 16) ? FMT_S24_LE : FMT_S16_LE;
    void *pcm = pcm_open(rate, ch, d->is_stream, want_fmt);
    apply_decode_priority();
    if (!pcm) {
        dec_close(d);
        pthread_mutex_lock(&g_lock);
        g_active = 0; g_running = 0;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    /* What pcm_open() actually negotiated -- equal to `rate` except when it
     * applied BG41's Bluetooth cap, in which case everything downstream of
     * decode (EQ, WSOLA, decimation, the write itself) has to key off this
     * instead. */
    unsigned eff_rate = g_out_rate;
    eq_set_format(eff_rate, ch);

    /* Every track opens a brand new bluealsa PCM connection (pcm_open()
     * above) -- see bt_repush_volume()'s own comment for why that needs a
     * re-push before bluealsa's reset default gets read back and clobbers
     * g_vol. */
    bt_repush_volume();

    /* Two buffers, because the two paths carry different sample sizes. Only
     * the lossless formats above 16 bits use the wide one; MP3 and AAC decode
     * to 16 whatever the container claims. */
    /* Bluetooth's A2DP transport is always opened S16_LE (see pcm_open) no
     * matter what the source is, so "hires" has to track what was actually
     * granted, not just what the file claims. Packing 4-byte samples into a
     * stream ALSA opened as 2-byte is what turned a 24-bit FLAC into static
     * over a headset while the same file played cleanly on the jack. */
    int hires = dec_is_wide(d) && g_out_kind != 2;
    int src_wide = dec_is_wide(d);   /* read at full precision even when hires is off */
    short   *buf   = malloc((size_t)CHUNK_FRAMES * (size_t)ch * sizeof(short));
    int32_t *buf32 = malloc((size_t)CHUNK_FRAMES * (size_t)ch * sizeof(int32_t));
    if (!buf || !buf32) { free(buf); free(buf32); dec_close(d); x_close(pcm); return NULL; }
    size_t frame_bytes = hires ? (size_t)ch * 4 : (size_t)ch * sizeof(short);
    uint64_t done = 0;
    int poll_tick = 0, resume_tick = 0, idle_close_tick = 0;
    int bt_poll_tick = 0;
    /* Self-measurement, because "decode is close to realtime" was an
     * assumption and the stock player disproves it: it plays the same
     * 192/24 files over USB without drain, so decode and USB output are
     * both affordable on this CPU and any shortfall is ours. This reports
     * what share of one core this thread actually uses, alongside whether
     * the device was opened exact or through a converting plug layer --
     * a plughw fallback would mean libasound resampling 192 kHz in
     * software, which would be entirely self-inflicted and would look
     * exactly like the reported symptoms. */
    int cpu_tick = 0;
    struct timespec last_wall = {0, 0}, last_cpu = {0, 0};
    for (;;) {
        pthread_mutex_lock(&g_lock);
        int run = g_running, paused = g_paused;
        /* BG59: "the mixer does it on BT" is only true once a real AVRCP
         * mixer control has actually been found (bt_mixer[0], set by
         * find_bt_mixer() in audio_bt_volume_service() on the bt_poll
         * thread) -- some headsets never negotiate AVRCP absolute volume at
         * all, in which case bt_mixer stays empty forever and that write
         * path is permanently a no-op. Forcing vol=100 unconditionally in
         * that case meant volume control did *nothing* over Bluetooth: not
         * the mixer (never found), and not software gain either (skipped
         * because Bluetooth was assumed to always have a working mixer) --
         * matching exactly the reported symptom of the on-screen slider
         * moving with no audible change. Falling back to software scaling
         * when there's no confirmed mixer keeps volume control working
         * either way. bt_mixer[0] is read here without bt_vol_lock: it's a
         * single byte's worth of "is this empty," set from one thread and
         * read from another, and a stale read for one tick just means this
         * check catches up next tick, not a real race. */
        int vol = (audio_using_bt() && bt_mixer[0]) ? 100 : g_vol;
        int seek = g_seek_to_ms; g_seek_to_ms = -1;
        int speed = g_speed;
        int have_table = g_seek_pending_ready;
        drmp3_seek_point *tbl_points = g_seek_pending_points;
        drmp3_uint32 tbl_count = g_seek_pending_count;
        if (have_table) { g_seek_pending_ready = 0; g_seek_pending_points = NULL; }
        pthread_mutex_unlock(&g_lock);
        if (!run) break;

        /* BG53: bind a finished background seek table the moment it's
         * ready. d->mp3 is only ever touched from this thread, so this is
         * the one safe place to call drmp3_bind_seek_table() -- see the
         * comment above mp3_seektable_worker(). */
        if (have_table) {
            if (d->kind == DEC_MP3) {
                free(bound_points);
                drmp3_bind_seek_table(&d->mp3, tbl_count, tbl_points);
                bound_points = tbl_points;
            } else {
                free(tbl_points);   /* shouldn't happen, but the track could
                                      * in principle have changed kind */
            }
        }

        /* Every 512 chunks (~5.5 s at 192 kHz, ~24 s at 44.1) compare this
         * thread's own CPU time against wall time. Two clock_gettime calls
         * per window, so the instrument cannot meaningfully distort what it
         * measures. Integer milliseconds throughout: this CPU has no useful
         * hardware floating point. */
        if (++cpu_tick >= 512) {
            cpu_tick = 0;
            struct timespec wall, cpu;
            clock_gettime(CLOCK_MONOTONIC, &wall);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
            if (last_wall.tv_sec) {
                long dw = (wall.tv_sec - last_wall.tv_sec) * 1000L
                        + (wall.tv_nsec - last_wall.tv_nsec) / 1000000L;
                long dc = (cpu.tv_sec - last_cpu.tv_sec) * 1000L
                        + (cpu.tv_nsec - last_cpu.tv_nsec) / 1000000L;
                if (dw > 0)
                    alog("[audio] decode %ld%% of one core @ %u Hz %d-bit %s\n",
                         dc * 100 / dw, rate, d->bits, g_exact ? "exact" : "CONVERTED");
            }
            last_wall = wall; last_cpu = cpu;
        }

        if (seek >= 0) {
            uint64_t f = (uint64_t)seek * rate / 1000;
            dec_seek(d, f, g_path, &bound_points);
            done = f;
            pthread_mutex_lock(&g_lock);
            g_pos_ms = seek;                  /* move the clock even while paused */
            pthread_mutex_unlock(&g_lock);
        }
        if (paused) {
            /* If the pause was the output vanishing rather than a decision,
             * watch for that same route coming back and pick up where it left
             * off — walking out of range and back should not cost a manual
             * rewind. Only the same route: resuming a headset's album out of
             * the jack because the headset went away is not a kindness. */
            if (g_out_lost && !pcm && ++resume_tick >= 20) {
                resume_tick = 0;
                int back = (g_lost_kind == 2) ? bt_sink_connected() : 1;
                if (back) {
                    pcm = pcm_open(rate, ch, d->is_stream,
                                   (d->bits > 16) ? FMT_S24_LE : FMT_S16_LE);
                    if (pcm) {
                        apply_decode_priority();
                        hires = dec_is_wide(d) && g_out_kind != 2;
                        src_wide = dec_is_wide(d);
                        frame_bytes = hires ? (size_t)ch * 4 : (size_t)ch * sizeof(short);
                        eff_rate = g_out_rate;
                        eq_set_format(eff_rate, ch);
                        g_out_lost = 0;
                        pthread_mutex_lock(&g_lock);
                        g_paused = 0;
                        pthread_mutex_unlock(&g_lock);
                        bt_repush_volume();
                        alog("[audio] output came back; resuming\n");
                    }
                }
            }
            /* A paused stream still holds the playback device open, and on this
             * hardware that keeps the DAC and the headphone amp powered for as
             * long as the pause lasts -- measured as the largest single drain
             * with nothing playing at all (a book left paused overnight flattened
             * the battery in about nine hours). Hand the device back once the
             * pause looks deliberate rather than momentary.
             *
             * Nothing new is needed to get it back: the !pcm branch below already
             * reopens on resume, because an output that vanished and returned
             * leaves exactly this state. The grace period is what keeps a quick
             * pause/unpause from cycling the DAC -- and on this hardware that
             * cycle is audible, so it wants to be worth the trouble.
             *
             * Deliberately not while g_out_lost: that case owns `pcm` for its own
             * reconnect logic just above, and must not race with this. */
            if (pcm && !g_out_lost && ++idle_close_tick >= PCM_IDLE_CLOSE_TICKS) {
                idle_close_tick = 0;
                x_drop(pcm);
                x_close(pcm);
                pcm = NULL;
                alog("[audio] paused: released the output device\n");
            }
            /* Once the device is closed there is nothing left to service, so the
             * poll can slow right down too -- 16 wakeups a second to sit still
             * is its own small drain. */
            usleep(pcm ? 60000 : 400000);
            continue;
        }
        idle_close_tick = 0;

        /* Resuming after the output went away: open whatever is there now.
         * That may be a different device entirely — the headset having gone,
         * this lands on the jack — which is what someone pressing play
         * expects to happen. */
        if (!pcm) {
            pcm = pcm_open(rate, ch, d->is_stream, want_fmt);
            if (pcm) apply_decode_priority();
            if (!pcm) {
                alog("[audio] no output to resume on; staying paused\n");
                pthread_mutex_lock(&g_lock);
                g_paused = 1;
                pthread_mutex_unlock(&g_lock);
                continue;
            }
            hires = dec_is_wide(d) && g_out_kind != 2;
            src_wide = dec_is_wide(d);
            frame_bytes = hires ? (size_t)ch * 4 : (size_t)ch * sizeof(short);
            eff_rate = g_out_rate;
            eq_set_format(eff_rate, ch);
            g_out_lost = 0;
            bt_repush_volume();
            alog("[audio] resumed on a fresh device\n");
        }

        /* Plugging the amp in halfway through an album should not mean waiting
         * for the next track. Only the USB check ran here at all before BG90 --
         * it's a readlink, cheap enough for once a second -- while Bluetooth
         * connecting mid-playback waited for the next track regardless, since
         * bt_sink_connected() forks a whole `bluealsa-cli` subprocess and
         * doing that every second from this thread would be a real, ongoing
         * cost competing with decode for the one core. bt_poll_tick instead
         * checks it only once every BT_POLL_EVERY passes of this block (~5s at
         * that cadence) -- still switches over within a few seconds of putting
         * headphones on rather than needing a manual nudge, at a fraction of
         * the subprocess-spawn cost polling every second would have. Skipped
         * entirely once already on Bluetooth or USB (BG90's own priority --
         * USB > Bluetooth > jack -- means Bluetooth connecting can never
         * preempt either, so there's nothing for this check to do then). */
        if (++poll_tick >= (int)(rate / CHUNK_FRAMES) + 1) {
            poll_tick = 0;
            int now = usb_card();
            int bt_now = 0;
            /* BG90 regression, reported live as "wake from sleep feels
             * fractionally slower": this thread has no other way to know
             * the screen is locked, and forking bluealsa-cli right as the
             * user presses power competes for the one core against the
             * wake-up redraw. Nothing to gain from finding a Bluetooth
             * reconnect while the screen's dark anyway -- see
             * audio_set_screen_locked()'s own comment. */
            if (g_out_kind == 0 && !g_screen_locked &&
                ++bt_poll_tick >= BT_POLL_EVERY) {
                bt_poll_tick = 0;
                /* BG98: how expensive this subprocess spawn actually is on
                 * this hardware -- the concrete number behind the "wake
                 * feels slower" theory, not just the reasoning for it. */
                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                bt_now = bt_sink_connected();
                clock_gettime(CLOCK_MONOTONIC, &t1);
                alog("[audio] bt_sink_connected() took %ld ms\n",
                     (t1.tv_sec - t0.tv_sec) * 1000L +
                     (t1.tv_nsec - t0.tv_nsec) / 1000000L);
            }
            if ((g_out_kind != 2 && now != g_out_card) ||
                (g_out_kind == 0 && bt_now)) {
                alog("[audio] output changed, reopening\n");
                x_drop(pcm); x_close(pcm);
                pcm = pcm_open(rate, ch, d->is_stream, want_fmt);
                if (!pcm) break;
                apply_decode_priority();
                hires = dec_is_wide(d) && g_out_kind != 2;
                src_wide = dec_is_wide(d);
                frame_bytes = hires ? (size_t)ch * 4 : (size_t)ch * sizeof(short);
                eff_rate = g_out_rate;
                eq_set_format(eff_rate, ch);
            }

            /* BG32 diagnostics. Drained on the same once-a-second tick as the
             * USB poll, from the same thread that accumulates them, so this
             * needs no lock. Silent unless something is actually worth seeing:
             * a clean chunk of normal music trips none of these conditions, so
             * a quiet log is itself the result -- it means the samples leaving
             * the EQ were fine and a reported tone came from further down the
             * chain (bluealsa, the A2DP link, the headset), not from here.
             *
             * Deliberately not gated on eq_enabled(): a drain with the EQ off
             * reports zero samples, which distinguishes "EQ ran and was clean"
             * from "EQ never ran at all" when reading the log afterwards.
             *
             * Per channel, not aggregated: every report of this tone has named
             * the left ear specifically, and both channels run identical
             * coefficients, so a left/right split in our own output is the one
             * measurement that can tell "this filter made the tone" apart from
             * "our output is clean and something downstream did." A threshold
             * on the per-channel peak difference, not just on the individual
             * peaks, catches the case both channels are loud but only one is
             * clipped-loud. */
            eq_stats_t es;
            eq_stats_drain(&es);
            float pdiff = fabsf(es.peak[0] - es.peak[1]);
            if (es.trips || es.clipped[0] || es.clipped[1] ||
                es.peak[0] > 1.0f || es.peak[1] > 1.0f ||
                es.zmax > 1.0f || pdiff > 0.3f)
                alog("[eq] peak L%.3f R%.3f zmax %.3f clipped L%lu/R%lu of %lu"
                     " trips %lu last b%d c%d\n",
                     (double)es.peak[0], (double)es.peak[1], (double)es.zmax,
                     es.clipped[0], es.clipped[1], es.samples,
                     es.trips, es.trip_band, es.trip_ch);
        }

        /* Read at full precision whenever the source has it, regardless of
         * hires (the output format): a BT-forced-16-bit track still reads
         * 32-bit so the truncation down to 16 can be dithered below, rather
         * than handed to dr_flac/dr_wav's own undithered s16 reader. */
        uint64_t got = src_wide ? dec_read32(d, buf32, CHUNK_FRAMES)
                                : dec_read(d, buf, CHUNK_FRAMES);

        /* A skip is just an early end of track.
         *
         * Reaching the end of a track already hands over to the next one
         * without touching the open device (see the got == 0 block below,
         * and the comment there about where the gap used to come from).
         * Skipping did not: it went out through audio_play(), which stops
         * the worker, closes the PCM and opens a new one. On the local DAC
         * that is a quick reopen; over Bluetooth it tears down the A2DP
         * stream and builds a fresh bluealsa connection, and *nothing is
         * transmitted* until that completes.
         *
         * Measured on a verified capture of the Bluetooth stream: every
         * track change punched a hole of 250-400 ms, every
         * music/audiobook/podcast switch 500-1200 ms, and each hole ended
         * to the millisecond on the log line for the new PCM opening. 49
         * gaps, 14.2 s of audio simply absent, in 13 minutes.
         *
         * So a skip now sets the next path and raises this flag, and the
         * worker treats it exactly as the natural boundary it already
         * handles well: same decoder handover, same device kept open, same
         * reopen only if the format genuinely changes. */
        if (got > 0) {
            pthread_mutex_lock(&g_lock);
            int skip = g_skip_now && g_next_path[0];
            if (skip) g_skip_now = 0;
            pthread_mutex_unlock(&g_lock);
            if (skip) got = 0;
        }
        /* done/g_pos_ms track source frames throughout -- captured before
         * decimation below can shrink `got`, so position and duration (both
         * figured against `rate`, the source rate) read correctly regardless
         * of whether this chunk got folded down for Bluetooth. */
        uint64_t got_src = got;
        /* BG41: fold eff_rate's Bluetooth cap into the samples themselves,
         * before EQ sees them -- otherwise the cascade still ran at the full
         * source rate regardless of what got negotiated, which was most of
         * the self-inflicted cost. bt_target_rate() only ever returns an
         * exact 2x or 4x divisor of `rate`, so this is plain box-car
         * decimation: no fractional position to carry between chunks, no
         * per-track state, and it collapses to a no-op (decim == 1) for
         * every source that wasn't hi-res-over-Bluetooth in the first place.
         * A moving-average filter is not a brick-wall anti-alias -- it nulls
         * exactly at the new Nyquist and leaks a little just below it -- but
         * the destination is a lossy Bluetooth codec throwing away far more
         * than that regardless, and acoustic recordings carry little real
         * energy above 24 kHz to alias from in the first place. */
        /* `eff_rate < rate` specifically, not merely `!=`: this is plain
         * integer decimation, so it only means anything when the negotiated
         * rate divides the source rate. bt_target_rate() is built to hand
         * pcm_open() an exact 2x/4x divisor, but it only maps 88.2/176.4 and
         * 96/192 -- every other rate passes through untouched, and pcm_open()
         * skips its own `r != rate` sanity check on the Bluetooth path
         * (see the `!use_bt` in that test), so snd_pcm_hw_params_set_rate_near
         * is free to round *up*. A 44.1 kHz source on a link that negotiates
         * 48 kHz lands here with eff_rate > rate, `rate / eff_rate` is 0, and
         * `got / decim` divides by zero -- SIGFPE, process gone, no log.
         * Guarding the ratio instead of just reordering the test: a
         * non-integer ratio (say 48k negotiated against a 44.1k source)
         * can't be box-car decimated correctly either, and silently
         * mangling it would be worse than leaving the sample rate alone and
         * letting the layer below resample. */
        unsigned decim = (eff_rate > 0 && eff_rate < rate && rate % eff_rate == 0)
                       ? rate / eff_rate : 1;
        if (got > 0 && decim > 1) {
            uint64_t got_d = got / decim;
            if (src_wide) {
                for (uint64_t f = 0; f < got_d; f++)
                    for (int c = 0; c < ch; c++) {
                        int64_t sum = 0;
                        for (unsigned k = 0; k < decim; k++)
                            sum += buf32[(f * decim + k) * (unsigned)ch + (unsigned)c];
                        buf32[f * (unsigned)ch + (unsigned)c] = (int32_t)(sum / decim);
                    }
            } else {
                for (uint64_t f = 0; f < got_d; f++)
                    for (int c = 0; c < ch; c++) {
                        int32_t sum = 0;
                        for (unsigned k = 0; k < decim; k++)
                            sum += buf[(f * decim + k) * (unsigned)ch + (unsigned)c];
                        buf[f * (unsigned)ch + (unsigned)c] = (short)(sum / (int)decim);
                    }
            }
            got = got_d;
        }
        /* Before any volume scaling, shift or dither below, on whichever
         * buffer actually holds this chunk's decode -- the same raw samples
         * regardless of which output path they take afterward. A no-op
         * (single flag check) when the EQ is off, and likewise skipped
         * outright on USB when the bypass setting is on and this chunk is
         * actually going out over USB -- see g_usb_bypass's own comment. */
        if (got > 0 && !(g_usb_bypass && g_out_kind == 1)) {
            if (src_wide) eq_process_s32(buf32, (int)got, ch);
            else          eq_process_s16(buf, (int)got, ch);
        }
        if (got == 0) {
            /* End of the track. This is where the gap used to come from: the
             * decoder and the PCM device were both torn down and opened again,
             * and the silence in between is audible on anything that runs
             * continuously. Instead, pick up the next track and keep writing
             * into the same open device — nothing is drained, so the buffered
             * tail of one track runs straight into the head of the next. */
            char nextp[sizeof(g_path)];
            pthread_mutex_lock(&g_lock);
            snprintf(nextp, sizeof(nextp), "%s", g_next_path);
            g_next_path[0] = '\0';
            pthread_mutex_unlock(&g_lock);
            if (!nextp[0]) break;

            dec_t *nd = &g_dec_slot[slot ^ 1];
            if (open_any(nd, nextp) != 0) {
                alog("[audio] cannot decode %s\n", nextp);
                break;
            }
            mp3_seektable_kickoff(nd, nextp);
            dec_close(d);
            free(bound_points);
            bound_points = NULL;   /* new track: its own table, if any, arrives via the loop above */
            d = nd;
            slot ^= 1;

            /* Read precision follows the new track regardless of whether the
             * device below ends up reopening — a run of tracks that are all
             * non-hires-for-BT but alternate between 16-bit and 24-bit
             * sources needs this checked every track, not only when hires
             * itself flips. */
            src_wide = dec_is_wide(d);

            /* Only a change of rate or channel count forces the device to be
             * reopened, and then the tail is drained first so it is not cut
             * off. Same format — the overwhelmingly common case within an
             * album — and the join is seamless. */
            int nch = d->channels > 0 ? d->channels : 2;
            unsigned nrate = d->rate ? d->rate : 44100;
            int nhires = dec_is_wide(d) && g_out_kind != 2;
            /* A change of depth needs the device reopening just as a change of
             * rate does — the sample size on the wire is different. */
            if (nch != ch || nrate != rate || nhires != hires) {
                alog("[audio] format change %u/%d -> %u/%d\n", rate, ch, nrate, nch);
                if (x_drain) x_drain(pcm);
                x_close(pcm);
                ch = nch; rate = nrate;
                hires = nhires;
                want_fmt = hires ? FMT_S24_LE : FMT_S16_LE;
                pcm = pcm_open(rate, ch, d->is_stream, want_fmt);
                if (!pcm) break;
                apply_decode_priority();
                hires = hires && g_out_kind != 2;
                eff_rate = g_out_rate;
                eq_set_format(eff_rate, ch);
                bt_repush_volume();
                short *nb = realloc(buf, (size_t)CHUNK_FRAMES * (size_t)ch * sizeof(short));
                int32_t *nb32 = realloc(buf32, (size_t)CHUNK_FRAMES * (size_t)ch * sizeof(int32_t));
                if (!nb || !nb32) break;
                buf = nb; buf32 = nb32;
                frame_bytes = hires ? (size_t)ch * 4 : (size_t)ch * sizeof(short);
            }

            done = 0;
            pthread_mutex_lock(&g_lock);
            g_dur_ms = d->frames ? (int)(d->frames * 1000 / rate) : 0;
            g_pos_ms = 0;
            g_advance++;
            snprintf(g_path, sizeof(g_path), "%s", nextp);
            pthread_mutex_unlock(&g_lock);
            continue;
        }

        size_t n = (size_t)got * (size_t)ch;
        if (hires) {
            /* dr_flac and dr_wav hand back samples left-aligned in 32 bits;
             * ALSA's S24_LE wants them in the low 24, so everything shifts
             * down by 8 regardless of volume. S32_LE wants exactly what the
             * decoder already produced, so it shifts by nothing -- getting
             * this wrong on a device that took S32_LE would not distort, it
             * would just play 48 dB quiet. */
            int shift = (g_out_fmt == FMT_S32_LE) ? 0 : 8;
            int gain = vol < 100 ? vol * vol * 256 / 10000 : 256;
            for (size_t i = 0; i < n; i++) {
                int32_t v = buf32[i] >> shift;
                if (gain != 256) v = (int32_t)(((int64_t)v * gain) >> 8);
                buf32[i] = v;
            }
        } else if (src_wide) {
            /* A 24-bit source forced down to Bluetooth's fixed S16_LE.
             * dr_flac/dr_wav's own s16 reader would take the top 16 bits and
             * silently drop the rest — plain truncation, with nothing to
             * break up what gets thrown away. That is audible as a low-level
             * hiss in a quiet decaying passage (reported on Piano Works at
             * 0:35, gone over USB where the full 24 bits reach the DAC
             * untouched): structured quantization distortion, not codec
             * noise, and entirely something this code was doing to itself.
             * Triangular dither trades that structure for plain noise,
             * which is quieter and is the standard fix for reducing bit
             * depth at all. Samples are the same 32-bit-left-aligned form as
             * the hires branch above; the discarded precision is the low 16
             * bits, so the dither is scaled to +/-1 LSB at 16-bit. */
            int gain = vol < 100 ? vol * vol * 256 / 10000 : 256;
            for (size_t i = 0; i < n; i++) {
                int32_t noise = ((rand() & 0xFFFF) - 0x8000)
                               + ((rand() & 0xFFFF) - 0x8000);   /* two uniform -> triangular */
                int32_t v = (buf32[i] + (noise >> 1)) >> 16;
                if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                if (gain != 256) v = (v * gain) >> 8;
                buf[i] = (short)v;
            }
        } else if (vol < 100) {
            int gain = vol * vol * 256 / 10000;     /* squared: usable at low settings */
            for (size_t i = 0; i < n; i++) buf[i] = (short)((buf[i] * gain) >> 8);
        }

        /* R29: cheap abs-max scan for the waveform seek bar, over exactly
         * what's about to be written (post-gain, so a quiet volume reads as
         * a quiet waveform too -- callers only compare this against other
         * samples from the same track's own capture, so that's consistent
         * within a track rather than wrong in any way that matters). Every
         * sample, not a stride -- n tops out around CHUNK_FRAMES*channels,
         * a few thousand plain comparisons, negligible next to the decode
         * this loop already does every chunk. */
        int32_t chunk_peak = 0;
        if (hires) {
            for (size_t i = 0; i < n; i++) {
                int32_t v = buf32[i];
                if (v < 0) v = -v;
                if (v > chunk_peak) chunk_peak = v;
            }
        } else {
            for (size_t i = 0; i < n; i++) {
                int32_t v = buf[i];
                if (v < 0) v = -v;
                if (v > chunk_peak) chunk_peak = v;
            }
        }

        /* WSOLA changes what leaves the buffer from here on, not how much
         * content was decoded -- done and g_pos_ms below still count decoded
         * (content) frames, exactly as before this existed, so position and
         * duration read correctly regardless of speed. Only the 16-bit path:
         * WSOLA works on shorts, and an audiobook is never hires in practice,
         * so a hires source simply keeps playing at 1.0x. */
        if (!hires && speed != 1000) {
            if (g_wsola_rate != (int)eff_rate || g_wsola_ch != ch) {
                wsola_init(&g_wsola, (long)eff_rate, ch, speed);
                g_wsola_rate = (int)eff_rate; g_wsola_ch = ch; g_wsola_speed = speed;
            } else if (g_wsola_speed != speed) {
                /* BG44: a pure speed change (rate/channels unchanged) needs
                 * no flush -- wsola_set_speed() only touches Ha, so the
                 * overlap-add continuity and cross-correlation reference
                 * survive, and the transition is inaudible instead of the
                 * ~40ms re-prime gap a full wsola_init() cost on every tap
                 * of the speed control. */
                wsola_set_speed(&g_wsola, speed);
                g_wsola_speed = speed;
            }
            wsola_feed(&g_wsola, buf, (int)got);
            for (;;) {
                int of = wsola_drain(&g_wsola, g_wsola_out,
                                     (int)(sizeof(g_wsola_out) / sizeof(short) / ch));
                if (of <= 0) break;
                pcm = write_pcm_frames(pcm, (const char *)g_wsola_out,
                                       (snd_pcm_uframes_t)of, frame_bytes);
                if (!pcm) break;
            }
        } else {
            const char *p = hires ? (const char *)buf32 : (const char *)buf;
            pcm = write_pcm_frames(pcm, p, (snd_pcm_uframes_t)got, frame_bytes);
        }
        /* The write did not happen, so the position must not advance. */
        if (!pcm) continue;

        done += got_src;
        pthread_mutex_lock(&g_lock);
        g_last_peak = chunk_peak;
        g_pos_ms = (int)(done * 1000 / rate);
        pthread_mutex_unlock(&g_lock);
    }

    free(buf);
    free(buf32);
    /* Drain rather than drop when the queue simply ran out, so the last
     * seconds actually play. */
    if (pcm) {
        pthread_mutex_lock(&g_lock);
        int stopped = !g_running;
        pthread_mutex_unlock(&g_lock);
        if (stopped) x_drop(pcm);
        else if (x_drain) x_drain(pcm);
        x_close(pcm);
    }
    dec_close(d);
    free(bound_points);
    pthread_mutex_lock(&g_lock);
    g_active = 0; g_running = 0; g_paused = 0;
    pthread_mutex_unlock(&g_lock);
    alog("[audio] finished\n");
    return NULL;
}

int audio_play(const char *path) {
    audio_stop();
    if (load_libs() != 0) return -1;
    pthread_mutex_lock(&g_lock);
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_running = 1; g_active = 1; g_paused = 0; g_pos_ms = 0; g_dur_ms = 0;
    g_seek_to_ms = -1;
    g_next_path[0] = '\0';
    g_advance = 0;
    g_skip_now = 0;
    g_out_lost = 0;
    pthread_mutex_unlock(&g_lock);
    if (pthread_create(&g_thread, NULL, worker, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        g_active = 0; g_running = 0;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    g_thread_valid = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* Skip straight to `path`, keeping the output device open if the worker is
 * already running and the format allows it. Returns 0 if the running worker
 * took it, or falls back to a full audio_play() when there is nothing to
 * hand over to (stopped, paused into a dead worker, or no worker at all).
 *
 * The caller does not need to know which happened: both end with `path`
 * playing. The difference is only whether the Bluetooth link had to be torn
 * down to get there -- see the skip handling in worker(). */
int audio_skip_to(const char *path) {
    if (!path || !path[0]) return -1;
    pthread_mutex_lock(&g_lock);
    int live = g_running && g_active && g_thread_valid;
    if (live) {
        snprintf(g_next_path, sizeof(g_next_path), "%s", path);
        g_skip_now = 1;
        g_seek_to_ms = -1;
        g_paused = 0;
    }
    pthread_mutex_unlock(&g_lock);
    if (live) return 0;
    return audio_play(path);
}

void audio_stop(void) {
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    int join = g_thread_valid;
    g_thread_valid = 0;
    pthread_mutex_unlock(&g_lock);
    if (join) pthread_join(g_thread, NULL);
    pthread_mutex_lock(&g_lock);
    g_active = 0; g_paused = 0; g_pos_ms = 0; g_dur_ms = 0;
    pthread_mutex_unlock(&g_lock);
}

void audio_seek_ms(int ms) {
    if (ms < 0) ms = 0;
    pthread_mutex_lock(&g_lock);
    g_seek_to_ms = ms;
    pthread_mutex_unlock(&g_lock);
}

void audio_set_speed(int permille) {
    if (permille < 800) permille = 800;
    if (permille > 2000) permille = 2000;
    pthread_mutex_lock(&g_lock);
    g_speed = permille;
    pthread_mutex_unlock(&g_lock);
}
int audio_speed(void) { pthread_mutex_lock(&g_lock); int v = g_speed; pthread_mutex_unlock(&g_lock); return v; }

void audio_toggle(void) {
    pthread_mutex_lock(&g_lock);
    g_paused = !g_paused;
    pthread_mutex_unlock(&g_lock);
}

int audio_is_active(void) { pthread_mutex_lock(&g_lock); int v=g_active; pthread_mutex_unlock(&g_lock); return v; }
int audio_is_paused(void) { pthread_mutex_lock(&g_lock); int v=g_paused; pthread_mutex_unlock(&g_lock); return v; }
int audio_pos_ms(void)    { pthread_mutex_lock(&g_lock); int v=g_pos_ms; pthread_mutex_unlock(&g_lock); return v; }
/* R29: see g_last_peak's own comment -- a raw abs-sample peak, scale
 * depends on which decode path produced it, meaningful only relative to
 * other reads taken during the same track's own capture. */
int32_t audio_current_peak(void) { pthread_mutex_lock(&g_lock); int32_t v=g_last_peak; pthread_mutex_unlock(&g_lock); return v; }
int audio_seek_pending_ms(void) { pthread_mutex_lock(&g_lock); int v=g_seek_to_ms; pthread_mutex_unlock(&g_lock); return v; }
int audio_dur_ms(void)    { pthread_mutex_lock(&g_lock); int v=g_dur_ms; pthread_mutex_unlock(&g_lock); return v; }
void audio_set_volume(int p){ if(p<0)p=0; if(p>100)p=100; pthread_mutex_lock(&g_lock); g_vol=p; pthread_mutex_unlock(&g_lock); }
void audio_set_next(const char *path) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_next_path, sizeof(g_next_path), "%s", path ? path : "");
    pthread_mutex_unlock(&g_lock);
}

int audio_take_advance(void) {
    pthread_mutex_lock(&g_lock);
    int v = g_advance;
    g_advance = 0;
    pthread_mutex_unlock(&g_lock);
    return v;
}

int audio_volume(void)    { pthread_mutex_lock(&g_lock); int v=g_vol; pthread_mutex_unlock(&g_lock); return v; }
