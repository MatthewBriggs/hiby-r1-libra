/* music_hook.c — a music and radio app on the R1's About launcher tile.
 *
 * LD_PRELOAD into hiby_player, re-point a launcher tile's callback at us, and
 * take the framebuffer and input for as long as the app is open.
 *
 *   tile     about, callback slots 0x00892150 and 0x00892570 (the tile shows
 *            up twice in hiby_player's own data, both patched so it's caught
 *            wherever the launcher reads it from)
 *   cave     0x00760800, a zeroed run in .rodata
 *
 * This app used to live on the Stream media tile, chain-loading a separate
 * standalone Podcasts app onto the About tile from its own constructor.
 * Podcasts is built into this app directly now (see podcast.c), so that
 * second app was retired and this hook moved onto the About tile in its
 * place -- Stream media is untouched and back to its stock behaviour.
 *
 * Addresses were read straight out of hiby_player rather than found by
 * scanning a live device: tile records hold their name as an inline string at
 * +0x00 and the callback at +0x48, so a new firmware can be re-derived in
 * seconds. See the tile-table notes in the repo.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <linux/fb.h>
#include <linux/input.h>

/* RP7 musl experiment: <linux/input.h>'s struct input_event is polymorphic
 * on __USE_TIME_BITS64 -- glibc on this 32-bit target leaves it undefined
 * (the legacy 4+4-byte `struct timeval time` layout), but musl always
 * defines it (64-bit time_t on every platform, no override exists -- a
 * deliberate musl design choice, not a bug). This device's actual kernel
 * (4.4.94, ~2016) only ever emits the legacy layout, so a musl build using
 * the system struct directly would desync every read() at the very first
 * event: not just a wrong timestamp, a wrong *stride*, corrupting touch and
 * key input outright. Bypassing the system struct entirely for this one
 * interface -- own fixed layout, correct on both builds, never dependent on
 * which time_t width the toolchain defaults to. */
typedef struct {
    int32_t tv_sec, tv_usec;
    uint16_t type, code;
    int32_t value;
} r1_input_event_t;

#include "text.h"
#include "icons.h"
#include "library.h"
#include "audiobook.h"
#include "podcast.h"
#include "audio.h"
#include "eq.h"
#include "eqprofile.h"
#include "mseb.h"
#include "recent.h"
#include "cover.h"
#include "art.h"
#include "lastfm.h"
#include "spotify.h"
#include "index.h"
#include "scanner.h"
#include "status.h"
#include "radio.h"
#include "playlist.h"

/* ---- device geometry ----------------------------------------------------- */
#define FB_W 480
#define FB_H 800

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
/* R41: variables behind these names, not literals -- same reasoning as
 * COL_ACCENT below, so a light theme can repoint every draw call without
 * touching any of them. apply_theme() is the only place that assigns these. */
static uint16_t g_col_bg     = RGB(16, 16, 20);
static uint16_t g_col_header = RGB(26, 26, 33);
static uint16_t g_col_text   = RGB(240, 240, 245);
static uint16_t g_col_dim    = RGB(125, 125, 136);
static uint16_t g_col_row    = RGB(26, 26, 33);
static uint16_t g_col_line   = RGB(38, 38, 46);
#define COL_BG      g_col_bg
#define COL_HEADER  g_col_header
#define COL_TEXT    g_col_text
#define COL_DIM     g_col_dim
#define COL_ROW     g_col_row
#define COL_LINE    g_col_line
/* A variable behind this name, not a literal -- R20 wants the accent
 * colour user-selectable, and every draw call already says COL_ACCENT. */
static uint16_t g_accent = RGB(240, 138, 42);
#define COL_ACCENT  g_accent

/* R20: a small preset palette rather than a full colour picker -- matching
 * the hardware's own colour variants (or close to them) is the actual ask,
 * not arbitrary RGB entry. */
static const struct { const char *name; uint16_t color; } ACCENT_PRESETS[] = {
    { "Orange", RGB(240, 138, 42)  },
    { "Gold",   RGB(212, 175, 55)  },
    { "Silver", RGB(180, 186, 194) },
    { "Red",    RGB(224, 70, 70)   },
    { "Blue",   RGB(70, 140, 224)  },
    { "Green",  RGB(90, 180, 100)  },
    { "Purple", RGB(160, 100, 220) },
};
#define ACCENT_N ((int)(sizeof(ACCENT_PRESETS) / sizeof(ACCENT_PRESETS[0])))
static int g_accent_idx;
/* R41/R43: light theme is an exact per-channel invert of the dark palette
 * above (light = 255 - dark, per channel), which is what guarantees every
 * contrast relationship dark mode already relies on -- text-vs-background,
 * dim-vs-background, line-vs-background -- survives the swap unchanged
 * rather than needing to be independently re-balanced. Grey isn't an invert
 * of anything -- R43 asked for a genuine middle ground, not dark-lite or
 * light-lite, so its six values are their own deliberate design: a mid-grey
 * field (not near-black, not near-white) with light text, sized to read
 * comfortably in daylight without light theme's starkness or dark theme's
 * depth. Accent presets are untouched across all three, per R41's own
 * "maintaining accent colour functionality" -- they're saturated enough to
 * read against any of them. */
#define THEME_DARK  0
#define THEME_LIGHT 1
#define THEME_GREY  2
#define THEME_AUTO  3
#define THEME_MODE_N 4
static const char *THEME_MODE_NAMES[THEME_MODE_N] = { "Dark", "Light", "Grey", "Auto" };
static int theme_mode;
/* R45: defined below tz_offset_minutes(), which it needs -- forward
 * declared here so apply_theme() can call it regardless of definition
 * order. Returns THEME_LIGHT/THEME_DARK/THEME_GREY directly (never
 * THEME_AUTO) -- see its own comment for why Grey gets a say here now. */
static int auto_theme_effective(void);
/* RP1 follow-up: defined near music_init() below, forward declared here so
 * go_back() and the header tap handler can both read g_is_standalone
 * regardless of definition order. See g_is_standalone's own comment. */
static int is_hiby_player(void);
/* Set once, at the top of music_entry(), from is_hiby_player() -- true in
 * the LD_PRELOAD-hooked build (running inside hiby_player's own process),
 * false in the standalone build (library_standalone, its own process, no
 * launcher underneath). Backing out of Main Menu means something different
 * in each: hooked, it hands control back to hiby_player's own launcher
 * (go_back() returning 0, same as always); standalone, there is no launcher
 * to hand back to, and go_back() returning 0 there just made
 * standalone_main.c's own re-entry loop restart the whole app -- which
 * looked exactly like a crash-restart from the outside, reported live as
 * one. Main Menu is the true root in standalone; nowhere left to go from
 * there is nowhere left to go, not a boundary to bounce off. */
static int g_is_standalone;
static void apply_theme(void) {
    /* R45: Auto isn't its own palette -- it picks Light, Dark or Grey by
     * time of day and applies that one, same as if the user had picked it
     * directly. theme_mode itself still reads "Auto" everywhere it's
     * displayed or saved; only the six colour variables below are ever
     * affected. */
    int effective = theme_mode;
    if (effective == THEME_AUTO) effective = auto_theme_effective();
    switch (effective) {
    case THEME_LIGHT:
        g_col_bg     = RGB(239, 239, 235);
        g_col_header = RGB(229, 229, 222);
        g_col_text   = RGB(15, 15, 10);
        g_col_dim    = RGB(130, 130, 119);
        g_col_row    = RGB(229, 229, 222);
        g_col_line   = RGB(217, 217, 209);
        break;
    case THEME_GREY:
        g_col_bg     = RGB(60, 60, 66);
        g_col_header = RGB(72, 72, 79);
        g_col_text   = RGB(235, 235, 238);
        g_col_dim    = RGB(170, 170, 178);
        g_col_row    = RGB(72, 72, 79);
        g_col_line   = RGB(85, 85, 92);
        break;
    default:
        g_col_bg     = RGB(16, 16, 20);
        g_col_header = RGB(26, 26, 33);
        g_col_text   = RGB(240, 240, 245);
        g_col_dim    = RGB(125, 125, 136);
        g_col_row    = RGB(26, 26, 33);
        g_col_line   = RGB(38, 38, 46);
        break;
    }
}

/* R44: a fixed UTC-offset list rather than full IANA tzdata -- this device
 * has no zoneinfo database to speak of, and R45's only actual use for this
 * (a sunrise/sunset estimate) needs nothing more precise than "roughly which
 * meridian." DST is deliberately not modelled: it depends on region and date
 * in a way a single offset can't express, and getting it wrong twice a year
 * would be worse than a sunrise/sunset guess that's steadily an hour off in
 * summer. Ordered west to east so the picker reads as a line around the
 * globe, not an alphabetised list. */
static const struct { const char *name; int offset_min; } TZ_PRESETS[] = {
    { "UTC-12:00",              -720 },
    { "UTC-11:00",              -660 },
    { "UTC-10:00 (Hawaii)",     -600 },
    { "UTC-9:00 (Alaska)",      -540 },
    { "UTC-8:00 (Pacific)",     -480 },
    { "UTC-7:00 (Mountain)",    -420 },
    { "UTC-6:00 (Central)",     -360 },
    { "UTC-5:00 (Eastern)",     -300 },
    { "UTC-4:00 (Atlantic)",    -240 },
    { "UTC-3:00 (Argentina)",   -180 },
    { "UTC-2:00",               -120 },
    { "UTC-1:00 (Azores)",       -60 },
    { "UTC+0:00 (London)",         0 },
    { "UTC+1:00 (Central Europe)", 60 },
    { "UTC+2:00 (Eastern Europe)", 120 },
    { "UTC+3:00 (Moscow)",       180 },
    { "UTC+3:30 (Iran)",         210 },
    { "UTC+4:00 (Dubai)",        240 },
    { "UTC+4:30 (Kabul)",        270 },
    { "UTC+5:00 (Pakistan)",     300 },
    { "UTC+5:30 (India)",        330 },
    { "UTC+5:45 (Nepal)",        345 },
    { "UTC+6:00 (Bangladesh)",   360 },
    { "UTC+6:30 (Myanmar)",      390 },
    { "UTC+7:00 (Bangkok)",      420 },
    { "UTC+8:00 (China/Singapore)", 480 },
    { "UTC+9:00 (Japan/Korea)",  540 },
    { "UTC+9:30 (Adelaide)",     570 },
    { "UTC+10:00 (Sydney)",      600 },
    { "UTC+11:00",               660 },
    { "UTC+12:00 (NZ)",          720 },
    { "UTC+13:00",               780 },
    { "UTC+14:00",               840 },
};
#define TZ_N ((int)(sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0])))
static int tz_idx = 12;   /* UTC+0:00, a neutral default rather than guessing */
/* Getter for other subsystems (R45's sunrise/sunset calc) to query the
 * current offset without reaching into tz_idx/TZ_PRESETS directly. */
static int tz_offset_minutes(void) { return TZ_PRESETS[tz_idx].offset_min; }

/* R45: Auto mode's day/night test. Real sunrise/sunset needs latitude, which
 * this device has no way to collect on its own -- R44 only gathers a UTC
 * offset, not a full geographic fix. Rather than pretend to a precision this
 * app can't have, this assumes a fixed mid-latitude (40 deg N, roughly New
 * York/Madrid/Beijing) and combines it with the real day-of-year and the
 * timezone offset to get a genuine seasonal swing in day length -- accurate
 * for anyone actually near that latitude, and still the right *shape* of
 * behaviour (later dawn and earlier dusk in winter, the reverse in summer)
 * everywhere else, rather than a fixed clock-hour cutoff that would not
 * account for seasons at all. Cooper's equation for solar declination, the
 * standard approximate form (accurate to about 1 degree, which is closer
 * than the assumed latitude itself is to anyone's actual position). */
/* R45 follow-up: reported live -- Grey was never actually reachable from
 * Auto, at any time of day. The original version only ever chose between
 * Light and Dark; Grey existed as a manually-selectable theme but had no
 * role in Auto at all, which reads as a bug once Grey exists -- an "Auto"
 * that ignores one of the three themes it could pick from isn't really
 * automatic over all of them. Fixed by giving Grey the twilight band
 * itself: a fixed 45-minute window straddling both sunrise and sunset,
 * Light for the rest of the day and Dark for the rest of the night. 45
 * minutes, not a fraction of day length, because the *reason* for a
 * transition theme -- easing the eye between a bright and a dark palette
 * -- doesn't get longer just because the day itself does. */
#define AUTO_THEME_TWILIGHT_HOURS 0.75   /* 45 minutes, each side */
static int auto_theme_effective(void) {
    time_t now = time(NULL);
    struct tm u;
    gmtime_r(&now, &u);
    int day_of_year = u.tm_yday + 1;             /* 1..366 */
    double local_hour = u.tm_hour + u.tm_min / 60.0 + u.tm_sec / 3600.0
                       + tz_offset_minutes() / 60.0;
    /* Wrap into [0,24) -- the offset above can push it either side of the
     * UTC day, and only the time-of-day matters here, not which calendar
     * day it lands on. */
    local_hour = fmod(local_hour, 24.0);
    if (local_hour < 0) local_hour += 24.0;

    const double rad = M_PI / 180.0;
    const double assumed_lat = 40.0;
    double decl = 23.45 * sin(rad * (360.0 / 365.0) * (284 + day_of_year));
    double cos_omega = -tan(assumed_lat * rad) * tan(decl * rad);
    if (cos_omega > 1.0) return THEME_DARK;    /* sun never rises at this latitude today */
    if (cos_omega < -1.0) return THEME_LIGHT;  /* sun never sets at this latitude today */
    double omega = acos(cos_omega) / rad;    /* degrees */
    double sunrise = 12.0 - omega / 15.0;
    double sunset  = 12.0 + omega / 15.0;
    if (fabs(local_hour - sunrise) <= AUTO_THEME_TWILIGHT_HOURS ||
        fabs(local_hour - sunset)  <= AUTO_THEME_TWILIGHT_HOURS)
        return THEME_GREY;
    return (local_hour >= sunrise && local_hour < sunset) ? THEME_LIGHT : THEME_DARK;
}

static int button_lock_enabled;   /* off by default: a new gesture, opt in */
/* "USB Transport Mode" (R37, extended) -- off by default, same reasoning as
 * button_lock_enabled: a new behaviour, opt in. Displayed name changed from
 * "Bypass DSP on USB" to reflect that it's grown past just the DSP bypass
 * (now also pins volume at 100%, on the reasoning that a USB DAC/amp should
 * own its own volume rather than have this app attenuate ahead of it) --
 * the persisted config key (usb_bypass_enabled) and internal identifiers
 * are unchanged, since it's still fundamentally one on/off setting and
 * renaming the key would silently drop whatever a device already has saved.
 * usb_bypass_active tracks whether the override is *currently* engaged
 * (distinct from the setting itself: engaged only while output is actually
 * USB); usb_bypass_bt_was_on and usb_bypass_saved_vol remember what
 * Bluetooth and the volume were before this turned them off/pinned them, so
 * leaving USB puts both back exactly as deep_suspend() already does for its
 * own Bluetooth teardown -- never silently changing a setting the user
 * chose themselves. */
static int usb_bypass_enabled;

/* R81: start playing when a headset connects. Off by default -- it makes the
 * device produce sound without anyone touching it, which must be opted into.
 * Detected in bt_poll() (which already asks st_bt_name() every ~5s, so this
 * costs no extra polling) and acted on by the main thread, the same handoff
 * bt_eq_pending_path already uses: audio_toggle() belongs to the UI thread. */
static int bt_autoplay_enabled;
static int bt_autoplay_pending;      /* set under bt_lock, cleared by the main loop */
static int usb_bypass_active;
static int usb_bypass_bt_was_on;
static int usb_bypass_saved_vol;

#define TEXT_PX_TITLE 34
#define TEXT_PX_BODY  30
#define TEXT_PX_SMALL 22

/* Was 62 -- widened so the header actions (MSEB's Reset, Podcasts' Sync)
 * have a taller strip to land a tap in, not just a wider one. */
#define HEADER_H  78
/* Height of the status strip. Deliberately not STATUS_H-the-include-guard:
 * status.h used that name to guard itself, so whichever came second lost —
 * either this constant was redefined, or the header's contents were skipped
 * entirely. */
#define STATUS_H  32
#define CONTENT_Y (STATUS_H + HEADER_H)
#define ROW_H     72
/* R50: the Queue screen's one extra header line (track count + remaining
 * playtime), fixed above the scrolling rows rather than the first row's own
 * slot -- vis_rows()/scroll_to_px() both need to know the list itself has
 * this much less room on that one screen. */
#define QUEUE_SUMMARY_H 44
/* How many consecutive ticks since inertia was last active a scrollable
 * list needs before a tap is trusted as a real row selection rather than
 * the tail end of a fling. ~100ms at the ~30/s tick rate this loop runs
 * at. See list_settled_ticks's own comment for why this is keyed on
 * inertia_active alone, not list_dragging. */
#define LIST_TAP_SETTLE_TICKS 3

/* ---- tile hook ----------------------------------------------------------- */
#define ABOUT_CB_1    0x00892150u
#define ABOUT_CB_2    0x00892570u   /* the live About tile callback */
#define ABOUT_CB_ORIG 0x0053BC20u   /* what it holds on a stock 2.0.25/2.0.26 */
#define CAVE_ADDR     0x00760800u
#define CAVE_PAGE     (CAVE_ADDR & ~0xFFFu)
#define DATA_PAGE     0x00892000u
#define PAGE_SPAN     0x2000u

/* Was /tmp, which a reboot wipes — and a lockup is always followed by a
 * reboot, so the one log that mattered was never there afterwards. */
#define LOG_PATH "/usr/data/music.log"

static uint32_t orig_cb;

/* Set at each step of the index gesture, read by the watchdog. If the loop
 * stalls, the last phase named here is where it stalled. */
static volatile const char *g_phase = "idle";
static volatile unsigned    g_tick;

/* Bound the log. Four writers (mlog here, alog in audio.c, ilog in index.c,
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

static void mlog(const char *fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    /* Timestamped, like the audio log: without one there is no telling a
     * normal run of short tracks from the player racing through them. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[300];
    int m = snprintf(line, sizeof(line), "[%6ld.%03ld] %s",
                     (long)ts.tv_sec, ts.tv_nsec / 1000000L, b);
    if (m <= 0) return;
    if (m > (int)sizeof(line)) m = (int)sizeof(line);
    log_roll_if_big(LOG_PATH);
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, line, (size_t)m); close(fd); }
}

/* ---- drawing ------------------------------------------------------------- */
/* ---- artwork ------------------------------------------------------------- */
/* Loaded off the UI thread: a first-time extract-and-decode is a few hundred
 * milliseconds, and the transport must stay live throughout. The UI draws a
 * plain panel until the bitmap appears. */
#define ART_PX FB_W   /* Now Playing art runs edge-to-edge, like stock */
/* R83: requested live as "down by 5mm", for the transport row (play/pause
 * and its neighbours) on all three playing screens that have one -- music/
 * podcast and audiobook, not radio, which nobody asked to move. This
 * panel's own physical size isn't stated anywhere in this repo (checked);
 * ~15px/mm falls out of the R1's commonly cited 2.45" diagonal against this
 * screen's actual 480x800 (sqrt(480^2+800^2)/2.45in), a suspiciously round
 * number that's some real confirmation it's in the right neighbourhood
 * rather than a bare guess. Worth revisiting if that spec is ever found
 * written down somewhere more authoritative than a review page.
 * R83 follow-up: brought back up 10px, then another 5px, then another
 * 10px, then another 10px, on live feedback that the 5mm push went too
 * far. */
#define CTRL_NUDGE_PX 40
/* BG104: a Last.fm/Spotify cover comes back at whatever resolution the host
 * chose to publish, sometimes well over a thousand pixels a side -- fine to
 * store, but confirmed live to cause a visible flicker on this device's
 * panel (see BACKLOG.md), most likely a downsampling artifact that a large
 * reduction ratio makes bad enough to be visible in cover_load()'s own box
 * filter. 800 keeps real detail (this screen never shows a cover past
 * ART_PX/FB_W) while landing nowhere near that ratio for anything a host
 * plausibly serves. */
#define FETCHED_COVER_MAX_DIM 800

/* BG104 follow-up (R85): the same flicker, reported live again -- "high
 * resolution images lightly flickering". The original fix only pre-shrank
 * network-fetched covers before they ever reached cover_load()'s own box
 * filter; a *local* source just as large (a user's own oversized
 * cover.jpg, or embedded art extracted from the file) went straight in at
 * full size, hitting the identical box-filter behaviour. Not applied by
 * overwriting the user's own file in their album folder the way the
 * fetched path overwrites its own cache copy -- that file is theirs, not
 * this app's, and shrinking it in place the first time anyone's cover
 * happened to be large would be a surprising thing for a music player to
 * do to a library. A real folder image gets copied into this scratch path
 * first and the copy is what gets shrunk/loaded; embedded art already
 * lands in ART_SCRATCH (art_candidate()'s own doc comment: rewritten every
 * time, never the source track file), so it's shrunk in place directly,
 * same as a freshly-fetched cover already is. */
/* One scratch pair per art worker, never shared.
 *
 * cover_load_capped() rewrites its input file in place (cover_downscale_max()
 * decodes, shrinks and re-encodes over the top of it), so a scratch path is
 * exclusively owned by one decode for the whole of that decode. art_worker()
 * (the playing track) and view_art_worker() (the album being browsed) both
 * used ART_SCRATCH, and all three workers shared one local-copy path -- so
 * opening an album page while a track's art loaded had one worker rewriting
 * the very file the other was part-way through reading. The decode kept the
 * rows it had already read and never got the rest: art correct down to some
 * horizontal line and flat grey below it, which is exactly how this was
 * reported ("A Kind of Magic", "At War with the Mystics").
 *
 * Latent since the scratch was introduced, but it only became reproducible
 * with the cover-colour work (R92/R94): computing a palette per album, and
 * prewarming palettes in the background, turned concurrent art decoding from
 * rare into the normal case. The prewarm pass was given its own path at the
 * time for exactly this reason -- these two just never got the same. */
#define ART_LOCAL_SCRATCH        "/tmp/.music_art_local.jpg"
#define ART_VIEW_SCRATCH         "/tmp/.music_art_view.jpg"
#define ART_VIEW_LOCAL_SCRATCH   "/tmp/.music_art_view_local.jpg"
#define COVER_PREWARM_LOCAL_SCRATCH "/tmp/.music_art_prewarm_local.jpg"

/* jpg: whatever art_candidate() just handed back -- either ART_SCRATCH
 * (embedded art, already disposable) or a real file in the album's own
 * folder. key: unaffected either way, since it's already keyed by track or
 * folder path, never by which physical file the pixels came from this
 * time. cover_downscale_max() is cheap to call unconditionally: a header-
 * only read and an immediate no-op return for anything already within
 * FETCHED_COVER_MAX_DIM, which is the common case for most covers. */
/* Set by the hold-the-artwork-to-refresh gesture, consumed once by the next
 * art worker to start. A cached cover is otherwise permanent: nothing in the
 * app ever decides an entry is wrong, so an album that cached badly (the
 * shared-scratch collision above did exactly that) stayed wrong until someone
 * deleted files over adb. This is the way to say "that one is wrong, do it
 * again" without leaving the device. */
static volatile int art_force_fresh;

/* The gesture itself: forget what this album cached -- bitmap and colours --
 * and ask for it again from source. The palette file has to go too, or the
 * refreshed art would be drawn under the colours derived from the bad decode
 * it replaced. */
static void art_refresh_request(const char *track, const char *artist,
                                const char *album, const char *album_artist,
                                int for_view);

static uint16_t *cover_load_capped(const char *jpg, const char *key, int px,
                                   const char *local_scratch, int fresh) {
    const char *use = jpg;
    if (strncmp(jpg, "/tmp/", 5) != 0) {
        FILE *in = fopen(jpg, "rb");
        if (in) {
            FILE *out = fopen(local_scratch, "wb");
            if (out) {
                char buf[8192];
                size_t got;
                while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
                    fwrite(buf, 1, got, out);
                fclose(out);
                use = local_scratch;
            }
            fclose(in);
        }
    }
    cover_downscale_max(use, FETCHED_COVER_MAX_DIM);
    return fresh ? cover_load_fresh(use, key, px) : cover_load(use, key, px);
}

static pthread_mutex_t art_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *art_bits;            /* ART_PX * ART_PX, or NULL */
static char      art_want[512];       /* track the loader should be showing */
/* R23: artist/album to search Last.fm with if no local art turns up for
 * art_want -- empty from any caller that shouldn't trigger that fallback
 * at all (podcasts, audiobooks, radio: none of these are "albums" Last.fm
 * would sensibly match against). */
static char      art_want_artist[LIB_NAME_LEN];
/* Separate from art_want_artist on purpose -- see art_request()'s own
 * R23/BG-orphee comment for why the two can legitimately differ and
 * only one of them is safe to use for "is this still the same album". */
static char      art_want_album_artist[LIB_NAME_LEN];
static char      art_want_album[LIB_NAME_LEN];
/* Bluetooth codec and battery are read on their own thread. They were read
 * from the drawing path, and both shell out — bluealsa-cli and dbus-send. A
 * D-Bus call that blocks (a headset dropping mid-query will do it) then takes
 * the whole UI with it, which looks exactly like the player locking up. The
 * drawing now only ever reads these two strings. */
static pthread_mutex_t bt_lock = PTHREAD_MUTEX_INITIALIZER;
static char bt_codec_cached[32];
static int  bt_batt_cached = -1;
static int  bt_poll_run;
static pthread_t bt_thread;
static int  bt_thread_valid;

/* BG38 (part 2): fuzzy-match a saved EQ profile to whichever headphone is
 * connected, so Sonys getting the Sonys' profile and Jabras getting the
 * Jabras' doesn't need a manual pick every session.
 *
 * The actual eq_switch_to()/save_conf() call has to happen on the main
 * thread: eq_cur/eq_cur_path and everything save_conf() reads are only
 * ever touched from there today, with no lock protecting them, the same
 * as every other UI-owned global in this file. Doing the switch directly
 * from this background thread would be a new, real data race against
 * whatever the main thread is doing with the EQ screen at the same
 * moment. So this thread only ever does the (cheap, stateless) fuzzy
 * match and hands the winning path across through bt_lock -- the same
 * handoff shape already used for the pending Bluetooth volume delta --
 * and the main loop drains it and performs the switch itself. */
static char bt_matched_name[64];      /* device this thread last matched */
static char bt_eq_pending_path[EP_PATH_LEN];   /* set here, applied on the main thread */
/* R100: distinct from bt_eq_pending_path's own empty-means-nothing-new
 * meaning -- that alone can't tell "haven't matched yet" apart from
 * "matched, and there genuinely isn't one for this device", and the two
 * need different handling (leave whatever EQ state was already active vs.
 * explicitly turn it off). Set opposite ways at the two places
 * bt_match_profile() can end: 0 the moment a match is found (alongside
 * setting the pending path itself), 1 if the scan runs out without one. */
static int bt_eq_no_match;

/* Case-insensitive, either-direction substring match against each profile's
 * own name (from its filename): forgiving of a device advertising a
 * slightly different string than the profile was saved under ("Sony
 * WH-1000XM4" vs "WH-1000XM4"), which is the whole reason this is fuzzy
 * rather than exact. A minimum length on the shorter side keeps a short or
 * generic profile name (a stray "EQ" or "1") from matching almost anything
 * by accident. First match wins; ep_scan()'s own directory order decides
 * ties, same as everywhere else profiles are listed. Runs on the bt_poll
 * thread, so a local array only -- see the comment above for why it must
 * not touch the shared eq_profiles[]. */
static void bt_match_profile(const char *dev_name) {
    ep_entry_t profiles[EP_MAX_PROFILES];
    int n = ep_scan(profiles, EP_MAX_PROFILES);
    for (int i = 0; i < n; i++) {
        size_t dn = strlen(dev_name), pn = strlen(profiles[i].name);
        if ((dn < 4 && dn <= pn) || (pn < 4 && pn <= dn)) continue;
        if (strcasestr(dev_name, profiles[i].name) ||
            strcasestr(profiles[i].name, dev_name)) {
            pthread_mutex_lock(&bt_lock);
            snprintf(bt_eq_pending_path, sizeof(bt_eq_pending_path), "%s",
                    profiles[i].path);
            bt_eq_no_match = 0;
            pthread_mutex_unlock(&bt_lock);
            return;
        }
    }
    /* R100: reported live -- connecting a headset with no saved profile
     * left whatever EQ curve a *previous* device's profile (or the user's
     * own manual setting) had left active, rather than turning it off for
     * a device nothing was ever tuned for. */
    pthread_mutex_lock(&bt_lock);
    bt_eq_no_match = 1;
    pthread_mutex_unlock(&bt_lock);
}

static void *bt_poll(void *arg) {
    (void)arg;
    while (bt_poll_run) {
        int on_bt = audio_using_bt();
        if (on_bt) {
            char c[32];
            st_bt_codec(c, sizeof(c));
            int b = st_bt_battery();
            audio_bt_volume_service();
            pthread_mutex_lock(&bt_lock);
            snprintf(bt_codec_cached, sizeof(bt_codec_cached), "%s", c);
            bt_batt_cached = b;
            pthread_mutex_unlock(&bt_lock);
        } else {
            pthread_mutex_lock(&bt_lock);
            bt_codec_cached[0] = '\0';
            bt_batt_cached = -1;
            pthread_mutex_unlock(&bt_lock);
        }
        /* BG38 (part 2): connection-based, not audio_using_bt()'s
         * playback-based -- reconnecting a headset with nothing actively
         * routing through it yet (paused, or between tracks) must still
         * get its profile matched rather than wait for playback to touch
         * Bluetooth again, which is the bug a real test caught. st_bt_name()
         * already answers "is something connected" on its own (bt_pcm_path()
         * looks for a live sink PCM in bluealsa's registry -- the same check
         * bt_sink_connected() in audio.c makes for routing), with its own
         * internal 10s cache, so running it every cycle regardless of on_bt
         * is cheap. */
        char nm[64];
        st_bt_name(nm, sizeof(nm));
        if (nm[0]) {
            if (strcmp(nm, bt_matched_name) != 0) {
                /* R81: an empty -> non-empty name is a headset arriving, which
                 * is exactly the transition this thread already watches for to
                 * match an EQ profile. Flagged rather than acted on here:
                 * audio_toggle() is the UI thread's, same as the EQ handoff
                 * immediately above. */
                if (bt_matched_name[0] == '\0' && bt_autoplay_enabled) {
                    pthread_mutex_lock(&bt_lock);
                    bt_autoplay_pending = 1;
                    pthread_mutex_unlock(&bt_lock);
                }
                bt_match_profile(nm);
                snprintf(bt_matched_name, sizeof(bt_matched_name), "%s", nm);
            }
        } else {
            bt_matched_name[0] = '\0';
        }
        /* Checking for a pending volume write is a cheap mutex read, not a
         * D-Bus round-trip, so it can happen on every sub-tick without
         * cost — the volume key would otherwise wait up to the full ~5s
         * cycle to actually reach the mixer.
         *
         * That reasoning only holds when there is a Bluetooth mixer to write
         * to. Off Bluetooth there is no pending volume this thread can
         * service and nothing it can learn by looking, so the fine-grained
         * sub-tick was ten wakeups a second, indefinitely, to discover
         * nothing -- and this thread lives for the whole life of the process,
         * including every hour the device spends in a pocket. Same ~5s
         * cycle either way; only the granularity changes. */
        int sub_us = on_bt ? 100000 : 1000000;
        int subs   = on_bt ? 50 : 5;
        for (int i = 0; i < subs && bt_poll_run; i++) {
            usleep(sub_us);
            if (on_bt && audio_bt_volume_pending()) audio_bt_volume_service();
        }
    }
    return NULL;
}

static pthread_t art_thread;
static int       art_thread_valid;
static int       art_seq_v;      /* bumped when the bitmap changes */
/* Set when a request is dispatched, cleared when art_worker() finishes --
 * whether that landed a bitmap or gave up. art_seq_v alone can't answer
 * "has this track's art finished loading": art_request() bumps it the
 * moment art_bits is cleared to NULL for a new album, well before the
 * worker has even started. Exists for one reader: the resume-from-idle
 * splash, which needs to know when it is safe to reveal the real screen
 * without a blank cover flashing first (see its own comment). */
static volatile int art_done = 1;

static int art_seq(void) {
    pthread_mutex_lock(&art_lock);
    int v = art_seq_v;
    pthread_mutex_unlock(&art_lock);
    return v;
}

/* R92: Now Playing colours derived from the current album art -- a second
 * attempt at a feature built once already (by another session), shipped,
 * and explicitly reverted. Live feedback on why: the background always
 * ended up plain white or black rather than an actual shade, and the
 * chosen colours were never checked against each other for contrast (an
 * unreadable seek bar was the specific complaint). Both addressed below,
 * not just patched over -- see compute_cover_palette()'s own comment for
 * the mechanism. Persisted like any other Settings toggle
 * (cover_palette_enabled); the three colours themselves are runtime-only,
 * recomputed from art_bits, never saved. */
static int      cover_palette_enabled;
static uint16_t np_bg = 0, np_accent = 0, np_fg = 0xFFFF;
static int       np_palette_seq = -1;   /* art_seq_v this was last computed for */

static void rgb565_to_rgb8(uint16_t c, int *r, int *g, int *b) {
    int r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    *r = (r5 * 255 + 15) / 31;
    *g = (g6 * 255 + 31) / 63;
    *b = (b5 * 255 + 15) / 31;
}
static uint16_t rgb8_to_rgb565(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static float pal_luma(int r, int g, int b) {
    return (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
}
/* HSV-style saturation (0..1) -- cheap, and all that's needed to tell a
 * genuinely-coloured pixel from a desaturated near-black/near-white one,
 * which is the entire fix for "background always ended up white or
 * black": that outcome is what picking the single *most common* pixel in
 * a luma range does on typical art, where a big desaturated dark border
 * or a bright white highlight usually outnumbers any one saturated shade.
 * Scoring bins by population *and* saturation together fixes it without
 * needing a smarter histogram -- an unpopular-but-colourful bin can now
 * outscore a popular-but-grey one. */
static float pal_sat(int r, int g, int b) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return mx > 0 ? (float)(mx - mn) / (float)mx : 0.0f;
}
static void rgb8_to_hsl(int r, int g, int b, float *H, float *S, float *L) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float mx = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
    float mn = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
    float l = (mx + mn) / 2.0f, h = 0.0f, s = 0.0f;
    if (mx != mn) {
        float d = mx - mn;
        s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == rf)      h = (gf - bf) / d + (gf < bf ? 6.0f : 0.0f);
        else if (mx == gf) h = (bf - rf) / d + 2.0f;
        else                h = (rf - gf) / d + 4.0f;
        h /= 6.0f;
    }
    *H = h; *S = s; *L = l;
}
static float pal_hue2rgb(float p, float q, float t) {
    if (t < 0) t += 1; if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6.0f;
    return p;
}
static void hsl_to_rgb8(float h, float s, float l, int *r, int *g, int *b) {
    float rf, gf, bf;
    if (s <= 0.0f) { rf = gf = bf = l; }
    else {
        float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        float p = 2.0f * l - q;
        rf = pal_hue2rgb(p, q, h + 1.0f / 3);
        gf = pal_hue2rgb(p, q, h);
        bf = pal_hue2rgb(p, q, h - 1.0f / 3);
    }
    *r = (int)(rf * 255.0f + 0.5f);
    *g = (int)(gf * 255.0f + 0.5f);
    *b = (int)(bf * 255.0f + 0.5f);
}

/* 4 bits/channel -- enough bins to keep genuinely different colours apart
 * without spreading one real shade's pixels across dozens of near-
 * identical bins, which would let population-based scoring get swamped by
 * quantization noise. */
#define PAL_BINS 4096
static int pal_bin_of(int r, int g, int b) {
    return ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
}
static void pal_bin_rgb(int bin, int *r, int *g, int *b) {
    *r = (((bin >> 8) & 0xF) << 4) | 8;
    *g = (((bin >> 4) & 0xF) << 4) | 8;
    *b = ((bin & 0xF) << 4) | 8;
}

/* Pure function: given an ART_PX*ART_PX bitmap, derive bg/accent/fg. No
 * globals touched, so this is equally usable live (compute_cover_palette(),
 * from art_bits) and offline (cover_prewarm_worker(), from a bitmap decoded
 * for an album that was never played). Dark and light candidates are
 * pooled into separate histograms by luma, scored by count*saturation so a
 * common but grey pixel can't win over a rarer but genuinely-coloured one
 * (see pal_sat()'s own comment -- this is the fix for "always ended up
 * plain white or black"), then the winning accent is pushed in HSL
 * lightness away from the winning background until they actually
 * contrast, rather than trusting whatever the histogram happened to pick
 * (the fix for "the seek bar looked awful"). */
static void derive_palette_from_bits(const uint16_t *bits, uint16_t *out_bg,
                                     uint16_t *out_accent, uint16_t *out_fg) {
    /* R94 follow-up: reported live as general lag and lockups right after
     * the prewarm pass shipped -- these were `static`, reused between
     * calls to avoid a malloc/free pair, back when this only ever ran on
     * the UI thread. cover_prewarm_worker() now calls this same function
     * from its own thread, so a live palette computation (a track
     * changing) and the background prewarm pass could land inside this
     * function at the same time, both reading and writing the *same*
     * unprotected buffers -- a data race, not just a slowdown, and a
     * plausible cause of an outright hang or corruption depending on
     * exactly where the two interleaved. Heap-allocated per call instead:
     * 32KB, freed before returning, cheap next to decoding the cover
     * itself and the only way two threads can each get their own copy
     * without a mutex serializing them against each other. */
    uint32_t *hist_dark  = calloc(PAL_BINS, sizeof(uint32_t));
    uint32_t *hist_light = calloc(PAL_BINS, sizeof(uint32_t));
    if (!hist_dark || !hist_light) { free(hist_dark); free(hist_light); return; }

    int total = ART_PX * ART_PX;
    for (int i = 0; i < total; i += 4) {   /* every 4th pixel: plenty for a histogram */
        int r, g, b;
        rgb565_to_rgb8(bits[i], &r, &g, &b);
        float l = pal_luma(r, g, b);
        int bin = pal_bin_of(r, g, b);
        if (l < 0.45f)      hist_dark[bin]++;
        else if (l > 0.62f) hist_light[bin]++;
        /* midtones (0.45-0.62) deliberately counted toward neither -- a
         * clean gap between the two roles instead of a boundary that
         * quantization noise could flip either way. */
    }

    int best_dark = -1, best_light = -1;
    float best_dark_score = 0, best_light_score = 0;
    for (int i = 0; i < PAL_BINS; i++) {
        if (hist_dark[i]) {
            int r, g, b; pal_bin_rgb(i, &r, &g, &b);
            float score = (float)hist_dark[i] * (0.25f + 0.75f * pal_sat(r, g, b));
            if (score > best_dark_score) { best_dark_score = score; best_dark = i; }
        }
        if (hist_light[i]) {
            int r, g, b; pal_bin_rgb(i, &r, &g, &b);
            float score = (float)hist_light[i] * (0.25f + 0.75f * pal_sat(r, g, b));
            if (score > best_light_score) { best_light_score = score; best_light = i; }
        }
    }

    int bgr = 20, bgg = 20, bgb = 24;         /* fallback: a plain dark neutral */
    int acr = 255, acg = 255, acb = 255;      /* fallback: white */
    if (best_dark >= 0)  pal_bin_rgb(best_dark, &bgr, &bgg, &bgb);
    if (best_light >= 0) pal_bin_rgb(best_light, &acr, &acg, &acb);

    /* Contrast check, not a trust-the-histogram guess: push the accent's
     * own lightness away from the background's until the gap is real,
     * preserving its hue/saturation (still "purple", just a purple that
     * actually reads against this particular background) rather than
     * sliding it toward grey. */
    float bh, bs, bl, ah, as, al;
    rgb8_to_hsl(bgr, bgg, bgb, &bh, &bs, &bl);
    rgb8_to_hsl(acr, acg, acb, &ah, &as, &al);
    #define NP_MIN_L_DELTA 0.35f
    if (al - bl < NP_MIN_L_DELTA) {
        al = bl + NP_MIN_L_DELTA;
        if (al > 1.0f) al = 1.0f;
        if (as < 0.15f) as = 0.4f;   /* a washed-out accent gained no hue to push toward */
        hsl_to_rgb8(ah, as, al, &acr, &acg, &acb);
    }

    *out_bg     = rgb8_to_rgb565(bgr, bgg, bgb);
    *out_accent = rgb8_to_rgb565(acr, acg, acb);
    *out_fg     = pal_luma(bgr, bgg, bgb) > 0.5f ? 0x0000 : 0xFFFF;

    free(hist_dark);
    free(hist_light);
}

/* R94: on-disk cache so an album's palette only ever needs deriving once --
 * either here, live, the first time it's actually played, or ahead of time
 * by cover_prewarm_worker() for a recently-added album that was never
 * opened at all. Keyed on artist+album (a djb2 hash, same shape
 * wave_cache_path() already uses for a track path) since that's the
 * granularity the whole feature themes at -- every track on an album
 * shares one palette, the same reasoning art_request()'s own R84 same-
 * album skip already established for the art bitmap itself.
 *
 * R94 follow-up: a version byte, not the versionless "just delete the
 * directory once" this comment originally said -- reported live as a
 * cover that now decodes correctly (see cover.c's own zero-scanline-band
 * fix) still theming wrong, because the *palette* cached from the old,
 * corrupted decode had no way to know it was ever built from bad input.
 * Bumping PAL_CACHE_VERSION invalidates every existing entry the same way
 * WAVE_CACHE_VERSION already does for the waveform cache -- the right
 * lever for exactly this "derive_palette_from_bits()'s own output for the
 * same input just changed" case, which a per-file mtime check (the cover
 * art itself didn't change; how it's read did) couldn't have caught
 * anyway. */
#define PAL_CACHE_DIR "/data/mnt/sd_0/.cover_palettes"
#define PAL_CACHE_VERSION 2
static void pal_cache_path(const char *artist, const char *album, char *out, size_t n) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)artist; *p; p++)
        h = ((h << 5) + h) ^ *p;
    h = ((h << 5) + h) ^ '\x1f';
    for (const unsigned char *p = (const unsigned char *)album; *p; p++)
        h = ((h << 5) + h) ^ *p;
    snprintf(out, n, "%s/%08lx.pal", PAL_CACHE_DIR, h & 0xFFFFFFFFul);
}
static int pal_cache_load(const char *artist, const char *album,
                          uint16_t *bg, uint16_t *accent, uint16_t *fg) {
    if (!artist[0] || !album[0]) return 0;   /* podcasts/radio: no stable key */
    char p[512];
    pal_cache_path(artist, album, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    uint8_t ver;
    int ok = fread(&ver, 1, 1, f) == 1 && ver == PAL_CACHE_VERSION;
    uint16_t v[3];
    ok = ok && fread(v, sizeof(v[0]), 3, f) == 3;
    fclose(f);
    if (ok) { *bg = v[0]; *accent = v[1]; *fg = v[2]; }
    return ok;
}
static void pal_cache_save(const char *artist, const char *album,
                           uint16_t bg, uint16_t accent, uint16_t fg) {
    if (!artist[0] || !album[0]) return;
    mkdir(PAL_CACHE_DIR, 0755);
    char p[512], tmp[520];
    pal_cache_path(artist, album, p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint8_t ver = PAL_CACHE_VERSION;
    uint16_t v[3] = { bg, accent, fg };
    int ok = fwrite(&ver, 1, 1, f) == 1 && fwrite(v, sizeof(v[0]), 3, f) == 3;
    fclose(f);
    if (!ok || rename(tmp, p) != 0) unlink(tmp);
}

/* R98: whether np_bg/np_accent/np_fg actually hold a real, current palette
 * -- separate from whether art_bits (the full ART_PX*ART_PX bitmap) has
 * finished loading. Reported live as the theme applying noticeably later
 * than it could: every np_col_*() helper gated on art_bits being non-NULL,
 * so even a *cached* palette -- available instantly, before this function
 * even runs a second time -- sat unused until the much slower full-art
 * decode/blit pipeline also finished. The two are genuinely independent:
 * art_want_artist/art_want_album (what pal_cache_load() keys on) are set
 * synchronously by art_request() the moment a track changes, well before
 * art_worker() has even started decoding, let alone finished -- a cache
 * hit is known immediately. */
static int np_palette_valid;

/* Recomputes np_bg/np_accent/np_fg for whatever art_bits currently holds,
 * only when it has actually changed (np_palette_seq vs art_seq_v -- the
 * same R84 same-album-skip signal Now Playing's own art already uses, so
 * this costs nothing on a same-album track change). Checks the on-disk
 * cache first -- a hit (this album was played before, or cover_prewarm_
 * worker() got to it first) skips the histogram pass entirely *and* the
 * wait for art_bits, which is what makes a freshly-scanned album's very
 * first playthrough show its real theme immediately rather than the plain
 * default while either runs. */
static void compute_cover_palette(void) {
    int seq = art_seq();
    if (seq == np_palette_seq) return;
    np_palette_seq = seq;

    if (pal_cache_load(art_want_album_artist, art_want_album, &np_bg, &np_accent, &np_fg)) {
        np_palette_valid = 1;
        return;
    }

    pthread_mutex_lock(&art_lock);
    uint16_t *bits = art_bits;
    /* Sampled into a local scratch copy so the lock isn't held through the
     * whole histogram pass -- this runs on the UI thread, right before a
     * draw, and art_worker() replacing art_bits mid-scan would otherwise
     * block a fresh cover fetch on however long that takes. ART_PX*ART_PX
     * uint16_t -- the same size Now Playing's own blit already reads
     * whole every frame, so one more copy of it is not a new cost class. */
    static uint16_t *scratch;
    if (bits) {
        if (!scratch) scratch = malloc((size_t)ART_PX * ART_PX * sizeof(uint16_t));
        if (scratch) memcpy(scratch, bits, (size_t)ART_PX * ART_PX * sizeof(uint16_t));
        else bits = NULL;
    }
    pthread_mutex_unlock(&art_lock);
    if (!bits) { np_palette_valid = 0; return; }   /* nothing cached, art not loaded yet either */

    derive_palette_from_bits(bits, &np_bg, &np_accent, &np_fg);
    pal_cache_save(art_want_album_artist, art_want_album, np_bg, np_accent, np_fg);
    np_palette_valid = 1;
}

/* R94: precomputes recently-added albums' palettes right after a library
 * scan, so opening one of them for the first time already has its theme
 * ready instead of showing the plain default for the one frame
 * compute_cover_palette()'s own live histogram pass would otherwise take.
 * Own scratch path (art.h's own art_candidate() doc comment already
 * anticipates this: "the scanner's background cover pre-warm pass...can
 * run at any time relative to whichever UI art loader happens to be
 * active") -- ART_SCRATCH is shared by art_worker()/view_art_worker(), and
 * this can genuinely race either of those since it runs unprompted after
 * any scan, not just when the user is looking at Now Playing. */
#define COVER_PREWARM_N 10   /* matches RECENT_ALBUMS_N's own "last 10" */
#define COVER_PREWARM_SCRATCH "/tmp/.music_art_prewarm.jpg"
typedef struct {
    char artist[LIB_NAME_LEN];
    char album[LIB_NAME_LEN];
    char track[LIB_PATH_LEN];
} cover_prewarm_item_t;
static cover_prewarm_item_t cover_prewarm_items[COVER_PREWARM_N];
static int       cover_prewarm_n;
static pthread_t cover_prewarm_thread;
/* R94 follow-up: reported live as lag/lockups. This used to be joined from
 * cover_prewarm_start() on the UI thread whenever a previous pass was
 * still running -- decoding up to 10 covers is real work on this CPU, and
 * blocking the UI thread on it, for however long it took, is a stall
 * severe enough to read as a lockup. Detached instead (never joined at
 * all -- see cover_prewarm_start()'s own comment), with this plain flag
 * as the only "is one already running" signal: set before the thread
 * starts, cleared as the very last thing the worker itself does. */
static volatile int cover_prewarm_running;

/* Every artwork worker calls this first, and it is the whole reason they can
 * run at all without being heard.
 *
 * Measured over a verified Bluetooth capture: 49 dropouts and 14.2 s of
 * audio missing entirely across 13 minutes, in gaps of 280-715 ms, one at
 * every track change, every music/audiobook/podcast switch, and even while
 * merely scrolling menus. Each gap sat exactly on top of a burst where this
 * single core was pinned at 100%, with an artwork thread taking 47-67% of
 * it -- decoding a cover is genuinely expensive here.
 *
 * What makes that fatal specifically on Bluetooth is apply_decode_priority()
 * in audio.c: it gives the decode thread nice -8 on the local DAC, but
 * deliberately leaves it at 0 over Bluetooth so it cannot starve bluealsa's
 * encoder, which is a separate process downstream of us. Correct as far as
 * it goes -- but it also left decode with no advantage over *our own*
 * background threads, so artwork and audio competed as equals and audio lost.
 *
 * Lowering the artwork threads is the fix that does not re-open that: the
 * decoder keeps exactly the share it has now, bluealsa's encoder keeps its
 * share, and only this work yields. Nobody notices a cover arriving a
 * fraction of a second later; everybody hears a 700 ms hole. */
#define ART_WORKER_NICE 10

static void art_worker_yield_priority(const char *what) {
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), ART_WORKER_NICE) != 0)
        mlog("[music] %s: could not lower thread priority\n", what);
}

static void *cover_prewarm_worker(void *arg) {
    (void)arg;
    art_worker_yield_priority("cover-prewarm");
    for (int i = 0; i < cover_prewarm_n; i++) {
        cover_prewarm_item_t *it = &cover_prewarm_items[i];
        uint16_t bg, accent, fg;
        if (pal_cache_load(it->artist, it->album, &bg, &accent, &fg))
            continue;   /* a live play already cached this one since the scan */

        char jpg[LIB_PATH_LEN], key[LIB_PATH_LEN];
        uint16_t *bits = NULL;
        for (int n = 0; !bits; n++) {
            int rc = art_candidate(it->track, n, jpg, sizeof(jpg), key, sizeof(key),
                                   COVER_PREWARM_SCRATCH);
            if (rc == -1) break;
            if (rc == ART_SKIP) continue;
            bits = cover_load_capped(jpg, key, ART_PX, COVER_PREWARM_LOCAL_SCRATCH, 0);
        }
        /* Local-candidate only, deliberately -- no Last.fm/Spotify fallback
         * here the way view_art_worker() has for a manually-opened album.
         * This runs unattended right after any scan, including one that
         * just found a large batch of new music; spending a network round
         * trip per album with no local cover, for albums nobody has
         * necessarily even looked at yet, is a cost this background pass
         * has no business taking on its own initiative. A played track
         * still gets the fetched-cover treatment normally, through
         * art_worker()/compute_cover_palette() same as always. */
        if (bits) {
            derive_palette_from_bits(bits, &bg, &accent, &fg);
            pal_cache_save(it->artist, it->album, bg, accent, fg);
            free(bits);
        }
    }
    cover_prewarm_running = 0;
    return NULL;
}

/* UI-thread only: lib_albums_recent_added()/lib_tracks_for_album() touch
 * g_db, which every other art-adjacent worker in this file (art_worker(),
 * view_art_worker(), artist_art_worker()) avoids doing off the UI thread --
 * the snapshot into cover_prewarm_items[] happens here, synchronously
 * (cheap: at most 10 small queries), and only the slow part -- decoding up
 * to 10 covers and hashing them -- runs on cover_prewarm_worker()'s own
 * thread, detached (see cover_prewarm_running's own comment) rather than
 * ever joined from here. A pass still running from an earlier trigger
 * just means this one is skipped outright -- another scan finishing, or
 * the toggle being flipped again, tries again later; nothing is lost by
 * not stacking a second pass on top of one already in flight. */
static void cover_prewarm_start(void) {
    if (!cover_palette_enabled || cover_prewarm_running) return;
    lib_row_t rows[COVER_PREWARM_N];
    int n = lib_albums_recent_added(rows, COVER_PREWARM_N);
    cover_prewarm_n = 0;
    for (int i = 0; i < n && cover_prewarm_n < COVER_PREWARM_N; i++) {
        lib_track_t one[1];
        if (lib_tracks_for_album(rows[i].owner, rows[i].name, one, 1, rows[i].count) < 1)
            continue;
        cover_prewarm_item_t *it = &cover_prewarm_items[cover_prewarm_n];
        /* Same artist-tag-first, album-artist-fallback art_request() itself
         * uses at play time (see play_index()'s own call) -- has to match
         * exactly, since this is the cache key compute_cover_palette()
         * will look this album up under later. */
        snprintf(it->artist, sizeof(it->artist), "%s",
                one[0].artist[0] ? one[0].artist : rows[i].owner);
        snprintf(it->album, sizeof(it->album), "%s", rows[i].name);
        snprintf(it->track, sizeof(it->track), "%s", one[0].path);
        cover_prewarm_n++;
    }
    if (cover_prewarm_n > 0) {
        cover_prewarm_running = 1;
        if (pthread_create(&cover_prewarm_thread, NULL, cover_prewarm_worker, NULL) == 0)
            pthread_detach(cover_prewarm_thread);
        else
            cover_prewarm_running = 0;
    }
}

/* Drop-in replacements for COL_BG/COL_ACCENT/COL_TEXT at any Now Playing
 * draw site that should follow the cover art -- each falls straight back
 * to the plain theme colour whenever the toggle is off or there is no art
 * to derive from, so a call site never needs its own if/else. */
static uint16_t np_col_bg(void)     { return (cover_palette_enabled && np_palette_valid) ? np_bg     : COL_BG; }
static uint16_t np_col_accent(void) { return (cover_palette_enabled && np_palette_valid) ? np_accent : COL_ACCENT; }
static uint16_t np_col_fg(void)     { return (cover_palette_enabled && np_palette_valid) ? np_fg     : COL_TEXT; }
/* R92 follow-up: reported live -- "on dark backgrounds, all text should be
 * white". Secondary text (artist/album, track count, the clock) was left
 * at plain COL_DIM regardless of the art, which stayed a fixed grey no
 * matter how dark the background got. np_fg is already exactly "white on
 * a dark background, black on a light one" (see compute_cover_palette()'s
 * own comment) -- this is the same colour, just falling back to COL_DIM
 * rather than COL_TEXT when the feature is off, so a call site that used
 * to be dim stays dim with cover colours disabled. */
static uint16_t np_col_dim(void)    { return (cover_palette_enabled && np_palette_valid) ? np_fg     : COL_DIM; }
/* R96: the mode (shuffle/repeat) and queue buttons' own ring+glyph --
 * explicit request that both go white (ring and icon alike) whenever the
 * background is genuinely dark, reverting to the plain COL_DIM grey on a
 * light background or with the feature off. np_fg is already exactly
 * "white on a dark background, black on a light one" (see
 * compute_cover_palette()'s own comment) -- reusing it as the is-dark
 * signal rather than re-deriving one, and only actually returning white
 * for the dark case (np_fg's own black value is never what's wanted here,
 * even on a light background -- COL_DIM is). */
static uint16_t np_col_circle(void) {
    if (!(cover_palette_enabled && np_palette_valid)) return COL_DIM;
    return np_fg == 0xFFFF ? 0xFFFF : COL_DIM;
}
/* R92 follow-up: reported live -- the unplayed track/prev-next-button glyphs
 * kept their plain COL_LINE/COL_TEXT even once the background went dark,
 * reading as invisible or low-contrast rather than merely dim. COL_LINE is
 * a light-theme divider grey with no relationship to whatever this album's
 * own background turned out to be; blending toward np_fg instead keeps it
 * visibly *there* -- a real "unplayed" marker, not just the background
 * showing through -- while still reading as secondary next to the accent
 * fill and np_fg's own full-contrast text/icons. */
static uint16_t np_blend(uint16_t a, uint16_t b, float t) {
    int ar, ag, ab, br, bg, bb;
    rgb565_to_rgb8(a, &ar, &ag, &ab);
    rgb565_to_rgb8(b, &br, &bg, &bb);
    return rgb8_to_rgb565(ar + (int)((br - ar) * t),
                          ag + (int)((bg - ag) * t),
                          ab + (int)((bb - ab) * t));
}
static uint16_t np_col_line(void) {
    return (cover_palette_enabled && np_palette_valid) ? np_blend(np_bg, np_fg, 0.35f) : COL_LINE;
}

static void *art_worker(void *arg) {
    (void)arg;
    art_worker_yield_priority("art");
    char track[512], artist[LIB_NAME_LEN], album[LIB_NAME_LEN];
    char album_artist[LIB_NAME_LEN];
    int fresh = art_force_fresh;
    art_force_fresh = 0;
    pthread_mutex_lock(&art_lock);
    snprintf(track, sizeof(track), "%s", art_want);
    snprintf(artist, sizeof(artist), "%s", art_want_artist);
    snprintf(album, sizeof(album), "%s", art_want_album);
    snprintf(album_artist, sizeof(album_artist), "%s", art_want_album_artist);
    pthread_mutex_unlock(&art_lock);

    char jpg[512], key[512];
    uint16_t *bits = NULL;
    /* Walk the candidates until one actually decodes, rather than trusting the
     * first file that exists. cover_load() returning NULL is a normal outcome,
     * not an error: a large progressive cover or an over-size image is declined
     * on purpose to keep the player out of the OOM killer, and a PNG someone
     * named .jpg simply fails. Any of those used to mean a blank panel even
     * when the track had good embedded art a few candidates further down. */
    for (int n = 0; !bits; n++) {
        int rc = art_candidate(track, n, jpg, sizeof(jpg), key, sizeof(key), ART_SCRATCH);
        if (rc == -1) break;
        if (rc == ART_SKIP) continue;
        bits = cover_load_capped(jpg, key, ART_PX, ART_LOCAL_SCRATCH, fresh);
    }

    /* R23: every local candidate is exhausted -- try Last.fm first, then
     * Spotify, if there's an artist/album to search with at all (empty for
     * anything that isn't a regular music track -- see art_want_artist/
     * album's own comment), a network to reach either with, and at least
     * one of the two has an API key/credentials configured. Last.fm goes
     * first: no auth round trip needed for a plain read, so a hit there is
     * cheaper. Spotify is the fallback specifically because its search
     * matches oddly-titled live/bootleg/reissue releases (checked live:
     * Last.fm had no match at all for a Velvet Underground bootleg volume,
     * exactly this shape of title) that Last.fm's fan-tagged database
     * often doesn't carry, the same reason Navidrome itself chains
     * multiple agents rather than trusting one. Saved as this album's own
     * cover.jpg, so every other track of it (this session and every one
     * after) finds it the ordinary folder-image way from here on -- this
     * only ever runs once per album, not once per track. A miss from both
     * is remembered with one sentinel file so a real "no match anywhere"
     * doesn't retry (and re-hit both APIs) every single time the album is
     * opened. */
    if (!bits && artist[0] && album[0] && st_net_up() &&
        (lastfm_has_key() || spotify_has_key())) {
        char dir[512], dest[560], nomatch[580];
        album_dir(track, dir, sizeof(dir));
        snprintf(dest, sizeof(dest), "%s/cover.jpg", dir);
        snprintf(nomatch, sizeof(nomatch), "%s/.cover_no_match", dir);
        struct stat nm_st;
        if (stat(nomatch, &nm_st) != 0) {
            int ok = lastfm_has_key() && lastfm_fetch_cover(artist, album, dest) == 0;
            const char *source = "lastfm";
            if (!ok && spotify_has_key() && spotify_fetch_cover(artist, album, dest) == 0) {
                ok = 1;
                source = "spotify";
            }
            if (ok) {
                /* BG104: shrink before caching/showing it -- see
                 * FETCHED_COVER_MAX_DIM's own comment. A no-op return (0)
                 * when the fetch was already small enough; either way
                 * cover_load() below reads whatever ended up on disk. */
                cover_downscale_max(dest, FETCHED_COVER_MAX_DIM);
                bits = cover_load(dest, dir, ART_PX);
                mlog("[music] %s: cover fetched for %s / %s\n", source, artist, album);
            } else {
                FILE *f = fopen(nomatch, "w");
                if (f) fclose(f);
                mlog("[music] art: no match anywhere for %s / %s\n", artist, album);
            }
        }
    }

    pthread_mutex_lock(&art_lock);
    /* R84: discard only if the user has moved on to a different *album* --
     * or, for anything with no artist/album at all (audiobooks/podcasts/
     * radio, which pass "" -- see art_request()'s own comment), a
     * different track, the same exact-match check this always used, since
     * there's no album grouping to fall back to there. Comparing by album
     * rather than by exact track path is what lets art_request() skip
     * spawning a new worker at all for a same-album track change without
     * a worker already in flight from the track *before* that one getting
     * wrongly discarded here once art_want itself has moved on further. */
    /* BG-orphee: compares album_artist, same reasoning as art_request()'s
     * own same_album check just above -- see its comment. */
    int stale = (album_artist[0] && album[0])
              ? (strcmp(album_artist, art_want_album_artist) != 0 || strcmp(album, art_want_album) != 0)
              : (strcmp(track, art_want) != 0);
    if (stale) { free(bits); }
    else { free(art_bits); art_bits = bits; art_seq_v++; }
    pthread_mutex_unlock(&art_lock);
    art_done = 1;
    return NULL;
}

/* Defined with the rest of the Now Playing layout, further down. */
static void title_reset(void);

/* artist/album: only a regular music track has both -- pass "" from
 * anywhere else (podcasts, audiobooks, radio) to leave the Last.fm
 * fallback in art_worker() off for those. */
static void art_request(const char *track, const char *artist, const char *album,
                        const char *album_artist) {
    /* New track, new title: a scroll position carried over from the last one
     * would leave the new name starting halfway along. Called from every path
     * that changes what is playing, chapters included. */
    title_reset();
if (art_thread_valid) { pthread_join(art_thread, NULL); art_thread_valid = 0; }
    pthread_mutex_lock(&art_lock);
    /* R84: reported live as the cover disappearing and reappearing between
     * tracks of the same album -- not a redraw glitch, this was genuinely
     * blanking art_bits and re-decoding/re-fetching identical artwork on
     * every track change, same album or not. Same album_artist+album as the
     * still-current request means the same cover, so art_bits (read by
     * both Now Playing and the mini player -- see BG70's own comment)
     * stays exactly as it is and no worker gets spawned at all. art_want
     * (the track path) still updates either way, so a worker still in
     * flight from the previous track -- a slow Last.fm/Spotify fetch,
     * say -- isn't discarded by its own "has the user moved on" check
     * once it finishes; it's fetching the same album's art regardless of
     * which of that album's tracks is current by the time it lands.
     *
     * BG-orphee: this compares album_artist, not the track's own artist
     * R23 passes through as `artist` -- reported live as the cover (and,
     * worse, the cover-colour palette below, which keys on the same pair)
     * reloading on every single track of an album where the *tag* genuinely
     * varies per track (Johann Johannsson's "Orphee", credited to a
     * different soloist -- Clarice Jensen, Anthony Weeden, Paul Hillier --
     * on several tracks) despite it being one release with one cover
     * throughout. R23 already established that `artist` legitimately isn't
     * a stable per-album identity for exactly this reason (a compilation);
     * album_artist -- the album's own artist tag, not any one track's --
     * is what stays constant across a release regardless of who the tags
     * credit per track, which is what "same album" actually needs to mean
     * here. `artist` is kept as its own parameter rather than folded away:
     * it is still the right thing to display and the right thing to search
     * Last.fm/Spotify with (R23's own reasoning for preferring it holds for
     * both of those), just not what decides whether art needs reloading. */
    int same_album = album_artist && album_artist[0] && album && album[0] &&
                     !strcmp(album_artist, art_want_album_artist) && !strcmp(album, art_want_album);
    if (!same_album) {
        free(art_bits);
        art_bits = NULL;
        art_seq_v++;
    }
    snprintf(art_want, sizeof(art_want), "%s", track);
    snprintf(art_want_artist, sizeof(art_want_artist), "%s", artist ? artist : "");
    snprintf(art_want_album, sizeof(art_want_album), "%s", album ? album : "");
    snprintf(art_want_album_artist, sizeof(art_want_album_artist), "%s",
             album_artist ? album_artist : "");
    pthread_mutex_unlock(&art_lock);
    if (same_album) return;
    art_done = 0;
    if (pthread_create(&art_thread, NULL, art_worker, NULL) == 0)
        art_thread_valid = 1;
    else
        art_done = 1;   /* never actually started -- nothing to wait for */
}

/* BG70: a completely separate bitmap from art_bits above, not a save/
 * restore around the same one. The album-detail screen's cover used to
 * call art_request() (the playing track's own loader) to show whatever
 * album was being *browsed* -- but art_bits is also what the mini-player
 * and Now Playing read, so the moment you opened a different album than
 * whatever was actually playing, the mini-player's thumbnail flipped to
 * the browsed album too, for the entire time that screen stayed open, not
 * just after leaving it. A go_back()-time restore only patched the "after"
 * half; the two states need to never share a buffer in the first place. */
static pthread_mutex_t view_art_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *view_art_bits;
static char      view_art_want[512];
static char      view_art_want_artist[LIB_NAME_LEN];
static char      view_art_want_album[LIB_NAME_LEN];
static pthread_t view_art_thread;
static int       view_art_thread_valid;
static int       view_art_seq_v;
/* R63: set once view_art_worker() has exhausted every candidate (local, and
 * network if it got that far) for the *current* request -- lets the album
 * page tell "nothing to show yet, still working" apart from "confirmed,
 * nothing is ever coming for this album", so it can stop reserving ART_PX
 * of blank cover space above the tracklist once the second one is actually
 * known, rather than leaving it there forever. With no network to fall back
 * to, the worker resolves this almost immediately (the local candidate scan
 * is a handful of stat()s plus at most one decode); with a network it waits
 * for the real fetch, same as the cover itself would. Reset by
 * view_art_request() at the start of every new request; only the worker
 * whose track still matches view_art_want at the end may set it -- same
 * staleness rule view_art_bits itself already follows, so a late-finishing
 * stale worker can't mark a newer, still-in-flight request "done". */
static int       view_art_done;

static int view_art_seq(void) {
    pthread_mutex_lock(&view_art_lock);
    int v = view_art_seq_v;
    pthread_mutex_unlock(&view_art_lock);
    return v;
}

/* True once view_art_done is set and no bitmap turned up: confirmed nothing
 * is coming for the album currently on screen, not merely nothing yet. */
static int view_art_gone(void) {
    pthread_mutex_lock(&view_art_lock);
    int gone = view_art_done && !view_art_bits;
    pthread_mutex_unlock(&view_art_lock);
    return gone;
}

/* R63 audit: tracks_hdr_title_y() (and everything chained off it below --
 * artist_y, info_y, hdr_h, tracks_max_px(), the cover-box draw) used to call
 * view_art_gone() itself, which takes view_art_lock. A single frame chains
 * through 4-5 of those calls, and the touch handler's hit-testing does the
 * same on every tap -- all taking and releasing a mutex that's also being
 * fought over by view_art_worker(), for a value that only ever changes once
 * per album view (the moment the worker gives up). Worse, since each call
 * re-locks independently, the worker could flip the answer between two of
 * them inside one draw pass -- e.g. the cover box at line ~4680 drawn "not
 * gone" while tracks_hdr_title_y() moments later reads "gone" and places the
 * title where the still-drawn cover box already is. Snapshotting once per
 * drawn frame (draw_ui(), before draw_screen()) fixes both: one lock/unlock
 * instead of several, and every reader in that frame -- and every touch
 * event before the next frame redraws -- sees the exact value the screen
 * currently shows. */
static int g_view_art_gone_frame;

static void *view_art_worker(void *arg) {
    (void)arg;
    art_worker_yield_priority("view-art");
    char track[512], artist[LIB_NAME_LEN], album[LIB_NAME_LEN];
    int fresh = art_force_fresh;
    art_force_fresh = 0;
    pthread_mutex_lock(&view_art_lock);
    snprintf(track, sizeof(track), "%s", view_art_want);
    snprintf(artist, sizeof(artist), "%s", view_art_want_artist);
    snprintf(album, sizeof(album), "%s", view_art_want_album);
    pthread_mutex_unlock(&view_art_lock);

    char jpg[512], key[512];
    uint16_t *bits = NULL;
    for (int n = 0; !bits; n++) {
        int rc = art_candidate(track, n, jpg, sizeof(jpg), key, sizeof(key), ART_VIEW_SCRATCH);
        if (rc == -1) break;
        if (rc == ART_SKIP) continue;
        bits = cover_load_capped(jpg, key, ART_PX, ART_VIEW_LOCAL_SCRATCH, fresh);
    }
    /* Same Last.fm-then-Spotify network fallback art_worker() uses, kept
     * in sync deliberately -- an album can be viewed without ever being
     * played, and it deserves the same chance at a fetched cover. */
    if (!bits && artist[0] && album[0] && st_net_up() &&
        (lastfm_has_key() || spotify_has_key())) {
        char dir[512], dest[560], nomatch[580];
        album_dir(track, dir, sizeof(dir));
        snprintf(dest, sizeof(dest), "%s/cover.jpg", dir);
        snprintf(nomatch, sizeof(nomatch), "%s/.cover_no_match", dir);
        struct stat nm_st;
        if (stat(nomatch, &nm_st) != 0) {
            int ok = lastfm_has_key() && lastfm_fetch_cover(artist, album, dest) == 0;
            if (!ok && spotify_has_key() && spotify_fetch_cover(artist, album, dest) == 0)
                ok = 1;
            if (ok) {
                /* BG104: see art_worker()'s matching call for why. */
                cover_downscale_max(dest, FETCHED_COVER_MAX_DIM);
                bits = cover_load(dest, dir, ART_PX);
            } else {
                FILE *f = fopen(nomatch, "w");
                if (f) fclose(f);
            }
        }
    }

    pthread_mutex_lock(&view_art_lock);
    if (strcmp(track, view_art_want) != 0) { free(bits); }
    else { free(view_art_bits); view_art_bits = bits; view_art_seq_v++; view_art_done = 1; }
    pthread_mutex_unlock(&view_art_lock);
    return NULL;
}

static void view_art_request(const char *track, const char *artist, const char *album) {
if (view_art_thread_valid) { pthread_join(view_art_thread, NULL); view_art_thread_valid = 0; }
    pthread_mutex_lock(&view_art_lock);
    free(view_art_bits);
    view_art_bits = NULL;
    view_art_seq_v++;
    view_art_done = 0;
    snprintf(view_art_want, sizeof(view_art_want), "%s", track);
    snprintf(view_art_want_artist, sizeof(view_art_want_artist), "%s", artist ? artist : "");
    snprintf(view_art_want_album, sizeof(view_art_want_album), "%s", album ? album : "");
    pthread_mutex_unlock(&view_art_lock);
    if (pthread_create(&view_art_thread, NULL, view_art_worker, NULL) == 0)
        view_art_thread_valid = 1;
}

/* BG-playlist: playlists have no single album to show art for, but nothing
 * else stops view_art_bits from just being whatever the last real album
 * page left behind -- view_art_request() is never called on the playlist
 * path (there's no track/artist/album triple that means anything here), so
 * without an explicit clear the header silently kept showing stale art from
 * wherever browsing came from. */
static void view_art_clear(void) {
    if (view_art_thread_valid) { pthread_join(view_art_thread, NULL); view_art_thread_valid = 0; }
    pthread_mutex_lock(&view_art_lock);
    free(view_art_bits);
    view_art_bits = NULL;
    view_art_seq_v++;
    view_art_done = 1;
    view_art_want[0] = '\0';
    pthread_mutex_unlock(&view_art_lock);
}

static void art_refresh_request(const char *track, const char *artist,
                                const char *album, const char *album_artist,
                                int for_view) {
    char pal[512];
    /* view_art has no separate album-identity concept of its own (see its
     * own request function -- it always blanks on a new request, per-track,
     * no "same album" case to get wrong), so album_artist is only used to
     * key the palette when this is the played-track path. */
    pal_cache_path(for_view ? artist : album_artist, album, pal, sizeof(pal));
    unlink(pal);
    np_palette_valid = 0;
    art_force_fresh = 1;
    if (for_view) view_art_request(track, artist, album);
    else          art_request(track, artist, album, album_artist);
}

static void view_blit_art_clip(uint16_t *fb, int x, int y, int clip_top, int clip_bot) {
    pthread_mutex_lock(&view_art_lock);
    if (view_art_bits) {
        for (int r = 0; r < ART_PX; r++) {
            int py = y + r;
            if (py < clip_top || py >= clip_bot || py < 0 || py >= FB_H) continue;
            memcpy(fb + (size_t)py * FB_W + x,
                   view_art_bits + (size_t)r * ART_PX,
                   (size_t)ART_PX * sizeof(uint16_t));
        }
    }
    pthread_mutex_unlock(&view_art_lock);
}

/* Artist page: a third independent buffer, same reasoning BG70 already
 * established for view_art_bits above -- this can't share art_bits (the
 * playing track's own cover) or view_art_bits (the browsed album's cover)
 * without the exact cross-contamination bug those two were split apart to
 * fix. Also carries the bio text, fetched in the same worker call since
 * Last.fm's artist.getinfo returns both in one response -- no reason to
 * pay for a second round trip.
 *
 * Unlike album covers (cached as cover.jpg right in the album's own
 * folder), an artist has no single folder to cache into -- their albums
 * can be scattered across many. Cached instead under a dedicated
 * directory, one photo/bio/no-match-sentinel trio per artist, keyed on a
 * filesystem-safe version of the artist's name. */
#define ARTIST_CACHE_DIR "/data/mnt/sd_0/.artist_cache"
static pthread_mutex_t artist_art_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *artist_art_bits;
static char      artist_bio_text[8192];
static char      artist_art_want[LIB_NAME_LEN];
static pthread_t artist_art_thread;
static int       artist_art_thread_valid;
static int       artist_art_seq_v;
/* Distinguishes "fetch still running" from "fetch finished and found
 * nothing" -- artist_bio_text[0] alone can't tell those apart, and the
 * draw side needs to (a placeholder "Loading..." vs simply no bio section
 * at all, per "their bio, if there is one"). */
static int       artist_art_loading;

static int artist_art_seq(void) {
    pthread_mutex_lock(&artist_art_lock);
    int v = artist_art_seq_v;
    pthread_mutex_unlock(&artist_art_lock);
    return v;
}

/* Same "replace anything not filesystem-safe with '_'" this app applies
 * nowhere else (podcast feed names are trusted as-is from feeds.txt, the
 * one existing precedent) -- artist names routinely carry '/' (a genuine
 * character in some names, not just a tag-writer quirk), which would
 * otherwise be read as a path separator by every fopen() below. */
static void artist_cache_key(const char *artist, char *out, size_t out_n) {
    size_t j = 0;
    for (size_t i = 0; artist[i] && j + 1 < out_n; i++) {
        unsigned char c = (unsigned char)artist[i];
        out[j++] = (isalnum(c) || c == '-' || c == ' ') ? (char)c : '_';
    }
    out[j] = '\0';
}

static void *artist_art_worker(void *arg) {
    (void)arg;
    art_worker_yield_priority("artist-art");
    char artist[LIB_NAME_LEN];
    pthread_mutex_lock(&artist_art_lock);
    snprintf(artist, sizeof(artist), "%s", artist_art_want);
    pthread_mutex_unlock(&artist_art_lock);

    char key[LIB_NAME_LEN];
    artist_cache_key(artist, key, sizeof(key));
    mkdir(ARTIST_CACHE_DIR, 0755);
    char jpg[560], bio_path[560], nomatch[560];
    snprintf(jpg, sizeof(jpg), "%s/%s.jpg", ARTIST_CACHE_DIR, key);
    snprintf(bio_path, sizeof(bio_path), "%s/%s.bio.txt", ARTIST_CACHE_DIR, key);
    snprintf(nomatch, sizeof(nomatch), "%s/%s.no_match", ARTIST_CACHE_DIR, key);

    uint16_t *bits = NULL;
    char bio[8192] = "";

    /* Cached from an earlier visit this device has already made -- no
     * network at all, same as any local-file art candidate. */
    struct stat st;
    int have_jpg = stat(jpg, &st) == 0 && st.st_size > 0;
    /* R85: same box-filter flicker risk as any other large source -- jpg
     * here is always this app's own ARTIST_CACHE_DIR copy (fetched or
     * previously cached), never a file the user owns, so shrinking it in
     * place is exactly what the fetched-cover path already does to its own
     * cache copy. Cheap no-op for anything already within bounds. */
    if (have_jpg) cover_downscale_max(jpg, FETCHED_COVER_MAX_DIM);
    if (have_jpg) bits = cover_load(jpg, key, ART_PX);
    if (have_jpg && !bits) {
        /* Cached file exists but doesn't decode -- confirmed live this is
         * Last.fm's own "no photo for this artist" response, which still
         * points to an *image* URL rather than omitting one: a generic
         * placeholder graphic served as a PNG, which this device's JPEG-
         * only decoder (cover.c dlopens libjpeg, nothing else -- same gap
         * BG52 found for a podcast cover) can't read. Discarded rather than
         * trusted forever, so the fetch below gets a real second chance --
         * at Spotify, if nothing else. */
        unlink(jpg);
        have_jpg = 0;
    }
    FILE *bf = fopen(bio_path, "rb");
    if (bf) {
        size_t n = fread(bio, 1, sizeof(bio) - 1, bf);
        bio[n] = '\0';
        fclose(bf);
    }

    int need_jpg = !bits;
    int need_bio = !bio[0];
    if ((need_jpg || need_bio) && artist[0] && st_net_up() &&
        (lastfm_has_key() || spotify_has_key())) {
        struct stat nm_st;
        if (stat(nomatch, &nm_st) != 0) {
            int got_jpg = have_jpg, got_bio = bio[0] != '\0';
            if (lastfm_has_key()) {
                char lf_bio[8192] = "";
                int rc = lastfm_fetch_artist(artist, jpg, lf_bio, sizeof(lf_bio));
                if (rc > 0) {
                    /* Decode it now, not just trust the download -- see the
                     * cached-file comment above for why a "successful"
                     * fetch can still be an undecodable placeholder. Only
                     * a real decode counts as having a photo, and only
                     * that skips the Spotify fallback below. */
                    if (rc & 1) {
                        cover_downscale_max(jpg, FETCHED_COVER_MAX_DIM);   /* R85 */
                        bits = cover_load(jpg, key, ART_PX);
                        if (bits) got_jpg = 1;
                        else unlink(jpg);
                    }
                    if ((rc & 2) && lf_bio[0]) {
                        snprintf(bio, sizeof(bio), "%s", lf_bio);
                        got_bio = 1;
                        FILE *wf = fopen(bio_path, "wb");
                        if (wf) { fputs(bio, wf); fclose(wf); }
                    }
                }
            }
            if (!got_jpg && spotify_has_key() &&
                spotify_fetch_artist_image(artist, jpg) == 0) {
                cover_downscale_max(jpg, FETCHED_COVER_MAX_DIM);   /* R85 */
                bits = cover_load(jpg, key, ART_PX);
                if (bits) got_jpg = 1;
            }
            if (!got_jpg && !got_bio) {
                FILE *f = fopen(nomatch, "w");
                if (f) fclose(f);
            }
            /* Reported live: photo and bio worked fine within a session but
             * were gone after a full device reboot -- fclose() only flushes
             * to the OS's own page cache, not to the physical SD card, and
             * this app's own deploy-time reboots are the only ones that
             * think to call sync() first (see deploy.sh's own comment on
             * this exact class of loss). A user-initiated reboot has no
             * reason to wait for a background fetch's writes to reach the
             * card, and FAT-family filesystems (what these cards format
             * as) have no journal to replay lost writes from afterward.
             * Forced durable here, once per fetch rather than per write,
             * since this already only runs on a background thread and a
             * fetch is inherently a multi-second operation next to which
             * one sync() is noise. */
            if (got_jpg || got_bio) sync();
            mlog("[music] artist page: %s -> photo %s, bio %s\n",
                 artist, got_jpg ? "yes" : "no", got_bio ? "yes" : "no");
        }
    }

    pthread_mutex_lock(&artist_art_lock);
    if (strcmp(artist, artist_art_want) != 0) {
        free(bits);
    } else {
        free(artist_art_bits);
        artist_art_bits = bits;
        snprintf(artist_bio_text, sizeof(artist_bio_text), "%s", bio);
        artist_art_seq_v++;
        artist_art_loading = 0;
    }
    pthread_mutex_unlock(&artist_art_lock);
    return NULL;
}

static void artist_art_request(const char *artist) {
    /* Reported live: navigating to the artist page took a couple of
     * seconds, every time. Root cause -- unlike view_art_request()/
     * art_request() above (whose own pthread_join here rarely costs
     * anything, since most album covers resolve from a local file in
     * milliseconds), this screen has no local fallback at all: every visit
     * is a real network round trip, several seconds long. Joining a
     * still-running previous fetch before starting a new one meant
     * re-entering this screen while an earlier fetch was still in flight
     * blocked the UI thread for however long that earlier fetch had left.
     * Detached instead of joined: the old thread finishes and cleans up on
     * its own, and its eventual result is silently discarded by the
     * strcmp() staleness check already in artist_art_worker() (artist_art_
     * want is overwritten below, under the same lock, before that check
     * ever runs) -- nothing here actually depended on the join for
     * correctness, only for tidiness. */
    if (artist_art_thread_valid) {
        pthread_detach(artist_art_thread);
        artist_art_thread_valid = 0;
    }
    pthread_mutex_lock(&artist_art_lock);
    free(artist_art_bits);
    artist_art_bits = NULL;
    artist_bio_text[0] = '\0';
    artist_art_seq_v++;
    artist_art_loading = 1;
    snprintf(artist_art_want, sizeof(artist_art_want), "%s", artist ? artist : "");
    pthread_mutex_unlock(&artist_art_lock);
    if (pthread_create(&artist_art_thread, NULL, artist_art_worker, NULL) == 0)
        artist_art_thread_valid = 1;
}

static void artist_blit_art_clip(uint16_t *fb, int x, int y, int clip_top, int clip_bot) {
    pthread_mutex_lock(&artist_art_lock);
    if (artist_art_bits) {
        for (int r = 0; r < ART_PX; r++) {
            int py = y + r;
            if (py < clip_top || py >= clip_bot || py < 0 || py >= FB_H) continue;
            memcpy(fb + (size_t)py * FB_W + x,
                   artist_art_bits + (size_t)r * ART_PX,
                   (size_t)ART_PX * sizeof(uint16_t));
        }
    }
    pthread_mutex_unlock(&artist_art_lock);
}

static void blit_art(uint16_t *fb, int x, int y) {
    pthread_mutex_lock(&art_lock);
    if (art_bits) {
        for (int r = 0; r < ART_PX; r++)
            memcpy(fb + (size_t)(y + r) * FB_W + x,
                   art_bits + (size_t)r * ART_PX,
                   (size_t)ART_PX * sizeof(uint16_t));
    }
    pthread_mutex_unlock(&art_lock);
}

/* R46: blit_art() writes ART_PX rows unconditionally with no bounds check --
 * fine every other place it's called, since the cover never moves there, but
 * the album-detail screen scrolls its cover along with everything else, so y
 * can land anywhere from well above 0 to well below FB_H. Skips whichever
 * rows fall outside [clip_top, clip_bot) (and the framebuffer itself, for
 * anything calling this with unchecked bounds) instead of writing past the
 * buffer. */
static void blit_art_clip(uint16_t *fb, int x, int y, int clip_top, int clip_bot) {
    pthread_mutex_lock(&art_lock);
    if (art_bits) {
        for (int r = 0; r < ART_PX; r++) {
            int py = y + r;
            if (py < clip_top || py >= clip_bot || py < 0 || py >= FB_H) continue;
            memcpy(fb + (size_t)py * FB_W + x,
                   art_bits + (size_t)r * ART_PX,
                   (size_t)ART_PX * sizeof(uint16_t));
        }
    }
    pthread_mutex_unlock(&art_lock);
}

/* Nearest-neighbour downsample of the same ART_PX*ART_PX bitmap, for the
 * mini player's thumbnail -- there's no reason to decode/cache a second,
 * smaller copy just for a 56px square when the full one is already resident. */
static void blit_art_scaled(uint16_t *fb, int x, int y, int size) {
    pthread_mutex_lock(&art_lock);
    if (art_bits) {
        for (int r = 0; r < size; r++) {
            const uint16_t *src = art_bits + (size_t)(r * ART_PX / size) * ART_PX;
            uint16_t *dst = fb + (size_t)(y + r) * FB_W + x;
            for (int c = 0; c < size; c++)
                dst[c] = src[c * ART_PX / size];
        }
    }
    pthread_mutex_unlock(&art_lock);
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FB_W) w = FB_W - x;
    if (y + h > FB_H) h = FB_H - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint16_t *p = fb + (size_t)(y + r) * FB_W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

/* The transport was first drawn with text glyphs — "|<", "||", ">|" — which
 * read as a placeholder because the font's bar and bracket do not line up or
 * match weight. These are scanline-filled shapes instead, so the buttons are
 * actually symmetric. */
static void fill_circle(uint16_t *fb, int cx, int cy, int r, uint16_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy;
        if (yy < 0 || yy >= FB_H) continue;
        int dx = (int)(sqrt((double)(r * r - dy * dy)) + 0.5);
        fill_rect(fb, cx - dx, yy, dx * 2, 1, c);
    }
}

/* A track or a toggle's background, capped with a half circle at each end
 * instead of a square corner -- the stadium/pill shape sliders and switches
 * actually use. h is expected even; the caps are h/2 each. */
static void fill_pill(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    int r = h / 2;
    if (w <= h) { fill_circle(fb, x + w / 2, y + r, r, c); return; }
    fill_rect(fb, x + r, y, w - 2 * r, h, c);
    fill_circle(fb, x + r, y + r, r, c);
    fill_circle(fb, x + w - r, y + r, r, c);
}

/* A rect with a small corner radius, distinct from fill_pill's full-height
 * stadium caps -- a squat shape like the battery glyph wants a subtle round,
 * not two semicircles eating most of its width. */
static void fill_round_rect(uint16_t *fb, int x, int y, int w, int h, int r, uint16_t c) {
    if (r > h / 2) r = h / 2;
    if (2 * r > w) r = w / 2;
    if (r <= 0) { fill_rect(fb, x, y, w, h, c); return; }
    fill_rect(fb, x, y + r, w, h - 2 * r, c);        /* full-width middle band */
    for (int i = 0; i < r; i++) {
        int dx = (int)(sqrt((double)(r * r - (r - i) * (r - i))) + 0.5);
        int inset = r - dx;
        fill_rect(fb, x + inset, y + i, w - 2 * inset, 1, c);           /* top */
        fill_rect(fb, x + inset, y + h - 1 - i, w - 2 * inset, 1, c);   /* bottom */
    }
}

/* Bresenham, for the glyphs that are all diagonals. */
static void draw_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        fill_rect(fb, x0, y0, 2, 2, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Centers the switch on an explicit block height rather than always ROW_H --
 * some rows (Settings' Power button lock / USB Transport Mode) draw a
 * two-line description below the title, and the divider bounding "this
 * setting" spans that whole block, not just the top 72px title slice. The
 * old fixed y+16 also put the pill's midpoint at y+32 against a plain row's
 * actual center at y+36, a 4px-high misalignment even ignoring the
 * description issue (BG63). */
static void draw_toggle_switch_h(uint16_t *fb, int y, int on, int block_h) {
    int w = 68, h = 32, x = FB_W - 24 - w, top = y + block_h / 2 - h / 2;
    fill_pill(fb, x, top, w, h, on ? COL_ACCENT : COL_LINE);
    fill_circle(fb, on ? x + w - h / 2 : x + h / 2, top + h / 2, h / 2 - 3,
                on ? COL_BG : COL_DIM);
}

static void draw_toggle_switch(uint16_t *fb, int y, int on) {
    draw_toggle_switch_h(fb, y, on, ROW_H);
}

/* dir = +1 points right, -1 points left. */
static void fill_triangle(uint16_t *fb, int cx, int cy, int h, int dir, uint16_t c) {
    int half = h / 2;
    int w = (h * 87) / 100;                 /* equilateral-ish, not a wedge */
    for (int dy = -half; dy <= half; dy++) {
        int span = w - (w * (dy < 0 ? -dy : dy)) / half;
        if (span <= 0) continue;
        int x = (dir > 0) ? cx - w / 2 : cx + w / 2 - span;
        fill_rect(fb, x, cy + dy, span, 1, c);
    }
}

/* Arbitrary triangle, for the skip-arc arrowhead below -- fill_triangle()
 * above only does the two axis-aligned cases the transport row needed until
 * now. Standard edge-function fill: a point is inside when it is on the
 * same side of all three edges. One fill_rect() per pixel is wasteful, but
 * this draws an arrowhead a few hundred pixels across, at most a few times a
 * frame. */
static void fill_tri3(uint16_t *fb, int x0, int y0, int x1, int y1,
                      int x2, int y2, uint16_t c) {
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int w0 = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0);
            int w1 = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1);
            int w2 = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                fill_rect(fb, x, y, 1, 1, c);
        }
    }
}

static void draw_text(uint16_t *fb, int x, int y, const char *s,
                      uint16_t c, int px, int right_edge) {
    text_draw(fb, FB_W, FB_H, 0, x, y, s, c, px, right_edge ? right_edge : FB_W);
}

/* For a row that may be scrolled partway off the top or bottom of a list —
 * clipped pixel-row by pixel-row rather than skipped whole, so a row half
 * revealed by a drag shows its visible half instead of flickering in and out
 * at the row boundary. */
static void draw_text_clip(uint16_t *fb, int x, int y, const char *s,
                           uint16_t c, int px, int right_edge,
                           int clip_top, int clip_bot) {
    text_draw(fb, FB_W, clip_bot, clip_top, x, y, s, c, px, right_edge ? right_edge : FB_W);
}

/* lib_format_name()'s codes come from the stock scanner's own numeric
 * `format` column, read off real device data one value at a time as new
 * formats turned up. Checked a real AIFF file's row directly (BG25): the
 * scanner gives it format=1, the exact code already mapped to "WAV" -- it
 * is not misdetecting the file, it just has one code for "linear PCM"
 * that does not distinguish the RIFF/WAVE container from AIFF's. So an
 * unrecognized code is not the only way this needs a fix: code 1 also needs
 * disambiguating by the one thing the DB row doesn't carry, the container
 * the bytes are actually in. The path's extension answers that directly, the
 * same idea as the audiobook screen already uses for chapters, which have no
 * DB row to read a code from at all. */
static const char *track_format_name(const lib_track_t *t) {
    const char *name = lib_format_name(t->format);
    const char *dot = strrchr(t->path, '.');
    if (!strcmp(name, "WAV") && dot &&
        (!strcasecmp(dot, ".aif") || !strcasecmp(dot, ".aiff")))
        return "AIFF";
    if (strcmp(name, "?") != 0) return name;
    static char ext[16];
    snprintf(ext, sizeof(ext), "%s", dot && dot[1] ? dot + 1 : "?");
    for (char *p = ext; *p; p++) *p = (char)toupper((unsigned char)*p);
    return ext;
}

static void fill_rect_clip(uint16_t *fb, int x, int y, int w, int h, uint16_t c,
                           int clip_top, int clip_bot) {
    if (y < clip_top)  { h -= clip_top - y; y = clip_top; }
    if (y + h > clip_bot) h = clip_bot - y;
    if (h <= 0) return;
    fill_rect(fb, x, y, w, h, c);
}

/* BG69: fill_circle()/fill_pill() only ever clipped against the screen
 * edges, not an arbitrary clip_top -- fine everywhere they'd been used
 * until Settings started scrolling, where a toggle mid-row can have its
 * top half still above CONTENT_Y while its centre (where these primitives
 * actually draw from) is well below it. Skipping the whole draw call
 * whenever the row was *at all* out of bounds was the first attempt at
 * this; it missed exactly this straddling case, since the toggle's own
 * drawn pixels don't start at the row's nominal top -- they're centred
 * within it. Row-height gating can't get that right without duplicating
 * this same per-scanline math, so the primitives clip properly instead. */
static void fill_circle_clip(uint16_t *fb, int cx, int cy, int r, uint16_t c,
                             int clip_top, int clip_bot) {
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy;
        if (yy < clip_top || yy >= clip_bot || yy < 0 || yy >= FB_H) continue;
        int dx = (int)(sqrt((double)(r * r - dy * dy)) + 0.5);
        fill_rect(fb, cx - dx, yy, dx * 2, 1, c);
    }
}

static void fill_pill_clip(uint16_t *fb, int x, int y, int w, int h, uint16_t c,
                          int clip_top, int clip_bot) {
    int r = h / 2;
    if (w <= h) { fill_circle_clip(fb, x + w / 2, y + r, r, c, clip_top, clip_bot); return; }
    fill_rect_clip(fb, x + r, y, w - 2 * r, h, c, clip_top, clip_bot);
    fill_circle_clip(fb, x + r, y + r, r, c, clip_top, clip_bot);
    fill_circle_clip(fb, x + w - r, y + r, r, c, clip_top, clip_bot);
}

static void draw_toggle_switch_h_clip(uint16_t *fb, int y, int on, int block_h,
                                      int clip_top, int clip_bot) {
    int w = 68, h = 32, x = FB_W - 24 - w, top = y + block_h / 2 - h / 2;
    fill_pill_clip(fb, x, top, w, h, on ? COL_ACCENT : COL_LINE, clip_top, clip_bot);
    fill_circle_clip(fb, on ? x + w - h / 2 : x + h / 2, top + h / 2, h / 2 - 3,
                     on ? COL_BG : COL_DIM, clip_top, clip_bot);
}

/* ---- screens ------------------------------------------------------------- */
/* Rows are fetched a screenful at a time rather than held in one array: the
 * library is 4722 tracks and this device has 56 MB. Only what is on screen,
 * plus a little either side, is ever in memory. */
#define PAGE_MAX 32

typedef enum { SC_MENU = 0, SC_MUSIC_MENU, SC_ARTISTS, SC_ALBUMS, SC_TRACKS, SC_PLAYING,
               SC_RADIO, SC_PLAYLISTS, SC_AUDIOBOOKS, SC_PODCASTS, SC_POD_SYNC,
               SC_EQ, SC_EQ_BANDS, SC_EQ_BAND, SC_MSEB,
               SC_SETTINGS, SC_SETTINGS_THEME, SC_SETTINGS_ABOUT,
               SC_SETTINGS_TIMEZONE, SC_SETTINGS_THEMEMODE, SC_QUEUE,
               SC_ARTIST_PAGE,
               SC_SETTINGS_WIFI, SC_SETTINGS_BT, SC_SETTINGS_USB, SC_KEYBOARD } screen_t;

/* L2: the top-level menu ("Main Menu", EXIT on the right) stays small on
 * purpose rather than listing every library-browsing facet alongside
 * Audiobooks/EQ/Radio/Settings -- those facets live one level down, behind
 * "Music" (see menu[] below). */
static const struct { const char *label; } top_menu[] = {
    { "Music" },
    { "Audiobooks" },
    { "Podcasts" },
    { "Parametric EQ" },
    { "MSEB" },
    { "Radio" },
    { "Settings" },
};
#define TOP_MUSIC      0
#define TOP_AUDIOBOOKS 1
#define TOP_PODCASTS   2
#define TOP_EQ         3
#define TOP_MSEB       4
#define TOP_RADIO      5
#define TOP_SETTINGS   6
#define TOP_N ((int)(sizeof(top_menu) / sizeof(top_menu[0])))

/* Reached via "Music" from the main menu (SC_MUSIC_MENU). Each entry is a
 * column to group by, except Albums which skips the grouping step and
 * lists the lot, and Playlists which is not a library query at all. */
static const struct { const char *label; const char *column; } menu[] = {
    { "Album artists",       "album_artist" },
    { "Albums",              NULL           },
    { "Recently added",      NULL           },
    { "Recently heard",      NULL           },
    { "Artists",             "artist"       },
    { "Genres",              "genre"        },
    { "Playlists",           NULL           },
};
#define MENU_RECENT_ADDED 2
#define MENU_RECENT_HEARD 3
#define MENU_PLAYLISTS    6
#define RECENT_ALBUMS_N 10   /* R30: "the last 10 albums", not PAGE_MAX's 32 */
#define MENU_N ((int)(sizeof(menu) / sizeof(menu[0])))

static const char *cur_facet;      /* column being grouped by, NULL for Albums */
static const char *cur_facet_label;
/* 0 normal, 1 Recently added, 2 Recently heard -- set only by those two menu
 * entries, cleared by every other facet pick. See load_page()/
 * index_visible()/the SC_ALBUMS header title for what it changes. R30:
 * SC_ALBUMS's screen and row-tap-to-tracks code are reused as-is for both
 * rather than a new screen, since a recent-albums list is, structurally,
 * just an album list with an unusual sort and no A-Z index -- everything
 * rows[]/row_at()/play_from_list() already do for a normal album row
 * applies unchanged. */
static int         recent_mode;
#define RECENT_ADDED 1
#define RECENT_HEARD 2

/* Held-track actions, drawn over whatever list is showing rather than as a
 * screen of their own: it is a decision about one row, and pushing a screen
 * would lose the reader's place in a long list. */
static int live_x, live_y;          /* where the finger is now, while it is down */

/* Dragging along the progress bar. The seek itself already worked — a drag
 * ends as a tap at the release point — but with nothing moving under your
 * finger there was no way to aim, which is the whole point of scrubbing. */
/* Quick settings, pulled down from the status strip. Brightness, Wi-Fi and
 * Bluetooth are wanted often enough that leaving the app to reach them is the
 * annoyance; everything else stays in the firmware's own settings. */
#define QS_H       634   /* +QS_ROW_H over 562, for the new format/quality row (R83) */
#define QS_ROW_H   72
/* Row label column. Was a bare 68 until the Wi-Fi/Bluetooth/EQ row icons grew
 * larger -- Wi-Fi's natural width at its new height puts its right edge
 * exactly at 68, no gap at all, so the label column moved out to clear it.
 * +10 past that on top, on request, for a bit more breathing room still. */
#define QS_LABEL_X 86
#define QS_PULL    40           /* how far down before it counts as a pull */
/* BG16: the start zone used to be STATUS_H (32px), and logging real presses
 * showed every miss landing at down_y 33-67 against every hit at 25-31 --
 * the status row itself is just too thin a target to reliably start a
 * swipe from. Widened well past the worst observed miss, but capped where
 * the volume HUD's own drag zone begins (STATUS_H..STATUS_H+VOL_H) so the
 * two don't fight over the same touch when the HUD is up. */
#define QS_PULL_ZONE 80

static int qs_open;
static int qs_bright, qs_bright_max = 101, qs_wifi, qs_bt;
static int qs_dragging;         /* on the brightness bar */

static void qs_refresh(void) {
    qs_bright = st_brightness();
    qs_bright_max = st_brightness_max();
    qs_wifi = st_wifi_on();
    qs_bt = st_bt_on();
}

/* Input devices, rescanned so a headset connecting mid-session is picked up. */
#define KFD_MAX 8
static int  kfd[KFD_MAX];
static int  kfd_src[KFD_MAX];         /* KEYS_BUTTONS or KEYS_REMOTE */
static char kfd_name[KFD_MAX][32];
static int  kfd_n;

/* Volume overlay. Pressing a volume key throws it up; it can also be dragged,
 * which matters because the Bluetooth mixer is a 0-127 scale and stepping it
 * in percent lands between its notches — hence readings like 13%, 19%, 26%. */
#define VOL_H       96
#define VOL_TICKS   90        /* ~3 s at the loop's cadence */
static int vol_ticks, vol_dragging;

/* R60: a hardware seek has no on-screen tap to show it happened -- the user
 * may not even be looking at the screen when they press it, which is the
 * whole point of a hardware button, so there is nothing else that tells them
 * the press landed or how far it actually skipped. Same countdown-in-ticks
 * pattern as vol_ticks/VOL_TICKS just above, shorter-lived since this is a
 * glance-and-gone confirmation, not a slider the user is actively watching. */
#define SEEK_TOAST_TICKS 45   /* ~1.5 s at the loop's cadence */
static int  seek_toast_ticks;
static char seek_toast_text[24];

static void seek_toast(int delta_ms) {
    snprintf(seek_toast_text, sizeof(seek_toast_text), "%+d s", delta_ms / 1000);
    seek_toast_ticks = SEEK_TOAST_TICKS;
}
static int vol_drag_pct = -1, vol_applied = -1, vol_apply_tick;

static int scrub_active;
/* Running a finger down the A-Z strip. Like scrubbing, this has to be tracked
 * while the finger is down: waiting for the release gives one jump at the end
 * instead of the list moving under your thumb. */
static int index_active;
static int index_shown = -1;        /* letter slot last jumped to */

/* Where the bar sits, needed by both the drawing and the hit test.
 *
 * These used to carry a `title_extra` term, because a title that wrapped to a
 * second line pushed everything below it down and the bar's position was
 * therefore whatever the last frame happened to draw. Titles are one line now
 * (see draw_scroll_title), so the layout below them is fixed again and the
 * hit tests can be read straight off these constants. */
static int title_y(void) { return ART_PX + 20; }

/* R46: the album-detail screen's own header block -- cover, title, artist,
 * then the tracks/format/duration line, all scrolling together with the
 * track list beneath them rather than sitting fixed above it. Same spacing
 * Now Playing already uses for its own title/artist block (title_y()+44,
 * +82), so the two screens read consistently even though this one scrolls
 * and Now Playing doesn't. */
/* R63: once view_art_gone() confirms no cover is ever coming for this album
 * (no local file, and either no network to try Last.fm/Spotify with or the
 * fetch already came back empty), reserving ART_PX (a full screen width) of
 * blank COL_ROW box above the title was just dead space the user had to
 * scroll past to reach the tracklist -- collapse to a small top margin
 * instead, the same 24px left/top rhythm the rest of this screen already
 * uses. Every other measurement in this header (artist_y, info_y, hdr_h,
 * tracks_max_px()'s scroll bound, the cover draw itself below) derives from
 * this one function, so nothing else needs its own check. */
#define NO_ART_TOP_PAD 24
static int tracks_hdr_title_y(void)  { return g_view_art_gone_frame ? NO_ART_TOP_PAD : ART_PX + 20; }
static int tracks_hdr_artist_y(void) { return tracks_hdr_title_y() + 44; }
static int tracks_hdr_info_y(void)   { return tracks_hdr_artist_y() + 38; }
static int tracks_hdr_h(void)        { return tracks_hdr_info_y() + 40; }

/* Artist page: same layout rhythm as the album page's own header just
 * above -- photo, then name, then a summary line -- one less tier (no
 * "artist" line under the title, since this page *is* that artist). */
static int artist_page_title_y(void) { return ART_PX + 20; }
static int artist_page_info_y(void)  { return artist_page_title_y() + 44; }
static int artist_page_hdr_h(void)   { return artist_page_info_y() + 40; }

/* Word-wrapped bio text -- same wrap loop pod_draw_notes() below uses for
 * podcast show notes (see its own comment for why this isn't cached), but
 * split from the drawing itself: the artist page needs the *total* wrapped
 * height up front to compute how far the whole page can scroll, the way
 * tracks_max_px() needs track_n up front, which pod_draw_notes() never
 * needed since its own box scrolls independently rather than folding into
 * one page's overall content height.
 *
 * fb == NULL measures only (returns the wrapped height in px, draws
 * nothing, ignores off/clip_top/clip_bot); otherwise draws each line at
 * ty - off, clipped to [clip_top, clip_bot), and returns the same height
 * either way, so a caller can measure and draw with the same call shape. */
static int artist_bio_layout(uint16_t *fb, int x, int y, int w, int off,
                             int clip_top, int clip_bot) {
    const int lh = 32, px = TEXT_PX_SMALL;
    int ty = y;
    const char *p = artist_bio_text;
    if (!p[0]) return 0;
    while (*p) {
        while (*p == ' ') p++;
        if (*p == '\n') { p++; ty += lh; continue; }
        char line[256];
        line[0] = '\0';
        for (;;) {
            const char *ws = p;
            while (*p && *p != ' ' && *p != '\n') p++;
            int wlen = (int)(p - ws);
            if (wlen == 0) break;
            char probe[300];
            if (line[0]) snprintf(probe, sizeof(probe), "%s %.*s", line, wlen, ws);
            else         snprintf(probe, sizeof(probe), "%.*s", wlen, ws);
            if (line[0] && text_width(probe, px) > w) { p = ws; break; }
            snprintf(line, sizeof(line), "%s", probe);
            if (*p == '\n') { p++; break; }
            while (*p == ' ') p++;
            if (!*p) break;
        }
        if (line[0] && fb) {
            int draw_y = ty - off;
            /* Reported live: line ends were clipped off. draw_text_clip()'s
             * own right_edge param (confirmed by reading text_draw() --
             * it computes available width as right_edge - x internally) is
             * an absolute screen-space x-coordinate, not a width the way
             * every wrap decision above measures against -- w alone here
             * was ~x px short of where the wrap loop actually allowed text
             * to reach, silently truncating the last word or two of every
             * line. x + w is the boundary that actually matches what was
             * wrapped for. */
            if (draw_y + lh > clip_top && draw_y < clip_bot)
                draw_text_clip(fb, x, draw_y, line, COL_TEXT, px, x + w, clip_top, clip_bot);
            if (draw_y > clip_bot + 400) break;   /* far enough past the visible window */
        }
        ty += lh;
    }
    return ty - y;
}

/* Growing the art to edge-to-edge (ART_PX == FB_W, up from 384) ate more
 * vertical space than removing the status strip freed -- 96px added against
 * only 48px freed, which is what made a second title line unaffordable.
 *
 * BG40: this used to be its own formula (title_y() + 104), fourteen pixels
 * above what the main draw path actually put the bar at (title_y() + 118) --
 * both the tap zone here and draw_scrub_strip()'s lightweight redraw during
 * an active drag were quietly eleven-then-three pixels off from where a
 * non-scrubbing frame draws it, which is a real bug (a bar that jumps on the
 * first scrub touch, tap zones biased high), just masked by generous
 * tolerance bands. Now the one shared value everyone -- the main draw path
 * included -- calls, so it cannot drift again. */
static int bar_y(void) { return title_y() + 118; }

/* Same layout formula as bar_y(), two bars instead of one: book (display-only,
 * thin) above chapter (scrubbable, the same weight and hit-test band the
 * regular player's one bar already has). Shared by drawing and the tap test
 * below, exactly as bar_y() is shared for the regular player. */
/* +20 on both, keeping the 40px between them, because the "Book" caption did
 * not clear the line above it. The captions hang 26px above their own bar, so
 * "Book" sat at 560 while the album line at 544 is TEXT_PX_BODY (30) tall and
 * runs to 574 -- straight through it. Only "Book" was affected: "Chapter" at
 * 600 already cleared the book bar at 586. Both move together because shifting
 * the book bar alone would push "Chapter" into it instead, trading one
 * collision for another. At +20 the transport centre lands at 704 and its
 * 42px circle ends at 746, still clear of the footer row at 766. */
static int ab_book_bar_y(void)    { return ART_PX + 20 + 106; }
static int ab_chapter_bar_y(void) { return ART_PX + 20 + 146; }

/* Long titles scroll sideways under the finger instead of wrapping.
 *
 * title_off is what is committed between drags; title_off_live is what the
 * current frame draws, which differs only while a finger is actually on the
 * title. Both are <= 0: 0 is the start of the string, and dragging left goes
 * negative. title_span is how far there is to go, published by the draw code
 * because only it knows the rendered width of the string it just measured. */
static int title_off, title_off_live, title_span, title_dragging;

static void title_reset(void) { title_off = title_off_live = title_span = 0; }

/* SC_EQ_BAND's layout, shared between drawing and the tap/drag hit tests
 * below -- one source of numbers so the two cannot drift apart. */
static int eq_enabled_y(void) { return CONTENT_Y + 20; }
static int eq_type_y(void)    { return eq_enabled_y() + 56 + 26; }
static int eq_freq_y(void)    { return eq_type_y() + 34 + 30 + 24; }
static int eq_gain_y(void)    { return eq_freq_y() + 46; }
static int eq_q_y(void)       { return eq_gain_y() + 46; }

/* SC_EQ overview's rows, same reason. */
static int eq_row_enabled_y(void) { return CONTENT_Y; }
static int eq_row_profile_y(void) { return eq_row_enabled_y() + ROW_H; }
static int eq_preamp_y(void)      { return eq_row_profile_y() + ROW_H; }
static int eq_curve_y(void)       { return eq_preamp_y() + 60; }
static int eq_row_bands_y(void)   { return eq_curve_y() + 66; }

#define MSEB_ROW_H 70
static int mseb_row_enabled_y(void) { return CONTENT_Y; }
static int mseb_band_row_y(int i) { return mseb_row_enabled_y() + ROW_H + i * MSEB_ROW_H; }

/* R72: kept as the reserved right-margin boundary mseb_reset_x()/pod_sync_x()/
 * queue_clear_x() below still measure themselves against, even though "BACK"
 * itself moved to a leading arrow next to the title and nothing is actually
 * drawn at this position any more -- repositioning those three as well was
 * not asked for, so they keep the exact layout they already had. */
static int header_back_x(void) {
    return FB_W - 24 - text_width("BACK", TEXT_PX_SMALL);
}

/* R72: back moved from top-right "BACK" text to a leading accent arrow next
 * to the title -- the convention this app's transport screens already use
 * for "this is a direction" (BG97's rotate-arrow skip icons), just a plain
 * chevron rather than a rotate-and-seek glyph. g_header_show_back is set
 * once per drawn frame, in draw_screen()'s own title/header block, and read
 * from here by the tap handler -- same "compute once, every reader this
 * frame sees exactly what's on screen" shape g_view_art_gone_frame already
 * uses, rather than duplicating draw_screen()'s whole per-screen title/
 * show_back if-chain a second time just to answer "is there a back arrow
 * right now". */
static int  g_header_show_back;
#define BACK_ARROW_X  18
#define BACK_ARROW_W  16
#define BACK_ARROW_H  22
#define BACK_ARROW_TAP_W 120   /* generous zone, not a tight box -- same
                                 * reasoning the mini player's own zones use */

static void draw_back_arrow(uint16_t *fb, int x, int cy, uint16_t c) {
    fill_tri3(fb, x, cy, x + BACK_ARROW_W, cy - BACK_ARROW_H / 2,
              x + BACK_ARROW_W, cy + BACK_ARROW_H / 2, c);
}

/* Left edge of the "Reset" header action, shared between the header's own
 * draw and its tap zone so they cannot drift apart -- the same reason
 * bar_y()/eq_row_*_y() above are functions rather than repeated literals.
 * Positioned left of "BACK" with a 20px gap, both right-aligned inward from
 * the usual 24px margin. */
static int mseb_reset_x(void) {
    int reset_w = text_width("Reset", TEXT_PX_SMALL);
    return header_back_x() - 20 - reset_w;
}

/* Same idea as mseb_reset_x(), for SC_PODCASTS's "Sync" header action. */
static int pod_sync_x(void) {
    int sync_w = text_width("Sync", TEXT_PX_SMALL);
    return header_back_x() - 20 - sync_w;
}

/* R52: same idea again, for SC_QUEUE's "Clear" header action. */
static int queue_clear_x(void) {
    int clear_w = text_width("Clear", TEXT_PX_SMALL);
    /* Reported live: wanted further from BACK than mseb_reset_x()/
     * pod_sync_x()'s shared 20px -- unlike Reset or Sync, this one actually
     * discards something (the rest of the queue), so a bit more separation
     * from the tap zone right next to it is worth the extra width. */
    return header_back_x() - 40 - clear_w;
}

/* R82: same idea again, for the USB Storage Mode banner's own "Done"
 * action -- not a header row (the banner isn't one), so right-aligned
 * against the screen's own 24px margin rather than header_back_x(). */
static int usb_done_x(void) {
    return FB_W - 24 - text_width("Done", TEXT_PX_BODY);
}

/* Mass storage exports the card's block device.  It cannot coexist with
 * even one local reader or writer without risking exFAT corruption. */
static void set_usb_storage_mode(int mode) {
    if (mode == 1) {
        audio_stop();
        pod_cancel_io();
        /* These can read cached artwork or write a fetched cover on the SD
         * card.  Their work is intentionally joined, not abandoned, before
         * the filesystem is given to the host. */
        if (art_thread_valid) {
            pthread_join(art_thread, NULL);
            art_thread_valid = 0;
        }
        if (view_art_thread_valid) {
            pthread_join(view_art_thread, NULL);
            view_art_thread_valid = 0;
        }
        if (artist_art_thread_valid) {
            pthread_join(artist_art_thread, NULL);
            artist_art_thread_valid = 0;
        }
        scanner_pause_for_usb(1);
        index_pause_for_usb(1);
        while (scanner_scan_running() || index_scan_running()) usleep(10000);
    } else {
        scanner_pause_for_usb(0);
        index_pause_for_usb(0);
    }
    st_usb_mode_set(mode);
}

/* BG47 (revised): just two skip arcs now, -10s left of play/pause and +30s
 * right of it -- not the symmetric +/-10/+/-30 four-button set this
 * started as. 96, not the original 70: matches the audiobook screen's own
 * skip-arc offset exactly, per explicit request to space these out the
 * same way that screen does. Offset shared between the draw code and the
 * tap handler, same reason bar_y() is a function rather than a repeated
 * literal. */
#define POD_SKIP_OFF 96
/* Centre x of the podcast speed ring -- mid - 96 - 70, exactly the
 * audiobook screen's own scx formula (see its comment: "96 must match the
 * skip rings' offset above"), not just the same gap. */
static int pod_speed_x(int mid) { return mid - POD_SKIP_OFF - 70; }
/* Centre x of the show-notes info icon -- mirrors pod_speed_x() on the
 * other side of the transport row, same gap past the +30s arc that the
 * speed ring leaves before the -10s one. Moved here from the top-right
 * corner of the cover art per explicit request. */
static int pod_info_x(int mid) { return mid + POD_SKIP_OFF + 70; }

/* How long the device may sit locked with nothing playing before it goes into
 * its low-power idle. Minutes, and 0 means never. Declared up here with the
 * layout rather than down with the rest of the config, because the settings
 * screen draws the current value well before load_conf() is defined.
 *
 * What "low-power idle" means deliberately stops short of suspend-to-RAM for
 * now: writing `mem` to /sys/power/state on this device hangs it partway
 * through the sequence -- the panel blanks, the backlight stays lit, and
 * nothing brings it back, not the power button and not an RTC alarm armed and
 * verified beforehand. The stock player reaches suspend through its own
 * teardown first (it ships a separate /usr/bin/bt_suspend, has an early-suspend
 * phase, and carries the string "fb has already suspended"), and until that
 * order is worked out, a setting that can wedge the device is not one worth
 * shipping. What this timeout drives instead is releasing the parts that
 * actually draw current -- see deep_suspend(). */
static const int SLEEP_CHOICES[] = { 0, 1, 2, 5, 10, 30 };
#define SLEEP_CHOICE_N ((int)(sizeof(SLEEP_CHOICES) / sizeof(SLEEP_CHOICES[0])))
static int sleep_idx = 2;                       /* index into SLEEP_CHOICES */
static int sleep_minutes(void) { return SLEEP_CHOICES[sleep_idx]; }

/* Whether that same timeout also suspends the SoC (see deep_suspend()).
 * Declared here with the rest of the persisted settings because load_conf()
 * reads it long before the suspend code itself is defined. */
static int deep_sleep_enabled;

/* A full poweroff after sitting locked with nothing playing, independent of
 * the suspend timer above and not sharing its gate. This is the direct
 * answer to what suspend turned out to cost: an unattended overnight test
 * with deep sleep on needed a hard power cycle and still measured no real
 * improvement, and the working reference implementation's own suspend
 * sequence (recovered from open_hiby_player's binary) puts the SDIO bus that
 * Wi-Fi and the SD card share through a full teardown and rebuild on every
 * cycle -- fine once, risky dozens of times unattended, and that
 * investigation is paused pending its source. A power-off draws whatever the
 * PMIC's own standby leakage is, which is not a figure this codebase can
 * improve on, and costs a real cold boot to get back rather than a
 * suspend's sub-second wake -- a trade explicitly chosen over suspend's
 * fragility for a "left alone for hours" scenario. Off by default, same as
 * deep sleep: a feature that ends the process is not one to default on
 * before it has been run for real. */
static const int AUTO_OFF_CHOICES[] = { 0, 5, 15, 30, 60, 120 };
#define AUTO_OFF_CHOICE_N ((int)(sizeof(AUTO_OFF_CHOICES) / sizeof(AUTO_OFF_CHOICES[0])))
static int auto_off_idx;                        /* index into AUTO_OFF_CHOICES */
static int auto_off_minutes(void) { return AUTO_OFF_CHOICES[auto_off_idx]; }

/* R90: how many equal steps a Bluetooth volume-key press moves across the
 * AVRCP mixer's own 0-127 range -- see audio_set_bt_vol_steps()'s own
 * comment in audio.h/audio.c for why this exists at all (in short: the app
 * used to step in percent and let amixer convert to/from raw on every
 * write and read, which is where the "down 5%, back up 1%" bug came from).
 * Index into BT_VOL_STEP_CHOICES, same cycle-on-tap shape as Auto shutdown
 * just below it. */
static const int BT_VOL_STEP_CHOICES[] = { 16, 32, 64, 128 };
#define BT_VOL_STEP_CHOICE_N ((int)(sizeof(BT_VOL_STEP_CHOICES) / sizeof(BT_VOL_STEP_CHOICES[0])))
static int bt_vol_step_idx = 1;   /* index into BT_VOL_STEP_CHOICES -- default 32 */

/* SC_SETTINGS's rows, same reason. */
/* The +40 below each description used to be the gap before the *next row's
 * title*, back when the divider sat above the description instead of below
 * it. It was never big enough to hold the description itself: text_draw's y
 * is the top of the line box, glyphs draw from y+ascent, and a descender on
 * the second line ("again", "playing") reaches past dy+40 -- so the divider,
 * now placed right after the description, cut through those descenders.
 * +64 gives the two-line block genuine clearance rather than reusing a
 * number sized for a different layout. */
static int set_row_lock_y(void)  { return CONTENT_Y; }
static int set_lock_desc_y(void) { return set_row_lock_y() + ROW_H; }
static int set_row_usbbypass_y(void)  { return set_lock_desc_y() + 64; }
static int set_usbbypass_desc_y(void) { return set_row_usbbypass_y() + ROW_H; }
/* "Idle sleep" used to sit here. Its row is gone from Settings: it drove
 * deep_suspend(), and that whole line of work is paused until there is source
 * for open_hiby_player to compare against -- an unattended overnight run with
 * it on ended in a hard power cycle and measured no improvement. The timeout
 * is still read from music.conf so an existing config still parses, but with
 * deep_sleep defaulting to 0 nothing reaches the suspend path, and there is no
 * longer a way to switch it on by accident from the UI. Auto shutdown is the
 * shipped answer to the same problem. */
/* R81: two-line description like the toggles above it, hence the +64 block
 * rather than a plain ROW_H row. */
static int set_row_btautoplay_y(void)  { return set_usbbypass_desc_y() + 64; }
static int set_btautoplay_desc_y(void) { return set_row_btautoplay_y() + ROW_H; }
/* R90: plain ROW_H row, no description -- "16/32/64/128" reads for itself
 * once picked, same as Theme/Timezone below. Grouped with the other
 * Bluetooth-specific row (Auto-play) above it rather than down with the
 * unrelated appearance rows. */
static int set_row_btvolsteps_y(void) { return set_btautoplay_desc_y() + ROW_H; }
static int set_row_autooff_y(void)  { return set_row_btvolsteps_y() + ROW_H; }
static int set_autooff_desc_y(void) { return set_row_autooff_y() + ROW_H; }
/* R41: no description under this one -- "Light theme" needs no explaining
 * the way the toggles above did, so it's a plain ROW_H row like Accent
 * colour and About below it, not another +64 two-line block. */
static int set_row_lighttheme_y(void) { return set_autooff_desc_y() + 64; }
/* R44: plain ROW_H row too -- the offset reads for itself once picked, same
 * as Theme above it. */
static int set_row_timezone_y(void) { return set_row_lighttheme_y() + ROW_H; }
static int set_row_theme_y(void) { return set_row_timezone_y() + ROW_H; }
/* R92: two-line description like Power button lock/USB Transport Mode
 * above -- "derived from the current cover" isn't self-explanatory from
 * the label alone the way a plain colour picker would be. */
static int set_row_coverpalette_y(void)  { return set_row_theme_y() + ROW_H; }
static int set_coverpalette_desc_y(void) { return set_row_coverpalette_y() + ROW_H; }
/* R26: About, one row below Accent colour -- which now needs its own
 * trailing divider back (it used to be the last row and closed the list
 * itself), and this row takes over closing the list instead. */
static int set_row_about_y(void) { return set_coverpalette_desc_y() + 64; }
/* Wi-Fi/Bluetooth: reachable here too, not just via holding their row in
 * quick settings -- that hold is fast once you know it's there, but
 * nothing on screen ever told a first-time user it existed. */
static int set_row_wifi_y(void) { return set_row_about_y() + ROW_H; }
static int set_row_bt_y(void)   { return set_row_wifi_y() + ROW_H; }
static int set_row_usb_y(void)  { return set_row_bt_y() + ROW_H; }
/* RP1/RP6: one button for both scans -- used to be two separate rows
 * ("Rebuild library index" alongside this one), dropped once RP1 shipped
 * (v0.32) made scanner.c the sole source of what files exist at all
 * (see library.c's lib_open() and its own comment on why). Two passes are
 * still run underneath (scanner_rescan_now() kicks index_rescan_now() too,
 * see its own comment in scanner.c) but a user tapping "check for new
 * music" has one question, not two, and only ever needs the one number. */
static int set_row_scan_y(void) { return set_row_usb_y() + ROW_H; }
/* R44: total content height, for the Settings screen's own scroll clamp --
 * ceil() so a last row that doesn't fill a whole ROW_H still gets fully
 * scrollable rather than clipped short. */
static int settings_content_rows(void) {
    int content_px = set_row_scan_y() + ROW_H - CONTENT_Y;
    return (content_px + ROW_H - 1) / ROW_H;
}

/* Bump this with every release -- it had been stuck at "0.1" since the very
 * first one, through 0.14, because nothing ever reminded anyone to touch it.
 * There is no build-time derivation from the git tag (this .so is built and
 * pushed by hand, not by CI against a tagged commit), so this stays a
 * literal that a human edits; the discipline is remembering to, not the
 * mechanism. */
#define LIBRARY_VERSION "0.51.2"

/* A custom-built kernel keeps uname()'s own release string exactly
 * "4.4.94+" on purpose -- that string is also the vermagic every one of the
 * 28 kernel modules is checked against on load, 9 of them closed-source
 * blobs this project can never recompile, so it can never change without
 * breaking module loading outright. /usr/resource/kernel_build_id is a
 * separate marker this project's own firmware patcher stamps (same
 * pattern as CONFIG_JSON's stamped version string, read a few lines down
 * from here) -- "4.4.94_r1", "_r2", ... -- read here in preference to the
 * real uname() when present, so About can show which of this project's
 * own kernel builds is actually running without touching the one string
 * that has to stay stock. */
#define KERNEL_BUILD_ID_PATH "/usr/resource/kernel_build_id"

static void about_kernel(char *out, size_t n) {
    FILE *f = fopen(KERNEL_BUILD_ID_PATH, "r");
    if (f) {
        if (fgets(out, (int)n, f)) {
            size_t len = strlen(out);
            while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
                out[--len] = '\0';
            fclose(f);
            if (out[0]) return;
        } else {
            fclose(f);
        }
    }
    struct utsname u;
    if (uname(&u) == 0) snprintf(out, n, "%s", u.release);
    else snprintf(out, n, "unknown");
}

/* config.json's own "version" key appears twice -- a bare integer for the
 * slef entry ahead of it ("version":1, no quotes) and a quoted string for
 * the product entry that actually matters here ("version":"1.6a"). The
 * quote right after the colon is what tells them apart, confirmed against
 * the file directly rather than assumed. patch_firmware.py stamps this
 * field itself (stamp_config_json()), so it already reads a
 * build-identifying value like "1.6a", not the vendor's plain "1.6". */
static void about_firmware(char *out, size_t n) {
    snprintf(out, n, "unknown");
    FILE *f = fopen("/usr/resource/config.json", "r");
    if (!f) return;
    char buf[4096];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    const char *p = strstr(buf, "\"version\":\"");
    if (!p) return;
    p += strlen("\"version\":\"");
    const char *end = strchr(p, '"');
    if (!end) return;
    size_t len = (size_t)(end - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

/* R26 follow-up. The efuse chip ID is the right source (confirmed live --
 * hiby_player's own strings name both this /proc path and getSerialNumber
 * together), but the raw 32-hex-digit ID is not itself the serial number:
 * the stock About screen's "Serial number" is "R1" + the first 8 hex digits
 * of the chip ID, uppercased. Confirmed by direct comparison on this unit --
 * chip ID "7b42423542a0c500afa0560804000001" against stock's own displayed
 * "R17B424235". First attempt showed the full raw ID; wrong, per the user. */
static void about_serial(char *out, size_t n) {
    snprintf(out, n, "unknown");
    FILE *f = fopen("/proc/jz/efuse/efuse_chip_id", "r");
    if (!f) return;
    char buf[128];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return; }
    fclose(f);
    const char *p = strstr(buf, "CHIP_ID:");
    if (p) p += strlen("CHIP_ID:"); else p = buf;
    while (*p == ' ') p++;
    if (strlen(p) < 8) return;
    char hex8[9];
    memcpy(hex8, p, 8);
    hex8[8] = '\0';
    for (int i = 0; i < 8; i++) hex8[i] = (char)toupper((unsigned char)hex8[i]);
    snprintf(out, n, "R1%s", hex8);
}

/* SD_ROOT matches library.c's own -- /data is a symlink to usr/data, so this
 * resolves to the same mount statfs sees at /usr/data/mnt/sd_0 either way. */
static void about_sd_free(char *out, size_t n) {
    struct statfs sf;
    if (statfs("/data/mnt/sd_0/", &sf) != 0 || sf.f_bsize <= 0) {
        snprintf(out, n, "unknown");
        return;
    }
    double gb = (double)sf.f_bavail * (double)sf.f_bsize / (1000.0 * 1000.0 * 1000.0);
    snprintf(out, n, "%.1f GB free", gb);
}

/* MemAvailable (not MemFree): what the kernel itself estimates could actually
 * be given to a new allocation without swapping, once reclaimable cache is
 * counted -- MemFree alone reads alarmingly low on this device even when
 * plenty is available, for exactly the reason BG44's own investigation ran
 * into (buff/cache regularly the largest share of a 56 MB budget). */
static void about_ram_free(char *out, size_t n) {
    snprintf(out, n, "unknown");
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        long kb;
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) {
            snprintf(out, n, "%.0f MB free", kb / 1024.0);
            break;
        }
    }
    fclose(f);
}

/* One line, dragged sideways rather than wrapped.
 *
 * This replaced a split_title() that broke a long name at its first colon or
 * " - " and drew two lines. That was a good fit while the artwork was 384px
 * and there was room underneath; with the cover running edge to edge there is
 * not, and a wrapped title pushed the transport controls into the footer.
 * Dragging also beats splitting on its own terms for this library: it shows
 * the *whole* string, including the tail a split still had to ellipse, and it
 * does not have to guess where the interesting half of a classical title
 * starts.
 *
 * Publishes title_span so the drag handler knows how far there is to go --
 * only the draw side knows the rendered width of the string. */
/* R92: fg/bg passed in rather than hardcoded -- the shared music/podcast
 * Now Playing screen wants np_col_fg()/np_col_bg() when cover colours are
 * active, but this same function also draws the audiobook screen's title,
 * which never themes off album art (see draw_screen()'s own comment on
 * why) and must keep the plain COL_TEXT/COL_BG regardless of whatever a
 * previous music session last computed. */
static void draw_scroll_title(uint16_t *fb, int y, const char *s, uint16_t fg, uint16_t bg) {
    int avail = FB_W - 48;
    int w = text_width(s, TEXT_PX_TITLE);
    title_span = w > avail ? w - avail : 0;
    if (title_off_live < -title_span) title_off_live = -title_span;
    if (title_off < -title_span)      title_off = -title_span;

    draw_text(fb, 24 + title_off_live, y, s, fg, TEXT_PX_TITLE, FB_W - 24);
    /* text_draw only clips at the framebuffer edge, so whatever has slid past
     * the left margin is painted back out here. Without this, letters sit in
     * the margin and read as a rendering fault rather than a scroll. */
    fill_rect(fb, 0, y - 2, 24, TEXT_PX_TITLE + 12, bg);
}

/* h:mm:ss past an hour, m:ss below it. Everything an audiobook shows needs
 * this: m:ss alone made an eleven-hour book "663:33" and a two-hour chapter
 * of Blue Mars "119:47". Tracks use it too — a Mahler symphony movement is
 * not far off the same problem. */
static void fmt_dur(char *out, size_t n, int64_t ms) {
    if (ms < 0) ms = 0;
    int s = (int)(ms / 1000);
    if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else           snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void fmt_left(char *out, size_t n, int64_t ms) {
    char t[32];
    fmt_dur(t, sizeof(t), ms);
    snprintf(out, n, "%s left", t);
}

static int scrub_ms(int dur) {
    int px = live_x - 24, span = FB_W - 48;
    if (px < 0) px = 0;
    if (px > span) px = span;
    return dur > 0 ? (int)((long long)dur * px / span) : 0;
}

/* R74: 4 = a playlist's own long-press menu (Rename/Delete/Cancel), 5 = the
 * Delete confirmation step -- both fixed 3-/2-row lists, same SHEET_N shape
 * sheet_open == 1 already uses, not the playlist_n-sized lists sheet_open
 * == 2/3 are. */
static int  sheet_open;                 /* 0 none, 1 actions, 2 pick a playlist, 3 pick an EQ profile, 4 playlist menu, 5 confirm delete */
static int  sheet_track;                /* index into tracks[] */
static int  sheet_playlist;             /* index into playlists[], for sheet_open == 4/5 */
static char sheet_note[64];             /* what the last action did */
static const char *const sheet_items[] = {
    "Play next", "Add to queue", "Add to playlist", "Cancel"
};
#define SHEET_N ((int)(sizeof(sheet_items) / sizeof(sheet_items[0])))
/* R74 */
static const char *const playlist_menu_items[] = { "Rename", "Delete", "Cancel" };
#define PLAYLIST_MENU_N ((int)(sizeof(playlist_menu_items) / sizeof(playlist_menu_items[0])))
static const char *const playlist_delete_items[] = { "Delete", "Cancel" };
#define PLAYLIST_DELETE_N ((int)(sizeof(playlist_delete_items) / sizeof(playlist_delete_items[0])))
#define SHEET_ROW 72

static pl_t playlists[PL_MAX];
static int  playlist_n;

static screen_t screen;
static int scroll;                      /* index of the first visible row */
static int scroll_px;                   /* 0..ROW_H-1: how far into that row */
static int total;                       /* rows in the current list */

/* BG37: SC_ARTISTS' own scroll position, saved the moment a row is tapped
 * (whichever facet -- Album artists, Artists, Genres, all share this same
 * screen and tap handler) and restored in go_back()'s SC_ALBUMS case, so
 * returning from an artist's albums lands back where you were rather than
 * at the top. One slot, not a stack: this only ever needs to undo the one
 * level SC_ARTISTS -> SC_ALBUMS goes, the same scope BG7's albums_artist
 * already covers for the artist filter itself. */
static int artists_scroll_saved, artists_scroll_px_saved;

/* BG37 follow-up: the same idea, one level down. SC_ALBUMS' own scroll
 * position was never saved at all -- BG37 only ever covered SC_ARTISTS ->
 * SC_ALBUMS, not SC_ALBUMS -> SC_TRACKS, so backing out of an album always
 * landed on top of the Albums list regardless. Saved the moment an album
 * row is tapped, restored in go_back()'s default case (the only path back
 * from SC_TRACKS to a real SC_ALBUMS browse -- the ab_list branch there is
 * Chapters, a different screen's data, and ALBUMS-with-no-facet is a
 * distinct forward entry that correctly wants the top). */
static int albums_scroll_saved, albums_scroll_px_saved;

/* Live drag + inertia state for list scrolling, set up alongside scroll_to_px
 * below (which needs vis_rows/load_page, not yet declared here). */
static int   list_dragging;
static int   list_down_y, list_start_px;
static int   list_last_y;
static unsigned list_last_tick;
static float  list_velocity;            /* px per tick, signed */
static int    inertia_active;
/* Ticks since inertia was last active -- resets only on inertia_active
 * itself, deliberately not on list_dragging too: list_dragging goes true
 * on *any* touch-down on a scrollable screen, ordinary stationary taps
 * included, so resetting on it as well (tried once) made an ordinary
 * album-row tap read as "still settling" and get swallowed on every
 * single tap, not just the scroll-then-tap case this exists for. Gates a
 * tap-driven selection below on having cleared a short threshold, since
 * inertia_active alone flips false the instant |list_velocity| decays
 * under its 0.6 px/tick arming threshold -- which can be the very same
 * tick a tap lands in, while the list is still visibly settling. */
static unsigned list_settled_ticks;

static int index_lock_end = -1; /* absolute row index the last index jump can't show past, -1 = none */

static lib_row_t rows[PAGE_MAX];
static int row_n;
static int row_base = -1;       /* absolute offset of rows[0], -1 when stale */
static void index_cache_reset(void);

static void reset_scroll(void) {
    scroll = 0; scroll_px = 0;
    list_dragging = 0; inertia_active = 0; list_velocity = 0;
    index_lock_end = -1;
    row_base = -1;
    row_n = 0;
    index_cache_reset();
}
/* BG111: was PAGE_MAX*8 (256) -- a real 515-chapter audiobook (one .m4b,
 * embedded chapter track) hit this exactly, silently dropping everything
 * past chapter 256 since ab_load_book()'s own copy loop stops at
 * sizeof(tracks)/sizeof(tracks[0]) same as any other overflow guard would.
 * Raised alongside AB_MAX_CHAPTERS (audiobook.h) to the same 1024 headroom;
 * queue[] and the two playlist paths[] scratch buffers below have to move
 * in lockstep (queue[] is memcpy()'d straight from tracks[] at whatever
 * track_n currently is, and both paths[] buffers read back into tracks[]
 * the same way -- any one of the four smaller than the others reintroduces
 * a silent truncation at whatever the smallest one's capacity is). */
static lib_track_t tracks[PAGE_MAX * 32];
static int track_n;

/* What is playing has to survive browsing away from it: open another album and
 * tracks[] is replaced, so a queue that was just an index into it would start
 * naming the wrong song in the mini player. The queue is therefore its own
 * copy, with the artist and album it came from. */
static radio_station_t stations[RADIO_MAX];
static int  station_n;
/* A stream has no queue, no length and no artwork, so Now Playing has to know
 * it is showing one rather than a track. */
static int  radio_mode;
static char radio_name[LIB_NAME_LEN];
static char radio_msg[64];        /* why the last station did not start */

/* Audiobooks. Chapters reuse tracks[]/queue[] exactly as an album's tracks
 * do -- ab_scan_chapters() fills lib_track_t the same shape sweep_album_
 * folder() does -- so play_from_list(), the mini player, hardware keys and
 * the queue-icon-into-SC_TRACKS mechanism all work unchanged. Only the Now
 * Playing screen itself needs new drawing and input handling, gated on this
 * flag exactly as SC_PLAYING already gates on radio_mode. */
/* Two separate facts, and conflating them meant browsing an album while a
 * book played drew the album with the audiobook's transport. `audiobook_mode`
 * is what is *playing*, and is set and cleared beside radio_mode wherever
 * playback starts; `ab_list` is whether tracks[] currently holds a book's
 * chapters, which is about where the browser is. */
static int  audiobook_mode;
static int  ab_list;
static ab_book_t ab_books[AB_MAX_BOOKS];
static lib_row_t ab_rows[AB_MAX_BOOKS];   /* ab_books[], reshaped for the row-list draw code */
static int  ab_book_n;
static int  ab_speed_permille = 1000;     /* persists across books; reset only by leaving the mode */
/* The open book. Chapters are also mirrored into tracks[]/queue[] so the
 * chapter list, the mini player and Now Playing's title all keep working
 * through the same code an album's tracks use; this carries the part a
 * lib_track_t has nowhere to put — where in its file each chapter starts. */
static ab_book_data_t ab_book;
static char ab_playing[LIB_PATH_LEN];     /* the file the decoder actually has open */

static int was_active;    /* to spot a track ending of its own accord */

/* Podcasts. Feeds are a flat list, drawn and tapped exactly like
 * SC_AUDIOBOOKS (pod_rows[]/row_at()); an episode list of one feed reuses
 * SC_TRACKS exactly like a book's chapters do (`pod_list` alongside
 * `ab_list`). Playback is deliberately NOT queued across a whole feed the
 * way an album's tracks are -- queue_n stays 1, one episode at a time --
 * since a feed mixes downloaded and not-yet-downloaded episodes and a
 * podcast queue isn't something a listener expects to play straight
 * through the way an album is. `podcast_mode` is what's *playing*, set and
 * cleared beside audiobook_mode/radio_mode; `pod_list` is whether tracks[]
 * currently holds one feed's episodes, same split as ab_list/audiobook_mode. */
#define POD_MAX_FEEDS 64
static pod_feed_t pod_feeds[POD_MAX_FEEDS];
static lib_row_t  pod_rows[POD_MAX_FEEDS];   /* pod_feeds[], reshaped for the row-list draw code */
static int        pod_feed_n;
static pod_episode_t pod_eps[POD_MAX_ITEMS]; /* parallel to tracks[] while pod_list is set */
static int        pod_ep_n;
static char       cur_feed[POD_NAME_LEN];
static int        podcast_mode;
static int        pod_list;
/* BG47: separate from ab_speed_permille on purpose -- persists across
 * episodes the same way ab_speed_permille persists across books, but a
 * podcast's chosen speed has no reason to leak into or be overwritten by
 * an audiobook's. */
static int        pod_speed_permille = 1000;
/* Show notes: raw text, wrapped and clipped at draw time. Loaded once, when
 * an episode starts, not re-read every frame. */
static char       pod_notes_text[8192];
static int        pod_notes_avail;
static int        pod_notes_showing;
static int        pod_notes_scroll_px;
static int        pod_notes_dragging;
static int        pod_notes_down_y, pod_notes_start_px;

/* SC_POD_SYNC: a dedicated screen for the whole-feed sync, per BACKLOG.md's
 * L5 entry ("the UPDATE FEEDS screen, which streams live podsync subprocess
 * output into its own screen") -- not folded into a toast or a status line
 * on SC_PODCASTS, since a multi-feed sync can run long enough that the
 * reader wants to see it's actually making progress, not just that it
 * started. Filled from pod_update_tail() every tick while running (see the
 * main loop), newest last, same as the fetcher's own log file reads. */
#define POD_SYNC_LOG_N 10
static char pod_sync_log[POD_SYNC_LOG_N][POD_NAME_LEN];
static int  pod_sync_log_n;

/* Parametric EQ. eq.c/eq.h own the live filter state applied in the audio
 * thread; this is the UI-side working copy, written to eq_set_band()/
 * eq_set_preamp() at the same call sites that update it locally, so the two
 * never need an explicit resync. eq_cur_path is empty when nothing has been
 * loaded yet -- the EQ then sits at its all-zero (silent no-op) default. */
static ep_entry_t  eq_profiles[EP_MAX_PROFILES];
static int         eq_profile_n;
static char        eq_cur_path[EP_PATH_LEN];
static eq_profile_t eq_cur;
static int         eq_editing_band;       /* index into eq_cur.band[] on SC_EQ_BAND */

/* ---- T9 on-screen keyboard -------------------------------------------
 * The only text-entry surface this app has ever needed -- WiFi SSIDs/
 * passphrases -- so a real multi-tap T9 keypad rather than a cramped
 * QWERTY grid: fewer, bigger touch targets on a 480px-wide screen, and it
 * matches how this device's physical remote/button navigation already
 * thinks in a numeric grid.
 *
 * Two modes, switched by a dedicated key, and between them every
 * printable ASCII character 0x20-0x7e is reachable -- checked by hand
 * against the full printable set, since a WPA passphrase can legally
 * contain nearly any of it. Letters mode: each digit key cycles its
 * lowercase letters, then uppercase, then its own digit -- the same
 * grouping every physical phone keypad used, including 1 and 0 carrying
 * punctuation/space instead of letters (neither ever had letters on a
 * real phone either). Symbols mode swaps all ten keys to the printable
 * ASCII letters mode doesn't reach. A couple of characters (^ and ~) are
 * reachable from two different keys -- harmless, not worth trimming for
 * one fewer keystroke on an already-short list. */
#define KB_BUF_MAX 96
static char       kb_buf[KB_BUF_MAX];
static int        kb_len;
static char       kb_title[64];
static screen_t   kb_return_screen;
/* What Done actually does -- kept as a purpose enum rather than a function
 * pointer so kb_buf's lifetime and the action are both plain state, visible
 * to mlog() and safe across a screen redraw, rather than a pointer that has
 * to be re-armed correctly on every kb_open() call site. */
typedef enum { KB_PURPOSE_NONE = 0, KB_PURPOSE_WIFI_PASSWORD, KB_PURPOSE_WIFI_SSID_MANUAL,
               KB_PURPOSE_NEW_PLAYLIST_NAME, KB_PURPOSE_RENAME_PLAYLIST } kb_purpose_t;
static kb_purpose_t kb_purpose;
/* Three modes, not two. The original pair had the mode key labelled "123"
 * while actually switching to *symbols* -- reported as exactly that
 * confusion. Digits were only reachable by cycling several taps into a
 * letter key, which is also poor for a passphrase. So: a real numbers
 * mode where each key is simply its own digit, and the mode key now names
 * what you will GET rather than where you are (letters shows "123",
 * numbers shows "#+=", symbols shows "ABC") -- the convention phone and
 * tablet keyboards already use. */
typedef enum { KB_MODE_LETTERS = 0, KB_MODE_NUMBERS, KB_MODE_SYMBOLS, KB_MODE_N } kb_mode_t;
static int         kb_mode;           /* kb_mode_t */
static int         kb_last_key = -1;  /* which key the pending char came from, -1 = none pending */
static int         kb_cycle_pos;      /* position within that key's own cycle string */
/* R73: requested live ("the T9 keyboard needs capital letters"). Capitals
 * already existed, technically -- KB_LETTERS' own cycle strings end in
 * "ABC" -- but reaching one meant multi-tapping all the way past a/b/c and
 * the bare digit first, which is not what anyone means by "the keyboard
 * needs capitals". Same three-state cycle real T9 phones already use on
 * their own shift/"*" key: off, shift-once (capitalizes the next letter
 * typed, then reverts on its own), caps-lock (stays on). Deliberately
 * layered UNDER the existing cycle rather than replacing it: which
 * character a key cycles to (a vs b vs c) and whether it comes out
 * capitalized are orthogonal, so shift just uppercases whatever kb_apply_
 * key() would have inserted anyway -- KB_LETTERS' own "ABC" tail is still
 * there and still reachable, just redundant now, not worth ripping out for
 * a table only this function reads. */
typedef enum { KB_SHIFT_OFF = 0, KB_SHIFT_ONCE, KB_SHIFT_LOCK } kb_shift_t;
static int         kb_shift;          /* kb_shift_t */
/* Whether the character *currently being cycled* is capitalized -- snapshot
 * once when that character starts (see kb_apply_key()'s own comment on why
 * this can't just re-read kb_shift every keystroke: shift-once has to stay
 * applied through every tap of a multi-tap cycle for the one character it
 * was meant for, not just the first tap that started it). */
static int         kb_char_shifted;
static int         kb_cursor;         /* insertion point: 0..kb_len, index into kb_buf */
static struct timespec kb_last_tap_at;
#define KB_CYCLE_MS 600   /* same tap-timing feel as HOLD_MS elsewhere -- long enough to
                            * land a deliberate second tap, short enough not to feel laggy */

/* Between the three modes these cover all 95 printable ASCII characters
 * (0x20-0x7e) exactly once, counted by hand rather than assumed: space +
 * 4 punctuation + 52 letters + 10 digits + 28 symbols = 95. That matters
 * because a WPA passphrase may legally contain any of them, and a
 * keyboard that silently cannot type one is worse than no keyboard.
 *
 * Key 1 previously carried ".,'-_1" and rendered as an unreadable smear
 * of punctuation -- reported. Trimmed to the four that actually earn a
 * top-level slot; the rest moved into symbols mode, which has room. */
static const char *KB_LETTERS[10] = {
    /*0*/ " ",
    /*1*/ ".,?!",
    /*2*/ "abc2ABC",
    /*3*/ "def3DEF",
    /*4*/ "ghi4GHI",
    /*5*/ "jkl5JKL",
    /*6*/ "mno6MNO",
    /*7*/ "pqrs7PQRS",
    /*8*/ "tuv8TUV",
    /*9*/ "wxyz9WXYZ",
};
static const char *KB_NUMBERS[10] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};
static const char *KB_SYMBOLS[10] = {
    /*0*/ " ",
    /*1*/ "\"#$",
    /*2*/ "%&'",
    /*3*/ "()*",
    /*4*/ "+-/",
    /*5*/ ":;=",
    /*6*/ "<>@",
    /*7*/ "[\\]",
    /*8*/ "^_`",
    /*9*/ "{|}~",
};

static const char **kb_table(void) {
    return kb_mode == KB_MODE_NUMBERS ? KB_NUMBERS
         : kb_mode == KB_MODE_SYMBOLS ? KB_SYMBOLS : KB_LETTERS;
}

static long kb_ms_since_last_tap(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - kb_last_tap_at.tv_sec) * 1000L +
           (now.tv_nsec - kb_last_tap_at.tv_nsec) / 1000000L;
}

/* Open the keyboard for one specific purpose. buf/init_text let a caller
 * pre-fill it (editing an existing value) -- both WiFi call sites so far
 * start empty, but the mechanism is generic rather than assuming that. */
static void kb_open(const char *title, kb_purpose_t purpose, const char *init_text) {
    snprintf(kb_title, sizeof(kb_title), "%s", title);
    kb_purpose = purpose;
    snprintf(kb_buf, sizeof(kb_buf), "%s", init_text ? init_text : "");
    kb_len = (int)strlen(kb_buf);
    kb_cursor = kb_len;   /* editing a pre-filled value starts at its end */
    kb_mode = KB_MODE_LETTERS;
    kb_last_key = -1;
    kb_cycle_pos = 0;
    kb_shift = KB_SHIFT_OFF;
    kb_char_shifted = 0;
    /* kb_commit() re-opens the keyboard for a second step (SSID then
     * password) while `screen` is still SC_KEYBOARD from the first step --
     * capturing that here would make Cancel-from-the-password-step land
     * back on the keyboard it just left instead of on whatever screen
     * this whole flow actually started from. Only a *fresh* open (called
     * from a real screen) gets to set it. */
    if (screen != SC_KEYBOARD) kb_return_screen = screen;
    screen = SC_KEYBOARD;
}

/* All edits happen at kb_cursor rather than at the end of the buffer, so
 * text can be corrected mid-string instead of only backspaced to. */
static void kb_insert_at_cursor(char ch) {
    if (kb_len >= KB_BUF_MAX - 1) return;
    /* +1 moves the NUL along with the tail. */
    memmove(kb_buf + kb_cursor + 1, kb_buf + kb_cursor,
            (size_t)(kb_len - kb_cursor) + 1);
    kb_buf[kb_cursor] = ch;
    kb_len++;
    kb_cursor++;
}

/* Same key tapped again inside the cycle window -- replace the character
 * that same key just inserted with the next one in its cycle, rather than
 * inserting a second character. A different key, the window elapsing, or
 * the cursor having been moved by a tap (kb_last_key = -1) all commit
 * what's there and start fresh. */
static void kb_apply_key(int key) {
    const char *cyc = kb_table()[key];
    int cyclen = (int)strlen(cyc);
    if (cyclen == 0) return;
    int same_key = (key == kb_last_key) && kb_ms_since_last_tap() < KB_CYCLE_MS
                   && kb_cursor > 0;
    if (same_key) {
        kb_cycle_pos = (kb_cycle_pos + 1) % cyclen;
        char ch = cyc[kb_cycle_pos];
        /* kb_char_shifted, not kb_shift -- this character's case was
         * already decided when it started (see the else branch below and
         * this function's own top-level comment); cycling a/b/c/etc. must
         * not re-read a shift-once that already reverted after the first
         * tap. */
        if (kb_char_shifted) ch = (char)toupper((unsigned char)ch);
        kb_buf[kb_cursor - 1] = ch;   /* in place: length unchanged */
    } else {
        kb_cycle_pos = 0;
        kb_char_shifted = (kb_mode == KB_MODE_LETTERS) && (kb_shift != KB_SHIFT_OFF);
        char ch = cyc[0];
        if (kb_char_shifted) ch = (char)toupper((unsigned char)ch);
        kb_insert_at_cursor(ch);
        /* Shift-once is consumed by the character it just capitalized, win
         * or lose which letter cycling eventually lands on; caps-lock keeps
         * going. */
        if (kb_shift == KB_SHIFT_ONCE) kb_shift = KB_SHIFT_OFF;
    }
    kb_last_key = key;
    clock_gettime(CLOCK_MONOTONIC, &kb_last_tap_at);
}

static void kb_backspace(void) {
    if (kb_cursor <= 0) return;
    memmove(kb_buf + kb_cursor - 1, kb_buf + kb_cursor,
            (size_t)(kb_len - kb_cursor) + 1);
    kb_len--;
    kb_cursor--;
    kb_last_key = -1;   /* backspacing shouldn't cycle into whatever's now before the cursor */
}

/* x of the caret when it sits before character index `idx` -- measured with
 * the same text_width() the field itself draws through, so the caret lands
 * exactly between the glyphs rather than at an estimated pitch. */
#define KB_FIELD_X 36
static int kb_caret_x(int idx) {
    char tmp[KB_BUF_MAX];
    if (idx < 0) idx = 0;
    if (idx > kb_len) idx = kb_len;
    memcpy(tmp, kb_buf, (size_t)idx);
    tmp[idx] = '\0';
    return KB_FIELD_X + text_width(tmp, TEXT_PX_BODY);
}

/* Tapping in the field puts the caret at the nearest character boundary --
 * every boundary is measured and the closest wins, rather than dividing by
 * an average character width, because this is a proportional font where
 * "il" and "MW" are nothing like the same width. kb_len is capped at 96, so
 * the linear scan costs nothing on a single tap. */
static void kb_set_cursor_from_x(int tx) {
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i <= kb_len; i++) {
        int cx = kb_caret_x(i);
        int d = cx > tx ? cx - tx : tx - cx;
        if (d < bestd) { bestd = d; best = i; }
    }
    kb_cursor = best;
    kb_last_key = -1;   /* a moved caret must not continue the previous key's cycle */
}

/* Which SSID a KB_PURPOSE_WIFI_PASSWORD session is for -- kb_buf itself
 * only ever holds the password being typed, so the SSID has to live
 * somewhere else, set by whichever screen called kb_open(). */
static char kb_wifi_target_ssid[64];

/* R71: which track (if any) a KB_PURPOSE_NEW_PLAYLIST_NAME session should
 * add to the playlist once it's created -- set when the keyboard is opened
 * from the "Add to Playlist" sheet's own "New Playlist..." row, left at -1
 * when opened from SC_PLAYLISTS' own "New Playlist" row instead, where
 * there is no specific track in play and creating an empty playlist is the
 * whole point. */
static int kb_new_playlist_add_track = -1;

/* R74: which playlist a KB_PURPOSE_RENAME_PLAYLIST session is renaming --
 * kb_buf only ever holds the new name being typed, same reasoning
 * kb_wifi_target_ssid's own comment gives for the SSID during a password
 * session. The path, not an index into playlists[]: a rescan between
 * opening the keyboard and committing (unlikely in the couple of seconds
 * typing takes, but not impossible) could reshuffle indices, while the
 * path itself stays a stable identifier either way. */
static char kb_rename_playlist_path[LIB_PATH_LEN];

/* SSIDs and passphrases can contain nearly any byte a shell or a plain
 * "key = value" line would treat as a control character (spaces, '=',
 * '#', quotes, even raw newlines are technically legal in a passphrase).
 * Hex is the same trick st_bt_name() already uses to survive bluetoothctl
 * output unscathed -- no escaping scheme to get subtly wrong, and every
 * byte round-trips exactly. */
static void hex_encode(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 2 < outsz; p++)
        o += (size_t)snprintf(out + o, outsz - o, "%02x", *p);
    out[o] = '\0';
}
static void hex_decode(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (const char *p = in; p[0] && p[1] && o + 1 < outsz; p += 2) {
        int hi = (p[0] >= 'a') ? p[0] - 'a' + 10 : p[0] - '0';
        int lo = (p[1] >= 'a') ? p[1] - 'a' + 10 : p[1] - '0';
        out[o++] = (char)((hi << 4) | lo);
    }
    out[o] = '\0';
}

#define WIFI_CREDS_PATH "/usr/data/settings.txt"

/* Read-modify-write, same idiom save_conf() already uses for music.conf --
 * a network re-entered (password changed, or just reselected) replaces its
 * own line rather than appending a duplicate. Kept in its own file rather
 * than folded into music.conf: credentials are a different kind of data
 * (potentially many rows, not a fixed set of singleton keys) and arguably
 * deserve being easy to find/wipe on their own. */
static void wifi_save_credential(const char *ssid, const char *password) {
    char hex_ssid[80]; hex_encode(ssid, hex_ssid, sizeof(hex_ssid));
    char lines[64][256];
    int n = 0;
    FILE *f = fopen(WIFI_CREDS_PATH, "r");
    if (f) {
        char line[256];
        char prefix[96];
        snprintf(prefix, sizeof(prefix), "wifi_cred = %s ", hex_ssid);
        while (n < 64 && fgets(line, sizeof(line), f)) {
            if (strncmp(line, prefix, strlen(prefix)) != 0)
                snprintf(lines[n++], sizeof(lines[0]), "%s", line);
        }
        fclose(f);
    }
    f = fopen(WIFI_CREDS_PATH, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    char hex_pw[192]; hex_encode(password, hex_pw, sizeof(hex_pw));
    fprintf(f, "wifi_cred = %s %s\n", hex_ssid, hex_pw);
    fclose(f);
}

/* Everything wifi_on.sh already relies on: ctrl_interface is set in
 * /usr/data/wpa_supplicant.conf (confirmed live), so wpa_cli talks to a
 * supplicant that's already running rather than needing one started here.
 * add_network/set_network/enable_network/save_config is the standard
 * wpa_cli sequence for "remember and connect to this network" -- save_config
 * is what makes it survive past this boot, same file wifi_on.sh already
 * reads back on every future Wi-Fi-on. shell-quoted through a fixed-size
 * buffer with embedded quotes stripped rather than escaped: a passphrase
 * containing a literal `"` is vanishingly unlikely on this device's actual
 * use, and getting shell-escaping subtly wrong here is a worse failure mode
 * (arbitrary command injection from a typed password) than refusing the
 * one exotic character. */
static void shell_safe(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < outsz; p++)
        if (*p != '"' && *p != '\\' && *p != '`' && *p != '$') out[o++] = *p;
    out[o] = '\0';
}
static void wifi_connect(const char *ssid, const char *password) {
    char s_ssid[64], s_pw[128];
    shell_safe(ssid, s_ssid, sizeof(s_ssid));
    shell_safe(password, s_pw, sizeof(s_pw));
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "id=$(wpa_cli add_network | tail -1); "
        "wpa_cli set_network $id ssid '\"%s\"' >/dev/null; "
        "wpa_cli set_network $id psk '\"%s\"' >/dev/null; "
        "wpa_cli enable_network $id >/dev/null; "
        "wpa_cli save_config >/dev/null &",
        s_ssid, s_pw);
    if (system(cmd) == -1) mlog("[music] wifi_connect: system() failed\n");
    wifi_save_credential(ssid, password);
    mlog("[music] wifi: connecting to %s\n", ssid);
}

/* Done was tapped -- hand the finished buffer to whichever purpose opened
 * the keyboard, then return to wherever that screen came from. A no-op
 * default case rather than an assert: KB_PURPOSE_NONE only happens if a
 * future call site forgets to set one, and silently doing nothing is a
 * far better failure mode on a device with no attached debugger than a
 * crash mid-typing. */
static void kb_commit(void) {
    switch (kb_purpose) {
        case KB_PURPOSE_WIFI_PASSWORD:
            wifi_connect(kb_wifi_target_ssid, kb_buf);
            break;
        case KB_PURPOSE_WIFI_SSID_MANUAL:
            /* Hidden-network entry: the buffer just typed *is* the SSID --
             * hand off to a second keyboard pass for its password, same
             * purpose the scan-and-select path already uses from here. */
            snprintf(kb_wifi_target_ssid, sizeof(kb_wifi_target_ssid), "%s", kb_buf);
            kb_open("Password for this network", KB_PURPOSE_WIFI_PASSWORD, "");
            return;   /* stays on SC_KEYBOARD -- do not fall through to the pop below */
        case KB_PURPOSE_NEW_PLAYLIST_NAME: {
            /* R71. An empty kb_buf still creates one -- pl_create() itself
             * falls back to "New Playlist" rather than this needing its own
             * empty-name guard, same reasoning kb_buf's other purposes trust
             * their own callee with an edge case rather than duplicating the
             * check here. */
            char path[LIB_PATH_LEN];
            if (pl_create(kb_buf, path, sizeof(path)) == 0) {
                playlist_n = pl_list(playlists, PL_MAX);
                if (kb_new_playlist_add_track >= 0 && kb_new_playlist_add_track < track_n) {
                    pl_append(path, tracks[kb_new_playlist_add_track].path);
                    /* pl_create() may have de-duplicated the typed name (an
                     * existing "Road Trip" leaves this one "Road Trip (2)")
                     * -- the note should say what actually got created, not
                     * what was typed, so this reads path's own filename
                     * rather than kb_buf. */
                    const char *base = strrchr(path, '/');
                    base = base ? base + 1 : path;
                    char name[LIB_NAME_LEN];
                    snprintf(name, sizeof(name), "%s", base);
                    char *ext = strrchr(name, '.');
                    if (ext && !strcasecmp(ext, ".m3u")) *ext = '\0';
                    snprintf(sheet_note, sizeof(sheet_note), "Added to %s", name);
                }
            }
            kb_new_playlist_add_track = -1;
            break;
        }
        case KB_PURPOSE_RENAME_PLAYLIST: {
            char path[LIB_PATH_LEN];
            if (pl_rename(kb_rename_playlist_path, kb_buf, path, sizeof(path)) == 0)
                playlist_n = pl_list(playlists, PL_MAX);
            break;
        }
        case KB_PURPOSE_NONE:
        default:
            break;
    }
    screen = kb_return_screen;
}
/* Which slider is being dragged live, on whichever of SC_EQ/SC_EQ_BAND is
 * showing -- 0 none, 1 preamp, 2 frequency, 3 gain, 4 Q. Mirrors qs_dragging/
 * vol_dragging: touch-down-in-band starts it, touch-up ends it, live_x drives
 * the value every tick in between. */
static int         eq_dragging;

static float       mseb_gain[MSEB_BAND_N];
static int         mseb_on;
/* -1 none, else the band index being dragged on SC_MSEB. Same live_x-driven
 * pattern as eq_dragging, kept separate since MSEB has 9 independent
 * sliders rather than eq_dragging's fixed handful of named ones. */
static int         mseb_dragging = -1;

static lib_track_t queue[PAGE_MAX * 32];   /* BG111: kept in lockstep with tracks[] -- see its own comment */
static int  queue_n;
static char q_artist[LIB_NAME_LEN];
static char q_album[LIB_NAME_LEN];
/* BG73: whether the queue's own q_artist/q_album identity is a playlist
 * rather than an album -- a playlist's order was chosen by the user, so
 * "Play next" must never drop the rest of it, only an album's leftover
 * tracks are fair game for that (see queue_play_next()). Set from
 * browsing_is_playlist at the moment a fresh queue is created. */
static int  q_is_playlist;
/* BG85: queue_play_next() used to overwrite q_artist/q_album with the
 * played-next track's own album the instant it was queued -- but that
 * track isn't playing yet, it's only next in line, so Now Playing showed
 * the *new* album's name (q_album, drawn separately from the actual
 * playing track's own name) against audio that was still very much the
 * old one, for however long the current track had left to run. Reported
 * live as "adding to queue changes the name of the currently playing
 * album." These hold the pending identity instead, applied by
 * queue_apply_pending() only once playback actually reaches q_pending_at
 * -- so the header keeps naming whatever is actually audible right up
 * until it changes for real. -1/empty when nothing is pending. */
static char q_artist_pending[LIB_NAME_LEN];
static char q_album_pending[LIB_NAME_LEN];
static int  q_pending_at = -1;
/* BG73 follow-up: set the moment queue_play_next() drops an album's
 * leftover tracks in favour of a played-next one from somewhere else --
 * from then on queue[] no longer represents one browsable album (its own
 * earlier entries are still the old album, its tail is the new one), so
 * swiping back from Now Playing must stop presenting it as a single
 * album page (one title, one cover) and show the plain queue list instead
 * (see go_back()'s SC_PLAYING case). Cleared whenever a fresh queue starts
 * (play_from_list()). Playlists are deliberately not covered by this --
 * a playlist was already an arbitrary mix of albums/artists from the
 * start, so a played-next track spliced into one isn't a new kind of
 * "not really one album" the way it is for a real album's own queue. */
static int  queue_mixed;
/* Reported live: once queue_mixed made swipe-back-from-Now-Playing land on
 * SC_QUEUE, backing out of *that* unconditionally returned to SC_PLAYING --
 * fine when SC_QUEUE was reached deliberately via the queue button (that's
 * still exactly what "back" should do there), but a dead loop when it was
 * reached as the swipe-back fallback instead: the swipe already meant "back
 * out of Now Playing", so bouncing back to Now Playing on the very next back
 * goes nowhere. Sourcing SC_QUEUE from two different gestures means "back"
 * out of it has to know which one got it there -- set on both entries. */
static int  queue_via_back;

/* R70: drag-to-reorder via the grip handle. queue_drag_active is gated on
 * the touch having started inside the grip's own hit zone -- touch_x/
 * touch_y are fixed at the press location for the life of a gesture (see
 * the raw touch reader), so testing them once here at press time and again
 * every tick afterward is the same test, no separate "just pressed" edge
 * needed. Audiobooks never reach this: their own queue button goes
 * straight to go_back() into the chapter list (SC_TRACKS), never SC_QUEUE
 * -- see that call site's own comment -- so no explicit exclusion is
 * needed here either. */
static int  queue_drag_active;
static int  queue_drag_display_i;   /* current slot of the row being dragged */
static int  queue_drag_grab_dy;     /* touch_y minus the row's own top, at grab time */

/* R70: swipe-to-remove, on the row body rather than the grip -- a touch
 * starting anywhere on a row except the grip zone is a swipe candidate
 * until movement commits it one way or the other (mirrors
 * edge_zone_ambiguous's own "still could become either" reasoning). Once
 * horizontal travel is clearly leading, it can never become a reorder-drag
 * (grip-only) or a list scroll (vertical-only) -- it already isn't either,
 * having started off the grip. */
static int  queue_swipe_active;
static int  queue_swipe_display_i;
static int  queue_swipe_dx;         /* live_x - touch_x while active, signed */

/* R70: same grip/drag/swipe shapes as SC_QUEUE's own above, extended to a
 * playlist's own track list (SC_TRACKS with browsing_is_playlist set).
 * Kept as separate state rather than reusing the queue_* variables --
 * SC_QUEUE and this can never be showing at once, but sharing variables
 * across two unrelated screens is exactly the kind of thing that grows a
 * subtle bug the day one of them changes without the other. This screen's
 * own continuous-scroll coordinate space (header_h/off, not CONTENT_Y/
 * scroll) also means the hit-testing arithmetic genuinely differs, not
 * just the variable names. */
static int  playlist_drag_active;
static int  playlist_drag_idx;        /* index into tracks[] being dragged */
static int  playlist_drag_grab_dy;    /* touch_y minus the row's own top, at grab time */
static int  playlist_swipe_active;
static int  playlist_swipe_idx;
static int  playlist_swipe_dx;
/* R70: swipe-to-remove on a podcast feed's episode list -- no grip, no drag,
 * since episodes carry no user-chosen order to reorder (they're newest-
 * first by mtime/pubdate, same as SC_PODCASTS itself). Flat CONTENT_Y/
 * scroll coordinate space, same as SC_QUEUE, not track_index_at() -- this
 * is the ab_list/pod_list "flat list" SC_TRACKS path, not the continuous-
 * scroll one playlists and plain albums share. */
static int  pod_swipe_active;
static int  pod_swipe_idx;
static int  pod_swipe_dx;
static char cur_artist[LIB_NAME_LEN];
static char cur_album[LIB_NAME_LEN];
/* BG73: whether cur_artist/cur_album (whatever's currently loaded into
 * tracks[]) is a playlist -- set wherever SC_TRACKS is entered as a browse
 * target (SC_PLAYLISTS vs SC_ALBUMS), read by play_from_list()/
 * queue_play_next() so a fresh queue or a Play Next both know which of the
 * two "leave the rest alone" vs "drop the rest" rules applies. */
static int  browsing_is_playlist;

/* R70: the .m3u path for whatever playlist SC_TRACKS is currently showing,
 * looked up fresh by name rather than cached at the one call site that
 * opens a playlist from SC_PLAYLISTS -- browsing_is_playlist can also
 * become true via the "back from Now Playing" route (see its own BG73
 * comment), which never touches a cached path, so a cache would be stale
 * or empty there. A rare, deliberate action (starting a drag or a swipe),
 * not a redraw-rate concern -- same reasoning the Bluetooth pairing tap
 * handler already uses for re-querying bt_scan_devices() fresh. Returns 0
 * and leaves out untouched if cur_album no longer matches any playlist on
 * the card (deleted or renamed since it was opened). */
static int cur_playlist_path(char *out, size_t outsz) {
    pl_t pl[PL_MAX];
    int n = pl_list(pl, PL_MAX);
    for (int i = 0; i < n; i++) {
        if (!strcmp(pl[i].name, cur_album)) {
            snprintf(out, outsz, "%s", pl[i].path);
            return 1;
        }
    }
    return 0;
}

/* R70: writes tracks[0..track_n) back to the current playlist's .m3u, in
 * whatever order the array is in right now -- called after every reorder
 * and every remove, same "commit immediately" shape queue_move_display()/
 * queue_remove_display() use for the Now Playing queue. A no-op (not an
 * error) if cur_album no longer names a real playlist on the card -- see
 * cur_playlist_path()'s own comment on when that happens. */
/* BG111 raised tracks[]/queue[] and the two playlist path scratch buffers
 * from 256 to 1024 entries so a long audiobook's chapters stop truncating --
 * right fix, but at LIB_PATH_LEN each those two scratch buffers are 393 KB
 * apiece sitting in .bss for the entire life of the app, on a device with
 * 57 MB of RAM total. Neither one needs to persist: one writes tracks[] out
 * to an .m3u, the other reads an .m3u back into tracks[], both on the UI
 * thread and neither while the other runs. Taken for the moment they are
 * used instead, which hands ~786 KB back to everything else.
 *
 * MemFree sits near 1.3 MB on this device but MemAvailable is ~25 MB -- the
 * difference is reclaimable page cache, so an allocation this size succeeds
 * comfortably. It is still checked: failing to allocate means doing nothing
 * rather than truncating a playlist someone just reordered. */
#define PL_SCRATCH_N (PAGE_MAX * 32)
typedef char pl_path_t[LIB_PATH_LEN];

static pl_path_t *pl_scratch_get(const char *what) {
    pl_path_t *p = malloc(sizeof(pl_path_t) * PL_SCRATCH_N);
    if (!p) mlog("[music] %s: no memory for %d-path scratch\n", what, PL_SCRATCH_N);
    return p;
}

static void playlist_persist(void) {
    char path[LIB_PATH_LEN];
    if (!cur_playlist_path(path, sizeof(path))) return;
    pl_path_t *paths = pl_scratch_get("playlist_persist");
    if (!paths) return;
    int n = track_n < PL_SCRATCH_N ? track_n : PL_SCRATCH_N;
    for (int i = 0; i < n; i++)
        snprintf(paths[i], LIB_PATH_LEN, "%s", tracks[i].path);
    pl_write(path, paths, n);
    free(paths);
}

/* R70: moves tracks[from] to tracks[to], shifting between, and persists.
 * Unlike queue_move_display() this never has a shuffle order to reorder
 * instead -- playlists don't shuffle -- so it's always a direct array
 * shift. Does NOT touch queue[]/cur_track even if this exact playlist is
 * also the one currently playing: browsing and playback are already kept
 * as separate concerns everywhere else in this file (q_is_playlist vs
 * browsing_is_playlist), and reaching into a live queue mid-drag would be
 * a bigger, riskier change than this feature needs. Known limitation, not
 * silently glossed over: the "now playing" row highlight (idx == cur_track)
 * can point at the wrong row until the next fresh play of this playlist,
 * if it is both open for editing and currently playing at the same time. */
static void playlist_move_track(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= track_n || to >= track_n) return;
    lib_track_t v = tracks[from];
    if (from < to) for (int i = from; i < to; i++) tracks[i] = tracks[i + 1];
    else            for (int i = from; i > to; i--) tracks[i] = tracks[i - 1];
    tracks[to] = v;
    playlist_persist();
}

/* R70: removes tracks[idx] from the browse list and persists. Does not
 * touch queue[]/playback -- removing a track from a playlist you're
 * looking at is not the same act as removing it from what's currently
 * playing, and if this playlist happens to be both, ongoing playback is
 * left alone on purpose (see playlist_move_track()'s own comment). */
static void playlist_remove_track(int idx) {
    if (idx < 0 || idx >= track_n) return;
    for (int i = idx; i < track_n - 1; i++) tracks[i] = tracks[i + 1];
    track_n--;
    playlist_persist();
}
/* Same idea as browsing_is_playlist just above, for a second thing SC_TRACKS
 * needs to remember about how it was reached: an album opened from the
 * artist page's own album list has "back" return there directly (nothing
 * needs re-fetching -- artist_page_name/artist_page_albums/artist_art_bits
 * are all still sitting in memory untouched), not fall through to the
 * default "restore the Albums list" case, which would skip past the artist
 * page entirely and land somewhere the user never actually was browsing. */
static int  tracks_from_artist_page;
/* What cur_artist was filtering the Albums list by, before a tap into a
 * specific album's Tracks overwrote it with that one album's artist (BG7):
 * two artists can share an album name, so the track query needs the exact
 * artist, but that leaves nothing telling "back" whether Albums was ever
 * filtered at all. Set wherever SC_ALBUMS is actually entered as a browse
 * target, restored into cur_artist on the way back out of Tracks. */
static char albums_artist[LIB_NAME_LEN];
/* Set right before a normal Albums -> Tracks -> Playing descent; cleared at
 * the mini-player's direct jump into Playing. Tells go_back()'s SC_PLAYING
 * case whether albums_artist (the "artist" facet column's group name, e.g.
 * "Alice Sara Ott") is still good, or whether q_artist (BG7's fallback,
 * which is really the album_artist *column*'s value, e.g. "Alice Sara Ott
 * (piano)" -- a different tag that can hold a different string) is the only
 * thing available. Without this, backing out of a normal browse clobbered a
 * perfectly good albums_artist with a value from the wrong column, and the
 * next "back" filtered Albums by a facet value that matched nothing. */
static int played_from_browse;
static int  cur_track;          /* index into tracks[] */

/* Artist page: tapping an artist's name on the album page. Own album list
 * rather than reusing rows[]/row_n -- those belong to whichever paginated
 * list screen (Albums, Artists) is still sitting underneath this one, and
 * this screen loads its albums in a single one-shot fetch the same way the
 * album-detail page loads tracks[] up front, not page by page. */
static char artist_page_name[LIB_NAME_LEN];
#define ARTIST_ALBUMS_MAX (PAGE_MAX * 4)
static lib_row_t artist_page_albums[ARTIST_ALBUMS_MAX];
static int artist_page_album_n;
/* Where "back" from the artist page should land -- the album/artist it was
 * opened from, so backing out returns to browsing that album rather than
 * always falling to the same generic spot regardless of entry point. */
static char artist_page_back_album[LIB_NAME_LEN];
static char artist_page_back_artist[LIB_NAME_LEN];
/* The artist page has two genuinely different entry points now: an album
 * page's artist name (back -> that album, the pair above) and the Artists
 * list itself (back -> the Artists list, same "restore where the list was"
 * shape SC_ALBUMS's own go_back() case already uses). go_back()'s
 * SC_ARTIST_PAGE case needs to know which one got it here. */
static int artist_page_from_list;

/* An A-Z strip down the right edge. 117 artists and 277 albums nine rows at a
 * time is not navigable by swiping, and this is a great deal cheaper to build
 * and to use than a keyboard on a 480x800 screen. */
#define INDEX_W 45          /* half again as wide: it is a thumb, not a stylus */
/* The strip is drawn INDEX_W wide but caught wider: a thumb lands short of
 * the edge more often than it lands on it, and the rows have nothing out
 * here to lose, since their text is already clipped inside INDEX_W. */
#define INDEX_TOUCH_W 72
#define INDEX_N 27                    /* # then A-Z */

/* Sliding left off the strip subdivides the letter under the thumb: B is a
 * shelf of Bruckner, Beethoven and Brahms, and one stop for all of them is
 * no stop at all. Only second letters that exist are offered, so every band
 * leads somewhere. INDEX_SUB_DEAD is the slack before the first one engages,
 * so an ordinary vertical run down the strip never trips it. */
#define INDEX_SUB_MAX  12
#define INDEX_SUB_DEAD 70
#define INDEX_PFX_MAX  200

/* Gathered when the index is first grabbed, not while the thumb is moving. */
static char index_pfx[INDEX_PFX_MAX * 2];
static int  index_pfx_n;
static char index_pfx_key[LIB_NAME_LEN + 16];   /* what it was gathered for */
static int  index_letter_off[LIB_INDEX_LETTERS];
static int  index_letter_off_ready;
typedef struct { char key[3]; int off; } index_pair_off_t;
static index_pair_off_t index_pair_off[INDEX_PFX_MAX];
static int index_pair_off_n;

static char index_sub_lock;     /* the letter the strip is held inside, 0 when not */
static char index_sub[INDEX_SUB_MAX];
static int  index_sub_n;      /* subkeys under the current letter */

static int index_visible(void);
static void load_page(void);

static char index_letter(int i) { return i == 0 ? '#' : (char)('A' + i - 1); }

#define MINI_H 76

/* Shared between draw_mini() and its tap handler, rather than the two each
 * carrying their own copy of these numbers -- BG21 this same session was
 * exactly that drift (a hit zone left pointing at a button's old position
 * after only the draw side moved), and duplicating the numbers here would
 * invite the same bug the next time this row's layout changes. Positions
 * are button centres; zones are the x past which the next button to the
 * right takes over, so each one only has to be right of its own left
 * neighbour and left of its own right one -- not aligned to any particular
 * edge of the glyph drawn at it. */
#define MINI_BTN_R     20              /* every button in a shared row, incl. play/pause */
#define MINI_PLAY_CX   (FB_W - 90)     /* play/pause, when it shares the bar */
#define MINI_SIDE_CX   (FB_W - 34)     /* next (music) / skip-forward (audiobook) */
#define MINI_BACK_CX   (FB_W - 146)    /* skip-back, audiobook only */
#define MINI_ZONE_SIDE (FB_W - 62)     /* right of this: the side button */
#define MINI_ZONE_PLAY (FB_W - 118)    /* right of this: play/pause */
#define MINI_ZONE_BACK (FB_W - 174)    /* right of this: skip-back (audiobook) */

/* The mini player sits over the bottom of the list, so the list has to give up
 * the rows it covers or the last one is unreachable. */
static int mini_visible(void);
static int vis_rows(void) {
    /* R46: the album-detail screen has no status bar or title bar to leave
     * room for -- its content starts at y=0, not CONTENT_Y, same as Now
     * Playing's own art already does. Artist page: same, its own photo
     * runs edge-to-edge from y=0 too. */
    int plain_album = screen == SC_TRACKS && !ab_list && !pod_list;
    int artist_page = screen == SC_ARTIST_PAGE;
    int top = (plain_album || artist_page) ? 0 : CONTENT_Y;
    /* R50: the Queue screen's one extra header line (track count + playtime)
     * sits above the rows, not in the first row's own slot -- one fewer
     * row's worth of height is actually available to scroll through. */
    if (screen == SC_QUEUE) top += QUEUE_SUMMARY_H;
    /* Reported live: the plain-album screen reserved the same 40px bottom
     * margin as every other screen unconditionally, whether or not there
     * was actually a sheet_note toast to show in it -- a track could settle
     * with its bottom half cut off right at that boundary even with nothing
     * drawn there. That margin only ever holds sheet_note on this screen
     * (see the draw side), so it's only reserved while one is actually
     * showing; every other screen's reasons for the same 40px (route info,
     * Wi-Fi-off messages, etc.) aren't audited here, so they keep the
     * unconditional margin. The artist page has no sheet_note gesture at
     * all, so it never reserves this margin unless the mini player needs it. */
    int bottom_margin = mini_visible() ? MINI_H
                       : plain_album    ? (sheet_note[0] ? 40 : 0)
                       : artist_page    ? 0
                       : 40;
    int h = FB_H - top - bottom_margin;
    return h / ROW_H;
}

/* R75: requested live ("the wifi scan list should be scrollable"). This
 * screen now scrolls like SC_SETTINGS' own longer list does -- scan
 * results can run well past a screenful on a dense apartment building or
 * office. wifi_nets/wifi_net_n/wifi_nets_refresh_at moved to file scope
 * (were function-local statics in the draw block) since the scroll-limit
 * calculation and the tap handler both need to read the current result
 * count too, not just the draw call that refreshes it -- same
 * "compute/refresh once, every reader this frame sees the same thing"
 * shape g_header_show_back already established for the back arrow. */
static wifi_found_net_t wifi_nets[10];
static int              wifi_net_n;
static time_t           wifi_nets_refresh_at;

/* BG109 follow-up ("we also need scrolling on wifi and bluetooth device
 * lists"): same treatment, same reasons, for SC_SETTINGS_BT's device
 * list. Previously a small, bespoke, non-scrolling draw loop capped by
 * bt_scan_max_visible() (removed -- no longer needed once a row that's
 * scrolled off-screen has no on-screen y for a tap to land on in the
 * first place, same as R75's own wifi tap handler already reasons).
 * devs/dev_n/last_refresh hoisted to file scope for the same reason
 * wifi_nets/wifi_net_n were. */
static bt_found_dev_t bt_devs[8];
static int             bt_dev_n;
static time_t          bt_devs_refresh_at;
static int bt_row_n(void) { return 3 + bt_dev_n; }   /* toggle, status, "Scan for devices" */

/* 4 fixed rows (toggle, status, "Scan for networks", "Add network
 * manually") plus however many results are currently cached -- the
 * scroll-limit ternary's own `total` for this screen. */
static int wifi_row_n(void) { return 4 + wifi_net_n; }

/* Only the two lists long enough to need it -- and not Recent, capped at
 * PAGE_MAX items and sorted by recency rather than alphabetically, where a
 * letter strip would have nothing meaningful to jump to. */
static int index_visible(void) {
    return (screen == SC_ARTISTS || screen == SC_ALBUMS) && !recent_mode;
}

static int index_bottom(void) { return FB_H - (mini_visible() ? MINI_H : 40); }

/* How many stops the strip is showing: 27 letters, or the second letters of
 * the one it is locked to. */
static int index_stops(void) { return index_sub_lock ? index_sub_n : INDEX_N; }

/* The label for stop `i` — "B" at the top level, "Br" inside one. */
static void index_label(int i, char *out) {
    if (index_sub_lock) { out[0] = index_sub_lock; out[1] = index_sub[i]; out[2] = '\0'; }
    else                { out[0] = index_letter(i); out[1] = '\0'; }
}

static void index_cache_reset(void) {
    index_pfx_n = 0;
    index_pfx_key[0] = '\0';
    index_letter_off_ready = 0;
    index_pair_off_n = 0;
}

/* This work belongs to picking up the A-Z strip, not every ordinary touch in
 * an indexed list. The letter offsets are one aggregate query, replacing two
 * count-distinct scans for every letter crossed during a fast drag. */
static void index_prepare(void) {
    char want[LIB_NAME_LEN + 16];
    snprintf(want, sizeof(want), "%d/%s/%s", (int)screen,
             cur_facet ? cur_facet : "", cur_artist);
    if (strcmp(want, index_pfx_key) == 0) return;

    index_pfx_n = lib_prefixes(cur_facet,
                               cur_artist[0] ? cur_artist : NULL,
                               screen == SC_ALBUMS,
                               index_pfx, INDEX_PFX_MAX);
    index_letter_off_ready =
        lib_letter_offsets(cur_facet, cur_artist[0] ? cur_artist : NULL,
                           screen == SC_ALBUMS, index_letter_off) == 0;
    index_pair_off_n = 0;
    snprintf(index_pfx_key, sizeof(index_pfx_key), "%s", want);
    mlog("[music] index: %d prefixes, letter offsets %s for %s\n",
         index_pfx_n, index_letter_off_ready ? "ready" : "unavailable", want);
}

/* Single keys are already covered by the aggregate map. Second-letter keys
 * are much rarer, so memoising their exact legacy query keeps their SQLite
 * comparison behaviour without carrying the whole distinct list in memory. */
static int index_offset(const char *key) {
    if (!key[1] && key[0] >= 'A' && key[0] <= '[' && index_letter_off_ready)
        return index_letter_off[key[0] - 'A'];

    for (int i = 0; i < index_pair_off_n; i++)
        if (!strcmp(key, index_pair_off[i].key)) return index_pair_off[i].off;

    int off = (screen == SC_ARTISTS) ? lib_group_offset(cur_facet, key)
                                     : lib_albums_offset(cur_facet,
                                                         cur_artist[0] ? cur_artist : NULL,
                                                         key);
    if (key[1] && index_pair_off_n < INDEX_PFX_MAX) {
        snprintf(index_pair_off[index_pair_off_n].key,
                 sizeof(index_pair_off[index_pair_off_n].key), "%s", key);
        index_pair_off[index_pair_off_n++].off = off;
    }
    return off;
}

static void draw_index(uint16_t *fb) {
    int top = CONTENT_Y + 4, bot = index_bottom() - 4;
    int n = index_stops();
    int step = (bot - top) / (n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        char c[4];
        index_label(i, c);
        int w = text_width(c, TEXT_PX_SMALL);
        int on = index_active && i == index_shown;
        int cy2 = top + i * step + (step - 18) / 2;
        if (on) {
            /* A droplet pointing back at the strip: round where the letter
             * sits, tapering to a point on the right. Far enough out to clear
             * the thumb, and dark-on-orange so it reads at a glance. */
            int r = 22, cx = FB_W - INDEX_W - 110;
            fill_circle(fb, cx, cy2 + 9, r, COL_ACCENT);
            for (int dx = 0; dx <= r + 16; dx++) {
                int half = r - (r * dx) / (r + 16);
                if (half > 0) fill_rect(fb, cx + dx, cy2 + 9 - half, 1, half * 2, COL_ACCENT);
            }
            draw_text(fb, cx - w / 2, cy2, c, COL_BG, TEXT_PX_SMALL, FB_W);
        } else {
            draw_text(fb, FB_W - INDEX_W / 2 - w / 2, cy2, c, COL_DIM, TEXT_PX_SMALL, FB_W);
        }
    }
}

/* Jump so the first entry beginning with the touched letter is at the top. */
static int index_slot(int y) {
    int top = CONTENT_Y + 4, bot = index_bottom() - 4;
    int n = index_stops();
    int step = (bot - top) / (n > 0 ? n : 1);
    if (step <= 0 || n <= 0) return -1;
    int i = (y - top) / step;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return i;
}

static void index_jump_key(int i, const char *key) {
    if (i < 0) return;

    int off = 0;
    if (i > 0) off = index_offset(key);
    mlog("[music] jumpkey i=%d key0=%d key1=%d off=%d sub_lock=%d\n",
         i, (int)(unsigned char)key[0], (int)(unsigned char)key[1], off, (int)(unsigned char)index_sub_lock);
    int max_top = total - vis_rows();
    if (max_top < 0) max_top = 0;

    /* The viewport must never show past the end of the top-level letter this
     * jump landed in — whether that's a plain single-letter jump (E has 4
     * albums and room for 8: no amount of dragging should pull in F) or a
     * locked subdivision (Es within E, which may still show sibling
     * subletters like Ex, just never F). Both share one boundary: the start
     * of the next top-level letter, key[0]+1 — true even when locked, since
     * index_label puts the locked letter itself in key[0]. */
    if (i > 0 && key[0]) {
        char next[2] = { (char)(key[0] + 1), '\0' };
        int end = index_offset(next);
        int letter_max = end - vis_rows();
        /* Floored at off, not 0: when the letter is shorter than a screen
         * (most of them, for a library this size), end - vis_rows() falls
         * below where the letter starts, and clamping to 0 dragged the
         * whole jump back toward the start of the alphabet — pulling in the
         * tail of the previous letter above U rather than the next one
         * below it. Anchoring at its own start and letting the remainder of
         * the screen show past the end is the same trade-off already made
         * below; there is no version of this that shows only U when U is
         * shorter than a screen. */
        if (letter_max < off) letter_max = off;
        if (letter_max < max_top) max_top = letter_max;
        index_lock_end = end;
    } else {
        index_lock_end = -1;          /* '#', the very top: nothing to bound */
    }

    if (off > max_top) off = max_top;
    if (off < 0) off = 0;
    scroll = off;
    scroll_px = 0;
    inertia_active = 0; list_velocity = 0;
    load_page();
}

/* The key for a slot, with its second letter when one is selected. */
static void index_key(int i, char *out) { index_label(i, out); }

static void index_jump(int y) {
    char key[3]; int i = index_slot(y);
    if (i < 0) return;
    index_key(i, key);
    /* Slot 0 is '#' only at the top level; inside a letter every stop is real. */
    index_jump_key(index_sub_lock ? 1 : i, key);
}

/* ab_books[] -> ab_rows[], so the existing lib_row_t-shaped draw loop and
 * row_at() can serve the book list without knowing it isn't a database
 * query. A book list is tens to low hundreds of entries, nothing like the
 * 4700-track library the paged rows[]/row_base machinery exists for, so the
 * whole thing is just kept in memory rather than paged. */
static void ab_rebuild_rows(void) {
    for (int i = 0; i < ab_book_n; i++) {
        snprintf(ab_rows[i].name, sizeof(ab_rows[i].name), "%s", ab_books[i].title);
        /* 0 draws nothing. A one-file book was reporting "1" beside a title
         * whose chapter list has seventeen entries in it, which said the
         * opposite of the truth. */
        ab_rows[i].count = ab_books[i].file_count > 1 ? ab_books[i].file_count : 0;
        ab_rows[i].owner[0] = '\0';
    }
}

/* pod_feeds[] -> pod_rows[], same reasoning as ab_rebuild_rows() above. */
static void pod_rebuild_rows(void) {
    for (int i = 0; i < pod_feed_n; i++) {
        snprintf(pod_rows[i].name, sizeof(pod_rows[i].name), "%s", pod_feeds[i].name);
        pod_rows[i].count = 0;
        pod_rows[i].owner[0] = '\0';
    }
}

/* pod_eps[] -> tracks[], same reasoning as ab_load_book() mirroring chapters
 * into tracks[] -- an undownloaded episode gets an empty path, same as any
 * other lib_track_t whose file isn't there yet, so the tap handler is what
 * has to check pod_eps[idx].downloaded, not audio_play() failing at runtime. */
static void pod_rebuild_tracks(void) {
    int max = (int)(sizeof(tracks) / sizeof(tracks[0]));
    track_n = 0;
    for (int i = 0; i < pod_ep_n && track_n < max; i++) {
        lib_track_t *t = &tracks[track_n];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", pod_eps[i].name);
        if (pod_eps[i].downloaded)
            snprintf(t->path, sizeof(t->path), "%s", pod_eps[i].path);
        t->dur_ms = pod_eps[i].dur_ms;
        t->track = -1;
        t->disc  = -1;
        track_n++;
    }
}

/* R70: swipe-to-remove on the episode list. Re-reads the feed from disk
 * afterward rather than patching pod_eps[idx] in place -- same one-shot
 * recompute pattern a completed download already uses just above -- so a
 * still-manifest-listed episode reappears as not-downloaded (the normal
 * look for one never fetched) instead of this needing its own idea of what
 * an emptied slot should show. Downloaded-only: nothing to delete for an
 * episode that is only a manifest entry to begin with. */
static void pod_remove_download(int idx) {
    if (idx < 0 || idx >= pod_ep_n || !pod_eps[idx].downloaded) return;
    pod_delete_download(pod_eps[idx].path);
    pod_ep_n = pod_load_episodes(cur_feed, pod_eps, POD_MAX_ITEMS);
    pod_rebuild_tracks();
}

/* Mirrors ab_save_current_pos() exactly -- see its comment for why the
 * audio_is_active() guard matters (audio_stop() zeroes g_pos_ms on the way
 * out, and podcast_mode/cur_track/queue are static, carrying stale values
 * into the next call otherwise). The last few seconds count as finished
 * rather than a resumable position, so re-opening a just-completed episode
 * starts over instead of resuming one second from the end. */
static void pod_save_current_pos(void) {
    if (!podcast_mode || !audio_is_active()) return;
    if (cur_track < 0 || cur_track >= queue_n) return;
    /* Reported live: reopening an episode came back near 0:00 instead of
     * the real ~38-minute resume point. Root cause: this read audio_pos_ms()
     * directly, same mistake the display code made (see BG80's extension
     * above) -- if the user switches away again before this episode's own
     * resume seek has actually landed (the same 10-20s worker-thread
     * transition window that fix addresses), audio_pos_ms() is still
     * sitting near 0, and saving it overwrites the real position with
     * near-nothing. audio_seek_pending_ms() is the target already asked
     * for, still outstanding until the worker catches up; preferring it
     * here means a save mid-transition writes back the resume point that
     * was already correct, not the decoder's not-there-yet position. */
    int pending = audio_seek_pending_ms();
    int pos = pending >= 0 ? pending : audio_pos_ms();
    int dur = audio_dur_ms();
    /* BG48/BG49's root cause reaches here too: audio_dur_ms() is always 0
     * for MP3 (see pod_probe_dur()'s comment in podcast.c), so without this
     * fallback the dur>0 check below could never see a real duration and an
     * episode could never be marked POD_FINISHED, only ever a plain resume
     * position -- same fallback the draw/seek code already uses. */
    if (dur <= 0) dur = queue[cur_track].dur_ms;
    if (dur > 0 && pos >= dur - 3000) pod_resume_store(queue[cur_track].path, POD_FINISHED, dur);
    else pod_resume_store(queue[cur_track].path, pos, dur);
}

/* One episode, playing on its own -- see the comment by podcast_mode for why
 * this doesn't queue the rest of the feed the way play_from_list() does for
 * an album. Resumes from pod_resume_lookup() when there is a saved position;
 * a POD_FINISHED episode restarts from 0, same as reaching the end of any
 * other track leaves nothing to resume into. */
static void pod_play_episode(int idx) {
    if (idx < 0 || idx >= pod_ep_n || !pod_eps[idx].downloaded) return;
    pod_save_current_pos();
    radio_mode = 0;
    audiobook_mode = 0;
    podcast_mode = 1;
    memcpy(&queue[0], &tracks[idx], sizeof(queue[0]));
    queue_n = 1;
    cur_track = 0;
    q_album[0] = '\0';
    q_artist[0] = '\0';
    q_pending_at = -1;   /* BG85: podcasts never go through queue_play_next() */
    /* Reported live: opening an episode right after something was actively
     * playing over Bluetooth (an audiobook, say) made it look stuck at 0:00
     * for 10-20s before jumping to the real resumed position. Root cause
     * wasn't the resume file itself (a few KB either way, this device's
     * CPU chews through that instantly) -- it was that this lookup used to
     * run *after* audio_play(), synchronously on the main thread, at the
     * exact moment the worker thread was also hitting the SD card hard
     * tearing down the old decoder/Bluetooth PCM connection and opening
     * the new file. This exFAT card is already known to serialize/stall
     * under exactly that kind of concurrent access (see index.c's own
     * comment on it). ab_resume_book() never had this problem because it
     * already does its own lookup *before* calling ab_play_chapter() --
     * mirrored here rather than rediscovered: look up first, while the SD
     * card is still idle, then hand audio_play() a target that's already
     * known instead of racing it. */
    int dur = 0;
    int resume = pod_resume_lookup(pod_eps[idx].path, &dur);
    pod_notes_avail = pod_load_notes(pod_eps[idx].path, pod_notes_text, sizeof(pod_notes_text)) > 0;
    audio_set_next(NULL);
    /* R99: per-feed, not one speed shared across every podcast -- a show's
     * own pacing preference shouldn't reset picking a different episode of
     * the same show, or bleed into an unrelated show's own preference.
     * Looked up fresh on every episode start rather than cached against
     * cur_feed changing, the same reasoning ab_resume_book()/the resume
     * lookup just above already established for doing this early rather
     * than assuming stale state is still correct. 0 back means this feed
     * has no saved speed yet -- 1.0x, same default pod_speed_permille
     * itself already starts at. */
    pod_speed_permille = pod_speed_lookup(cur_feed);
    if (pod_speed_permille <= 0) pod_speed_permille = 1000;
    audio_set_speed(pod_speed_permille);
    audio_play(pod_eps[idx].path);
    art_request(pod_eps[idx].path, "", "", "");
    was_active = 1;
    if (resume > 0 && resume != POD_FINISHED) audio_seek_ms(resume);
    pod_notes_showing = 0;
    pod_notes_scroll_px = 0;
    mlog("[music] podcast episode %s\n", pod_eps[idx].name);
}

static void load_page(void) {
    if (screen == SC_AUDIOBOOKS) return;   /* ab_rows[] is already complete, no paging */
    if (screen == SC_PODCASTS) return;     /* pod_rows[] likewise -- a folder count, not a query */
    if (!index_visible()) return;

    /* Keep a compact window around the viewport. The rows used to be fetched
     * at `scroll` on every crossed row; a 32-row page only displays about nine
     * at a time, so retaining its overlap avoids most GROUP BY/OFFSET queries
     * while keeping the same row-stepped presentation. */
    int base = scroll - (PAGE_MAX - vis_rows()) / 2;
    int max_base = total - PAGE_MAX;
    if (max_base < 0) max_base = 0;
    if (base < 0) base = 0;
    if (base > max_base) base = max_base;

    if (screen == SC_ARTISTS)
        row_n = lib_group(cur_facet, rows, PAGE_MAX, base);
    else
        row_n = lib_albums(cur_facet, cur_artist, rows, PAGE_MAX, base);
    row_base = base;
}

static int page_covers_viewport(void) {
    if (!index_visible() || row_base < 0) return 0;
    int end = scroll + vis_rows();
    if (end > total) end = total;
    return scroll >= row_base && end <= row_base + row_n;
}

static lib_row_t *row_at(int absolute) {
    if (screen == SC_AUDIOBOOKS)
        return (absolute >= 0 && absolute < ab_book_n) ? &ab_rows[absolute] : NULL;
    if (screen == SC_PODCASTS)
        return (absolute >= 0 && absolute < pod_feed_n) ? &pod_rows[absolute] : NULL;
    int local = absolute - row_base;
    return local >= 0 && local < row_n ? &rows[local] : NULL;
}

/* Scroll to an absolute pixel offset (scroll*ROW_H + scroll_px), clamped to
 * the list's range. Only re-pages the database-backed lists (ARTISTS,
 * ALBUMS), and only when the whole-row part actually changes — dragging
 * across a screenful of rows re-pages a handful of times, not once a pixel,
 * since PAGE_MAX (32) comfortably covers what fits on screen either way. */
/* Returns 1 if the visible rows actually changed. A drag no longer slides
 * rows across the screen a pixel at a time -- they stay exactly where they
 * are and simply hold different content once the finger has moved a full
 * row's worth of distance, the way turning a page does rather than
 * scrolling one. Nothing else needs to change to get this: scroll_px still
 * tracks how far into the next row the drag has gone, for continuity across
 * ticks, but the draw loop only ever sees it at 0, since dirty is now only
 * raised on the tick a row actually crosses. That is the whole fix for the
 * smoothness this was chasing — not a cheaper redraw, just far fewer of
 * them: one per 72px of travel instead of one on nearly every tick. */
/* Reported live: replaces the old inline disc-number-next-to-track-number
 * column with a full-width banner (icon, "Disc N", that disc's own total
 * playtime) inserted before each disc's first track. Height chosen to sit
 * clearly apart from a ROW_H (72) track row without dominating the list. */
#define DISC_BANNER_H 56

static int track_disc_num(int idx) {
    return tracks[idx].disc > 0 ? tracks[idx].disc : 1;
}

/* Whether this album's tracks span more than one disc -- same rule the
 * banner and the old inline number both gate on: a single-disc album (or
 * one where every track is untagged, normalising to disc 1) gets no
 * marking at all, nothing to distinguish.
 *
 * BG86: never for a playlist. Each track's `disc` field is leftover
 * metadata from whichever album it originally belonged to, not something
 * meaningful about the playlist's own ordering -- a playlist mixing tracks
 * from several discs/albums saw banners inserted between essentially
 * unrelated songs, at whatever points their carried-over disc numbers
 * happened to differ. track_index_at() and the draw loop agree with each
 * other about where those banners shift every row below them to, so there
 * was never an actual draw/tap mismatch -- but nobody browsing a playlist
 * expects disc banners to begin with, so the shift reads as "tapped one
 * row, a different track got selected." */
static int tracks_multi_disc(void) {
    if (browsing_is_playlist) return 0;
    if (track_n <= 1) return 0;
    int first = track_disc_num(0);
    for (int i = 1; i < track_n; i++)
        if (track_disc_num(i) != first) return 1;
    return 0;
}

/* Total extra height every disc banner adds -- disc 1 gets one too (same
 * convention the old inline number used: idx == 0 always counted as a
 * "disc start"), not just discs 2 onward. Needed up front by
 * tracks_max_px() the same way track_n * ROW_H already is; the draw loop
 * and track_index_at() below recompute the same banner positions their own
 * way rather than sharing this exact number, since they need to know
 * *where* each one falls, not just how many pixels they add in total. */
static int tracks_banner_px(void) {
    if (!tracks_multi_disc()) return 0;
    int n = 1;
    for (int i = 1; i < track_n; i++)
        if (track_disc_num(i) != track_disc_num(i - 1)) n++;
    return n * DISC_BANNER_H;
}

/* Content-space y (i.e. content_y = y + off, the same coordinate the draw
 * loop's own `ry` runs in) to the track index whose row contains it,
 * walking the identical banner-then-row layout the draw loop below lays
 * out -- the one place both have to agree, or a tap would land one row
 * away from what's actually drawn there, the exact class of bug BG2/BG61
 * already fixed once for scroll_px. Returns -1 for a y landing on a banner
 * itself, the header, or past the last track -- same "does nothing" rule
 * every other non-row tap target on this screen already follows. */
static int track_index_at(int content_y) {
    int multi_disc = tracks_multi_disc();
    int ry = tracks_hdr_h();
    int last_disc = -999;
    for (int idx = 0; idx < track_n; idx++) {
        int disc = track_disc_num(idx);
        if (multi_disc && disc != last_disc) {
            if (content_y >= ry && content_y < ry + DISC_BANNER_H) return -1;
            ry += DISC_BANNER_H;
            last_disc = disc;
        }
        if (content_y >= ry && content_y < ry + ROW_H) return idx;
        ry += ROW_H;
    }
    return -1;
}

/* Disc banner's own icon, same vendored-and-baked pipeline as the gear
 * icon (R51) -- compact-disc-solid-full.svg via gen_icons.py. */
static void draw_disc_icon(uint16_t *fb, int x, int y, uint16_t c) {
    draw_icon(fb, FB_W, FB_H, x, y, &icon_disc, c);
}

/* R46 follow-up: exact max scroll offset for the album-detail screen, shared
 * by scroll_to_px() and the inertia tick's spring-back -- both need the same
 * number and neither should compute it slightly differently. */
static int tracks_max_px(void) {
    int content_px = tracks_hdr_h() + track_n * ROW_H + tracks_banner_px();
    /* Reported live: reserving the 40px sheet_note margin even when there's
     * no toast to show it left a track settling with its bottom half cut
     * off at that boundary for no reason -- see vis_rows()'s matching
     * comment. Only this function/vis_rows()/the draw block's own clip_bot
     * need to agree, since this screen is the only caller of all three. */
    int bottom_margin = mini_visible() ? MINI_H : (sheet_note[0] ? 40 : 0);
    int visible_px = FB_H - bottom_margin;
    int max_px = content_px - visible_px;
    return max_px < 0 ? 0 : max_px;
}

/* Artist page's own version of the pair above -- photo/name/count header,
 * then the album list, then the bio wrapped underneath. No sheet_note
 * margin to account for: that toast only ever comes from the album page's
 * own long-press sheet, which this screen has no equivalent gesture for. */
static int artist_page_content_px(void) {
    int hdr = artist_page_hdr_h();
    int albums_px = artist_page_album_n * ROW_H;
    int bio_y = hdr + albums_px;
    int bio_h = artist_bio_layout(NULL, 0, bio_y, FB_W - 48, 0, 0, 0);
    return bio_y + bio_h;
}

static int artist_page_max_px(void) {
    int content_px = artist_page_content_px();
    int visible_px = FB_H - (mini_visible() ? MINI_H : 0);
    int max_px = content_px - visible_px;
    return max_px < 0 ? 0 : max_px;
}

static int scroll_to_px(int total_px) {
    if ((screen == SC_TRACKS && !ab_list && !pod_list) || screen == SC_ARTIST_PAGE) {
        /* R46 follow-up: exact pixel bounds, not the ceil()'d row-count
         * `limit` every other screen below uses -- rounding a fractional
         * last row up to a whole ROW_H let scroll go a full row past the
         * real end of content, which is exactly the "large gap after the
         * last track" reported live. Rubber-banded rather than hard-
         * clamped too: dragging past either end is allowed, damped to a
         * third of the actual finger travel, so it gives rather than
         * stopping dead -- the spring-back on release lives in the
         * inertia tick below, which also drives this screen since it's
         * marked "continuous" there.
         *
         * The artist page shares this exact same branch, not a parallel
         * copy -- same continuous-scroll mechanics (photo sliding with a
         * list beneath it), just artist_page_max_px() instead of
         * tracks_max_px() for where the bottom actually is. */
        int max_px = (screen == SC_ARTIST_PAGE) ? artist_page_max_px() : tracks_max_px();
        if (total_px < 0) total_px = total_px / 3;
        else if (total_px > max_px) total_px = max_px + (total_px - max_px) / 3;
        /* Floor division, not C's truncate-toward-zero -- total_px can be
         * negative during overscroll above the top, and scroll_px must stay
         * in [0, ROW_H) regardless, the same invariant every use of
         * `scroll*ROW_H + scroll_px` elsewhere already assumes. */
        int new_scroll = (total_px >= 0) ? total_px / ROW_H
                                          : -((-total_px + ROW_H - 1) / ROW_H);
        int new_px = total_px - new_scroll * ROW_H;
        int changed = new_scroll != scroll;
        scroll = new_scroll;
        scroll_px = new_px;
        return changed;
    }
    int limit = (screen == SC_TRACKS) ? track_n :
                (screen == SC_QUEUE)  ? queue_n :
                (screen == SC_RADIO)  ? station_n :
                (screen == SC_PLAYLISTS) ? playlist_n + 1 :   /* +1: R71's own "New Playlist" row */
                (screen == SC_SETTINGS) ? settings_content_rows() :
                (screen == SC_SETTINGS_WIFI) ? wifi_row_n() :   /* R75 */
                (screen == SC_SETTINGS_BT) ? bt_row_n() :       /* BG109 */
                (screen == SC_SETTINGS_TIMEZONE) ? TZ_N : total;
    int max_top = limit - vis_rows();
    if (max_top < 0) max_top = 0;
    int max_px = max_top * ROW_H;
    if (total_px < 0) total_px = 0;
    if (total_px > max_px) total_px = max_px;
    int new_scroll = total_px / ROW_H;
    int new_px     = total_px % ROW_H;
    int changed = new_scroll != scroll;
    if (changed) {
        scroll = new_scroll;
        if (index_visible() && !page_covers_viewport()) load_page();
    }
    scroll_px = new_px;
    return changed;
}

/* Right-aligned secondary figure: album count, track count, duration.
 *
 * Anchored inside the A-Z strip where one is showing. Anchoring to the screen
 * edge put these digits underneath the letters — which is what "the numbers
 * overlap the quick scroll letters" meant, and removing the numbers was the
 * wrong reading of it. */
static void draw_right(uint16_t *fb, int y, const char *s) {
    int w = text_width(s, TEXT_PX_SMALL);
    int right = FB_W - 24 - (index_visible() ? INDEX_W : 0);
    draw_text(fb, right - w, y, s, COL_DIM, TEXT_PX_SMALL, FB_W);
}

/* Same as draw_right(), but lets the caller pick a colour instead of the
 * fixed COL_DIM -- used for values that should read as accented/interactive
 * rather than plain informational text (BG63). */
static void draw_right_col(uint16_t *fb, int y, const char *s, uint16_t col) {
    int w = text_width(s, TEXT_PX_SMALL);
    int right = FB_W - 24 - (index_visible() ? INDEX_W : 0);
    draw_text(fb, right - w, y, s, col, TEXT_PX_SMALL, FB_W);
}

static void draw_right_clip(uint16_t *fb, int y, const char *s, int clip_top, int clip_bot) {
    int w = text_width(s, TEXT_PX_SMALL);
    int right = FB_W - 24 - (index_visible() ? INDEX_W : 0);
    draw_text_clip(fb, right - w, y, s, COL_DIM, TEXT_PX_SMALL, FB_W, clip_top, clip_bot);
}

/* R44: draw_right_col(), clipped -- Settings scrolling needs this the same
 * way the library lists already needed draw_right_clip() above. */
static void draw_right_col_clip(uint16_t *fb, int y, const char *s, uint16_t col,
                                 int clip_top, int clip_bot) {
    int w = text_width(s, TEXT_PX_SMALL);
    int right = FB_W - 24 - (index_visible() ? INDEX_W : 0);
    draw_text_clip(fb, right - w, y, s, col, TEXT_PX_SMALL, FB_W, clip_top, clip_bot);
}

/* Every route into playback goes through here: tapping a track, and the two
 * transport buttons. Keeping it in one place is what stops the artwork request
 * from being dropped on one path and not the others. */
#define QUEUE_MAX ((int)(sizeof(queue) / sizeof(queue[0])))

static void queue_follower(void);

/* Insert a track into the queue. at < 0 appends. If nothing is playing the
 * queue is started from this album first, so "add to queue" from a cold start
 * does something sensible rather than nothing. */
static void queue_insert(int track_idx, int at) {
    if (track_idx < 0 || track_idx >= track_n) return;
    if (queue_n == 0) {
        snprintf(q_artist, sizeof(q_artist), "%s", cur_artist);
        snprintf(q_album,  sizeof(q_album),  "%s", cur_album);
        q_is_playlist = browsing_is_playlist;
    }
    if (queue_n >= QUEUE_MAX) return;
    if (at < 0 || at > queue_n) at = queue_n;
    memmove(&queue[at + 1], &queue[at],
            sizeof(queue[0]) * (size_t)(queue_n - at));
    queue[at] = tracks[track_idx];
    queue_n++;
    if (at <= cur_track && queue_n > 1) cur_track++;   /* keep pointing at the same song */
    /* Reported live: "Add to queue" (not just Play Next) from a different
     * album while one was playing had the same wrong-album-page-on-swipe-
     * back problem BG73 fixed for Play Next -- queue_insert() is the one
     * place both funnel through, so it's the one place that has to know
     * the queue stopped being a single clean album, not the caller. Skipped
     * for a playlist queue, same reasoning as queue_play_next()'s own
     * comment: a playlist was already an arbitrary mix of albums/artists
     * from the start, adding one more track to it isn't a new state. */
    if (!q_is_playlist) queue_mixed = 1;
    queue_follower();                                  /* what comes next may have changed */
    mlog("[music] queued %s at %d\n", queue[at].name, at);
}

/* BG73: "Play next" specifically, as opposed to plain queue_insert()'s
 * splice-and-shift ("add to queue"). Reported live: queuing a track from a
 * different album while one was already playing left the old album's own
 * remaining tracks sitting in the queue behind it, and q_artist/q_album
 * (the queue's own recorded identity, used e.g. when swiping back from Now
 * Playing to a track's album) never updated to the new track's album either
 * -- both because queue_insert() only ever sets them from a cold start
 * (queue_n == 0), not on every insert. An album's remaining tracks are only
 * queued because that's what starting playback does, not because the user
 * chose them, so a deliberate Play Next should displace them, not merely
 * delay them behind one more track. A playlist is different -- its order
 * was actually chosen -- so Play Next there still just slots the new track
 * in and leaves the rest of the playlist to resume after it, same as
 * queue_insert() already does for everyone. */
static void queue_play_next(int track_idx) {
    if (track_idx < 0 || track_idx >= track_n) return;
    if (queue_n > 0 && !q_is_playlist && cur_track + 1 < queue_n) {
        queue_n = cur_track + 1;      /* drop the old album's leftovers */
        /* queue_mixed is set below, inside queue_insert() -- true either
         * way once this truncation has happened, so no need to duplicate
         * it here. */
    }
    if (queue_n == 0) {
        /* Nothing playing yet -- no "currently playing album" to keep
         * showing, so this is safe (and needed: queue_insert() below only
         * sets q_artist/q_album from a cold start, same as it always did). */
        snprintf(q_artist, sizeof(q_artist), "%s", cur_artist);
        snprintf(q_album,  sizeof(q_album),  "%s", cur_album);
        q_is_playlist = browsing_is_playlist;
    } else if (!q_is_playlist) {
        /* BG85: defer -- see queue_apply_pending()'s own comment. The new
         * track lands right after cur_track (queue_insert() below), so
         * that's exactly the index playback has to reach for this to
         * become true. */
        snprintf(q_artist_pending, sizeof(q_artist_pending), "%s", cur_artist);
        snprintf(q_album_pending,  sizeof(q_album_pending),  "%s", cur_album);
        q_pending_at = cur_track + 1;
    }
    queue_insert(track_idx, cur_track + 1);
}

/* Whether queueing what's currently being *browsed* into the queue that's
 * currently *playing* would mix two kinds that have no business sharing one
 * queue. queue[] is deliberately one array for everything (podcast episodes,
 * book chapters and music tracks are all just lib_track_t paths by the time
 * they reach it), which is what let R58 give podcasts real queueing for free
 * -- but "the queue is generic" is an implementation detail, not a promise
 * that a podcast episode belongs in the middle of an album.
 *
 * The three kinds genuinely don't interchange: a book's queue *is* its
 * chapter table (ab_follow()/ab_save_current_pos() index straight into
 * ab_book.chap[] by cur_track, so a foreign entry spliced in desynchronises
 * both), podcast_mode drives an entirely different Now Playing transport
 * and its own resume-position store, and each mode's end-of-queue behaviour
 * differs. Nothing crashes on a mix -- it just plays as an entry the
 * surrounding UI is describing wrongly, which is worse than refusing.
 *
 * Radio isn't listed: it has no queue at all (queue_follower() hands the
 * worker NULL outright while radio_mode), so there's nothing to mix into --
 * the guard below treats a radio queue as empty and lets the insert start a
 * fresh one, same as any other cold start. */
static int queue_kind_conflict(void) {
    if (queue_n == 0 || !audio_is_active()) return 0;   /* nothing to mix with */
    int browsing_pod = pod_list, playing_pod = podcast_mode;
    if (audiobook_mode) return 1;      /* a book's queue takes no foreign entries at all */
    return browsing_pod != playing_pod;
}

/* Call after every cur_track update that could be crossing into a
 * queue_play_next() track -- play_index() and the natural gapless-advance
 * path both need this, since either can be how playback actually reaches
 * it. A no-op whenever nothing is pending, or the newly-current track
 * hasn't reached it yet. */
static void queue_apply_pending(void) {
    if (q_pending_at < 0 || cur_track < q_pending_at) return;
    snprintf(q_artist, sizeof(q_artist), "%s", q_artist_pending);
    snprintf(q_album,  sizeof(q_album),  "%s", q_album_pending);
    q_pending_at = -1;
}

static void play_station(int i) {
    if (i < 0 || i >= station_n) return;
    radio_msg[0] = '\0';
    if (!stations[i].url[0]) {
        snprintf(radio_msg, sizeof(radio_msg), "%s has no URL yet", stations[i].name);
        return;
    }
    if (!st_net_up()) {
        snprintf(radio_msg, sizeof(radio_msg), "Wi-Fi is off");
        return;
    }
    radio_mode = 1;
    audiobook_mode = 0;
    podcast_mode = 0;
    audio_set_speed(1000);           /* a stream has no WSOLA use for it */
    snprintf(radio_name, sizeof(radio_name), "%s", stations[i].name);
    art_request("", "", "", "");         /* clears whatever art was showing */
    audio_play(stations[i].url);
    was_active = 1;
    mlog("[music] station %s\n", stations[i].name);
}

/* R47: shuffle/repeat, music only -- audiobooks and podcasts have their own
 * next/prev logic already (chapter rollover, the fixed +/-30s skip), and
 * shuffling a book or looping one podcast episode forever isn't what either
 * mode's own transport is for. */
#define REPEAT_OFF 0
#define REPEAT_ALL 1
#define REPEAT_ONE 2
static int shuffle_enabled;
static int repeat_mode;
static int shuffle_order[QUEUE_MAX];
static int shuffle_n;    /* == queue_n as of the last regenerate() */

/* Fisher-Yates over [0, queue_n), then cur_track's entry is swapped to the
 * front: toggling shuffle on mid-album must not itself jump to a different
 * track, only randomize what comes after the one already playing. Called
 * lazily (shuffle_n != queue_n) rather than at every queue change, so a
 * shuffle that's off costs nothing. */
static void shuffle_regenerate(void) {
    shuffle_n = queue_n;
    for (int i = 0; i < shuffle_n; i++) shuffle_order[i] = i;
    for (int i = shuffle_n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = shuffle_order[i]; shuffle_order[i] = shuffle_order[j]; shuffle_order[j] = t;
    }
    for (int i = 0; i < shuffle_n; i++) {
        if (shuffle_order[i] == cur_track) {
            int t = shuffle_order[0]; shuffle_order[0] = shuffle_order[i]; shuffle_order[i] = t;
            break;
        }
    }
}

static int shuffle_find_pos(int idx) {
    for (int i = 0; i < shuffle_n; i++) if (shuffle_order[i] == idx) return i;
    return -1;
}

/* R70: moves the track at display position `from` to display position `to`,
 * shifting whatever's between them -- shuffle_order[] when shuffle's on
 * (same branch queue_display_index() below uses, so this always reorders
 * whatever the screen is actually showing), queue[] directly otherwise.
 * queue[] is also cur_track's own index space, so a direct reorder there
 * has to carry cur_track along with whichever physical slot the playing
 * track ends up in -- tracked by path rather than by hand-rolling the
 * index arithmetic a shift-by-one would need, since a wrong index here
 * plays the wrong track outright rather than just displaying one. */
static void queue_move_display(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= queue_n || to >= queue_n) return;
    if (shuffle_enabled) {
        if (shuffle_n != queue_n) shuffle_regenerate();
        int v = shuffle_order[from];
        if (from < to) for (int i = from; i < to; i++) shuffle_order[i] = shuffle_order[i + 1];
        else            for (int i = from; i > to; i--) shuffle_order[i] = shuffle_order[i - 1];
        shuffle_order[to] = v;
    } else {
        char playing_path[LIB_PATH_LEN];
        int had_playing = cur_track >= 0 && cur_track < queue_n;
        if (had_playing) snprintf(playing_path, sizeof(playing_path), "%s", queue[cur_track].path);
        lib_track_t v = queue[from];
        if (from < to) for (int i = from; i < to; i++) queue[i] = queue[i + 1];
        else            for (int i = from; i > to; i--) queue[i] = queue[i - 1];
        queue[to] = v;
        if (had_playing)
            for (int i = 0; i < queue_n; i++)
                if (!strcmp(queue[i].path, playing_path)) { cur_track = i; break; }
    }
}

/* -1 means "nothing plays next" -- callers already treat that as a no-op,
 * since play_index() itself guards i<0. */
static int next_track_index(void) {
    if (queue_n == 0) return -1;
    /* Shuffle/repeat only ever change this for plain music. Audiobooks,
     * podcasts and radio fall straight through to the plain sequential step
     * below, unchanged from before this existed -- returning -1 here for
     * them would have broken the Now Playing/mini-player transport zones,
     * which reuse this same helper for all four modes (the hardware keys
     * have their own explicit audiobook/podcast branches instead, but these
     * two on-screen zones never did, relying on the mirrored queue[]
     * ab_play_chapter's own comment describes). */
    if (!audiobook_mode && !podcast_mode && !radio_mode) {
        if (repeat_mode == REPEAT_ONE) return cur_track;
        if (shuffle_enabled) {
            if (shuffle_n != queue_n) shuffle_regenerate();
            int pos = shuffle_find_pos(cur_track);
            if (pos < 0) return -1;
            int nxt = pos + 1;
            if (nxt >= shuffle_n) return (repeat_mode == REPEAT_ALL) ? shuffle_order[0] : -1;
            return shuffle_order[nxt];
        }
        if (repeat_mode == REPEAT_ALL) {
            int nxt = cur_track + 1;
            return (nxt >= queue_n) ? 0 : nxt;
        }
    }
    int nxt = cur_track + 1;
    return (nxt < queue_n) ? nxt : -1;
}

static int prev_track_index(void) {
    if (queue_n == 0) return -1;
    if (!audiobook_mode && !podcast_mode && !radio_mode && shuffle_enabled) {
        if (shuffle_n != queue_n) shuffle_regenerate();
        int pos = shuffle_find_pos(cur_track);
        if (pos < 0) return -1;
        int prv = pos - 1;
        if (prv < 0) return (repeat_mode == REPEAT_ALL) ? shuffle_order[shuffle_n - 1] : -1;
        return shuffle_order[prv];
    }
    int prv = cur_track - 1;
    if (prv < 0) {
        if (!audiobook_mode && !podcast_mode && !radio_mode && repeat_mode == REPEAT_ALL)
            return queue_n - 1;
        return -1;
    }
    return prv;
}

/* BG71: the queue view's row order -- shuffle_order[] when shuffle's on
 * (already a permutation of [0,queue_n) with cur_track anchored at
 * position 0, exactly what "actual play order" means), plain array order
 * otherwise. Lazily regenerated the same way next_track_index() already
 * does, so opening the queue view right after toggling shuffle still
 * reflects it correctly. */
static int queue_display_index(int display_i) {
    if (shuffle_enabled) {
        if (shuffle_n != queue_n) shuffle_regenerate();
        if (display_i >= 0 && display_i < shuffle_n) return shuffle_order[display_i];
        return -1;
    }
    return display_i;
}

/* Hand the worker the track after this one so it can roll straight into it. */
static void queue_follower(void) {
    int nxt = next_track_index();
    audio_set_next((!radio_mode && nxt >= 0) ? queue[nxt].path : NULL);
}

static void play_index(int i);

/* R70: swipe-to-remove. Physically deletes from queue[] regardless of
 * shuffle -- shuffle_order[] only ever holds *display* order for a queue[]
 * that still exists, so a real removal has to shrink queue[] itself either
 * way. That alone leaves shuffle_n one behind the new (smaller) queue_n,
 * which queue_display_index()'s existing lazy check already treats as
 * "regenerate" -- the same mechanism next_track_index() relies on, not a
 * separate fix needed here.
 *
 * Removing the row that's actually playing needed a decision (see R70's own
 * open question in BACKLOG.md): picked auto-advance, closing the gap and
 * continuing with whatever now sits in that same display slot -- the
 * convention every mainstream player already uses, and the one place a
 * repeat-one edge case gets simplified deliberately: this is "the current
 * entry is gone", not "the current entry finished", so it always moves on
 * by one slot rather than consulting repeat_mode/next_track_index() (which
 * would hand back the very index just removed under repeat-one). */
static void queue_remove_display(int display_i) {
    if (display_i < 0 || display_i >= queue_n) return;
    int idx = queue_display_index(display_i);
    if (idx < 0) return;
    int removing_playing = audio_is_active() && idx == cur_track;
    char keep_path[LIB_PATH_LEN];
    keep_path[0] = '\0';
    if (!removing_playing && cur_track >= 0 && cur_track < queue_n)
        snprintf(keep_path, sizeof(keep_path), "%s", queue[cur_track].path);

    for (int i = idx; i < queue_n - 1; i++) queue[i] = queue[i + 1];
    queue_n--;

    if (removing_playing) {
        if (queue_n == 0) {
            audio_stop();
            cur_track = -1;
        } else {
            int new_display_i = display_i < queue_n ? display_i : queue_n - 1;
            int new_idx = queue_display_index(new_display_i);
            if (new_idx < 0 || new_idx >= queue_n) new_idx = 0;
            play_index(new_idx);
        }
    } else if (keep_path[0]) {
        for (int i = 0; i < queue_n; i++)
            if (!strcmp(queue[i].path, keep_path)) { cur_track = i; break; }
    }
}

/* R29: waveform seek bar, Music only (per its own backlog entry --
 * audiobooks/podcasts explicitly excluded). Built from real playback, not
 * a separate offline decode pass: the first time a track plays, its
 * amplitude envelope is sampled from the exact PCM already being decoded
 * for output (audio_current_peak(), polled alongside audio_pos_ms() in the
 * main loop) and cached to the card; every playthrough after that loads
 * the cache instead of capturing again -- "appears on every playthrough
 * after the first", requested live, literally. */
/* R86 follow-up: reported live as uneven bar thickness, after the previous
 * fix already fixed uneven gaps -- 120 doesn't divide the available 432px
 * (FB_W-48) evenly (432/120 = 3.6), so no integer-pixel column width can
 * be perfectly uniform: some columns end up 3px, others 4px, no matter how
 * the remainder is distributed. 144 is the closest bucket count to 120
 * that divides 432 exactly (3px per column, zero remainder) -- every
 * column is now genuinely, not just approximately, the same width. Also
 * invalidates every existing cache file (their on-disk length no longer
 * matches WAVE_BUCKETS, so wave_load()'s exact-size check simply misses
 * and the next play recaptures) -- harmless and self-healing, not
 * something that needed its own separate migration. */
#define WAVE_BUCKETS 144
#define WAVE_CACHE_DIR "/data/mnt/sd_0/.music_waveforms"
/* Bump on any change to what a cache file actually contains, so old files
 * from before the change become unreachable orphans rather than being
 * misread -- same discipline cover.c's own CACHE_VERSION documents. */
#define WAVE_CACHE_VERSION "1"

static uint8_t  wave_buckets[WAVE_BUCKETS];   /* currently displayed, valid only if wave_loaded */
static int      wave_loaded;
static int      wave_capturing;
static uint32_t wave_capture[WAVE_BUCKETS];   /* raw running peak per bucket, this playthrough */
static uint8_t  wave_touched[WAVE_BUCKETS];   /* 1 once a bucket has been sampled at all, separate
                                                  from wave_capture[]'s value -- real silence also
                                                  writes 0, so the peak alone can't tell "sampled,
                                                  quiet" from "never sampled". */
static char     wave_capture_path[LIB_PATH_LEN];
static int      wave_last_bucket;             /* highest bucket touched so far, -1 = none yet */

/* Same djb2-and-hex-name shape as cover.c's own cache_path() -- a track
 * path can contain anything the filesystem allows, so hashing it into a
 * fixed-width hex name sidesteps sanitizing it into something legal. */
static void wave_cache_path(const char *track_path, char *out, size_t n) {
    unsigned long h = 5381;
    for (const unsigned char *p = (const unsigned char *)track_path; *p; p++)
        h = ((h << 5) + h) ^ *p;
    for (const unsigned char *p = (const unsigned char *)WAVE_CACHE_VERSION; *p; p++)
        h = ((h << 5) + h) ^ *p;
    mkdir(WAVE_CACHE_DIR, 0755);
    snprintf(out, n, "%s/%08lx.wave", WAVE_CACHE_DIR, h & 0xFFFFFFFFul);
}

/* 1 and out filled if a fresh cache exists, 0 otherwise (including a stale
 * one -- older than the track file itself, same rule cover.c's own
 * load_cache() uses for a replaced cover.jpg -- which is deleted so a
 * later capture doesn't collide with it under the same hash). */
static int wave_load(const char *track_path, uint8_t *out) {
    char p[512];
    wave_cache_path(track_path, p, sizeof(p));
    struct stat cs, ts;
    if (stat(p, &cs) != 0) return 0;
    if (stat(track_path, &ts) == 0 && ts.st_mtime > cs.st_mtime) {
        unlink(p);
        return 0;
    }
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t got = fread(out, 1, WAVE_BUCKETS, f);
    fclose(f);
    return got == WAVE_BUCKETS;
}

static void wave_save(const char *track_path, const uint8_t *buckets) {
    char p[512], tmp[520];
    wave_cache_path(track_path, p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    int ok = fwrite(buckets, 1, WAVE_BUCKETS, f) == WAVE_BUCKETS;
    fclose(f);
    if (!ok || rename(tmp, p) != 0) unlink(tmp);
}

/* Called every time play_index() is about to move on to a different track
 * -- whichever track was capturing (if any) just had its playthrough end,
 * whether by reaching the end, being skipped, or the queue advancing. */
static void wave_finish_capture(void) {
    if (!wave_capturing) return;
    wave_capturing = 0;
    /* A capture that never got near the end (an early skip, or a track
     * abandoned for the session) is not "a play through" -- saving it
     * would cache a misleadingly short/empty shape that never gets a
     * chance to be completed later, since a cache file existing at all is
     * what stops a future play from capturing again. */
    if (wave_last_bucket < WAVE_BUCKETS - 4) return;
    /* Reaching a late bucket is not the same as having sampled every bucket
     * along the way: a forward seek, or resuming past a stretch only ever
     * partly sampled earlier, jumps straight to a late bucket without ever
     * touching the ones in between, which still satisfies the check above.
     * Reported live: a Bruckner movement's cached waveform had real data for
     * the first few seconds, then 88 of 144 buckets flat zero, then real
     * data again from ~63% onward -- exactly that shape, from a mid-track
     * seek. Require near-total coverage of 0..wave_last_bucket, not just
     * that the far end was reached. */
    int touched = 0;
    for (int i = 0; i <= wave_last_bucket; i++) if (wave_touched[i]) touched++;
    if (touched < wave_last_bucket + 1 - 4) return;
    uint32_t maxv = 0;
    for (int i = 0; i < WAVE_BUCKETS; i++) if (wave_capture[i] > maxv) maxv = wave_capture[i];
    if (maxv == 0) return;   /* silence throughout, e.g. output was lost -- nothing real to show */
    uint8_t out[WAVE_BUCKETS];
    for (int i = 0; i < WAVE_BUCKETS; i++)
        out[i] = (uint8_t)(wave_capture[i] * 255 / maxv);
    wave_save(wave_capture_path, out);
}

/* Everything a track change needs to do to wave_* state, regardless of
 * whether it arrived via play_index() (a tap, a skip button, a playlist
 * pick) or the gapless worker rolling on by itself at a track boundary.
 * R86 follow-up: the gapless path used to skip this entirely, since it
 * never went through play_index() -- cur_track moved on but wave_capturing/
 * wave_capture[]/wave_capture_path kept pointing at the track that just
 * ended, so peaks sampled during the new track were written into the old
 * track's buffer at the old track's bucket math. Reported live as the
 * waveform "often doesn't appear" after the first track in a queue, and
 * once as a track showing a flat line of dots -- a capture blended from
 * two different tracks' position/duration mapping normalizes to almost
 * nothing. Music only, same as play_index()'s own gate: podcast_mode is
 * checked explicitly since this helper runs before podcast_mode is known
 * to be off for a caller that isn't play_index(). */
static void wave_track_changed(const char *path) {
    wave_finish_capture();
    wave_loaded = 0;
    wave_capturing = 0;
    if (!podcast_mode) {
        if (wave_load(path, wave_buckets)) {
            wave_loaded = 1;
        } else {
            wave_capturing = 1;
            memset(wave_capture, 0, sizeof(wave_capture));
            memset(wave_touched, 0, sizeof(wave_touched));
            snprintf(wave_capture_path, sizeof(wave_capture_path), "%s", path);
            wave_last_bucket = -1;
        }
    }
}

static void play_index(int i) {
    radio_mode = 0;
    audiobook_mode = 0;
    /* R58: NOT podcast_mode = 0 here any more. This is the generic
     * "advance within queue[]" function -- Next/Prev, a tap in SC_QUEUE,
     * and the natural end-of-track fallback restart all funnel through it
     * -- and now that a podcast episode can have a real next-queued
     * episode (queue_play_next()/queue_insert(), reachable from the
     * episode list's own long-press sheet same as music), those same
     * generic callers are exactly how playback advances within a podcast
     * queue too. Forcing podcast_mode off here would silently drop back
     * into the plain-music Now Playing UI/end-of-queue behaviour the
     * moment a queued episode's turn came up. The one place that
     * legitimately starts something that ISN'T a podcast -- a fresh
     * browse from Albums/Playlists -- sets podcast_mode = 0 itself
     * (play_from_list()), same as it already owns queue_mixed/
     * q_pending_at's fresh-start reset. */
    if (i < 0 || i >= queue_n) return;
    cur_track = i;
    queue_apply_pending();   /* BG85 */
    /* REVERTED, 2026-09-10: this briefly used audio_skip_to() to avoid the
     * 250-400 ms Bluetooth dropout at every track change (R104). Reported
     * live within the hour: tracks that would not start until tapped several
     * times, and a skip that left the previous track playing. The handover
     * flag is only read inside the `got > 0` branch of worker(), so any
     * moment the worker is not actively decoding -- paused, output lost, or
     * already between tracks -- swallows the request silently, with nothing
     * logged and no fallback. Correctness first: back on audio_play() until
     * the handover is driven from a point in the loop that always runs. */
    audio_play(queue[i].path);
    /* R29: finish whatever the previous track's own capture was, then
     * decide what this one shows -- a cache if one already exists (every
     * playthrough after the one that built it), nothing yet while this
     * playthrough builds it fresh. podcast_mode is checked explicitly
     * (radio_mode/audiobook_mode are already known 0, set just above) --
     * this feature is Music only, per its own backlog entry. */
    wave_track_changed(queue[i].path);
    /* R23: the track's own artist wins when it has one, same reasoning
     * Now Playing's display already uses (BG30) -- q_artist is the
     * album's artist and can genuinely differ per track (a compilation).
     * Last.fm matches on the album though, so q_album, not the track's
     * own title, is always the right second half of the pair. */
    art_request(queue[i].path, queue[i].artist[0] ? queue[i].artist : q_artist, q_album, q_artist);
    queue_follower();
    was_active = 1;
    /* R30's "recently heard" side. q_album is the queue's album, set by
     * play_from_list() before this ever runs -- empty here means the queue
     * came from somewhere that never set it (radio, before that guard was
     * added, say), not a real album to record. Marked on every track start,
     * not just the first: an album played track-by-track over an evening
     * should read as heard "now", not at whatever moment side one began. */
    if (q_album[0]) recent_heard_mark(q_album);
    mlog("[music] play %s\n", queue[i].path);
}

/* Starting a track from the browser makes that album the queue. */
static void play_from_list(int idx) {
    if (idx < 0 || idx >= track_n) return;
    memcpy(queue, tracks, sizeof(queue[0]) * (size_t)track_n);
    queue_n = track_n;
    snprintf(q_artist, sizeof(q_artist), "%s", cur_artist);
    snprintf(q_album,  sizeof(q_album),  "%s", cur_album);
    q_is_playlist = browsing_is_playlist;    /* BG73 */
    queue_mixed = 0;                         /* BG73 follow-up: a fresh queue is always clean */
    q_pending_at = -1;                       /* BG85: a fresh queue has nothing pending */
    podcast_mode = 0;                        /* R58: play_index() no longer clears this itself */
    play_index(idx);
}

/* ---- audiobooks ---------------------------------------------------------- *
 * A chapter is a stretch of a file rather than a file, so these three take
 * the place of play_from_list/play_index/the advance handler. Everything
 * else — the chapter list, the mini player, the hardware keys — goes on
 * using tracks[]/queue[], which is why the chapters are mirrored into them. */

static void ab_load_book(const ab_book_t *b) {
    ab_open_book(b->dir, &ab_book);
    int max = (int)(sizeof(tracks) / sizeof(tracks[0]));
    track_n = 0;
    for (int i = 0; i < ab_book.chap_n && track_n < max; i++) {
        lib_track_t *t = &tracks[track_n];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", ab_book.chap[i].title);
        snprintf(t->path, sizeof(t->path), "%s", ab_book.files[ab_book.chap[i].file]);
        t->dur_ms = (int)ab_book.chap[i].dur_ms;
        t->track = -1;
        t->disc  = -1;
        track_n++;
    }
    snprintf(cur_album, sizeof(cur_album), "%s", b->title);
    cur_artist[0] = '\0';
    ab_list = 1;
}

/* The next file after the one chapter `i` lives in, for gapless roll-over.
 * Chapters inside a single file need nothing queued — the decoder never
 * reaches the end of the file at a chapter boundary. */
static const char *ab_next_file(int i) {
    if (i < 0 || i >= ab_book.chap_n) return NULL;
    int f = ab_book.chap[i].file;
    for (int j = i + 1; j < ab_book.chap_n; j++)
        if (ab_book.chap[j].file != f) return ab_book.files[ab_book.chap[j].file];
    return NULL;
}

static void ab_play_chapter(int i) {
    if (i < 0 || i >= ab_book.chap_n || i >= track_n) return;
    audiobook_mode = 1;
    radio_mode = 0;
    podcast_mode = 0;
    if (queue_n != track_n) {
        memcpy(queue, tracks, sizeof(queue[0]) * (size_t)track_n);
        queue_n = track_n;
        snprintf(q_album, sizeof(q_album), "%s", cur_album);
        q_artist[0] = '\0';
        q_pending_at = -1;   /* BG85: audiobooks never go through queue_play_next() */
    }
    cur_track = i;
    const char *path = ab_book.files[ab_book.chap[i].file];
    /* Chapters of one file share a decoder: moving between them is a seek,
     * not a reopen, which is what keeps a tap on chapter 12 of an eleven-hour
     * book instant rather than a fresh parse of the whole moov. */
    if (strcmp(ab_playing, path) != 0 || !audio_is_active()) {
        snprintf(ab_playing, sizeof(ab_playing), "%s", path);
        audio_play(path);
        art_request(path, "", "", "");
    }
    audio_seek_ms((int)ab_book.chap[i].file_start_ms);
    audio_set_next(ab_next_file(i));
    was_active = 1;
    mlog("[music] chapter %d/%d %s at %lldms\n", i + 1, ab_book.chap_n,
         ab_book.chap[i].title, (long long)ab_book.chap[i].file_start_ms);
}

/* Write the position playing right now, if any -- called wherever a book
 * might stop being the thing on screen: leaving the player, switching to a
 * different book, and app shutdown. Also called on a timer (see the main
 * loop) so a crash or a battery pull loses at most a few seconds, not the
 * whole session. A no-op whenever there is nothing to save, so callers do
 * not need to guard it themselves. */
static void ab_save_current_pos(void) {
    /* audio_is_active(), not just audiobook_mode: audio_stop() zeroes
     * g_pos_ms on the way out, and audiobook_mode/cur_track/ab_book are
     * static -- they carry the previous session's values into a fresh
     * music_entry() until something new plays. Without this guard, the very
     * first save call of a new session (the book-tap handler's "save
     * whatever was playing before switching") reads that stale zero and
     * overwrites a real saved position with 0 before ab_resume_book() ever
     * gets to read it back. Caught by hand: exited mid-chapter-5, reopened
     * the book, and it silently came back at 0:00 -- this is why. */
    if (!audiobook_mode || !audio_is_active()) return;
    if (cur_track < 0 || cur_track >= ab_book.chap_n) return;
    /* Same latent bug pod_save_current_pos() just got fixed for (see its
     * own comment): audio_pos_ms() alone can't tell "really at the start"
     * apart from "hasn't caught up to its own pending resume seek yet".
     * Never actually observed here (this guard's own history above is a
     * different stale-value bug), but the mechanism is identical, so
     * fixed the same way rather than waiting to catch it by hand twice. */
    int pending = audio_seek_pending_ms();
    ab_save_position(&ab_book, ab_book.chap[cur_track].file,
                     pending >= 0 ? pending : audio_pos_ms());
}

/* The chapter a saved (file, ms) position falls inside -- same shape as
 * ab_follow()'s search, but starting cold from a resume rather than from
 * wherever cur_track already was. */
static int ab_chapter_for(int file_idx, long file_ms) {
    int best = 0;
    for (int i = 0; i < ab_book.chap_n; i++)
        if (ab_book.chap[i].file == file_idx && ab_book.chap[i].file_start_ms <= file_ms)
            best = i;
    return best;
}

/* Start the book where it was left, if it ever has been. play_chapter() also
 * covers a book with no saved position -- chapter 0 falls out of the same
 * (file=0, ms=0) shape the "never played" case already returns. */
static void ab_resume_book(void) {
    int fi = 0; long ms = 0;
    ab_load_position(ab_book.dir, &fi, &ms);
    if (fi >= ab_book.file_n) fi = 0;
    int i = ab_chapter_for(fi, ms);
    ab_play_chapter(i);
    /* play_chapter() already seeks to the chapter's own start; a resume
     * partway through that chapter corrects it by the difference. */
    if (ms > ab_book.chap[i].file_start_ms) audio_seek_ms((int)ms);
    mlog("[music] resumed %s at file %d, %ldms\n", ab_book.dir, fi, ms);
}

/* Average bitrate of the file actually on disk -- an AAC file states no
 * nominal rate the way a FLAC header does, so this is size*8/duration, the
 * same arithmetic VBR bitrate has always meant. Cached on the path: a stat()
 * per frame is cheap enough here, but there is no reason to pay it thirty
 * times a second when the file has not changed. */
static char ab_bitrate_path[AB_PATH_LEN];
static int  ab_bitrate_kbps;

static int ab_file_bitrate_kbps(const char *path, int64_t file_dur_ms) {
    if (strcmp(path, ab_bitrate_path) != 0) {
        struct stat st;
        ab_bitrate_kbps = (file_dur_ms > 0 && stat(path, &st) == 0)
                        ? (int)((int64_t)st.st_size * 8 / file_dur_ms) : 0;
        snprintf(ab_bitrate_path, sizeof(ab_bitrate_path), "%s", path);
    }
    return ab_bitrate_kbps;
}

/* BG106: same cache-on-path shape as ab_file_bitrate_kbps() just above --
 * audio_mp3_is_vbr() is cheap, but the Now Playing screen redraws often
 * enough (the once-a-second clock tick alone) that there is no reason to
 * re-read the file every time when it hasn't changed. */
static char track_vbr_path[LIB_PATH_LEN];
static int  track_vbr_cached;

static int track_is_vbr_mp3(const char *path) {
    if (strcmp(path, track_vbr_path) != 0) {
        const char *dot = strrchr(path, '.');
        track_vbr_cached = (dot && !strcasecmp(dot, ".mp3")) ? audio_mp3_is_vbr(path) : 0;
        snprintf(track_vbr_path, sizeof(track_vbr_path), "%s", path);
    }
    return track_vbr_cached;
}

/* Which chapter the playing position is now inside. Derived rather than
 * remembered, so simply listening across a boundary moves the display on
 * without anything having to notice the crossing. */
static int ab_follow(void) {
    if (!audiobook_mode || ab_book.chap_n <= 0) return 0;
    int c = cur_track;
    if (c < 0 || c >= ab_book.chap_n) return 0;
    /* BG80 pattern, same as everywhere else in this file that reads a
     * position: a resume seek deep into a book (R66's cold-start restore is
     * the newest way to trigger this, but ab_resume_book() has always done
     * this on a plain "open the book" too) leaves audio_pos_ms() reading
     * near 0 for however long the worker thread takes to land the seek --
     * the same 10-20s-worst-case transition window this pattern exists for
     * elsewhere. Reading that raw here walked the second loop below all the
     * way back to chapter 0 every time, since 0 is "before" every chapter's
     * own start -- reported live as "Opening Credits" flashing on screen
     * for a moment right after a resume, before the seek actually landed
     * and this corrected itself back to the real chapter. */
    int pending = audio_seek_pending_ms();
    int64_t pos = pending >= 0 ? pending : audio_pos_ms();
    int f = ab_book.chap[c].file;
    while (c + 1 < ab_book.chap_n && ab_book.chap[c + 1].file == f &&
           pos >= ab_book.chap[c + 1].file_start_ms) c++;
    while (c > 0 && ab_book.chap[c - 1].file == f &&
           pos < ab_book.chap[c].file_start_ms) c--;
    if (c == cur_track) return 0;
    cur_track = c;
    return 1;
}

/* ---- parametric EQ -------------------------------------------------------- */

/* True once eq_cur_path already points at a fork this app made -- nothing
 * left to protect there, so further edits just save it in place. */
static int eq_is_custom_path(const char *path) {
    static const char suf[] = " (Custom).txt";
    size_t n = strlen(path), sn = sizeof(suf) - 1;
    return n >= sn && strcmp(path + n - sn, suf) == 0;
}

/* The first edit to an imported profile forks it to "<name> (Custom).txt"
 * instead of overwriting the import in place -- a file dropped in from
 * AutoEq or a headphone vendor stays exactly what was dropped in. Once on a
 * fork, eq_is_custom_path() is true and this is a no-op; further edits land
 * on the fork directly, the same file, not a fresh "(Custom) (Custom)". */
static void eq_fork_if_needed(void) {
    if (eq_is_custom_path(eq_cur_path)) return;
    char dir[EP_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", eq_cur_path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else dir[0] = 0;
    /* eq_cur.name is both source and destination below -- snprintf reading
     * and writing the same buffer is undefined behaviour, and in practice
     * lost everything but the literal suffix. Read it out first. */
    char base[EP_NAME_LEN];
    snprintf(base, sizeof(base), "%s", eq_cur.name);
    snprintf(eq_cur_path, sizeof(eq_cur_path), "%s/%s (Custom).txt", dir, base);
    snprintf(eq_cur.name, sizeof(eq_cur.name), "%s (Custom)", base);
}

/* Persist a live edit back into the file it came from -- the point of storing
 * profiles as plain text in EQProfiles rather than only in eq.c's memory. A
 * no-op with nothing loaded (a fresh install, before any profile exists).
 * Only ever called from an actual edit site (see the tap handlers below),
 * never defensively on the way out of a screen -- each edit already saves
 * itself the moment it happens, so a save here always means something
 * genuinely changed, which is what makes forking-on-first-edit safe. */
static void eq_save_current(void) {
    if (!eq_cur_path[0]) return;
    eq_fork_if_needed();
    ep_save(eq_cur_path, &eq_cur);
}

static void eq_switch_to(const char *path) {
    ep_load(path, &eq_cur);
    snprintf(eq_cur_path, sizeof(eq_cur_path), "%s", path);
    eq_set_profile(&eq_cur);
}

/* Frequency sliders are log-scaled (20 Hz - 20 kHz): a linear slider spends
 * 99% of its travel above 2 kHz, which is backwards for how parametric bands
 * are actually placed. t is 0..1 across the slider's width. */
static float eq_freq_from_t(float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    return 20.0f * powf(1000.0f, t);   /* 20 Hz at t=0, 20000 Hz at t=1 */
}
static float eq_t_from_freq(float hz) {
    if (hz < 20.0f) hz = 20.0f; if (hz > 20000.0f) hz = 20000.0f;
    return logf(hz / 20.0f) / logf(1000.0f);
}

static int mini_visible(void) {
    /* R81 follow-up: reported live -- the USB Storage Mode banner and its
     * "Done" button were invisible/unreachable whenever the mini player
     * happened to be showing, since draw_ui() draws it unconditionally
     * over whatever draw_screen() just drew (see its own call site) --
     * SC_MENU's own draw code never calls draw_mini() itself, so this was
     * easy to miss building the banner in the first place. st_usb_mode()
     * gates every reader of this function at once rather than needing a
     * separate check wherever the banner (or anything else) cares. */
    return audio_is_active() && screen != SC_PLAYING &&
           (queue_n > 0 || radio_mode) && st_usb_mode() != 1;
}

/* Cover (or book cover), title, subtitle and a pause button, over the bottom
 * of any list screen. A live stream has no art to show -- art_request("") is
 * what play_station() already clears art_bits with -- so radio keeps the
 * original text-only layout instead of a thumbnail-shaped hole. */
static void draw_mini(uint16_t *fb) {
    int by = FB_H - MINI_H;
    fill_rect(fb, 0, by, FB_W, MINI_H, COL_HEADER);
    fill_rect(fb, 0, by, FB_W, 1, COL_LINE);

    /* BG55: flush against the screen, not floating with a gap on every
     * side -- that gap (COL_HEADER showing all the way around the art) was
     * what read as an unwanted frame, not an actual stroke drawn on it.
     * Bottom-left corner of the art sits in the bottom-left corner of the
     * screen (tx=0, bottom edge at FB_H); the thumbnail is taller than
     * MINI_H so its top edge overhangs above the bar's own top border,
     * rather than sitting inset within it on every side. */
    int thumb = MINI_H + 8, tx = 0, ty = FB_H - thumb;
    int text_x = 20;
    if (!radio_mode) {
        /* COL_ROW (the full player's own art placeholder) is the same value
         * as this bar's COL_HEADER background, so it would be invisible
         * here specifically -- COL_LINE instead, for real contrast against
         * the bar while art loads or when a track has none. */
        fill_rect(fb, tx, ty, thumb, thumb, COL_LINE);
        blit_art_scaled(fb, tx, ty, thumb);
        text_x = tx + thumb + 16;
    }

    /* R32: a position indicator at the very bottom edge of the screen --
     * not the mini-player's own top border (drawn above), the last rows of
     * the framebuffer itself, corner to corner. Display only, same as the
     * full player's own scrub strip's dur/pos sourcing (draw_scrub_strip)
     * but with no track/background under it and no scrub handling -- this
     * is glanceable position while browsing, not a second place to drag.
     * Radio has no meaningful duration, so it draws nothing here rather
     * than a bar that can never move. Drawn *after* the art thumbnail
     * above (BG55's flush-bottom art reaches this same row on its left
     * side), so it's never the art that ends up covering the bar -- 6px
     * thick (R79: 3x the original bump to 2px), so it survives sitting
     * this close to the bezel and reads at a glance. */
    if (!radio_mode && cur_track >= 0 && cur_track < queue_n) {
        lib_track_t *t = &queue[cur_track];
        int pos, dur;
        /* BG56: audio_dur_ms()/audio_pos_ms() are file-scoped, not
         * chapter-scoped -- for a multi-chapter book that's the position
         * and length of the whole file (often the whole book), not the
         * chapter actually playing. This is the same chapter-relative
         * math the full player's own Now Playing screen already uses
         * (see its book/chapter bar code) -- the mini player just never
         * got it, so its glanceable bar was quietly showing book
         * progress the whole time. */
        /* BG80 (extended, same as the full player's own Now Playing fix):
         * prefer the still-outstanding seek target over audio_pos_ms()
         * while the worker thread is mid-transition (a fresh decoder/
         * Bluetooth PCM connection especially) -- otherwise this bar shows
         * 0/stale, which reads as "reset to the start", for however long
         * that takes. Both are file-scoped absolute ms, so the same
         * chapter-start subtraction applies to either. */
        int pending = audio_seek_pending_ms();
        int raw_pos = pending >= 0 ? pending : audio_pos_ms();
        if (audiobook_mode && cur_track < ab_book.chap_n) {
            const ab_chapter_t *ch = &ab_book.chap[cur_track];
            dur = (int)ch->dur_ms;
            if (dur <= 0) dur = audio_dur_ms();   /* one-file-one-chapter book */
            pos = raw_pos - (int)ch->file_start_ms;
            if (pos < 0) pos = 0;
        } else {
            dur = audio_dur_ms();
            if (dur <= 0) dur = t->dur_ms;
            pos = raw_pos;
        }
        if (dur > 0) {
            int w = FB_W * pos / dur;
            if (w > FB_W) w = FB_W;
            if (w > 0) fill_rect(fb, 0, FB_H - 6, w, 6, COL_ACCENT);
        }
    }

    /* Text clips before whichever control cluster is narrowest for this
     * mode -- radio's lone play button leaves the most room, audiobook's
     * three buttons the least. */
    int text_edge = radio_mode ? FB_W - 104
                  : audiobook_mode ? MINI_ZONE_BACK - 20
                  : MINI_ZONE_PLAY - 20;
    if (radio_mode) {
        draw_text(fb, text_x, by + 12, radio_name, COL_TEXT, TEXT_PX_SMALL, text_edge);
        draw_text(fb, text_x, by + 42, "Internet radio", COL_DIM, TEXT_PX_SMALL, text_edge);
    } else {
        lib_track_t *t = &queue[cur_track];
        draw_text(fb, text_x, by + 12, t->name, COL_TEXT, TEXT_PX_SMALL, text_edge);
        /* A chapter carries no artist -- tracks[] built by ab_load_book()
         * never sets one -- so the regular artist line would be blank here.
         * q_album is the book title, set once when the queue was loaded and
         * stable regardless of what's being browsed elsewhere meanwhile.
         * For music, the track's own artist wins when it has one: q_artist
         * is the *album's* artist (BG30 -- a track's own tag can genuinely
         * differ from it, e.g. a compilation, and showing the album's
         * artist for every track was wrong). q_artist is only a fallback
         * for the rare track with no artist tag of its own at all. */
        const char *sub = audiobook_mode ? q_album
                         : (t->artist[0] ? t->artist : q_artist);
        /* R47's " . S" / " . R" / " . R1" suffix is gone: appended to the
         * artist line it read as part of the artist ("New Order . R1"),
         * which is what it was reported as. The mode is already shown
         * unambiguously by the transport row's own mode button, so this was
         * a second, worse copy of that state rather than the only way to
         * see it. */
        draw_text(fb, text_x, by + 42, sub, COL_DIM, TEXT_PX_SMALL, text_edge);
    }

    int cy = by + MINI_H / 2;
    /* Radio is the one mode with nothing either side of play/pause, so it
     * keeps the original larger button and position rather than shrinking
     * to match a cluster it isn't part of. */
    int cx = radio_mode ? FB_W - 46 : MINI_PLAY_CX;
    int pr = radio_mode ? 26 : MINI_BTN_R;
    fill_circle(fb, cx, cy, pr, COL_ACCENT);
    if (audio_is_paused()) {
        fill_triangle(fb, cx + 2, cy, pr - 4, +1, COL_BG);
    } else {
        /* Bar size scales with the circle instead of a fixed size left over
         * from when this button was always drawn at the same one radius --
         * the previous formula doubled the height by mistake (an errant
         * "* 2"), which combined with the smaller shared-row radius is what
         * made the bars look oversized. */
        int bw = pr / 4, bh = pr - 4, gap = bw;
        fill_rect(fb, cx - bw - gap / 2, cy - bh / 2, bw, bh, COL_BG);
        fill_rect(fb, cx + gap / 2,      cy - bh / 2, bw, bh, COL_BG);
    }

    if (audiobook_mode) {
        /* Plain white -10/+10, no circle -- play/pause is the only filled
         * button in the row, so it stays the one thing that reads as "the
         * button" at a glance. */
        const char *back_lbl = "-10", *fwd_lbl = "+10";
        draw_text(fb, MINI_BACK_CX - text_width(back_lbl, TEXT_PX_SMALL) / 2,
                  cy - TEXT_PX_SMALL / 2 + 2, back_lbl, COL_TEXT, TEXT_PX_SMALL, FB_W);
        draw_text(fb, MINI_SIDE_CX - text_width(fwd_lbl, TEXT_PX_SMALL) / 2,
                  cy - TEXT_PX_SMALL / 2 + 2, fwd_lbl, COL_TEXT, TEXT_PX_SMALL, FB_W);
    } else if (!radio_mode) {
        /* Next track: triangle against a bar, the same glyph the full
         * player uses for its own next button -- a lone triangle here read
         * as a second play button rather than "skip". Plain white, no
         * circle, same reasoning as the audiobook buttons above. */
        int nh = 22, ntw = (nh * 87) / 100;
        fill_triangle(fb, MINI_SIDE_CX - ntw / 2, cy, nh, +1, COL_TEXT);
        fill_rect(fb, MINI_SIDE_CX + ntw / 2 + 4, cy - nh / 2, 4, nh, COL_TEXT);
    }
}

/* Outline, level, and the nub on the end. Turns red when it is nearly out and
 * accent-coloured on charge, so the state reads without counting digits. A
 * filled rounded rect punched out by a slightly smaller one in the header
 * colour, the same trick as the pill sliders, rather than four traced edges. */
/* Five discrete fill levels (icon_batt_0/25/50/75/100), not a continuous
 * procedural fill -- the FA glyph reads far better at this pixel size than
 * the hand-drawn rounded-rect ever did, and every call site already prints
 * the exact percentage as text alongside the icon, so nothing is lost:
 * the number still carries the precision, the icon now just looks right. */
static void draw_battery(uint16_t *fb, int x, int y, int pct, int charging) {
    const icon_t *ic = pct > 87 ? &icon_batt_100 : pct > 62 ? &icon_batt_75
                      : pct > 37 ? &icon_batt_50  : pct > 12 ? &icon_batt_25
                      : &icon_batt_0;
    uint16_t c = charging ? COL_ACCENT : (pct >= 0 && pct <= 15 ? RGB(230, 80, 70) : COL_DIM);
    draw_icon(fb, FB_W, FB_H, x, y - 1, ic, c);
}

/* Three bars and a play triangle: the queue, in the place a hamburger would
 * sit. It replaced the word QUEUE, which alongside a title reading "Now
 * playing" was two labels telling you what you could already see. */
static void draw_queue_icon(uint16_t *fb, int x, int y, uint16_t c) {
    fill_rect(fb, x, y,      26, 3, c);
    fill_rect(fb, x, y + 8,  26, 3, c);
    fill_rect(fb, x, y + 16, 15, 3, c);
    fill_triangle(fb, x + 23, y + 17, 13, +1, c);
}

/* R70 step 1: the reorder handle -- six dots, two columns of three, the same
 * "grab here to drag" glyph most touch UIs already use for a reorderable
 * list, so it needs no label to read as "drag this row" at a glance. Visual
 * only for now: draw_grip_icon() just marks where a drag will start once R70
 * actually wires one up (not yet -- see SC_QUEUE's own row-drawing comment).
 * (x, y) is the icon's top-left; 12x24 overall (two 4px dots per row, 4px
 * gaps both ways). */
static void draw_grip_icon(uint16_t *fb, int x, int y, uint16_t c) {
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 2; col++)
            fill_rect(fb, x + col * 8, y + row * 10, 4, 4, c);
}

/* Clipped, for SC_QUEUE's scrolling list -- same reasoning as
 * fill_rect_clip() next to fill_rect() throughout this file. */
static void draw_grip_icon_clip(uint16_t *fb, int x, int y, uint16_t c,
                                 int clip_top, int clip_bot) {
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 2; col++)
            fill_rect_clip(fb, x + col * 8, y + row * 10, 4, 4, c, clip_top, clip_bot);
}

/* Body and cone, proportioned to match -- the version this replaced paired a
 * 16px triangle with a 7px box. Two shapes, same as the bluetooth glyph. */
static void draw_speaker(uint16_t *fb, int x, int y, uint16_t c) {
    fill_rect(fb, x, y - 5, 5, 10, c);
    fill_triangle(fb, x + 10, y, 14, -1, c);
}

/* Volume on the left, battery on the right — the same split every other
 * screen uses for its own left margin vs. right-aligned action, rather than
 * both clusters crowded together on one side. */
static void draw_status(uint16_t *fb) {
    char buf[16];
    /* One shared centre line. Everything here is positioned from it rather
     * than from its own top edge, which is what left the speaker sitting
     * above the digits next to it. */
    const int mid = STATUS_H / 2;
    const int ty  = mid - TEXT_PX_SMALL / 2;      /* text box is TEXT_PX_SMALL tall */
    /* Volume and battery only. The headphone and Bluetooth icons both said
     * what the route already says at the foot of the player. */

    int vol = audio_volume();
    const icon_t *vic = vol <= 0 ? &icon_vol_mute : vol < 34 ? &icon_vol_low
                       : vol < 67 ? &icon_vol_mid  : &icon_vol_high;
    draw_icon(fb, FB_W, FB_H, 24, mid - vic->h / 2, vic, COL_DIM);
    /* R95 follow-up: current/total steps for every output -- explicit
     * correction after the first cut only changed this on Bluetooth. */
    {
        int steps = audio_bt_vol_steps();
        int cur = (vol * steps + 50) / 100;
        if (cur < 0) cur = 0; if (cur > steps) cur = steps;
        snprintf(buf, sizeof(buf), "%d/%d", cur, steps);
    }
    draw_text(fb, 24 + 26, ty, buf, COL_DIM, TEXT_PX_SMALL, FB_W - 24 - 26);

    int pct = st_battery_pct();
    int bx = FB_W - 18 - 28;
    draw_battery(fb, bx, mid - 6, pct, st_charging());
    if (pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", pct);
        int tw = text_width(buf, TEXT_PX_SMALL);
        draw_text(fb, bx - 8 - tw, ty, buf, COL_DIM, TEXT_PX_SMALL, FB_W);
    }

    fill_rect(fb, 0, STATUS_H - 1, FB_W, 1, COL_LINE);
}

/* Word-wrapped, scrollable show notes -- drawn in place of the cover art
 * within the same box while pod_notes_showing is set. Wrapped fresh at every
 * draw off text_width() rather than pre-computed: the notes buffer is at
 * most 8KB and this is the only place in the app that ever wraps a paragraph,
 * so a real line-layout cache would be more code than the cost it avoids. */
static void pod_draw_notes(uint16_t *fb, int x, int y, int w, int h) {
    fill_rect(fb, x, y, w, h, COL_ROW);
    /* BG50: 20% bigger than TEXT_PX_SMALL (22 -> 26), line height scaled to
     * match rather than left at the old 30px, which read as cramped once
     * the glyphs themselves grew. */
    const int lh = 36, px = 26;
    int ty = y + 16 - pod_notes_scroll_px;
    const char *p = pod_notes_text;
    while (*p) {
        while (*p == ' ') p++;
        if (*p == '\n') { p++; ty += lh; continue; }   /* blank source line stays blank */
        char line[256];
        line[0] = '\0';
        for (;;) {
            const char *ws = p;
            while (*p && *p != ' ' && *p != '\n') p++;
            int wlen = (int)(p - ws);
            if (wlen == 0) break;
            char probe[300];
            if (line[0]) snprintf(probe, sizeof(probe), "%s %.*s", line, wlen, ws);
            else         snprintf(probe, sizeof(probe), "%.*s", wlen, ws);
            if (line[0] && text_width(probe, px) > w - 32) { p = ws; break; }   /* rewind: this word starts the next line */
            snprintf(line, sizeof(line), "%s", probe);
            if (*p == '\n') { p++; break; }
            while (*p == ' ') p++;
            if (!*p) break;
        }
        if (line[0] && ty + lh > y && ty < y + h)
            draw_text_clip(fb, x + 16, ty, line, COL_TEXT, px, w - 32, y, y + h);
        ty += lh;
        if (ty > y + h + 400) break;   /* far enough past the bottom to stop wrapping the rest */
    }
}

/* R73: the shift toggle -- letters-mode only, so it lives in the gap
 * between the text field (ends at fy+70 = 170) and the key grid (starts
 * at KB_GRID_Y), rather than competing with backspace/mode/space for one
 * of the 12 grid slots, which are already exactly full (10 digits + mode
 * + backspace). Shared x/y/w/h between draw and tap for the usual reason. */
#define KB_SHIFT_X 24
#define KB_SHIFT_Y 178
#define KB_SHIFT_W 100
#define KB_SHIFT_H 34

static const char *kb_shift_label(void) {
    return kb_shift == KB_SHIFT_LOCK ? "ABC"
         : kb_shift == KB_SHIFT_ONCE ? "Abc" : "abc";
}

/* T9 keyboard's own key geometry -- shared between the draw side and the
 * tap handler below so the two can never disagree about where a key is,
 * the same reasoning every other hit-zone pair in this file already
 * follows (e.g. BG47's skip-arc zones). */
#define KB_GRID_Y 220
#define KB_KEY_W  (FB_W / 3)
#define KB_KEY_H  130
static int kb_key_x(int col) { return col * KB_KEY_W; }
static int kb_key_y(int row) { return KB_GRID_Y + row * KB_KEY_H; }

/* Row-major 4x3: rows 0-2 are the phone keypad's 1-9, row 3 is mode-toggle/
 * space/backspace. -1 marks the two control slots so the tap handler and
 * draw side share one lookup instead of a parallel special-case list. */
static const int KB_GRID[4][3] = {
    { 1, 2, 3 },
    { 4, 5, 6 },
    { 7, 8, 9 },
    { -1, 0, -2 },   /* -1 = mode toggle, -2 = backspace; 0 is the real space key */
};

/* Labels are hand-written rather than derived from the cycle strings so
 * the letter keys can read "ABC" instead of "abc2ABC" -- the digit and the
 * uppercase run are in the cycle for typing, not for display. Symbols and
 * numbers labels do match their cycles exactly, since there's nothing to
 * hide there. */
static const char *kb_key_label(int mode, int key) {
    static const char *letters[10] = { "space", ".,?!", "ABC", "DEF", "GHI",
        "JKL", "MNO", "PQRS", "TUV", "WXYZ" };
    static const char *numbers[10] = { "0", "1", "2", "3", "4",
        "5", "6", "7", "8", "9" };
    static const char *symbols[10] = { "space", "\"#$", "%&'", "()*", "+-/",
        ":;=", "<>@", "[\\]", "^_`", "{|}~" };
    return mode == KB_MODE_NUMBERS ? numbers[key]
         : mode == KB_MODE_SYMBOLS ? symbols[key] : letters[key];
}

/* The mode key names what you'll get, not where you are. */
static const char *kb_mode_key_label(void) {
    return kb_mode == KB_MODE_LETTERS ? "123"
         : kb_mode == KB_MODE_NUMBERS ? "#+=" : "ABC";
}

static void draw_keyboard(uint16_t *fb) {
    fill_rect(fb, 0, 0, FB_W, FB_H, COL_BG);
    draw_text(fb, 24, 56, kb_title, COL_TEXT, TEXT_PX_BODY, FB_W - 180);
    draw_text(fb, 24, 20, "Cancel", COL_DIM, TEXT_PX_SMALL, 200);
    draw_right_col(fb, 20, "Done", COL_ACCENT);

    /* The buffer being edited, in a bordered field of its own so it reads
     * as "the thing you're typing" rather than another button. Right-
     * aligned scroll would be nicer for a long passphrase overflowing the
     * field, but this app has never needed that anywhere else either --
     * plain left-clipped text, same as every list row already does. */
    int fy = 100;
    fill_rect(fb, 24, fy, FB_W - 48, 70, COL_ROW);
    fill_rect(fb, 24, fy + 70 - 1, FB_W - 48, 1, COL_LINE);
    draw_text_clip(fb, KB_FIELD_X, fy + 22, kb_buf[0] ? kb_buf : "", COL_TEXT, TEXT_PX_BODY,
                   FB_W - 24, fy, fy + 70);
    /* Caret. Drawn solid rather than blinking: a blink needs its own
     * repaint tick, and on a screen that otherwise only redraws on input
     * that would be the sole reason to wake the UI loop continuously. */
    {
        int cx = kb_caret_x(kb_cursor);
        if (cx > FB_W - 28) cx = FB_W - 28;   /* keep it inside the field on overflow */
        fill_rect(fb, cx, fy + 16, 2, TEXT_PX_BODY + 10, COL_ACCENT);
    }

    /* R73: shift toggle, letters mode only -- dimmed and inert rather than
     * hidden in numbers/symbols mode, so the grid below doesn't jump up
     * and down as the mode key is tapped. */
    if (kb_mode == KB_MODE_LETTERS) {
        fill_rect(fb, KB_SHIFT_X, KB_SHIFT_Y, KB_SHIFT_W, KB_SHIFT_H, COL_ROW);
        const char *slabel = kb_shift_label();
        int slw = text_width(slabel, TEXT_PX_SMALL);
        draw_text(fb, KB_SHIFT_X + (KB_SHIFT_W - slw) / 2,
                  KB_SHIFT_Y + (KB_SHIFT_H - TEXT_PX_SMALL) / 2,
                  slabel, kb_shift != KB_SHIFT_OFF ? COL_ACCENT : COL_DIM,
                  TEXT_PX_SMALL, KB_SHIFT_X + KB_SHIFT_W);
    } else {
        const char *slabel = kb_shift_label();
        int slw = text_width(slabel, TEXT_PX_SMALL);
        draw_text(fb, KB_SHIFT_X + (KB_SHIFT_W - slw) / 2,
                  KB_SHIFT_Y + (KB_SHIFT_H - TEXT_PX_SMALL) / 2,
                  slabel, COL_LINE, TEXT_PX_SMALL, KB_SHIFT_X + KB_SHIFT_W);
    }

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 3; col++) {
            int key = KB_GRID[row][col];
            int x = kb_key_x(col), y = kb_key_y(row);
            fill_rect(fb, x + 4, y + 4, KB_KEY_W - 8, KB_KEY_H - 8, COL_ROW);
            const char *label;
            uint16_t col_txt = COL_TEXT;
            if (key == -1) { label = kb_mode_key_label(); col_txt = COL_ACCENT; }
            else if (key == -2) { label = "<-"; col_txt = COL_ACCENT; }
            else label = kb_key_label(kb_mode, key);
            int lw = text_width(label, TEXT_PX_BODY);
            draw_text(fb, x + (KB_KEY_W - lw) / 2, y + (KB_KEY_H - TEXT_PX_BODY) / 2 - 4,
                     label, col_txt, TEXT_PX_BODY, x + KB_KEY_W);
            /* The digit itself, small, in the corner -- phone-keypad muscle
             * memory still applies even though this is a touchscreen. Not in
             * numbers mode, where the big label already *is* the digit and a
             * second copy in the corner just reads as a smudge. */
            if (key >= 0 && kb_mode != KB_MODE_NUMBERS) {
                char dbuf[2] = { (char)('0' + key), 0 };
                draw_text(fb, x + KB_KEY_W - 22, y + 10, dbuf, COL_DIM, TEXT_PX_SMALL, x + KB_KEY_W);
            }
        }
    }
}

static void draw_screen(uint16_t *fb) {
    if (screen == SC_KEYBOARD) { draw_keyboard(fb); return; }
    /* R14 flagged skipping this full clear (header/mini-player strips are
     * unconditionally repainted anyway) as the next lever -- tried, live on
     * device, 2026-08-24: real corruption during list scroll (stale text
     * overlapping between rows), root cause not fully tracked down against
     * this app's double-buffered page-flip. Reverted rather than ship it
     * broken. Left as a known-attempted, not-yet-safe idea, not an open
     * "try this" -- the double-buffer staleness reasoning needs more than
     * a same-frame-combination-stable counter to actually hold. */
    /* R92: cover-derived colours only apply to the shared music/podcast
     * Now Playing screen -- audiobook_mode keeps its own plain background,
     * same reasoning R29's waveform already used to scope itself to Music
     * (a book chapter isn't "an album" the way this feature's colours are
     * themed around). Recomputed here, once per frame it's actually
     * needed, rather than only from art_request()'s own callback: this is
     * a plain seq-number check (see compute_cover_palette()'s own
     * comment) that's a no-op on every frame but the one where art_bits
     * genuinely just changed. */
    if (cover_palette_enabled && screen == SC_PLAYING && !audiobook_mode)
        compute_cover_palette();
    fill_rect(fb, 0, 0, FB_W, FB_H,
              (screen == SC_PLAYING && !audiobook_mode) ? np_col_bg() : COL_BG);
    /* The player has no title bar at all: a strip saying "Now playing" over a
     * screen showing the track, the artist and the artwork was telling you
     * what you could already see, and it was 62px that the artwork wanted.
     * Now Playing also drops the status strip itself (volume, battery) so the
     * art can run edge-to-edge from the very top, like stock -- the battery
     * figure moves to the output-route line at the bottom, volume is simply
     * gone (it was never anything more than a placeholder there), and the
     * Bluetooth codec/headset-battery readout that used to share that line
     * moved into the quick-settings Bluetooth row instead. */
    /* R46: the album-detail screen drops its status bar/title bar the same
     * way Now Playing already does, for the same reason -- the cover runs
     * edge-to-edge from y=0, and there's no room left for either. Back is
     * the swipe gesture everywhere else already relies on. */
    if (screen != SC_PLAYING && screen != SC_ARTIST_PAGE && !(screen == SC_TRACKS && !ab_list && !pod_list)) {
        fill_rect(fb, 0, 0, FB_W, CONTENT_Y, COL_HEADER);
    }

    const char *title = "Home";   /* R80: renamed from "Main Menu" */
    /* R?? follow-up: Main Menu had no "back" to offer even in the hooked
     * build (go_back() here just hands control to hiby_player's launcher,
     * which the edge swipe already does identically), and in standalone it
     * did nothing but restart the app -- see g_is_standalone's own comment.
     * A button that either duplicates the swipe or does nothing isn't worth
     * the screen space in either build. */
    /* R72: back moved from top-right "BACK" text to a leading accent arrow
     * next to the title (see the draw site below) -- there is no longer a
     * generic right-hand header label at all, only the three screen-
     * specific extra actions (Reset/Sync/Clear) drawn further down, each
     * already positioned by its own dedicated x-function. */
    int show_back = 0;
    if (screen == SC_ARTISTS) { title = cur_facet_label; show_back = 1; }
    else if (screen == SC_ALBUMS)  {
        title = recent_mode == RECENT_ADDED ? "Recently added"
              : recent_mode == RECENT_HEARD ? "Recently heard"
              : !cur_artist[0] ? "Albums"
              : cur_artist[0] == LIB_UNKNOWN_MARK[0] ? "Unknown" : cur_artist;
        show_back = 1;
    }
    else if (screen == SC_TRACKS)  { title = cur_album;  show_back = 1; }
    /* The album being played is the queue, so "QUEUE" goes back to the track
     * list — the same list, with the playing row marked. */

    else if (screen == SC_RADIO)   { title = "Radio"; show_back = 1; }
    else if (screen == SC_PLAYLISTS) { title = "Playlists"; show_back = 1; }
    else if (screen == SC_AUDIOBOOKS) { title = "Audiobooks"; show_back = 1; }
    else if (screen == SC_PODCASTS)   { title = "Podcasts"; show_back = 1; }
    else if (screen == SC_POD_SYNC)   { title = "Updating feeds"; show_back = 1; }
    else if (screen == SC_EQ)         { title = "Parametric EQ"; show_back = 1; }
    else if (screen == SC_EQ_BANDS)   { title = "Bands"; show_back = 1; }
    else if (screen == SC_MSEB)       { title = "MSEB"; show_back = 1; }
    else if (screen == SC_EQ_BAND) {
        static char band_title[16];   /* draw_screen's own `buf` isn't declared this early */
        snprintf(band_title, sizeof(band_title), "Band %d", eq_editing_band + 1);
        title = band_title; show_back = 1;
    }
    else if (screen == SC_SETTINGS)       { title = "Settings"; show_back = 1; }
    else if (screen == SC_SETTINGS_THEME) { title = "Accent colour"; show_back = 1; }
    else if (screen == SC_SETTINGS_ABOUT) { title = "About"; show_back = 1; }
    else if (screen == SC_SETTINGS_TIMEZONE) { title = "Timezone"; show_back = 1; }
    else if (screen == SC_SETTINGS_THEMEMODE) { title = "Theme"; show_back = 1; }
    else if (screen == SC_SETTINGS_WIFI) { title = "Wi-Fi"; show_back = 1; }
    else if (screen == SC_SETTINGS_BT)   { title = "Bluetooth"; show_back = 1; }
    else if (screen == SC_SETTINGS_USB)  { title = "USB working mode"; show_back = 1; }
    else if (screen == SC_QUEUE) { title = "Queue"; show_back = 1; }
    else if (screen == SC_MUSIC_MENU)     { title = "Music"; show_back = 1; }

    /* The player has no title bar. Drawn unconditionally, it sat behind the
     * artwork with the ends of "Music" and "EXIT" poking out either side of
     * the cover. */
    /* R46: the album-detail screen drops its status bar/title bar the same
     * way Now Playing already does, for the same reason -- the cover runs
     * edge-to-edge from y=0, and there's no room left for either. Back is
     * the swipe gesture everywhere else already relies on. */
    if (screen != SC_PLAYING && screen != SC_ARTIST_PAGE && !(screen == SC_TRACKS && !ab_list && !pod_list)) {
        g_header_show_back = show_back;
        int title_x = show_back ? BACK_ARROW_X + BACK_ARROW_W + 14 : 18;
        if (show_back)
            draw_back_arrow(fb, BACK_ARROW_X, STATUS_H + 14 + TEXT_PX_TITLE / 2, COL_ACCENT);
        draw_text(fb, title_x, STATUS_H + 14, title, COL_TEXT, TEXT_PX_TITLE, FB_W - 40 - (title_x - 18));
        /* MSEB's one extra header action: zero every band back to 0 dB.
         * Doesn't touch Enabled -- "reset" clears the tuning, not the
         * on/off state, which the user didn't ask to lose. */
        if (screen == SC_MSEB)
            draw_text(fb, mseb_reset_x(), STATUS_H + 20, "Reset", COL_DIM, TEXT_PX_SMALL, FB_W);
        /* Podcasts' one extra header action: run .podsync/podsync_once.sh
         * for every feed. Dimmed rather than hidden while it's already
         * running, same convention a disabled control uses elsewhere in
         * this app, since tapping it again would just fork a second sync
         * over the first one's half-written files. */
        if (screen == SC_PODCASTS) {
            draw_text(fb, pod_sync_x(), STATUS_H + 20,
                      pod_update_running() ? "Syncing" : "Sync",
                      pod_update_running() ? COL_DIM : COL_ACCENT, TEXT_PX_SMALL, FB_W);
        }
        /* R52: Queue's one extra header action -- drop everything queued
         * after the currently-playing track. Dimmed when there's nothing
         * past it to clear, same "nothing would happen" convention Sync
         * uses while already running, rather than a tap that silently does
         * nothing with no indication why. */
        if (screen == SC_QUEUE) {
            int has_more = cur_track >= 0 && cur_track + 1 < queue_n;
            draw_text(fb, queue_clear_x(), STATUS_H + 20, "Clear",
                      has_more ? COL_ACCENT : COL_DIM, TEXT_PX_SMALL, FB_W);
        }
        draw_status(fb);
        fill_rect(fb, 0, CONTENT_Y - 1, FB_W, 1, COL_LINE);
    }

    /* Rows draw at fixed positions and never shift for scroll_px: a page
     * flip, not a slide. Reading scroll_px here was the actual bug behind
     * "still scrolling as before" — dirty being gated on a row completing
     * did not stop OTHER redraws (the clock, art loading, anything else
     * that marks dirty) from painting mid-drag using whatever fractional
     * scroll_px happened to be at that instant, which looked exactly like
     * continuous scrolling. Ignoring it here is what actually makes rows
     * hold still until a full row's worth of drag completes. */
    int y = CONTENT_Y;
    int clip_bot = FB_H - (mini_visible() ? MINI_H : 40);
    char buf[32];

    if (screen == SC_MENU) {
        /* TEMP: log only when it actually changes from what was last seen,
         * so a long natural session leaves a trail showing exactly when
         * (not just that) the last entry stops being "Settings", instead of
         * either a single boot-time snapshot or a flood on every redraw.
         * TOP_N-1, not a hardcoded index: MSEB's insertion already moved
         * this once, from 4 to 5, and a literal index silently starts
         * watching the wrong slot every time the menu grows again. */
        static const void *dbg_last;
        if (top_menu[TOP_N - 1].label != dbg_last) {
            dbg_last = top_menu[TOP_N - 1].label;
            for (int i = 0; i < TOP_N; i++)
                mlog("[dbg] top_menu[%d] ptr=%p str=\"%s\"\n",
                     i, (void *)top_menu[i].label, top_menu[i].label);
        }
        /* R81: the card is unsafe to read from while it's exported as a USB
         * drive to whatever it's plugged into -- a host machine can rename,
         * move, or delete files out from under a scan or a currently-open
         * book/feed at any moment, with no way for this app to know. Music/
         * Audiobooks/Podcasts (the three that actually read the card open-
         * endedly) grey out and stop opening for exactly as long as that's
         * true; Parametric EQ/MSEB/Radio/Settings don't touch the card the
         * same way and stay live. */
        int usb_storage = st_usb_mode() == 1;
        for (int i = 0; i < TOP_N; i++) {
            int disabled = usb_storage && (i == TOP_MUSIC || i == TOP_AUDIOBOOKS || i == TOP_PODCASTS);
            draw_text(fb, 24, y + 20, top_menu[i].label,
                      disabled ? COL_DIM : COL_TEXT, TEXT_PX_BODY, FB_W - 24);
            fill_rect(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE);
            y += ROW_H;
        }
        if (usb_storage) {
            int bh = 60;
            fill_rect(fb, 0, FB_H - bh, FB_W, bh, COL_ACCENT);
            const char *msg = "USB Storage Mode";
            int ty = FB_H - bh + (bh - TEXT_PX_BODY) / 2 - 2;
            /* R82: "Done" switches back to ADB -- left-aligned label, since
             * "Done" now claims the right side the centered text used to
             * have (same left-label/right-action split header_back_x()'s
             * own row already uses, just without an actual header here). */
            draw_text(fb, 24, ty, msg, COL_BG, TEXT_PX_BODY, usb_done_x() - 40);
            draw_text(fb, usb_done_x(), ty, "Done", COL_BG, TEXT_PX_BODY, FB_W - 24);
        }
        return;
    }

    if (screen == SC_MUSIC_MENU) {
        for (int i = 0; i < MENU_N; i++) {
            draw_text(fb, 24, y + 20, menu[i].label, COL_TEXT, TEXT_PX_BODY, FB_W - 24);
            fill_rect(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE);
            y += ROW_H;
        }
        return;
    }

    if (screen == SC_PLAYLISTS) {
        /* R76: smooth per-pixel scroll, same off-based shape the album-
         * detail page and Settings already use, rather than the whole-row
         * "page flip" this screen (and several others) had instead --
         * requested live ("smooth scrolling as in albums, everywhere").
         * Iterated by absolute row index rather than vis_rows() count, so
         * a row half-scrolled off either edge still draws its visible
         * sliver instead of only ever showing whole rows.
         * R71: row 0 is "New Playlist" (an action, not a playlist), so
         * every real entry's index is shifted by one -- idx - 1, not idx,
         * into playlists[]. */
        int off = scroll * ROW_H + scroll_px;
        for (int idx = 0; idx <= playlist_n; idx++) {
            int ry = CONTENT_Y + idx * ROW_H - off;
            if (ry + ROW_H < CONTENT_Y) continue;
            if (ry > clip_bot) break;
            if (idx == 0) {
                draw_text_clip(fb, 24, ry + 20, "New Playlist", COL_ACCENT, TEXT_PX_BODY,
                              FB_W - 40, CONTENT_Y, clip_bot);
            } else {
                draw_text_clip(fb, 24, ry + 20, playlists[idx - 1].name, COL_TEXT, TEXT_PX_BODY,
                               FB_W - 40, CONTENT_Y, clip_bot);
            }
            fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        }
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_RADIO) {
        /* R76: smooth per-pixel scroll -- see SC_PLAYLISTS' own comment. */
        int off = scroll * ROW_H + scroll_px;
        for (int idx = 0; idx < station_n; idx++) {
            int ry = CONTENT_Y + idx * ROW_H - off;
            if (ry + ROW_H < CONTENT_Y) continue;
            if (ry > clip_bot) break;
            int playing = radio_mode && audio_is_active() &&
                          !strcmp(stations[idx].name, radio_name);
            if (playing) {
                fill_rect_clip(fb, 0, ry, FB_W, ROW_H, COL_ROW, CONTENT_Y, clip_bot);
                fill_rect_clip(fb, 0, ry, 4, ROW_H, COL_ACCENT, CONTENT_Y, clip_bot);
            }
            /* A station with no URL is shown dimmed rather than hidden: it is
             * a slot in the file waiting to be filled in. */
            int have = stations[idx].url[0] != '\0';
            draw_text_clip(fb, 24, ry + 20, stations[idx].name,
                           playing ? COL_ACCENT : (have ? COL_TEXT : COL_DIM),
                           TEXT_PX_BODY, FB_W - 40, CONTENT_Y, clip_bot);
            fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        }
        if (station_n == 0)
            draw_text(fb, 24, CONTENT_Y + 20, "No stations configured", COL_DIM, TEXT_PX_BODY, FB_W - 40);
        /* A station that plays nothing because Wi-Fi is off looks exactly like
         * a station that is broken. Say which. */
        if (!st_net_up())
            draw_text(fb, 24, FB_H - 34, "Wi-Fi is off", RGB(230, 80, 70), TEXT_PX_SMALL, FB_W - 40);
        else if (radio_msg[0])
            draw_text(fb, 24, FB_H - 34, radio_msg, RGB(230, 80, 70), TEXT_PX_SMALL, FB_W - 40);
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_QUEUE) {
        /* R50: track count (the whole queue, matching what the list below
         * actually has rows for) and remaining playtime (summed from the
         * currently-playing track onward, not the whole queue -- "how much
         * music is left" is what a summary line is actually useful for, not
         * a total that includes tracks already heard). Fixed above the
         * scrolling rows rather than the first row's own slot, so it stays
         * on screen regardless of scroll position (vis_rows() already
         * carves out the room for it). */
        {
            char cbuf[24], dbuf[16], line[48];
            snprintf(cbuf, sizeof(cbuf), "%d track%s", queue_n, queue_n == 1 ? "" : "s");
            int64_t remain_ms = 0;
            int from = (cur_track >= 0 && cur_track < queue_n) ? cur_track : 0;
            for (int i = from; i < queue_n; i++) remain_ms += queue[i].dur_ms;
            fmt_dur(dbuf, sizeof(dbuf), remain_ms);
            snprintf(line, sizeof(line), "%s \xc2\xb7 %s total", cbuf, dbuf);
            /* Fixed, not scrolled -- vis_rows() already excludes this height
             * from the scrollable area (see its own SC_QUEUE case), so this
             * is a sticky header, not the list's own first row. */
            draw_text_clip(fb, 24, y + 16, line, COL_DIM, TEXT_PX_SMALL, FB_W - 40, CONTENT_Y, clip_bot);
            fill_rect_clip(fb, 0, y + QUEUE_SUMMARY_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        }
        /* R76: smooth per-pixel scroll for the rows below the fixed summary
         * -- see SC_PLAYLISTS' own comment. list_top (not CONTENT_Y) is
         * both the row loop's base and its own top clip bound, so a
         * scrolled row can never bleed up over the summary line itself. */
        int list_top = CONTENT_Y + QUEUE_SUMMARY_H;
        int off = scroll * ROW_H + scroll_px;
        /* BG71: the actual upcoming play order -- queue_display_index()
         * walks shuffle_order[] when shuffle's on rather than plain array
         * order, so this genuinely reflects what happens next rather than
         * just the album's own track order. */
        for (int display_i = 0; display_i < queue_n; display_i++) {
            int y = list_top + display_i * ROW_H - off;
            if (y + ROW_H < list_top) continue;
            if (y > clip_bot) break;
            int idx = queue_display_index(display_i);
            if (idx < 0) break;
            lib_track_t *t = &queue[idx];
            int playing = audio_is_active() && idx == cur_track;
            /* R70: the row currently under the grip -- same highlight shape
             * as "playing" (full-row fill plus an accent edge), so a
             * dragged row is unmistakable as it snaps between slots, but a
             * full-width accent edge rather than playing's 4px one, so the
             * two never look identical if a track happens to be both. */
            int dragging_this = queue_drag_active && display_i == queue_drag_display_i;
            /* R70: the row currently being swiped -- dx0 shifts every piece
             * of its own content horizontally as the finger moves, revealing
             * COL_ACCENT (drawn first, full row, unshifted) in the space the
             * content used to cover. Mutually exclusive with dragging_this
             * by construction (grip-only vs. body-only starts), but not with
             * playing -- suppressed there deliberately: mid-swipe is a more
             * urgent state than "this is playing", and showing both accent
             * treatments overlapping would just look like a rendering bug. */
            int swiping_this = queue_swipe_active && display_i == queue_swipe_display_i;
            int dx0 = swiping_this ? queue_swipe_dx : 0;
            if (swiping_this)
                fill_rect_clip(fb, 0, y, FB_W, ROW_H, COL_ACCENT, list_top, clip_bot);
            if (playing && !swiping_this) {
                fill_rect_clip(fb, 0, y, FB_W, ROW_H, COL_ROW, list_top, clip_bot);
                fill_rect_clip(fb, 0, y, 4, ROW_H, COL_ACCENT, list_top, clip_bot);
            }
            if (dragging_this) {
                if (!playing) fill_rect_clip(fb, 0, y, FB_W, ROW_H, COL_ROW, list_top, clip_bot);
                fill_rect_clip(fb, 0, y, FB_W, 2, COL_ACCENT, list_top, clip_bot);
                fill_rect_clip(fb, 0, y + ROW_H - 2, FB_W, 2, COL_ACCENT, list_top, clip_bot);
            }
            /* R70 step 1: FB_W - 150, not the usual FB_W - 110 every other
             * duration-bearing row uses -- 40px carved out on the right for
             * the grip below (12px icon + margin either side), so a long
             * title doesn't draw underneath it before eliding.
             *
             * BUG fix: the right_edge clip bound has to shift by dx0 right
             * alongside x, or it doesn't -- x moves but the window it's
             * being clipped against stays put, so the available width
             * shrinks as the row slides and the title vanishes well before
             * reaching the delete threshold (reported live as "goes
             * completely accent colour"). Shifting both keeps the same
             * window, just moved, so the row's own text reads exactly as it
             * always did throughout the swipe -- only its position changes. */
            draw_text_clip(fb, 24 + dx0, y + 20, t->name, playing ? COL_ACCENT : COL_TEXT,
                          TEXT_PX_BODY, FB_W - 150 + dx0, list_top, clip_bot);
            if (t->dur_ms > 0) {
                char b[16];
                fmt_dur(b, sizeof(b), t->dur_ms);
                int bw = text_width(b, TEXT_PX_SMALL);
                /* Same right margin (24px, minus the index strip) draw_right_clip()
                 * uses, just shifted left by the grip's own reserved column. */
                int right = FB_W - 24 - (index_visible() ? INDEX_W : 0) - 40;
                draw_text_clip(fb, right - bw + dx0, y + 22, b, COL_DIM, TEXT_PX_SMALL,
                               FB_W + dx0, list_top, clip_bot);
            }
            /* R70: the drag handle -- accent while this exact row is the one
             * being dragged, same as the rest of dragging_this's highlight. */
            draw_grip_icon_clip(fb, FB_W - 24 - (index_visible() ? INDEX_W : 0) - 16 + dx0,
                                 y + (ROW_H - 24) / 2,
                                 (playing || dragging_this) ? COL_ACCENT : COL_DIM, list_top, clip_bot);
            /* BUG fix: this 1px COL_LINE separator used to draw unconditionally,
             * landing right on top of dragging_this's own 2px bottom accent
             * border and painting over its last pixel -- the top border stayed
             * a clean 2px while the bottom read as 1px of accent plus 1px of
             * grey, visibly thinner. The accent border already marks this row's
             * bottom edge, so the plain separator is redundant here, not just
             * harmless to skip. Same reasoning extends to swiping_this: a
             * grey line cutting across a full-row accent reveal would read
             * as a rendering glitch, not a deliberate row boundary. */
            if (!dragging_this && !swiping_this)
                fill_rect_clip(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE, list_top, clip_bot);
        }
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_PLAYING) {
        if (audiobook_mode) {
            lib_track_t *t = (cur_track >= 0 && cur_track < queue_n) ? &queue[cur_track] : NULL;
            if (!t) return;
            int cy = 0, cx = (FB_W - ART_PX) / 2;
            fill_rect(fb, cx, cy, ART_PX, ART_PX, COL_ROW);
            blit_art(fb, cx, cy);
            int ty = title_y();
            draw_scroll_title(fb, ty, t->name, COL_TEXT, COL_BG);

            draw_text(fb, 24, ty + 44, q_album, COL_DIM, TEXT_PX_BODY, FB_W - 24);
            snprintf(buf, sizeof(buf), "%d of %d", cur_track + 1, queue_n);
            draw_right(fb, ty + 44, buf);

            /* Positions come from the chapter table, not from summing whole
             * files: a chapter is a stretch of a file, so "where in the
             * chapter" is the file position minus where the chapter starts. */
            const ab_chapter_t *ch =
                (cur_track >= 0 && cur_track < ab_book.chap_n) ? &ab_book.chap[cur_track] : NULL;
            int64_t book_total = ab_book.total_ms;
            int chap_dur = ch ? (int)ch->dur_ms : 0;
            /* A format the scan could not measure has no duration until the
             * decoder reports one; one file then means one chapter, so the
             * file's length is the book's. */
            if (chap_dur <= 0) {
                chap_dur = audio_dur_ms();
                if (book_total <= 0) book_total = chap_dur;
            }
            int64_t chap_start = ch ? ch->file_start_ms : 0;
            /* BG80: right after a chapter/book (re)opens, the worker thread
             * can take a while to actually get to the queued seek -- opening
             * a decoder, or a fresh Bluetooth PCM connection especially --
             * during which audio_pos_ms() genuinely reads 0/whatever the
             * previous file left behind, not where we just asked to resume.
             * Drawing that raw looks like the player reset to the chapter's
             * start for a moment before snapping to the real position.
             * audio_seek_pending_ms() is the target we actually asked for,
             * still outstanding until the worker applies it -- prefer it
             * over the live position for exactly that window, after which
             * it's -1 and audio_pos_ms() has already caught up to it. */
            int pending = audio_seek_pending_ms();
            int64_t live_pos = (pending >= 0) ? pending : audio_pos_ms();
            int chap_pos = scrub_active ? scrub_ms(chap_dur)
                                        : (int)(live_pos - chap_start);
            if (chap_pos < 0) chap_pos = 0;
            int64_t book_pos = (ch ? ch->book_start_ms : 0) + chap_pos;

            /* Real-world time, not content time: at 1.3x, thirty minutes of
             * book is twenty-three minutes away, and that is the number
             * worth showing on a countdown. Position and the bar fill both
             * stay in content time -- unaffected by speed, and what the
             * scrub math already assumes. */
            double speed = ab_speed_permille / 1000.0;

            int bby = ab_book_bar_y();
            draw_text(fb, 24, bby - 26, "Book", COL_DIM, TEXT_PX_SMALL, FB_W - 200);
            if (book_total > 0) {
                fmt_left(buf, sizeof(buf), (int64_t)((book_total - book_pos) / speed));
                draw_right(fb, bby - 26, buf);
            }
            fill_rect(fb, 24, bby, FB_W - 48, 4, COL_LINE);
            if (book_total > 0) {
                int w = (int)((FB_W - 48) * book_pos / book_total);
                if (w > FB_W - 48) w = FB_W - 48;
                fill_rect(fb, 24, bby, w, 4, COL_DIM);   /* dim: display-only, not the accent bar */
            }

            int cby = ab_chapter_bar_y();
            draw_text(fb, 24, cby - 26, "Chapter", COL_DIM, TEXT_PX_SMALL, FB_W - 200);
            if (chap_dur > 0) {
                fmt_left(buf, sizeof(buf), (int64_t)((chap_dur - chap_pos) / speed));
                draw_right(fb, cby - 26, buf);
            }
            int cbh = scrub_active ? 10 : 6;
            int cbyy = cby - (cbh - 6) / 2;
            fill_rect(fb, 24, cbyy, FB_W - 48, cbh, COL_LINE);
            if (chap_dur > 0) {
                int w = (FB_W - 48) * chap_pos / chap_dur;
                if (w > FB_W - 48) w = FB_W - 48;
                fill_rect(fb, 24, cbyy, w, cbh, COL_ACCENT);
                if (scrub_active) fill_circle(fb, 24 + w, cby + 3, 13, COL_ACCENT);
            }

            int cyy = cby + 58 + CTRL_NUDGE_PX;
            int mid = FB_W / 2;

            /* -10s / +10s, not the regular player's prev/next glyphs: a
             * curved skip-arrow icon with the number inside -- a seek
             * amount isn't inferable from a plain triangle. Vendored Font
             * Awesome arrow-rotate icons (icon_skip_back/_forward,
             * icons_data.h), replacing this app's own hand-drawn
             * arc approximation on explicit request. */
            {
                /* Closer to the play button than the regular player's
                 * prev/next (128) sat -- pulled in so the row reads as one
                 * cluster with the speed ring rather than two rings out at
                 * the edges with a gap to it. The input handler's hit test
                 * uses this same 96 for its speed-ring position; keep them
                 * in sync if this moves again. */
                int off = 96;
                draw_icon(fb, FB_W, FB_H, mid - off - icon_skip_back.w / 2,
                         cyy - icon_skip_back.h / 2, &icon_skip_back, COL_TEXT);
                draw_icon(fb, FB_W, FB_H, mid + off - icon_skip_forward.w / 2,
                         cyy - icon_skip_forward.h / 2, &icon_skip_forward, COL_TEXT);
                const char *n = "10s";
                int nw = text_width(n, TEXT_PX_SMALL);
                draw_text(fb, mid - off - nw / 2, cyy - TEXT_PX_SMALL / 2 + 2, n, COL_TEXT, TEXT_PX_SMALL, FB_W);
                draw_text(fb, mid + off - nw / 2, cyy - TEXT_PX_SMALL / 2 + 2, n, COL_TEXT, TEXT_PX_SMALL, FB_W);
            }

            fill_circle(fb, mid, cyy, 42, COL_ACCENT);
            if (audio_is_paused()) {
                fill_triangle(fb, mid + 4, cyy, 36, +1, COL_BG);
            } else {
                fill_rect(fb, mid - 15, cyy - 18, 10, 36, COL_BG);
                fill_rect(fb, mid + 5,  cyy - 18, 10, 36, COL_BG);
            }

            /* Speed: a ring (two concentric fills, since fill_circle only
             * does solid) rather than the transport's solid accent circle,
             * so it doesn't read as a fourth transport button. */
            int scx = mid - 96 - 70, scy = cyy;   /* 96 must match the skip rings' offset above */
            fill_circle(fb, scx, scy, 22, COL_LINE);
            fill_circle(fb, scx, scy, 20, COL_BG);
            snprintf(buf, sizeof(buf), "%.1f\xc3\x97", speed);
            int sw = text_width(buf, TEXT_PX_SMALL);
            draw_text(fb, scx - sw / 2, scy - TEXT_PX_SMALL / 2 + 2, buf, COL_TEXT, TEXT_PX_SMALL, FB_W);

            /* R83: the route/battery readout and the format/kbps line that
             * used to sit either side of this row both moved to the quick-
             * settings top bar (qs_format_info()) -- visible from wherever
             * quick settings is pulled down, not just this one screen. */
            draw_queue_icon(fb, FB_W - 24 - 26, FB_H - 32, COL_DIM);
            return;
        }
        if (radio_mode) {
            /* A live stream has no length, no track number and no cover, so
             * the layout drops the parts that would only ever be blank. */
            int cy = 120;
            draw_text(fb, 24, cy, radio_name, COL_TEXT, TEXT_PX_TITLE, FB_W - 48);
            draw_text(fb, 24, cy + 52, "Internet radio", COL_DIM, TEXT_PX_BODY, FB_W - 48);

            int pos = audio_pos_ms();
            snprintf(buf, sizeof(buf), "%d:%02d", pos / 60000, (pos / 1000) % 60);
            draw_text(fb, 24, cy + 110, buf, COL_DIM, TEXT_PX_SMALL, FB_W);
            draw_right(fb, cy + 110, audio_is_active() ? "LIVE" : "stopped");

            int cyy = cy + 190, mid = FB_W / 2;
            fill_circle(fb, mid, cyy, 42, COL_ACCENT);
            if (audio_is_paused()) fill_triangle(fb, mid + 4, cyy, 36, +1, COL_BG);
            else {
                fill_rect(fb, mid - 15, cyy - 18, 10, 36, COL_BG);
                fill_rect(fb, mid + 5,  cyy - 18, 10, 36, COL_BG);
            }

            /* Say which codec actually turned up: an HLS station is AAC, and
             * labelling everything MP3 was simply wrong. */
            snprintf(buf, sizeof(buf), "%s stream  \xc2\xb7  %s",
                     audio_codec()[0] ? audio_codec() : "Live", audio_output());
            draw_text(fb, 24, FB_H - 34, buf, COL_ACCENT, TEXT_PX_SMALL, FB_W - 48);
            return;
        }

        lib_track_t *t = (cur_track >= 0 && cur_track < queue_n) ? &queue[cur_track] : NULL;
        if (!t) return;

        int cy = 0, cx = (FB_W - ART_PX) / 2;
        /* The info icon (drawn further down, in the transport row) toggles
         * show notes in place of the cover art, within the same box -- see
         * pod_draw_notes() and the matching tap zone below. */
        if (podcast_mode && pod_notes_showing) {
            pod_draw_notes(fb, cx, cy, ART_PX, ART_PX);
        } else {
            fill_rect(fb, cx, cy, ART_PX, ART_PX, COL_ROW);
            blit_art(fb, cx, cy);
        }
        int ty = title_y();
        draw_scroll_title(fb, ty, t->name, np_col_fg(), np_col_bg());
        /* The track's own artist wins when it has one -- q_artist is the
         * *album's* artist and can genuinely differ from a track's own tag
         * (BG30: a compilation showed the album's artist for every track
         * instead of each one's real artist). Falls back to q_artist only
         * for a track with no artist tag of its own (e.g. a playlist entry
         * pulled in from elsewhere) so the line is never simply blank.
         * A podcast episode has no artist tag and an intentionally-empty
         * q_artist (see pod_play_episode()), so this line is blank for one
         * rather than showing the feed name twice. */
        draw_text(fb, 24, ty + 44, t->artist[0] ? t->artist : q_artist,
                  np_col_dim(), TEXT_PX_BODY, FB_W - 24);
        draw_text(fb, 24, ty + 82, podcast_mode ? cur_feed : q_album, np_col_dim(), TEXT_PX_SMALL, FB_W - 110);
        /* Position in the queue, not the track's own number. Those are not the
         * same thing and showing one against the other produced "11 of 3" on a
         * playlist — and "11 of 9" on an album whose numbering has gaps, which
         * this one does. A podcast episode is never queued alongside others
         * (see podcast_mode's comment), so "1 of 1" would say nothing true. */
        if (!podcast_mode) {
            snprintf(buf, sizeof(buf), "%d of %d", cur_track + 1, queue_n);
            draw_right_col(fb, ty + 82, buf, np_col_dim());
        }

        /* BG80 (extended here): audio_pos_ms() reads 0/stale for however
         * long the worker thread takes to tear down the previous decoder/
         * output and open this one -- a fresh Bluetooth PCM connection
         * especially, worse yet coming from something that was still
         * actively playing (an audiobook, say) rather than paused first.
         * Originally fixed only for the audiobook screen's own Book/Chapter
         * bars; reported live here on a podcast (BT connected, mid-
         * audiobook-playback switch) as "looks like it reset to the start"
         * for 10-20s before snapping to the real resumed position -- the
         * exact symptom BG80 already describes, just never applied to this
         * shared music/podcast block. audio_seek_pending_ms() is the
         * target already asked for, still outstanding until the worker
         * applies it; -1 once caught up or if nothing is pending. */
        int pending = audio_seek_pending_ms();
        int pos = pending >= 0 ? pending : audio_pos_ms();
        int dur = audio_dur_ms();
        if (dur <= 0) dur = t->dur_ms;
        /* While scrubbing the bar and the clock follow the finger rather than
         * the decoder, so you can see where you are about to land. */
        if (scrub_active) pos = scrub_ms(dur);
        int by = bar_y();      /* BG40: was its own ty+118, now the shared value */
        /* R102: reported live as thin -- 6px (10 while scrubbing) read as
         * flimsy next to the waveform's own columns. Bumped to a flat 10
         * (14 scrubbing), the same +4 bump on top scrubbing already had. */
        int bh = scrub_active ? 14 : 10;
        /* R86 follow-up: R86's first cut grew the waveform around the
         * *plain bar's* centre (`by`) without checking what that centre
         * actually had room for -- screenshotted live, the taller columns
         * ran straight into the album/track-count line above ("Give Blood
         * / 7 of 16"), only 36px above `by`. Properly centred instead, in
         * the real space between that line and the transport buttons
         * below -- both ends computed the same way the things they're
         * bounded by are already positioned elsewhere in this block/file
         * (ty+82 + one TEXT_PX_SMALL line for the top; the button row's
         * own centre minus its 42px radius for the bottom, matching the
         * literal `fill_circle(..., 42, ...)` below). wave_cy/max_h/
         * clock_y computed once here and read by both the draw block just
         * below and the clock text further down, rather than each
         * re-deriving the same bounds a second time.
         *
         * R102: clock_y used to be wave_bot-anchored only for wave_loaded,
         * falling back to a plain `by+14` otherwise -- reported live as
         * inconsistent spacing between tracks, since which one a given
         * track got depended on nothing the listener could see (whether
         * its waveform happened to be cached yet). Computed unconditionally
         * now, off `by` alone, so every track's clock sits in exactly the
         * same place regardless of wave_loaded.
         *
         * R104: wave_cy/max_h moved out of the wave_loaded branch and made
         * unconditional too, same reasoning as clock_y just above -- the
         * plain bar was still centring on raw `by`, 2-6px above the true
         * midpoint of the title/clock gap once R102 thickened it, reported
         * live as "needs to be more vertically central." wave_cy already
         * computed the correct centre of exactly that gap for the waveform
         * case; the plain bar now shares it rather than deriving its own
         * off `by`. max_h keeps its own >=16 floor even though the plain
         * bar never reads it, since wave_cy's derivation depends on it. */
        int wave_top = ty + 82 + TEXT_PX_SMALL + 12;
        int wave_bot = (by + 70 + CTRL_NUDGE_PX) - 42 - 10;
        int gap = 10;
        int clock_y = wave_bot - 6;
        int max_h = (clock_y - gap) - wave_top;
        if (max_h < 16) max_h = 16;
        int wave_cy = wave_top + max_h / 2;
        int byy = wave_cy - bh / 2;
        /* R29: the waveform seek bar, when this track has a cached one
         * (Music only -- wave_loaded is never set for a podcast episode,
         * see play_index()'s own comment, so this always falls through to
         * the plain bar below for those). Same column count as
         * WAVE_BUCKETS, spread evenly across the same width the plain bar
         * fills; each column's height comes straight from its cached 0-255
         * peak. Coloured up to the played fraction, dim past it -- the
         * same played/unplayed language the plain fill already used, just
         * shaped instead of flat. */
        if (wave_loaded) {
            int wave_w = FB_W - 48;
            int played_col = dur > 0 ? WAVE_BUCKETS * pos / dur : 0;
            /* Reported live, twice: first as uneven spacing (a fixed col_w
             * drawn at an independently-truncated cx each iteration drifts
             * in and out of step with its own rounding -- fixed by sizing
             * each column off the real gap to the *next* column's own
             * start instead of a fixed average), then as uneven thickness
             * once that was fixed -- WAVE_BUCKETS not dividing wave_w
             * evenly meant some columns were still 1px wider than others
             * no matter how the gap-sizing worked, since that's a genuine
             * integer-remainder problem, not a rounding bug. Fixed at the
             * source instead (see WAVE_BUCKETS's own comment): 144 divides
             * this screen's 432px exactly, so cx1-cx0 below is a constant
             * 3 for every column, not merely close. */
            /* R94 follow-up: reported live as general lag -- np_col_accent()/
             * np_col_line() were being called (and np_col_line() doing real
             * float HSL/blend math) once per column, up to 144 times every
             * single frame, recomputing the exact same two colours each
             * time since neither changes mid-frame. Hoisted out to once. */
            uint16_t wave_accent = np_col_accent(), wave_line = np_col_line();
            for (int c = 0; c < WAVE_BUCKETS; c++) {
                int h = max_h * wave_buckets[c] / 255;
                if (h < 2) h = 2;
                int cx0 = 24 + c * wave_w / WAVE_BUCKETS;
                int cx1 = 24 + (c + 1) * wave_w / WAVE_BUCKETS;
                int col_w = cx1 - cx0;
                if (col_w < 1) col_w = 1;
                fill_rect(fb, cx0, wave_cy - h / 2, col_w > 1 ? col_w - 1 : col_w, h,
                          c <= played_col ? wave_accent : wave_line);
            }
            if (scrub_active) {
                int w = dur > 0 ? wave_w * pos / dur : 0;
                if (w > wave_w) w = wave_w;
                fill_circle(fb, 24 + w, wave_cy, 13, np_col_accent());
            }
        } else {
            fill_rect(fb, 24, byy, FB_W - 48, bh, np_col_line());
            if (dur > 0) {
                int w = (FB_W - 48) * pos / dur;
                if (w > FB_W - 48) w = FB_W - 48;
                fill_rect(fb, 24, byy, w, bh, np_col_accent());
                if (scrub_active)
                    fill_circle(fb, 24 + w, wave_cy, 13, np_col_accent());
            }
        }
        snprintf(buf, sizeof(buf), "%d:%02d", pos / 60000, (pos / 1000) % 60);
        draw_text(fb, 24, clock_y, buf, np_col_dim(), TEXT_PX_SMALL, FB_W);
        int rem = dur - pos; if (rem < 0) rem = 0;
        /* Real-world time, not content time -- same reasoning as the
         * audiobook screen's Book/Chapter countdowns: position/duration
         * stay in content time (unaffected by speed, what the bar fill
         * above already assumes), but the countdown label should shrink
         * faster at faster speeds. Music has no speed control, so this is
         * a no-op there (pod_speed_permille only changes in podcast_mode). */
        if (podcast_mode)
            rem = (int)(rem / (pod_speed_permille / 1000.0));
        snprintf(buf, sizeof(buf), "-%d:%02d", rem / 60000, (rem / 1000) % 60);
        draw_right_col(fb, clock_y, buf, np_col_dim());

        /* BG40: was +58, tight enough against the clock row above (ends
         * around by+36) that the gap read as uneven next to the bigger one
         * below the buttons. +70 splits the difference more evenly. */
        int cyy = by + 70 + CTRL_NUDGE_PX;        /* centre line of the transport (R83) */
        int mid = FB_W / 2;

        if (podcast_mode) {
            /* BG47 (revised): just two skip buttons -- -10s left of
             * play/pause, +30s right of it, per explicit correction away
             * from the original symmetric +/-10/+/-30 four-button set.
             * Vendored Font Awesome arrow-rotate icons (icon_skip_back/
             * _forward, icons_data.h), same pair the audiobook screen's
             * +/-10s uses -- replaced this app's own hand-drawn arc
             * approximation (draw_skip_arc(), removed) on explicit request.
             * Speed control is unchanged from
             * before: ported from that same audiobook screen's concentric-
             * ring control and cycling behaviour, backed by its own
             * pod_speed_permille rather than ab_speed_permille -- separate
             * modes, no reason a podcast's chosen speed should share state
             * with a book's. POD_SKIP_OFF is shared with the tap handler
             * below so the two cannot drift apart -- same reasoning as
             * bar_y(). */
            draw_icon(fb, FB_W, FB_H, mid - POD_SKIP_OFF - icon_skip_back.w / 2,
                     cyy - icon_skip_back.h / 2, &icon_skip_back, np_col_fg());
            draw_icon(fb, FB_W, FB_H, mid + POD_SKIP_OFF - icon_skip_forward.w / 2,
                     cyy - icon_skip_forward.h / 2, &icon_skip_forward, np_col_fg());
            const char *n10 = "10s", *n30 = "30s";
            int w10 = text_width(n10, TEXT_PX_SMALL), w30 = text_width(n30, TEXT_PX_SMALL);
            draw_text(fb, mid - POD_SKIP_OFF - w10 / 2, cyy - TEXT_PX_SMALL / 2 + 2, n10, np_col_fg(), TEXT_PX_SMALL, FB_W);
            draw_text(fb, mid + POD_SKIP_OFF - w30 / 2, cyy - TEXT_PX_SMALL / 2 + 2, n30, np_col_fg(), TEXT_PX_SMALL, FB_W);

            int scx = pod_speed_x(mid), scy = cyy;
            fill_circle(fb, scx, scy, 22, COL_LINE);
            fill_circle(fb, scx, scy, 20, np_col_bg());
            snprintf(buf, sizeof(buf), "%.1f\xc3\x97", pod_speed_permille / 1000.0);
            int sw = text_width(buf, TEXT_PX_SMALL);
            draw_text(fb, scx - sw / 2, scy - TEXT_PX_SMALL / 2 + 2, buf, np_col_fg(), TEXT_PX_SMALL, FB_W);

            /* Show-notes info icon, right of the +30s arc -- moved here
             * from the top-right corner of the cover art per explicit
             * request. Same ring-then-fill trick as the speed control just
             * drawn: two concentric fill_circle()s, since fill_circle only
             * ever draws solid. */
            if (pod_notes_avail) {
                int icx = pod_info_x(mid), icy = cyy;
                fill_circle(fb, icx, icy, 22, COL_LINE);
                fill_circle(fb, icx, icy, 20, np_col_bg());
                draw_text(fb, icx - 3, icy - TEXT_PX_BODY / 2 + 2, "i", np_col_fg(), TEXT_PX_BODY, FB_W);
            }
        } else {
            /* previous: triangle against a bar. Offset 96, not the old 128 --
             * matches the audiobook player's own skip-arc spacing exactly
             * (see its "off = 96" comment), pulled in per explicit request so
             * this row reads as one cluster rather than two icons stranded
             * out at the edges. R47's mode button below took the room this
             * freed on the left. */
            fill_triangle(fb, mid - 96, cyy, 34, -1, np_col_fg());
            fill_rect(fb, mid - 96 - 15 - 5, cyy - 17, 5, 34, np_col_fg());
            /* next: mirrored */
            fill_triangle(fb, mid + 96, cyy, 34, +1, np_col_fg());
            fill_rect(fb, mid + 96 + 15, cyy - 17, 5, 34, np_col_fg());

            /* R47: Normal -> Shuffle -> Repeat -> Normal, one button. Same
             * slot the audiobook player uses for its own extra control past
             * the skip rings (its speed ring sits at mid-96-70) -- placing
             * this anywhere else was what read as "not in line with" that
             * screen. Ring is COL_DIM always, same weight as the artist
             * line -- a first attempt made it COL_TEXT/COL_ACCENT, too bold
             * for a secondary control; only the icon inside brightens to
             * COL_ACCENT when a mode is actually engaged.
             *
             * Reported live: the plain "S"/"R"/"-" letters replaced with
             * real icons -- bars (default/off), repeat, shuffle, vendored
             * through the same gen_icons.py pipeline as every other icon,
             * grouped as one family (see gen_icons.py's own comment) so
             * the three share a crop box and come out the same visual
             * weight, since they swap for each other at this exact spot. */
            int pmx = mid - 96 - 82, pmy = cyy;
            int pm_on = shuffle_enabled || repeat_mode != REPEAT_OFF;
            uint16_t pm_circle = np_col_circle();
            fill_circle(fb, pmx, pmy, 26, pm_circle);
            fill_circle(fb, pmx, pmy, 25, np_col_bg());
            const icon_t *pmicon = shuffle_enabled ? &icon_mode_shuffle
                                  : repeat_mode != REPEAT_OFF ? &icon_mode_repeat
                                  : &icon_mode_off;
            /* Engaged (shuffle/repeat on) keeps its existing accent
             * highlight, unaffected by R96 -- that distinction wasn't
             * part of what was reported, only the plain/off colour was. */
            draw_icon(fb, FB_W, FB_H, pmx - 13, pmy - 13, pmicon,
                      pm_on ? np_col_accent() : pm_circle);
        }

        fill_circle(fb, mid, cyy, 42, np_col_accent());
        if (audio_is_paused()) {
            /* nudged right: a triangle's visual centre is left of its bounding
             * box, and centring the box makes it look like it is falling over */
            fill_triangle(fb, mid + 4, cyy, 36, +1, np_col_bg());
        } else {
            fill_rect(fb, mid - 15, cyy - 18, 10, 36, np_col_bg());
            fill_rect(fb, mid + 5,  cyy - 18, 10, 36, np_col_bg());
        }

        /* R93: moved from the bottom-right corner (a leftover from when the
         * header owned that spot) into the transport row itself, mirroring
         * the mode button's own ring-then-icon circle on the opposite side
         * of the skip buttons, per explicit request. Same radii/cutout as
         * that button so the two read as a matched pair. Music only --
         * podcast_mode's row already fills that same slot with its own
         * info-circle (pod_info_x()), so it keeps the plain corner icon
         * rather than colliding with it. */
        if (!podcast_mode) {
            int qx = mid + 96 + 82, qy = cyy;
            fill_circle(fb, qx, qy, 26, np_col_circle());
            fill_circle(fb, qx, qy, 25, np_col_bg());
            draw_queue_icon(fb, qx - 13, qy - 9, np_col_circle());
        } else {
            draw_queue_icon(fb, FB_W - 24 - 26, FB_H - 32, COL_DIM);
        }
        return;
    }

    if (screen == SC_TRACKS && !ab_list && !pod_list) {
        /* R46: album detail as one continuous scroll -- cover, title, artist
         * and the tracks/format/duration line all move together with the
         * track list beneath them, rather than a fixed header sitting above
         * a separately-scrolling list. off is a plain pixel offset (same
         * technique Settings already uses for its own not-a-clean-multiple-
         * of-ROW_H content) rather than scroll indexing directly into
         * tracks[], since the header's own height isn't a ROW_H multiple. */
        /* Reported live: shadows the outer clip_bot (declared above, shared
         * by every other screen) with this screen's own -- matches
         * vis_rows()/tracks_max_px()'s reasoning exactly, and has to, since
         * a mismatch here would mean the scroll bounds and what's actually
         * drawn disagree about where the bottom of the content is. */
        int clip_bot = FB_H - (mini_visible() ? MINI_H : (sheet_note[0] ? 40 : 0));
        int off = scroll * ROW_H + scroll_px;
        int header_h = tracks_hdr_h();

        /* R63: skip the cover box entirely once view_art_gone() has confirmed
         * there's nothing to show -- tracks_hdr_title_y() (and everything
         * derived from it) has already collapsed the reserved space to
         * match, so drawing an ART_PX COL_ROW placeholder here would just
         * paint a big blank square nothing above it left room to explain. */
        if (!g_view_art_gone_frame) {
            int cover_y = 0 - off;
            fill_rect_clip(fb, 0, cover_y, ART_PX, ART_PX, COL_ROW, 0, clip_bot);
            view_blit_art_clip(fb, 0, cover_y, 0, clip_bot);
        }

        draw_text_clip(fb, 24, tracks_hdr_title_y() - off, cur_album,
                       COL_TEXT, TEXT_PX_TITLE, FB_W - 24, 0, clip_bot);
        /* Blank rather than a guessed label when an album has no unified
         * album_artist tag -- same "blank rather than a wrong guess"
         * reasoning the disc-number column above already follows. */
        if (cur_artist[0])
            draw_text_clip(fb, 24, tracks_hdr_artist_y() - off, cur_artist,
                           COL_DIM, TEXT_PX_BODY, FB_W - 24, 0, clip_bot);

        {
            int iy = tracks_hdr_info_y() - off;
            char cbuf[24], fbuf[64], dbuf[16];
            snprintf(cbuf, sizeof(cbuf), "%d track%s", track_n, track_n == 1 ? "" : "s");
            lib_track_t *t0 = track_n > 0 ? &tracks[0] : NULL;
            fbuf[0] = '\0';
            if (t0)
                snprintf(fbuf, sizeof(fbuf), "%s  %d/%g kHz",
                         track_format_name(t0), t0->bits, t0->rate / 1000.0);
            int64_t total_ms = 0;
            for (int i = 0; i < track_n; i++) total_ms += tracks[i].dur_ms;
            fmt_dur(dbuf, sizeof(dbuf), total_ms);

            int ix = 24;
            draw_text_clip(fb, ix, iy, cbuf, COL_DIM, TEXT_PX_SMALL, FB_W, 0, clip_bot);
            ix += text_width(cbuf, TEXT_PX_SMALL);
            if (fbuf[0]) {
                draw_text_clip(fb, ix, iy, "  \xc2\xb7  ", COL_DIM, TEXT_PX_SMALL, FB_W, 0, clip_bot);
                ix += text_width("  \xc2\xb7  ", TEXT_PX_SMALL);
                draw_text_clip(fb, ix, iy, fbuf, COL_ACCENT, TEXT_PX_SMALL, FB_W, 0, clip_bot);
                ix += text_width(fbuf, TEXT_PX_SMALL);
            }
            draw_text_clip(fb, ix, iy, "  \xc2\xb7  ", COL_DIM, TEXT_PX_SMALL, FB_W, 0, clip_bot);
            ix += text_width("  \xc2\xb7  ", TEXT_PX_SMALL);
            draw_text_clip(fb, ix, iy, dbuf, COL_DIM, TEXT_PX_SMALL, FB_W, 0, clip_bot);
        }
        fill_rect_clip(fb, 0, header_h - off - 1, FB_W, 1, COL_LINE, 0, clip_bot);

        /* Reported live: a full-width banner (icon, "Disc N", that disc's
         * own total playtime) before each disc's first track, replacing
         * the old inline number next to the track number. Walks from
         * idx == 0 every draw rather than starting from a first_idx shortcut
         * the way this loop used to -- banners make a row's position
         * depend on how many came before it, not just a flat idx * ROW_H,
         * so there's no cheap way to jump straight to an arbitrary offset
         * any more. track_n tops out in the low hundreds, so walking all of
         * it every redraw is not a real cost -- tracks_max_px()'s own
         * duration-summing loop already does the same over every track on
         * every scroll_to_px() call. */
        int multi_disc = tracks_multi_disc();
        /* R70: drag-to-reorder/swipe-to-remove, playlists only -- never a
         * plain album (its order is the album's, not the user's to edit). */
        int show_edit = browsing_is_playlist;
        int ry = header_h;
        int last_disc = -999;
        for (int idx = 0; idx < track_n; idx++) {
            lib_track_t *t = &tracks[idx];
            int disc = track_disc_num(idx);
            if (multi_disc && disc != last_disc) {
                int by = ry - off;
                if (by + DISC_BANNER_H > 0 && by < clip_bot) {
                    int64_t disc_ms = 0;
                    for (int j = idx; j < track_n && track_disc_num(j) == disc; j++)
                        disc_ms += tracks[j].dur_ms;
                    char discbuf[24], dbuf[16];
                    snprintf(discbuf, sizeof(discbuf), "Disc %d", disc);
                    fmt_dur(dbuf, sizeof(dbuf), disc_ms);
                    fill_rect_clip(fb, 0, by, FB_W, DISC_BANNER_H, COL_HEADER, 0, clip_bot);
                    draw_disc_icon(fb, 20, by + (DISC_BANNER_H - 28) / 2, COL_DIM);
                    draw_text_clip(fb, 58, by + (DISC_BANNER_H - TEXT_PX_BODY) / 2 - 2, discbuf,
                                  COL_TEXT, TEXT_PX_BODY, FB_W - 140, 0, clip_bot);
                    draw_right_clip(fb, by + (DISC_BANNER_H - TEXT_PX_SMALL) / 2, dbuf, 0, clip_bot);
                    fill_rect_clip(fb, 0, by + DISC_BANNER_H - 1, FB_W, 1, COL_LINE, 0, clip_bot);
                }
                ry += DISC_BANNER_H;
                last_disc = disc;
            }
            int row_y = ry - off;
            if (row_y > clip_bot) break;   /* rows only get later from here -- nothing further can be visible */
            if (row_y + ROW_H > 0) {
                int playing = audio_is_active() && idx == cur_track &&
                             !strcmp(cur_album, q_album) && !strcmp(cur_artist, q_artist);
                /* R70: same shapes as SC_QUEUE's own dragging_this/
                 * swiping_this -- see that screen's own comments for why
                 * each highlight/reveal is drawn the way it is. */
                int dragging_this = show_edit && playlist_drag_active && idx == playlist_drag_idx;
                int swiping_this = show_edit && playlist_swipe_active && idx == playlist_swipe_idx;
                int dx0 = swiping_this ? playlist_swipe_dx : 0;
                if (swiping_this)
                    fill_rect_clip(fb, 0, row_y, FB_W, ROW_H, COL_ACCENT, 0, clip_bot);
                if (playing && !swiping_this) {
                    fill_rect_clip(fb, 0, row_y, FB_W, ROW_H, COL_ROW, 0, clip_bot);
                    fill_rect_clip(fb, 0, row_y, 4, ROW_H, COL_ACCENT, 0, clip_bot);
                }
                if (dragging_this) {
                    if (!playing) fill_rect_clip(fb, 0, row_y, FB_W, ROW_H, COL_ROW, 0, clip_bot);
                    fill_rect_clip(fb, 0, row_y, FB_W, 2, COL_ACCENT, 0, clip_bot);
                    fill_rect_clip(fb, 0, row_y + ROW_H - 2, FB_W, 2, COL_ACCENT, 0, clip_bot);
                }
                /* BG-playlist: t->track is the source album's own track
                 * number, leftover metadata like t->disc -- meaningless (and
                 * actively misleading) for a playlist's own order, so this
                 * row never shows one at all. */
                if (show_edit)      buf[0] = '\0';
                else if (t->track > 0) snprintf(buf, sizeof(buf), "%d", t->track);
                else                buf[0] = '\0';
                draw_text_clip(fb, 20 + dx0, row_y + 22, buf, COL_DIM, TEXT_PX_SMALL,
                              56 + dx0, 0, clip_bot);
                draw_text_clip(fb, 68 + dx0, row_y + 20, t->name, playing ? COL_ACCENT : COL_TEXT,
                              TEXT_PX_BODY, (show_edit ? FB_W - 150 : FB_W - 110) + dx0, 0, clip_bot);
                if (t->dur_ms > 0) {
                    fmt_dur(buf, sizeof(buf), t->dur_ms);
                    if (show_edit) {
                        int bw = text_width(buf, TEXT_PX_SMALL);
                        int right = FB_W - 24 - 40;
                        draw_text_clip(fb, right - bw + dx0, row_y + 22, buf, COL_DIM,
                                       TEXT_PX_SMALL, FB_W, 0, clip_bot);
                    } else {
                        draw_right_clip(fb, row_y + 22, buf, 0, clip_bot);
                    }
                }
                if (show_edit)
                    draw_grip_icon_clip(fb, FB_W - 24 - 16 + dx0, row_y + (ROW_H - 24) / 2,
                                         (playing || dragging_this) ? COL_ACCENT : COL_DIM,
                                         0, clip_bot);
                if (!dragging_this && !swiping_this)
                    fill_rect_clip(fb, 0, row_y + ROW_H - 1, FB_W, 1, COL_LINE, 0, clip_bot);
            }
            ry += ROW_H;
        }
        if (sheet_note[0] && !mini_visible())
            draw_text(fb, 24, FB_H - 34, sheet_note, COL_ACCENT, TEXT_PX_SMALL, FB_W - 48);
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_ARTIST_PAGE) {
        /* Same continuous-scroll shape as the album page above: photo,
         * name, a summary line, then content beneath all move together as
         * one pixel offset -- here the content is an album list followed
         * by the bio text rather than a track list. */
        int clip_bot = FB_H - (mini_visible() ? MINI_H : 0);
        int off = scroll * ROW_H + scroll_px;
        int header_h = artist_page_hdr_h();

        int photo_y = 0 - off;
        fill_rect_clip(fb, 0, photo_y, ART_PX, ART_PX, COL_ROW, 0, clip_bot);
        artist_blit_art_clip(fb, 0, photo_y, 0, clip_bot);

        draw_text_clip(fb, 24, artist_page_title_y() - off, artist_page_name,
                       COL_TEXT, TEXT_PX_TITLE, FB_W - 24, 0, clip_bot);
        {
            char cbuf[32];
            snprintf(cbuf, sizeof(cbuf), "%d album%s", artist_page_album_n,
                     artist_page_album_n == 1 ? "" : "s");
            draw_text_clip(fb, 24, artist_page_info_y() - off, cbuf,
                           COL_DIM, TEXT_PX_SMALL, FB_W, 0, clip_bot);
        }
        fill_rect_clip(fb, 0, header_h - off - 1, FB_W, 1, COL_LINE, 0, clip_bot);

        for (int idx = 0; idx < artist_page_album_n; idx++) {
            int ry = header_h + idx * ROW_H - off;
            if (ry + ROW_H < 0) continue;
            if (ry > clip_bot) break;
            lib_row_t *r = &artist_page_albums[idx];
            draw_text_clip(fb, 24, ry + 22, r->name, COL_TEXT,
                          TEXT_PX_BODY, FB_W - 90, 0, clip_bot);
            char cbuf[16];
            snprintf(cbuf, sizeof(cbuf), "%d", r->count);
            draw_right_clip(fb, ry + 22, cbuf, 0, clip_bot);
            fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, 0, clip_bot);
        }

        int bio_y = header_h + artist_page_album_n * ROW_H;
        if (artist_art_loading) {
            int ly = bio_y + 24 - off;
            if (ly + 30 > 0 && ly < clip_bot)
                draw_text_clip(fb, 24, ly, "Loading artist info...", COL_DIM,
                               TEXT_PX_SMALL, FB_W - 48, 0, clip_bot);
        } else if (artist_bio_text[0]) {
            artist_bio_layout(fb, 24, bio_y + 24, FB_W - 48, off, 0, clip_bot);
        }

        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_TRACKS) {
        /* R36: mark disc boundaries in a multi-disc album -- ab_list/pod_list
         * rows all carry disc == -1 (audiobook.c/podcast.c never set it), so
         * they normalise to the same value and multi_disc is always false
         * for chapters and episodes, exactly as it should be. Single-disc
         * albums are also excluded on purpose: every track there would
         * otherwise show the same "[1]" marker for no reason. */
        int multi_disc = 0;
        if (!ab_list && !pod_list && track_n > 1) {
            int first_disc = tracks[0].disc > 0 ? tracks[0].disc : 1;
            for (int i = 1; i < track_n; i++) {
                int d = tracks[i].disc > 0 ? tracks[i].disc : 1;
                if (d != first_disc) { multi_disc = 1; break; }
            }
        }
        /* Reached from Now Playing this doubles as the queue, so the playing
         * track is marked wherever the list is entered from. */
        for (int i = 0; i < vis_rows(); i++) {
            int idx = scroll + i;
            if (idx >= track_n) break;
            lib_track_t *t = &tracks[idx];
            /* pod_list: cur_track/queue[] identify the playing episode by a
             * one-entry queue (see podcast_mode's comment), not by an index
             * into this freshly-reloaded feed list -- idx == cur_track would
             * just mark whichever episode happens to sort first. Matched by
             * path instead, the same thing a fresh pod_load_episodes() call
             * can't be expected to keep a stable index for. */
            int playing = pod_list
                ? (audio_is_active() && podcast_mode && cur_track == 0 &&
                   queue_n > 0 && t->path[0] && !strcmp(t->path, queue[0].path))
                : (audio_is_active() && idx == cur_track &&
                   !strcmp(cur_album, q_album) && !strcmp(cur_artist, q_artist));
            /* R70: swipe-to-remove reveal, episodes only -- only ever true for
             * a downloaded one (pod_remove_download()'s own gate), so the
             * duration column below is always the branch showing during a
             * swipe, never the Download affordance. */
            int swiping_this = pod_list && pod_swipe_active && idx == pod_swipe_idx;
            int dx0 = swiping_this ? pod_swipe_dx : 0;
            if (swiping_this)
                fill_rect_clip(fb, 0, y, FB_W, ROW_H, COL_ACCENT, CONTENT_Y, clip_bot);
            if (playing && !swiping_this) {
                fill_rect_clip(fb, 0, y, FB_W, ROW_H, COL_ROW, CONTENT_Y, clip_bot);
                fill_rect_clip(fb, 0, y, 4, ROW_H, COL_ACCENT, CONTENT_Y, clip_bot);
            }
            /* The number the file states. Blank rather than a dash when the
             * file does not say, which is rare now it is read from tags. */
            int disc = t->disc > 0 ? t->disc : 1;
            int disc_start = multi_disc &&
                (idx == 0 || (tracks[idx - 1].disc > 0 ? tracks[idx - 1].disc : 1) != disc);
            /* Two fixed columns when multi_disc, so every track number lines
             * up regardless of which rows happen to carry a disc marker --
             * the disc digit only occupies its own column on the first
             * track of each disc, the track column itself never moves. */
            int track_x = multi_disc ? 44 : 20;
            if (disc_start) {
                char discbuf[8];
                snprintf(discbuf, sizeof(discbuf), "%d", disc);
                /* right_edge is an absolute clip x, not a width -- 40, not
                 * 20, so the disc digit itself has room to draw. */
                draw_text_clip(fb, 20, y + 22, discbuf, COL_ACCENT, TEXT_PX_SMALL, 40, CONTENT_Y, clip_bot);
            }
            if (t->track > 0) snprintf(buf, sizeof(buf), "%d", t->track);
            else              buf[0] = '\0';
            draw_text_clip(fb, track_x, y + 22, buf, COL_DIM, TEXT_PX_SMALL,
                          track_x + 36, CONTENT_Y, clip_bot);
            /* FB_W - 110, matching every other row in this file that reserves
             * space for a short right-aligned figure (album/track counts):
             * index_visible() is false on this screen, so draw_right_clip's
             * duration text right-aligns to FB_W-24 with no index-strip
             * allowance, and a duration string never needs more than that.
             * This was FB_W-150, leaving ~76px of clipped title unused for
             * no reason (BG10). */
            /* Per BACKLOG.md's L5 entry: dimmed = not downloaded yet, the
             * same visual language a disabled control uses elsewhere in
             * this app, so the list reads at a glance which rows play and
             * which only start a download. */
            draw_text_clip(fb, 68 + dx0, y + 20, t->name,
                          playing ? COL_ACCENT
                          : (pod_list && !pod_eps[idx].downloaded) ? COL_DIM
                          : COL_TEXT,
                          TEXT_PX_BODY, FB_W - 110,
                          CONTENT_Y, clip_bot);
            /* pod_list: an episode not yet downloaded has no duration to
             * show (t->dur_ms is 0, same as any file that isn't there yet)
             * -- show the download affordance in that slot instead, or its
             * live progress if this is the one downloading right now. */
            if (pod_list && !pod_eps[idx].downloaded) {
                if (pod_download_active() && pod_download_slot() == idx) {
                    long tot = pod_download_total();
                    long got = pod_download_bytes();
                    /* BG110: belt-and-suspenders alongside podcast.c's own
                     * fix -- bytes should never exceed a correctly-parsed
                     * total, but showing KB instead of a nonsense/overflowed
                     * percent if it ever does (a still-wrong header, a
                     * chunked response with no Content-Length at all) beats
                     * a six-digit or negative number on screen either way. */
                    if (tot > 0 && got >= 0 && got <= tot)
                        snprintf(buf, sizeof(buf), "%ld%%", got * 100 / tot);
                    else
                        snprintf(buf, sizeof(buf), "%ld KB", got / 1024);
                } else {
                    snprintf(buf, sizeof(buf), "Download");
                }
                draw_right_clip(fb, y + 22, buf, CONTENT_Y, clip_bot);
            } else if (t->dur_ms > 0) {
                fmt_dur(buf, sizeof(buf), t->dur_ms);
                if (dx0) {
                    int bw = text_width(buf, TEXT_PX_SMALL);
                    int right = FB_W - 24 - (index_visible() ? INDEX_W : 0);
                    draw_text_clip(fb, right - bw + dx0, y + 22, buf, COL_DIM,
                                   TEXT_PX_SMALL, FB_W, CONTENT_Y, clip_bot);
                } else {
                    draw_right_clip(fb, y + 22, buf, CONTENT_Y, clip_bot);
                }
            }
            if (!swiping_this)
                fill_rect_clip(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
            y += ROW_H;
        }
        if (sheet_note[0] && !mini_visible()) {
            draw_text(fb, 24, FB_H - 34, sheet_note, COL_ACCENT, TEXT_PX_SMALL, FB_W - 48);
        } else if (track_n > 0 && !mini_visible() && !ab_list && !pod_list) {
            /* Chapters (audiobook_mode) reuses this screen but never the SQL
             * index -- t->format/bits/rate are never populated for them (see
             * audiobook.h), so this line would show a meaningless "FLAC
             * 0/0 kHz" rather than nothing. */
            lib_track_t *t = &tracks[0];
            snprintf(buf, sizeof(buf), "%s  %d/%g kHz",
                     track_format_name(t), t->bits, t->rate / 1000.0);
            draw_text(fb, 24, FB_H - 34, buf, COL_ACCENT, TEXT_PX_SMALL, FB_W - 24);
        }
        return;
    }

    if (screen == SC_POD_SYNC) {
        draw_text(fb, 24, y + 6, pod_update_running() ? "Syncing every feed..."
                                : pod_update_died()    ? "Sync stopped early."
                                                        : "Sync finished.",
                  pod_update_died() ? RGB(230, 80, 70) : COL_DIM, TEXT_PX_BODY, FB_W - 48);
        y += 46;
        fill_rect(fb, 0, y - 1, FB_W, 1, COL_LINE);
        for (int i = 0; i < pod_sync_log_n; i++) {
            draw_text_clip(fb, 24, y + 14, pod_sync_log[i], COL_TEXT, TEXT_PX_SMALL,
                           FB_W - 48, CONTENT_Y, clip_bot);
            y += 40;
        }
        if (pod_sync_log_n == 0)
            draw_text(fb, 24, y + 14, "Starting...", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
        return;
    }

    if (screen == SC_EQ) {
        int ry = eq_row_enabled_y();
        draw_text(fb, 24, ry + 24, "Enabled", COL_TEXT, TEXT_PX_BODY, FB_W - 140);
        draw_toggle_switch(fb, ry, eq_enabled());
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry = eq_row_profile_y();
        draw_text(fb, 24, ry + 24, "Profile", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 24, eq_cur_path[0] ? eq_cur.name : "None");
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        int py = eq_preamp_y();
        int pby = py + 26, pbw = FB_W - 48;
        /* While the finger is down on this bar, show where it actually is
         * rather than the last committed value -- same trick the scrub bar
         * uses (scrub_ms() against live_x), so the slider visibly tracks the
         * drag instead of only jumping on release. */
        float shown_preamp = eq_cur.preamp_db;
        if (eq_dragging == 1) {
            int px = live_x - 24; if (px < 0) px = 0; if (px > pbw) px = pbw;
            shown_preamp = -12.0f + 18.0f * (float)px / (float)pbw;
        }
        draw_text(fb, 24, py, "Preamp", COL_DIM, TEXT_PX_SMALL, FB_W - 160);
        snprintf(buf, sizeof(buf), "%.1f dB", shown_preamp);
        draw_right(fb, py, buf);
        fill_pill(fb, 24, pby, pbw, 6, COL_LINE);
        float pt = (shown_preamp + 12.0f) / 18.0f;   /* -12..+6 dB */
        if (pt < 0) pt = 0; if (pt > 1) pt = 1;
        int pw = (int)(pbw * pt);
        if (pw > 0) fill_pill(fb, 24, pby, pw, 6, COL_ACCENT);
        fill_circle(fb, 24 + pw, pby + 3, 12, COL_ACCENT);

        /* Response curve: real, from the live coefficients, not illustrative --
         * eq_response_db() is the same evaluation eq_process_* is built on. */
        int cy = eq_curve_y(), cgw = FB_W - 48, cgh = 50;
        float freqs[28], resp[28];
        for (int i = 0; i < 28; i++) freqs[i] = eq_freq_from_t((float)i / 27.0f);
        eq_response_db(freqs, resp, 28);
        fill_rect(fb, 24, cy + cgh / 2, cgw, 1, COL_LINE);
        int lx = -1, ly = -1;
        for (int i = 0; i < 28; i++) {
            float d = resp[i];
            if (d > 18.0f) d = 18.0f; else if (d < -18.0f) d = -18.0f;
            int xx = 24 + cgw * i / 27;
            int yy = cy + cgh / 2 - (int)(d / 18.0f * (cgh / 2));
            if (lx >= 0) draw_line(fb, lx, ly, xx, yy, COL_ACCENT);
            lx = xx; ly = yy;
        }

        ry = eq_row_bands_y();
        draw_text(fb, 24, ry + 24, "Bands", COL_TEXT, TEXT_PX_BODY, FB_W - 140);
        snprintf(buf, sizeof(buf), "%d", eq_cur.band_n);
        draw_right(fb, ry + 24, buf);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_MSEB) {
        int ry = mseb_row_enabled_y();
        draw_text(fb, 24, ry + 24, "Enabled", COL_TEXT, TEXT_PX_BODY, FB_W - 140);
        draw_toggle_switch(fb, ry, mseb_on);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        int span = FB_W - 48;
        for (int i = 0; i < MSEB_BAND_N; i++) {
            int by = mseb_band_row_y(i);
            /* Same live-tracking trick as the preamp slider on SC_EQ: while
             * this band is being dragged, show where the finger actually is
             * rather than the last committed value. */
            float g = mseb_gain[i];
            if (mseb_dragging == i) {
                int px = live_x - 24; if (px < 0) px = 0; if (px > span) px = span;
                g = -12.0f + 24.0f * (float)px / (float)span;
            }
            uint16_t c = mseb_on ? COL_TEXT : COL_DIM;
            draw_text(fb, 24, by + 2, EQ_MSEB_BANDS[i].name, c, TEXT_PX_SMALL, span - 100);
            snprintf(buf, sizeof(buf), "%+.1f dB", g);
            draw_right(fb, by + 2, buf);
            draw_text(fb, 24, by + 22, EQ_MSEB_BANDS[i].freq_label, COL_DIM, TEXT_PX_SMALL, span);

            int sy = by + 46;
            fill_pill(fb, 24, sy, span, 6, COL_LINE);
            fill_rect(fb, 24 + span / 2, sy - 4, 1, 14, COL_DIM);
            float t = (g + 12.0f) / 24.0f;
            if (t < 0) t = 0; if (t > 1) t = 1;
            int px = (int)(span * t), cx = span / 2;
            uint16_t fillc = mseb_on ? COL_ACCENT : COL_DIM;
            if (px > cx)      fill_pill(fb, 24 + cx, sy, px - cx, 6, fillc);
            else if (px < cx) fill_pill(fb, 24 + px, sy, cx - px, 6, fillc);
            fill_circle(fb, 24 + px, sy + 3, 11, fillc);

            if (i < MSEB_BAND_N - 1)
                fill_rect(fb, 0, by + MSEB_ROW_H - 1, FB_W, 1, COL_LINE);
        }
        return;
    }

    if (screen == SC_EQ_BANDS) {
        /* R76: smooth per-pixel scroll -- see SC_PLAYLISTS' own comment.
         * eq_cur.band[] is a small in-memory array, not database-paged, so
         * (unlike SC_ARTISTS/SC_ALBUMS -- deliberately left alone, see the
         * scrollable-screens note near the `continuous` flag below) there's
         * no data-window concern here, only draw position. */
        int off = scroll * ROW_H + scroll_px;
        for (int idx = 0; idx < eq_cur.band_n; idx++) {
            int ry = CONTENT_Y + idx * ROW_H - off;
            if (ry + ROW_H < CONTENT_Y) continue;
            if (ry > clip_bot) break;
            eq_band_t *b = &eq_cur.band[idx];
            const char *tn = b->type == EQ_PEAK ? "Peak" :
                            b->type == EQ_LOW_SHELF ? "Low shelf" : "High shelf";
            snprintf(buf, sizeof(buf), "Band %d \xc2\xb7 %s", idx + 1, tn);
            draw_text_clip(fb, 24, ry + 14, buf, b->on ? COL_TEXT : COL_DIM,
                          TEXT_PX_BODY, FB_W - 110, CONTENT_Y, clip_bot);
            snprintf(buf, sizeof(buf), "%.0f Hz  \xc2\xb7  %+.1f dB  \xc2\xb7  Q %.2f",
                    b->fc, b->gain_db, b->q);
            draw_text_clip(fb, 24, ry + 42, buf, COL_DIM, TEXT_PX_SMALL,
                          FB_W - 110, CONTENT_Y, clip_bot);
            draw_toggle_switch_h_clip(fb, ry, b->on, ROW_H, CONTENT_Y, clip_bot);
            fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        }
        if (eq_cur.band_n == 0)
            draw_text(fb, 24, CONTENT_Y + 20, "No bands in this profile", COL_DIM, TEXT_PX_BODY, FB_W - 40);
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_EQ_BAND) {
        eq_band_t *b = (eq_editing_band >= 0 && eq_editing_band < eq_cur.band_n)
                     ? &eq_cur.band[eq_editing_band] : NULL;
        if (!b) return;

        int ey = eq_enabled_y();
        draw_text(fb, 24, ey + 4, "Enabled", COL_TEXT, TEXT_PX_BODY, FB_W - 140);
        draw_toggle_switch(fb, ey - 12, b->on);

        int ty = eq_type_y();
        draw_text(fb, 24, ty - 26, "Filter type", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
        int segw = (FB_W - 48) / 3;
        const char *names[3] = { "Low shelf", "Peak", "High shelf" };
        for (int s = 0; s < 3; s++) {
            int sx = 24 + segw * s;
            int selected = (int)b->type == s;
            if (selected) fill_rect(fb, sx + 2, ty, segw - 4, 34, COL_ACCENT);
            int tw2 = text_width(names[s], TEXT_PX_SMALL);
            draw_text(fb, sx + (segw - tw2) / 2, ty + 9, names[s],
                     selected ? COL_BG : COL_DIM, TEXT_PX_SMALL, FB_W);
        }

        int sw = FB_W - 48;

        int fy = eq_freq_y();
        float shown_fc = b->fc;
        if (eq_dragging == 2) {
            int px = live_x - 24; if (px < 0) px = 0; if (px > sw) px = sw;
            shown_fc = eq_freq_from_t((float)px / (float)sw);
        }
        draw_text(fb, 24, fy - 24, "Frequency", COL_DIM, TEXT_PX_SMALL, FB_W - 160);
        snprintf(buf, sizeof(buf), "%.0f Hz", shown_fc);
        draw_right(fb, fy - 24, buf);
        fill_pill(fb, 24, fy, sw, 4, COL_LINE);
        float ft = eq_t_from_freq(shown_fc);
        int fx = 24 + (int)(sw * ft);
        if (fx > 24) fill_pill(fb, 24, fy, fx - 24, 4, COL_ACCENT);
        fill_circle(fb, fx, fy + 2, 11, COL_ACCENT);

        int gy = eq_gain_y();
        float shown_gain = b->gain_db;
        if (eq_dragging == 3) {
            int px = live_x - 24; if (px < 0) px = 0; if (px > sw) px = sw;
            shown_gain = -12.0f + 24.0f * (float)px / (float)sw;
        }
        draw_text(fb, 24, gy - 24, "Gain", COL_DIM, TEXT_PX_SMALL, FB_W - 160);
        snprintf(buf, sizeof(buf), "%+.1f dB", shown_gain);
        draw_right(fb, gy - 24, buf);
        fill_pill(fb, 24, gy, sw, 4, COL_LINE);
        float gt = (shown_gain + 12.0f) / 24.0f;
        if (gt < 0) gt = 0; if (gt > 1) gt = 1;
        int gx = 24 + (int)(sw * gt);
        if (gx > 24) fill_pill(fb, 24, gy, gx - 24, 4, COL_ACCENT);
        fill_circle(fb, gx, gy + 2, 11, COL_ACCENT);

        int qy = eq_q_y();
        float shown_q = b->q;
        if (eq_dragging == 4) {
            int px = live_x - 24; if (px < 0) px = 0; if (px > sw) px = sw;
            shown_q = 0.1f + 9.9f * (float)px / (float)sw;
        }
        draw_text(fb, 24, qy - 24, "Q", COL_DIM, TEXT_PX_SMALL, FB_W - 160);
        snprintf(buf, sizeof(buf), "%.2f", shown_q);
        draw_right(fb, qy - 24, buf);
        fill_pill(fb, 24, qy, sw, 4, COL_LINE);
        float qt = (shown_q - 0.1f) / (10.0f - 0.1f);
        if (qt < 0) qt = 0; if (qt > 1) qt = 1;
        int qx = 24 + (int)(sw * qt);
        if (qx > 24) fill_pill(fb, 24, qy, qx - 24, 4, COL_ACCENT);
        fill_circle(fb, qx, qy + 2, 11, COL_ACCENT);

        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS) {
        /* Each divider belongs between two complete settings -- title *and*
         * its own description -- not between a title and the description
         * explaining that same title. Drawing it at "this row's bottom"
         * put it one slot too early throughout: it cut "Power button lock"
         * off from its own description instead of separating that
         * description from "Idle sleep" below it. Drawn at the top of the
         * row that follows instead, so it always closes off the block above
         * it, whatever that block was. */
        /* Each setting's right-side control (toggle or value) is centered on
         * the FULL divider-bound block for that setting, not just the 72px
         * title slice -- rows with a two-line description below the title
         * (lock, usbbypass, autooff) have a visual "area" spanning both, and
         * a control centered on only the title slice reads as sitting too
         * high against that whole card (BG63). */
        /* R44: Settings outgrew one screen the moment Timezone joined Theme
         * below Auto shutdown, hence the scroll offset applied to every row
         * from here on. Unlike the library lists, whose rows only ever sit
         * below CONTENT_Y in the first place, Settings' own rows start right
         * at the top -- scrolling can carry one up into the header/status
         * strip, and plain draw_text/fill_rect only clip against the screen
         * edges (y<0, y>=FB_H), not against CONTENT_Y. Live-tested this
         * missing the first time: row text visibly wrote over the header
         * while scrolling up. Every call below is the _clip variant with
         * clip_top=CONTENT_Y for that reason -- including the toggle
         * switches (BG69): a row-height gate that skips the whole draw call
         * once fully out of bounds isn't enough on its own, since the
         * toggle's own pixels are centred within the row rather than
         * starting at its nominal top, and can still straddle the header
         * line even while that gate says "still in bounds". */
        int off = scroll * ROW_H + scroll_px;
        int clip_bot = FB_H;
        int ry = set_row_lock_y() - off;
        int lock_h = set_row_usbbypass_y() - set_row_lock_y();
        draw_text_clip(fb, 24, ry + 20, "Power button lock", COL_TEXT, TEXT_PX_BODY, FB_W - 140, CONTENT_Y, clip_bot);
        draw_toggle_switch_h_clip(fb, ry, button_lock_enabled, lock_h, CONTENT_Y, clip_bot);

        int dy = set_lock_desc_y() - off;
        draw_text_clip(fb, 24, dy, "Double-press power to lock the screen and", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, dy + 26, "disable buttons. Double-press again to undo.", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);

        ry = set_row_usbbypass_y() - off;
        int usbbypass_h = set_row_btautoplay_y() - set_row_usbbypass_y();
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 20, "USB Transport Mode", COL_TEXT, TEXT_PX_BODY, FB_W - 140, CONTENT_Y, clip_bot);
        draw_toggle_switch_h_clip(fb, ry, usb_bypass_enabled, usbbypass_h, CONTENT_Y, clip_bot);

        int uy = set_usbbypass_desc_y() - off;
        draw_text_clip(fb, 24, uy, "Disables PEQ/MSEB/Bluetooth and locks volume", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, uy + 26, "at 50% while output is USB. Restored after.", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);

        ry = set_row_btautoplay_y() - off;
        int btautoplay_h = set_row_btvolsteps_y() - set_row_btautoplay_y();
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 20, "Play on headset connect", COL_TEXT, TEXT_PX_BODY, FB_W - 140, CONTENT_Y, clip_bot);
        draw_toggle_switch_h_clip(fb, ry, bt_autoplay_enabled, btautoplay_h, CONTENT_Y, clip_bot);

        int py = set_btautoplay_desc_y() - off;
        draw_text_clip(fb, 24, py, "Resume playback automatically when a", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, py + 26, "Bluetooth headset reconnects.", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);

        ry = set_row_btvolsteps_y() - off;
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 20, "Volume steps", COL_TEXT, TEXT_PX_BODY, FB_W - 200, CONTENT_Y, clip_bot);
        snprintf(buf, sizeof(buf), "%d", audio_bt_vol_steps());
        draw_right_col_clip(fb, ry + ROW_H / 2 - TEXT_PX_SMALL / 2, buf, COL_ACCENT, CONTENT_Y, clip_bot);

        ry = set_row_autooff_y() - off;
        int autooff_h = set_row_lighttheme_y() - set_row_autooff_y();
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 20, "Auto shutdown", COL_TEXT, TEXT_PX_BODY, FB_W - 200, CONTENT_Y, clip_bot);
        if (auto_off_minutes() == 0) snprintf(buf, sizeof(buf), "Never");
        else                         snprintf(buf, sizeof(buf), "%d min", auto_off_minutes());
        draw_right_col_clip(fb, ry + autooff_h / 2 - TEXT_PX_SMALL / 2, buf, COL_ACCENT, CONTENT_Y, clip_bot);

        int ay = set_autooff_desc_y() - off;
        draw_text_clip(fb, 24, ay, "Powers the device off when locked with", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ay + 26, "nothing playing. Tap to change.", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);

        ry = set_row_lighttheme_y() - off;
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 20, "Theme", COL_TEXT, TEXT_PX_BODY, FB_W - 200, CONTENT_Y, clip_bot);
        /* R45: opens a picker like Accent colour/Timezone below, rather than
         * R43's plain cycle-tap -- four values (Dark/Light/Grey/Auto) reads
         * better as a named list than a tap-to-advance control. */
        draw_right_col_clip(fb, ry + ROW_H / 2 - TEXT_PX_SMALL / 2,
                            THEME_MODE_NAMES[theme_mode], COL_ACCENT, CONTENT_Y, clip_bot);

        ry = set_row_timezone_y() - off;
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 20, "Timezone", COL_TEXT, TEXT_PX_BODY, FB_W - 200, CONTENT_Y, clip_bot);
        draw_right_col_clip(fb, ry + ROW_H / 2 - TEXT_PX_SMALL / 2,
                            TZ_PRESETS[tz_idx].name, COL_ACCENT, CONTENT_Y, clip_bot);

        ry = set_row_theme_y() - off;
        int theme_h = set_row_coverpalette_y() - set_row_theme_y();
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 20, "Accent colour", COL_TEXT, TEXT_PX_BODY, FB_W - 200, CONTENT_Y, clip_bot);
        /* The value itself renders in the accent colour it names, rather
         * than a separate swatch blob next to plain text (BG63 follow-up). */
        draw_right_col_clip(fb, ry + theme_h / 2 - TEXT_PX_SMALL / 2,
                            ACCENT_PRESETS[g_accent_idx].name, ACCENT_PRESETS[g_accent_idx].color, CONTENT_Y, clip_bot);

        ry = set_row_coverpalette_y() - off;
        int coverpalette_h = set_row_about_y() - set_row_coverpalette_y();
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 20, "Cover colours", COL_TEXT, TEXT_PX_BODY, FB_W - 140, CONTENT_Y, clip_bot);
        draw_toggle_switch_h_clip(fb, ry, cover_palette_enabled, coverpalette_h, CONTENT_Y, clip_bot);

        int cpy = set_coverpalette_desc_y() - off;
        draw_text_clip(fb, 24, cpy, "Now Playing's background and accent follow", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, cpy + 26, "the current album art (Music only).", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);

        ry = set_row_about_y() - off;
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 20, "About", COL_TEXT, TEXT_PX_BODY, FB_W - 200, CONTENT_Y, clip_bot);
        /* Reindex used to sit here and drew this row's closing line as part
         * of its own block (see its own since-removed comment) -- now that
         * it's gone, About draws its own trailing divider instead of
         * leaving Wi-Fi with no line above it. */
        fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        ry = set_row_wifi_y() - off;
        draw_text_clip(fb, 24, ry + 20, "Wi-Fi", COL_TEXT, TEXT_PX_BODY, FB_W - 200, CONTENT_Y, clip_bot);
        {
            int on = st_wifi_on();
            char nm[64] = "";
            if (on) st_wifi_ssid(nm, sizeof(nm));
            draw_right_clip(fb, ry + 20, on ? (nm[0] ? nm : "not connected") : "off", CONTENT_Y, clip_bot);
        }
        fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        ry = set_row_bt_y() - off;
        draw_text_clip(fb, 24, ry + 20, "Bluetooth", COL_TEXT, TEXT_PX_BODY, FB_W - 200, CONTENT_Y, clip_bot);
        {
            int on = st_bt_on();
            char nm[64] = "";
            if (on) st_bt_name(nm, sizeof(nm));
            draw_right_clip(fb, ry + 20, on ? (nm[0] ? nm : "not connected") : "off", CONTENT_Y, clip_bot);
        }
        fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        ry = set_row_usb_y() - off;
        draw_text_clip(fb, 24, ry + 20, "USB working mode", COL_TEXT, TEXT_PX_BODY, FB_W - 200, CONTENT_Y, clip_bot);
        {
            int m = st_usb_mode();
            draw_right_clip(fb, ry + 20, m == 0 ? "ADB" : m == 1 ? "USB Storage" : "Unplugged", CONTENT_Y, clip_bot);
        }
        fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        ry = set_row_scan_y() - off;
        int scan_disabled = st_usb_mode() == 1;
        draw_text_clip(fb, 24, ry + 20, "Scan library",
                       scan_disabled ? COL_DIM : COL_TEXT, TEXT_PX_BODY,
                       FB_W - 200, CONTENT_Y, clip_bot);
        {
            /* One row, two passes underneath (scanner_rescan_now() kicks
             * both -- see its own comment). Reports scanner.c's own count,
             * the one that actually answers "how much music is there" --
             * index.c's pass is the quieter per-track detail fill-in behind
             * it and doesn't need its own number here. Still shown running
             * if either is, so the row doesn't read "done" while index.c is
             * still working through what this pass just found. */
            int scanned = 0, written = 0;
            int started = scanner_scan_progress(&scanned, &written);
            if (scan_disabled)
                snprintf(buf, sizeof(buf), "USB Storage active");
            else if (scanner_scan_running() || index_scan_running())
                snprintf(buf, sizeof(buf), "Scanning… %d", scanned);
            else if (started)
                snprintf(buf, sizeof(buf), "%d files found", scanned);
            else
                snprintf(buf, sizeof(buf), "Not started");
            draw_right_clip(fb, ry + 20, buf, CONTENT_Y, clip_bot);
        }
        fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS_ABOUT) {
        char kernel[64], firmware[32], serial[40], sd_free[24], ram_free[24];
        about_kernel(kernel, sizeof(kernel));
        about_firmware(firmware, sizeof(firmware));
        about_serial(serial, sizeof(serial));
        about_sd_free(sd_free, sizeof(sd_free));
        about_ram_free(ram_free, sizeof(ram_free));

        int ry = CONTENT_Y;
        draw_text(fb, 24, ry + 20, "Libra version", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, LIBRARY_VERSION);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "Firmware version", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, firmware);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "Kernel version", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, kernel);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "Serial number", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, serial);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "SD card", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, sd_free);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        ry += ROW_H;
        draw_text(fb, 24, ry + 20, "Memory", COL_TEXT, TEXT_PX_BODY, FB_W - 200);
        draw_right(fb, ry + 20, ram_free);
        fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);

        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS_WIFI) {
        /* R75: same off/_clip shape SC_SETTINGS' own longer list already
         * uses -- see that screen's comment on why _clip specifically
         * (rows can scroll up into the header, and plain draw_text/
         * fill_rect only clip against the physical screen edges, not
         * CONTENT_Y). */
        int off = scroll * ROW_H + scroll_px;
        int clip_bot = mini_visible() ? FB_H - MINI_H : FB_H;
        int ry = CONTENT_Y - off;
        int on = st_wifi_on();
        draw_text_clip(fb, 24, ry + 20, "Wi-Fi", COL_TEXT, TEXT_PX_BODY, FB_W - 140, CONTENT_Y, clip_bot);
        draw_toggle_switch_h_clip(fb, ry, on, ROW_H, CONTENT_Y, clip_bot);
        ry += ROW_H;
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        char nm[64] = "";
        if (on) st_wifi_ssid(nm, sizeof(nm));
        draw_text_clip(fb, 24, ry + 20, "Status", COL_TEXT, TEXT_PX_BODY, FB_W - 24, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 46, on ? (nm[0] ? nm : "not connected") : "off",
                      COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);
        ry += ROW_H;
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        /* RP2: scan-and-select, the same shape Bluetooth's own settings
         * screen already has. Manual entry stays alongside it, not
         * replaced -- a hidden network never shows up in any scan, so it
         * remains the only way to join one. */
        draw_text_clip(fb, 24, ry + 20, "Scan for networks", COL_ACCENT, TEXT_PX_BODY, FB_W - 48, CONTENT_Y, clip_bot);
        ry += ROW_H;
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        draw_text_clip(fb, 24, ry + 20, "Add network manually", COL_ACCENT, TEXT_PX_BODY, FB_W - 48, CONTENT_Y, clip_bot);
        ry += ROW_H;
        fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        /* Same 2s refresh cadence as SC_SETTINGS_BT's own scan list, and
         * the same reasoning: this screen rides the once-a-second clock-tick
         * redraw, and a wpa_cli round trip on every one of those would be
         * wasted work for a list a scan-in-progress is still trying to grow. */
        time_t now = time(NULL);
        if (now - wifi_nets_refresh_at >= 2) {
            wifi_net_n = wifi_scan_results(wifi_nets, 10);
            wifi_nets_refresh_at = now;
        }
        if (wifi_net_n == 0) {
            draw_text_clip(fb, 24, ry + 20, "No networks found yet", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);
        } else {
            for (int i = 0; i < wifi_net_n; i++) {
                draw_text_clip(fb, 24, ry + 20, wifi_nets[i].ssid,
                              COL_TEXT, TEXT_PX_BODY, FB_W - 90, CONTENT_Y, clip_bot);
                draw_right_clip(fb, ry + 20, wifi_nets[i].open ? "open" : "secured", CONTENT_Y, clip_bot);
                ry += ROW_H;
                fill_rect_clip(fb, 0, ry - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
            }
        }
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS_BT) {
        /* BG109: off-based/_clip scrolling, same shape as SC_SETTINGS_WIFI
         * (R75) just above. */
        int clip_bot = mini_visible() ? FB_H - MINI_H : FB_H;
        int off = scroll * ROW_H + scroll_px;
        int on = st_bt_on();
        int ry;

        ry = CONTENT_Y - off;
        draw_text_clip(fb, 24, ry + 20, "Bluetooth", COL_TEXT, TEXT_PX_BODY, FB_W - 140, CONTENT_Y, clip_bot);
        draw_toggle_switch_h_clip(fb, ry, on, ROW_H, CONTENT_Y, clip_bot);
        fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        char nm[64] = "";
        if (on) st_bt_name(nm, sizeof(nm));
        ry = CONTENT_Y + ROW_H - off;
        draw_text_clip(fb, 24, ry + 20, "Status", COL_TEXT, TEXT_PX_BODY, FB_W - 24, CONTENT_Y, clip_bot);
        draw_text_clip(fb, 24, ry + 46, on ? (nm[0] ? nm : "not connected") : "off",
                  COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);
        fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        ry = CONTENT_Y + 2 * ROW_H - off;
        draw_text_clip(fb, 24, ry + 20, "Scan for devices", COL_ACCENT, TEXT_PX_BODY, FB_W - 48, CONTENT_Y, clip_bot);
        fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);

        /* Refreshed at most every 2s, not on every redraw -- this screen
         * gets pulled into the once-a-second clock-tick redraw the same as
         * every other screen (see the sec != last_sec dirty trigger), and
         * a popen() to bluetoothctl on every one of those would be wasted
         * work for a list that hasn't changed. Same shape as st_bt_name()'s
         * own 10s cache, just a shorter window since this list is what a
         * scan-in-progress is actively trying to grow. */
        time_t now = time(NULL);
        if (now - bt_devs_refresh_at >= 2) {
            bt_dev_n = bt_scan_devices(bt_devs, 8);
            bt_devs_refresh_at = now;
        }
        if (bt_dev_n == 0) {
            ry = CONTENT_Y + 3 * ROW_H - off;
            draw_text_clip(fb, 24, ry + 20, "No devices found yet", COL_DIM, TEXT_PX_SMALL, FB_W - 48, CONTENT_Y, clip_bot);
        } else {
            for (int i = 0; i < bt_dev_n; i++) {
                ry = CONTENT_Y + (3 + i) * ROW_H - off;
                if (ry + ROW_H < CONTENT_Y) continue;
                if (ry > clip_bot) break;
                draw_text_clip(fb, 24, ry + 20, bt_devs[i].name[0] ? bt_devs[i].name : bt_devs[i].mac,
                              COL_TEXT, TEXT_PX_BODY, FB_W - 48, CONTENT_Y, clip_bot);
                draw_right_clip(fb, ry + 20, bt_devs[i].mac, CONTENT_Y, clip_bot);
                fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
            }
        }
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS_THEME) {
        for (int i = 0; i < ACCENT_N; i++) {
            int ry = CONTENT_Y + i * ROW_H;
            fill_circle(fb, 44, ry + ROW_H / 2, 14, ACCENT_PRESETS[i].color);
            draw_text(fb, 72, ry + 20, ACCENT_PRESETS[i].name, COL_TEXT, TEXT_PX_BODY, FB_W - 140);
            if (i == g_accent_idx) {
                /* A tick from two line segments, matching the app's other
                 * line-primitive glyphs rather than a font checkmark. */
                int cx = FB_W - 44, cy = ry + ROW_H / 2;
                draw_line(fb, cx - 10, cy, cx - 3, cy + 7, COL_ACCENT);
                draw_line(fb, cx - 3, cy + 7, cx + 10, cy - 8, COL_ACCENT);
            }
            fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);
        }
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS_TIMEZONE) {
        /* R44: 33 entries, unlike Accent colour's 7 -- scrolls the same way
         * Settings itself now does, rather than the fixed one-screen list
         * above, and needs the same clip_top=CONTENT_Y treatment for the
         * same reason (see Settings' own comment on this). */
        int off = scroll * ROW_H + scroll_px;
        for (int i = 0; i < TZ_N; i++) {
            int ry = CONTENT_Y + i * ROW_H - off;
            draw_text_clip(fb, 24, ry + 20, TZ_PRESETS[i].name, COL_TEXT, TEXT_PX_BODY, FB_W - 100, CONTENT_Y, FB_H);
            if (i == tz_idx && ry + ROW_H > CONTENT_Y) {
                int cx = FB_W - 44, cy = ry + ROW_H / 2;
                draw_line(fb, cx - 10, cy, cx - 3, cy + 7, COL_ACCENT);
                draw_line(fb, cx - 3, cy + 7, cx + 10, cy - 8, COL_ACCENT);
            }
            fill_rect_clip(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, FB_H);
        }
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS_THEMEMODE) {
        /* R45: four entries, fits in one screen like Accent colour -- no
         * scroll needed. */
        for (int i = 0; i < THEME_MODE_N; i++) {
            int ry = CONTENT_Y + i * ROW_H;
            draw_text(fb, 24, ry + 20, THEME_MODE_NAMES[i], COL_TEXT, TEXT_PX_BODY, FB_W - 100);
            if (i == theme_mode) {
                int cx = FB_W - 44, cy = ry + ROW_H / 2;
                draw_line(fb, cx - 10, cy, cx - 3, cy + 7, COL_ACCENT);
                draw_line(fb, cx - 3, cy + 7, cx + 10, cy - 8, COL_ACCENT);
            }
            fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);
        }
        if (mini_visible()) draw_mini(fb);
        return;
    }

    if (screen == SC_SETTINGS_USB) {
        /* ADB / Storage, driven by stock's own adbon/adboff -- see
         * st_usb_mode()'s own comment in status.c. DAC and OTG need
         * exclusive gadget ownership the same way Storage does, and
         * aren't exposed here at all -- stock's own System menu still has
         * them, untouched. */
        static const char *names[2] = { "ADB", "USB Storage" };
        int cur = st_usb_mode();
        for (int i = 0; i < 2; i++) {
            int ry = CONTENT_Y + i * ROW_H;
            draw_text(fb, 24, ry + 20, names[i], COL_TEXT, TEXT_PX_BODY, FB_W - 100);
            if (i == cur) {
                int cx = FB_W - 44, cy = ry + ROW_H / 2;
                draw_line(fb, cx - 10, cy, cx - 3, cy + 7, COL_ACCENT);
                draw_line(fb, cx - 3, cy + 7, cx + 10, cy - 8, COL_ACCENT);
            }
            fill_rect(fb, 0, ry + ROW_H - 1, FB_W, 1, COL_LINE);
        }
        int dy = CONTENT_Y + 2 * ROW_H + 20;
        draw_text(fb, 24, dy, "Switching to USB Storage disconnects ADB --", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
        draw_text(fb, 24, dy + 26, "the two need the USB port exclusively.", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
        if (mini_visible()) draw_mini(fb);
        return;
    }

    for (int i = 0; i < vis_rows(); i++) {
        int absolute = scroll + i;
        lib_row_t *row = row_at(absolute);
        if (!row) break;
        /* Stop drawing at the end of whatever top-level letter the index put
         * us in — a plain jump to a short letter (E: 4 albums, room for 8)
         * as much as a locked subdivision (Es within E). Leaves blank space
         * below rather than padding out with the next letter. */
        if (index_lock_end >= 0 && absolute >= index_lock_end) break;
        draw_text_clip(fb, 24, y + 20, row->name, COL_TEXT, TEXT_PX_BODY, FB_W - 110,
                       CONTENT_Y, clip_bot);
        if (row->count > 0) {
            snprintf(buf, sizeof(buf), "%d", row->count);
            draw_right_clip(fb, y + 22, buf, CONTENT_Y, clip_bot);
        }
        fill_rect_clip(fb, 0, y + ROW_H - 1, FB_W, 1, COL_LINE, CONTENT_Y, clip_bot);
        y += ROW_H;
    }
    /* Every other screen branch above ends with this same check before its
     * own return -- this shared fallthrough (Artists/Albums/Audiobooks/
     * Podcasts, the only row_at()-driven lists with no earlier explicit
     * branch) had gone without it: clip_bot already reserves MINI_H of
     * space for it via mini_visible() above, so rows correctly stopped
     * short of the bottom, but nothing ever painted into the gap that left. */
    if (mini_visible()) draw_mini(fb);
}

/* ---- input --------------------------------------------------------------- */
/* Returns 1 on a tap, with the coordinates; 0 otherwise. Same shape as the
 * Podcasts app: the node is drained every pass so nothing queues against our
 * grab and replays later. */
/* 1 = tap at (ox,oy); 2 = vertical drag, oy carries the distance. A drag has to
 * be distinguished from a tap or every scroll also opens whatever was under the
 * finger. */
/* RBR: bumped 18 -> 24. Reported live as "the capture area for a tap is too
 * small" on Settings specifically -- extensive live testing (precise
 * synthetic taps at exact row boundaries, every Settings sub-screen) never
 * reproduced an actual hitbox/draw-geometry mismatch; every row's tap zone
 * matched what was drawn exactly. Left as a best-effort widening of the
 * plain threshold rather than a confirmed root-cause fix: Settings rows
 * carry more text than most lists (title + description), so a tap aimed at
 * a specific line rather than dead-center of the row is more likely to
 * carry a few extra pixels of aim/tremor than this app's list screens
 * generally see. */
#define DRAG_MIN 24
/* A fixed 18px threshold, checked continuously from the moment of touch-down,
 * misclassifies both directions: a fast, decisive tap with a little finger
 * tremor easily exceeds 18px and reads as a scroll, while a slow, deliberate
 * small drag can stay under it and reads as a tap. A real scroll and a real
 * tap differ in more than distance -- a scroll is sustained motion, a tap is
 * quick -- so touches still within this short a window of going down get a
 * more generous distance allowance before being called "moved" at all. */
#define FAST_TAP_MS   150
#define FAST_DRAG_MIN 30

/* 1 = tap, 2 = vertical drag (*oy carries the distance), 3 = swipe in from the
 * left edge (back), 4 = press and hold (handled entirely outside this
 * function, via touch_down/live_x/live_y and hold_fired -- not actually
 * returned from here, but grouped into this list since it's a gesture of
 * the same kind), 5 = swipe up from the bottom edge (R80: home, always).
 * The edge test is on where the finger went down, so a horizontal or
 * vertical drag started mid-screen is not mistaken for either edge
 * gesture. */
#define EDGE_ZONE  100
#define EDGE_TRAVEL 60
#define HOLD_MS     550
/* R80: deliberately much narrower than EDGE_ZONE -- a normal scroll to the
 * end of a list with nothing playing (mini_visible() false, so a list can
 * genuinely reach the very bottom row) is also an upward drag, and could
 * start anywhere the last row happens to sit. Narrow enough that only a
 * swipe starting right at the physical bezel reaches it, the same
 * distinction a phone's own system home-gesture strip relies on. */
#define HOME_EDGE_ZONE   40
#define HOME_EDGE_TRAVEL 60

/* Live state for the back gesture, so the UI can show it happening rather than
 * only reacting once the finger is lifted. */
static int edge_active, edge_travel, edge_y;
/* Same, for the bottom-edge home swipe. */
static int home_edge_active, home_edge_travel, home_edge_x;

/* Live state for the press-and-hold. Timing the press from when the *release*
 * is read does not work: both can arrive in the same polling pass and the hold
 * then measures as instant. The hold is detected while the finger is still
 * down, which is also how a hold is supposed to feel — it fires under your
 * finger rather than when you let go. */
static int touch_down, touch_x, touch_y, touch_moved, hold_fired;
static struct timespec touch_at;

/* R59: holding a skip arc repeats it, escalating the per-repeat amount the
 * longer it's held -- reuses hold_fired (above) to suppress the release-tap
 * handler's own single skip once a hold has already fired at least one.
 * skip_hold_dir is 0 (nothing armed yet this touch), -1 (holding the back
 * arc) or +1 (holding the forward one) -- fixed at the moment the first
 * repeat fires (from touch_x/touch_y, stable since a hold requires
 * !touch_moved) so a hold can't retarget mid-gesture by drifting between
 * zones. skip_hold_next_ms is the held-time threshold for the *next*
 * repeat, in the same touch_at-relative clock the long-press sheet's own
 * detection already uses. */
static int  skip_hold_dir;
static long skip_hold_next_ms;
#define SKIP_REPEAT_MS 350   /* cadence once a hold is repeating */

/* Escalation schedule, in the ticket's own words: "first hold = skip 10s,
 * hold longer = skip 30s, hold even longer = skip 1 min." Keyed on total
 * held time so it doesn't matter whether the interval above changes later --
 * hold longer, bigger jumps, however often they land. */
static int skip_hold_amount(long held_ms) {
    if (held_ms < 3000) return 10000;
    if (held_ms < 6000) return 30000;
    return 60000;
}

/* -1/+1/0 (back/forward/not a skip zone) for a point in SC_PLAYING's
 * transport row, mirroring the release-tap handler's own hit zones exactly
 * -- audiobook's plain left/right thirds, podcast's midpoint-of-adjacent-
 * centres zones (BG47) -- since a hold has to target the same button a tap
 * there would have. Only the two skip zones matter here; every other zone
 * (play/pause, speed ring, notes) has nothing to escalate by holding it. */
static int skip_zone_at(int x, int y) {
    if (audiobook_mode) {
        int cby = ab_chapter_bar_y();
        int cyy = cby + 58 + CTRL_NUDGE_PX;
        if (y <= cyy - 48 || y >= cyy + 48) return 0;
        if (x < FB_W / 3) return -1;
        if (x > 2 * FB_W / 3) return 1;
        return 0;
    }
    if (podcast_mode) {
        int bary = bar_y();
        int cyy = bary + 70 + CTRL_NUDGE_PX;
        if (y <= cyy - 48 || y >= cyy + 48) return 0;
        int mid = FB_W / 2;
        int x10 = mid - POD_SKIP_OFF, xp30 = mid + POD_SKIP_OFF;
        int xspd = pod_speed_x(mid), xicon = pod_info_x(mid);
        if (x >= (xspd + x10) / 2 && x < (x10 + mid) / 2) return -1;
        if (x >= (mid + xp30) / 2 && x < (!pod_notes_avail ? FB_W : (xp30 + xicon) / 2)) return 1;
        return 0;
    }
    return 0;
}

/* Elapsed ms since touch-down (touch_at), then DRAG_MIN or the more
 * generous FAST_DRAG_MIN depending on whether that's still within the fast-
 * tap window. Shared by both axes below so a diagonal touch is judged by
 * one consistent age, not whichever axis's event happened to arrive first. */
static int drag_threshold(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - touch_at.tv_sec) * 1000L +
              (now.tv_nsec - touch_at.tv_nsec) / 1000000L;
    return ms < FAST_TAP_MS ? FAST_DRAG_MIN : DRAG_MIN;
}

static int read_gesture(int fd, int *ox, int *oy) {
    r1_input_event_t ev;
    static int x, y, down_x = -1, down_y = -1, moved, have_down;
    int out = 0;
    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) {
            x = ev.value;
            live_x = x;
            /* Sideways counts as movement for the hold, though not for the
             * scroll: a slow swipe in from the edge has no vertical travel at
             * all, so tracking only Y made it look like a stationary finger
             * and it opened the hold menu instead of going back. */
            if (have_down && down_x >= 0 && abs(x - down_x) > drag_threshold())
                touch_moved = 1;
        }
        else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) {
            y = ev.value;
            live_y = y;
            if (have_down && down_y >= 0 && abs(y - down_y) > drag_threshold()) {
                moved = 1;
                touch_moved = 1;
            }
        }
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value == 1) {
                down_x = x; down_y = y; moved = 0; have_down = 1;
                edge_active = 0; edge_travel = 0;
                home_edge_active = 0; home_edge_travel = 0;
                touch_down = 1; touch_x = x; touch_y = y;
                touch_moved = 0; hold_fired = 0;
                skip_hold_dir = 0;   /* R59: nothing armed yet this touch */
                clock_gettime(CLOCK_MONOTONIC, &touch_at);
            }
            else if (have_down) {
                have_down = 0;
                edge_active = 0;
                home_edge_active = 0;
                touch_down = 0;
                int dx = x - down_x, dy = y - down_y;
                /* Not while scrubbing, and not with quick settings open. The
                 * progress bar and the brightness slider both start at x=24,
                 * well inside the edge zone, so dragging from the start of
                 * either was indistinguishable from a swipe back — the
                 * scrub bar was fixed for this already; the brightness
                 * slider shared the same geometry and was never added. On
                 * SC_MENU, go_back() returning 0 exits the app outright,
                 * which is what "the screen went black" during a brightness
                 * drag actually was. The EQ preamp/frequency/gain/Q sliders
                 * start at the same x=24 and share the guard for the same
                 * reason. */
                if (!scrub_active && !qs_open && !eq_dragging && !title_dragging && down_x >= 0 && down_x < EDGE_ZONE &&
                    dx > EDGE_TRAVEL && dx > abs(dy)) {
                    out = 3;
                /* R80: same exclusions as the back gesture, for the same
                 * reasons -- the scrub bar and every slider that starts at
                 * y near the bottom of their own screens would otherwise be
                 * indistinguishable from a swipe starting at the bezel. */
                } else if (!scrub_active && !qs_open && !eq_dragging && !title_dragging &&
                           down_y >= 0 && down_y > FB_H - HOME_EDGE_ZONE &&
                           (down_y - y) > HOME_EDGE_TRAVEL && (down_y - y) > abs(dx)) {
                    out = 5;
                } else if (moved) {
                    out = 2; *oy = dy;
                } else if (hold_fired) {
                    out = 0;              /* the hold already acted; not a tap */
                } else {
                    out = 1; *ox = x; *oy = y;
                }
                down_x = down_y = -1;
            }
        }

        /* Deliberately outside the if/else-if chain above. Written as another
         * branch of it, this swallowed the touch-release event whenever an
         * edge drag was in progress — the gesture never completed, so the
         * hint appeared and stayed put and nothing ever went back. It is not
         * a kind of event; it is an observation about where the finger is. */
        if (!scrub_active && !qs_open && !eq_dragging && !title_dragging && have_down && down_x >= 0 && down_x < EDGE_ZONE) {
            int edx = x - down_x;
            if (edx > 4 && edx > abs(y - down_y)) {
                edge_active = 1;
                edge_travel = edx;
                edge_y = y;
            }
        }
        /* R80: same live-preview reasoning as the back gesture just above,
         * for the bottom-edge home swipe. */
        if (!scrub_active && !qs_open && !eq_dragging && !title_dragging &&
            have_down && down_y >= 0 && down_y > FB_H - HOME_EDGE_ZONE) {
            int up = down_y - y;
            if (up > 4 && up > abs(x - down_x)) {
                home_edge_active = 1;
                home_edge_travel = up;
                home_edge_x = x;
            }
        }
    }
    return out;
}

/* Each screen returns early once it has drawn, so the mini player goes on
 * afterwards rather than being repeated at the foot of every branch. */
/* One definition of "back", so the header control and the edge swipe cannot
 * drift apart. Returns 0 when there is nowhere left to go, which at the top
 * level means leaving the app. */
static int go_back(void) {
    /* Show notes intercept, not guard: closing them has to happen *here*,
     * ahead of everything else below, so both ways into "back" -- the edge
     * swipe and the header BACK control, which both just call go_back() --
     * close the notes first rather than leaving the player under them. Same
     * "one definition of back" property the function comment above is
     * about, just extended to a second thing back can mean on this screen. */
    if (screen == SC_PLAYING && podcast_mode && pod_notes_showing) {
        pod_notes_showing = 0;
        return 1;
    }
    /* Every way out of a screen goes through here, including all the way
     * out of the app (SC_MENU returning 0), so this is the one place that
     * needs to know about leaving rather than every call site. A no-op
     * whenever a book isn't what's playing. Likewise pod_save_current_pos()
     * for an episode. */
    ab_save_current_pos();
    pod_save_current_pos();
    switch (screen) {
        case SC_MENU:
            /* Standalone: nowhere to go -- see g_is_standalone's own
             * comment. Swallow the gesture (return 1, "did something")
             * rather than let the caller treat 0 as "leave the app". */
            return g_is_standalone ? 1 : 0;
        case SC_PLAYING:
            if (radio_mode) { screen = SC_RADIO; reset_scroll(); break; }
            /* BG73 follow-up: reported live -- once Play Next has spliced in
             * a track from a different album, this queue no longer
             * represents one browsable album, so presenting it as a single
             * title+cover page (the rest of this case, below) is actively
             * misleading -- the very first row can be from an album the
             * header no longer names. Land on the plain queue list instead,
             * the same place the dedicated queue button goes. Audiobooks and
             * podcasts never set queue_mixed (they don't go through
             * queue_play_next()), so this can't misfire for them. */
            if (queue_mixed) {
                screen = SC_QUEUE; reset_scroll();
                queue_via_back = 1;
                break;
            }
            /* Straight from the queue copy — no second trip to the database,
             * and it is right even if the browser has wandered off. For a
             * book the queue is its chapters, so this is the chapter list.
             *
             * R70 fix: a plain album is the one exception -- it re-queries
             * fresh instead, so a queue reorder can't leak into what's
             * supposed to be the album's own stable, tag/DB-derived order.
             * Reported live as "One Vision" (from A Kind of Magic, no less)
             * appearing to move within the *album* itself after being
             * dragged in the Queue screen -- nothing on disk or in the SQL
             * index was actually touched (R70's reorder only ever mutates
             * queue[]/shuffle_order[] in memory), but handing that reordered
             * queue[] to tracks[] via a plain memcpy and presenting it as
             * the album's own browsable order is a real display bug
             * regardless of what's true underneath. Playlists/chapters/
             * episodes still take the memcpy below: a playlist's queue *is*
             * its order (and per R70 a reorder there is meant to end up
             * persisted, not routed around), and there's no equivalent
             * fresh query this cheap for chapters or episodes.
             *
             * cur_track indexes into tracks[] everywhere below this (see
             * BG73's own comment on tracks[cur_track] a little further down)
             * -- after a fresh query, tracks[] is a different array in a
             * different order than queue[], so cur_track has to be remapped
             * by path rather than carried over as a raw index. */
            if (!audiobook_mode && !podcast_mode && !q_is_playlist) {
                char playing_path[LIB_PATH_LEN];
                snprintf(playing_path, sizeof(playing_path), "%s",
                         (cur_track >= 0 && cur_track < queue_n) ? queue[cur_track].path : "");
                track_n = lib_tracks_for_album(q_artist, q_album, tracks,
                                               (int)(sizeof(tracks) / sizeof(tracks[0])), 0);
                for (int i = 0; i < track_n; i++)
                    if (!strcmp(tracks[i].path, playing_path)) { cur_track = i; break; }
            } else {
                memcpy(tracks, queue, sizeof(queue[0]) * (size_t)queue_n);
                track_n = queue_n;
            }
            snprintf(cur_artist, sizeof(cur_artist), "%s", q_artist);
            snprintf(cur_album,  sizeof(cur_album),  "%s", q_album);
            /* BG73: this view *is* the queue, so whether it's a playlist is
             * unambiguously q_is_playlist -- not whatever browsing_is_playlist
             * happened to be left at by incidental browsing since. Keeps a
             * later Play Next from this same view (queue_play_next() reads
             * browsing_is_playlist) from getting the wrong rule. */
            browsing_is_playlist = q_is_playlist;
            /* So a further "back" out of this queue view (BG7's fix) restores
             * this same artist rather than whatever an earlier, unrelated
             * Albums browse last left in albums_artist -- but only when we
             * actually got here without a normal Albums browse to fall back
             * on (played_from_browse false). When we did descend normally,
             * albums_artist already holds the right facet-column value and
             * q_artist (a different column, see the field's own comment)
             * would overwrite it with the wrong one. */
            if (!played_from_browse)
                snprintf(albums_artist, sizeof(albums_artist), "%s", q_artist);
            /* q_album is deliberately left empty for a podcast episode (see
             * pod_play_episode()) so the feed name has to come from
             * cur_feed instead -- otherwise SC_TRACKS's header would show a
             * blank title. And unlike a book's chapters, the queue copy above
             * is NOT the full episode list here -- podcast_mode's queue is
             * deliberately just the one playing episode (see its comment), so
             * "back" has to re-fetch the feed rather than mirror the queue,
             * or the list would collapse to a single row. */
            if (podcast_mode) {
                snprintf(cur_album, sizeof(cur_album), "%s", cur_feed);
                pod_ep_n = pod_load_episodes(cur_feed, pod_eps, POD_MAX_ITEMS);
                pod_rebuild_tracks();
            }
            ab_list = audiobook_mode;
            pod_list = podcast_mode;
            pod_notes_showing = 0;
            screen = SC_TRACKS;
            reset_scroll();
            /* R46 follow-up: Now Playing already showed this cover -- back
             * out of it landing on the *same* cover again just to re-scroll
             * past it read as pointless. Jumps straight past it to where the
             * title sits, ART_PX being exactly the scroll distance that
             * takes.
             *
             * BG75: reported live on a genuinely short album (3 tracks,
             * Bartok's Violin Concerto No. 2) -- its header alone (cover +
             * title/artist/info) is 622px, and only 216px of track rows sit
             * below it, so tracks_max_px() is 114, well short of ART_PX
             * (480). Calling scroll_to_px(ART_PX) directly sends that
             * through its rubber-band branch (damped to a third of the
             * overshoot past tracks_max_px()), landing scroll on neither a
             * clean top-of-tracklist nor the true content end -- a
             * half-scrolled cover with the title nowhere near the top,
             * which is what "doesn't scroll away" actually looked like. A
             * first attempt clamped the target to tracks_max_px() to at
             * least land cleanly (no gap below the last track), but that
             * still left the cover mostly uncleared -- for an album this
             * short there simply isn't enough tracklist beneath the cover
             * to push it fully off-screen while also ending exactly at the
             * last track. Confirmed live is the other side of that
             * trade-off: always land with the title at the top, gap below
             * the last track and all. That means bypassing scroll_to_px()'s
             * rubber-band/clamping entirely here -- this is a one-shot
             * placement, not a drag or inertia tick fighting to overscroll,
             * so there's nothing to damp against in the first place. */
            if (!ab_list && !pod_list) {
                scroll = ART_PX / ROW_H;
                scroll_px = ART_PX % ROW_H;
                /* BG71 sub-bug: swiping back from Now Playing lands here
                 * (the same rich album-detail page the queue button used
                 * to, before it got its own SC_QUEUE) -- reported live as
                 * showing whatever album was last *browsed*, not the one
                 * actually playing, because nothing here ever refreshed
                 * view_art_bits. It's a separate buffer from the playing
                 * track's own art specifically so the two can't leak into
                 * each other (BG70) -- but that also means arriving here
                 * has to explicitly ask for the right cover, the same way
                 * tapping an album row in Albums already does, rather than
                 * inheriting whatever the last view happened to leave.
                 *
                 * BG73 follow-up: tracks[0], not tracks[cur_track], used to
                 * be fine here since every entry in a queue was always from
                 * the one album cur_album/cur_artist named -- any track's
                 * path resolved to the same cover. Now that Play Next can
                 * splice in a track from a different album, tracks[0] can be
                 * a leftover from an album this queue has already moved on
                 * from (cur_album now names the *new* one), so it has to be
                 * the actually-playing entry's own path instead. */
                if (track_n > 0) {
                    int a = (cur_track >= 0 && cur_track < track_n) ? cur_track : 0;
                    view_art_request(tracks[a].path, cur_artist, cur_album);
                }
            }
            break;
        case SC_RADIO:
        case SC_AUDIOBOOKS:
        case SC_PODCASTS:
        case SC_EQ:
        case SC_MSEB:
        case SC_SETTINGS:
        case SC_MUSIC_MENU:
            screen = SC_MENU; reset_scroll();
            break;
        case SC_PLAYLISTS:
        case SC_ARTISTS:
            /* One level deeper than the rest (L2): these are reached via
             * "Music", not the top-level menu directly. */
            screen = SC_MUSIC_MENU; reset_scroll();
            break;
        case SC_EQ_BANDS:
            screen = SC_EQ; reset_scroll();
            break;
        case SC_KEYBOARD:
            /* BG107: reported live -- typing a new playlist's name and
             * swiping back out (rather than tapping Done) landed on
             * Albums with nothing created. No case existed for this
             * screen at all, so it fell into the default: case below,
             * built for SC_TRACKS/chapters/episodes -- none of that
             * case's flags (tracks_from_artist_page/pod_list/ab_list/
             * recent_mode) have anything to do with the keyboard, so it
             * always fell through to that case's own final fallback,
             * Albums, regardless of what screen actually opened the
             * keyboard. Same behaviour as tapping Cancel (kb_commit() is
             * deliberately not called here -- a back gesture reads as
             * "never mind", not "confirm"), so this also fixes the
             * identical gap for the Wi-Fi SSID/password keyboard, which
             * had the same missing case. */
            screen = kb_return_screen;
            break;
        case SC_QUEUE:
            /* Reported live: reached via the queue button, "back" means
             * "close the queue", landing on Now Playing is right. Reached as
             * the swipe-back-from-Now-Playing fallback for a mixed queue
             * (queue_via_back), it means "back out of Now Playing" -- and
             * there's no meaningful "browse this album" state to land on in
             * between the way the unmixed case has, since the queue no
             * longer is one album, so this continues straight out to Albums,
             * same as backing out of that unmixed album page a second time
             * eventually would. Otherwise this and the default case's own
             * "back to Albums" branch would bounce forever: SC_PLAYING's
             * go_back() sends a mixed queue straight back here, so landing
             * back on SC_PLAYING from here would just re-enter this case on
             * the very next back with nothing having changed. */
            if (queue_via_back) {
                /* BG87: albums_artist/albums_scroll_saved are written only
                 * at the moment an Albums row is actually tapped -- for a
                 * mixed queue that write can be from a much earlier,
                 * unrelated album browse, or may never have happened at all
                 * (Play Next can splice in a track from the Artist Page, a
                 * playlist, or Recent, none of which touch these). Reusing
                 * them here restored Albums already filtered/scrolled to
                 * wherever that last, unrelated tap happened to leave
                 * things -- reported live as landing on a mismatched album
                 * screen, as if mid-scroll through someone else's browse.
                 * There's no reliable "this queue's own Albums context" to
                 * restore for a mixed queue, so land on a clean, unfiltered
                 * list rather than a confidently wrong one. */
                cur_artist[0] = '\0';
                screen = SC_ALBUMS; reset_scroll();
                total = lib_albums_count(cur_facet, NULL);
                scroll = 0;
                scroll_px = 0;
                load_page();
            } else if (queue_n == 0) {
                /* R70 fix: reported live as a blank white-looking screen --
                 * SC_PLAYING's own draw code already bails out cleanly on an
                 * empty queue (`if (!t) return;`), but draw_screen()'s
                 * unconditional top-of-frame fill_rect(..., COL_BG) still
                 * runs first, so "bails out cleanly" looked like "nothing
                 * but a blank background, no way back". Swiping away every
                 * track leaves genuinely nothing to play (queue_remove_
                 * display() already called audio_stop() on the way here),
                 * so there's no "now playing" to show regardless -- land on
                 * the main menu instead of a screen with nothing on it. */
                screen = SC_MENU; reset_scroll();
            } else {
                screen = SC_PLAYING; reset_scroll();
            }
            break;
        case SC_ARTIST_PAGE:
            if (artist_page_from_list) {
                /* Reached via the Artists list itself -- back there, same
                 * "restore where the list was" shape SC_ALBUMS's own case
                 * below uses for the identical Artists-list return trip. */
                screen = SC_ARTISTS; reset_scroll();
                total = lib_group_count(cur_facet);
                scroll = artists_scroll_saved;
                if (scroll > total - 1) scroll = total > 0 ? total - 1 : 0;
                if (scroll < 0) scroll = 0;
                scroll_px = artists_scroll_px_saved;
                load_page();
            } else {
                /* Back to the album this was opened from, not a generic
                 * fallback -- artist_page_back_album/artist_page_back_artist
                 * were captured at the moment the artist name was tapped,
                 * specifically so "back" returns to browsing that album
                 * rather than always landing somewhere else regardless of
                 * entry point. */
                snprintf(cur_album, sizeof(cur_album), "%s", artist_page_back_album);
                snprintf(cur_artist, sizeof(cur_artist), "%s", artist_page_back_artist);
                screen = SC_TRACKS; reset_scroll();
                ab_list = 0;
                pod_list = 0;
                track_n = lib_tracks_for_album(cur_artist, cur_album, tracks,
                                               (int)(sizeof(tracks) / sizeof(tracks[0])), 0);
                if (track_n > 0)
                    view_art_request(tracks[0].path, cur_artist, cur_album);
            }
            break;
        case SC_POD_SYNC:
            /* Refreshed the same way opening Podcasts does -- a sync that's
             * still running when the reader backs out of watching it may
             * finish moments later with new feeds or episodes on disk that
             * this list hasn't seen yet. */
            screen = SC_PODCASTS; reset_scroll();
            pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
            pod_rebuild_rows();
            total = pod_feed_n;
            break;
        case SC_SETTINGS_THEME:
        case SC_SETTINGS_ABOUT:
        case SC_SETTINGS_TIMEZONE:
        case SC_SETTINGS_THEMEMODE:
        case SC_SETTINGS_WIFI:
        case SC_SETTINGS_BT:
        case SC_SETTINGS_USB:
            screen = SC_SETTINGS; reset_scroll();
            break;
        case SC_EQ_BAND:
            /* No save here: each slider/toggle already persists itself the
             * moment it's touched (see the tap handlers). A save on the way
             * out too would mean just opening a band and leaving it, with
             * nothing dragged, forks it into a "(Custom)" copy for no
             * reason -- eq_save_current() must only ever fire from an
             * actual edit. */
            screen = SC_EQ_BANDS; reset_scroll();
            total = eq_cur.band_n;
            break;
        case SC_ALBUMS:
            if (cur_facet) {
                screen = SC_ARTISTS; reset_scroll();
                /* Restore the count for the list being returned to. Without
                 * this the footer kept the number from the level below — one
                 * artist's two albums, reported as "1-9 of 2". */
                total = lib_group_count(cur_facet);
                /* BG37: land back where the list was, not at the top.
                 * Clamped in case the library changed shape underneath (a
                 * rescan removed rows) since the artist was tapped. */
                scroll = artists_scroll_saved;
                if (scroll > total - 1) scroll = total > 0 ? total - 1 : 0;
                if (scroll < 0) scroll = 0;
                scroll_px = artists_scroll_px_saved;
                load_page();
            } else {
                /* Reached via "Music" -> Albums, no facet above it. */
                screen = SC_MUSIC_MENU; reset_scroll();
            }
            break;
        default:
            /* SC_TRACKS doubles as the queue view for a regular album AND
             * (when audiobook_mode) as Chapters -- same screen, same data
             * shape, but "back" has to land somewhere that makes sense for
             * whichever it is, not always the SQL album browser. */
            if (tracks_from_artist_page) {
                /* Opened from the artist page's own album list -- back
                 * there directly. Nothing to re-fetch: artist_page_name/
                 * artist_page_albums/artist_art_bits are all still sitting
                 * in memory exactly as they were, untouched by having
                 * browsed into one of the artist's albums and back out.
                 *
                 * BG88: clear the flag the moment it's consumed. Left set,
                 * it stuck around into the artist page's *own* go_back()
                 * case (SC_ARTIST_PAGE, just above), whose "not from the
                 * list" branch lands back on this same SC_TRACKS default
                 * case for the originating album -- reading the still-set
                 * flag as if THAT visit had also come from the artist
                 * page's list, and bouncing straight back to SC_ARTIST_PAGE
                 * again instead of continuing out to Albums/Artists. Two
                 * taps into an album from the artist page, both backed out
                 * of, was enough to loop forever between the two screens. */
                tracks_from_artist_page = 0;
                screen = SC_ARTIST_PAGE; reset_scroll();
            } else if (pod_list) {
                screen = SC_PODCASTS; reset_scroll();
                pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
                pod_rebuild_rows();
                total = pod_feed_n;
            } else if (ab_list) {
                screen = SC_AUDIOBOOKS; reset_scroll();
                ab_book_n = ab_scan_books(ab_books, AB_MAX_BOOKS);
                ab_rebuild_rows();
                total = ab_book_n;
            } else if (recent_mode) {
                /* Recently added/heard: load_page() is a no-op here --
                 * index_visible() is deliberately false for recent_mode, so
                 * the facet-query path below would leave rows[]/row_n stale
                 * against a freshly wrong `total`, rendering blank. Same
                 * one-shot recompute the menu tap itself used to get here. */
                screen = SC_ALBUMS; reset_scroll();
                row_n = (recent_mode == RECENT_ADDED)
                      ? lib_albums_recent_added(rows, RECENT_ALBUMS_N)
                      : lib_albums_recent_heard(recent_heard_ts, rows, RECENT_ALBUMS_N);
                row_base = 0;
                total = row_n;
            } else {
                /* BG7: tapping an album row overwrites cur_artist with that
                 * one album's own artist (needed so two artists sharing an
                 * album name still get the right tracks) -- restore what
                 * Albums was actually filtered by before that happened,
                 * rather than requerying and redrawing the header with the
                 * single album's artist stuck in place. */
                snprintf(cur_artist, sizeof(cur_artist), "%s", albums_artist);
                screen = SC_ALBUMS; reset_scroll();
                total = lib_albums_count(cur_facet, cur_artist[0] ? cur_artist : NULL);
                /* BG37 follow-up: land back where the list was, not at the
                 * top. Same clamp BG37 uses for SC_ARTISTS, in case the
                 * album count changed underneath (a rescan) since the album
                 * was tapped. */
                scroll = albums_scroll_saved;
                if (scroll > total - 1) scroll = total > 0 ? total - 1 : 0;
                if (scroll < 0) scroll = 0;
                scroll_px = albums_scroll_px_saved;
                load_page();
            }
            break;
    }
    return 1;
}

/* Blend two RGB565 colours. Used only by the back-gesture hint, which wants to
 * fade rather than switch on. */
static uint16_t mix565(uint16_t a, uint16_t b, int t /* 0..256 */) {
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (br - ar) * t / 256;
    int g = ag + (bg - ag) * t / 256;
    int bl = ab + (bb - ab) * t / 256;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* A sliver at the left edge that grows and warms as the finger travels, going
 * fully accent-coloured once the swipe is far enough to count. Without it the
 * gesture is invisible until it has already happened, and there is no way to
 * tell a swipe that will register from one that will not. */
#define HINT_MAX_W 8
#define HINT_H     150

static void draw_back_hint(uint16_t *fb) {
    int t = edge_travel * 256 / EDGE_TRAVEL;
    if (t > 256) t = 256;
    int w = HINT_MAX_W * t / 256;
    if (w < 2) w = 2;

    int top = edge_y - HINT_H / 2;
    if (top < CONTENT_Y) top = CONTENT_Y;
    if (top + HINT_H > FB_H) top = FB_H - HINT_H;

    /* Warm late rather than linearly: for most of the travel this should be
     * barely there, and only clearly accent-coloured once the swipe is far
     * enough that letting go will actually go back. */
    uint16_t c = mix565(COL_LINE, COL_ACCENT, t * t / 256);
    /* Tapered ends, so it reads as a soft highlight rather than a bar. */
    for (int i = 0; i < HINT_H; i++) {
        int d = i < HINT_H / 2 ? i : HINT_H - 1 - i;
        int ww = d < 24 ? w * d / 24 : w;
        if (ww > 0) fill_rect(fb, 0, top + i, ww, 1, c);
    }
}

/* R80: same idea as draw_back_hint(), rotated -- a sliver at the bottom edge
 * that grows and warms as the finger travels up, centred on where it went
 * down rather than following it live (HINT_H is wide enough that chasing
 * the finger reads as jittery for a mostly-vertical swipe; the back hint
 * gets away with following because its own axis is the travel axis). */
static void draw_home_hint(uint16_t *fb) {
    int t = home_edge_travel * 256 / HOME_EDGE_TRAVEL;
    if (t > 256) t = 256;
    int h = HINT_MAX_W * t / 256;
    if (h < 2) h = 2;

    int left = home_edge_x - HINT_H / 2;
    if (left < 0) left = 0;
    if (left + HINT_H > FB_W) left = FB_W - HINT_H;

    uint16_t c = mix565(COL_LINE, COL_ACCENT, t * t / 256);
    for (int i = 0; i < HINT_H; i++) {
        int d = i < HINT_H / 2 ? i : HINT_H - 1 - i;
        int hh = d < 24 ? h * d / 24 : h;
        if (hh > 0) fill_rect(fb, left + i, FB_H - hh, 1, hh, c);
    }
}

#define SHEET_HEAD 46

static int sheet_rows(void) {
    /* R71: +2 for "New Playlist..." and "Cancel", not just +1 -- see
     * draw_sheet()'s own row labelling just below. */
    return sheet_open == 2 ? playlist_n + 2 :
           sheet_open == 3 ? eq_profile_n + 1 :
           sheet_open == 4 ? PLAYLIST_MENU_N :
           sheet_open == 5 ? PLAYLIST_DELETE_N : SHEET_N;
}
static int sheet_top(void) { return FB_H - sheet_rows() * SHEET_ROW - SHEET_HEAD; }

static void draw_sheet(uint16_t *fb) {
    int top = sheet_top();
    /* Dim what is behind it so the sheet reads as being in front -- the full
     * screen, status bar and header included (BG22): starting at CONTENT_Y
     * left them undimmed, which read as a shadow that stopped partway up
     * rather than one behind the whole screen. */
    for (int yy = 0; yy < top; yy++)
        for (int xx = 0; xx < FB_W; xx++) {
            uint16_t *px = fb + (size_t)yy * FB_W + xx;
            *px = mix565(*px, COL_BG, 150);
        }

    fill_rect(fb, 0, top, FB_W, FB_H - top, COL_HEADER);
    fill_rect(fb, 0, top, FB_W, 1, COL_LINE);

    /* The caption belongs inside the panel: drawn above it, it landed on top of
     * whichever list row happened to be there. */
    const char *cap = sheet_open == 3 ? "Choose profile"
                    : (sheet_open == 4 || sheet_open == 5)
                    ? (sheet_playlist >= 0 && sheet_playlist < playlist_n ? playlists[sheet_playlist].name : "")
                    : (sheet_track >= 0 && sheet_track < track_n)
                    ? tracks[sheet_track].name : "";
    draw_text(fb, 24, top + 14, cap, COL_DIM, TEXT_PX_SMALL, FB_W - 48);

    int rows = sheet_rows();
    for (int i = 0; i < rows; i++) {
        int ry = top + SHEET_HEAD + i * SHEET_ROW;
        const char *label;
        int last;
        int is_action = 0;   /* R71/R74: reads as an action, not a plain name/Cancel */
        if (sheet_open == 2) {
            last = (i == playlist_n + 1);
            label = last ? "Cancel" : (i == playlist_n) ? "New Playlist..." : playlists[i].name;
            is_action = (i == playlist_n);
        } else if (sheet_open == 3) {
            last = (i == eq_profile_n);
            label = last ? "Cancel" : eq_profiles[i].name;
        } else if (sheet_open == 4) {
            last = (i == PLAYLIST_MENU_N - 1);
            label = playlist_menu_items[i];
            is_action = !last;
        } else if (sheet_open == 5) {
            last = (i == PLAYLIST_DELETE_N - 1);
            label = playlist_delete_items[i];
            is_action = !last;
        } else {
            last = (i == SHEET_N - 1);
            label = sheet_items[i];
        }
        draw_text(fb, 24, ry + 16, label, last ? COL_DIM : is_action ? COL_ACCENT : COL_TEXT,
                  TEXT_PX_BODY, FB_W - 48);
        if (!last) fill_rect(fb, 24, ry + SHEET_ROW - 1, FB_W - 48, 1, COL_LINE);
    }
}

/* Geometry shared by the drawing and the hit tests. */
static int qs_bar_y(void)  { return STATUS_H + 74; }
static int qs_wifi_y(void) { return STATUS_H + 130; }
static int qs_bt_y(void)   { return qs_wifi_y() + QS_ROW_H; }
static int qs_usb_y(void)  { return qs_bt_y() + QS_ROW_H; }
static int qs_eq_y(void)   { return qs_usb_y() + QS_ROW_H; }
static int qs_mseb_y(void) { return qs_eq_y() + QS_ROW_H; }
/* R83: what's actually playing's own format/quality -- moved here, under
 * MSEB, from the top bar next to volume (see draw_quick_settings()'s own
 * comment on the earlier spot this held). */
static int qs_codec_y(void) { return qs_mseb_y() + QS_ROW_H; }
/* R51: top-right corner, level with "Brightness" opposite it -- not its own
 * row (tried first, corrected live: too much space for what it does, and
 * putting a whole row's worth of weight behind a single shortcut read as
 * more important than it is). */
static int qs_gear_x(void) { return FB_W - 24 - 20; }
static int qs_gear_y(void) { return STATUS_H + 6; }

static void draw_bt_icon(uint16_t *fb, int x, int y, uint16_t c) {
    draw_icon(fb, FB_W, FB_H, x, y, &icon_bt_qs, c);
}

static void draw_wifi_icon(uint16_t *fb, int x, int y, uint16_t c) {
    draw_icon(fb, FB_W, FB_H, x, y, &icon_wifi_qs, c);
}

static void draw_usb_icon(uint16_t *fb, int x, int y, uint16_t c) {
    draw_icon(fb, FB_W, FB_H, x, y, &icon_usb_qs, c);
}

/* A small frequency-response squiggle with a control point at each vertex --
 * what SC_EQ draws full-size via eq_response_db(), echoed in miniature. Bars
 * read as "generic audio/volume"; this reads as what the feature actually
 * is, a curve you shape at a few points. */
static void draw_eq_icon(uint16_t *fb, int x, int y, uint16_t c) {
    /* 1.45x the original points/radius, to sit at the same visual weight as
     * the enlarged Wi-Fi/Bluetooth bitmaps in the same list -- see the Quick
     * Settings row comments for why they all changed together. */
    int px[5] = { 0, 9, 17, 26, 35 };
    int py[5] = { 20, 25, 9, 16, 1 };
    for (int i = 0; i < 4; i++)
        draw_line(fb, x + px[i], y + py[i], x + px[i + 1], y + py[i + 1], c);
    for (int i = 0; i < 5; i++)
        fill_circle(fb, x + px[i], y + py[i], 3, c);
}

/* R51: a cog for the quick-settings row leading to the full Settings menu.
 * Vendored from Font Awesome's gear-solid-full.svg the same way wifi/
 * bluetooth's own icons already are -- see gen_icons.py and THIRD_PARTY.md. */
static void draw_gear_icon(uint16_t *fb, int x, int y, uint16_t c) {
    draw_icon(fb, FB_W, FB_H, x, y, &icon_gear_qs, c);
}


/* R83: what's actually playing's own format/quality, in the same shape
 * each mode's Now Playing screen used to compute independently at the foot
 * of the player (see BG106/R60's own comments on the VBR and podcast/
 * audiobook extension-plus-kbps cases this mirrors). Recomputed from
 * scratch here rather than shared with those -- each was built from locals
 * (t/ch/dur) only ever in scope mid-draw on that specific screen, not
 * reachable from quick settings, which can be pulled down from anywhere.
 * Empty string when there's nothing to show: radio (a live stream has no
 * file-based format to read) or nothing loaded at all. */
static void qs_format_info(char *out, size_t outsz) {
    out[0] = '\0';
    if (radio_mode || cur_track < 0 || cur_track >= queue_n) return;
    lib_track_t *t = &queue[cur_track];
    char ext[16];
    const char *dot = strrchr(t->path, '.');
    snprintf(ext, sizeof(ext), "%s", dot && dot[1] ? dot + 1 : "");
    for (char *p = ext; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (audiobook_mode) {
        const ab_chapter_t *ch = (cur_track < ab_book.chap_n) ? &ab_book.chap[cur_track] : NULL;
        int64_t file_dur = (ab_book.file_n == 1) ? ab_book.total_ms
                                                  : (ch ? ch->dur_ms : 0);
        int kbps = ab_file_bitrate_kbps(t->path, file_dur);
        if (kbps > 0) snprintf(out, outsz, "%s  %d kbps", ext, kbps);
        else          snprintf(out, outsz, "%s", ext);
    } else if (podcast_mode) {
        int dur = audio_dur_ms();
        if (dur <= 0) dur = t->dur_ms;
        int kbps = ab_file_bitrate_kbps(t->path, dur);
        if (kbps > 0) snprintf(out, outsz, "%s  %d kbps", ext, kbps);
        else          snprintf(out, outsz, "%s", ext);
    } else if (track_is_vbr_mp3(t->path)) {
        /* R101: reported live -- "just 'VBR' isn't right, give an
         * average". True by definition (a VBR file's own header states no
         * single rate to report, the same reason audiobook/podcast's own
         * branches above already compute one instead of reading a
         * header), so this reuses that exact same size*8/duration
         * arithmetic ab_file_bitrate_kbps() already provides rather than
         * a second implementation of the same sum. */
        int dur = audio_dur_ms();
        if (dur <= 0) dur = t->dur_ms;
        int kbps = ab_file_bitrate_kbps(t->path, dur);
        if (kbps > 0)
            snprintf(out, outsz, "%s  %d/%g kHz  VBR (avg %d kbps)",
                     track_format_name(t), t->bits, t->rate / 1000.0, kbps);
        else
            snprintf(out, outsz, "%s  %d/%g kHz  VBR",
                     track_format_name(t), t->bits, t->rate / 1000.0);
    } else {
        snprintf(out, outsz, "%s  %d/%g kHz  %d kbps",
                 track_format_name(t), t->bits, t->rate / 1000.0, t->bitrate / 1000);
    }
}

static void draw_quick_settings(uint16_t *fb) {
    fill_rect(fb, 0, 0, FB_W, QS_H, COL_HEADER);
    fill_rect(fb, 0, QS_H - 1, FB_W, 1, COL_LINE);
    draw_status(fb);

    /* R83: route (Bluetooth/USB/3.5mm), moved here from the foot of each
     * mode's Now Playing screen -- visible from wherever quick settings was
     * pulled down, not just while actually looking at Now Playing, and the
     * accessory's own codec/battery already live in the Bluetooth row below
     * (see its own comment) so nothing here duplicates that. Right after
     * the volume readout draw_status() just drew, in the same top strip.
     * Format/quality itself moved again, live feedback, to its own row at
     * the bottom of the panel instead -- see qs_codec_y() below. */
    {
        const int mid = STATUS_H / 2;
        int vol = audio_volume();
        char volbuf[16];
        snprintf(volbuf, sizeof(volbuf), "%d%%", vol);
        int rx = 24 + 26 + text_width(volbuf, TEXT_PX_SMALL) + 24;

        const char *route_kind = audio_output();   /* "3.5 mm" / "USB" / "Bluetooth" */
        const icon_t *ric = !strcmp(route_kind, "Bluetooth") ? &icon_bt_sm
                           : !strcmp(route_kind, "USB")      ? &icon_usb_sm : NULL;
        if (ric) {
            draw_icon(fb, FB_W, FB_H, rx, mid - ric->h / 2, ric, COL_DIM);
        } else {
            draw_text(fb, rx, mid - TEXT_PX_SMALL / 2, route_kind, COL_DIM, TEXT_PX_SMALL, FB_W);
        }
    }

    draw_text(fb, 24, STATUS_H + 12, "Brightness", COL_DIM, TEXT_PX_SMALL, FB_W - 48);
    /* R51: quick access to the full Settings menu. Dim, same weight as
     * "Brightness" opposite it -- a shortcut, not something with its own
     * on/off state to draw attention to. */
    draw_gear_icon(fb, qs_gear_x(), qs_gear_y(), COL_DIM);
    int by = qs_bar_y(), bw = FB_W - 48;
    fill_pill(fb, 24, by, bw, 8, COL_LINE);
    int filled = qs_bright_max > 0 ? bw * qs_bright / qs_bright_max : 0;
    if (filled > 0) fill_pill(fb, 24, by, filled, 8, COL_ACCENT);
    fill_circle(fb, 24 + filled, by + 4, 13, COL_ACCENT);

    /* Icon, name, and what it is actually attached to — the useful part, and
     * what stock shows. Coloured when on, so state reads without the toggle. */
    char nm[64];
    int wy = qs_wifi_y();
    /* +16, not +12: a bounding-box center undersells this icon's shape.
     * Almost all of its ink is the arcs in the top two-thirds -- the dot is
     * a handful of pixels -- so a box-centered placement put the visible
     * mass level with "Wi-Fi" and left it reading as floating above "off"
     * rather than spanning to it, even though the box itself matched the
     * text block exactly. Placed by alpha-weighted centroid instead (row
     * 11.7 of 34 in the bitmap), matched to Bluetooth's own weighted
     * centroid (row 18 of 38 at its +10 offset -> 28 from the row top). */
    draw_wifi_icon(fb, 24, wy + 16, qs_wifi ? COL_ACCENT : COL_DIM);
    /* QS_LABEL_X, not the old 68: at its new, larger size the Wi-Fi icon's
     * natural width (its source aspect ratio times the target height) puts
     * its right edge exactly at 68 -- zero gap, reading as crowding into the
     * "W". 76 clears it with room to spare. */
    draw_text(fb, QS_LABEL_X, wy + 6, "Wi-Fi", qs_wifi ? COL_TEXT : COL_DIM, TEXT_PX_SMALL, 200);
    st_wifi_ssid(nm, sizeof(nm));
    draw_text(fb, QS_LABEL_X, wy + 32, qs_wifi ? (nm[0] ? nm : "not connected") : "off",
              COL_DIM, TEXT_PX_SMALL, FB_W - 180);
    draw_toggle_switch(fb, wy, qs_wifi);

    int by2 = qs_bt_y();
    /* Block-centered vertically, same as Wi-Fi above. x=34, not the icon
     * column's usual 24-26: measured on a real screenshot, this icon's own
     * alpha-weighted horizontal centroid landed at x=38 against Wi-Fi's 46 --
     * a real, visible 8px gap between the two icons' center lines, not
     * merely a left-edge difference. Left-aligning icons of different
     * natural widths does not make them share a center; +8 here matches
     * this one's centroid to Wi-Fi's rather than its left edge. */
    draw_bt_icon(fb, 34, by2 + 10, qs_bt ? COL_ACCENT : COL_DIM);
    draw_text(fb, QS_LABEL_X, by2 + 6, "Bluetooth", qs_bt ? COL_TEXT : COL_DIM, TEXT_PX_SMALL, 200);
    st_bt_name(nm, sizeof(nm));
    if (qs_bt && nm[0]) {
        /* The codec and the headset's own battery used to sit on the Now
         * Playing screen's route line; they moved here, next to the name they
         * actually describe, freeing that line for the device's own battery. */
        char codec[32]; int batt;
        pthread_mutex_lock(&bt_lock);
        snprintf(codec, sizeof(codec), "%s", bt_codec_cached);
        batt = bt_batt_cached;
        pthread_mutex_unlock(&bt_lock);
        char base[80];
        if (codec[0]) snprintf(base, sizeof(base), "%s \xc2\xb7 %s", nm, codec);
        else          snprintf(base, sizeof(base), "%s", nm);
        draw_text(fb, QS_LABEL_X, by2 + 32, base, COL_DIM, TEXT_PX_SMALL, FB_W - 180);
        if (batt >= 0) {
            int tx = QS_LABEL_X + text_width(base, TEXT_PX_SMALL);
            draw_text(fb, tx, by2 + 32, " \xc2\xb7 ", COL_DIM, TEXT_PX_SMALL, FB_W - 180);
            tx += text_width(" \xc2\xb7 ", TEXT_PX_SMALL);
            draw_battery(fb, tx, by2 + 37, batt, 0);
            tx += 30;
            char pct[8];
            snprintf(pct, sizeof(pct), "%d%%", batt);
            draw_text(fb, tx, by2 + 32, pct, COL_DIM, TEXT_PX_SMALL, FB_W - 180 - (tx - QS_LABEL_X));
        }
    } else {
        draw_text(fb, QS_LABEL_X, by2 + 32, qs_bt ? "not connected" : "off",
                  COL_DIM, TEXT_PX_SMALL, FB_W - 180);
    }
    draw_toggle_switch(fb, by2, qs_bt);

    /* No toggle switch here, unlike Wi-Fi/Bluetooth above -- USB working
     * mode isn't a binary on/off, so the row is tap-to-open rather than
     * tap-to-flip. A chevron says that the way the toggle says the other
     * two rows are switches. */
    int byu = qs_usb_y();
    int usb_mode = st_usb_mode();
    draw_usb_icon(fb, 34, byu + 7, COL_DIM);
    draw_text(fb, QS_LABEL_X, byu + 6, "USB working mode", COL_TEXT, TEXT_PX_SMALL, 260);
    draw_text(fb, QS_LABEL_X, byu + 32,
              usb_mode == 0 ? "ADB" : usb_mode == 1 ? "USB Storage" : "unplugged",
              COL_DIM, TEXT_PX_SMALL, FB_W - 180);
    {
        int cx = FB_W - 24 - 10, cy = byu + QS_ROW_H / 2;
        draw_line(fb, cx - 8, cy - 8, cx, cy, COL_DIM);
        draw_line(fb, cx, cy, cx - 8, cy + 8, COL_DIM);
    }

    int by3 = qs_eq_y();
    /* R89: on's own preference is untouched by USB Transport Mode -- see
     * its engage-site comment -- but showing the toggle as ON while the
     * bypass has actually skipped processing entirely reads as "PEQ is
     * still on" (reported live), which is backwards: it's the one thing
     * that's genuinely off right now. Drawn state follows what is actually
     * running, not the saved setting, same distinction the volume toast
     * already draws for the locked value vs. the saved one. */
    int on = eq_enabled() && !usb_bypass_active;
    /* Block-centered vertically, same as Wi-Fi/Bluetooth above (y was +6
     * when the icon was smaller and merely aligned to the title line). x=29
     * for the same centroid-matching reason as Bluetooth's +34 above --
     * measured centroid 42 against Wi-Fi's 46, so +5. */
    draw_eq_icon(fb, 29, by3 + 16, on ? COL_ACCENT : COL_DIM);
    draw_text(fb, QS_LABEL_X, by3 + 6, "Parametric EQ", on ? COL_TEXT : COL_DIM, TEXT_PX_SMALL, 200);
    draw_text(fb, QS_LABEL_X, by3 + 32,
              usb_bypass_active ? "USB Transport Mode" : eq_cur_path[0] ? eq_cur.name : "no profile",
              COL_DIM, TEXT_PX_SMALL, FB_W - 180);
    draw_toggle_switch(fb, by3, on);

    /* Same shape as the Parametric EQ row above -- a quick toggle, not a way
     * in to editing the 9 bands (that stays under Parametric EQ in the main
     * menu). Reuses the same squiggle icon: MSEB is still, visually, "an
     * EQ" -- a distinct glyph for it would say otherwise. */
    int by4 = qs_mseb_y();
    int mseb_shown = mseb_on && !usb_bypass_active;
    draw_eq_icon(fb, 29, by4 + 16, mseb_shown ? COL_ACCENT : COL_DIM);
    draw_text(fb, QS_LABEL_X, by4 + 6, "MSEB", mseb_shown ? COL_TEXT : COL_DIM, TEXT_PX_SMALL, 200);
    draw_text(fb, QS_LABEL_X, by4 + 32,
              usb_bypass_active ? "USB Transport Mode" : "HiBy tuning bands",
              COL_DIM, TEXT_PX_SMALL, FB_W - 180);
    draw_toggle_switch(fb, by4, mseb_shown);

    /* R83: what's actually playing's own format/quality -- moved here,
     * under MSEB, from the top bar next to volume (asked for live, a
     * second time, after the top bar placement first shipped). No icon or
     * toggle, so text stays at QS_LABEL_X rather than x=24 -- aligned under
     * the rows above it instead of ragged against them. */
    int by5 = qs_codec_y();
    draw_text(fb, QS_LABEL_X, by5 + 6, "Format", COL_DIM, TEXT_PX_SMALL, 200);
    char fmt[64];
    qs_format_info(fmt, sizeof(fmt));
    draw_text(fb, QS_LABEL_X, by5 + 32, fmt[0] ? fmt : "Nothing playing",
              fmt[0] ? COL_ACCENT : COL_DIM, TEXT_PX_SMALL, FB_W - 48);

    /* A grab handle, so it is obvious the panel goes back up. */
    fill_rect(fb, FB_W / 2 - 26, QS_H - 14, 52, 4, COL_LINE);
}

static int vol_bar_y(void) { return STATUS_H + VOL_H / 2 + 6; }

static void draw_volume(uint16_t *fb) {
    int top = STATUS_H;
    fill_rect(fb, 0, top, FB_W, VOL_H, COL_HEADER);
    fill_rect(fb, 0, top + VOL_H - 1, FB_W, 1, COL_LINE);

    int v = vol_dragging && vol_drag_pct >= 0 ? vol_drag_pct : audio_volume();
    /* R89: USB Transport Mode pins and locks volume (see its own engage
     * comment) -- a hardware button press still lands here (vol_ticks is
     * set unconditionally in the key handler) even though audio_volume_step()
     * itself is a no-op while locked, which without this looked like a
     * press that silently did nothing. Dimmed the same way any other
     * disabled control in this app is (COL_DIM, matching R81's USB Storage
     * Mode banner), with the label saying why rather than just showing an
     * unmoving percentage. */
    if (usb_bypass_active) {
        draw_text(fb, 24, top + 10, "Volume locked", COL_DIM, TEXT_PX_SMALL, FB_W - 160);
        draw_text(fb, FB_W - 24 - text_width("USB Transport Mode", TEXT_PX_SMALL),
                  top + 10, "USB Transport Mode", COL_DIM, TEXT_PX_SMALL, FB_W);
        int by = vol_bar_y(), bw = FB_W - 48;
        fill_rect(fb, 24, by, bw, 8, COL_LINE);
        int filled = bw * v / 100;
        fill_rect(fb, 24, by, filled, 8, COL_DIM);
        fill_circle(fb, 24 + filled, by + 4, 13, COL_DIM);
        return;
    }

    char buf[16];
    /* R95 follow-up: current/total steps for every output, not just
     * Bluetooth -- explicit correction after the first cut only changed
     * this on BT. Derived from the same 0-100 v every output already
     * agrees on (wired's own software gain included), rather than reading
     * the BT-only raw mixer value this used to -- one formula instead of
     * a per-output branch. */
    {
        int steps = audio_bt_vol_steps();
        int cur = (v * steps + 50) / 100;
        if (cur < 0) cur = 0; if (cur > steps) cur = steps;
        snprintf(buf, sizeof(buf), "%d/%d", cur, steps);
    }
    draw_text(fb, 24, top + 10, "Volume", COL_DIM, TEXT_PX_SMALL, FB_W - 120);
    int tw = text_width(buf, TEXT_PX_SMALL);
    draw_text(fb, FB_W - 24 - tw, top + 10, buf, COL_TEXT, TEXT_PX_SMALL, FB_W);

    int by = vol_bar_y(), bw = FB_W - 48;
    fill_rect(fb, 24, by, bw, 8, COL_LINE);
    int filled = bw * v / 100;
    fill_rect(fb, 24, by, filled, 8, COL_ACCENT);
    fill_circle(fb, 24 + filled, by + 4, 13, COL_ACCENT);
}

/* R60: confirms a hardware seek actually landed, and by how much -- see
 * seek_toast()'s own comment for why that has no other on-screen affordance.
 * A small pill under the status bar, same layer vol_ticks/draw_volume()
 * already draws in, out of the way of every mode's own transport row below
 * it (cover art, progress bars, skip arcs) since none of them reach this
 * high up the screen. */
static void draw_seek_toast(uint16_t *fb) {
    int tw = text_width(seek_toast_text, TEXT_PX_BODY);
    int pw = tw + 40, ph = 44;
    int px = (FB_W - pw) / 2, py = STATUS_H + 12;
    fill_rect(fb, px, py, pw, ph, COL_HEADER);
    fill_rect(fb, px, py + ph - 1, pw, 1, COL_LINE);
    draw_text(fb, px + 20, py + (ph - TEXT_PX_BODY) / 2 - 2, seek_toast_text,
              COL_ACCENT, TEXT_PX_BODY, FB_W);
}

/* Just the progress bar and its two clocks. A scrub changes nothing else, and
 * repainting the whole screen — a 384x384 cover blit included — for a bar six
 * pixels tall is what made dragging feel heavy. */
static void draw_scrub_strip(uint16_t *fb) {
    if (cur_track < 0 || cur_track >= queue_n) return;
    lib_track_t *t = &queue[cur_track];

    int by = bar_y();
    int top = by - 16, h = 52;
    fill_rect(fb, 0, top, FB_W, h, COL_BG);

    int dur = audio_dur_ms();
    if (dur <= 0) dur = t->dur_ms;
    int pos = scrub_active ? scrub_ms(dur) : audio_pos_ms();

    int bh = scrub_active ? 10 : 6;
    int byy = by - (bh - 6) / 2;
    fill_rect(fb, 24, byy, FB_W - 48, bh, COL_LINE);
    if (dur > 0) {
        int w = (FB_W - 48) * pos / dur;
        if (w > FB_W - 48) w = FB_W - 48;
        fill_rect(fb, 24, byy, w, bh, COL_ACCENT);
        if (scrub_active) fill_circle(fb, 24 + w, by + 3, 13, COL_ACCENT);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d", pos / 60000, (pos / 1000) % 60);
    draw_text(fb, 24, by + 14, buf, COL_DIM, TEXT_PX_SMALL, FB_W);
    int rem = dur - pos;
    if (rem < 0) rem = 0;
    snprintf(buf, sizeof(buf), "-%d:%02d", rem / 60000, (rem / 1000) % 60);
    draw_right(fb, by + 14, buf);
}

/* R14 profiling, temporary. Accumulated in memory and reported once every
 * PROF_EVERY painted frames: mlog() writes to /usr/data, and BG27 established
 * that storage I/O on this thread is exactly what stalls the UI loop, so
 * logging per frame would measure the instrument rather than the redraw. */
#define PROF_EVERY 60
static int      prof_n;
static uint64_t prof_ui_us, prof_clear_us, prof_text_us;
static uint64_t prof_ui_max;
uint64_t g_prof_text_us;        /* added to by text.c's draw path */

static uint64_t us_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

uint64_t g_prof_clear_us;       /* added to by fill_rect's full-screen path */

/* R77: hold-to-power-off. Below POWER_HOLD_UI_MS a press is an ordinary tap
 * (wake/lock, or the double-press button-lock toggle -- both entirely
 * unchanged). At/after it, this reads as a deliberate hold: the countdown
 * overlay appears, and the single-press wake/lock action that would
 * otherwise have already fired is either done (if it was a wake -- see
 * power_key_pending_lock's own comment) or cancelled (if it was a lock,
 * which would have blanked the very screen this overlay needs to draw on).
 *
 * Release timing then decides the outcome: released before
 * POWER_HOLD_SAFE_MS cancels outright (just a hold that didn't go anywhere);
 * released between the two thresholds shuts down gracefully, saving
 * everything a normal shutdown does; still held at POWER_HOLD_FORCE_MS
 * fires on its own, with nothing saved -- an escape hatch for a shutdown
 * that seems stuck, not something a release timing choice can reach.
 *
 * Timed with us_now() (CLOCK_MONOTONIC), not the input event's own kernel
 * timestamp the way DOUBLE_PRESS_MS is (see have_last_power_press's own
 * comment on why that one has to be) -- a few tens of ms of poll latency is
 * real risk against a 400ms double-press window, but negligible against
 * thresholds in the seconds. */
#define POWER_HOLD_UI_MS     350
#define POWER_HOLD_SAFE_MS  3000
#define POWER_HOLD_FORCE_MS 5000
static int      power_key_down;
static uint64_t power_key_down_us;
static int      power_key_pending_lock;   /* screen was lit at press time; lock deferred */
static int      power_hold_ui_shown;
static int      power_hard_fired;
/* 0 = nothing pending, 1 = graceful shutdown to run once `base` (the real
 * framebuffer, only in scope in the main loop) is reachable, set from
 * handle_keys() on a qualifying release; read and cleared there. The force
 * path needs no such handoff -- it never touches resume_save()/base at all. */
static int      power_release_outcome;
/* Set once either shutdown path actually calls poweroff, so the overlay
 * keeps showing a final message instead of popping back to the ordinary
 * screen while poweroff's own shutdown sequence (not instant) runs its
 * course underneath it. */
static int      power_shutdown_kind;   /* 0 = none, 1 = graceful, 2 = forced */
/* BG112: a clean copy of the screen from the instant before the countdown
 * overlay first appears -- resume_save() (called from the graceful path,
 * once the overlay has been on screen for anywhere up to two seconds) must
 * not capture the overlay itself. Same size as one framebuffer page; taken
 * once per hold, at the same 0->1 transition that first shows the overlay. */
static uint16_t power_pre_hold_fb[FB_W * FB_H];

/* Same visual language as draw_sheet()'s own modal: dim everything behind,
 * a solid panel in front -- centered here rather than bottom-anchored,
 * since this is a single status, not a list to pick from. No arc/ring
 * primitive exists anywhere in this file (checked -- fill_circle only ever
 * draws whole circles), so progress reads as a growing horizontal bar
 * instead, the same shape draw_volume()'s own bar already uses. */
static void draw_power_hold(uint16_t *fb) {
    for (int yy = 0; yy < FB_H; yy++)
        for (int xx = 0; xx < FB_W; xx++) {
            uint16_t *px = fb + (size_t)yy * FB_W + xx;
            *px = mix565(*px, COL_BG, 170);
        }

    int pw = FB_W - 96, ph = 220;
    int px0 = (FB_W - pw) / 2, py0 = (FB_H - ph) / 2;
    fill_rect(fb, px0, py0, pw, ph, COL_HEADER);
    fill_rect(fb, px0, py0, pw, 1, COL_LINE);
    fill_rect(fb, px0, py0 + ph - 1, pw, 1, COL_LINE);

    long elapsed_ms = power_shutdown_kind ? 0
                    : (long)((us_now() - power_key_down_us) / 1000);
    int past_safe = power_shutdown_kind == 1 || elapsed_ms >= POWER_HOLD_SAFE_MS;

    const char *title = power_shutdown_kind == 1 ? "Shutting down"
                       : power_shutdown_kind == 2 ? "Forcing shutdown"
                       : past_safe ? "Release to shut down"
                       : "Hold to power off";
    int tw = text_width(title, TEXT_PX_BODY);
    draw_text(fb, px0 + (pw - tw) / 2, py0 + 36, title, COL_TEXT, TEXT_PX_BODY, pw - 40);

    /* One bar spanning the full 0..POWER_HOLD_FORCE_MS hold, with a notch
     * marking POWER_HOLD_SAFE_MS -- the point past which releasing is the
     * good outcome, not the danger one, so the notch reads as "you're
     * clear" rather than a threat. */
    int bx = px0 + 40, bw = pw - 80, by = py0 + 100, bh = 14;
    fill_rect(fb, bx, by, bw, bh, COL_LINE);
    long clamped = power_shutdown_kind == 2 ? POWER_HOLD_FORCE_MS
                 : power_shutdown_kind == 1 ? POWER_HOLD_SAFE_MS
                 : elapsed_ms;
    if (clamped > POWER_HOLD_FORCE_MS) clamped = POWER_HOLD_FORCE_MS;
    int filled = (int)(bw * clamped / POWER_HOLD_FORCE_MS);
    fill_rect(fb, bx, by, filled, bh, COL_ACCENT);
    int notch = bx + (int)((long)bw * POWER_HOLD_SAFE_MS / POWER_HOLD_FORCE_MS);
    fill_rect(fb, notch, by - 4, 2, bh + 8, COL_HEADER);

    char sub[48];
    if (power_shutdown_kind) {
        sub[0] = '\0';
    } else if (!past_safe) {
        snprintf(sub, sizeof(sub), "%.1fs", (POWER_HOLD_SAFE_MS - elapsed_ms) / 1000.0);
    } else {
        snprintf(sub, sizeof(sub), "Forcing in %.1fs",
                 (POWER_HOLD_FORCE_MS - elapsed_ms) / 1000.0);
    }
    if (sub[0]) {
        int sw = text_width(sub, TEXT_PX_SMALL);
        draw_text(fb, px0 + (pw - sw) / 2, by + 34, sub, COL_DIM, TEXT_PX_SMALL, pw - 40);
    }
}

static void draw_ui(uint16_t *fb) {
    uint64_t t0 = us_now();
    uint64_t clear0 = g_prof_clear_us, text0 = g_prof_text_us;

    /* One lock/unlock for the whole frame -- see g_view_art_gone_frame's own
     * comment above view_art_gone(). */
    g_view_art_gone_frame = view_art_gone();
    draw_screen(fb);
    if (index_visible()) draw_index(fb);
    if (mini_visible()) draw_mini(fb);
    if (sheet_open) draw_sheet(fb);
    if (qs_open) draw_quick_settings(fb);
    else if (vol_ticks > 0) draw_volume(fb);
    else if (screen == SC_PLAYING && seek_toast_ticks > 0) draw_seek_toast(fb);
    if (edge_active) draw_back_hint(fb);
    if (home_edge_active) draw_home_hint(fb);
    if (power_hold_ui_shown) draw_power_hold(fb);   /* R77: on top of everything else */

    uint64_t dt = us_now() - t0;
    prof_ui_us    += dt;
    prof_clear_us += g_prof_clear_us - clear0;
    prof_text_us  += g_prof_text_us - text0;
    if (dt > prof_ui_max) prof_ui_max = dt;
    if (++prof_n >= PROF_EVERY) {
        mlog("[prof] draw_ui avg %lu us (max %lu) | clear %lu | text %lu | other %lu\n",
             (unsigned long)(prof_ui_us / prof_n),
             (unsigned long)prof_ui_max,
             (unsigned long)(prof_clear_us / prof_n),
             (unsigned long)(prof_text_us / prof_n),
             (unsigned long)((prof_ui_us - prof_clear_us - prof_text_us) / prof_n));
        prof_n = 0; prof_ui_us = prof_clear_us = prof_text_us = 0; prof_ui_max = 0;
    }
}

/* ---- screen lock --------------------------------------------------------- */
/* The stock UI blanks the screen on its own timeout, but nothing does that
 * while this app owns the framebuffer and has the input nodes grabbed, so the
 * panel simply stayed lit forever.
 *
 * Power toggles the lock; it also locks itself after a spell of no input. The
 * media keys keep working while locked and deliberately do not wake the
 * screen — locked-with-music-playing is the pocket case, and lighting the
 * panel every time the volume changes defeats the point. Touch is drained and
 * discarded rather than left unread: events queued against our grab would
 * otherwise all replay the moment the screen came back, acting on whatever
 * screen happened to be up.
 */
#define BACKLIGHT "/sys/class/backlight/backlight_pwm0/brightness"
#define DEFAULT_BRIGHTNESS 43
/* The unit's blue notification LED, on by default (brightness 50 of 100)
 * from stock firmware startup with no obvious script setting it -- read
 * back from hiby_player's own binary, so it's written by the player itself
 * rather than a kernel/driver default, which is what makes a single
 * off-at-startup write not fully reliable on its own: if the stock code
 * reasserts it later, the write needs repeating, not just doing once. Tied
 * to button_locked specifically (the double-press lock), not the ordinary
 * screen lock -- that one fires on every idle timeout and a blinking LED
 * every time the screen naturally dims would be worse than the LED always
 * being on. */
#define LED_BLUE         "/sys/class/leds/blue/brightness"
#define LED_RED          "/sys/class/leds/red/brightness"
#define LED_BLUE_TRIGGER "/sys/class/leds/blue/trigger"
#define LED_RED_TRIGGER  "/sys/class/leds/red/trigger"
/* Not 100 (this LED's own max_brightness): same PWM-wraparound quirk as
 * BG17's backlight -- writing the literal max value wraps the duty cycle to
 * 0% instead of 100%, so the LED went fully dark instead of lighting up.
 * Confirmed live: sysfs read back "100" correctly after the write, so the
 * write itself wasn't the problem, only what value it was written with. */
#define LED_ON   99
/* This board's "breathing" kernel trigger is a no-op in practice -- selecting
 * it (confirmed via /sys/.../trigger showing [breathing]) produced a flat-on
 * LED, not a pulse, and it exposes none of the delay_on/delay_off files a
 * working software-blink trigger normally would. The pulse below is done by
 * hand instead: a plain triangle ramp written from the main loop, which
 * already ticks at a known, steady rate (see VOL_TICKS's own "~3s" comment
 * for the same 30Hz assumption). Both LEDs stay in manual (trigger=none) so
 * these raw brightness writes are what actually controls them. */
/* Counted in main-loop ticks, and the loop runs at 10 Hz while locked, which
 * is the only state this pulse ever runs in (it is gated on button_locked,
 * and that always locks). These were 150/300 back when every tick was 33 ms;
 * left alone they would have stretched the same pulse over fifteen seconds. */
#define LED_PULSE_TICKS 50     /* one full up/down cycle: ~5s */
#define LED_SWAP_TICKS  100    /* colour alternates every ~10s (2 pulses) */
/* Seconds of no input before the panel goes dark; 0 disables it. Off for now
 * by explicit request while the crash and index work settle — the auto-lock
 * was one of several things making it hard to tell what state a report was
 * actually describing. Meant to come back properly later (see the backlog),
 * not abandoned. */
#define CONF_PATH "/usr/data/music.conf"
#define IDLE_DEFAULT_S 0
static int idle_ticks = IDLE_DEFAULT_S * 30;    /* loop runs at ~30/s while awake */

/* Moved up from its previous spot just after save_conf() -- load_conf()
 * needs to set this directly, and C won't let it reference a static declared
 * later in the file. */
static int saved_brightness = -1;

/* BG95: file-scope rather than a local inside music_entry()'s own loop --
 * see handle_keys()'s own comment on the single-power-press "wake from dark
 * for any reason" path for why it needs to read this too, not just the BG6
 * watchdog that originally tracked it. */
static int last_lit_bright;

/* R64: the persisted *preference*, separate from saved_brightness above --
 * set_locked() also writes saved_brightness, at every lock, with whatever
 * the backlight happens to read right then (see its own comment: that's
 * exactly right for "what to restore to on unlock," which is all it was
 * ever for). Reusing that same variable as R64's reboot-persisted value at
 * first meant a lock landing at an unexpected level -- 0, if the backlight
 * was already down for any reason at that instant -- silently overwrote the
 * user's real preference with it, and the very next unrelated save_conf()
 * call (any toggle, any screen) wrote that corrupted value to music.conf.
 * Reported live: brightness stopped surviving reboots again despite R64
 * appearing to work at first. This is updated only at the two places the
 * user actually moves the slider, never by lock/unlock. */
static int brightness_pref = -1;

/* R64: the *intended* radio state, updated at the same moment st_wifi_set()/
 * st_bt_set() are, not re-derived from st_wifi_on()/st_bt_on() at save time.
 * Both scripts background themselves and take real seconds (wifi_on.sh waits
 * on DHCP) -- a save_conf() running right after firing one would almost
 * always read the *pre-toggle* live state back, persisting the opposite of
 * what was just asked for. Seeded from live state once at startup (see
 * load_conf()) so an unrelated save_conf() call, before either radio has
 * ever been toggled this session, still writes something meaningful instead
 * of a sentinel. */
static int wifi_pref = 1, bt_pref = 1;

static void load_conf(void) {
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;
    /* Wide enough for eq_profile_path -- EP_PATH_LEN plus the key and '='. */
    char line[EP_PATH_LEN + 32];
    /* BG38: applied after the loop, once both are known, rather than as each
     * line is seen -- eq_on can appear before or after eq_profile_path, and
     * eq_switch_to() should only ever run once per load. -1 = not present in
     * the file (a config from before this existed), so eq_enabled() is left
     * at its compiled-in default rather than forced off. */
    char eq_path_buf[EP_PATH_LEN] = "";
    int eq_on_saved = -1;
    /* R64: -1 means not present in the file (a config from before this
     * existed, or a fresh /usr/data) -- left alone rather than forced off,
     * same reasoning as eq_on_saved just above. */
    int wifi_saved = -1, bt_saved = -1, brightness_saved = -1;
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "idle_lock_seconds = %d", &v) == 1 ||
            sscanf(line, "idle_lock_seconds=%d", &v) == 1) {
            if (v < 0) v = 0;
            idle_ticks = v * 30;
        } else if (sscanf(line, "accent_index = %d", &v) == 1 ||
                   sscanf(line, "accent_index=%d", &v) == 1) {
            if (v >= 0 && v < ACCENT_N) {
                g_accent_idx = v;
                g_accent = ACCENT_PRESETS[v].color;
            }
        } else if (sscanf(line, "button_lock_enabled = %d", &v) == 1 ||
                   sscanf(line, "button_lock_enabled=%d", &v) == 1) {
            button_lock_enabled = v != 0;
        } else if (sscanf(line, "usb_bypass_enabled = %d", &v) == 1 ||
                   sscanf(line, "usb_bypass_enabled=%d", &v) == 1) {
            usb_bypass_enabled = v != 0;
        } else if (sscanf(line, "cover_palette_enabled = %d", &v) == 1 ||
                   sscanf(line, "cover_palette_enabled=%d", &v) == 1) {
            cover_palette_enabled = v != 0;
        } else if (sscanf(line, "bt_autoplay_enabled = %d", &v) == 1 ||
                   sscanf(line, "bt_autoplay_enabled=%d", &v) == 1) {
            bt_autoplay_enabled = v != 0;
        } else if (sscanf(line, "theme_mode = %d", &v) == 1 ||
                   sscanf(line, "theme_mode=%d", &v) == 1) {
            if (v >= 0 && v < THEME_MODE_N) theme_mode = v;
        } else if (sscanf(line, "tz_index = %d", &v) == 1 ||
                   sscanf(line, "tz_index=%d", &v) == 1) {
            if (v >= 0 && v < TZ_N) tz_idx = v;
        } else if (sscanf(line, "shuffle_enabled = %d", &v) == 1 ||
                   sscanf(line, "shuffle_enabled=%d", &v) == 1) {
            shuffle_enabled = v != 0;
        } else if (sscanf(line, "repeat_mode = %d", &v) == 1 ||
                   sscanf(line, "repeat_mode=%d", &v) == 1) {
            if (v >= REPEAT_OFF && v <= REPEAT_ONE) repeat_mode = v;
        } else if (sscanf(line, "light_theme = %d", &v) == 1 ||
                   sscanf(line, "light_theme=%d", &v) == 1) {
            /* R43: superseded by theme_mode, which a save always writes
             * instead from here on -- read only for a config saved by the
             * R41 build, before Grey existed. Harmless if theme_mode above
             * already set this from a newer file: whichever line appears
             * second in an old file wins, and a config this old was written
             * by code that only ever wrote one of the two keys, never both. */
            theme_mode = (v != 0) ? THEME_LIGHT : THEME_DARK;
        } else if (sscanf(line, "deep_sleep = %d", &v) == 1 ||
                   sscanf(line, "deep_sleep=%d", &v) == 1) {
            /* Deliberately conf-only and off by default, with no Settings row
             * yet: this is the one feature here that can leave the device
             * unresponsive if the wake path misbehaves, and it has already done
             * so once. It earns a toggle in the UI after it has been shown to
             * suspend and wake reliably on real hardware, not before. */
            deep_sleep_enabled = v != 0;
        } else if (sscanf(line, "sleep_minutes = %d", &v) == 1 ||
                   sscanf(line, "sleep_minutes=%d", &v) == 1) {
            /* Stored as the value, not the index, so the file stays meaningful
             * on its own and reordering the choices cannot silently change
             * what an existing config means. */
            for (int i = 0; i < SLEEP_CHOICE_N; i++)
                if (SLEEP_CHOICES[i] == v) { sleep_idx = i; break; }
        } else if (sscanf(line, "auto_off_minutes = %d", &v) == 1 ||
                   sscanf(line, "auto_off_minutes=%d", &v) == 1) {
            for (int i = 0; i < AUTO_OFF_CHOICE_N; i++)
                if (AUTO_OFF_CHOICES[i] == v) { auto_off_idx = i; break; }
        } else if (sscanf(line, "bt_vol_steps = %d", &v) == 1 ||
                   sscanf(line, "bt_vol_steps=%d", &v) == 1) {
            for (int i = 0; i < BT_VOL_STEP_CHOICE_N; i++)
                if (BT_VOL_STEP_CHOICES[i] == v) { bt_vol_step_idx = i; break; }
            audio_set_bt_vol_steps(BT_VOL_STEP_CHOICES[bt_vol_step_idx]);
        } else if (sscanf(line, "eq_on = %d", &v) == 1 ||
                   sscanf(line, "eq_on=%d", &v) == 1) {
            eq_on_saved = v != 0;
        } else if (sscanf(line, "eq_profile_path = %383[^\r\n]", eq_path_buf) == 1 ||
                   sscanf(line, "eq_profile_path=%383[^\r\n]", eq_path_buf) == 1) {
            /* Paths can carry spaces (SD-card folder names do), hence %[^\r\n]
             * rather than %s, which would stop at the first one. */
        } else if (sscanf(line, "wifi_enabled = %d", &v) == 1 ||
                   sscanf(line, "wifi_enabled=%d", &v) == 1) {
            wifi_saved = v != 0;
        } else if (sscanf(line, "bt_enabled = %d", &v) == 1 ||
                   sscanf(line, "bt_enabled=%d", &v) == 1) {
            bt_saved = v != 0;
        } else if (sscanf(line, "brightness = %d", &v) == 1 ||
                   sscanf(line, "brightness=%d", &v) == 1) {
            if (v > 0) brightness_saved = v;
        } else if (sscanf(line, "ab_speed_permille = %d", &v) == 1 ||
                   sscanf(line, "ab_speed_permille=%d", &v) == 1) {
            /* BG91: reported as "audiobook speed doesn't survive reboots,
             * podcasts handle this correctly" -- direct inspection found
             * pod_speed_permille has never actually been saved anywhere
             * either; it only *looks* persistent because it's a plain
             * static that outlives a track change within one running
             * session, same as ab_speed_permille already does. Both get the
             * real fix here, not just the one named in the report. */
            if (v >= 800 && v <= 2000) ab_speed_permille = v;
        } else if (sscanf(line, "pod_speed_permille = %d", &v) == 1 ||
                   sscanf(line, "pod_speed_permille=%d", &v) == 1) {
            if (v >= 800 && v <= 2000) pod_speed_permille = v;
        }
    }
    fclose(f);
    /* BG38: EQ settings did not survive a reboot -- nothing wrote them here
     * at all. eq_switch_to() is a no-op against a path that no longer exists
     * (a profile deleted or a card swapped since), same as any other stale
     * path this app already tolerates. */
    if (eq_path_buf[0]) eq_switch_to(eq_path_buf);
    if (eq_on_saved >= 0) eq_set_enabled(eq_on_saved);
    /* MSEB has its own file (MSEB_PATH), not a music.conf key -- there is
     * nothing profile-like about it to switch between, just one fixed set of
     * gains, so it does not need the eq_profile_path indirection above. */
    mseb_load(mseb_gain, &mseb_on);
    eq_set_mseb(mseb_on, mseb_gain);
    recent_heard_load();   /* R30 -- its own file, not a music.conf key either */
    apply_theme();         /* R41/R43: theme_mode just loaded above, if present */
    /* R64: restore whichever of these differs from whatever this boot's own
     * init scripts already left the radios in -- st_wifi_set()/st_bt_set()
     * both background a real shell command, so skipping the call when the
     * live state already matches avoids spawning one for nothing on every
     * single app start. wifi_pref/bt_pref then track the *intended* state
     * from here on (see their own comment) -- seeded from whatever's actually
     * live post-restore, not from wifi_saved/bt_saved directly, so a config
     * with no saved value yet (wifi_saved == -1) still starts these agreeing
     * with reality instead of defaulting to their compiled-in initializer
     * regardless of what this boot's init scripts actually did. */
    if (wifi_saved >= 0 && wifi_saved != st_wifi_on()) st_wifi_set(wifi_saved);
    if (bt_saved >= 0 && bt_saved != st_bt_on()) st_bt_set(bt_saved);
    wifi_pref = wifi_saved >= 0 ? wifi_saved : st_wifi_on();
    bt_pref = bt_saved >= 0 ? bt_saved : st_bt_on();
    /* Brightness has no live/saved distinction to check -- always apply it,
     * through st_brightness_set() (not a direct write) so its own max-1
     * PWM-wraparound clamp still applies, same reasoning as every other
     * brightness restore in this file. */
    if (brightness_saved > 0) {
        saved_brightness = brightness_saved;
        brightness_pref = brightness_saved;
        st_brightness_set(brightness_saved);
    }
}

/* Does this line set `key`? Length taken from the key itself rather than
 * written out by hand: the hand-written version had "button_lock_enabled"
 * (19 characters) matched with a length of 20, so byte 20 compared the
 * literal's NUL against the line's space, never matched, and every save kept
 * the old line *and* appended a new one. The file had accumulated twenty-odd
 * copies before anyone looked -- which matters, because the reader below only
 * takes 64 lines and would eventually have pushed real settings out. */
static int conf_line_is(const char *line, const char *key) {
    return strncmp(line, key, strlen(key)) == 0;
}

/* Read-modify-write rather than a full rewrite from known keys: the file's
 * own comment block (see BG5) is meant to stay human-readable, and this
 * only ever touches the lines it owns. */
static void save_conf(void) {
    /* Wide enough for eq_profile_path -- see the matching line[] in load_conf(). */
    char lines[64][EP_PATH_LEN + 32];
    int n = 0;
    FILE *f = fopen(CONF_PATH, "r");
    if (f) {
        while (n < 64 && fgets(lines[n], sizeof(lines[0]), f)) {
            if (!conf_line_is(lines[n], "accent_index") &&
                !conf_line_is(lines[n], "button_lock_enabled") &&
                !conf_line_is(lines[n], "usb_bypass_enabled") &&
                !conf_line_is(lines[n], "cover_palette_enabled") &&
                !conf_line_is(lines[n], "bt_autoplay_enabled") &&
                !conf_line_is(lines[n], "light_theme") &&
                !conf_line_is(lines[n], "theme_mode") &&
                !conf_line_is(lines[n], "tz_index") &&
                !conf_line_is(lines[n], "shuffle_enabled") &&
                !conf_line_is(lines[n], "repeat_mode") &&
                !conf_line_is(lines[n], "deep_sleep") &&
                !conf_line_is(lines[n], "sleep_minutes") &&
                !conf_line_is(lines[n], "auto_off_minutes") &&
                !conf_line_is(lines[n], "bt_vol_steps") &&
                !conf_line_is(lines[n], "eq_on") &&
                !conf_line_is(lines[n], "eq_profile_path") &&
                !conf_line_is(lines[n], "wifi_enabled") &&
                !conf_line_is(lines[n], "bt_enabled") &&
                !conf_line_is(lines[n], "brightness") &&
                !conf_line_is(lines[n], "ab_speed_permille") &&
                !conf_line_is(lines[n], "pod_speed_permille"))
                n++;
        }
        fclose(f);
    }
    f = fopen(CONF_PATH, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    fprintf(f, "accent_index = %d\n", g_accent_idx);
    fprintf(f, "button_lock_enabled = %d\n", button_lock_enabled);
    fprintf(f, "usb_bypass_enabled = %d\n", usb_bypass_enabled);
    fprintf(f, "cover_palette_enabled = %d\n", cover_palette_enabled);
    fprintf(f, "bt_autoplay_enabled = %d\n", bt_autoplay_enabled);
    fprintf(f, "theme_mode = %d\n", theme_mode);
    fprintf(f, "tz_index = %d\n", tz_idx);
    fprintf(f, "shuffle_enabled = %d\n", shuffle_enabled);
    fprintf(f, "repeat_mode = %d\n", repeat_mode);
    fprintf(f, "sleep_minutes = %d\n", sleep_minutes());
    fprintf(f, "deep_sleep = %d\n", deep_sleep_enabled);
    fprintf(f, "auto_off_minutes = %d\n", auto_off_minutes());
    fprintf(f, "bt_vol_steps = %d\n", audio_bt_vol_steps());
    /* BG38 */
    fprintf(f, "eq_on = %d\n", eq_enabled());
    if (eq_cur_path[0]) fprintf(f, "eq_profile_path = %s\n", eq_cur_path);
    /* R64: the tracked *intended* state (see wifi_pref/bt_pref's own
     * comment), not a fresh st_wifi_on()/st_bt_on() query -- both toggle
     * scripts background themselves and take real seconds, so a query made
     * here, right after firing one, would almost always still read the
     * state from *before* the toggle. */
    fprintf(f, "wifi_enabled = %d\n", wifi_pref);
    fprintf(f, "bt_enabled = %d\n", bt_pref);
    if (brightness_pref > 0) fprintf(f, "brightness = %d\n", brightness_pref);
    fprintf(f, "ab_speed_permille = %d\n", ab_speed_permille);   /* BG91 */
    fprintf(f, "pod_speed_permille = %d\n", pod_speed_permille); /* BG91 */
    fclose(f);
}

/* ---- R66: resume after auto-shutdown -------------------------------------
 *
 * The auto-shutdown timer powers the device off after a set idle period, and
 * before this the way back in was a cold start: main menu, nothing loaded,
 * and the stock boot logo in between. Everything needed to do better is
 * already on hand at the moment the timer fires -- what was loaded, where
 * the user was, and the exact pixels they last saw -- so it is written out
 * then and picked back up on the next start.
 *
 * Deliberately one-shot and auto-shutdown-only: both files are written from
 * that one call site and unlinked as soon as they are consumed, so a manual
 * power-off, a crash-restart or an ordinary reboot all still cold-start.
 * Restoring a user's screen after a *crash* would be actively wrong -- it
 * would hide the restart it is meant to be honest about.
 *
 * The thumbnail is stored downscaled by RS_SHIFT rather than as a full
 * framebuffer: 47 KB instead of 750 KB on a partition with ~22 MB free, and
 * the blur R66 asks for falls out of the bilinear upscale on the way back
 * in for free rather than needing a separate pass over 384000 pixels. The
 * downscale is a plain box average, which is itself the first half of that
 * blur. */
#define RESUME_STATE "/usr/data/resume.state"
#define RESUME_THUMB "/usr/data/resume.thumb"
#define RS_SHIFT 2                        /* 4x each axis */
#define RS_W (FB_W >> RS_SHIFT)           /* 120 */
#define RS_H (FB_H >> RS_SHIFT)           /* 200 */
#define RS_BOX (1 << RS_SHIFT)
#define RM_MUSIC     0
#define RM_AUDIOBOOK 1
#define RM_PODCAST   2

/* Static rather than on the stack: 48 KB, and both users are called exactly
 * once per process, so there is nothing to gain from it being automatic. */
static uint16_t rs_thumb[RS_W * RS_H];

static void resume_save(const uint16_t *front) {
    if (front) {
        for (int ty = 0; ty < RS_H; ty++) {
            for (int tx = 0; tx < RS_W; tx++) {
                unsigned r = 0, g = 0, b = 0;
                for (int dy = 0; dy < RS_BOX; dy++) {
                    const uint16_t *row = front
                                        + (size_t)((ty << RS_SHIFT) + dy) * FB_W
                                        + (tx << RS_SHIFT);
                    for (int dx = 0; dx < RS_BOX; dx++) {
                        uint16_t p = row[dx];
                        r += (p >> 11) & 0x1F;
                        g += (p >> 5) & 0x3F;
                        b += p & 0x1F;
                    }
                }
                rs_thumb[ty * RS_W + tx] =
                    (uint16_t)(((r / (RS_BOX * RS_BOX)) << 11) |
                               ((g / (RS_BOX * RS_BOX)) << 5) |
                                (b / (RS_BOX * RS_BOX)));
            }
        }
        FILE *tf = fopen(RESUME_THUMB ".tmp", "wb");
        if (tf) {
            int ok = fwrite(rs_thumb, sizeof(rs_thumb), 1, tf) == 1 && fflush(tf) == 0;
            fclose(tf);
            if (!ok || rename(RESUME_THUMB ".tmp", RESUME_THUMB) != 0)
                unlink(RESUME_THUMB ".tmp");
        }
    }

    /* Radio is a live stream, not a position in a file -- there is nothing
     * to come back to, so it saves no playback at all and resumes as a plain
     * navigation restore. */
    int mode = audiobook_mode ? RM_AUDIOBOOK : podcast_mode ? RM_PODCAST : RM_MUSIC;
    const char *path = (!radio_mode && cur_track >= 0 && cur_track < queue_n)
                     ? queue[cur_track].path : "";
    /* Same pending-seek preference the resume files themselves use (BG80):
     * a position saved while a seek is still outstanding must be the target
     * asked for, not the decoder's not-there-yet reading of it. */
    int pending = audio_seek_pending_ms();
    int pos = pending >= 0 ? pending : audio_pos_ms();

    char tmp[sizeof(RESUME_STATE) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", RESUME_STATE);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "mode=%d\n", mode);
    fprintf(f, "screen=%d\n", (int)screen);
    fprintf(f, "pos=%d\n", pos);
    fprintf(f, "path=%s\n", path);
    fprintf(f, "album=%s\n", q_album);
    fprintf(f, "artist=%s\n", q_artist);
    fprintf(f, "feed=%s\n", cur_feed);
    fprintf(f, "abdir=%s\n", ab_book.dir);
    int ok = (fflush(f) == 0);
    fclose(f);
    if (!ok || rename(tmp, RESUME_STATE) != 0) unlink(tmp);
    else mlog("[music] resume state saved (mode %d, screen %d, %dms)\n", mode, (int)screen, pos);
}

static int resume_pending(void) { return access(RESUME_STATE, F_OK) == 0; }

/* 0..256 blend of two RGB565s, per channel in their own widths. */
static uint16_t rs_lerp(uint16_t a, uint16_t b, int t) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    return (uint16_t)(((ar + (((br - ar) * t) >> 8)) << 11) |
                      ((ag + (((bg - ag) * t) >> 8)) << 5)  |
                       (ab + (((bb - ab) * t) >> 8)));
}

/* rs_thumb -> a full-panel image, by bilinear upsample -- a 120x200 source
 * stretched to 480x800 is inherently soft, which is what stands in for an
 * actual blur here. dim halves every channel on top of that; both users
 * below share this loop rather than each keeping their own copy of it. */
static void resume_upscale(uint16_t *fb, int dim) {
    for (int y = 0; y < FB_H; y++) {
        int syq = (y << (8 - RS_SHIFT));
        int sy = syq >> 8, fy = syq & 0xFF;
        if (sy > RS_H - 2) { sy = RS_H - 2; fy = 255; }
        const uint16_t *r0 = rs_thumb + sy * RS_W;
        const uint16_t *r1 = r0 + RS_W;
        uint16_t *out = fb + (size_t)y * FB_W;
        for (int x = 0; x < FB_W; x++) {
            int sxq = (x << (8 - RS_SHIFT));
            int sx = sxq >> 8, fx = sxq & 0xFF;
            if (sx > RS_W - 2) { sx = RS_W - 2; fx = 255; }
            uint16_t p = rs_lerp(rs_lerp(r0[sx], r0[sx + 1], fx),
                                 rs_lerp(r1[sx], r1[sx + 1], fx), fy);
            if (dim)
                /* Halve every channel in place -- one shift each, no second
                 * pass over the frame. */
                p = (uint16_t)(((((p >> 11) & 0x1F) >> 1) << 11) |
                               ((((p >> 5) & 0x3F) >> 1) << 5)  |
                                ((p & 0x1F) >> 1));
            out[x] = p;
        }
    }
}

/* The saved screen, blurred and dimmed. Painted before lib_open() so it is
 * on the panel for the whole of the library open and the restore below
 * rather than after them.
 *
 * Dimmed as well as blurred: it has to be unmistakably not a live screen,
 * which is the whole point -- a sharp copy of the last frame is exactly
 * what a frozen device looks like. rs_thumb is left populated on return
 * (not cleared, not re-read from disk anywhere else): resume_bridge_splash()
 * below reuses this exact same read for its own, undimmed upscale, once
 * restore has decided what is actually coming back. */
static void resume_splash(uint16_t *fb) {
    FILE *f = fopen(RESUME_THUMB, "rb");
    if (!f) return;
    size_t got = fread(rs_thumb, 1, sizeof(rs_thumb), f);
    fclose(f);
    unlink(RESUME_THUMB);
    if (got != sizeof(rs_thumb)) return;
    resume_upscale(fb, 1);
}

/* The bridge between "frozen, dimmed splash" and "the real screen, playing
 * again": full brightness, same soft upscale, painted the moment restore has
 * decided what track/chapter/episode is coming back -- which is also the
 * moment art_request() fires for it and art_bits goes back to NULL. Without
 * this, the very next dirty frame painted the real Now Playing screen with
 * no cover at all (the fresh decode hadn't landed yet), which read as the
 * artwork flashing away and back. This is not that flash: the old cover is
 * still the same pixels, just soft, until the sharp one is ready to replace
 * it directly -- see resume_wait_art in the frame loop. */
static void resume_bridge_splash(uint16_t *fb) {
    resume_upscale(fb, 0);
}

/* Blends the currently-visible page toward a fully-rendered `target` frame
 * over a handful of steps, alternating pages the same way an ordinary dirty
 * frame does below, so a jump between two very different images -- dim to
 * full brightness, or the soft bridge splash to the sharp real screen --
 * reads as a quick fade rather than a cut. Reported live as "a bit rough"
 * with both of those done as a single hard swap.
 *
 * Blocking and self-paced (its own usleep, not the input-driven loop this
 * runs before): both call sites are between real frames, at a point where
 * there is nothing else to service meanwhile. */
#define RESUME_FADE_STEPS   8
#define RESUME_FADE_STEP_US 28000

static void resume_fade_to(uint16_t *base, size_t page_px, int fbfd,
                           struct fb_var_screeninfo *v, int *page,
                           const uint16_t *target) {
    uint16_t *from = malloc(page_px * sizeof(uint16_t));
    if (!from) {
        /* No memory for the blend buffer -- land directly rather than not
         * transitioning at all. */
        memcpy(base + (size_t)(*page) * page_px, target, page_px * sizeof(uint16_t));
        memcpy(base + (size_t)(*page ^ 1) * page_px, target, page_px * sizeof(uint16_t));
        return;
    }
    memcpy(from, base + (size_t)(*page) * page_px, page_px * sizeof(uint16_t));

    for (int step = 1; step <= RESUME_FADE_STEPS; step++) {
        int t = step * 256 / RESUME_FADE_STEPS;
        if (t > 256) t = 256;
        int next = *page ^ 1;
        uint16_t *out = base + (size_t)next * page_px;
        for (size_t i = 0; i < page_px; i++) out[i] = rs_lerp(from[i], target[i], t);
        v->yoffset = (uint32_t)(next * FB_H);
        if (ioctl(fbfd, FBIOPAN_DISPLAY, v) < 0) break;
        *page = next;
        usleep(RESUME_FADE_STEP_US);
    }
    /* Land exactly on target either way -- t=256 above already reduces to
     * an exact copy, but pin both pages explicitly so nothing screenshotting
     * the spare page mid-fade (there is no such caller today, but see the
     * screenshot-mirroring comment on the ordinary draw path) ever reads a
     * partially-blended frame, and so repeated blending can't leave banding
     * behind. */
    memcpy(base + (size_t)(*page) * page_px, target, page_px * sizeof(uint16_t));
    memcpy(base + (size_t)(*page ^ 1) * page_px, target, page_px * sizeof(uint16_t));
    free(from);
}

/* Rebuild what was loaded, then land on the screen it was being viewed
 * from. Playback is restored *paused* whatever it was doing before: the
 * device only auto-shuts-down while idle and locked, so nothing was
 * playing at the time anyway, and starting audio by itself on a device the
 * user has just picked up would be a surprise, not a convenience. */
static void resume_try_restore(void) {
    FILE *f = fopen(RESUME_STATE, "r");
    if (!f) return;
    int mode = -1, scr = -1, pos = 0;
    char path[LIB_PATH_LEN] = "", album[LIB_NAME_LEN] = "", artist[LIB_NAME_LEN] = "";
    char feed[POD_NAME_LEN] = "", abdir[AB_PATH_LEN] = "";
    char line[LIB_PATH_LEN + 32];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        int v;
        /* %[^\r\n] rather than %s throughout: every one of these can carry
         * spaces (album titles, feed names, SD-card folder paths). */
        if (sscanf(line, "mode=%d", &v) == 1) mode = v;
        else if (sscanf(line, "screen=%d", &v) == 1) scr = v;
        else if (sscanf(line, "pos=%d", &v) == 1) pos = v;
        else if (!strncmp(line, "path=", 5))   snprintf(path, sizeof(path), "%s", line + 5);
        else if (!strncmp(line, "album=", 6))  snprintf(album, sizeof(album), "%s", line + 6);
        else if (!strncmp(line, "artist=", 7)) snprintf(artist, sizeof(artist), "%s", line + 7);
        else if (!strncmp(line, "feed=", 5))   snprintf(feed, sizeof(feed), "%s", line + 5);
        else if (!strncmp(line, "abdir=", 6))  snprintf(abdir, sizeof(abdir), "%s", line + 6);
    }
    fclose(f);
    unlink(RESUME_STATE);        /* one-shot, whatever happens below */

    int restored = 0;
    if (mode == RM_AUDIOBOOK && abdir[0]) {
        /* Exactly the sequence tapping the book in the list runs -- the
         * position itself comes from audiobook_resume.txt via
         * ab_resume_book(), which is authoritative and already correct, so
         * `pos` is deliberately not applied here. */
        ab_book_n = ab_scan_books(ab_books, AB_MAX_BOOKS);
        ab_rebuild_rows();
        for (int i = 0; i < ab_book_n; i++) {
            if (strcmp(ab_books[i].dir, abdir) != 0) continue;
            ab_load_book(&ab_books[i]);
            audiobook_mode = 1;
            audio_set_speed(ab_speed_permille);
            ab_playing[0] = '\0';
            queue_n = 0;
            ab_resume_book();
            restored = 1;
            break;
        }
    } else if (mode == RM_PODCAST && feed[0] && path[0]) {
        pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
        pod_rebuild_rows();
        snprintf(cur_feed, sizeof(cur_feed), "%s", feed);
        pod_ep_n = pod_load_episodes(cur_feed, pod_eps, POD_MAX_ITEMS);
        pod_rebuild_tracks();
        snprintf(cur_album, sizeof(cur_album), "%s", cur_feed);
        cur_artist[0] = '\0';
        browsing_is_playlist = 0;
        ab_list = 0; pod_list = 1;
        audio_set_speed(pod_speed_permille);
        for (int i = 0; i < pod_ep_n; i++) {
            if (strcmp(pod_eps[i].path, path) != 0) continue;
            pod_play_episode(i);      /* looks its own position up, same as a tap */
            restored = 1;
            break;
        }
    } else if (mode == RM_MUSIC && path[0] && album[0]) {
        /* Music has no resume file of its own the way books and episodes
         * do, so this is the one branch that carries its own position. */
        snprintf(cur_album, sizeof(cur_album), "%s", album);
        snprintf(cur_artist, sizeof(cur_artist), "%s", artist);
        browsing_is_playlist = 0;
        ab_list = 0; pod_list = 0;
        track_n = lib_tracks_for_album(cur_artist, cur_album, tracks,
                                       (int)(sizeof(tracks) / sizeof(tracks[0])), 0);
        for (int i = 0; i < track_n; i++) {
            if (strcmp(tracks[i].path, path) != 0) continue;
            play_from_list(i);
            if (pos > 0) audio_seek_ms(pos);
            restored = 1;
            break;
        }
    }

    /* Paused, not playing -- see this function's own comment. audio_toggle()
     * rather than a stop: the decoder stays open at the restored position,
     * so the first press of play is instant and the progress bar is already
     * showing the right place. */
    if (restored && audio_is_active() && !audio_is_paused()) audio_toggle();

    /* Navigation, as an explicit whitelist. Most screens are backed by state
     * only their own entry path builds (a facet query, a scanned row list, a
     * loaded EQ profile); landing on one whose backing state was never built
     * would show an empty or wrong list rather than where the user was. These
     * are the ones the restore above has already set up, plus the two row
     * lists and one menu that are cheap to rebuild from nothing. */
    screen_t want = restored ? SC_PLAYING : SC_MENU;
    switch (scr) {
        case SC_PLAYING:
        case SC_QUEUE:
            if (restored) want = (screen_t)scr;
            break;
        case SC_TRACKS:
            if (restored && track_n > 0) want = SC_TRACKS;
            break;
        case SC_AUDIOBOOKS:
            ab_book_n = ab_scan_books(ab_books, AB_MAX_BOOKS);
            ab_rebuild_rows();
            total = ab_book_n;
            want = SC_AUDIOBOOKS;
            break;
        case SC_PODCASTS:
            pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
            pod_rebuild_rows();
            total = pod_feed_n;
            want = SC_PODCASTS;
            break;
        case SC_MUSIC_MENU:
            want = SC_MUSIC_MENU;
            break;
        default:
            break;
    }
    screen = want;
    reset_scroll();
    mlog("[music] resumed: mode %d, restored %d, screen %d -> %d\n",
         mode, restored, scr, (int)want);
}

static int locked;
static int g_fbfd = -1;

/* R20: a stronger lock than the plain screen lock above -- opt-in via
 * Settings, and once on, every hardware button is swallowed rather than
 * just the screen staying dark while media keys keep working. */
#define DOUBLE_PRESS_MS 400
static int button_locked;
/* The event's own kernel timestamp, not the time this loop got round to
 * reading it. Those are the same thing only while the loop runs flat out;
 * once it slows down when locked (which is the point -- see the sleep at the
 * bottom of the main loop), two presses 200 ms apart can be read one poll
 * apart and measure as however long that poll took, quietly putting a real
 * double press outside DOUBLE_PRESS_MS. Reading ev.time makes the window mean
 * what it says regardless of how often anyone looks. */
static r1_input_event_t last_power_press;   /* only .tv_sec/.tv_usec read */
static int have_last_power_press;

/* BG98: measures rather than guesses whether a wake is slow, and by how
 * much. Set the moment a power press is recognised as "wake this up" (both
 * branches below), read and logged once the next real frame actually
 * reaches the panel -- this thread's own us_now() (CLOCK_MONOTONIC) on both
 * ends, not the input event's own kernel timestamp, so the fixed ~0-100ms
 * the 10Hz locked poll itself costs (present in every build, unrelated to
 * BG90/BG98) contributes equally to every measurement rather than being
 * folded into a number that looks like it's all contention. 0 = nothing
 * pending. */
static uint64_t g_wake_t0;
/* BG98 follow-up: the total (g_wake_t0 to first frame) turned out to be a
 * very consistent ~300ms with no measurable correlation to BG90's own
 * bt_sink_connected() timing -- these three break that total down further,
 * to find out where inside it the time actually goes. All relative to
 * g_wake_t0; logged alongside it, once, at the same point the total is. */
static uint64_t g_wake_setlocked_us;   /* just the set_locked(0) call itself */
static uint64_t g_wake_predraw_us;     /* set_locked(0) returning to the draw_ui() call site */

static int read_int_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void write_int_file(const char *path, int v) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d", v);
    fclose(f);
}

static void write_text_file(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(s, f);
    fclose(f);
}

/* Advances only while button_locked (the caller checks); a plain triangle
 * ramp, colour swapping every LED_SWAP_TICKS. The every-third-tick throttle
 * that used to be here existed to get 30 Hz of loop down to 10 Hz of sysfs
 * writes; the loop itself now runs at 10 Hz whenever this is called, so the
 * write rate is already what it was and throttling again would only make the
 * ramp visibly steppy. */
static int led_tick;

static void led_pulse_step(void) {
    {
        int swap_to_red = (led_tick / LED_SWAP_TICKS) & 1;
        int p = led_tick % LED_PULSE_TICKS;
        int half = LED_PULSE_TICKS / 2;
        int level = p < half ? (p * LED_ON) / half
                              : ((LED_PULSE_TICKS - p) * LED_ON) / half;
        if (level < 0) level = 0;
        if (level > LED_ON) level = LED_ON;
        write_int_file(swap_to_red ? LED_RED : LED_BLUE, level);
        write_int_file(swap_to_red ? LED_BLUE : LED_RED, 0);
    }
    led_tick++;
}

/* ---- low-power idle ------------------------------------------------------ */
/* This used to drop the Wi-Fi association after sleep_minutes() of sitting
 * locked, and put it back on the way out. That is gone, for two reasons.
 *
 * The first is that it stopped being worth anything: deep_suspend() below
 * stops the SoC outright, which takes the radio with it, so switching Wi-Fi
 * off a few seconds beforehand saves a rounding error.
 *
 * The second is that it looked actively dangerous. Every lock and unlock ran
 * wifi_off.sh / wifi_on.sh, which kill wpa_supplicant and take the interface
 * down and up -- far more driver state-cycling than stock ever does, and
 * hiby_player polls the same driver throughout (there is a
 * `wpa_cli -i wlan0 signal_poll` child of it in the process list). After a
 * session of this the Broadcom driver wedged: dhd_dpc spinning at 97% of the
 * CPU in kernel space, load average 22, zero idle. Because dhd sits on the
 * SDIO bus the SD card shares, that starved storage too -- `sync` hung, an
 * exFAT mount took 492 seconds, and the device could not complete a reboot or
 * even `reboot -f`. It needed the battery pulling.
 *
 * Not proven to be the cause, but it is the one thing here doing something
 * stock does not, and the saving no longer justifies the suspicion. See BG27.
 */

/* ---- deep suspend -------------------------------------------------------- */
/* Suspend-to-RAM, which is the only thing left that actually stops the SoC and
 * the DDR rather than just idling them. Everything above is housekeeping by
 * comparison.
 *
 * The order below is not a guess. Writing `mem` cold was tried first and hangs
 * this device outright: the panel blanks, the backlight stays lit, and neither
 * the power button nor an RTC alarm armed and verified beforehand brings it
 * back, because the sequence never completes -- it cost two hard reboots to
 * establish. The working order was then recovered from open_hiby_player, a
 * third-party replacement player whose own implementation logs "returned from
 * mem sleep", i.e. actually resumes:
 *
 *     /usr/bin/bt_suspend  ->  blank fb via sysfs  ->  echo mem  ->  bt_resume
 *
 * bt_suspend is the part we were missing and is not optional: bluealsa,
 * bluetoothd and bt-agent all sit on the SDIO/UART the suspend path has to
 * quiesce, and the stock script even carries its own warning that the sleep(1)
 * inside it is load-bearing when called from C. The framebuffer is blanked
 * through /sys/class/graphics/fb0/blank rather than the FBIOBLANK ioctl,
 * because that is what the implementation known to work on this hardware
 * uses. Both scripts are byte-identical to stock 1.6 on this firmware. */
#define FB_BLANK_NODE  "/sys/class/graphics/fb0/blank"
#define PWR_STATE      "/sys/power/state"
#define RTC_WAKEALARM  "/sys/class/rtc/rtc0/wakealarm"
#define RTC_EPOCH      "/sys/class/rtc/rtc0/since_epoch"

/* Belt and braces, and cheap: if the power button ever fails to wake the
 * device the RTC brings it back rather than leaving it dead in a pocket, which
 * is exactly the failure this feature could otherwise produce. Cancelled on
 * the way out, so it costs nothing whenever the button works. */
#define SUSPEND_RTC_BACKSTOP_S 900

static int suspend_ok(void) {
    if (!deep_sleep_enabled) return 0;
    /* Playing is the only thing that blocks this, and the caller has already
     * excluded it. A *connected* headset used to block it too, on the grounds
     * that bt_suspend would drop the link -- but connected is not the same as
     * in use, the drop is recoverable (bt_resume rebuilds the stack and puts
     * the adapter back on), and waking by button is accepted. Left in, that
     * gate would have meant anyone who leaves a headset paired never suspends
     * at all, which is most of the saving gone for a reconnect nobody asked
     * to avoid. */
    return 1;
}

static void deep_suspend(void) {
    /* Stock's bt_resume ends with `bt-adapter --set "Powered" "Off"`, so a
     * round trip through suspend leaves Bluetooth switched off even if it was
     * on beforehand -- verified on device, hci0 reads DOWN afterwards. Note
     * what it was so it can be put back; anything else silently changes a
     * setting the user chose. */
    int bt_was_on = st_bt_on();
    int now = read_int_file(RTC_EPOCH);
    if (now > 0) {
        write_int_file(RTC_WAKEALARM, 0);                       /* clear any stale alarm */
        write_int_file(RTC_WAKEALARM, now + SUSPEND_RTC_BACKSTOP_S);
    }
    mlog("[music] suspend: bluetooth teardown\n");
    if (system("/usr/bin/bt_suspend >/dev/null 2>&1") == -1) { /* carry on regardless */ }
    write_int_file(FB_BLANK_NODE, 4);                           /* FB_BLANK_POWERDOWN */

    mlog("[music] suspend: entering mem sleep\n");
    write_text_file(PWR_STATE, "mem");                          /* blocks until resume */
    mlog("[music] suspend: resumed\n");

    write_int_file(RTC_WAKEALARM, 0);                           /* backstop no longer needed */
    /* Backgrounded: bt_resume re-runs brcm_patchram_plus with a sleep 5 and
     * several more after it, so it is ten seconds of work. Blocking the UI
     * thread for that would freeze the very double-press the user is about to
     * make to unlock. Same pattern st_wifi_set/st_bt_set already use.
     *
     * The power-on is chained onto it inside the same background shell rather
     * than issued here, because it has to happen *after* bt_resume's ten
     * seconds of stack rebuilding, not racing it. */
    if (system(bt_was_on
               ? "( /usr/bin/bt_resume; bt-adapter --set Powered On ) >/dev/null 2>&1 &"
               : "/usr/bin/bt_resume >/dev/null 2>&1 &") == -1) { }
}

static void set_locked(int on) {
    if (on == locked) return;
    locked = on;
    audio_set_screen_locked(on);   /* BG90 follow-up -- see its own comment in audio.h */
    if (on) {
        /* BG95 follow-up, found live: the button-lock double-press handler
         * (see its own comment) deliberately forces set_locked() to re-run
         * its full body even when this app already believes it's locked --
         * needed so a double-press can still toggle button_locked on top of
         * an already-locked screen. But a real double *press* (not just a
         * bounce) reaching that handler within DOUBLE_PRESS_MS of a plain
         * single-press lock means this exact line runs a second time with
         * the backlight already at zero from the first -- read_int_file()
         * comes back ~0, silently overwriting the real value this same
         * call had just captured moments earlier. music.log showed it
         * happening on nearly every lock for this user specifically (two
         * "locked" lines ~200ms apart, well inside the 400ms window --
         * their own habitual double-click, not hardware bounce), and every
         * wake after one used that clobbered value or DEFAULT_BRIGHTNESS
         * instead of the level actually in use, reported live as
         * brightness dropping "to 50%" between wakes. Only overwrite while
         * there's still something real on the panel to capture -- a
         * redundant re-lock of an already-dark screen has nothing to gain
         * from re-reading it anyway. */
        int b = read_int_file(BACKLIGHT);
        if (b > 0) saved_brightness = b;
        write_int_file(BACKLIGHT, 0);
        /* Dark is not off. With only the backlight at zero the LCD controller
         * carries on scanning out: measured at ~61 framebuffer interrupts a
         * second while locked, which is 480*800*2 bytes pulled from DDR sixty
         * times a second -- around 46 MB/s of memory traffic to display a
         * black screen nobody is looking at, keeping the memory controller
         * busy the whole time the device is in a pocket. Powering the panel
         * down stops the scanout as well as the light.
         *
         * The matching unblank below already existed and is unconditional, so
         * the way back is the path that was always taken -- and the BG6
         * watchdog deliberately leaves a blanked panel alone while locked, so
         * it will not fight this. */
        if (g_fbfd >= 0) ioctl(g_fbfd, FBIOBLANK, FB_BLANK_POWERDOWN);
    } else {
        /* BG98 follow-up: set_locked(0) accounts for essentially the whole
         * ~300ms measured wake latency -- splitting its own two operations
         * to find out which one it actually is. */
        uint64_t tb0 = us_now();
        /* Unblank unconditionally. The player can power the framebuffer down
         * underneath us on its own display timeout, and restoring brightness
         * to a blanked panel leaves a black screen that no amount of pressing
         * will bring back. It costs nothing if it was never blanked. */
        if (g_fbfd >= 0) ioctl(g_fbfd, FBIOBLANK, FB_BLANK_UNBLANK);
        uint64_t tb1 = us_now();
        /* Through st_brightness_set(), not a direct write: saved_brightness
         * can be the literal max (the slider clamps to qs_bright_max, 101),
         * and writing that value directly goes dark instead of brightest --
         * st_brightness_set() is the one place that caps it to max-1. */
        st_brightness_set(saved_brightness > 0 ? saved_brightness : DEFAULT_BRIGHTNESS);
        mlog("[music] unblank %lu us, brightness-restore %lu us\n",
             (unsigned long)(tb1 - tb0), (unsigned long)(us_now() - tb1));
    }
    mlog("[music] %s\n", on ? "locked" : "unlocked");
}

/* ---- hardware keys ------------------------------------------------------- */
/* Two devices carry media keys and both are worth having:
 *
 *   event3  earpods_adc  — the inline remote: volume, play/pause, prev/next,
 *                          rewind and fast-forward
 *   event2  jz adc keyboard — the side buttons: volume, play/pause, prev
 *
 * Both are grabbed while the app is open so the stock player does not act on
 * the same press as well.
 */
#define KEY_VOLUMEDOWN_  114
#define KEY_VOLUMEUP_    115
#define KEY_NEXTSONG_    163
#define KEY_PLAYPAUSE_   164
#define KEY_PREVSONG_    165
#define KEY_REWIND_      168
#define KEY_FASTFWD_     208
#define KEY_POWER_       116
/* What BlueZ's AVRCP input device actually sends. It does not use PLAYPAUSE:
 * a headset's play and pause arrive as distinct PLAYCD and PAUSECD, which is
 * why the buttons appeared dead while the presses were reaching the log. */
#define KEY_PAUSE_       119
#define KEY_STOPCD_      166
#define KEY_PLAYCD_      200
#define KEY_PAUSECD_     201
#define KEY_PLAY_        207



/* The unit's own buttons do not report what their labels say: the button
 * marked skip-forward sends PLAYPAUSE and skip-back sends NEXTSONG. Mapping by
 * keycode alone therefore had forward pausing and back skipping forward. The
 * inline remote is a normal remote and does mean what it sends, so the two are
 * mapped separately rather than one being bent to fit the other.
 *
 * This unit has no play/pause button of its own; that control is on screen. */
typedef enum { KEYS_BUTTONS = 0, KEYS_REMOTE } key_src_t;

/* Returns non-zero if anything happened, so the caller can mark the UI dirty;
 * -1 means the fd itself has died and the caller should close and forget it
 * (see the comment below the loop). */
static int handle_keys(int fd, key_src_t src) {
    r1_input_event_t ev;
    int acted = 0;
    ssize_t r;
    while (fd >= 0 && (r = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY) continue;
        /* R77: the one key this function needs a release event for at all --
         * every other key here only ever means anything on the way down.
         * Handled ahead of the generic "presses only" filter below, which
         * still applies to everything else including this key's own press. */
        if (ev.code == KEY_POWER_ && ev.value == 0) {
            if (power_key_down) {
                long elapsed_ms = (long)((us_now() - power_key_down_us) / 1000);
                power_key_down = 0;
                int graceful = 0;
                if (!power_hard_fired) {
                    if (elapsed_ms < POWER_HOLD_UI_MS) {
                        if (power_key_pending_lock) set_locked(1);   /* the ordinary tap, just deferred */
                    } else if (elapsed_ms >= POWER_HOLD_SAFE_MS && elapsed_ms < POWER_HOLD_FORCE_MS) {
                        power_release_outcome = 1;   /* graceful: main loop has `base` for resume_save() */
                        graceful = 1;
                    }
                    /* Between POWER_HOLD_UI_MS and POWER_HOLD_SAFE_MS: a hold
                     * that didn't go anywhere -- cancel outright, no action. */
                }
                /* Left showing when the outcome is graceful -- the main loop
                 * flips it to the "Shutting down" message once it processes
                 * power_release_outcome, and poweroff itself is not instant
                 * (see its own call site's comment); popping the overlay now
                 * would leave a bare instant of the ordinary screen showing
                 * again in between. */
                if (!graceful && !power_hard_fired) power_hold_ui_shown = 0;
                power_key_pending_lock = 0;
                mlog("[music] power released after %ldms\n", elapsed_ms);
                acted = 1;
            }
            continue;
        }
        if (ev.value == 0) continue;   /* presses only, for everything else */
        mlog("[music] key %d\n", ev.code);
        /* Power is the lock key, and the only one that wakes the screen. A
         * double press (within DOUBLE_PRESS_MS) toggles the stronger
         * button lock instead, when that's enabled in Settings -- checked
         * before anything else so it still works while button_locked is
         * swallowing every other key below. */
        if (ev.code == KEY_POWER_) {
            long ms = have_last_power_press
                    ? (ev.tv_sec - last_power_press.tv_sec) * 1000L +
                      (ev.tv_usec - last_power_press.tv_usec) / 1000L
                    : -1;
            last_power_press = ev;
            have_last_power_press = 1;

            if (button_lock_enabled && ms >= 0 && ms < DOUBLE_PRESS_MS) {
                have_last_power_press = 0;   /* don't chain into a third press */
                button_locked = !button_locked;
                locked = !button_locked;     /* so set_locked() below isn't a same-state no-op */
                if (!button_locked) {
                    /* BG95 follow-up: the "dark for any reason" wake path
                     * below already trusts last_lit_bright over
                     * saved_brightness for exactly this reason -- this path
                     * unlocks just as often (arguably more, since it's the
                     * one two power presses close together actually reach)
                     * but never had the same protection. Worth having
                     * regardless of what's currently corrupting
                     * saved_brightness -- see set_locked()'s own comment on
                     * the specific way this user's double-click habit was
                     * doing it -- since this is the general-purpose fix, not
                     * a patch for one specific cause. */
                    if (last_lit_bright > 0) saved_brightness = last_lit_bright;
                    g_wake_t0 = us_now();   /* BG98: this call is the wake */
                }
                set_locked(button_locked);
                if (!button_locked) g_wake_setlocked_us = us_now() - g_wake_t0;
                if (button_locked) {
                    led_tick = 0;            /* start each pulse cycle fresh, on blue */
                } else {
                    write_int_file(LED_BLUE, 0);
                    write_int_file(LED_RED, 0);
                }
                mlog("[music] button lock %s\n", button_locked ? "on" : "off");
                acted = 1; continue;
            }
            /* R77: hold-tracking starts on every qualifying press (i.e. one
             * that wasn't just consumed as the second half of a double
             * press above), button_locked included -- a hold-to-shutdown
             * has to work as a safety action regardless of the UI lock, even
             * though the ordinary tap action beneath it stays swallowed
             * exactly as before while locked. */
            power_key_down = 1;
            power_key_down_us = us_now();
            power_hard_fired = 0;

            if (button_locked) {
                power_key_pending_lock = 0;   /* nothing to defer; swallowed either way */
                acted = 1; continue;
            }

            /* set_locked(on) is a no-op when on == locked, which is right
             * for a double press but wrong here: the stock player can blank
             * the panel on its own display timeout without this app's
             * `locked` ever finding out, so the first press after that
             * happens sees locked==0 (still "awake" as far as this app
             * knows), toggles it to 1, and set_locked(1) on an already-dark
             * screen is invisible -- it took a second press to actually
             * wake it. Trust the real backlight value instead of the flag:
             * dark for any reason wakes it, in one press, regardless of
             * what locked currently believes. */
            int dark = read_int_file(BACKLIGHT) <= 0;
            if (dark) {
                /* BG95: set_locked(0) restores from saved_brightness, which
                 * is only ever written by set_locked(1) itself -- exactly
                 * the mechanism this comment's own "dark for any reason"
                 * case says not to assume. If the stock player's own timer
                 * did the blanking, saved_brightness was never touched and
                 * still holds whatever it was from this app's own *last*
                 * lock cycle (or its music.conf default, if there hasn't
                 * been one yet this session) -- reported live as brightness
                 * "resetting" on wake, to a level from before, not the one
                 * actually in use when the screen went dark. last_lit_bright
                 * is the BG6 watchdog's own continuously-sampled value,
                 * updated every ~0.5s any time the panel is actually lit
                 * regardless of which mechanism eventually blanks it, so
                 * it's still correct here no matter which one did. */
                if (last_lit_bright > 0) saved_brightness = last_lit_bright;
                g_wake_t0 = us_now();   /* BG98: this call is the wake */
                locked = 1; set_locked(0);
                g_wake_setlocked_us = us_now() - g_wake_t0;
                /* R77: wake already happened -- nothing left to defer, and a
                 * dark screen is exactly the case the countdown overlay
                 * most needs to have already woken by the time it appears. */
                power_key_pending_lock = 0;
            } else {
                /* R77: locking here -- unlike waking above -- blanks the
                 * panel outright (set_locked(1) powers the LCD down, see
                 * its own comment), which would hide the very countdown
                 * overlay a hold needs to draw on. Deferred until release
                 * proves this was just a tap, not the start of a hold. */
                power_key_pending_lock = 1;
            }
            acted = 1; continue;
        }

        /* Swallowed outright while button-locked -- but only this unit's own
         * buttons (BG20): the lock exists to stop a pocket from mashing the
         * touchscreen and the physical side buttons, not to cut off a
         * headset's remote, which is a deliberate action from a separate
         * object and the whole point of controlling playback without taking
         * the device out in the first place. */
        if (button_locked && src == KEYS_BUTTONS) { acted = 1; continue; }

        if (ev.code == KEY_VOLUMEUP_)   { audio_volume_step(+5); vol_ticks = VOL_TICKS; acted = 1; continue; }
        if (ev.code == KEY_VOLUMEDOWN_) { audio_volume_step(-5); vol_ticks = VOL_TICKS; acted = 1; continue; }

        if (src == KEYS_BUTTONS) {
            switch (ev.code) {
                case KEY_PLAYPAUSE_:            /* the skip-forward button */
                    /* R60: this unit's own button is physically labelled
                     * "skip forward", not "next" -- audiobook_mode used to
                     * jump a whole chapter here, a different amount than
                     * the on-screen +10s arc (the on-screen design
                     * deliberately dropped a chapter-skip triangle in
                     * favour of a seek arc with the amount written on it;
                     * see that draw code's own comment), so a hardware
                     * press did something the screen never offered and
                     * never confirmed happening. Now matches on-screen for
                     * both audiobook and podcast. */
                    if (audiobook_mode) { audio_seek_ms(audio_pos_ms() + 10000); seek_toast(+10000); }
                    else if (podcast_mode) { audio_seek_ms(audio_pos_ms() + 30000); seek_toast(+30000); }
                    else                play_index(next_track_index());
                    acted = 1; break;
                case KEY_NEXTSONG_:             /* the skip-back button */
                case KEY_PREVSONG_:
                    /* R60: podcast_mode was -30000 here against the
                     * on-screen skip-back arc's -10000 (the +30000 side
                     * above already matched) -- same mismatch, fixed the
                     * same way. */
                    if (audiobook_mode) { audio_seek_ms(audio_pos_ms() - 10000); seek_toast(-10000); }
                    else if (podcast_mode) { audio_seek_ms(audio_pos_ms() - 10000); seek_toast(-10000); }
                    else if (audio_pos_ms() > 3000) audio_seek_ms(0);
                    else play_index(prev_track_index());
                    acted = 1;
                    break;
                default: break;
            }
            continue;
        }

        switch (ev.code) {
            case KEY_PLAYPAUSE_:  audio_toggle();  acted = 1; break;
            /* Explicit play and pause, so a headset asking to pause cannot
             * toggle something already paused back into playing. */
            case KEY_PLAYCD_:
            case KEY_PLAY_:
                if (audio_is_paused()) audio_toggle();
                acted = 1;
                break;
            case KEY_PAUSECD_:
            case KEY_PAUSE_:
            case KEY_STOPCD_:
                if (!audio_is_paused() && audio_is_active()) audio_toggle();
                acted = 1;
                break;
            case KEY_NEXTSONG_:
                if (audiobook_mode) ab_play_chapter(cur_track + 1);
                else if (podcast_mode) { audio_seek_ms(audio_pos_ms() + 30000); seek_toast(+30000); }
                else                play_index(next_track_index());
                acted = 1;
                break;
            case KEY_PREVSONG_:
                /* Same rule as every other player: part-way in, previous means
                 * back to the start of this track — or of this chapter. A
                 * podcast episode instead gets the same -10s/+30s ad-skip as
                 * its on-screen transport (see the tap handler's comment).
                 * R60: this side used to be -30000, an asymmetric mismatch
                 * against the on-screen -10s arc even though the +30000 side
                 * above already matched it -- fixed to match both ways. */
                if (audiobook_mode) {
                    const ab_chapter_t *ch =
                        (cur_track >= 0 && cur_track < ab_book.chap_n)
                            ? &ab_book.chap[cur_track] : NULL;
                    int64_t into = audio_pos_ms() - (ch ? ch->file_start_ms : 0);
                    if (into > 3000) audio_seek_ms((int)(ch ? ch->file_start_ms : 0));
                    else ab_play_chapter(cur_track - 1);
                } else if (podcast_mode) {
                    audio_seek_ms(audio_pos_ms() - 10000);
                    seek_toast(-10000);
                } else if (audio_pos_ms() > 3000) {
                    audio_seek_ms(0);
                } else {
                    play_index(prev_track_index());
                }
                acted = 1;
                break;
            case KEY_FASTFWD_: {
                /* R60: was a flat +10000 for every mode, including podcast
                 * -- ignored its on-screen transport's own +30s forward
                 * amount (only the on-unit buttons' NEXTSONG_/PREVSONG_
                 * path had ever been taught that asymmetry). Audiobook and
                 * plain music already matched the on-screen amount at
                 * 10000, unchanged. */
                int d = podcast_mode ? 30000 : 10000;
                audio_seek_ms(audio_pos_ms() + d); seek_toast(+d);
                acted = 1; break;
            }
            case KEY_REWIND_:
                audio_seek_ms(audio_pos_ms() - 10000); seek_toast(-10000);
                acted = 1; break;
            default: break;
        }
    }
    /* A short read of 0 is a true EOF, not "no data queued" (that's EAGAIN,
     * expected on nearly every poll of a nonblocking fd and not an error).
     * EOF means the device itself is gone: BlueZ tears down and recreates
     * the AVRCP input node on every headset reconnect, reusing the same
     * eventN path scan_inputs() already has recorded, and its "already have
     * this name" check has no way to tell a now-dead fd from a live one by
     * name alone -- so it never reopens it, and this app is left polling a
     * handle that will never produce another event, permanently, until the
     * whole app restarts. RBG1: headset controls worked once, then silently
     * stopped after the first reconnect. Signalling death here lets the
     * caller close it and forget the name so the next scan picks the fresh
     * device back up. */
    if (fd >= 0 && r == 0) return -1;
    if (fd >= 0 && r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    return acted;
}

/* Open the fixed nodes plus anything that looks like a media remote. The
 * AVRCP device is a proper remote — its PLAYPAUSE means play/pause — unlike
 * this unit's own buttons, whose labels and keycodes disagree. */
/* Resolve an input device to its eventN node by name.
 *
 * The node numbers are not stable: they depend on the order the input drivers
 * register, which changes if any of them is loaded at a different point in
 * boot. Backgrounding the touchscreen module -- worth ~0.9s of boot time,
 * measured -- moves hyn_ts from event1 to event2 and shifts "jz adc keyboard"
 * down into event1, which silently swapped touch and the side buttons here
 * and left the screen unresponsive. Resolving by name makes that reordering a
 * non-event. scan_inputs() already did exactly this for AVRCP devices; this
 * just factors it out so the board's own devices get the same treatment.
 *
 * Returns 1 and fills `out` with e.g. "event2", or 0 if the name is absent. */
static int input_node_by_name(const char *want, char *out, size_t outsz) {
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return 0;
    char line[256], name[128] = "";
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "N: Name=", 8)) {
            snprintf(name, sizeof(name), "%s", line + 8);
        } else if (!strncmp(line, "H: Handlers=", 12) && strstr(name, want)) {
            char *ev = strstr(line, "event");
            if (!ev) continue;
            unsigned n = 0;
            while (n + 1 < outsz && ev[n] && ev[n] != ' ' && ev[n] != '\n') {
                out[n] = ev[n]; n++;
            }
            out[n] = '\0';
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void scan_inputs(void) {
    /* Named, with the historical node as a fallback purely in case
     * /proc/bus/input/devices is unreadable -- the names are what this board
     * actually reports, confirmed on device. */
    static const struct { const char *name; const char *fallback; int src; } fixed[] = {
        { "md-gpio-keys",    "event0", KEYS_BUTTONS },   /* power, next */
        { "jz adc keyboard", "event2", KEYS_BUTTONS },   /* side buttons */
        { "earpods_adc",     "event3", KEYS_REMOTE  },   /* inline remote */
    };
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        char node[32];
        if (!input_node_by_name(fixed[i].name, node, sizeof(node)))
            snprintf(node, sizeof(node), "%s", fixed[i].fallback);
        int have = 0;
        for (int k = 0; k < kfd_n; k++)
            if (!strcmp(kfd_name[k], node)) { have = 1; break; }
        if (have || kfd_n >= KFD_MAX) continue;
        char path[48];
        snprintf(path, sizeof(path), "/dev/input/%s", node);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGRAB, 1);
        kfd[kfd_n] = fd;
        kfd_src[kfd_n] = fixed[i].src;
        snprintf(kfd_name[kfd_n], sizeof(kfd_name[0]), "%s", node);
        kfd_n++;
    }

    /* Anything named AVRCP, whichever event number it landed on. */
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return;
    char line[256], name[128] = "";
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "N: Name=", 8)) {
            snprintf(name, sizeof(name), "%s", line + 8);
        } else if (!strncmp(line, "H: Handlers=", 12) && strstr(name, "AVRCP")) {
            char *ev = strstr(line, "event");
            if (!ev) continue;
            char node[32];
            unsigned n = 0;
            while (n + 1 < sizeof(node) && ev[n] && ev[n] != ' ' && ev[n] != '\n') {
                node[n] = ev[n]; n++;
            }
            node[n] = '\0';
            int have = 0;
            for (int k = 0; k < kfd_n; k++)
                if (!strcmp(kfd_name[k], node)) { have = 1; break; }
            if (have || kfd_n >= KFD_MAX) continue;
            char path[48];
            snprintf(path, sizeof(path), "/dev/input/%s", node);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            ioctl(fd, EVIOCGRAB, 1);
            kfd[kfd_n] = fd;
            kfd_src[kfd_n] = KEYS_REMOTE;
            snprintf(kfd_name[kfd_n], sizeof(kfd_name[0]), "%s", node);
            kfd_n++;
            mlog("[music] headset controls on %s\n", node);
        }
    }
    fclose(f);
}

/* ---- app -------------------------------------------------------------
 * Not `static`: the RP1-follow-on standalone build (standalone_main.c)
 * calls this directly from its own main(), bypassing the tile-hijack
 * trampoline entirely -- there is no launcher to hijack a tile from when
 * this binary is not running inside hiby_player. is_hiby_player()'s own
 * existing guard in music_init() already makes the constructor a safe
 * no-op under a different process name, so nothing else here needed to
 * change for that build to coexist with the normal LD_PRELOAD one. */
int music_entry(void *a0, void *a1) {
    (void)a0; (void)a1;
    g_is_standalone = !is_hiby_player();
    mlog("[music] entering app\n");
    load_conf();
    screen = SC_MENU; reset_scroll();

    /* R66: the framebuffer is set up before lib_open() rather than after so
     * the resume splash can be on the panel *during* the library open and
     * the restore that follows, which is the slow part and exactly the
     * stretch the user would otherwise spend looking at a boot logo. The
     * two are independent -- lib_open() only opens SQLite -- so nothing
     * else cares about the order. */
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { mlog("[music] no fb: %s\n", strerror(errno)); return 0; }
    g_fbfd = fbfd;                    /* set_locked needs it to unblank */

    struct fb_var_screeninfo v;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) < 0) {
        mlog("[music] vinfo failed\n"); close(fbfd); return 0;
    }

    size_t page_px = (size_t)FB_W * FB_H;
    size_t map_len = page_px * 2 * 2;
    uint16_t *base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (base == MAP_FAILED) {
        mlog("[music] mmap failed: %s\n", strerror(errno));
        close(fbfd); return 0;
    }

    /* Keep what the launcher had so it can be put back on the way out. */
    uint16_t *snapshot = malloc(page_px * 2);
    if (snapshot) memcpy(snapshot, base + (size_t)v.yoffset * FB_W, page_px * 2);

    /* Both pages, then pan to a known one: whichever the boot logo left
     * displayed is not something to assume, and the first real frame flips
     * to page 0 anyway (`page` starts at 0), so leaving page 1 unpainted
     * would show a torn half-splash for one frame at that flip.
     *
     * `page` itself lives here rather than down with frames/running/dirty
     * below (where it used to be declared) because the two fades between
     * here and the main loop starting need to track it exactly the same
     * way the loop's own dirty-frame flip does -- one shared variable, not
     * a second copy that could drift from what is actually on screen. */
    int page = 0;
    int resuming = resume_pending();
    if (resuming) {
        resume_splash(base);
        memcpy(base + page_px, base, page_px * 2);
        v.yoffset = 0;
        if (ioctl(fbfd, FBIOPAN_DISPLAY, &v) < 0)
            mlog("[music] resume splash pan failed: %s\n", strerror(errno));
    }

    if (lib_open() != 0) mlog("[music] library open failed\n");
    /* Whether the frame loop should hold the bridge splash up rather than
     * paint the real (still cover-less) Now Playing screen on its first
     * dirty pass -- see resume_bridge_splash()'s own comment. Only ever
     * true once restore has actually put something on the way back:
     * art_done reads false the instant art_request() dispatches a fresh
     * decode, and stays false only for that -- an idle SC_MENU restore
     * with nothing playing leaves it true (nothing was ever requested) and
     * the ordinary boot path skips all of this once resuming is false. */
    int resume_wait_art = 0;
    if (resuming) {
        resume_try_restore();
        if (!art_done) {
            resume_wait_art = 1;
            uint16_t *bridge = malloc(page_px * sizeof(uint16_t));
            if (bridge) {
                resume_upscale(bridge, 0);
                resume_fade_to(base, page_px, fbfd, &v, &page, bridge);
                free(bridge);
            } else {
                resume_bridge_splash(base);
                memcpy(base + page_px, base, page_px * 2);
            }
        }
    }

    /* Touch, by name for the same reason as scan_inputs()'s table above. */
    char tnode[32];
    if (!input_node_by_name("hyn_ts", tnode, sizeof(tnode)))
        snprintf(tnode, sizeof(tnode), "event1");
    char tpath[48];
    snprintf(tpath, sizeof(tpath), "/dev/input/%s", tnode);
    mlog("[music] touch on %s\n", tnode);
    int tfd = open(tpath, O_RDONLY | O_NONBLOCK);
    if (tfd >= 0 && ioctl(tfd, EVIOCGRAB, 1) < 0)
        mlog("[music] touch grab failed: %s\n", strerror(errno));

    /* event0 is not optional: it carries the power key, and it is also where
     * NEXTSONG lives — the skip button did nothing until this was opened,
     * because event2 only reports volume, play/pause and previous.
     *
     * A Bluetooth headset's own buttons are a different matter again: BlueZ
     * creates an input device for them ("WH-1000XM4 (AVRCP)") only when the
     * headset connects, which is usually after this app started. So the list
     * is rescanned rather than opened once. */
    scan_inputs();
    mlog("[music] keys: %d device(s) open\n", kfd_n);

    int jack_was = st_headset();

    bt_poll_run = 1;
    bt_thread_valid = (pthread_create(&bt_thread, NULL, bt_poll, NULL) == 0);

    /* Redrawing every 33 ms regardless burns CPU on a screen that is usually
     * static, and this app exists partly because the stock one is not smooth.
     * A frame is produced only when something actually changed: a gesture, the
     * clock ticking over a second, or artwork arriving. */
    int frames = 0, running = 1, dirty = 1;   /* page declared above, before the resume fades */
    int resume_wait_ticks = 0;   /* safety cap -- see the check below */
    int last_sec = -1, art_seen = 0, view_art_seen = 0, status_tick = 0, idle = 0, rescan_tick = 0;
    int sleep_idle = 0, auto_off_idle = 0;
    int ab_pos_tick = 0;
    int blank_tick = 0;    /* BG6 watchdog, see below -- last_lit_bright is file-scope now (BG95) */
    while (running) {
        g_tick++;
        int x, y;
        int g = (tfd >= 0) ? read_gesture(tfd, &x, &y) : 0;
        if (locked) g = 0;             /* drained above, acted on here: never */
        else if (g) idle = 0;

        /* Settings' "disable PEQ, MSEB and Bluetooth when playing over USB":
         * a pure runtime override, engaged and released purely by whether
         * output is actually USB right now -- never touches eq_enabled(),
         * mseb_on or their saved profile, so whatever the user's own EQ/MSEB
         * settings are, they come back exactly as chosen the moment USB
         * output stops. Same "note it, then put it back" idiom
         * deep_suspend() already uses for its own Bluetooth teardown. */
        {
            int want_bypass = usb_bypass_enabled && audio_using_usb();
            if (want_bypass && !usb_bypass_active) {
                usb_bypass_bt_was_on = st_bt_on();
                if (usb_bypass_bt_was_on) { st_bt_set(0); qs_bt = 0; }
                audio_set_usb_bypass(1);
                /* Save/pin volume before engaging the lock -- audio_set_volume()
                 * is the low-level setter the lock doesn't gate (see its own
                 * comment in audio.c), which is exactly what's needed to
                 * establish the pinned value itself. */
                usb_bypass_saved_vol = audio_volume();
                /* R89: pinned at 50%, not 100% -- reported live as audible
                 * popping on a USB DAC (FiiO BTR17) at full digital volume.
                 * This path sends unattenuated samples straight to whatever
                 * external DAC/amp is on the other end of the cable, which
                 * can have meaningfully more analog gain than the R1's own
                 * internal headphone amp -- 100% here was clipping the
                 * external amp's own stage, not this app's. 50% leaves real
                 * headroom without needing per-device calibration. */
                audio_set_volume(50);
                audio_set_vol_locked(1);
                usb_bypass_active = 1;
                mlog("[music] usb transport mode: engaged (peq/mseb off, vol -> 50%%%s)\n",
                     usb_bypass_bt_was_on ? ", bluetooth off" : "");
            } else if (!want_bypass && usb_bypass_active) {
                audio_set_usb_bypass(0);
                if (usb_bypass_bt_was_on) { st_bt_set(1); qs_bt = 1; }
                audio_set_vol_locked(0);
                audio_volume_set(usb_bypass_saved_vol);
                usb_bypass_active = 0;
                mlog("[music] usb transport mode: released (vol -> %d%%)\n", usb_bypass_saved_vol);
            }
        }

        /* R94: fires once, on a scan-running 1 -> 0 transition, whichever
         * pass (scanner.c's own MEDIA_TABLE walk or index.c's track_index/
         * album_cache rebuild) is the one still running when the other
         * finishes -- lib_albums_recent_added() reads album_cache, so
         * waiting for index_scan_running() specifically (not just
         * scanner_scan_running()) matters: starting the moment the file
         * walk alone finishes would read a cache that hasn't caught up to
         * it yet. */
        {
            static int scan_was_running;
            int scan_running = scanner_scan_running() || index_scan_running();
            if (scan_was_running && !scan_running) cover_prewarm_start();
            scan_was_running = scan_running;
        }

        /* BG38 (part 2): drain bt_poll's fuzzy-matched profile, if it found
         * one since the last time round. The switch itself has to happen
         * here, not on that thread -- see the comment on bt_match_profile(). */
        {
            char eqpath[EP_PATH_LEN];
            int no_match;
            pthread_mutex_lock(&bt_lock);
            snprintf(eqpath, sizeof(eqpath), "%s", bt_eq_pending_path);
            bt_eq_pending_path[0] = '\0';
            no_match = bt_eq_no_match;
            bt_eq_no_match = 0;
            pthread_mutex_unlock(&bt_lock);
            if (eqpath[0] && strcmp(eqpath, eq_cur_path) != 0) {
                eq_switch_to(eqpath);
                save_conf();
                dirty = 1;
            } else if (no_match && eq_enabled()) {
                /* R100: a device with no saved profile of its own gets a
                 * flat response rather than inheriting whatever the last
                 * device (or the user's own manual setting) left active.
                 * eq_enabled() itself, not the saved profile/bands, so
                 * reconnecting the *previous* matched device later still
                 * finds its own EQ exactly as it was. */
                eq_set_enabled(0);
                save_conf();
                dirty = 1;
            }
        }

        /* A track that reaches its end should roll into the next one; stopping
         * dead at every track boundary is the one thing a music player cannot
         * do. Only a natural end counts — audio_stop clears was_active. */
        /* The worker moves itself on at a track boundary now, so the UI's job
         * is to follow: update what is showing, fetch the new artwork, and
         * hand over the track after that. */
        {
            /* Written first as `while (adv-- > 0) ...; if (adv >= 0)`, which
             * leaves adv at -1 whatever it started as, so the follower was
             * never queued here — only in play_index. Every other boundary
             * therefore fell back to a full restart and gapped. */
            int adv = audio_take_advance();
            if (adv > 0 && audiobook_mode) {
                /* A book rolls over a *file*, which may be several chapters
                 * further on — the chapter within it is then found by
                 * ab_follow() from the position, same as any other crossing. */
                for (int i = 0; i < adv; i++) {
                    int f = ab_book.chap[cur_track].file;
                    int j = cur_track;
                    while (j < ab_book.chap_n && ab_book.chap[j].file == f) j++;
                    if (j >= ab_book.chap_n) break;
                    cur_track = j;
                    snprintf(ab_playing, sizeof(ab_playing), "%s",
                             ab_book.files[ab_book.chap[j].file]);
                    art_request(ab_playing, "", "", "");
                    mlog("[music] rolled into %s\n", ab_book.chap[j].title);
                    dirty = 1;
                }
                audio_set_next(ab_next_file(cur_track));
            } else if (adv > 0) {
                /* R47: steps through next_track_index() rather than a bare
                 * cur_track++, so cur_track ends up wherever queue_follower()
                 * actually told the worker to roll into -- shuffle order or
                 * a repeat-all wrap, not just "the next slot." */
                for (int i = 0; i < adv; i++) {
                    int nxt = next_track_index();
                    if (nxt < 0) break;
                    cur_track = nxt;
                    queue_apply_pending();   /* BG85 -- before art_request() reads q_album */
                    wave_track_changed(queue[cur_track].path);
                    art_request(queue[cur_track].path,
                                queue[cur_track].artist[0] ? queue[cur_track].artist : q_artist,
                                q_album, q_artist);
                    mlog("[music] rolled into %s\n", queue[cur_track].name);
                    dirty = 1;
                }
                queue_follower();
            }
            if (ab_follow()) dirty = 1;
        }

        /* Fallback for the case the worker could not continue — a missing or
         * unreadable file, say. A stream that stops has simply dropped out,
         * and jumping into the music library is not what anyone listening to
         * the radio wants. */
        if (was_active && !audio_is_active()) {
            was_active = 0;
            if (audiobook_mode) {
                /* Only a real file boundary counts. Running off the end of a
                 * chapter inside a file is not the end of anything — the
                 * decoder plays straight on and ab_follow() moves the display
                 * — so the only thing to pick up here is the next file. */
                int f = (cur_track >= 0 && cur_track < ab_book.chap_n)
                      ? ab_book.chap[cur_track].file : -1;
                int j = cur_track;
                while (j < ab_book.chap_n && ab_book.chap[j].file == f) j++;
                if (f >= 0 && j < ab_book.chap_n) {
                    mlog("[music] fell back to restart at chapter %d\n", j + 1);
                    ab_play_chapter(j);
                }
            } else if (!radio_mode && next_track_index() >= 0) {
                /* Reaching here means the worker could not roll on by itself,
                 * which costs a restart and therefore a gap. Logged, because
                 * a gap is otherwise only findable by ear. */
                int nxt = next_track_index();
                mlog("[music] fell back to restart at track %d\n", nxt + 1);
                play_index(nxt);
            } else if (podcast_mode && cur_track >= 0 && cur_track < queue_n) {
                /* An episode is never queued alongside a next one (see
                 * podcast_mode's comment), so reaching the end just stops --
                 * marked finished right away rather than left for the next
                 * periodic save, so reopening it immediately after shows it
                 * as done instead of resuming one tick's worth from the end.
                 * Not pod_save_current_pos(): audio_is_active() is already
                 * false here (that's what this whole block is gated on), and
                 * pod_save_current_pos() deliberately no-ops in that state --
                 * same reason ab_save_current_pos() has to be called before a
                 * stop, not after. */
                pod_resume_store(queue[cur_track].path, POD_FINISHED, 0);
            }
        }
        /* Also opened on a completed drag, not only by following the finger
         * down. A quick flick can be delivered entirely within one poll, and
         * then the live tracking never sees a finger that is still down. */
        if (!qs_open && g == 2 && touch_y < QS_PULL_ZONE &&
            !(vol_ticks > 0 && touch_y >= STATUS_H) && y > QS_PULL) {
            qs_open = 1;
            qs_refresh();
            dirty = 1; idle = 0;
        } else if (vol_ticks > 0 && !qs_open && g == 1 &&
                   y >= STATUS_H && y < STATUS_H + VOL_H) {
            int bw = FB_W - 48;
            int v = (x - 24) * 100 / (bw > 0 ? bw : 1);
            audio_volume_set(v < 0 ? 0 : (v > 100 ? 100 : v));
            vol_ticks = VOL_TICKS;
            dirty = 1; idle = 0;
        } else if (qs_open && (g == 1 || g == 2)) {
            if (qs_dragging) {
                /* The live block already applied the value continuously
                 * while the finger was down (qs_open && qs_dragging &&
                 * touch_down, below); this is just the release. Left to
                 * reach the branches below, a brightness drag that picked
                 * up enough incidental vertical wobble near either end to
                 * classify as g==2 on release would hit "swiped back up"
                 * and silently close the panel, or as g==1 land on
                 * whatever row the release Y happened to fall on — either
                 * way with the *release* position, not where the finger
                 * actually was on the bar. This is BG1 ("brightness slider
                 * overshoot turns screen off"): closing the panel while
                 * mid-drag through an otherwise-unrelated y<0 branch reads
                 * exactly like that from the outside. qs_dragging still
                 * reads true here because it isn't cleared until the live
                 * block runs later this same tick. */
            } else if (g == 2 && y < 0) {            /* swiped back up */
                qs_open = 0;
            } else if (g == 1) {
                int by = qs_bar_y();
                if (x > qs_gear_x() - 16 && y < qs_gear_y() + 36) {
                    /* R51: straight to the full Settings menu, closing the
                     * panel first -- left open behind it, the next "back"
                     * from Settings would have reopened this instead of
                     * returning to wherever the panel was pulled down over. */
                    qs_open = 0;
                    screen = SC_SETTINGS;
                    reset_scroll();
                } else if (y > by - 26 && y < by + 26) {
                    int bw = FB_W - 48;
                    int v = (x - 24) * qs_bright_max / (bw > 0 ? bw : 1);
                    qs_bright = v < 1 ? 1 : (v > qs_bright_max ? qs_bright_max : v);
                    st_brightness_set(qs_bright);
                    saved_brightness = qs_bright;
                    brightness_pref = qs_bright;
                    save_conf();          /* R64 */
                } else if (y > qs_wifi_y() && y < qs_wifi_y() + QS_ROW_H) {
                    qs_wifi = !qs_wifi;
                    st_wifi_set(qs_wifi);
                    wifi_pref = qs_wifi;
                    save_conf();          /* R64 */
                } else if (y > qs_bt_y() && y < qs_bt_y() + QS_ROW_H) {
                    qs_bt = !qs_bt;
                    st_bt_set(qs_bt);
                    bt_pref = qs_bt;
                    save_conf();          /* R64 */
                } else if (y > qs_usb_y() && y < qs_usb_y() + QS_ROW_H) {
                    qs_open = 0;
                    screen = SC_SETTINGS_USB; reset_scroll();
                } else if (y > qs_eq_y() && y < qs_eq_y() + QS_ROW_H) {
                    if (x > FB_W - 100) {
                        eq_set_enabled(!eq_enabled());
                        save_conf();          /* BG38 */
                    } else {
                        /* Closed first, not left open behind the sheet --
                         * qs_open && g==1 is checked ahead of sheet_open in
                         * this chain, so a sheet row tap would otherwise be
                         * swallowed here instead of reaching the sheet. */
                        qs_open = 0;
                        eq_profile_n = ep_scan(eq_profiles, EP_MAX_PROFILES);
                        sheet_open = 3;
                    }
                } else if (y > qs_mseb_y() && y < qs_mseb_y() + QS_ROW_H) {
                    /* No sheet to open for this one -- there's nothing to
                     * pick, just the one fixed set of bands -- so the whole
                     * row toggles, not just the switch end of it. */
                    mseb_on = !mseb_on;
                    eq_set_mseb(mseb_on, mseb_gain);
                    mseb_save(mseb_gain, mseb_on);
                } else if (y > QS_H) {
                    qs_open = 0;                    /* tapped away */
                }
            }
            dirty = 1; idle = 0;
        } else if (g == 3) {
            /* Swipe in from the left edge: the way out of the player, which
             * otherwise only offered the queue. go_back() itself closes show
             * notes first when they're open, rather than leaving the player
             * under them -- see its own comment. */
            if (!go_back()) running = 0;
            dirty = 1; idle = 0;
        } else if (g == 5) {
            /* R80: swipe up from the bottom edge, always home -- a direct
             * jump, not go_back() repeated until it happens to land on
             * SC_MENU. Playback, whatever's playing, is untouched (the mini
             * player is still right there to get back to it); only the
             * screen and whatever modal happened to be open change. */
            sheet_open = 0;
            qs_open = 0;
            screen = SC_MENU; reset_scroll();
            dirty = 1; idle = 0;
        } else if (index_visible() && touch_x >= FB_W - INDEX_TOUCH_W &&
                   touch_y >= CONTENT_Y && touch_y < index_bottom()) {
            /* Anything that began on the strip ends on the strip. Sliding
             * left and lifting used to arrive here as a tap at the release
             * point, which is over a list row, and opened it.
             *
             * touch_y, not live_y, is the bug BG3/BG4 turned out to be:
             * touch_y is frozen at wherever the finger first touched down
             * and never updates during the drag, so this re-jump undid the
             * entire live-tracked drag on release — dragging from M up to J
             * looked right the whole time and then snapped back to M, and a
             * subdivision selected mid-drag reverted the instant the finger
             * lifted, which is what "the second level is gone" actually was. */
            if (index_shown >= 0) index_jump(live_y);
        } else if (g == 2 && index_visible() && touch_x >= FB_W - INDEX_TOUCH_W) {
            /* Already followed live; nothing to do on release. */
        } else if (g == 2) {
            /* Already followed live below, one row at a time, with inertia
             * picking up from here on release. This used to jump by however
             * many whole rows the release distance divided into — four at a
             * time on an ordinary swipe, and nothing moved until the finger
             * lifted. */
        } else if (g == 1 && sheet_open) {
            int top = sheet_top();
            int i = (y >= top + SHEET_HEAD) ? (y - top - SHEET_HEAD) / SHEET_ROW : -1;
            if (sheet_open == 2) {
                if (i == playlist_n) {
                    /* R71: create-and-add in one step, rather than needing
                     * a separate trip to SC_PLAYLISTS first. sheet_open is
                     * cleared here (kb_open() only ever changes `screen`),
                     * so the sheet doesn't also draw underneath the keyboard. */
                    kb_new_playlist_add_track = sheet_track;
                    sheet_open = 0;
                    kb_open("Playlist name", KB_PURPOSE_NEW_PLAYLIST_NAME, "");
                } else {
                    if (i >= 0 && i < playlist_n && sheet_track < track_n) {
                        int rc = pl_append(playlists[i].path, tracks[sheet_track].path);
                        snprintf(sheet_note, sizeof(sheet_note), "%s %s",
                                rc == 1 ? "Added to" : "Already in", playlists[i].name);
                    }
                    sheet_open = 0;
                }
            } else if (sheet_open == 3) {
                /* No save-on-the-way-out here either, for the same reason as
                 * go_back()'s SC_EQ_BAND case: switching profiles without
                 * ever touching a slider must not fork the outgoing one. */
                if (i >= 0 && i < eq_profile_n) {
                    eq_switch_to(eq_profiles[i].path);
                    save_conf();          /* BG38 */
                }
                sheet_open = 0;
            } else if (sheet_open == 4) {
                /* R74: Rename/Delete/Cancel. Rename hands off to the T9
                 * keyboard pre-filled with the current name, same "clear
                 * sheet_open before kb_open()" shape R71's own create-and-add
                 * row already uses. Delete is a separate confirm step
                 * (sheet_open = 5), not immediate -- unlike a single track's
                 * swipe-to-remove (R70), this deletes a whole named,
                 * deliberately-curated list in one action, not one row a
                 * fresh listen could accidentally repeat. */
                if (i == 0 && sheet_playlist >= 0 && sheet_playlist < playlist_n) {
                    snprintf(kb_rename_playlist_path, sizeof(kb_rename_playlist_path),
                            "%s", playlists[sheet_playlist].path);
                    sheet_open = 0;
                    kb_open("Playlist name", KB_PURPOSE_RENAME_PLAYLIST,
                           playlists[sheet_playlist].name);
                } else if (i == 1 && sheet_playlist >= 0 && sheet_playlist < playlist_n) {
                    sheet_open = 5;
                } else {
                    sheet_open = 0;
                }
            } else if (sheet_open == 5) {
                if (i == 0 && sheet_playlist >= 0 && sheet_playlist < playlist_n) {
                    pl_delete(playlists[sheet_playlist].path);
                    playlist_n = pl_list(playlists, PL_MAX);
                }
                sheet_open = 0;
            } else if (i == 0) {
                /* R58: an undownloaded podcast episode has an empty path
                 * (pod_rebuild_tracks()'s own comment) -- queueing one is a
                 * silent dead end later rather than a crash now, but still
                 * worth catching here with the same message pod_play_
                 * episode()'s tap-to-play already gives for the same case,
                 * rather than pretending it queued. */
                if (sheet_track >= 0 && sheet_track < track_n && !tracks[sheet_track].path[0]) {
                    snprintf(sheet_note, sizeof(sheet_note), "Not downloaded yet");
                } else if (queue_kind_conflict()) {
                    /* See queue_kind_conflict()'s own comment. Named for
                     * what's already playing rather than a generic refusal,
                     * so it's obvious *why* -- and it stays true whichever
                     * direction the mix was attempted from. */
                    snprintf(sheet_note, sizeof(sheet_note), "%s is playing",
                             audiobook_mode ? "An audiobook"
                                            : podcast_mode ? "A podcast" : "Music");
                } else {
                    queue_play_next(sheet_track);   /* BG73 */
                    snprintf(sheet_note, sizeof(sheet_note), "Playing next");
                }
                sheet_open = 0;
            } else if (i == 1) {
                if (sheet_track >= 0 && sheet_track < track_n && !tracks[sheet_track].path[0]) {
                    snprintf(sheet_note, sizeof(sheet_note), "Not downloaded yet");
                } else if (queue_kind_conflict()) {
                    snprintf(sheet_note, sizeof(sheet_note), "%s is playing",
                             audiobook_mode ? "An audiobook"
                                            : podcast_mode ? "A podcast" : "Music");
                } else {
                    queue_insert(sheet_track, -1);
                    snprintf(sheet_note, sizeof(sheet_note), "Added to queue");
                }
                sheet_open = 0;
            } else if (i == 2) {
                playlist_n = pl_list(playlists, PL_MAX);
                sheet_open = 2;                 /* second level: which playlist */
            } else {
                sheet_open = 0;
            }
            dirty = 1; idle = 0;
        } else if (g == 1 && list_settled_ticks < LIST_TAP_SETTLE_TICKS) {
            /* A tap that lands while the list is still coasting from a
             * fling, or in the brief window right after, stops the scroll
             * and does nothing else -- selecting whatever row happened to
             * be under the finger was wrong far more often than right.
             * list_settled_ticks (see its own comment) is keyed on
             * inertia_active alone, not list_dragging -- an earlier attempt
             * that also reset on list_dragging broke every ordinary tap,
             * since list_dragging goes true on any touch-down at all, tap
             * or drag alike. */
            inertia_active = 0;
            list_velocity = 0;
            dirty = 1;
        } else if (g == 1) {
            sheet_note[0] = '\0';
            if (screen == SC_PLAYING && audiobook_mode && y >= STATUS_H) {
                /* Same corner, same meaning as the regular player: the
                 * queue is chapters here, reached the same way. */
                if (y > FB_H - 56 && x > FB_W - 76) { go_back(); }
                else {
                    /* Book bar: display-only, per explicit correction --
                     * no hit test here at all, on purpose. */
                    int cby = ab_chapter_bar_y();
                    if (y > cby - 26 && y < cby + 26) {
                        const ab_chapter_t *ch =
                            (cur_track >= 0 && cur_track < ab_book.chap_n)
                                ? &ab_book.chap[cur_track] : NULL;
                        int dur = ch ? (int)ch->dur_ms : 0;
                        if (dur <= 0) dur = audio_dur_ms();
                        int px = x - 24, span = FB_W - 48;
                        if (px < 0) px = 0;
                        if (px > span) px = span;
                        /* Scrubbing is within the chapter, so the target is
                         * measured from where the chapter starts in the file
                         * rather than from the start of an eleven-hour file. */
                        if (dur > 0)
                            audio_seek_ms((int)((ch ? ch->file_start_ms : 0) +
                                                (int64_t)dur * px / span));
                    }

                    int cyy = cby + 58 + CTRL_NUDGE_PX;
                    int mid = FB_W / 2;
                    /* Must match the skip rings' `off` in the draw code above
                     * (currently 96) -- this was left at the ring's old
                     * offset (128) after the draw side was pulled in, so the
                     * hit zone sat 32px away from where the speed ring is
                     * actually drawn. */
                    int scx = mid - 96 - 70;
                    if (y > cyy - 48 && y < cyy + 48) {
                        if (x > scx - 30 && x < scx + 30) {
                            ab_speed_permille += 100;
                            if (ab_speed_permille > 2000) ab_speed_permille = 1000;
                            audio_set_speed(ab_speed_permille);
                            save_conf();          /* BG91 */
                        } else if (x < FB_W / 3) {
                            audio_seek_ms(audio_pos_ms() - 10000);
                        } else if (x > 2 * FB_W / 3) {
                            audio_seek_ms(audio_pos_ms() + 10000);
                        } else {
                            audio_toggle();
                        }
                    }
                }
            } else if (screen == SC_PLAYING && radio_mode && y >= STATUS_H) {
                int cyy = 120 + 190;
                if (y > cyy - 48 && y < cyy + 48) audio_toggle();
            } else if (screen == SC_PLAYING && y >= STATUS_H) {
                /* R93: the queue control used to sit in the corner the
                 * header once owned, hit-tested separately from everything
                 * else in this block -- now it's drawn inline in the
                 * transport row itself (mirroring the mode button), so its
                 * tap zone moved into that same row's own x-dispatch below
                 * instead of a standalone corner check. */
                {
                /* The bar is only 6 px tall; the target has to be the band
                 * around it or it is unhittable with a finger. */
                int bary = bar_y();
                if (y > bary - 26 && y < bary + 26) {
                    int dur = audio_dur_ms();
                    if (dur <= 0 && cur_track < queue_n) dur = queue[cur_track].dur_ms;
                    int px = x - 24, span = FB_W - 48;
                    if (px < 0) px = 0;
                    if (px > span) px = span;
                    if (dur > 0) audio_seek_ms((int)((int64_t)dur * px / span));
                }

                int cyy = bary + 70 + CTRL_NUDGE_PX;      /* BG40: matches the draw-side offset (R83) */
                /* R93: podcast_mode's row has no spare slot for the queue
                 * circle music/the mode button get (see the draw side's own
                 * comment) -- it keeps the plain corner icon, so this is the
                 * one case still hit-tested as a standalone corner rather
                 * than a zone inside the transport row below.
                 *
                 * BG71: plain music/podcasts get their own queue view --
                 * the old go_back()-into-SC_TRACKS reuse showed the
                 * *album* (a rich cover/bio page, R46) rather than the
                 * actual upcoming play order, which silently diverges from
                 * it the moment shuffle is on. R58: an episode is never
                 * queued alongside a next one on its own, but the episode
                 * list's own long-press sheet can still queue_play_next()/
                 * queue_insert() another one, so this is never empty of
                 * anything worth seeing even for a podcast. */
                if (podcast_mode && y > FB_H - 56 && x > FB_W - 76) {
                    screen = SC_QUEUE; reset_scroll();
                    queue_via_back = 0;   /* reached deliberately -- back returns to Now Playing */
                } else if (y > cyy - 48 && y < cyy + 48) {
                    /* BG47 (revised): real hit zones under the drawn arcs/
                     * ring, not blind thirds -- boundaries are the
                     * midpoints between adjacent element centres, same
                     * POD_SKIP_OFF/pod_speed_x() the draw side uses so the
                     * two cannot drift apart. Just two skip zones now
                     * (-10s, +30s), not the original four. A podcast
                     * episode is never queued alongside a next one (see
                     * podcast_mode's comment), so this replaces prev/next
                     * track entirely rather than sharing the zone with it. */
                    if (podcast_mode) {
                        int mid = FB_W / 2;
                        int x10 = mid - POD_SKIP_OFF, xp30 = mid + POD_SKIP_OFF;
                        int xspd = pod_speed_x(mid);
                        int xicon = pod_info_x(mid);
                        if (x < (xspd + x10) / 2) {
                            pod_speed_permille += 100;
                            if (pod_speed_permille > 2000) pod_speed_permille = 1000;
                            audio_set_speed(pod_speed_permille);
                            pod_speed_store(cur_feed, pod_speed_permille);   /* R99 */
                            save_conf();          /* BG91 */
                        } else if (x < (x10 + mid) / 2) {
                            audio_seek_ms(audio_pos_ms() - 10000);
                        } else if (x < (mid + xp30) / 2) {
                            audio_toggle();
                        } else if (!pod_notes_avail || x < (xp30 + xicon) / 2) {
                            audio_seek_ms(audio_pos_ms() + 30000);
                        } else {
                            pod_notes_showing = !pod_notes_showing;
                            pod_notes_scroll_px = 0;
                        }
                    } else {
                        /* Real hit zones under the drawn icons -- midpoints
                         * between adjacent element centres, same idiom BG47
                         * already applied to podcast_mode above -- not blind
                         * thirds, which stopped matching once the icons
                         * moved in to offset 96. */
                        int mid2 = FB_W / 2;
                        int pmx = mid2 - 96 - 82, prevx = mid2 - 96, nextx = mid2 + 96;
                        int queuex = mid2 + 96 + 82;   /* R93: mirrors pmx */
                        if (x < (pmx + prevx) / 2) {
                            /* R47: Normal -> Shuffle -> Repeat -> Normal.
                             * Repeat means the current track, not the whole
                             * queue -- looping one song is the thing a
                             * dedicated button is actually for; looping a
                             * whole album barely differs from just letting
                             * it play. */
                            if (shuffle_enabled) {
                                shuffle_enabled = 0; repeat_mode = REPEAT_ONE;
                            } else if (repeat_mode == REPEAT_ONE) {
                                repeat_mode = REPEAT_OFF;
                            } else {
                                shuffle_enabled = 1;
                            }
                            if (shuffle_enabled) shuffle_regenerate();
                            queue_follower();
                            save_conf();
                        } else if (x < (prevx + mid2) / 2) {
                            play_index(prev_track_index());
                        } else if (x < (mid2 + nextx) / 2) {
                            audio_toggle();
                        } else if (x < (nextx + queuex) / 2) {
                            play_index(next_track_index());
                        } else {
                            screen = SC_QUEUE; reset_scroll();
                            queue_via_back = 0;   /* reached deliberately -- back returns to Now Playing */
                        }
                    }
                }
                }
            } else if (y < CONTENT_Y && screen != SC_KEYBOARD) {
                /* BG108: SC_KEYBOARD draws its own header (Cancel top-left,
                 * Done top-right, both at y=20 -- see draw_keyboard()) rather
                 * than the generic title+back-arrow bar every other screen
                 * has, but nothing here excluded it from this generic zone,
                 * whose own back check just below fired on every screen
                 * except SC_MENU. At the time this was fixed, back was still
                 * a top-right "BACK" label (R72 later moved it to a leading
                 * arrow next to the title) and Done sat at that exact same
                 * spot, so tapping it landed here first and called go_back()
                 * -- Cancel's behaviour, not Done's -- before SC_KEYBOARD's
                 * own dedicated handler further down ever got a turn. This
                 * exclusion stays correct after R72's move regardless: Cancel
                 * is top-left, which is exactly where the arrow's own tap
                 * zone lives now, so SC_KEYBOARD still needs to stay out of
                 * this whole block or Cancel would collide with *that*
                 * instead. Reported live: "'done' is in the same spot as
                 * 'back'... I definitely, 100% hit 'done'" -- true on both
                 * counts, the input landed correctly, it just resolved to
                 * the wrong action, not the "user error" it initially looked
                 * like against BG107's own build (which fixed go_back()
                 * itself to no longer make this specific case dump to Albums,
                 * but didn't stop go_back() from being reached at all here).
                 * mseb_reset_x()/pod_sync_x()'s own on-screen position sits
                 * well left of the old fixed "FB_W - 120" boundary these
                 * zones used to end at, which left a dead strip between the
                 * end of "Reset"/"Sync" and where that boundary began --
                 * landing there fell through to the BACK-zone check below
                 * and quietly left the screen instead of hitting the button.
                 * header_back_x() - 16 is precise: it reaches right up to
                 * where BACK's own zone actually starts. */
                if (screen == SC_MSEB && x >= mseb_reset_x() - 16 && x < header_back_x() - 16) {
                    for (int i = 0; i < MSEB_BAND_N; i++) mseb_gain[i] = 0.0f;
                    eq_set_mseb(mseb_on, mseb_gain);
                    mseb_save(mseb_gain, mseb_on);
                } else if (screen == SC_PODCASTS && x >= pod_sync_x() - 16 && x < header_back_x() - 16) {
                    if (!pod_update_running()) { pod_update_start(); pod_sync_log_n = 0; }
                    screen = SC_POD_SYNC; reset_scroll();
                } else if (screen == SC_QUEUE && x >= queue_clear_x() - 16 && x < header_back_x() - 16) {
                    /* R52: drop everything queued after the currently-
                     * playing track -- that one keeps playing undisturbed
                     * to its own end, same as reaching it naturally would,
                     * it just has nothing left queued up behind it once it
                     * does. No confirmation step: unlike deleting a
                     * playlist or a file, nothing here is lost for good --
                     * queuing more is one "Play next"/"Add to queue" away. */
                    if (cur_track >= 0 && cur_track + 1 < queue_n) {
                        queue_n = cur_track + 1;
                        /* Reported live: swiping back afterward showed a
                         * mangled "album" page -- the real album, but
                         * missing every track Clear had just dropped. Same
                         * root cause as the Add-to-Queue/Play-Next case
                         * above: once queue[] no longer matches what a
                         * fresh browse of q_album would produce, it can't
                         * be presented as that album's own page. Playlists
                         * excluded for the same reason queue_insert()
                         * excludes them. */
                        if (!q_is_playlist) queue_mixed = 1;
                        queue_follower();
                    }
                } else if (g_header_show_back && x < BACK_ARROW_TAP_W) go_back();
            } else if (index_visible() && x >= FB_W - INDEX_TOUCH_W && y >= CONTENT_Y &&
                       y < index_bottom()) {
                index_jump(y);
            } else if (mini_visible() && y >= FB_H - MINI_H) {
                /* Zone boundaries, not per-button hit tests -- generous on
                 * purpose, same reasoning as the full audiobook player's own
                 * thirds-based skip zones, and it sidesteps BG21 entirely:
                 * there's no separate copy of a button's position to drift
                 * out of sync with, only where the NEXT one over begins. */
                if (radio_mode) {
                    if (x > FB_W - 92) audio_toggle();
                    else               screen = SC_PLAYING;
                } else if (audiobook_mode) {
                    if (x > MINI_ZONE_SIDE)      audio_seek_ms(audio_pos_ms() + 10000);
                    else if (x > MINI_ZONE_PLAY) audio_toggle();
                    else if (x > MINI_ZONE_BACK) audio_seek_ms(audio_pos_ms() - 10000);
                    else                          screen = SC_PLAYING;
                } else if (podcast_mode) {
                    if (x > MINI_ZONE_SIDE)      audio_seek_ms(audio_pos_ms() + 30000);
                    else if (x > MINI_ZONE_PLAY) audio_toggle();
                    else if (x > MINI_ZONE_BACK) audio_seek_ms(audio_pos_ms() - 30000);
                    else                          screen = SC_PLAYING;
                } else {
                    if (x > MINI_ZONE_SIDE)      play_index(next_track_index());
                    else if (x > MINI_ZONE_PLAY) audio_toggle();
                    else { screen = SC_PLAYING; played_from_browse = 0; }
                }
            } else if (screen == SC_EQ) {
                int ry_enabled = eq_row_enabled_y();
                int ry_profile = eq_row_profile_y();
                int ry_bands   = eq_row_bands_y();
                if (y >= ry_enabled && y < ry_enabled + ROW_H) {
                    eq_set_enabled(!eq_enabled());
                    save_conf();          /* BG38 */
                } else if (y >= ry_profile && y < ry_profile + ROW_H) {
                    eq_profile_n = ep_scan(eq_profiles, EP_MAX_PROFILES);
                    sheet_open = 3;
                } else if (y >= ry_bands && y < ry_bands + ROW_H) {
                    screen = SC_EQ_BANDS; reset_scroll();
                    total = eq_cur.band_n;
                } else {
                    int pby = eq_preamp_y() + 26;
                    if (y > pby - 20 && y < pby + 20) {
                        int span = FB_W - 48, px = x - 24;
                        if (px < 0) px = 0; if (px > span) px = span;
                        eq_cur.preamp_db = -12.0f + 18.0f * (float)px / (float)span;
                        eq_set_preamp(eq_cur.preamp_db);
                        eq_save_current();
                    }
                }
            } else if (screen == SC_MSEB) {
                int ry_enabled = mseb_row_enabled_y();
                if (y >= ry_enabled && y < ry_enabled + ROW_H) {
                    mseb_on = !mseb_on;
                    eq_set_mseb(mseb_on, mseb_gain);
                    mseb_save(mseb_gain, mseb_on);
                } else {
                    int span = FB_W - 48;
                    for (int i = 0; i < MSEB_BAND_N; i++) {
                        int sy = mseb_band_row_y(i) + 46;
                        if (y <= sy - 20 || y >= sy + 20) continue;
                        int px = x - 24; if (px < 0) px = 0; if (px > span) px = span;
                        mseb_gain[i] = -12.0f + 24.0f * (float)px / (float)span;
                        eq_set_mseb(mseb_on, mseb_gain);
                        mseb_save(mseb_gain, mseb_on);
                        break;
                    }
                }
            } else if (screen == SC_EQ_BAND) {
                eq_band_t *b = (eq_editing_band >= 0 && eq_editing_band < eq_cur.band_n)
                             ? &eq_cur.band[eq_editing_band] : NULL;
                if (b) {
                    int ey = eq_enabled_y();
                    int ty = eq_type_y();
                    int fy = eq_freq_y(), gy = eq_gain_y(), qy = eq_q_y();
                    int span = FB_W - 48;
                    if (y > ey - 12 && y < ey + 20) {
                        b->on = !b->on;
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    } else if (y >= ty && y < ty + 34) {
                        int segw = span / 3;
                        int s = (x - 24) / (segw > 0 ? segw : 1);
                        if (s < 0) s = 0; if (s > 2) s = 2;
                        b->type = (eq_type_t)s;
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    } else if (y > fy - 20 && y < fy + 20) {
                        int px = x - 24; if (px < 0) px = 0; if (px > span) px = span;
                        b->fc = eq_freq_from_t((float)px / (float)span);
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    } else if (y > gy - 20 && y < gy + 20) {
                        int px = x - 24; if (px < 0) px = 0; if (px > span) px = span;
                        b->gain_db = -12.0f + 24.0f * (float)px / (float)span;
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    } else if (y > qy - 20 && y < qy + 20) {
                        int px = x - 24; if (px < 0) px = 0; if (px > span) px = span;
                        b->q = 0.1f + 9.9f * (float)px / (float)span;
                        eq_set_band(eq_editing_band, b);
                        eq_save_current();
                    }
                }
            } else if (screen == SC_KEYBOARD) {
                /* Cancel/Done share the same y-band as the title row (see
                 * draw_keyboard()'s own 20/56 y-values) -- split left/right
                 * at the field's own horizontal midpoint rather than a tight
                 * box around each word, so neither is harder to hit than it
                 * looks. */
                if (y < 60) {
                    if (x < FB_W / 2) screen = kb_return_screen;   /* Cancel */
                    else kb_commit();                              /* Done */
                } else if (y >= 100 && y < 170) {
                    kb_set_cursor_from_x(x);   /* tap in the field to place the caret */
                } else if (kb_mode == KB_MODE_LETTERS && y >= KB_SHIFT_Y && y < KB_SHIFT_Y + KB_SHIFT_H &&
                           x >= KB_SHIFT_X && x < KB_SHIFT_X + KB_SHIFT_W) {
                    /* R73: off -> shift-once -> caps-lock -> off, the same
                     * cycle a real T9 phone's own shift/"*" key already
                     * uses. Inert in numbers/symbols mode -- see the button's
                     * own dimmed drawing just above. */
                    kb_shift = (kb_shift + 1) % 3;
                } else if (y >= KB_GRID_Y && y < KB_GRID_Y + 4 * KB_KEY_H) {
                    int row = (y - KB_GRID_Y) / KB_KEY_H;
                    int col = x / KB_KEY_W;
                    if (row >= 0 && row < 4 && col >= 0 && col < 3) {
                        int key = KB_GRID[row][col];
                        if (key == -1) {
                            kb_mode = (kb_mode + 1) % KB_MODE_N;
                            kb_last_key = -1;
                        }
                        else if (key == -2) kb_backspace();
                        else kb_apply_key(key);
                    }
                }
            } else if (screen == SC_SETTINGS) {
                /* Tap coordinates are screen-space; row getters are
                 * content-space. Same `off` the draw code subtracts, so a
                 * tap lands on whatever row is actually drawn under the
                 * finger regardless of how far Settings has been scrolled. */
                int off = scroll * ROW_H + scroll_px;
                int ry_lock = set_row_lock_y() - off, ry_theme = set_row_theme_y() - off;
                int ry_coverpalette = set_row_coverpalette_y() - off;
                int coverpalette_h = set_row_about_y() - set_row_coverpalette_y();
                int ry_usbbypass = set_row_usbbypass_y() - off;
                int ry_autooff = set_row_autooff_y() - off, ry_about = set_row_about_y() - off;
                int ry_btautoplay = set_row_btautoplay_y() - off;
                int ry_btvolsteps = set_row_btvolsteps_y() - off;
                int ry_lighttheme = set_row_lighttheme_y() - off;
                int ry_timezone = set_row_timezone_y() - off;
                int ry_wifi = set_row_wifi_y() - off, ry_bt = set_row_bt_y() - off;
                int ry_usb = set_row_usb_y() - off;
                int ry_scan = set_row_scan_y() - off;
                /* RBR: these first three rows are taller than ROW_H -- each
                 * carries a two-line description underneath the label (see
                 * the draw side's own lock_h/usbbypass_h/autooff_h), and
                 * their toggle switches/values are centred within that
                 * *taller* block, not within a plain ROW_H slot. Hit-testing
                 * against plain ROW_H here left the switch itself (drawn
                 * lower, since there's more row to center within) outside
                 * its own tap zone -- reported live as "tapping the toggle
                 * is ignored, only the label works" (the label sits higher,
                 * at ry+20, still inside the shorter ROW_H-based zone). */
                int lock_h = set_row_usbbypass_y() - set_row_lock_y();
                int usbbypass_h = set_row_btautoplay_y() - set_row_usbbypass_y();
                int btautoplay_h = set_row_btvolsteps_y() - set_row_btautoplay_y();
                int autooff_h = set_row_lighttheme_y() - set_row_autooff_y();
                if (y >= ry_lock && y < ry_lock + lock_h) {
                    button_lock_enabled = !button_lock_enabled;
                    save_conf();
                } else if (y >= ry_usbbypass && y < ry_usbbypass + usbbypass_h) {
                    usb_bypass_enabled = !usb_bypass_enabled;
                    save_conf();
                } else if (y >= ry_btautoplay && y < ry_btautoplay + btautoplay_h) {
                    bt_autoplay_enabled = !bt_autoplay_enabled;
                    save_conf();
                } else if (y >= ry_btvolsteps && y < ry_btvolsteps + ROW_H) {
                    bt_vol_step_idx = (bt_vol_step_idx + 1) % BT_VOL_STEP_CHOICE_N;
                    audio_set_bt_vol_steps(BT_VOL_STEP_CHOICES[bt_vol_step_idx]);
                    save_conf();
                } else if (y >= ry_autooff && y < ry_autooff + autooff_h) {
                    /* Cycles rather than opening a picker: six short values,
                     * and a whole screen for them would be more chrome than
                     * the choice is worth. */
                    auto_off_idx = (auto_off_idx + 1) % AUTO_OFF_CHOICE_N;
                    save_conf();
                } else if (y >= ry_lighttheme && y < ry_lighttheme + ROW_H) {
                    screen = SC_SETTINGS_THEMEMODE; reset_scroll();
                } else if (y >= ry_timezone && y < ry_timezone + ROW_H) {
                    screen = SC_SETTINGS_TIMEZONE; reset_scroll();
                } else if (y >= ry_theme && y < ry_theme + ROW_H) {
                    screen = SC_SETTINGS_THEME; reset_scroll();
                } else if (y >= ry_coverpalette && y < ry_coverpalette + coverpalette_h) {
                    cover_palette_enabled = !cover_palette_enabled;
                    np_palette_seq = -1;   /* force a recompute on next draw */
                    /* R94: a scan that ran while this was off never
                     * prewarmed anything -- catch up right when it's
                     * switched on rather than waiting for the next
                     * scan to happen to run again. */
                    if (cover_palette_enabled) cover_prewarm_start();
                    save_conf();
                } else if (y >= ry_about && y < ry_about + ROW_H) {
                    screen = SC_SETTINGS_ABOUT; reset_scroll();
                } else if (y >= ry_wifi && y < ry_wifi + ROW_H) {
                    screen = SC_SETTINGS_WIFI; reset_scroll();
                } else if (y >= ry_bt && y < ry_bt + ROW_H) {
                    screen = SC_SETTINGS_BT; reset_scroll();
                } else if (y >= ry_usb && y < ry_usb + ROW_H) {
                    screen = SC_SETTINGS_USB; reset_scroll();
                } else if (y >= ry_scan && y < ry_scan + ROW_H &&
                           st_usb_mode() != 1) {
                    scanner_rescan_now();
                    dirty = 1;
                }
            } else if (screen == SC_SETTINGS_WIFI) {
                /* R75: scroll-aware row math, same off/content_y shape
                 * SC_SETTINGS' own longer list uses -- this screen scrolls
                 * now, so a plain (y - CONTENT_Y) / ROW_H would resolve to
                 * whatever row happened to be at that screen position
                 * *before* scrolling, not the row actually drawn under the
                 * finger. */
                int off = scroll * ROW_H + scroll_px;
                int content_y = y + off;
                int row = (content_y - CONTENT_Y) / ROW_H;
                if (row == 0) {
                    int on = !st_wifi_on();
                    st_wifi_set(on);
                    wifi_pref = on;
                    save_conf();          /* R64 */
                } else if (row == 2) {
                    wifi_scan_start();
                } else if (row == 3) {
                    kb_open("Network name (SSID)", KB_PURPOSE_WIFI_SSID_MANUAL, "");
                } else if (row >= 4) {
                    /* Re-queried fresh, same reasoning bt_pair()'s own tap
                     * handler gives for doing the same -- a tap is rare and
                     * deliberate, not a redraw-rate concern. No separate
                     * visibility cap needed any more (R75) -- a row that's
                     * scrolled off-screen simply has no on-screen y for a
                     * touch to land on in the first place. */
                    wifi_found_net_t nets[10];
                    int n = wifi_scan_results(nets, 10);
                    int idx = row - 4;
                    if (idx >= 0 && idx < n) {
                        if (nets[idx].open) {
                            wifi_connect(nets[idx].ssid, "");
                        } else {
                            snprintf(kb_wifi_target_ssid, sizeof(kb_wifi_target_ssid),
                                    "%s", nets[idx].ssid);
                            kb_open("Password for this network", KB_PURPOSE_WIFI_PASSWORD, "");
                        }
                    }
                }
            } else if (screen == SC_SETTINGS_BT) {
                /* BG109: scroll-aware row math, same shape as
                 * SC_SETTINGS_WIFI's own tap handler just above. */
                int off = scroll * ROW_H + scroll_px;
                int content_y = y + off;
                int row = (content_y - CONTENT_Y) / ROW_H;
                if (row == 0) {
                    int on = !st_bt_on();
                    st_bt_set(on);
                    bt_pref = on;
                    save_conf();          /* R64 */
                } else if (row == 2) {
                    bt_scan_start();
                } else if (row >= 3) {
                    /* Re-queried fresh, same reasoning R75's own wifi tap
                     * handler gives for doing the same -- a tap is rare and
                     * deliberate, not a redraw-rate concern. No separate
                     * visibility cap needed any more -- a row that's
                     * scrolled off-screen simply has no on-screen y for a
                     * touch to land on in the first place. */
                    bt_found_dev_t devs[8];
                    int n = bt_scan_devices(devs, 8);
                    int idx = row - 3;
                    if (idx >= 0 && idx < n)
                        bt_pair(devs[idx].mac);
                }
            } else if (screen == SC_SETTINGS_THEME) {
                int idx = (y - CONTENT_Y) / ROW_H;
                if (idx >= 0 && idx < ACCENT_N) {
                    g_accent_idx = idx;
                    g_accent = ACCENT_PRESETS[idx].color;
                    save_conf();
                }
            } else if (screen == SC_SETTINGS_TIMEZONE) {
                int off = scroll * ROW_H + scroll_px;
                int idx = (y - CONTENT_Y + off) / ROW_H;
                if (idx >= 0 && idx < TZ_N) {
                    tz_idx = idx;
                    save_conf();
                }
            } else if (screen == SC_SETTINGS_THEMEMODE) {
                int idx = (y - CONTENT_Y) / ROW_H;
                if (idx >= 0 && idx < THEME_MODE_N) {
                    theme_mode = idx;
                    apply_theme();
                    save_conf();
                }
            } else if (screen == SC_SETTINGS_USB) {
                int idx = (y - CONTENT_Y) / ROW_H;
                if (idx >= 0 && idx < 2) set_usb_storage_mode(idx);
            } else if (screen == SC_TRACKS && !ab_list && !pod_list) {
                /* R46: tap coordinates are screen-space; the header/track
                 * layout is content-space (see the draw side's own `off`).
                 * A tap on the header itself (cover/title/info) still does
                 * nothing -- but the artist line is now a real link to that
                 * artist's own page, the same tap zone the draw side uses
                 * for it (tracks_hdr_artist_y() to tracks_hdr_info_y()). */
                int off = scroll * ROW_H + scroll_px;
                int header_h = tracks_hdr_h();
                int content_y = y + off;
                if (cur_artist[0] && content_y >= tracks_hdr_artist_y() &&
                    content_y < tracks_hdr_info_y()) {
                    snprintf(artist_page_back_album, sizeof(artist_page_back_album), "%s", cur_album);
                    snprintf(artist_page_back_artist, sizeof(artist_page_back_artist), "%s", cur_artist);
                    snprintf(artist_page_name, sizeof(artist_page_name), "%s", cur_artist);
                    artist_page_from_list = 0;   /* back -> this album */
                    artist_page_album_n = lib_albums("album_artist", artist_page_name,
                                                     artist_page_albums, ARTIST_ALBUMS_MAX, 0);
                    screen = SC_ARTIST_PAGE;
                    reset_scroll();
                    artist_art_request(artist_page_name);
                } else if (content_y >= header_h) {
                    /* track_index_at(), not a plain (content_y - header_h) /
                     * ROW_H division -- disc banners mean a row's position
                     * no longer maps to its index that simply, and the two
                     * have to agree exactly or a tap lands on the wrong
                     * track (the same class of bug BG2/BG61 already fixed
                     * once for scroll_px). Also correctly resolves to
                     * nothing (-1) for a tap that lands on a banner itself. */
                    int idx = track_index_at(content_y);
                    /* R70: playlist_drag_active/playlist_swipe_active are
                     * still true here on the exact tick a drag or swipe
                     * ends -- same reasoning as SC_QUEUE's own guard (see
                     * its comment) -- so this excludes a drag/swipe release
                     * from also registering as tap-to-play. */
                    if (idx >= 0 && idx < track_n &&
                        !playlist_drag_active && !playlist_swipe_active) {
                        audio_set_speed(1000);
                        screen = SC_PLAYING;
                        played_from_browse = 1;
                        play_from_list(idx);
                    }
                }
            } else if (screen == SC_ARTIST_PAGE) {
                /* Same off/header_h shape as the album page's own tap
                 * handler just above -- a plain-album screen with a
                 * different row type (albums instead of tracks) and a
                 * different destination (another album page instead of
                 * playback). */
                int off = scroll * ROW_H + scroll_px;
                int header_h = artist_page_hdr_h();
                int content_y = y + off;
                if (content_y >= header_h) {
                    int idx = (content_y - header_h) / ROW_H;
                    if (idx >= 0 && idx < artist_page_album_n) {
                        lib_row_t *r = &artist_page_albums[idx];
                        snprintf(cur_album, sizeof(cur_album), "%s", r->name);
                        snprintf(cur_artist, sizeof(cur_artist), "%s", r->owner);
                        browsing_is_playlist = 0;   /* BG73 */
                        tracks_from_artist_page = 1;
                        screen = SC_TRACKS; reset_scroll();
                        ab_list = 0;
                        pod_list = 0;
                        track_n = lib_tracks_for_album(cur_artist, cur_album,
                                                       tracks, (int)(sizeof(tracks)/sizeof(tracks[0])),
                                                       r->count);
                        if (track_n > 0)
                            view_art_request(tracks[0].path, cur_artist, cur_album);
                    }
                }
            } else {
                /* No +scroll_px: BG2 made rows page instead of slide, so a
                 * row's drawn position never reflects scroll_px, only
                 * `scroll` (see draw_screen()'s own comment on this). Adding
                 * it back here reintroduced BG2's bug one level up — with a
                 * fractional drag still pending, this idx pointed one row
                 * past whatever was actually drawn under the finger, so a
                 * tap opened the row below the one that was visibly tapped. */
                /* R50: the Queue screen's summary line pushes its rows down
                 * by QUEUE_SUMMARY_H -- without this, every tap on it would
                 * resolve one row short of what's actually under the finger,
                 * same class of bug BG2/BG61 already fixed for scroll_px. */
                int idx = (y - CONTENT_Y - (screen == SC_QUEUE ? QUEUE_SUMMARY_H : 0)) / ROW_H;
                if (screen == SC_QUEUE && y < CONTENT_Y + QUEUE_SUMMARY_H) idx = -1;
                /* R76: `idx` above stays exactly as it was -- SC_MENU/
                 * SC_MUSIC_MENU/SC_TRACKS's own ab_list/pod_list flat path
                 * still draw the old paged way and still need it unchanged.
                 * smooth_row is the same formula solved with `off` folded
                 * in, for the screens converted to off-based smooth-scroll
                 * drawing (SC_ARTISTS/SC_ALBUMS/SC_PLAYLISTS/SC_RADIO/
                 * SC_QUEUE) -- see those screens' own draw-side comments for
                 * the matching y = base + absolute*ROW_H - off formula this
                 * inverts. Already an absolute index, not scroll-relative,
                 * so these screens use it directly rather than `scroll +
                 * idx`. */
                int off_row_base = CONTENT_Y + (screen == SC_QUEUE ? QUEUE_SUMMARY_H : 0);
                int off = scroll * ROW_H + scroll_px;
                int smooth_row = (y - off_row_base + off) / ROW_H;
                if (screen == SC_QUEUE && y < CONTENT_Y + QUEUE_SUMMARY_H) smooth_row = -1;
                if (screen == SC_MENU) {
                    /* R81: greyed out on the draw side for exactly the same
                     * condition -- see that comment for why. */
                    int usb_storage = st_usb_mode() == 1;
                    /* R82: "Done" on the banner, checked ahead of the
                     * ordinary row math below -- the banner sits below
                     * every real row (idx would land past TOP_N there
                     * anyway, into "nothing there"), so this only ever
                     * fires from an actual tap inside the banner's own
                     * 60px strip, never shadows a real menu row above it. */
                    if (usb_storage && y >= FB_H - 60 && x >= usb_done_x() - 24) {
                        set_usb_storage_mode(0);
                    } else if (idx >= TOP_N) { /* nothing there */ }
                    else if (idx == TOP_MUSIC && !usb_storage) {
                        screen = SC_MUSIC_MENU; reset_scroll();
                    } else if (idx == TOP_AUDIOBOOKS && !usb_storage) {
                        ab_book_n = ab_scan_books(ab_books, AB_MAX_BOOKS);
                        ab_rebuild_rows();
                        screen = SC_AUDIOBOOKS; reset_scroll();
                        total = ab_book_n;
                        mlog("[music] %d audiobooks\n", ab_book_n);
                    } else if (idx == TOP_PODCASTS && !usb_storage) {
                        pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
                        pod_rebuild_rows();
                        screen = SC_PODCASTS; reset_scroll();
                        total = pod_feed_n;
                        mlog("[music] %d podcast feeds\n", pod_feed_n);
                    } else if (idx == TOP_EQ) {
                        eq_profile_n = ep_scan(eq_profiles, EP_MAX_PROFILES);
                        /* First visit this session, nothing chosen yet: load
                         * whatever sorts first rather than show an empty
                         * screen when a profile is sitting right there. */
                        if (!eq_cur_path[0] && eq_profile_n > 0) {
                            eq_switch_to(eq_profiles[0].path);
                            save_conf();          /* BG38 */
                        }
                        screen = SC_EQ; reset_scroll();
                        mlog("[music] %d EQ profiles\n", eq_profile_n);
                    } else if (idx == TOP_MSEB) {
                        screen = SC_MSEB; reset_scroll();
                    } else if (idx == TOP_RADIO) {
                        station_n = radio_load(stations, RADIO_MAX);
                        screen = SC_RADIO; reset_scroll();
                        mlog("[music] %d stations\n", station_n);
                    } else if (idx == TOP_SETTINGS) {
                        screen = SC_SETTINGS; reset_scroll();
                    }
                } else if (screen == SC_MUSIC_MENU) {
                    if (idx >= MENU_N) { /* nothing there */ }
                    else if (idx == MENU_PLAYLISTS) {
                        recent_mode = 0;
                        playlist_n = pl_list(playlists, PL_MAX);
                        screen = SC_PLAYLISTS; reset_scroll();
                        mlog("[music] %d playlists\n", playlist_n);
                    } else if (idx == MENU_RECENT_ADDED || idx == MENU_RECENT_HEARD) {
                        /* Not lib_albums()/load_page() -- this list is a
                         * one-shot recency ranking, not something a facet or
                         * an A-Z offset can page through, so it is computed
                         * once here and handed to the same rows[]/row_at()
                         * the normal album list already uses to draw and to
                         * turn a tap into SC_TRACKS. */
                        recent_mode = (idx == MENU_RECENT_ADDED) ? RECENT_ADDED : RECENT_HEARD;
                        cur_facet = NULL;
                        cur_facet_label = menu[idx].label;
                        cur_artist[0] = '\0';
                        albums_artist[0] = '\0';
                        screen = SC_ALBUMS; reset_scroll();
                        row_n = (recent_mode == RECENT_ADDED)
                              ? lib_albums_recent_added(rows, RECENT_ALBUMS_N)
                              : lib_albums_recent_heard(recent_heard_ts, rows, RECENT_ALBUMS_N);
                        row_base = 0;
                        total = row_n;
                        mlog("[music] %s: %d\n", cur_facet_label, total);
                    } else if (menu[idx].column) {
                        recent_mode = 0;
                        cur_facet = menu[idx].column;
                        cur_facet_label = menu[idx].label;
                        screen = SC_ARTISTS; reset_scroll();
                        total = lib_group_count(cur_facet);
                        load_page();
                        mlog("[music] %s: %d\n", cur_facet_label, total);
                    } else {
                        /* Albums lists the whole library, with no facet above
                         * it to go back to. */
                        recent_mode = 0;
                        cur_facet = NULL;
                        cur_facet_label = menu[idx].label;
                        cur_artist[0] = '\0';
                        albums_artist[0] = '\0';
                        screen = SC_ALBUMS; reset_scroll();
                        total = lib_albums_count(NULL, NULL);
                        load_page();
                        mlog("[music] all albums: %d\n", total);
                    }
                } else if (screen == SC_ARTISTS && row_at(scroll + idx)) {
                    lib_row_t *row = row_at(scroll + idx);
                    /* BG15: row->owner is lib_group()'s marker for "this row's
                     * name is the 'Unknown' display label, not real data" —
                     * filter by the sentinel instead so the empty/NUL column
                     * is matched rather than the literal text "Unknown". */
                    snprintf(cur_artist, sizeof(cur_artist), "%s",
                             row->owner[0] ? LIB_UNKNOWN_MARK : row->name);
                    /* BG-albumshdr: albums_artist must only ever be set where
                     * SC_ALBUMS is actually entered (see its own comment) --
                     * this row lands on SC_ARTIST_PAGE directly (the "Reported
                     * live" shortcut below), never SC_ALBUMS itself, so this
                     * used to leak the tapped artist's name into albums_artist
                     * with nothing to ever clear it again. The next plain
                     * "Albums" visit reached by backing out of some unrelated
                     * album's track list (not via the Music menu's own clear,
                     * which only fires when a menu row is freshly tapped)
                     * restored that stale name as if Albums were still
                     * filtered by it -- reported
                     * live as "Queen" sitting in the header of what was
                     * visibly the full, unfiltered album list. Same bug class
                     * BG7/the SC_PLAYING case above already fixed twice for
                     * two other routes into this exact mistake. */
                    artists_scroll_saved = scroll;         /* BG37 */
                    artists_scroll_px_saved = scroll_px;
                    /* Reported live: tapping an artist row here should land
                     * on that artist's own page (photo/bio/albums) directly,
                     * not the plain Albums-filtered-by-artist list -- the
                     * artist page already shows that same album list, plus
                     * more, so this isn't losing a destination, just
                     * skipping a now-redundant stop on the way to it.
                     * cur_facet (not a hardcoded "album_artist") since this
                     * list can be reached via either "Album artists" or
                     * "Artists" from the music menu, and the query has to
                     * match whichever one actually got tapped. */
                    snprintf(artist_page_name, sizeof(artist_page_name), "%s", row->name);
                    artist_page_from_list = 1;   /* back -> the Artists list */
                    artist_page_album_n = lib_albums(cur_facet, cur_artist,
                                                     artist_page_albums, ARTIST_ALBUMS_MAX, 0);
                    screen = SC_ARTIST_PAGE; reset_scroll();
                    artist_art_request(artist_page_name);
                    mlog("[music] %s -> %d albums\n", cur_artist, artist_page_album_n);
                } else if (screen == SC_TRACKS && scroll + idx < track_n &&
                           !pod_swipe_active) {
                    /* Plain albums are caught by their own SC_TRACKS branch
                     * above now (R46) -- only ab_list/pod_list ever reach
                     * here. !pod_swipe_active: same "was this release the
                     * end of a gesture" guard R70 uses everywhere else --
                     * without it, a swipe that let go short of the delete
                     * threshold also read as a tap on whichever episode was
                     * under the finger and started (or downloaded) it. */
                    if (pod_list) {
                        /* This list is one feed's episodes. An undownloaded
                         * one starts its download instead of playing --
                         * nothing to play yet -- one at a time, same as the
                         * whole-feed sync below. */
                        int pi = scroll + idx;
                        if (pod_eps[pi].downloaded) {
                            audio_set_speed(1000);
                            screen = SC_PLAYING;
                            pod_play_episode(pi);
                        } else if (!pod_download_active()) {
                            pod_download_start(pi);
                        }
                    } else if (ab_list) {
                        /* This list is the book's chapters. */
                        screen = SC_PLAYING;
                        ab_play_chapter(scroll + idx);
                    }
                } else if (screen == SC_PLAYLISTS && smooth_row == 0) {
                    /* R71: row 0, the "New Playlist" action -- no track to
                     * add on this path (see kb_new_playlist_add_track's own
                     * comment), just create an empty one and land back
                     * here with it in the list. */
                    kb_new_playlist_add_track = -1;
                    kb_open("Playlist name", KB_PURPOSE_NEW_PLAYLIST_NAME, "");
                } else if (screen == SC_PLAYLISTS && smooth_row - 1 < playlist_n) {
                    int pi = smooth_row - 1;   /* R71: row 0 is "New Playlist", not playlists[0] */
                    pl_path_t *paths = pl_scratch_get("open playlist");
                    int got = 0;
                    track_n = 0;
                    if (paths) {
                        got = pl_read(playlists[pi].path, paths, PL_SCRATCH_N);
                        for (int k = 0; k < got; k++)
                            if (lib_track_by_path(paths[k], &tracks[track_n]) == 0)
                                track_n++;
                        free(paths);
                    }
                    /* A playlist is an order someone chose; leave it alone. */
                    snprintf(cur_album, sizeof(cur_album), "%s", playlists[pi].name);
                    cur_artist[0] = '\0';
                    view_art_clear();   /* BG-playlist: no per-playlist art, don't show stale art */
                    browsing_is_playlist = 1;   /* BG73 */
                    tracks_from_artist_page = 0;
                    screen = SC_TRACKS; reset_scroll();
                    ab_list = 0;
                    pod_list = 0;
                    mlog("[music] playlist %s: %d of %d found\n",
                         playlists[pi].name, track_n, got);
                } else if (screen == SC_RADIO && smooth_row >= 0 && smooth_row < station_n) {
                    play_station(smooth_row);
                    if (audio_is_active()) screen = SC_PLAYING;
                    else if (!radio_msg[0])
                        snprintf(radio_msg, sizeof(radio_msg), "Could not reach that station");
                } else if (screen == SC_QUEUE && smooth_row >= 0 && smooth_row < queue_n &&
                           !queue_drag_active && !queue_swipe_active) {
                    /* R70: queue_drag_active/queue_swipe_active are still true
                     * here on the exact tick a drag or swipe ends -- they only
                     * clear later this same tick, in the live-tracking block
                     * further down -- so this guard really means "was this
                     * release the end of a drag or swipe", not "is one
                     * starting right now". Without it, releasing a dragged
                     * row over its new slot (or a swipe that sprang back
                     * short of the delete threshold) also read as a tap on
                     * whatever track was under the finger and started
                     * playing it, which is not what letting go of either
                     * gesture means. */
                    int qidx = queue_display_index(smooth_row);
                    if (qidx >= 0) {
                        audio_set_speed(1000);
                        screen = SC_PLAYING;
                        /* Reported live: backing all the way out to Albums
                         * afterward showed the queued track's own artist
                         * (e.g. "Oasis") as Albums' own filter/title instead
                         * of "Albums". go_back()'s SC_PLAYING case only
                         * protects albums_artist from being overwritten by
                         * q_artist when played_from_browse is set (BG7) --
                         * every other route into playing a track that
                         * matters here already sets it (play_from_list()'s
                         * own caller does), this one just hadn't been
                         * updated to. */
                        played_from_browse = 1;
                        play_index(qidx);
                    }
                } else if (screen == SC_ALBUMS && row_at(scroll + idx)) {
                    lib_row_t *row = row_at(scroll + idx);
                    snprintf(cur_album, sizeof(cur_album), "%s", row->name);
                    /* Take the artist from the album row rather than from
                     * whatever facet led here: two artists can have an album
                     * of the same name, and the track query needs both.
                     * Unconditional, not "if row->owner[0]" -- BG30: an
                     * album with no album_artist tag at all (common; see the
                     * BG11 comment in library.c) used to leave cur_artist
                     * holding whatever a *previous* album or artist browse
                     * had set, which then stuck on the Now Playing screen
                     * indefinitely. row->owner empty here genuinely means
                     * "this album has no album_artist", and lib_tracks_for_
                     * album() already retries without the filter when an
                     * empty artist finds nothing, so writing "" through is
                     * correct, not just harmless. */
                    snprintf(cur_artist, sizeof(cur_artist), "%s", row->owner);
                    browsing_is_playlist = 0;   /* BG73 */
                    tracks_from_artist_page = 0;
                    albums_scroll_saved = scroll;          /* BG37 follow-up */
                    albums_scroll_px_saved = scroll_px;
                    screen = SC_TRACKS; reset_scroll();
                    ab_list = 0;
                    pod_list = 0;
                    {
                        struct timespec t0, t1;
                        clock_gettime(CLOCK_MONOTONIC, &t0);
                        track_n = lib_tracks_for_album(cur_artist, cur_album,
                                                       tracks, (int)(sizeof(tracks)/sizeof(tracks[0])),
                                                       row->count);
                        clock_gettime(CLOCK_MONOTONIC, &t1);
                        long ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                                  (t1.tv_nsec - t0.tv_nsec) / 1000000L;
                        mlog("[music] %s -> %d tracks (%ld ms)\n", cur_album, track_n, ms);
                    }
                    /* BG70: view_art_request(), not art_request() -- the
                     * album-detail screen's cover is what's being *browsed*,
                     * completely independent of art_bits/the playing track,
                     * so the mini-player and Now Playing are never at risk
                     * of showing it (see view_art_request()'s own comment). */
                    if (track_n > 0)
                        view_art_request(tracks[0].path, cur_artist, cur_album);
                } else if (screen == SC_AUDIOBOOKS && scroll + idx < ab_book_n) {
                    ab_book_t *b = &ab_books[scroll + idx];
                    if (audiobook_mode && !strcmp(b->dir, ab_book.dir)) {
                        /* BG57: tapping the book already playing should behave
                         * like tapping the mini player -- straight to Now
                         * Playing, exactly where it is -- not reopen it as if
                         * freshly chosen. ab_load_book()/ab_resume_book()
                         * exist to *start* a book; calling them here would
                         * rescan the chapters and reseek to the last *saved*
                         * position, discarding whatever's actually mid-play. */
                        screen = SC_PLAYING;
                    } else {
                        ab_save_current_pos();   /* whatever was playing, before it's overwritten */
                        ab_load_book(b);
                        audiobook_mode = 1;
                        audio_set_speed(ab_speed_permille);
                        ab_playing[0] = '\0';    /* force a real open */
                        queue_n = 0;             /* force the mirror into queue[] */
                        screen = SC_PLAYING;
                        ab_resume_book();
                        mlog("[music] %s -> %d chapters in %d file(s)\n",
                             cur_album, ab_book.chap_n, ab_book.file_n);
                    }
                } else if (screen == SC_PODCASTS && scroll + idx < pod_feed_n) {
                    snprintf(cur_feed, sizeof(cur_feed), "%s", pod_feeds[scroll + idx].name);
                    pod_ep_n = pod_load_episodes(cur_feed, pod_eps, POD_MAX_ITEMS);
                    pod_rebuild_tracks();
                    snprintf(cur_album, sizeof(cur_album), "%s", cur_feed);
                    cur_artist[0] = '\0';
                    /* R58: was never reset here before podcast episodes
                     * could actually be queued (queue_play_next()'s
                     * q_is_playlist read was moot either way then) -- left
                     * stale from a previous Playlists visit, it would now
                     * make a podcast feed's own queue wrongly behave like a
                     * playlist's (never truncating a played-next episode's
                     * leftovers), same class of bug as every other
                     * SC_TRACKS entry point already guards against. */
                    browsing_is_playlist = 0;
                    screen = SC_TRACKS; reset_scroll();
                    ab_list = 0;
                    pod_list = 1;
                    mlog("[music] %s -> %d episodes\n", cur_feed, pod_ep_n);
                } else if (screen == SC_EQ_BANDS && smooth_row >= 0 && smooth_row < eq_cur.band_n) {
                    /* The toggle's own strip (matching where it's drawn)
                     * flips the band without leaving the list; anywhere else
                     * on the row opens the full editor. */
                    if (x > FB_W - 100) {
                        eq_band_t *b = &eq_cur.band[smooth_row];
                        b->on = !b->on;
                        eq_set_band(smooth_row, b);
                        eq_save_current();
                    } else {
                        eq_editing_band = smooth_row;
                        screen = SC_EQ_BAND; reset_scroll();
                    }
                }
            }
        }

        int keyed = 0;
        for (int k = 0; k < kfd_n; k++) {
            int r = handle_keys(kfd[k], kfd_src[k]);
            if (r < 0) {
                /* RBG1: this fd died (its device was torn down and
                 * recreated, e.g. a Bluetooth headset's AVRCP reconnect).
                 * Close it and drop it by swapping in the last entry, so
                 * the name frees up and the next scan_inputs() call below
                 * picks up a fresh, live handle instead of leaving this
                 * slot permanently dead for the rest of the app session. */
                close(kfd[k]);
                kfd_n--;
                kfd[k] = kfd[kfd_n];
                kfd_src[k] = kfd_src[kfd_n];
                snprintf(kfd_name[k], sizeof(kfd_name[0]), "%s", kfd_name[kfd_n]);
                k--;
                continue;
            }
            keyed |= r;
        }
        if (keyed) { dirty = 1; idle = 0; }

        /* R77: hold-to-power-off, the two halves handle_keys() itself can't
         * finish on its own -- the countdown overlay needs to redraw every
         * tick with no new key event to drive it (the physical button sends
         * exactly one down event and, eventually, one up event -- nothing in
         * between), and the graceful path needs `base`, only ever in scope
         * here in the main loop, for resume_save(). One check regardless of
         * which fd handle_keys() happened to see the events on, not one per
         * fd the way the read loop itself is. */
        if (power_key_down && !power_hard_fired) {
            long elapsed_ms = (long)((us_now() - power_key_down_us) / 1000);
            if (!power_hold_ui_shown && elapsed_ms >= POWER_HOLD_UI_MS) {
                /* BG112: captured here, the one moment the currently-visible
                 * page is guaranteed still clean -- this tick's own draw_ui()
                 * call (below, later in the loop) is what first paints the
                 * overlay, and dirty is only being set to 1 a few lines down
                 * from here, for that same not-yet-run call. */
                memcpy(power_pre_hold_fb, base + (size_t)(page ^ 1) * page_px,
                       page_px * sizeof(uint16_t));
                power_hold_ui_shown = 1;
                power_key_pending_lock = 0;   /* becoming a hold, not a tap -- the deferred lock never fires */
                /* button_locked's whole point is that a stray pocket press
                 * does nothing at all, screen included (BG20) -- so this
                 * can't wake on the press itself the way an ordinary press
                 * does, only once a real sustained hold is confirmed here,
                 * by which point an accidental brush is not what's
                 * happening. Without it the countdown would draw onto a
                 * powered-down panel, invisible until release. */
                if (button_locked && read_int_file(BACKLIGHT) <= 0) {
                    if (last_lit_bright > 0) saved_brightness = last_lit_bright;
                    locked = 1; set_locked(0);
                }
            }
            if (power_hold_ui_shown) dirty = 1;   /* redraw every tick so the bar animates */
            if (elapsed_ms >= POWER_HOLD_FORCE_MS) {
                mlog("[music] power held %ldms -- forcing shutdown, nothing saved\n", elapsed_ms);
                power_hard_fired = 1;
                power_shutdown_kind = 2;
                dirty = 1;
                if (system("/sbin/poweroff") == -1) { }
            }
        }
        if (power_release_outcome == 1) {
            power_release_outcome = 0;
            mlog("[music] power held -- graceful shutdown, saving first\n");
            ab_save_current_pos();
            pod_save_current_pos();
            /* BG112: NOT "either page holds the last drawn frame" the way
             * the auto-shutdown path's own identical-looking call can rely
             * on -- the countdown overlay has been the last thing drawn for
             * up to two seconds by the time release gets here, on both
             * pages (the per-frame mirror below keeps them identical). The
             * clean copy taken the instant before the overlay first
             * appeared is what a resume should actually show. */
            resume_save(power_pre_hold_fb);
            power_shutdown_kind = 1;
            dirty = 1;
            if (system("/sbin/poweroff") == -1) { }
        }

        /* A headset connecting brings its AVRCP device with it. */
        if (++rescan_tick >= 90) { rescan_tick = 0; scan_inputs(); }

        /* Regardless of pause state, so putting the device down mid-chapter
         * and never touching it again still gets saved -- the alternative
         * is trusting a clean exit, and a battery pull or a crash is exactly
         * what this exists to survive. Once every ~15s: cheap enough not to
         * matter, infrequent enough not to wear the card writing it. */
        if (++ab_pos_tick >= 450) { ab_pos_tick = 0; ab_save_current_pos(); pod_save_current_pos(); }

        /* R29: waveform seek bar, Music only -- sampled on this same poll
         * every other live readout in this app already runs on, not a
         * callback from the decode thread. Paused reads nothing (a paused
         * chunk's peak is stale, from whenever playback actually last
         * wrote), which is fine -- the bucket it would have landed in gets
         * filled in on the next unpaused tick at the same position. */
        if (wave_capturing && audio_is_active() && !audio_is_paused()) {
            int dur = audio_dur_ms();
            if (dur > 0) {
                int pos = audio_pos_ms();
                int b = (int)((int64_t)pos * WAVE_BUCKETS / dur);
                if (b < 0) b = 0;
                if (b >= WAVE_BUCKETS) b = WAVE_BUCKETS - 1;
                int32_t peak = audio_current_peak();
                if (peak > (int32_t)wave_capture[b]) wave_capture[b] = (uint32_t)peak;
                wave_touched[b] = 1;
                if (b > wave_last_bucket) wave_last_bucket = b;
            }
        }

        /* Podcast downloads and whole-feed syncs both run as detached child
         * processes (see podcast.c) -- polled and reaped every tick, cheap
         * when idle since both are no-ops with nothing running. A completed
         * download re-reads the feed from disk rather than patching pod_eps[]
         * in place, the same one-shot recompute pattern SC_AUDIOBOOKS/
         * SC_PODCASTS already use after a rescan. */
        if (pod_download_poll() == 1 && pod_list) {
            pod_ep_n = pod_load_episodes(cur_feed, pod_eps, POD_MAX_ITEMS);
            pod_rebuild_tracks();
            dirty = 1;
        }
        /* Read the log BEFORE reaping, not after. podsync_once.sh writes its
         * final "__DONE__" line and then exits microseconds later, while this
         * loop only polls every ~33ms -- so reaping first meant waitpid()
         * almost always saw the child gone while update_running_flag was
         * still set, which pod_update_reap() reports as "died". Worse, it
         * clears the running flag on the way out, so the pod_update_tail()
         * below (gated on exactly that flag) never ran again and the
         * "__DONE__" that says otherwise was never read at all. The result
         * was a red "Sync stopped early." after essentially every
         * *successful* sync. Reading first means a completed run has always
         * consumed its own "__DONE__" (and cleared the flag itself) by the
         * time reap looks, leaving reap to flag only a genuine early exit --
         * one that ended without ever writing that line. */
        if (pod_update_running()) {
            pod_sync_log_n = pod_update_tail(pod_sync_log, POD_SYNC_LOG_N);
            if (!pod_update_running() && (screen == SC_PODCASTS || screen == SC_POD_SYNC)) {
                /* Just finished: the feed list on screen may have new
                 * subfolders (a brand new feed) or new manifest-only
                 * episodes -- refresh it the same way opening Podcasts does. */
                pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
                pod_rebuild_rows();
                total = pod_feed_n;
            }
            dirty = 1;
        }
        pod_update_reap();

        /* Pulling the headphones out should not carry on broadcasting to the
         * room. Only for the wired route — unplugging the jack says nothing
         * about a Bluetooth or USB stream. */
        {
            int jack = st_headset();
            if (jack_was && !jack && !audio_is_paused() && audio_is_active() &&
                !strcmp(audio_output(), "3.5 mm")) {
                audio_toggle();
                mlog("[music] headphones removed, paused\n");
                dirty = 1;
            }
            jack_was = jack;
        }

        if (vol_ticks > 0) {
            if (!vol_dragging) vol_ticks--;
            if (vol_ticks == 0) dirty = 1;
        }
        if (seek_toast_ticks > 0) {
            seek_toast_ticks--;
            if (seek_toast_ticks == 0) dirty = 1;
        }
        if (vol_ticks > 0 && touch_down && !qs_open && !usb_bypass_active &&
            touch_y >= STATUS_H && touch_y < STATUS_H + VOL_H) {
            if (!vol_dragging) { vol_dragging = 1; vol_applied = audio_volume(); }
            int bw = FB_W - 48;
            int v = (live_x - 24) * 100 / (bw > 0 ? bw : 1);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            if (v != vol_drag_pct) { vol_drag_pct = v; dirty = 1; }
            /* Applying this on every frame means a fork and exec of amixer,
             * twice over — set and read back — thirty times a second on
             * Bluetooth. The bar follows the finger; the mixer catches up a
             * few times a second, and exactly once more on release. */
            if (++vol_apply_tick >= 6) {
                vol_apply_tick = 0;
                if (vol_drag_pct != vol_applied) {
                    audio_volume_set(vol_drag_pct);
                    vol_applied = vol_drag_pct;
                }
            }
            idle = 0;
        } else if (vol_dragging && !touch_down) {
            vol_dragging = 0;
            if (vol_drag_pct != vol_applied) {
                audio_volume_set(vol_drag_pct);
                vol_applied = vol_drag_pct;
            }
            vol_ticks = VOL_TICKS;          /* restart the countdown on release */
        }

        /* Pull down from the status strip to open quick settings; the panel is
         * modal while it is open, so nothing below it needs to know. */
        if (!qs_open && touch_down && touch_y < QS_PULL_ZONE &&
            !(vol_ticks > 0 && touch_y >= STATUS_H) &&
            live_y - touch_y > QS_PULL) {
            qs_open = 1;
            qs_refresh();
            dirty = 1; idle = 0;
        }

        /* Dragging the brightness bar sets it live rather than on release: it
         * is the one setting where you want to see the result while choosing. */
        if (qs_open && qs_dragging && touch_down) {
            int bw = FB_W - 48;
            int v = (live_x - 24) * qs_bright_max / (bw > 0 ? bw : 1);
            if (v != qs_bright) {
                qs_bright = v < 1 ? 1 : (v > qs_bright_max ? qs_bright_max : v);
                st_brightness_set(qs_bright);
                saved_brightness = qs_bright;   /* so unlocking restores this */
                brightness_pref = qs_bright;    /* R64: so a reboot restores this */
                dirty = 1;
            }
            idle = 0;
        } else if (qs_dragging && !touch_down) {
            qs_dragging = 0;
            /* R64: once per drag, on release -- the live block above already
             * applies every intermediate value as the finger moves, but
             * calling save_conf() that often would mean a fopen/fclose pair
             * per frame of the drag, for a value only the final one needs. */
            save_conf();
        }

        /* Running down the A-Z strip. The test is on where the finger went
         * down, not where it is: a touchscreen only reports an axis that has
         * changed, so during a vertical drag the live X reading is whatever it
         * was last time and testing that missed the strip entirely. */
        {
            if (qs_open && touch_down && !qs_dragging &&
                touch_y > qs_bar_y() - 26 && touch_y < qs_bar_y() + 26)
                qs_dragging = 1;

            g_phase = "index";
            int was = index_active;
            index_active = touch_down && index_visible() &&
                           touch_x >= FB_W - INDEX_TOUCH_W &&
                           touch_y >= CONTENT_Y && touch_y < index_bottom();
            if (index_active) {
                /* Prepare only after this is known to be an A-Z gesture. */
                index_prepare();
                int prev_shown = index_shown;      /* only for the idx2 log below */

                /* Slot first, before anything reads index_shown/scroll/
                 * index_lock_end for a decision. A diagonal drag reports X
                 * and Y in the same tick, and deciding whether to lock
                 * using yesterday's letter while today's Y has already
                 * moved is exactly the bug this replaced: moving down while
                 * sliding left evaluated "should I lock" against whatever
                 * letter the thumb was over BEFORE the vertical move, so
                 * going left got silently ignored whenever it landed in the
                 * same tick as a vertical crossing. */
                int slot = index_slot(live_y);
                if (slot != index_shown) {
                    /* Live, not deferred. Waiting for the thumb to settle was
                     * meant to save two table scans per stop crossed, but the
                     * list lagging behind the droplet is the whole feel of the
                     * thing — and the scans it was protecting against are not
                     * the ones that ever cost anything. */
                    index_shown = slot;
                    g_phase = "index-jump";
                    index_jump(live_y);
                    dirty = 1;
                }

                /* Sliding left locks the strip inside the letter under the
                 * thumb: it stops being A-Z and becomes that letter's own
                 * second letters, which is then scrolled exactly like the
                 * top level. Sliding back out returns to A-Z. Everything
                 * here now reads index_shown/scroll/index_lock_end after
                 * the slot update above, so it always reflects this tick's
                 * position, not last tick's. */
                int deep = live_x < FB_W - INDEX_TOUCH_W - INDEX_SUB_DEAD;
                /* Only worth subdividing a letter that doesn't already fit
                 * on one screen. G has six albums and room for nine — sliding
                 * between Gi/Go/Gr subdivided it anyway, and switching bands
                 * that all draw from the same six albums looks exactly like
                 * unwanted scrolling even though it never reaches H.
                 * index_lock_end/scroll are already sitting from the
                 * top-level jump that necessarily happened before the thumb
                 * could slide left onto this letter, so no extra query. */
                int fits = index_lock_end >= 0 && index_lock_end - scroll <= vis_rows();
                if (deep && !index_sub_lock && index_shown > 0 && !fits) {
                    char L = index_letter(index_shown);
                    int n = 0;
                    for (int k = 0; k < index_pfx_n && n < INDEX_SUB_MAX; k++)
                        if (index_pfx[2 * k] == L) index_sub[n++] = index_pfx[2 * k + 1];
                    if (n > 1) {           /* one stop is not a subdivision */
                        index_sub_lock = L; index_sub_n = n;
                        /* Jump immediately rather than deferring to "next
                         * tick's slot != index_shown" — that deferral is
                         * exactly the kind of one-tick gap this fix removes. */
                        index_shown = -1;
                        index_jump(live_y);
                        dirty = 1;
                        mlog("[music] index: locked %c, %d stops\n", L, n);
                    }
                } else if (!deep && index_sub_lock) {
                    index_sub_lock = 0; index_sub_n = 0; index_lock_end = -1;
                    index_shown = -1;
                    index_jump(live_y);
                    dirty = 1;
                }

                if ((g_tick & 3) == 0)
                    mlog("[music] idx2 deep=%d fits=%d prev=%d shown=%d slot=%d lock=%c "
                         "lock_end=%d scroll=%d vis=%d live_x=%d live_y=%d\n",
                         deep, fits, prev_shown, index_shown, slot,
                         index_sub_lock ? index_sub_lock : '-',
                         index_lock_end, scroll, vis_rows(), live_x, live_y);
                idle = 0;
            } else if (was) {
                index_shown = -1;
                index_sub_lock = 0;
                index_sub_n = 0;
                index_lock_end = -1;
                dirty = 1;
            }
        }

        /* R70: drag-to-reorder, via the grip handle -- checked ahead of List
         * scrolling below so a grip-drag can suppress the ordinary scroll
         * drag entirely (queue_drag_active feeds into scrollable's own
         * condition further down), the same way scrub_active/qs_open
         * already do for their own drags. touch_x/touch_y are fixed at
         * the press location for the whole gesture (see the raw touch
         * reader), so this hit test is stable for as long as the touch
         * lasts -- no separate "just pressed" transition to track. */
        if (screen == SC_QUEUE) {
            int grip_x0 = FB_W - 24 - (index_visible() ? INDEX_W : 0) - 32;
            int list_top = CONTENT_Y + QUEUE_SUMMARY_H;
            /* R76: off, not a bare `- scroll`, now that the draw side scrolls
             * smoothly (y = list_top + display_i*ROW_H - off) -- this is
             * that same formula solved for display_i, so hit-testing always
             * agrees with whatever's actually drawn, mid-drag scroll_px
             * included, not just at whole-row rest. */
            int off = scroll * ROW_H + scroll_px;
            int press_display_i = (touch_y - list_top + off) / ROW_H;
            int valid_row = touch_y >= list_top &&
                            press_display_i >= 0 && press_display_i < queue_n;
            int grip_hit = valid_row && touch_x >= grip_x0;
            int was_qdrag = queue_drag_active;
            queue_drag_active = touch_down && grip_hit;
            if (queue_drag_active && !was_qdrag) {
                queue_drag_display_i = press_display_i;
                queue_drag_grab_dy = touch_y - (list_top + press_display_i * ROW_H - off);
                dirty = 1;
            } else if (!queue_drag_active && was_qdrag) {
                dirty = 1;   /* release: clear the highlight */
            } else if (queue_drag_active) {
                int ghost_top = live_y - queue_drag_grab_dy;
                int slot_top  = list_top + queue_drag_display_i * ROW_H - off;
                /* Only re-target once the finger has crossed into a new
                 * slot's own span, not merely nudged the current one --
                 * rounding to the nearest row would flip the target the
                 * instant the finger passed a row's midpoint in either
                 * direction, which reads as flickery rather than deliberate. */
                int target = queue_drag_display_i;
                if (ghost_top < slot_top - ROW_H / 2 && target > 0) target--;
                else if (ghost_top > slot_top + ROW_H / 2 && target < queue_n - 1) target++;
                if (target != queue_drag_display_i) {
                    queue_move_display(queue_drag_display_i, target);
                    queue_drag_display_i = target;
                    dirty = 1;
                }
            }

            /* R70: swipe-to-remove. Only a candidate when the press wasn't
             * on the grip (grip always means reorder, never delete), and
             * only actually latches once horizontal travel clearly leads
             * vertical -- an ordinary vertical scroll started on a row's
             * body must never be mistaken for the start of a delete swipe.
             * Once latched it stays latched for the rest of the gesture
             * (queue_swipe_active persists below rather than being
             * recomputed from live_x/live_y every tick), the same "can't
             * un-become itself mid-drag" shape edge_active already has. */
            if (!queue_swipe_active && touch_down && valid_row && !grip_hit) {
                int dx = live_x - touch_x, dy = live_y - touch_y;
                if (abs(dx) > abs(dy) && abs(dx) > 10) {
                    queue_swipe_active = 1;
                    queue_swipe_display_i = press_display_i;
                }
            }
            if (queue_swipe_active) {
                if (!touch_down) {
                    /* 120px, not a fraction of FB_W -- a swipe threshold that
                     * scales with screen width would need retuning on every
                     * device this ever ran on; a fixed distance doesn't. */
                    if (queue_swipe_dx <= -120 || queue_swipe_dx >= 120)
                        queue_remove_display(queue_swipe_display_i);
                    queue_swipe_active = 0;
                    queue_swipe_dx = 0;
                } else {
                    queue_swipe_dx = live_x - touch_x;
                }
                dirty = 1;
            }
        } else {
            queue_drag_active = 0;
            queue_swipe_active = 0;
            queue_swipe_dx = 0;
        }

        /* R70: same grip/drag/swipe shapes, extended to a playlist's own
         * track list. Coordinate space differs from SC_QUEUE's: this screen
         * scrolls continuously (header_h/off), not row-indexed from
         * CONTENT_Y, so content_y + track_index_at() (already shared with
         * the tap and press-and-hold handlers -- see BG103) does the
         * hit-testing instead of a flat division. Never true for a plain
         * album (browsing_is_playlist gates it) or audiobook chapters
         * (ab_list excludes this branch entirely). */
        if (screen == SC_TRACKS && !ab_list && !pod_list && browsing_is_playlist) {
            int off = scroll * ROW_H + scroll_px;
            int header_h = tracks_hdr_h();
            int content_y = touch_y + off;
            int press_idx = track_index_at(content_y);
            /* A row hidden under the mini player is not actually visible,
             * so a touch landing there must not register as a grip/swipe
             * start on it -- same reasoning BG102 already applied to the
             * Bluetooth scan list, just the other direction (there, a
             * hidden row read as tappable; here, without this, one would
             * read as draggable). */
            int mini_bottom = mini_visible() ? FB_H - MINI_H : FB_H;
            int grip_x0 = FB_W - 56;
            int valid_row = press_idx >= 0 && press_idx < track_n && touch_y < mini_bottom;
            int grip_hit = valid_row && touch_x >= grip_x0;
            int was_pdrag = playlist_drag_active;
            playlist_drag_active = touch_down && grip_hit;
            if (playlist_drag_active && !was_pdrag) {
                playlist_drag_idx = press_idx;
                int slot_top = header_h + press_idx * ROW_H - off;
                playlist_drag_grab_dy = touch_y - slot_top;
                dirty = 1;
            } else if (!playlist_drag_active && was_pdrag) {
                dirty = 1;
            } else if (playlist_drag_active) {
                int ghost_top = live_y - playlist_drag_grab_dy;
                int slot_top = header_h + playlist_drag_idx * ROW_H - off;
                int target = playlist_drag_idx;
                if (ghost_top < slot_top - ROW_H / 2 && target > 0) target--;
                else if (ghost_top > slot_top + ROW_H / 2 && target < track_n - 1) target++;
                if (target != playlist_drag_idx) {
                    playlist_move_track(playlist_drag_idx, target);
                    playlist_drag_idx = target;
                    dirty = 1;
                }
            }

            if (!playlist_swipe_active && touch_down && valid_row && !grip_hit) {
                int dx = live_x - touch_x, dy = live_y - touch_y;
                if (abs(dx) > abs(dy) && abs(dx) > 10) {
                    playlist_swipe_active = 1;
                    playlist_swipe_idx = press_idx;
                }
            }
            if (playlist_swipe_active) {
                if (!touch_down) {
                    if (playlist_swipe_dx <= -120 || playlist_swipe_dx >= 120)
                        playlist_remove_track(playlist_swipe_idx);
                    playlist_swipe_active = 0;
                    playlist_swipe_dx = 0;
                } else {
                    playlist_swipe_dx = live_x - touch_x;
                }
                dirty = 1;
            }
        } else {
            playlist_drag_active = 0;
            playlist_swipe_active = 0;
            playlist_swipe_dx = 0;
        }

        /* R70: swipe-to-remove on a podcast episode list -- no grip, no
         * drag (see pod_swipe_active's own comment). Flat CONTENT_Y/scroll
         * coordinate space, the same one this screen's tap dispatch already
         * uses (the "idx = (y - CONTENT_Y) / ROW_H" further up), not
         * track_index_at() -- that function belongs to the *other* SC_TRACKS
         * path (continuous-scroll, plain albums/playlists). Gated on a
         * downloaded episode only: nothing to delete for one that is still
         * just a manifest entry, and pod_remove_download() itself no-ops on
         * that case too, but excluding it here keeps a not-yet-downloaded
         * row from visually reacting to a swipe that would do nothing. */
        if (screen == SC_TRACKS && pod_list) {
            int press_idx = scroll + (touch_y - CONTENT_Y) / ROW_H;
            int mini_bottom = mini_visible() ? FB_H - MINI_H : FB_H;
            int valid_row = touch_y >= CONTENT_Y && touch_y < mini_bottom &&
                            press_idx >= 0 && press_idx < track_n &&
                            pod_eps[press_idx].downloaded;
            if (!pod_swipe_active && touch_down && valid_row) {
                int dx = live_x - touch_x, dy = live_y - touch_y;
                if (abs(dx) > abs(dy) && abs(dx) > 10) {
                    pod_swipe_active = 1;
                    pod_swipe_idx = press_idx;
                }
            }
            if (pod_swipe_active) {
                if (!touch_down) {
                    if (pod_swipe_dx <= -120 || pod_swipe_dx >= 120)
                        pod_remove_download(pod_swipe_idx);
                    pod_swipe_active = 0;
                    pod_swipe_dx = 0;
                } else {
                    pod_swipe_dx = live_x - touch_x;
                }
                dirty = 1;
            }
        } else {
            pod_swipe_active = 0;
            pod_swipe_dx = 0;
        }

        /* List scrolling: tracked live, one pixel at a time, rather than
         * jumping by whatever whole number of rows the release distance
         * happened to divide into — which is where "four items at a time"
         * came from, since an ordinary swipe covers roughly that many row
         * heights before the finger lifts. */
        {
            int scrollable = screen == SC_ARTISTS || screen == SC_ALBUMS ||
                             screen == SC_TRACKS || screen == SC_PLAYLISTS ||
                             screen == SC_RADIO || screen == SC_EQ_BANDS ||
                             screen == SC_SETTINGS || screen == SC_SETTINGS_TIMEZONE ||
                             screen == SC_SETTINGS_WIFI || screen == SC_SETTINGS_BT ||   /* R75/BG109 */
                             screen == SC_PODCASTS || screen == SC_AUDIOBOOKS ||   /* BG109 */
                             screen == SC_QUEUE || screen == SC_ARTIST_PAGE;
            /* R46: the album-detail screen has no status bar to keep a drag
             * from starting under -- its cover runs from y=0, same as Now
             * Playing's own art, and a touch-down anywhere on it has to be
             * draggable or the whole top of the screen would be dead to
             * scrolling. Artist page: same, its own photo runs edge-to-edge
             * from y=0 too. */
            int drag_top = ((screen == SC_TRACKS && !ab_list && !pod_list) ||
                            screen == SC_ARTIST_PAGE) ? 0 : CONTENT_Y;
            /* Reported live on the album-detail screen: the cover would
             * visibly "pop" (a brief, wrong scroll jump) right as a
             * left-edge swipe-back began, before the gesture was recognised
             * as a back-swipe and the screen navigated away. Root cause:
             * edge_active only latches once horizontal travel exceeds 4px
             * (see the touch-read loop above) -- for a touch that started
             * inside EDGE_ZONE, the few ticks before that threshold trips
             * were still wide open to list_dragging, which reads whatever
             * incidental vertical wobble a real human swipe has (nobody's
             * finger moves in a perfectly straight horizontal line) as a
             * genuine scroll position update. continuous redraw on this
             * screen (below) then painted that transient wrong position on
             * screen for a frame or two -- the pop. Held off here for
             * exactly as long as the gesture is still ambiguous: started in
             * the edge zone, not yet recognised as an edge-swipe, and
             * horizontal travel so far hasn't fallen behind vertical (i.e.
             * it could still become one). Once vertical travel clearly
             * leads, edge_active can never latch anyway (it requires
             * horizontal to dominate), so there's nothing left to protect
             * against and the drag is free to start. Touches starting well
             * clear of the edge zone are entirely unaffected. */
            int edge_zone_ambiguous = touch_x < EDGE_ZONE && !edge_active &&
                                       abs(live_x - touch_x) >= abs(live_y - touch_y);
            /* R80 follow-up: reported live -- swiping up from the bottom
             * edge scrolled the list AND navigated home, both at once.
             * home_edge_active only excludes list_dragging from the tick it
             * latches on (a few px of dominant vertical travel); before
             * that this is the exact same ambiguous-window gap the back
             * gesture's own edge_zone_ambiguous already exists to close,
             * just rotated -- a touch that started in HOME_EDGE_ZONE with
             * vertical travel not yet behind horizontal could still become
             * a home swipe, so list_dragging holds off rather than reading
             * a human hand's incidental sideways wobble as a real scroll. */
            int home_edge_zone_ambiguous = touch_y > FB_H - HOME_EDGE_ZONE && !home_edge_active &&
                                            abs(live_y - touch_y) >= abs(live_x - touch_x);
            int was = list_dragging;
            list_dragging = touch_down && scrollable && !index_active &&
                            !scrub_active && !qs_open && !queue_drag_active && !queue_swipe_active &&
                            !playlist_drag_active && !playlist_swipe_active && !pod_swipe_active &&
                            touch_y >= drag_top && !edge_active && !edge_zone_ambiguous &&
                            !home_edge_active && !home_edge_zone_ambiguous;
            if (list_dragging && !was) {
                /* A raw drag on the list itself is free browsing, not bound
                 * by wherever the index last landed — otherwise a stale
                 * boundary from an earlier jump could clip content that has
                 * nothing to do with it once scrolled somewhere else. */
                if (index_lock_end >= 0) dirty = 1;
                index_lock_end = -1;
                list_down_y = live_y;
                list_start_px = scroll * ROW_H + scroll_px;
                list_last_y = live_y;
                list_last_tick = g_tick;
                list_velocity = 0;
                inertia_active = 0;
            } else if (list_dragging) {
                int changed = scroll_to_px(list_start_px + (list_down_y - live_y));
                /* R46 follow-up: everywhere else, holding dirty for a whole
                 * row's worth of drag (BG2) is the right trade -- redrawing a
                 * plain text list every pixel bought nothing anyone could see
                 * and cost a redraw on nearly every tick. The album-detail
                 * screen is different: it's a full-screen cover image sliding
                 * along with the text, and holding it frozen between row
                 * crossings read as chunky/laggy rather than smooth, reported
                 * live right after this screen shipped. Continuous redraw
                 * only for this one screen, not a blanket change. */
                /* R76: same continuous-redraw treatment extended to every
                 * flat-list screen converted to off-based smooth scrolling
                 * this round -- SC_ARTISTS/SC_ALBUMS deliberately excluded,
                 * see their own row_at()/scroll_to_px() comment: they're
                 * backed by a database-paged window, not a plain array, and
                 * load_page() is only re-checked on a whole-row `changed`,
                 * so forcing continuous redraw there risks a mid-drag frame
                 * reading rows outside the currently loaded page. */
                int continuous = (screen == SC_TRACKS && !ab_list && !pod_list) || screen == SC_ARTIST_PAGE ||
                                  screen == SC_PLAYLISTS || screen == SC_RADIO || screen == SC_QUEUE ||
                                  screen == SC_EQ_BANDS || screen == SC_SETTINGS ||
                                  screen == SC_SETTINGS_WIFI || screen == SC_SETTINGS_BT || screen == SC_SETTINGS_TIMEZONE;
                if (changed || continuous) dirty = 1;
                unsigned dt = g_tick - list_last_tick;
                if (dt > 0) {
                    /* Forward-positive: finger moving up (y decreasing)
                     * scrolls the list forward, same sign as the jump above. */
                    list_velocity = (float)(list_last_y - live_y) / (float)dt;
                    list_last_y = live_y;
                    list_last_tick = g_tick;
                }
                idle = 0;
            } else if (was) {
                /* Release: hand off to inertia if the finger was moving. */
                inertia_active = list_velocity > 0.6f || list_velocity < -0.6f;
                /* R46 follow-up: a slow release that ends inside the
                 * rubber-banded overscroll has near-zero velocity and would
                 * otherwise never spring back -- force the coast tick to
                 * run regardless, so out-of-bounds always resolves rather
                 * than sticking wherever the finger happened to let go. */
                if (screen == SC_TRACKS && !ab_list && !pod_list) {
                    int cur = scroll * ROW_H + scroll_px;
                    if (cur < 0 || cur > tracks_max_px()) inertia_active = 1;
                }
                dirty = 1;
            }
        }
        if (inertia_active && !list_dragging) {
            /* Spring-pull physics below is only meaningful for the two
             * true-pixel-bounded screens (tracks_max_px()/artist_page_max_px()
             * are rubber-band bounds, not a row count) -- kept scoped to just
             * those two, unlike the broader continuous-redraw forcing. */
            int continuous = (screen == SC_TRACKS && !ab_list && !pod_list) || screen == SC_ARTIST_PAGE;
            if (continuous) {
                /* Spring pull toward whichever bound is exceeded, layered
                 * onto the ordinary fling velocity below -- proportional to
                 * how far out of bounds, so it eases in rather than jumping,
                 * and settles the position rather than just decaying
                 * whatever residual velocity release happened to leave.
                 * R53: 0.1875, not the original 0.15 -- reported live as too
                 * slow to snap back; +25% on the coefficient (not friction
                 * below, which also governs ordinary fling deceleration and
                 * is out of this request's scope) speeds up the pull without
                 * changing its easing shape. */
                int cur = scroll * ROW_H + scroll_px;
                int max_px = (screen == SC_ARTIST_PAGE) ? artist_page_max_px() : tracks_max_px();
                if (cur < 0) list_velocity += (0 - cur) * 0.1875f;
                else if (cur > max_px) list_velocity += (max_px - cur) * 0.1875f;
            }
            int changed = scroll_to_px((int)(scroll * ROW_H + scroll_px + list_velocity));
            /* Same continuous-redraw reasoning as the drag tick above, for
             * the coast after release -- `smooth_coast` covers the same
             * R76 screens as the drag tick's `continuous`, kept separate
             * from `continuous` itself so the spring-pull block above stays
             * scoped to only the two true-pixel-bounded screens. */
            int smooth_coast = continuous || screen == SC_PLAYLISTS || screen == SC_RADIO ||
                                screen == SC_QUEUE || screen == SC_EQ_BANDS || screen == SC_SETTINGS ||
                                screen == SC_SETTINGS_WIFI || screen == SC_SETTINGS_BT || screen == SC_SETTINGS_TIMEZONE;
            if (changed || smooth_coast) dirty = 1;
            list_velocity *= 0.90f;     /* friction: dead within about a second */
            if (list_velocity < 0.6f && list_velocity > -0.6f) {
                /* Still out of bounds with velocity spent -- keep the spring
                 * alone driving it home rather than declaring the coast
                 * finished while a gap is still visible. Snapped exactly
                 * once within 2px rather than left to the spring's own
                 * asymptotic approach: float-to-int truncation on a
                 * shrinking velocity could otherwise stall a pixel or two
                 * short of the boundary forever, never quite satisfying
                 * cur < 0 / cur > max_px again but never reaching 0 either. */
                int still_out = 0;
                if (continuous) {
                    int cur = scroll * ROW_H + scroll_px;
                    int max_px = (screen == SC_ARTIST_PAGE) ? artist_page_max_px() : tracks_max_px();
                    if (cur < 0 && cur > -2) scroll_to_px(0);
                    else if (cur > max_px && cur < max_px + 2) scroll_to_px(max_px);
                    else still_out = cur < 0 || cur > max_px;
                }
                if (!still_out) inertia_active = 0;
            }
            idle = 0;
        }
        /* Deliberately keyed on inertia_active alone -- see its own comment
         * on why list_dragging must not also reset this. Capped rather
         * than left to grow unbounded: only ever compared against a small
         * threshold below, and an unsigned counter running for the hours a
         * session can last would eventually wrap (harmlessly, but there's
         * no reason to lean on that instead of just capping it). */
        if (inertia_active) list_settled_ticks = 0;
        else if (list_settled_ticks < 1000) list_settled_ticks++;

        /* Show-notes scrolling: no inertia, no row snapping -- it's free text
         * in a fixed box, not a list, so the plain drag-follows-the-finger
         * half of the list-scrolling trick above is all this needs. Clamped
         * to [0, a generous upper bound] rather than measured exactly against
         * the wrapped text's real height, which pod_draw_notes() only knows
         * mid-draw -- an 8KB buffer can't wrap past roughly this many pixels
         * of lines, so overscroll into blank space is bounded, not unbounded. */
        {
            int notes_active = screen == SC_PLAYING && podcast_mode && pod_notes_showing;
            int was = pod_notes_dragging;
            pod_notes_dragging = touch_down && notes_active && touch_y < ART_PX;
            if (pod_notes_dragging && !was) {
                pod_notes_down_y = live_y;
                pod_notes_start_px = pod_notes_scroll_px;
            } else if (pod_notes_dragging) {
                int px = pod_notes_start_px + (pod_notes_down_y - live_y);
                if (px < 0) px = 0;
                if (px > 6000) px = 6000;
                if (px != pod_notes_scroll_px) { pod_notes_scroll_px = px; dirty = 1; }
                idle = 0;
            }
        }

        /* Dragging a long title sideways. Relative, not absolute: the string
         * follows the finger from wherever it was left, rather than jumping so
         * that some position maps to some offset -- which is what the scrub bar
         * and the sliders below want, but would be wrong here.
         *
         * touch_x is the position of the *press*, not the current finger, so
         * (live_x - touch_x) is travel since the drag began. The result is
         * committed on release, so the title stays where it was let go. */
        {
            int was = title_dragging;
            title_dragging = touch_down && screen == SC_PLAYING && !radio_mode &&
                             queue_n > 0 && title_span > 0 &&
                             touch_y > title_y() - 16 &&
                             touch_y < title_y() + TEXT_PX_TITLE + 16;
            if (title_dragging) {
                title_off_live = title_off + (live_x - touch_x);
                if (title_off_live > 0) title_off_live = 0;
                if (title_off_live < -title_span) title_off_live = -title_span;
                dirty = 1; idle = 0;
            } else if (was) {
                title_off = title_off_live;
                dirty = 1; idle = 0;
            }
        }

        /* Scrubbing: begins when the finger goes down on the bar and lasts
         * until it lifts. The seek is left to the release, which arrives as a
         * tap at the final position and is handled with the other taps. */
        {
            int was = scrub_active;
            /* BG92: missing !qs_open let a touch in the progress-bar's y-range
             * start a scrub even with the quick-settings panel open over top
             * of it -- every other gesture check here (edge-swipe-back,
             * title long-press) already guards on this, this one just didn't. */
            scrub_active = touch_down && screen == SC_PLAYING && !radio_mode &&
                           !qs_open && queue_n > 0 &&
                           touch_y > bar_y() - 26 && touch_y < bar_y() + 26;
            if (scrub_active || was) { dirty = 1; idle = 0; }
        }

        /* Same shape as scrubbing, for the EQ sliders: which one (if any) the
         * finger is over this tick, so the draw code can show it tracking
         * live_x instead of the stored value. The actual write happens on
         * release, which arrives as a tap at the final position and is
         * handled with the other taps -- nothing new needed for that part. */
        {
            int was = eq_dragging;
            eq_dragging = 0;
            if (touch_down) {
                if (screen == SC_EQ) {
                    int pby = eq_preamp_y() + 26;
                    if (touch_y > pby - 20 && touch_y < pby + 20) eq_dragging = 1;
                } else if (screen == SC_EQ_BAND) {
                    int fy = eq_freq_y(), gy = eq_gain_y(), qy = eq_q_y();
                    if      (touch_y > fy - 20 && touch_y < fy + 20) eq_dragging = 2;
                    else if (touch_y > gy - 20 && touch_y < gy + 20) eq_dragging = 3;
                    else if (touch_y > qy - 20 && touch_y < qy + 20) eq_dragging = 4;
                }
            }
            if (eq_dragging || was) { dirty = 1; idle = 0; }
        }

        {
            int was = mseb_dragging;
            mseb_dragging = -1;
            if (touch_down && screen == SC_MSEB) {
                for (int i = 0; i < MSEB_BAND_N; i++) {
                    int sy = mseb_band_row_y(i) + 46;
                    if (touch_y > sy - 20 && touch_y < sy + 20) { mseb_dragging = i; break; }
                }
            }
            if (mseb_dragging >= 0 || was >= 0) { dirty = 1; idle = 0; }
        }

        /* Press-and-hold on a track: fires under the finger, once. */
        /* Not on a chapter list: "play next" and "add to queue" are about a
         * queue of tracks, and a book's chapters are not that — acting on
         * them would rewrite the chapter table out from under the player. */
        if (touch_down && !touch_moved && !edge_active && !hold_fired && !sheet_open &&
            screen == SC_TRACKS && !ab_list &&
            touch_y >= (pod_list ? CONTENT_Y : 0)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long held = (now.tv_sec - touch_at.tv_sec) * 1000L +
                        (now.tv_nsec - touch_at.tv_nsec) / 1000000L;
            if (held >= HOLD_MS) {
                /* R46: plain albums address a row via the header-relative
                 * off/header_h math (see the draw/tap-handling code above),
                 * not scroll+((touch_y-CONTENT_Y)/ROW_H) -- that assumed
                 * content starting at CONTENT_Y, which is only still true
                 * for pod_list's unchanged layout.
                 *
                 * BUG fix: this used to redo that header-relative math by
                 * hand -- (content_y - header_h) / ROW_H -- rather than
                 * calling track_index_at(), which R56 introduced specifically
                 * so a row's position never has to be recomputed a second,
                 * independent way (see that function's own comment: BG2/
                 * BG61-class bugs are exactly two copies of the same
                 * arithmetic drifting apart). This plain division predates
                 * disc banners entirely and was never updated for them, so a
                 * long-press on a genuine multi-disc album resolved to the
                 * wrong track by however many banner-heights preceded it --
                 * plain tap-to-play (already routed through track_index_at())
                 * was unaffected, which is why only the press-and-hold sheet
                 * (Play Next / Add to Queue) ever acted on the wrong song.
                 * Reported live on "Substance 1987": holding a Disc 2 track
                 * opened the sheet for a different track a couple of rows
                 * off. */
                int idx;
                if (pod_list) {
                    idx = scroll + (touch_y - CONTENT_Y) / ROW_H;
                } else {
                    int off = scroll * ROW_H + scroll_px;
                    int content_y = touch_y + off;
                    /* Held on the cover itself rather than a track row:
                     * refresh it. Same gesture the rows use, told apart
                     * purely by where it landed -- the artwork occupies
                     * content rows 0..ART_PX, above every track row, and
                     * g_view_art_gone_frame is exactly the flag the draw
                     * side uses to know whether it drew a cover there at
                     * all (nothing to refresh if it didn't). */
                    if (!g_view_art_gone_frame && content_y >= 0 && content_y < ART_PX &&
                        !browsing_is_playlist && track_n > 0) {
                        hold_fired = 1;
                        art_refresh_request(tracks[0].path, cur_artist, cur_album, cur_artist, 1);
                        snprintf(sheet_note, sizeof(sheet_note), "Refreshing artwork");
                        dirty = 1; idle = 0;
                        continue;
                    }
                    idx = track_index_at(content_y);
                }
                hold_fired = 1;
                if (idx >= 0 && idx < track_n) {
                    sheet_open = 1;
                    sheet_track = idx;
                    dirty = 1; idle = 0;
                }
            }
        }

        /* R74: press-and-hold on a playlist -- Rename/Delete. Row 0 is
         * "New Playlist" (R71), not a real entry, so it's excluded the same
         * way the tap handler already excludes it from playlists[]. Flat
         * CONTENT_Y/ROW_H math, same as this screen's own tap dispatch --
         * SC_PLAYLISTS has no header/disc-banner concerns track_index_at()
         * exists for. */
        if (touch_down && !touch_moved && !edge_active && !hold_fired && !sheet_open &&
            screen == SC_PLAYLISTS) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long held = (now.tv_sec - touch_at.tv_sec) * 1000L +
                        (now.tv_nsec - touch_at.tv_nsec) / 1000000L;
            if (held >= HOLD_MS) {
                int idx = scroll + (touch_y - CONTENT_Y) / ROW_H;
                hold_fired = 1;
                if (idx >= 1 && idx - 1 < playlist_n) {
                    sheet_open = 4;
                    sheet_playlist = idx - 1;
                    dirty = 1; idle = 0;
                }
            }
        }

        /* R59: holding a skip arc repeats it instead of firing once on
         * release, escalating the per-repeat amount the longer it's held
         * -- see skip_hold_amount()'s own comment for the schedule. Same
         * touch_at-relative timing idiom as the long-press sheet just
         * above, but this one keeps firing past the first hit (that block
         * stops itself via !hold_fired; this one doesn't gate on it, since
         * hold_fired here is something *this* block sets, to suppress the
         * release-tap handler once the hold's already acted -- not
         * something it waits on). */
        if (touch_down && !touch_moved && !edge_active && !sheet_open &&
            screen == SC_PLAYING && (audiobook_mode || podcast_mode)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long held = (now.tv_sec - touch_at.tv_sec) * 1000L +
                        (now.tv_nsec - touch_at.tv_nsec) / 1000000L;
            if (skip_hold_dir == 0) {
                if (held >= HOLD_MS) {
                    int dir = skip_zone_at(touch_x, touch_y);
                    if (dir != 0) {
                        skip_hold_dir = dir;
                        audio_seek_ms(audio_pos_ms() + dir * skip_hold_amount(held));
                        seek_toast(dir * skip_hold_amount(held));
                        hold_fired = 1;   /* the eventual release is not a plain tap any more */
                        skip_hold_next_ms = held + SKIP_REPEAT_MS;
                        dirty = 1; idle = 0;
                    }
                }
            } else if (held >= skip_hold_next_ms) {
                audio_seek_ms(audio_pos_ms() + skip_hold_dir * skip_hold_amount(held));
                seek_toast(skip_hold_dir * skip_hold_amount(held));
                skip_hold_next_ms = held + SKIP_REPEAT_MS;
                dirty = 1; idle = 0;
            }
        }

        /* Holding the Wi-Fi/Bluetooth row in quick settings opens that
         * radio's own settings screen instead of just toggling it --
         * same touch_at-relative hold idiom as the two blocks just above,
         * fires once via hold_fired the same way the track-list sheet's
         * own long-press does (not a repeater like R59's skip-hold). qs_open
         * has to close itself here: the settings screens underneath aren't
         * drawn while the quick-settings panel is up. */
        if (touch_down && !touch_moved && !edge_active && !hold_fired && qs_open) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long held = (now.tv_sec - touch_at.tv_sec) * 1000L +
                        (now.tv_nsec - touch_at.tv_nsec) / 1000000L;
            if (held >= HOLD_MS) {
                screen_t target = SC_MENU;   /* sentinel: no row held */
                if (touch_y > qs_wifi_y() && touch_y < qs_wifi_y() + QS_ROW_H)
                    target = SC_SETTINGS_WIFI;
                else if (touch_y > qs_bt_y() && touch_y < qs_bt_y() + QS_ROW_H)
                    target = SC_SETTINGS_BT;
                if (target != SC_MENU) {
                    hold_fired = 1;
                    qs_open = 0;
                    screen = target; reset_scroll();
                    dirty = 1; idle = 0;
                }
            }
        }

        if (g) dirty = 1;
        {   /* the hint has to follow the finger, so redraw while it is up */
            static int hint_was;
            if (edge_active || hint_was) dirty = 1;
            hint_was = edge_active;
        }
        {   /* R80: same, for the home-swipe hint */
            static int home_hint_was;
            if (home_edge_active || home_hint_was) dirty = 1;
            home_hint_was = home_edge_active;
        }
        {
            int sec = audio_is_active() ? audio_pos_ms() / 1000 : -1;
            if (sec != last_sec) { last_sec = sec; dirty = 1; }
            int seq = art_seq();
            if (seq != art_seen) { art_seen = seq; dirty = 1; }
            /* Bridge splash still up from resume: hold it, not the real
             * (still cover-less) screen, until the restored track's art has
             * actually finished loading -- ~90 ticks (3s at 30Hz) is a
             * belt-and-braces cap, not the expected case; art_worker()
             * always sets art_done whether it found something or gave up,
             * so this is only reached if that never runs at all. */
            if (resume_wait_art) {
                if (art_done || ++resume_wait_ticks > 90) {
                    resume_wait_art = 0;
                    /* The bridge splash's own replacement, faded in rather
                     * than swapped -- reported live as "a bit rough" done
                     * as a hard cut. Rendered into a scratch buffer first
                     * (draw_ui() takes any FB_W*FB_H buffer, not only an
                     * on-screen page) so resume_fade_to() has a finished
                     * target to blend toward from the very first step. */
                    uint16_t *real = malloc(page_px * sizeof(uint16_t));
                    if (real) {
                        draw_ui(real);
                        resume_fade_to(base, page_px, fbfd, &v, &page, real);
                        free(real);
                        dirty = 0;
                    }
                    /* No memory for the blend buffer: leave dirty as it is
                     * (art finishing already set it above) and let the
                     * ordinary path below paint the real screen as a hard
                     * cut rather than not painting it at all. */
                }
            }
            /* BG70: the album-detail screen's own cover fetch, independent
             * of the playing track's -- needs its own redraw trigger the
             * same way, or a slow network fetch would only ever show up
             * whenever something else happened to mark the screen dirty. */
            int vseq = view_art_seq();
            if (vseq != view_art_seen) { view_art_seen = vseq; dirty = 1; }
            /* The status bar changes on its own — battery drains, the jack is
             * pulled — so it gets a slow tick of its own rather than relying
             * on the clock, which only runs during playback. */
            if (++status_tick >= 60) {
                status_tick = 0;
                if (qs_open) qs_refresh();      /* the radios take a moment */
                /* Same reasoning as BG6's standby-undo below: whatever turns
                 * this LED on isn't this app, so a one-shot fix doesn't rule
                 * out it happening again later. Only while unlocked --
                 * led_pulse_step() below already reasserts far more often
                 * than this ~2s tick while locked, so this would otherwise
                 * fight the pulse with a periodic flat value. */
                if (!button_locked) { write_int_file(LED_BLUE, 0); write_int_file(LED_RED, 0); }
                /* R45: rides this same ~2s tick rather than adding a new
                 * one -- cheap enough (a handful of trig calls) to just redo
                 * every time it fires, and only reached at all in Auto mode.
                 * dirty=1 below already covers redrawing once this changes
                 * the six colour variables. */
                if (theme_mode == THEME_AUTO) {
                    static int was_effective = -1;
                    int effective = auto_theme_effective();
                    if (effective != was_effective) { was_effective = effective; apply_theme(); }
                }
                dirty = 1;
            }
        }
        if (button_locked) led_pulse_step();

        if (!locked && idle_ticks > 0 && ++idle >= idle_ticks) set_locked(1);

        /* Low-power idle. Counted in loop ticks, and while locked the loop runs
         * at 10 Hz, which is the only rate that can ever reach the threshold --
         * the count resets the instant the device is unlocked or something
         * starts playing, so it can only ever accumulate at the locked rate. */
        /* "Nothing playing" has to mean *playing*, not "has a track loaded".
         * audio_is_active() stays true across a pause -- g_active is only
         * cleared when the worker exits -- so gating on it alone meant a book
         * left paused, which is precisely the case that flattened the battery
         * overnight, never reached the idle path at all. Paused is not
         * playing: the decoder is parked, the PCM has already been handed
         * back, and nothing is going to the amp. */
        /* R81: a headset connected while this was idle. Only unpauses a track
         * that is already loaded -- there is no persisted "last played track"
         * for music (audiobooks and podcasts keep per-file positions; music
         * keeps neither a queue nor a last track), so after a cold boot there
         * is genuinely nothing to start. See R82. */
        if (bt_autoplay_enabled) {
            pthread_mutex_lock(&bt_lock);
            int want = bt_autoplay_pending;
            bt_autoplay_pending = 0;
            pthread_mutex_unlock(&bt_lock);
            if (want && audio_is_active() && audio_is_paused()) {
                audio_toggle();
                mlog("[music] R81: headset connected, resuming playback\n");
                dirty = 1;
            }
        }

        int playing = audio_is_active() && !audio_is_paused();
        if (locked && !playing && sleep_minutes() > 0 && suspend_ok()) {
            if (++sleep_idle >= sleep_minutes() * 60 * 10) {
                deep_suspend();
                /* Resumed. Start the whole timeout again rather than suspending
                 * on the very next tick: the press that woke the device is a
                 * single press, which button_locked swallows, so an immediate
                 * re-suspend would make the thing effectively impossible to
                 * wake. A full idle period guarantees a window to get back in. */
                sleep_idle = 0;
            }
        } else {
            sleep_idle = 0;
        }

        /* Auto shutdown. Same gate and same tick-counted shape as the idle
         * timer above, but its own independent counter and its own setting --
         * a poweroff is not a suspend, does not care whether deep sleep is
         * enabled, and there is nothing to resume into afterwards, so unlike
         * sleep_idle there is no reset-and-repeat: this fires once. */
        if (locked && !playing && auto_off_minutes() > 0) {
            if (++auto_off_idle >= auto_off_minutes() * 60 * 10) {
                mlog("[music] auto shutdown after %d min idle\n", auto_off_minutes());
                /* Explicit rather than relying on the exit path below: there is
                 * no SIGTERM handler here, so whatever poweroff's shutdown
                 * sequence sends is not guaranteed to reach it, and the last
                 * periodic autosave could be up to 15s stale. This is exactly
                 * the position a resumed-from-suspend book needs to still be
                 * right, and it is nearly free to just save it fresh. */
                ab_save_current_pos();
                /* Same reasoning as the line above, for the other half of
                 * the pair -- an episode's own resume file is just as stale
                 * as a book's by up to the same 15s here, and R66's restore
                 * reads exactly that file back to place the episode. */
                pod_save_current_pos();
                /* R66. Either page holds the last drawn frame: nothing has
                 * been drawn since the screen locked (`if (locked) dirty = 0`),
                 * and the mirror below the draw block keeps both identical
                 * whenever a drag isn't in progress, which locked guarantees. */
                resume_save(base + (size_t)(page ^ 1) * page_px);
                if (system("/sbin/poweroff") == -1) { }
                /* poweroff is not instant; keep the loop from re-firing this
                 * every tick while the shutdown sequence runs. */
                auto_off_idle = 0;
            }
        } else {
            auto_off_idle = 0;
        }

        /* BG6: undo the stock player's standby blanking.
         *
         * hiby_player runs its own standby timer, and while this app is open
         * that timer can never reset: EVIOCGRAB gives us *exclusive* input,
         * so the player sees no touches and no keys at all, however busy the
         * user actually is, and eventually blanks the panel out from under
         * us. No HiBy setting stops it, because from its point of view the
         * device really has been idle the whole time. It is not observable
         * as an event either — it is their code path on their timer, which
         * is why nothing appears in this log before the screen goes dark.
         *
         * So watch the backlight instead of trying to intercept the cause:
         * dark while we did not lock it means something else did. Restoring
         * needs the unblank as well as the brightness, since the player may
         * also have powered the framebuffer down (see set_locked). */
        if (++blank_tick >= 15) {                 /* ~0.5 s */
            blank_tick = 0;
            int b = read_int_file(BACKLIGHT);
            if (b > 0) {
                last_lit_bright = b;              /* track what the user chose */
            } else if (!locked) {
                mlog("[music] panel blanked by the player; restoring\n");
                if (g_fbfd >= 0) ioctl(g_fbfd, FBIOBLANK, FB_BLANK_UNBLANK);
                /* Through st_brightness_set(), same reason as set_locked():
                 * a cached last_lit_bright at the literal max would just
                 * write the same value that goes dark. */
                st_brightness_set(last_lit_bright > 0 ? last_lit_bright : DEFAULT_BRIGHTNESS);
            }
        }

        /* Nothing is drawn while the panel is dark; the frame is produced on
         * wake instead. */
        if (locked) dirty = 0;

        /* While a finger is down on the scrub bar or the volume slider, repaint
         * only that part, straight into the page already on screen. A full
         * frame is a screen clear, a cover blit and a page flip; these two are
         * a few tens of kilobytes. */
        /* Volume only. The same trick on the scrub bar left a grey track with
         * no fill: the strip function does not reproduce the state the full
         * draw sets up, so the duration resolved to zero and the filled part
         * was skipped. A bar that will not move is worse than one that moves
         * heavily, so scrubbing keeps the full redraw. */
        if (dirty && vol_dragging && !qs_open && frames > 0) {
            draw_volume(base + (size_t)(page ^ 1) * page_px);
            dirty = 0;
        }

        if (dirty && !resume_wait_art) {
            /* BG98 follow-up: how long between set_locked(0) returning (in
             * handle_keys(), possibly several hundred lines and one loop
             * lap's worth of other per-tick work ago) and actually reaching
             * the draw call -- the middle third of the breakdown. */
            if (g_wake_t0) g_wake_predraw_us = us_now() - g_wake_t0;
            /* Draw into the page that is not on screen, then flip to it.
             * Painting the visible page instead — briefly tried, to make the
             * panel and a screenshot agree — clears the whole frame before
             * repainting it, and the cover visibly flickered once a second as
             * the clock ticked. */
            draw_ui(base + (size_t)page * page_px);
            v.yoffset = (uint32_t)(page * FB_H);
            if (ioctl(fbfd, FBIOPAN_DISPLAY, &v) < 0 && frames < 3)
                mlog("[music] pan failed: %s\n", strerror(errno));
            /* BG98: this is the first real frame reaching the panel since
             * g_wake_t0 was set (this pass through the loop is the same one
             * that read the wake key -- see handle_keys()'s own comment for
             * why `dirty` survives from there to here without needing
             * another lap). Logged unconditionally rather than only past a
             * threshold: infrequent enough (one press at a time, human-
             * paced) that seeing the normal case costs nothing and is worth
             * having for comparison against a slow one. Broken into three:
             * set_locked(0) itself (FBIOBLANK unblank + brightness restore),
             * everything from there to the draw_ui() call (other per-tick
             * work this same lap), and draw_ui()+pan (the remainder) --
             * whichever of the three actually accounts for the total is the
             * one worth chasing further. */
            if (g_wake_t0) {
                uint64_t total = us_now() - g_wake_t0;
                mlog("[music] wake latency %lu us (set_locked %lu, predraw %lu, draw+pan %lu)\n",
                     (unsigned long)total, (unsigned long)g_wake_setlocked_us,
                     (unsigned long)(g_wake_predraw_us - g_wake_setlocked_us),
                     (unsigned long)(total - g_wake_predraw_us));
                g_wake_t0 = 0;
            }
            /* Copy the finished frame to the other page as well. /dev/fb0
             * reports a single page to read(), so anything screenshotting the
             * device silently gets whichever page is not on screen; keeping
             * both identical means it gets the right picture either way.
             *
             * Not while a finger is dragging, though: it is three quarters of
             * a megabyte per frame on top of the repaint, and at thirty frames
             * a second that is what makes a drag feel like treacle. Nobody is
             * screenshotting mid-gesture. */
            /* Only where it paid for itself. Skipping the mirror during a
             * scrub also made screenshots read the stale spare page, which
             * cost an hour chasing a bar that was moving on the device and
             * not in the capture. */
            /* list_dragging/inertia_active belong here too: same 768 KB
             * mirror, paid every ~33ms tick throughout a scroll, and nobody
             * is screenshotting mid-drag any more here than during a volume
             * or quick-settings drag. */
            int dragging = vol_dragging || qs_dragging || list_dragging || inertia_active;
            if (!dragging)
                memcpy(base + (size_t)(page ^ 1) * page_px,
                       base + (size_t)page * page_px, page_px * 2);
            page ^= 1;
            frames++;
            dirty = 0;
        }
        /* 30 Hz is a frame rate, and with the panel dark there are no frames:
         * `if (locked) dirty = 0` above has already thrown the drawing away, so
         * what is left is polling. Slowing it to 10 Hz while locked cuts two
         * thirds of the wakeups out of the longest-running state this device
         * has -- sitting in a pocket -- and costs nothing visible, because the
         * only thing that can happen is a key press, input events queue in the
         * kernel until they are read, and the double-press window is measured
         * from the kernel's own timestamps rather than from when this loop
         * happened to look (see handle_keys). */
        usleep(locked ? 100000 : 33000);
    }

    ab_save_current_pos();     /* the periodic tick may be up to 15s stale */

    if (snapshot) {
        memcpy(base, snapshot, page_px * 2);
        memcpy(base + page_px, snapshot, page_px * 2);
        free(snapshot);
    }
    v.yoffset = 0;
    ioctl(fbfd, FBIOPAN_DISPLAY, &v);
    munmap(base, map_len);
    if (tfd >= 0) { ioctl(tfd, EVIOCGRAB, 0); close(tfd); }
    for (int i = 0; i < kfd_n; i++)
        if (kfd[i] >= 0) { ioctl(kfd[i], EVIOCGRAB, 0); close(kfd[i]); }
    kfd_n = 0;
    bt_poll_run = 0;
    if (bt_thread_valid) { pthread_join(bt_thread, NULL); bt_thread_valid = 0; }
    set_locked(0);                     /* never hand the panel back dark */
    close(fbfd);
    audio_stop();
    lib_close();
    mlog("[music] leaving app after %d frames\n", frames);
    return 0;
}

/* ---- tile name and icon -------------------------------------------------- */
/* The tile this app takes over is still labelled "About" with an info-circle
 * icon. The rootfs is read-only squashfs, so a bind mount is the only way to
 * change either short of reflashing, and it has to happen in this constructor
 * rather than from a boot script: the player reads its string table during
 * startup and a backgrounded script loses that race.
 *
 * Launcher tile labels live in settings.ini (music / net_set / sys_set /
 * about), not launcher.ini — that one drives a different menu, which is why
 * editing its <abo_dev> had no effect (found while this hook still lived on
 * the Stream media tile, whose own label is in sys_set.ini instead).
 */
#define RES_DIR    "/usr/data/music_res"
#define LABEL_INI  "/tmp/.music_settings.ini"
#define TILE_LABEL "Libra"

/* The ini is UTF-16LE, so tags have to be matched widened. */
static size_t widen(const char *s, uint8_t *out) {
    size_t n = 0;
    for (; *s; s++) { out[n++] = (uint8_t)*s; out[n++] = 0; }
    return n;
}

static const uint8_t *memfind(const uint8_t *hay, size_t hn,
                              const uint8_t *needle, size_t nn) {
    if (nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, needle, nn) == 0) return hay + i;
    return NULL;
}

static const char *make_label_ini(void) {
    static const char SRC[] = "/usr/resource/str/english/settings.ini";
    int fd = open(SRC, O_RDONLY);
    if (fd < 0) return NULL;

    static uint8_t buf[16384];
    ssize_t len = read(fd, buf, sizeof(buf));
    close(fd);
    /* A full buffer means the file was truncated, and writing half a string
     * table would cost every label on the screen, not just this one. */
    if (len <= 0 || (size_t)len == sizeof(buf)) return NULL;

    uint8_t otag[48], ctag[48], label[64];
    size_t on = widen("<about>", otag);
    size_t cn = widen("</about>", ctag);
    size_t ln = widen(TILE_LABEL, label);

    const uint8_t *a = memfind(buf, (size_t)len, otag, on);
    if (!a) return NULL;
    const uint8_t *b = memfind(a, (size_t)len - (size_t)(a - buf), ctag, cn);
    if (!b) return NULL;

    size_t head = (size_t)(a - buf) + on;
    size_t tail = (size_t)len - (size_t)(b - buf);

    fd = open(LABEL_INI, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;
    int ok = write(fd, buf, head) == (ssize_t)head &&
             write(fd, label, ln) == (ssize_t)ln   &&
             write(fd, b, tail)   == (ssize_t)tail;
    close(fd);
    if (!ok) { unlink(LABEL_INI); return NULL; }
    return LABEL_INI;
}

static void shadow_resources(void) {
    static const char *pairs[][2] = {
        { RES_DIR "/about.png",   "/usr/resource/litegui/theme1/launcher/about.png" },
        { RES_DIR "/about_s.png", "/usr/resource/litegui/theme1/launcher/about_s.png" },
        { RES_DIR "/about.png",   "/usr/resource/litegui/theme2/launcher/about.png" },
        { RES_DIR "/about_s.png", "/usr/resource/litegui/theme2/launcher/about_s.png" },
    };
    for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (access(pairs[i][0], R_OK) != 0) continue;   /* no icon shipped: keep the stock one */
        if (mount(pairs[i][0], pairs[i][1], NULL, MS_BIND, NULL) != 0)
            mlog("[music] bind %s failed: %s\n", pairs[i][1], strerror(errno));
    }

    const char *ini = make_label_ini();
    if (!ini)
        mlog("[music] label rewrite failed; tile keeps its stock name\n");
    else if (mount(ini, "/usr/resource/str/english/settings.ini", NULL, MS_BIND, NULL) != 0)
        mlog("[music] bind settings.ini failed: %s\n", strerror(errno));
}

/* ---- install ------------------------------------------------------------- */
static void build_trampoline(uint32_t target, uint32_t *out) {
    out[0] = 0x3C190000u | ((target + 0x8000) >> 16);  /* lui   t9, hi     */
    out[1] = 0x27390000u | (target & 0xFFFF);          /* addiu t9, t9, lo */
    out[2] = 0x03200008u;                              /* jr    t9         */
    out[3] = 0x00000000u;                              /* nop              */
}

#ifndef __NR_cacheflush
#define __NR_cacheflush 4147
#endif
#define BCACHE_FLAG 3

static int is_hiby_player(void) {
    char buf[128];
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return strstr(buf, "hiby") != NULL || strstr(buf, "system_main") != NULL;
}

__attribute__((constructor))
static void music_init(void) {
    if (!is_hiby_player()) return;
    mlog("[music] init pid=%d entry=%p\n", (int)getpid(), &music_entry);
    srand((unsigned)time(NULL));   /* R47: shuffle_regenerate()'s rand() */

    /* Manual only, on request -- an earlier auto-start here (regardless of
     * the tile hijack below succeeding) made the whole HiBy launcher UI
     * sluggish at boot, competing with everything else initialising at the
     * same time. scanner_rescan_now() (Settings' "Scan library" row, which
     * kicks index_rescan_now() alongside it -- see scanner.c's own comment)
     * is the only way this thread starts now. */

    volatile uint32_t *cave = (volatile uint32_t *)CAVE_ADDR;
    if (cave[0] != 0 || cave[1] != 0) { mlog("[music] cave occupied\n"); return; }

    if (mprotect((void *)CAVE_PAGE, PAGE_SPAN,
                 PROT_READ | PROT_WRITE | PROT_EXEC) < 0) {
        mlog("[music] cave mprotect failed\n");
        return;
    }
    uint32_t t[4];
    build_trampoline((uint32_t)&music_entry, t);
    cave[0] = t[0]; cave[1] = t[1]; cave[2] = t[2]; cave[3] = t[3];
    __asm__ __volatile__("sync" ::: "memory");
    syscall(__NR_cacheflush, (void *)CAVE_ADDR, 16, BCACHE_FLAG);
    mprotect((void *)CAVE_PAGE, PAGE_SPAN, PROT_READ | PROT_EXEC);

    /* The tile shows up at two callback addresses in hiby_player's data;
     * both get patched so it's caught wherever the launcher happens to read
     * it from that run (see podcast_hook.c's own history of this same
     * tile, which this hook inherited). Only the second is checked against
     * its known stock value before writing -- if that one has already
     * drifted from what this build expects, the first is left alone too
     * rather than guessing. */
    volatile uint32_t *cb1 = (volatile uint32_t *)ABOUT_CB_1;
    volatile uint32_t *cb2 = (volatile uint32_t *)ABOUT_CB_2;
    if (*cb2 != ABOUT_CB_ORIG) {
        mlog("[music] unexpected About callback 0x%08X, leaving it alone\n", *cb2);
        return;
    }
    if (mprotect((void *)DATA_PAGE, PAGE_SPAN, PROT_READ | PROT_WRITE) < 0) {
        mlog("[music] data mprotect failed\n");
        return;
    }
    orig_cb = *cb2;
    *cb2 = CAVE_ADDR;
    if (*cb1 == ABOUT_CB_ORIG) *cb1 = CAVE_ADDR;
    __asm__ __volatile__("sync" ::: "memory");
    /* If the player died while the screen was locked, the backlight was left
     * at 0 and nothing ever put it back: the device looks dead, and the only
     * way out is a reboot. Whatever happened last time, start lit. */
    if (read_int_file(BACKLIGHT) <= 0) {
        write_int_file(BACKLIGHT, DEFAULT_BRIGHTNESS);
        mlog("[music] backlight was off at startup; restored\n");
    }
    /* Stock firmware turns the blue one on before this constructor ever
     * runs, with no script found responsible for it -- off at startup
     * (button_locked can't be true this early). Both forced to manual mode
     * first: this session found the kernel's own "breathing" trigger a
     * no-op on this board, but nothing rules out something else along the
     * way leaving a trigger other than none selected, which would ignore
     * these raw brightness writes entirely. */
    write_text_file(LED_BLUE_TRIGGER, "none");
    write_text_file(LED_RED_TRIGGER, "none");
    write_int_file(LED_BLUE, 0);
    write_int_file(LED_RED, 0);

    shadow_resources();
    mlog("[music] About tile armed -> 0x%08X\n", CAVE_ADDR);
}
