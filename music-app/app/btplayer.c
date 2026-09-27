/* btplayer.c — see btplayer.h.
 *
 * The BlueZ side (profiles/audio/media.c, 5.54 on this firmware): a player
 * registered with org.bluez.Media1.RegisterPlayer is handed every AVRCP
 * session. A headset's PLAY/PAUSE/STOP/FORWARD/BACKWARD then become calls to
 * the player's Play/Pause/Stop/Next/Previous -- but only if it has said it can
 * (CanPlay, CanPause, CanGoNext, CanGoPrevious, CanControl); otherwise BlueZ
 * falls back to the key events it always sent. Status and track reach the
 * headset through the PropertiesChanged signals BlueZ watches for: a
 * PlaybackStatus change is sent on as a play-status notification, a Metadata
 * change as a track change. Volume and seek keys stay key events either way.
 *
 * Position is deliberately never sent. BlueZ keeps its own clock from the
 * last status change, which is all a headset needs, and a Position update
 * goes out as a "seeking" play status -- which would leave the headset
 * believing nothing is playing, the very thing this exists to prevent.
 *
 * The D-Bus policy BlueZ ships (etc/dbus-1/system.d/bluetooth.conf, confirmed
 * in the firmware) already lets bluetoothd call org.mpris.MediaPlayer2.Player
 * and org.freedesktop.DBus.Properties on another connection, which is what
 * delivers those calls here. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "btplayer.h"

#define PLAYER_PATH  "/org/libra/player"
#define ADAPTER_PATH "/org/bluez/hci0"
#define BLUEZ        "org.bluez"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"
#define PROPS_IFACE  "org.freedesktop.DBus.Properties"

static void (*g_log)(const char *fmt, ...);
#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

/* ---- libdbus, resolved at run time --------------------------------------
 * libdbus-1.so.3 is on the firmware (bluetoothd, bluealsa and wpa_supplicant
 * all use it) but its headers are not in this tree, so the calls this needs
 * are looked up with dlsym() against the declarations below: the library's
 * public ABI, stable since 1.0. */
typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
typedef uint32_t dbus_bool_t;
/* The caller allocates these and the library fills them in. The real
 * iterator is 56 bytes on this 32-bit target and the error a little over 16;
 * both are declared far larger here, so a miscounted field can never become
 * a stack overrun. Only DBusError's first two fields are read. */
typedef struct { void *opaque[32]; } DBusMessageIter;
typedef struct { const char *name; const char *message; void *opaque[8]; } DBusError;

#define BUS_SYSTEM      1
#define MSG_METHOD_CALL 1
#define MSG_SIGNAL      4

static struct {
    DBusConnection *(*bus_get_private)(int, DBusError *);
    const char *(*bus_get_unique_name)(DBusConnection *);
    void (*bus_add_match)(DBusConnection *, const char *, DBusError *);
    void (*set_exit_on_disconnect)(DBusConnection *, dbus_bool_t);
    dbus_bool_t (*get_unix_fd)(DBusConnection *, int *);
    dbus_bool_t (*read_write)(DBusConnection *, int);
    DBusMessage *(*pop_message)(DBusConnection *);
    dbus_bool_t (*send)(DBusConnection *, DBusMessage *, uint32_t *);
    DBusMessage *(*send_blocking)(DBusConnection *, DBusMessage *, int, DBusError *);
    void (*flush)(DBusConnection *);
    void (*close)(DBusConnection *);
    void (*conn_unref)(DBusConnection *);
    DBusMessage *(*new_method_call)(const char *, const char *, const char *, const char *);
    DBusMessage *(*new_method_return)(DBusMessage *);
    DBusMessage *(*new_error)(DBusMessage *, const char *, const char *);
    DBusMessage *(*new_signal)(const char *, const char *, const char *);
    void (*msg_unref)(DBusMessage *);
    int (*get_type)(DBusMessage *);
    const char *(*get_path)(DBusMessage *);
    const char *(*get_interface)(DBusMessage *);
    const char *(*get_member)(DBusMessage *);
    const char *(*get_sender)(DBusMessage *);
    dbus_bool_t (*get_no_reply)(DBusMessage *);
    dbus_bool_t (*is_signal)(DBusMessage *, const char *, const char *);
    void (*iter_init_append)(DBusMessage *, DBusMessageIter *);
    dbus_bool_t (*iter_open)(DBusMessageIter *, int, const char *, DBusMessageIter *);
    dbus_bool_t (*iter_close)(DBusMessageIter *, DBusMessageIter *);
    dbus_bool_t (*iter_append)(DBusMessageIter *, int, const void *);
    dbus_bool_t (*iter_init)(DBusMessage *, DBusMessageIter *);
    int (*iter_type)(DBusMessageIter *);
    void (*iter_get)(DBusMessageIter *, void *);
    dbus_bool_t (*iter_next)(DBusMessageIter *);
    void (*error_init)(DBusError *);
    void (*error_free)(DBusError *);
    dbus_bool_t (*threads_init_default)(void);
} dl;

static int dl_load(void) {
    /* libdbus aborts the whole process on a failed argument check unless told
     * otherwise -- a Libra crash over a malformed tag. The strings sent are
     * made valid first (utf8_copy()), but a warning is the right failure for
     * anything missed, not an abort. Read by the library on first use. */
    setenv("DBUS_FATAL_WARNINGS", "0", 1);
    void *h = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libdbus-1.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOG("[btplayer] no libdbus (%s); headset keys only\n", dlerror()); return 0; }
    static const struct { void **slot; const char *name; } syms[] = {
        { (void **)&dl.bus_get_private,        "dbus_bus_get_private" },
        { (void **)&dl.bus_get_unique_name,    "dbus_bus_get_unique_name" },
        { (void **)&dl.bus_add_match,          "dbus_bus_add_match" },
        { (void **)&dl.set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect" },
        { (void **)&dl.get_unix_fd,            "dbus_connection_get_unix_fd" },
        { (void **)&dl.read_write,             "dbus_connection_read_write" },
        { (void **)&dl.pop_message,            "dbus_connection_pop_message" },
        { (void **)&dl.send,                   "dbus_connection_send" },
        { (void **)&dl.send_blocking,          "dbus_connection_send_with_reply_and_block" },
        { (void **)&dl.flush,                  "dbus_connection_flush" },
        { (void **)&dl.close,                  "dbus_connection_close" },
        { (void **)&dl.conn_unref,             "dbus_connection_unref" },
        { (void **)&dl.new_method_call,        "dbus_message_new_method_call" },
        { (void **)&dl.new_method_return,      "dbus_message_new_method_return" },
        { (void **)&dl.new_error,              "dbus_message_new_error" },
        { (void **)&dl.new_signal,             "dbus_message_new_signal" },
        { (void **)&dl.msg_unref,              "dbus_message_unref" },
        { (void **)&dl.get_type,               "dbus_message_get_type" },
        { (void **)&dl.get_path,               "dbus_message_get_path" },
        { (void **)&dl.get_interface,          "dbus_message_get_interface" },
        { (void **)&dl.get_member,             "dbus_message_get_member" },
        { (void **)&dl.get_sender,             "dbus_message_get_sender" },
        { (void **)&dl.get_no_reply,           "dbus_message_get_no_reply" },
        { (void **)&dl.is_signal,              "dbus_message_is_signal" },
        { (void **)&dl.iter_init_append,       "dbus_message_iter_init_append" },
        { (void **)&dl.iter_open,              "dbus_message_iter_open_container" },
        { (void **)&dl.iter_close,             "dbus_message_iter_close_container" },
        { (void **)&dl.iter_append,            "dbus_message_iter_append_basic" },
        { (void **)&dl.iter_init,              "dbus_message_iter_init" },
        { (void **)&dl.iter_type,              "dbus_message_iter_get_arg_type" },
        { (void **)&dl.iter_get,               "dbus_message_iter_get_basic" },
        { (void **)&dl.iter_next,              "dbus_message_iter_next" },
        { (void **)&dl.error_init,             "dbus_error_init" },
        { (void **)&dl.error_free,             "dbus_error_free" },
        { (void **)&dl.threads_init_default,   "dbus_threads_init_default" },
    };
    for (unsigned i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        *syms[i].slot = dlsym(h, syms[i].name);
        if (!*syms[i].slot) {
            LOG("[btplayer] libdbus has no %s; headset keys only\n", syms[i].name);
            return 0;
        }
    }
    return 1;
}

/* ---- what is playing, shared with the UI thread ------------------------- */
typedef struct {
    int     status;
    char    title[256], artist[256], album[256];
    int64_t length_ms;
    int     track_no;
} snap_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static snap_t   g_cur;                     /* under g_lock */
static unsigned g_meta_gen, g_status_gen;  /* under g_lock; bumped per change */
static int      g_started;
static int      g_wake[2] = { -1, -1 };    /* nudges the D-Bus thread on a change */

#define CMD_MAX 8
static int g_cmd[CMD_MAX], g_cmd_head, g_cmd_n;   /* under g_lock */

/* D-Bus refuses a string that is not valid UTF-8, and tags are not always
 * that -- a Latin-1 title from an old rip, say. Invalid bytes become '?', and
 * a character is never cut in half at the end. */
static void utf8_copy(char *out, size_t n, const char *in) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < n) {
        unsigned c = p[0];
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 :
                  (c & 0xF8) == 0xF0 ? 4 : 0;
        int ok = len > 0;
        for (int k = 1; ok && k < len; k++)
            if ((p[k] & 0xC0) != 0x80) ok = 0;
        if (ok && len > 1) {
            unsigned cp = len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
            for (int k = 1; k < len; k++) cp = (cp << 6) | (p[k] & 0x3F);
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0xFFFE || cp == 0xFFFF)
                ok = 0;
        }
        if (!ok) { out[o++] = '?'; p++; continue; }
        if (o + (size_t)len >= n) break;
        memcpy(out + o, p, (size_t)len);
        o += (size_t)len;
        p += len;
    }
    out[o] = '\0';
}

/* The UI thread's own copy of what it last passed on, so an unchanged frame
 * costs a few compares and no lock. */
static char    ui_title[256], ui_artist[256], ui_album[256];
static int64_t ui_length = -1;
static int     ui_track = -1, ui_status = -1;

void btplayer_update(int status, const char *title, const char *artist,
                     const char *album, int64_t length_ms, int track_no) {
    if (!g_started) return;
    int meta = strncmp(title, ui_title, sizeof(ui_title) - 1) != 0 ||
               strncmp(artist, ui_artist, sizeof(ui_artist) - 1) != 0 ||
               strncmp(album, ui_album, sizeof(ui_album) - 1) != 0 ||
               length_ms != ui_length || track_no != ui_track;
    int stat = status != ui_status;
    if (!meta && !stat) return;
    pthread_mutex_lock(&g_lock);
    if (meta) {
        snprintf(ui_title, sizeof(ui_title), "%s", title);
        snprintf(ui_artist, sizeof(ui_artist), "%s", artist);
        snprintf(ui_album, sizeof(ui_album), "%s", album);
        ui_length = length_ms;
        ui_track = track_no;
        utf8_copy(g_cur.title, sizeof(g_cur.title), title);
        utf8_copy(g_cur.artist, sizeof(g_cur.artist), artist);
        utf8_copy(g_cur.album, sizeof(g_cur.album), album);
        g_cur.length_ms = length_ms;
        g_cur.track_no = track_no;
        g_meta_gen++;
    }
    if (stat) {
        ui_status = status;
        g_cur.status = status;
        g_status_gen++;
    }
    pthread_mutex_unlock(&g_lock);
    if (g_wake[1] >= 0 && write(g_wake[1], "x", 1) < 0) { /* already nudged */ }
}

int btplayer_take_command(void) {
    if (!g_started) return BTP_CMD_NONE;
    int cmd = BTP_CMD_NONE;
    pthread_mutex_lock(&g_lock);
    if (g_cmd_n > 0) {
        cmd = g_cmd[g_cmd_head];
        g_cmd_head = (g_cmd_head + 1) % CMD_MAX;
        g_cmd_n--;
    }
    pthread_mutex_unlock(&g_lock);
    return cmd;
}

static void push_cmd(int cmd) {
    pthread_mutex_lock(&g_lock);
    if (g_cmd_n < CMD_MAX) {
        g_cmd[(g_cmd_head + g_cmd_n) % CMD_MAX] = cmd;
        g_cmd_n++;
    }
    pthread_mutex_unlock(&g_lock);
}

static void snapshot(snap_t *out, unsigned *meta_gen, unsigned *status_gen) {
    pthread_mutex_lock(&g_lock);
    *out = g_cur;
    *meta_gen = g_meta_gen;
    *status_gen = g_status_gen;
    pthread_mutex_unlock(&g_lock);
}

/* ---- building messages --------------------------------------------------- */
static const char *status_name(int s) {
    return s == BTP_PLAYING ? "Playing" : s == BTP_PAUSED ? "Paused" : "Stopped";
}

static void md_begin(DBusMessageIter *md, DBusMessageIter *e, DBusMessageIter *v,
                     const char *key, const char *sig) {
    dl.iter_open(md, 'e', NULL, e);
    dl.iter_append(e, 's', &key);
    dl.iter_open(e, 'v', sig, v);
}
static void md_end(DBusMessageIter *md, DBusMessageIter *e, DBusMessageIter *v) {
    dl.iter_close(e, v);
    dl.iter_close(md, e);
}
static void md_str(DBusMessageIter *md, const char *key, const char *val) {
    DBusMessageIter e, v;
    md_begin(md, &e, &v, key, "s");
    dl.iter_append(&v, 's', &val);
    md_end(md, &e, &v);
}
static void md_strs(DBusMessageIter *md, const char *key, const char *val) {
    DBusMessageIter e, v, a;
    md_begin(md, &e, &v, key, "as");
    dl.iter_open(&v, 'a', "s", &a);
    dl.iter_append(&a, 's', &val);
    dl.iter_close(&v, &a);
    md_end(md, &e, &v);
}
static void md_i64(DBusMessageIter *md, const char *key, int64_t val) {
    DBusMessageIter e, v;
    md_begin(md, &e, &v, key, "x");
    dl.iter_append(&v, 'x', &val);
    md_end(md, &e, &v);
}
static void md_i32(DBusMessageIter *md, const char *key, int32_t val) {
    DBusMessageIter e, v;
    md_begin(md, &e, &v, key, "i");
    dl.iter_append(&v, 'i', &val);
    md_end(md, &e, &v);
}

/* A property's value as a variant appended to `it`. 0 for a name this does
 * not serve, with nothing appended. */
static int append_prop_value(DBusMessageIter *it, const char *prop, const snap_t *sn) {
    DBusMessageIter v;
    if (!strcmp(prop, "PlaybackStatus")) {
        const char *s = status_name(sn->status);
        dl.iter_open(it, 'v', "s", &v);
        dl.iter_append(&v, 's', &s);
        dl.iter_close(it, &v);
        return 1;
    }
    if (!strcmp(prop, "Metadata")) {
        DBusMessageIter md;
        dl.iter_open(it, 'v', "a{sv}", &v);
        dl.iter_open(&v, 'a', "{sv}", &md);
        md_str(&md, "xesam:title", sn->title);
        if (sn->artist[0]) md_strs(&md, "xesam:artist", sn->artist);
        if (sn->album[0]) md_str(&md, "xesam:album", sn->album);
        if (sn->length_ms > 0) md_i64(&md, "mpris:length", sn->length_ms * 1000);
        if (sn->track_no > 0) md_i32(&md, "xesam:trackNumber", sn->track_no);
        dl.iter_close(&v, &md);
        dl.iter_close(it, &v);
        return 1;
    }
    static const char *const yes[] = { "CanPlay", "CanPause", "CanGoNext", "CanGoPrevious", "CanControl" };
    for (unsigned i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        if (strcmp(prop, yes[i])) continue;
        dbus_bool_t b = 1;
        dl.iter_open(it, 'v', "b", &v);
        dl.iter_append(&v, 'b', &b);
        dl.iter_close(it, &v);
        return 1;
    }
    if (!strcmp(prop, "CanSeek")) {
        dbus_bool_t b = 0;
        dl.iter_open(it, 'v', "b", &v);
        dl.iter_append(&v, 'b', &b);
        dl.iter_close(it, &v);
        return 1;
    }
    if (!strcmp(prop, "Rate") || !strcmp(prop, "MinimumRate") || !strcmp(prop, "MaximumRate")) {
        double r = 1.0;
        dl.iter_open(it, 'v', "d", &v);
        dl.iter_append(&v, 'd', &r);
        dl.iter_close(it, &v);
        return 1;
    }
    if (!strcmp(prop, "Identity")) {
        const char *s = "Libra";
        dl.iter_open(it, 'v', "s", &v);
        dl.iter_append(&v, 's', &s);
        dl.iter_close(it, &v);
        return 1;
    }
    return 0;
}

static void dict_prop(DBusMessageIter *dict, const char *name, const snap_t *sn) {
    DBusMessageIter e;
    dl.iter_open(dict, 'e', NULL, &e);
    dl.iter_append(&e, 's', &name);
    append_prop_value(&e, name, sn);
    dl.iter_close(dict, &e);
}

/* Everything this player serves. "Identity" is not an MPRIS Player property,
 * but it is how BlueZ names the player to a headset that asks. */
static const char *const all_props[] = {
    "PlaybackStatus", "Metadata", "CanPlay", "CanPause", "CanGoNext",
    "CanGoPrevious", "CanControl", "CanSeek", "Rate", "MinimumRate", "MaximumRate",
};

static void append_all_props(DBusMessageIter *dict, const snap_t *sn, int with_identity) {
    for (unsigned i = 0; i < sizeof(all_props) / sizeof(all_props[0]); i++)
        dict_prop(dict, all_props[i], sn);
    if (with_identity) dict_prop(dict, "Identity", sn);
}

/* ---- the D-Bus thread ---------------------------------------------------- */
static DBusConnection *g_conn;
static int      g_registered;
static long     g_next_try, g_next_check;          /* monotonic seconds */
static unsigned g_sent_meta, g_sent_status;
static char     g_last_err[128];

static long mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static void reply(DBusMessage *call, DBusMessage *r) {
    if (!r) return;
    if (!dl.get_no_reply(call)) dl.send(g_conn, r, NULL);
    dl.msg_unref(r);
}

static void reply_error(DBusMessage *call, const char *name, const char *text) {
    if (dl.get_no_reply(call)) return;
    reply(call, dl.new_error(call, name, text));
}

static void drop_bus(void) {
    if (!g_conn) return;
    dl.close(g_conn);
    dl.conn_unref(g_conn);
    g_conn = NULL;
    g_registered = 0;
    LOG("[btplayer] lost the system bus\n");
}

static int connect_bus(void) {
    DBusError err;
    dl.error_init(&err);
    DBusConnection *c = dl.bus_get_private(BUS_SYSTEM, &err);
    if (!c) {
        const char *name = err.name ? err.name : "no bus";
        if (strcmp(name, g_last_err)) {
            LOG("[btplayer] no system bus yet: %s\n", err.message ? err.message : name);
            snprintf(g_last_err, sizeof(g_last_err), "%s", name);
        }
        dl.error_free(&err);
        return 0;
    }
    /* libdbus's default is to _exit() the process when the bus goes away --
     * a dbus-daemon restart would take Libra with it. */
    dl.set_exit_on_disconnect(c, 0);
    dl.bus_add_match(c, "type='signal',sender='org.freedesktop.DBus',"
                        "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                        "arg0='org.bluez'", NULL);
    dl.bus_add_match(c, "type='signal',sender='org.bluez',"
                        "interface='org.freedesktop.DBus.ObjectManager'", NULL);
    dl.flush(c);
    g_conn = c;
    g_registered = 0;
    g_next_try = 0;
    g_last_err[0] = '\0';
    LOG("[btplayer] on the system bus as %s\n", dl.bus_get_unique_name(c));
    return 1;
}

/* RegisterPlayer, carrying the current state. Also the periodic check that
 * the registration is still there: asked again, BlueZ answers AlreadyExists
 * while it is -- and registers afresh if it had been lost (bluetoothd
 * restarted by a Bluetooth off/on, say, between the signals that say so). */
static void try_register(void) {
    snap_t sn;
    unsigned mg, sg;
    snapshot(&sn, &mg, &sg);
    DBusMessage *m = dl.new_method_call(BLUEZ, ADAPTER_PATH, "org.bluez.Media1", "RegisterPlayer");
    if (!m) return;
    DBusMessageIter it, dict;
    const char *path = PLAYER_PATH;
    dl.iter_init_append(m, &it);
    dl.iter_append(&it, 'o', &path);
    dl.iter_open(&it, 'a', "{sv}", &dict);
    append_all_props(&dict, &sn, 1);
    dl.iter_close(&it, &dict);
    DBusError err;
    dl.error_init(&err);
    DBusMessage *r = dl.send_blocking(g_conn, m, 3000, &err);
    dl.msg_unref(m);
    if (r) {
        dl.msg_unref(r);
        if (g_registered) LOG("[btplayer] registration had gone; registered again\n");
        else LOG("[btplayer] registered with BlueZ as the media player\n");
        g_registered = 1;
        g_sent_meta = mg;          /* the registration itself carried this state */
        g_sent_status = sg;
        g_last_err[0] = '\0';
        return;
    }
    const char *name = err.name ? err.name : "no reply";
    if (!strcmp(name, "org.bluez.Error.AlreadyExists")) {
        if (!g_registered) {
            /* Ours from before a lost signal: still there, but what it last
             * heard may be stale -- send the lot. */
            g_sent_meta = mg - 1;
            g_sent_status = sg - 1;
        }
        g_registered = 1;
    } else {
        g_registered = 0;
        if (strcmp(name, g_last_err)) {
            LOG("[btplayer] not registered: %s%s%s\n", name,
                err.message ? ": " : "", err.message ? err.message : "");
            snprintf(g_last_err, sizeof(g_last_err), "%s", name);
        }
    }
    dl.error_free(&err);
}

/* PropertiesChanged for whatever changed since it last went out. Metadata
 * first: BlueZ treats it as a new track (and restarts its position clock)
 * before it sees a status change in the same signal. */
static void push_state(void) {
    snap_t sn;
    unsigned mg, sg;
    snapshot(&sn, &mg, &sg);
    int meta = mg != g_sent_meta, stat = sg != g_sent_status;
    if (!meta && !stat) return;
    DBusMessage *s = dl.new_signal(PLAYER_PATH, PROPS_IFACE, "PropertiesChanged");
    if (!s) return;
    DBusMessageIter it, dict, inv;
    const char *iface = MPRIS_PLAYER;
    dl.iter_init_append(s, &it);
    dl.iter_append(&it, 's', &iface);
    dl.iter_open(&it, 'a', "{sv}", &dict);
    if (meta) dict_prop(&dict, "Metadata", &sn);
    if (stat) dict_prop(&dict, "PlaybackStatus", &sn);
    dl.iter_close(&it, &dict);
    dl.iter_open(&it, 'a', "s", &inv);
    dl.iter_close(&it, &inv);
    dl.send(g_conn, s, NULL);
    dl.flush(g_conn);
    dl.msg_unref(s);
    g_sent_meta = mg;
    g_sent_status = sg;
}

static void on_props(DBusMessage *m, const char *member) {
    snap_t sn;
    unsigned mg, sg;
    snapshot(&sn, &mg, &sg);
    DBusMessageIter it;
    const char *iface = NULL, *prop = NULL;
    if (dl.iter_init(m, &it) && dl.iter_type(&it) == 's') {
        dl.iter_get(&it, &iface);
        if (dl.iter_next(&it) && dl.iter_type(&it) == 's') dl.iter_get(&it, &prop);
    }
    if (!strcmp(member, "GetAll")) {
        DBusMessage *r = dl.new_method_return(m);
        if (r) {
            DBusMessageIter ri, dict;
            dl.iter_init_append(r, &ri);
            dl.iter_open(&ri, 'a', "{sv}", &dict);
            if (iface && !strcmp(iface, MPRIS_PLAYER)) append_all_props(&dict, &sn, 0);
            dl.iter_close(&ri, &dict);
        }
        reply(m, r);
    } else if (!strcmp(member, "Get")) {
        DBusMessage *r = NULL;
        if (iface && prop && !strcmp(iface, MPRIS_PLAYER)) {
            r = dl.new_method_return(m);
            if (r) {
                DBusMessageIter ri;
                dl.iter_init_append(r, &ri);
                if (!append_prop_value(&ri, prop, &sn)) { dl.msg_unref(r); r = NULL; }
            }
        }
        if (r) reply(m, r);
        else reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", prop ? prop : "no property");
    } else if (!strcmp(member, "Set")) {
        reply_error(m, "org.freedesktop.DBus.Error.PropertyReadOnly", prop ? prop : "");
    } else {
        reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", member);
    }
}

static void on_call(DBusMessage *m) {
    const char *path = dl.get_path(m), *iface = dl.get_interface(m), *member = dl.get_member(m);
    if (!member) return;
    if (!path || strcmp(path, PLAYER_PATH)) {
        reply_error(m, "org.freedesktop.DBus.Error.UnknownObject", path ? path : "no path");
        return;
    }
    if (!iface || !strcmp(iface, MPRIS_PLAYER)) {
        static const struct { const char *name; int cmd; } cmds[] = {
            { "Play", BTP_CMD_PLAY }, { "Pause", BTP_CMD_PAUSE }, { "PlayPause", BTP_CMD_PLAYPAUSE },
            { "Stop", BTP_CMD_STOP }, { "Next", BTP_CMD_NEXT }, { "Previous", BTP_CMD_PREVIOUS },
        };
        for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
            if (strcmp(member, cmds[i].name)) continue;
            push_cmd(cmds[i].cmd);
            LOG("[btplayer] %s\n", member);
            reply(m, dl.new_method_return(m));
            return;
        }
        if (iface) {
            reply_error(m, "org.freedesktop.DBus.Error.NotSupported", member);
            return;
        }
    }
    if (iface && !strcmp(iface, PROPS_IFACE)) {
        on_props(m, member);
        return;
    }
    if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable") && !strcmp(member, "Introspect")) {
        static const char *const xml =
            "<node><interface name=\"" MPRIS_PLAYER "\">"
            "<method name=\"Play\"/><method name=\"Pause\"/><method name=\"PlayPause\"/>"
            "<method name=\"Stop\"/><method name=\"Next\"/><method name=\"Previous\"/>"
            "</interface></node>";
        DBusMessage *r = dl.new_method_return(m);
        if (r) {
            DBusMessageIter it;
            const char *s = xml;
            dl.iter_init_append(r, &it);
            dl.iter_append(&it, 's', &s);
        }
        reply(m, r);
        return;
    }
    reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", member);
}

static const char *first_string(DBusMessage *m, int type) {
    DBusMessageIter it;
    const char *s = NULL;
    if (dl.iter_init(m, &it) && dl.iter_type(&it) == type) dl.iter_get(&it, &s);
    return s;
}

static void on_signal(DBusMessage *m) {
    if (dl.is_signal(m, "org.freedesktop.DBus.Local", "Disconnected")) {
        drop_bus();
        return;
    }
    if (dl.is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char *name = first_string(m, 's');
        if (name && !strcmp(name, BLUEZ)) {
            /* bluetoothd went away or came back: either way whatever was
             * registered with the old one is gone. */
            g_registered = 0;
            g_next_try = 0;
        }
        return;
    }
    if (dl.is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded") ||
        dl.is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved")) {
        const char *path = first_string(m, 'o');
        if (path && !strcmp(path, ADAPTER_PATH)) {
            g_registered = 0;
            g_next_try = 0;
        }
    }
}

/* Everything already read off the socket. Run before every sleep, not just
 * after a wake: a blocking call (try_register()) reads and queues whatever
 * else arrives meanwhile -- a headset's Pause, say -- and poll() will not
 * wake for data the library has already taken off the socket. */
static void handle_queued(void) {
    DBusMessage *m;
    while (g_conn && (m = dl.pop_message(g_conn))) {
        int type = dl.get_type(m);
        if (type == MSG_METHOD_CALL) on_call(m);
        else if (type == MSG_SIGNAL) on_signal(m);
        dl.msg_unref(m);
    }
    if (g_conn && g_registered) push_state();
    if (g_conn) dl.flush(g_conn);
}

static void *thread_main(void *arg) {
    (void)arg;
    for (;;) {
        if (!g_conn && !connect_bus()) { sleep(5); continue; }
        long now = mono_s();
        if (!g_registered && now >= g_next_try) {
            try_register();
            g_next_try = now + 5;
            g_next_check = now + 30;
        } else if (g_registered && now >= g_next_check) {
            try_register();
            g_next_check = now + 30;
        }
        handle_queued();
        if (!g_conn) continue;

        /* Sleep until the bus has something, the UI thread nudges, or the
         * next retry/check is due -- not a fixed poll, since this runs for
         * as long as Libra does, pocket time included. */
        int fd = -1;
        if (!dl.get_unix_fd(g_conn, &fd)) fd = -1;
        long due = g_registered ? g_next_check : g_next_try;
        long wait_s = due - mono_s();
        int timeout_ms = wait_s <= 0 ? 0 : (wait_s > 30 ? 30000 : (int)(wait_s * 1000));
        struct pollfd pfd[2] = { { fd, POLLIN, 0 }, { g_wake[0], POLLIN, 0 } };
        if (fd >= 0) {
            if (poll(pfd, 2, timeout_ms) < 0 && errno != EINTR) usleep(100000);
        } else {
            usleep(250000);   /* no descriptor to wait on: fall back to a plain poll */
        }
        if (pfd[1].revents) {
            char buf[64];
            while (read(g_wake[0], buf, sizeof(buf)) > 0) { }
        }
        if (!dl.read_write(g_conn, 0)) { drop_bus(); continue; }
        handle_queued();
    }
    return NULL;
}

void btplayer_start(void (*log)(const char *fmt, ...)) {
    if (g_started) return;
    if (log) g_log = log;
    if (!dl_load()) return;
    dl.threads_init_default();
    if (pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) != 0) { g_wake[0] = g_wake[1] = -1; }
    pthread_t t;
    if (pthread_create(&t, NULL, thread_main, NULL) != 0) {
        LOG("[btplayer] no thread; headset keys only\n");
        return;
    }
    pthread_detach(t);
    g_started = 1;
}
