/* status.c — the things the status bar reports.
 *
 * All of it comes from sysfs except the Bluetooth check, which has to shell out
 * to bluealsa-cli and is therefore cached: forking a 240 MB process every frame
 * to ask the same question is not a reasonable way to draw an icon.
 *
 * Note the battery: there are two supplies, and axp_battery reads a flat 0.
 * The one that reports honestly is plain "battery".
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "status.h"

static int read_int_file(const char *path, int fallback);

static int read_int_file(const char *path, int fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    int v = fallback;
    if (fscanf(f, "%d", &v) != 1) v = fallback;
    fclose(f);
    return v;
}

int st_battery_pct(void) {
    return read_int_file("/sys/class/power_supply/battery/capacity", -1);
}

int st_charging(void) {
    FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
    if (!f) return 0;
    char s[32] = "";
    if (!fgets(s, sizeof(s), f)) s[0] = '\0';
    fclose(f);
    return strncmp(s, "Charging", 8) == 0;
}

int st_headset(void) {
    return read_int_file("/sys/class/switch/headset/state", 0) != 0;
}

/* ---- Bluetooth sink details --------------------------------------------- */
/* Both of these shell out, so both are cached: the codec cannot change without
 * a reconnection and a battery reading that is ten seconds stale is still a
 * useful battery reading.
 *
 * The exact wording of the tools' output has not been seen with a sink
 * actually connected, so the parsing takes the last field of any line that
 * mentions the thing it wants rather than matching a fixed layout, and the raw
 * first line is logged once so it can be checked against reality.
 */
static void run_cmd(const char *cmd, char *out, unsigned n) {
    out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) return;
    unsigned used = 0;
    char line[256];
    while (used + 1 < n && fgets(line, sizeof(line), p)) {
        unsigned len = (unsigned)strlen(line);
        if (used + len >= n) len = n - used - 1;
        memcpy(out + used, line, len);
        used += len;
    }
    out[used] = '\0';
    pclose(p);
}

/* R-scanwifi: wpa_cli escapes any non-printable/non-ASCII byte in an SSID
 * as \xNN (confirmed live via st_wifi_ssid()'s own history) -- shared here
 * since scan results carry the same escaping in the same shape. `in` need
 * not be NUL-terminated at exactly `inlen`; the caller passes the field's
 * own length from a larger tab/newline-delimited line. */
static void wpa_unescape(const char *in, size_t inlen, char *out, size_t outsz) {
    unsigned o = 0;
    for (size_t i = 0; i < inlen && o + 1 < outsz; ) {
        if (in[i] == '\\' && i + 3 < inlen && in[i + 1] == 'x') {
            int hi = in[i + 2], lo = in[i + 3];
            hi = (hi >= 'a') ? hi - 'a' + 10 : (hi >= 'A') ? hi - 'A' + 10 : hi - '0';
            lo = (lo >= 'a') ? lo - 'a' + 10 : (lo >= 'A') ? lo - 'A' + 10 : lo - '0';
            out[o++] = (char)((hi << 4) | lo);
            i += 4;
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

/* The first BlueALSA sink path, which both queries need. */
/* Exposed as st_bt_pcm_path() below: audio.c needs the same path to set the
 * PCM's SoftVolume property. */
static int bt_pcm_path(char *out, unsigned n) {
    char buf[1024];
    run_cmd("bluealsa-cli list-pcms 2>/dev/null", buf, sizeof(buf));
    char *line = strtok(buf, "\n");
    while (line) {
        /* Must match a2dp AND /sink, exactly as audio.c's bt_sink_connected()
         * does. A connected headset publishes three PCMs -- a2dpsrc/sink plus
         * hfpag/sink and hfpag/source -- and bluealsa-cli lists hfpag first,
         * so matching "/sink" alone returned the hands-free PCM. Its codec is
         * CVSD (8kHz mono voice), so the footer showed CVSD while the music
         * was actually playing over aptX on the A2DP transport. */
        if (strstr(line, "a2dp") && strstr(line, "/sink")) {
            while (*line == ' ') line++;
            snprintf(out, n, "%s", line);
            return 1;
        }
        line = strtok(NULL, "\n");
    }
    return 0;
}

void st_bt_codec(char *out, unsigned n) {
    static char cached[32];
    static time_t when;
    time_t now = time(NULL);
    if (cached[0] && now - when < 15) { snprintf(out, n, "%s", cached); return; }

    cached[0] = '\0';
    out[0] = '\0';
    char path[256];
    if (!bt_pcm_path(path, sizeof(path))) { when = now; return; }

    char cmd[384], buf[512];
    snprintf(cmd, sizeof(cmd), "bluealsa-cli codec '%s' 2>/dev/null", path);
    run_cmd(cmd, buf, sizeof(buf));

    /* The real output, with a headset connected, is two lines:
     *
     *   Available codecs: SBC AAC LDAC
     *   Selected codec: LDAC
     *
     * Take the selected one. The first attempt at this used strcasestr, which
     * needs _GNU_SOURCE to be declared and silently returned rubbish without
     * it — hence an empty codec and a footer that just said "Bluetooth". */
    char *sel = strstr(buf, "Selected codec:");
    if (sel) {
        sel += strlen("Selected codec:");
        while (*sel == ' ' || *sel == '\t') sel++;
        char *end = sel;
        while (*end && *end != '\n' && *end != '\r' && *end != ' ') end++;
        *end = '\0';
        if (*sel) snprintf(cached, sizeof(cached), "%s", sel);
    }
    when = now;
    snprintf(out, n, "%s", cached);
}

int st_bt_battery(void) {
    static int cached = -1;
    static time_t when;
    time_t now = time(NULL);
    if (now - when < 15 && cached != -1) return cached;
    when = now;
    cached = -1;

    /* Read it from BlueALSA, not BlueZ. org.bluez.Battery1 is the obvious
     * place and it does not exist here — BlueZ is 5.54 and that interface
     * arrived in 5.56, so the query answered "No such interface" every time.
     * The level comes in over HFP and BlueALSA is what terminates that link,
     * so it is BlueALSA that knows. */
    char path[256];
    if (!bt_pcm_path(path, sizeof(path))) return -1;

    /* .../dev_XX/a2dpsrc/sink -> .../dev_XX/rfcomm */
    char rfcomm[288];
    snprintf(rfcomm, sizeof(rfcomm), "%s", path);
    char *tail = strstr(rfcomm, "/a2dpsrc");
    if (!tail) tail = strstr(rfcomm, "/hfpag");
    if (!tail) return -1;
    snprintf(tail, sizeof(rfcomm) - (size_t)(tail - rfcomm), "/rfcomm");

    char cmd[384], buf[1024];
    snprintf(cmd, sizeof(cmd), "bluealsa-rfcomm '%s' --properties 2>/dev/null", rfcomm);
    run_cmd(cmd, buf, sizeof(buf));

    char *b = strstr(buf, "Battery:");
    if (b) {
        int v = atoi(b + 8);
        if (v >= 0 && v <= 100) cached = v;
    }
    return cached;
}

/* Names for the panel. Both shell out, both are cached, and both are only
 * asked for while the panel is open. */
void st_wifi_ssid(char *out, unsigned n) {
    static char cached[64];
    static time_t when;
    time_t now = time(NULL);
    if (cached[0] && now - when < 10) { snprintf(out, n, "%s", cached); return; }
    when = now;
    cached[0] = '\0';
    /* wpa_cli, not iwconfig — the latter is not on this firmware. Non-ASCII
     * SSIDs come back as \xNN escapes, which are decoded so the panel shows
     * the name rather than the escaping. */
    char buf[512];
    run_cmd("wpa_cli -i wlan0 status 2>/dev/null", buf, sizeof(buf));
    char *q = strstr(buf, "\nssid=");
    if (!q && !strncmp(buf, "ssid=", 5)) q = buf - 1;
    if (q) {
        q += 6;
        char *e = strchr(q, '\n');
        size_t len = e ? (size_t)(e - q) : strlen(q);
        wpa_unescape(q, len, cached, sizeof(cached));
    }
    snprintf(out, n, "%s", cached);
}

void st_bt_name(char *out, unsigned n) {
    static char cached[64];
    static time_t when;
    time_t now = time(NULL);
    if (cached[0] && now - when < 10) { snprintf(out, n, "%s", cached); return; }
    when = now;
    cached[0] = '\0';
    char path[256];
    if (bt_pcm_path(path, sizeof(path))) {
        /* .../dev_94_DB_56_8E_03_43/... -> 94:DB:...  then ask BlueZ its name */
        char *d = strstr(path, "dev_");
        if (d) {
            char mac[32];
            unsigned k = 0;
            for (const char *p = d + 4; *p && *p != '/' && k + 1 < sizeof(mac); p++)
                mac[k++] = (*p == '_') ? ':' : *p;
            mac[k] = '\0';
            char cmd[128], buf[512];
            snprintf(cmd, sizeof(cmd),
                     "printf 'info %s\nquit\n' | bluetoothctl 2>/dev/null", mac);
            run_cmd(cmd, buf, sizeof(buf));
            char *nm = strstr(buf, "Name: ");
            if (nm) {
                nm += 6;
                char *e = strchr(nm, '\n');
                size_t len = e ? (size_t)(e - nm) : strlen(nm);
                while (len && (nm[len-1] == '\r' || nm[len-1] == ' ')) len--;
                if (len >= sizeof(cached)) len = sizeof(cached) - 1;
                memcpy(cached, nm, len);
                cached[len] = '\0';
            }
        }
    }
    snprintf(out, n, "%s", cached);
}

/* ---- Bluetooth pairing ---------------------------------------------------
 * NoInputNoOutput agent throughout: this device has no PIN-entry UI, and
 * that capability tells BlueZ's pairing negotiation not to need one --
 * "Just Works" pairing, which covers the overwhelming majority of BT
 * audio gear (headphones/speakers have no display or keypad of their own
 * either, so they already expect this). A device that specifically
 * demands MITM-protected (PIN/passkey) pairing will fail to pair here;
 * there is no PIN prompt anywhere in this app to satisfy one with, and
 * building one is future work, not silently pretended to exist now. */
void bt_scan_start(void) {
    /* Same missing-`timeout`-applet bug as bt_pair() -- see its own comment.
     * Different fix here, though: bt_pair()'s session is meant to end the
     * instant its few commands are sent (stdin hitting EOF once printf is
     * done writing does that on its own), but this one needs "scan on" to
     * keep running for a real 8 seconds before ending, which EOF alone
     * would cut short immediately. `sleep 8` (present -- unlike timeout,
     * not something this device's busybox lacks) between "scan on" and
     * "quit" keeps the pipe's write end open for that whole window instead,
     * holding bluetoothctl's own stdin off EOF (and its scan running) for
     * as long as timeout 8 was meant to. */
    if (system("(printf 'agent NoInputNoOutput\\ndefault-agent\\nscan on\\n'; "
               "sleep 8; printf 'quit\\n') | bluetoothctl >/dev/null 2>&1 &") == -1) return;
}

/* Every device bluetoothd currently knows about -- already-paired and
 * freshly-discovered alike, `bluetoothctl devices` doesn't distinguish --
 * one "Device XX:XX:XX:XX:XX:XX Name..." line each. Parsed into a fixed
 * array rather than returned as text: the caller needs the MAC (to pair)
 * and the name (to display) as separate fields, not one blob to re-parse
 * itself. Returns how many were found, capped at max. */
/* Is this MAC a device bluez already has a pairing record for, as opposed to
 * one bt_scan_devices() only just discovered? Answered from bluez's own
 * on-disk state (/usr/data/bluetooth/<adapter>/<device>/info) rather than by
 * spawning bluetoothctl per device -- this runs once per device on every
 * refresh of the Bluetooth settings screen, and a stat() is a fraction of a
 * fork+exec. bluez creates that directory the moment a device is paired and
 * it survives disconnection/power-off, which is exactly "paired", not
 * "currently connected" -- a device that is on but not yet reconnected still
 * counts. Reported live: an already-paired headset that was switched on
 * didn't answer an inquiry scan (expected -- see bt_scan_start()'s own
 * comment on Just Works pairing; a paired device isn't in discoverable mode,
 * it only answers a direct page) and so never surfaced as connectable from
 * this screen even though `bluetoothctl connect <mac>` worked immediately by
 * hand. Splitting the list is the fix: paired devices belong above scan
 * results regardless of whether they showed up in this scan's inquiry. */
int bt_is_paired(const char *mac) {
    DIR *d = opendir("/usr/data/bluetooth");
    if (!d) return 0;
    struct dirent *ent;
    int paired = 0;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char path[300];
        snprintf(path, sizeof(path), "/usr/data/bluetooth/%s/%s/info", ent->d_name, mac);
        struct stat st;
        if (stat(path, &st) == 0) { paired = 1; break; }
    }
    closedir(d);
    return paired;
}

int bt_scan_devices(bt_found_dev_t *out, int max) {
    /* R84 follow-up: 4096 was sized for a handful of devices and silently
     * truncated in a crowded environment -- reported live as the Paired
     * devices section vanishing entirely because bluetoothd's inquiry, with
     * ambient LE traffic nearby, returned 27 devices and the caller's own
     * fixed-size array (separately bumped) only asked for the first 8 of
     * whatever this buffer held. 8192 comfortably holds well over a hundred
     * "Device XX:XX:XX:XX:XX:XX Name" lines. */
    char buf[8192];
    run_cmd("bluetoothctl devices 2>/dev/null", buf, sizeof(buf));
    int n = 0;
    char *line = buf;
    while (line && *line && n < max) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (!strncmp(line, "Device ", 7) && strlen(line) > 24) {
            const char *mac = line + 7;
            const char *name = line + 7 + 17;   /* "XX:XX:XX:XX:XX:XX" is 17 chars */
            if (*name == ' ') name++;
            memcpy(out[n].mac, mac, 17); out[n].mac[17] = '\0';
            snprintf(out[n].name, sizeof(out[n].name), "%s", name);
            /* Cleared, not left alone: the caller's array is reused across
             * refreshes, so a reading from a previous scan would otherwise
             * stick to whatever device now lands in this slot. */
            out[n].rssi = 0;
            n++;
        }
        line = nl ? nl + 1 : NULL;
    }
    return n;
}

/* bluez publishes RSSI as a property on each device object, not through
 * `bluetoothctl devices` (which prints only "Device <mac> <name>"), so it has
 * to come from D-Bus. GetManagedObjects returns every device in one reply --
 * ~110KB of text with a crowded scan running, but measured on hardware at
 * ~0.08s against ~0.25s for the `bluetoothctl devices` call that already runs
 * on this same refresh, because dbus-send is a single round trip where
 * bluetoothctl starts a whole interactive client. Streamed line by line rather
 * than slurped into a buffer, since that reply is far too big for the
 * fixed-size run_cmd() arrays used elsewhere here.
 *
 * The reply nests properties under each object path, e.g.
 *
 *     object path "/org/bluez/hci0/dev_40_ED_98_1D_2D_29"
 *     ...
 *                  string "RSSI"
 *                  variant       int16 -83
 *
 * so the parse tracks the most recent object path and attaches the next RSSI
 * value it sees to it -- order-independent within a device's property list,
 * which matters because bluez does not promise any particular property
 * order. */
/* Last reading seen for each device, kept because bluez does not keep one:
 * it publishes RSSI only while discovery is actually running and drops the
 * property again when the scan stops. bt_scan_start() runs its scan for 8
 * seconds, so without this the readings -- and the proximity ordering built on
 * them -- would appear for one or two refreshes and then blank out to "--"
 * moments later, which is worse than never showing them. A remembered reading
 * is still the right answer to "how close is this device": it is the last time
 * we actually heard from it. */
#define BT_RSSI_CACHE_N 64
static struct { char mac[18]; int rssi; } bt_rssi_cache[BT_RSSI_CACHE_N];
static int bt_rssi_cache_n;

static void bt_rssi_remember(const char *mac, int rssi) {
    for (int i = 0; i < bt_rssi_cache_n; i++)
        if (!strcasecmp(bt_rssi_cache[i].mac, mac)) { bt_rssi_cache[i].rssi = rssi; return; }
    if (bt_rssi_cache_n >= BT_RSSI_CACHE_N) return;   /* full: this one just goes uncached */
    snprintf(bt_rssi_cache[bt_rssi_cache_n].mac, sizeof(bt_rssi_cache[0].mac), "%s", mac);
    bt_rssi_cache[bt_rssi_cache_n].rssi = rssi;
    bt_rssi_cache_n++;
}

static int bt_rssi_recall(const char *mac) {
    for (int i = 0; i < bt_rssi_cache_n; i++)
        if (!strcasecmp(bt_rssi_cache[i].mac, mac)) return bt_rssi_cache[i].rssi;
    return 0;
}

void bt_fill_details(bt_found_dev_t *devs, int n) {
    if (n <= 0) return;
    FILE *p = popen("dbus-send --system --print-reply --dest=org.bluez / "
                    "org.freedesktop.DBus.ObjectManager.GetManagedObjects 2>/dev/null", "r");
    if (!p) {
        for (int i = 0; i < n; i++) {
            devs[i].rssi = bt_rssi_recall(devs[i].mac);
            devs[i].paired = bt_is_paired(devs[i].mac);
        }
        return;
    }
    char line[512];
    char mac[18] = "";
    int want_value = 0;            /* 0 none, 1 expecting RSSI, 2 expecting Paired */
    int saw_paired_property = 0;
    for (int i = 0; i < n; i++) devs[i].paired = 0;
    while (fgets(line, sizeof(line), p)) {
        const char *dev = strstr(line, "/org/bluez/");
        if (dev) {
            /* ".../dev_XX_XX_XX_XX_XX_XX" -> "XX:XX:XX:XX:XX:XX". Anything
             * else under /org/bluez (the adapter itself, a service, a
             * characteristic) simply has no dev_ segment ending the path and
             * is skipped. */
            const char *d = strstr(dev, "dev_");
            mac[0] = '\0';
            if (d && strlen(d) >= 4 + 17) {
                d += 4;
                int ok = 1;
                for (int i = 0; i < 17; i++) {
                    char c = d[i];
                    if (i % 3 == 2) {
                        if (c != '_') { ok = 0; break; }
                        mac[i] = ':';
                    } else {
                        if (!isxdigit((unsigned char)c)) { ok = 0; break; }
                        mac[i] = c;
                    }
                }
                if (ok) mac[17] = '\0'; else mac[0] = '\0';
            }
            want_value = 0;
            continue;
        }
        if (strstr(line, "\"RSSI\""))   { want_value = 1; continue; }
        if (strstr(line, "\"Paired\"")) { want_value = 2; continue; }
        if (want_value == 1) {
            const char *v = strstr(line, "int16");
            if (v) {
                int rssi = atoi(v + 5);
                if (mac[0] && rssi < 0) bt_rssi_remember(mac, rssi);
            }
            want_value = 0;
        } else if (want_value == 2) {
            if (mac[0] && strstr(line, "boolean true")) {
                for (int i = 0; i < n; i++)
                    if (!strcasecmp(devs[i].mac, mac)) { devs[i].paired = 1; break; }
            }
            saw_paired_property = 1;
            want_value = 0;
        }
    }
    pclose(p);
    /* Applied from the cache rather than straight from the parse, so a device
     * bluez reported a moment ago but has since dropped the property for keeps
     * its last reading instead of reverting to unknown. */
    for (int i = 0; i < n; i++) devs[i].rssi = bt_rssi_recall(devs[i].mac);
    /* Only if bluez said nothing at all -- a missing dbus-send, say. Its answer
     * is authoritative when there is one, and the on-disk guess it falls back
     * to is exactly what this replaced. */
    if (!saw_paired_property)
        for (int i = 0; i < n; i++) devs[i].paired = bt_is_paired(devs[i].mac);
}

/* pair, then trust (so it reconnects on its own next time without asking
 * again), then connect -- the standard bluetoothctl sequence, scripted the
 * same way st_bt_name()'s own "info" lookup already pipes commands in.
 * Backgrounded: pairing a real device takes a few seconds of radio
 * handshake, which would otherwise freeze the UI thread for that whole
 * time -- same reasoning st_wifi_set() already gives for its own
 * backgrounded restart.
 *
 * Reported live and confirmed directly: this device's busybox has no
 * `timeout` applet at all ("applet not found"), and there's no standalone
 * /bin/timeout or /usr/bin/timeout either -- so the old `timeout 15
 * bluetoothctl ...` command never ran bluetoothctl at all, it failed at
 * the shell with "timeout: not found" before bluetoothctl was even
 * invoked. Silent, since this is backgrounded and its own output is
 * thrown away -- tapping a device in the UI looked like it did nothing,
 * because nothing is exactly what ran. `trust` never landing is also the
 * likely reason auto-reconnect never worked either: `bluetoothctl info`
 * on a device paired through this path confirmed "Paired: yes, Trusted:
 * no" -- BlueZ generally won't auto-accept a reconnection from a device
 * it doesn't trust without prompting, which nothing here is set up to
 * answer. Dropped the timeout wrapper entirely rather than hunt for a
 * substitute: bluetoothctl's own piped stdin hits EOF once printf's
 * output is exhausted (confirmed live -- st_bt_name()'s simpler "info"/
 * "quit" pipe already relies on exactly this and works), which is what
 * actually ends the session; explicit "quit" added for clarity, but the
 * EOF is what was really doing the job all along. */
/* R-reconnect: this used to be pair+trust+connect, one shot, no retry --
 * fine when bluez links on the first try, but bt_init's own boot-time
 * reconnect (see /usr/bin/bt_init's BT_LASTUSED section) exists precisely
 * because a plain `connect` often doesn't: it can hand back an LE-only
 * link with no audio profile at all (observed live: "< LE ..." with no
 * bluealsa PCM), or just time out, and nothing here noticed or retried.
 * Reported live: switching to any paired device OTHER than whichever one
 * bt_init's boot script last reconnected got no such retry budget, so it
 * was hostage to a single attempt succeeding outright -- the boot path's
 * own reliability came entirely from the retry loop, not from anything
 * about being at boot. Ported that same loop here: ConnectProfile for the
 * A2DP Sink UUID specifically (0000110b-..., asks bluez for audio rather
 * than letting it pick and possibly hand back LE), a plain `connect`
 * fallback, and an actual bluealsa-cli A2DP-PCM check for success rather
 * than trusting hcitool con's mere presence of a link. Same backoff
 * schedule bt_init uses, since that is what live testing showed this
 * chip/stack combination actually needs.
 *
 * Backgrounded, same as before -- a real link can take tens of seconds.
 * Result written to /usr/data/bt_pair_status ("<mac> ok" or "<mac>
 * failed") once the loop is done either way, so the Settings screen's tap
 * handler can poll bt_pair_result() and clear its "Connecting..." row
 * without the UI thread blocking on any of this. */
void bt_pair(const char *mac) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "("
        "_mac=%s; "
        "printf 'agent NoInputNoOutput\\ndefault-agent\\npair '$_mac'\\ntrust '$_mac'\\nquit\\n' | bluetoothctl >/dev/null 2>&1; "
        "_path=/org/bluez/hci0/dev_$(echo $_mac | tr ':' '_'); "
        "_pcm_up() { bluealsa-cli list-pcms 2>/dev/null | grep -qi \"$(echo $_mac | tr ':' '_').*a2dp\"; }; "
        "_ok=0; "
        "for _gap in 1 2 3 5 5 5 10 10 10 10; do "
        "_pcm_up && { _ok=1; break; }; "
        "dbus-send --system --reply-timeout=8000 --dest=org.bluez \"$_path\" org.bluez.Device1.ConnectProfile string:0000110b-0000-1000-8000-00805f9b34fb >/dev/null 2>&1; "
        "_pcm_up && { _ok=1; break; }; "
        "printf 'agent NoInputNoOutput\\ndefault-agent\\nconnect '$_mac'\\nquit\\n' | bluetoothctl >/dev/null 2>&1; "
        "_pcm_up && { _ok=1; break; }; "
        "sleep $_gap; "
        "done; "
        "echo \"$_mac $([ $_ok = 1 ] && echo ok || echo failed)\" > /usr/data/bt_pair_status"
        ") >/dev/null 2>&1 &",
        mac);
    if (system(cmd) == -1) return;
}

int bt_pair_result(const char *mac) {
    FILE *f = fopen("/usr/data/bt_pair_status", "r");
    if (!f) return 0;
    char line[64] = "";
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (!got) return 0;
    char rmac[24] = "", rstate[16] = "";
    if (sscanf(line, "%23s %15s", rmac, rstate) != 2) return 0;
    if (strcmp(rmac, mac) != 0) return 0;
    if (!strcmp(rstate, "ok")) return 1;
    if (!strcmp(rstate, "failed")) return 2;
    return 0;
}

/* ---- Wi-Fi scanning -------------------------------------------------------
 * RP2 remaining scope: SC_SETTINGS_WIFI could join a network typed in by
 * hand but not list nearby ones and tap to join, the one thing Bluetooth's
 * own settings screen already did. Same wpa_cli this file's other Wi-Fi
 * functions already rely on (st_wifi_ssid(), music_hook.c's wifi_connect()),
 * not a new tool. */

/* `wpa_cli scan` triggers an async scan and returns immediately ("OK") --
 * unlike bt_scan_start(), nothing here needs backgrounding or a sleep to
 * hold a session open, since the scan itself runs inside wpa_supplicant,
 * not inside this command. Results trickle in over the next couple of
 * seconds; the caller polls wifi_scan_results() same as the Bluetooth
 * screen already polls bt_scan_devices(). */
void wifi_scan_start(void) {
    if (system("wpa_cli -i wlan0 scan >/dev/null 2>&1") == -1) return;
}

/* One row per SSID, strongest signal first -- `wpa_cli scan_results` lists
 * one row per BSSID, so the same network reachable from two access points
 * (or just re-broadcasting on overlapping channels) would otherwise show
 * as duplicate rows for the same name. A blank SSID (a hidden network,
 * broadcasting no name) is dropped -- nothing to tap it with; hidden
 * networks still go through "Add network manually", same as before this
 * existed. `open` is set when neither WPA1/2/3 marker is present in the
 * flags field -- close enough for whether to skip straight to connecting
 * or ask for a password first; WEP is treated as needing a password too
 * (wpa_cli's own psk-only wifi_connect() can't join a WEP network anyway,
 * so this at least won't silently mis-offer a one-tap connect that would
 * fail). */
int wifi_scan_results(wifi_found_net_t *out, int max) {
    char buf[4096];
    run_cmd("wpa_cli -i wlan0 scan_results 2>/dev/null", buf, sizeof(buf));
    int n = 0;
    char *line = strchr(buf, '\n');   /* skip the "bssid / frequency / ..." header */
    line = line ? line + 1 : NULL;
    while (line && *line && n < max) {
        char *nl = strchr(line, '\n');
        size_t linelen = nl ? (size_t)(nl - line) : strlen(line);
        char *f1 = memchr(line, '\t', linelen);
        char *f2 = f1 ? memchr(f1 + 1, '\t', linelen - (size_t)(f1 + 1 - line)) : NULL;
        char *f3 = f2 ? memchr(f2 + 1, '\t', linelen - (size_t)(f2 + 1 - line)) : NULL;
        char *f4 = f3 ? memchr(f3 + 1, '\t', linelen - (size_t)(f3 + 1 - line)) : NULL;
        if (f1 && f2 && f3 && f4) {
            int signal = atoi(f2 + 1);
            size_t flags_len = (size_t)(f4 - (f3 + 1));
            char flags[160];
            if (flags_len >= sizeof(flags)) flags_len = sizeof(flags) - 1;
            memcpy(flags, f3 + 1, flags_len);
            flags[flags_len] = '\0';
            const char *ssid_raw = f4 + 1;
            size_t ssid_len = (size_t)((line + linelen) - ssid_raw);
            char ssid[64];
            wpa_unescape(ssid_raw, ssid_len, ssid, sizeof(ssid));
            if (ssid[0]) {
                int dup = -1;
                for (int i = 0; i < n; i++)
                    if (!strcmp(out[i].ssid, ssid)) { dup = i; break; }
                int open = !strstr(flags, "WPA") && !strstr(flags, "WEP");
                if (dup >= 0) {
                    if (signal > out[dup].signal) { out[dup].signal = signal; out[dup].open = open; }
                } else {
                    snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", ssid);
                    out[n].signal = signal;
                    out[n].open = open;
                    n++;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    /* Selection sort by signal, strongest first -- n is small (one screen's
     * worth of nearby networks), same cost tradeoff prune_cache() already
     * accepts elsewhere in this codebase for a short, infrequent list. */
    for (int i = 0; i < n - 1; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) if (out[j].signal > out[best].signal) best = j;
        if (best != i) { wifi_found_net_t t = out[i]; out[i] = out[best]; out[best] = t; }
    }
    return n;
}

/* ---- quick settings ------------------------------------------------------ */
#define BACKLIGHT_NODE "/sys/class/backlight/backlight_pwm0"

int st_brightness(void)     { return read_int_file(BACKLIGHT_NODE "/brightness", 0); }
int st_brightness_max(void) { return read_int_file(BACKLIGHT_NODE "/max_brightness", 101); }

void st_brightness_set(int v) {
    int max = st_brightness_max();
    if (v < 1) v = 1;                     /* never all the way off from here */
    /* Never the literal max either -- the user reported the quick-settings
     * slider going to 100% turns the screen black rather than brightest.
     * The likely mechanism: this PWM backlight's duty-cycle register wraps
     * to 0% at its own period value instead of clamping at full-on, so
     * max_brightness itself (101) is one past the last safe value. max-1
     * is visually indistinguishable from max at every brightness this
     * panel has, so nothing is lost by never writing it. */
    if (v > max - 1) v = max - 1;
    FILE *f = fopen(BACKLIGHT_NODE "/brightness", "w");
    if (!f) return;
    fprintf(f, "%d", v);
    fclose(f);
}

/* wifi_off.sh takes the interface down, so operstate is the honest answer. */
int st_wifi_on(void) {
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (!f) return 0;
    char s[16] = "";
    if (!fgets(s, sizeof(s), f)) s[0] = '\0';
    fclose(f);
    return strncmp(s, "down", 4) != 0;
}

/* No sysfs equivalent: rfkill reads unblocked whether the adapter is powered
 * or not, so it has to be asked. Only called while the panel is open. */
int st_bt_on(void) {
    FILE *p = popen("hciconfig hci0 2>/dev/null", "r");
    if (!p) return 0;
    char line[256];
    int up = 0;
    while (fgets(line, sizeof(line), p))
        if (strstr(line, "UP RUNNING")) { up = 1; break; }
    pclose(p);
    return up;
}

/* Backgrounded: wifi_on.sh restarts wpa_supplicant and waits on DHCP, which is
 * seconds. Blocking the UI on that would look like a crash. */
void st_wifi_set(int on) {
    if (system(on ? "/usr/bin/wifi_on.sh >/dev/null 2>&1 &"
                  : "/usr/bin/wifi_off.sh >/dev/null 2>&1 &") == -1) return;
}

/* R64: /etc/init.d/S80_bt_init backgrounds /usr/bin/bt_init and returns
 * immediately, so this app's own startup (S92, right after S80 in the boot
 * sequence) runs *in parallel* with bt_init's ~15+ second sequence, not
 * after it -- and that sequence's very last command is an explicit
 * `bt-adapter --set Powered Off`, added upstream so stock firmware starts
 * with Bluetooth off until its own UI turns it on. A plain "on" call made
 * during that window (R64's boot-time restore, when it wants Bluetooth back
 * on) would very likely land *before* that trailing Off and then get
 * silently overwritten by it moments later -- reported live as the restore
 * never taking, every time, on a real reboot.
 *
 * bt_init itself touches /tmp/bt_init_ok as its last line, after that Off,
 * so waiting for that file first and then re-asserting "on" always comes
 * after, not before. Capped at 20s (bt_init_ok lands ~8.4s into a cold
 * boot, measured; it was ~16.3s before its fixed sleeps became condition
 * waits) so a firmware or init failure that never creates the file
 * can't hang this forever -- it just falls through to enabling immediately,
 * the same behavior as before this existed. A no-op wait on every call
 * after boot, once the file already exists, so this is safe to leave in
 * st_bt_set() itself rather than only in the boot-restore path: busybox has
 * no `timeout` applet (see bt_pair()'s own history with that), hence the
 * hand-rolled bounded loop rather than wrapping this in one. */
void st_bt_set(int on) {
    if (!on) {
        if (system("/usr/bin/bt_disable >/dev/null 2>&1 &") == -1) return;
        return;
    }
    /* 50ms, not 500ms. bt_init_ok lands ~8.4s into boot and this loop is
     * what gates bt_enable, so the poll granularity is paid on every cold
     * boot -- 0.25s wasted on average, measured. usleep is safe to tighten
     * here because the condition is a file test, a shell builtin with no
     * process spawn; the same trick applied to bt_init's command-based
     * waits was actively harmful (spawn costs 4.3ms on this device, so a
     * 10ms tick on "hciconfig | grep" is ~86% of the core, and it cost a
     * boot in ten). Same 20s cap, now 400 iterations rather than 40. */
    if (system("(i=0; while [ ! -f /tmp/bt_init_ok ] && [ $i -lt 400 ]; do "
               "usleep 50000; i=$((i+1)); done; /usr/bin/bt_enable) "
               ">/dev/null 2>&1 &") == -1) return;
}

/* USB working mode, take 2. The first version wrote a byte into
 * /usr/data/user.ini at an offset our own boot-ADB wrapper (S90adb) reads
 * on the way in -- but that script only confirms what 0 and 1 mean to
 * *itself* (whether to bother starting ADB), never what hiby_player's own
 * native code does with the byte, or when it acts on it. A setting that
 * might only take effect God-knows-when is not "switching modes", it's
 * hoping. Reverted -- see git history if that offset is ever worth
 * revisiting with real evidence.
 *
 * This version calls the exact same commands stock's own adbon/adboff
 * (/usr/bin/adbon, /usr/bin/adboff) already run, confirmed by reading
 * them directly: they toggle between ADB and USB mass-storage by
 * stopping one gadget function and starting the other on the *live*
 * configfs gadget, not by writing a preference anywhere. Proven safe by
 * ordinary use, not guessed at -- the actual fix for the same lesson the
 * first version was trying to apply and didn't quite land: don't invent
 * behaviour for undocumented state, reuse the vendor's own, already-
 * working mechanism instead. */
int st_usb_mode(void) {
    /* Live, not persisted: which gadget the UDC is actually bound to right
     * now. adb_demo bound + adbd running -> ADB mode; android0's
     * mass_storage.0 bound -> Storage mode. Mirrors how st_bt_on() already
     * asks the real hardware state instead of trusting a cached flag. */
    FILE *f = fopen("/sys/kernel/config/usb_gadget/adb_demo/UDC", "r");
    if (f) {
        char s[64] = "";
        if (fgets(s, sizeof(s), f)) { /* non-empty line = bound */ }
        fclose(f);
        if (s[0] && s[0] != '\n') return 0;   /* ADB */
    }
    f = fopen("/sys/kernel/config/usb_gadget/android0/UDC", "r");
    if (f) {
        char s[64] = "";
        if (fgets(s, sizeof(s), f)) { }
        fclose(f);
        if (s[0] && s[0] != '\n') return 1;   /* Storage */
    }
    return -1;   /* neither bound -- e.g. USB not plugged in */
}

/* Switching TO Storage mode (mode == 1) stops ADB as an unavoidable part
 * of what adboff actually does -- the two gadget functions need exclusive
 * ownership of the one UDC, same as DAC/OTG do per S90adb's own comment.
 * A session driving this over ADB will be disconnected by its own action;
 * that's adboff working correctly, not a bug, and switching back to ADB
 * mode needs the device's own touchscreen from that point (expected, and
 * why this was never tested live from this side of an ADB connection --
 * doing so would strand the very session testing it, with no way back
 * except the screen). Backgrounded either way: both scripts bring up a
 * new gadget and, for ADB, wait on enumeration -- not instant. */
void st_usb_mode_set(int mode) {
    if (system(mode == 1 ? "/usr/bin/adboff >/dev/null 2>&1 &"
                          : "/usr/bin/adbon >/dev/null 2>&1 &") == -1) return;
}

/* Radio needs a route off the device. wlan0 exists whether or not Wi-Fi is
 * switched on, so the interface being present says nothing — operstate does. */
int st_net_up(void) {
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    if (!f) return 0;
    char s[16] = "";
    if (!fgets(s, sizeof(s), f)) s[0] = '\0';
    fclose(f);
    return strncmp(s, "up", 2) == 0;
}

int st_bt_pcm_path(char *out, unsigned n) { return bt_pcm_path(out, n); }
