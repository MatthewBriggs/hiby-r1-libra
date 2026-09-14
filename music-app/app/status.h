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
typedef struct { char mac[18]; char name[48]; } bt_found_dev_t;
void bt_scan_start(void);
int  bt_scan_devices(bt_found_dev_t *out, int max);   /* returns count found, <= max */
int  bt_is_paired(const char *mac);                   /* has a bluez pairing record on disk */
void bt_pair(const char *mac);

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
