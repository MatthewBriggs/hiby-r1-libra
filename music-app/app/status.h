#ifndef MUSIC_STATUS_H
#define MUSIC_STATUS_H
int st_battery_pct(void);    /* 0-100, -1 unknown */
int st_charging(void);
int st_headset(void);        /* jack occupied */
int st_net_up(void);         /* Wi-Fi actually associated, not merely present */

/* Bluetooth sink details, cached. Both return 0/empty when nothing is
 * connected or the firmware does not report it. */
void st_bt_codec(char *out, unsigned n);
int  st_bt_battery(void);    /* percent, -1 unknown */
/* What each radio is attached to, for the quick-settings panel. Empty when
 * nothing is. */
void st_wifi_ssid(char *out, unsigned n);
void st_bt_name(char *out, unsigned n);

/* Bluetooth pairing. NoInputNoOutput throughout -- see bt_scan_start()'s
 * own comment in status.c for why, and what it means a device needing a
 * PIN/passkey will do (fail cleanly, not hang). */
/* rssi: signal strength in dBm (always negative), or 0 for "not known". bluez
 * only carries an RSSI for a device its own discovery has actually heard from
 * recently, so a paired device that is merely switched on -- or any device at
 * all when no scan is running -- legitimately has none.
 *
 * paired: bluez's own Paired property, filled by bt_fill_details(). */
typedef struct { char mac[18]; char name[48]; int rssi; int paired; } bt_found_dev_t;
/* The a2dp sink PCM of the connected headset, as bluealsa names it, or 0 if
 * there is none. Same match bt_pcm_path() documents internally. */
int st_bt_pcm_path(char *out, unsigned n);
void bt_scan_start(void);
int  bt_scan_devices(bt_found_dev_t *out, int max);   /* returns count found, <= max */
/* Fills in the rssi and paired fields of an already-populated
 * bt_scan_devices() array. One D-Bus round trip for the whole list rather than
 * a call per device -- measured on hardware at ~3x cheaper than the
 * `bluetoothctl devices` call that built the list in the first place.
 *
 * rssi is left 0 where no reading has ever been seen, and readings are
 * remembered between calls because bluez only publishes one while a scan is
 * actually running. paired comes from bluez's own Paired property rather than
 * bt_is_paired()'s look for an on-disk info file -- bluez writes those for
 * devices it has merely discovered too, which put a pile of passing strangers
 * in the Paired section the first time a scan was run next to this list. */
void bt_fill_details(bt_found_dev_t *devs, int n);
int  bt_is_paired(const char *mac);                   /* has a bluez pairing record on disk */
void bt_pair(const char *mac);
/* Polls the result bt_pair()'s own backgrounded retry loop writes to
 * /usr/data/bt_pair_status once it finishes with this mac, win or lose --
 * see bt_pair()'s comment. 0 while no result for this mac has landed yet
 * (still running, or nothing was ever tapped), 1 for connected, 2 for
 * gave up. Lets the Settings screen's tap handler know when to stop
 * showing "Connecting...". */
int  bt_pair_result(const char *mac);

/* RP2: Wi-Fi scan-and-select, the same shape as Bluetooth's just above.
 * `open` is 1 for a network with no WPA/WEP marker in its flags -- the
 * caller can connect straight away rather than asking for a password. */
typedef struct { char ssid[64]; int signal; int open; } wifi_found_net_t;
void wifi_scan_start(void);
int  wifi_scan_results(wifi_found_net_t *out, int max);   /* returns count found, <= max, strongest first */

/* Quick settings. The radios are driven through the firmware's own scripts
 * rather than by poking interfaces directly — wifi_on.sh restores the saved
 * network, which hand-rolled ifconfig would not. */
int  st_wifi_on(void);
int  st_bt_on(void);
void st_wifi_set(int on);
void st_bt_set(int on);
/* Live gadget state, not a persisted preference -- -1 = unknown/neither
 * bound, 0 = ADB, 1 = Storage. Set drives stock's own adbon/adboff. See
 * st_usb_mode()'s own comment in status.c for why, and for what switching
 * to Storage does to whatever's currently driving this over ADB. */
int  st_usb_mode(void);
void st_usb_mode_set(int mode);
int  st_brightness(void);
int  st_brightness_max(void);
void st_brightness_set(int v);
#endif
