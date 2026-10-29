/* ui_settings.c — Settings and its pages, the EQ and MSEB, Stats, a
 * podcast's own settings, and the keyboard. Part of ui_libra.c's translation
 * unit; the actions are Libra's own, from its old tap handler. */

/* ---- table-driven setting rows ---------------------------------------------
 * A row: title, an optional two-line description, and on the right either a
 * switch (toggle) or a value; act() is the tap. refresh() re-reads them. */
typedef struct {
    const char *title, *desc1, *desc2;
    int (*toggle)(void);
    void (*value)(char *buf, size_t n, uint16_t *col);
    void (*act)(void);
    int (*dim)(void);
    uint16_t title_col;            /* 0: COL_TEXT */
} set_row_t;

#define SET_MAX 24
static struct {
    const set_row_t *rows;
    int n;
    lv_obj_t *sw[SET_MAX], *val[SET_MAX], *title[SET_MAX], *row[SET_MAX];
} sr;

static void sr_tap(lv_event_t *e) {
    int i = ui_idx(e);
    if (i >= 0 && i < sr.n && sr.rows[i].act) sr.rows[i].act();
    if (ui.refresh) ui.refresh();
    UI_ACTED();
}

static void sr_refresh(void) {
    char buf[96];
    for (int i = 0; i < sr.n; i++) {
        const set_row_t *r = &sr.rows[i];
        if (r->toggle) ui_switch_set(sr.sw[i], r->toggle());
        if (r->value) {
            uint16_t col = COL_DIM;
            buf[0] = '\0';
            r->value(buf, sizeof(buf), &col);
            ui_set_text(sr.val[i], buf);
            ui_set_color(sr.val[i], col);
            lv_obj_align(sr.val[i], LV_ALIGN_RIGHT_MID, -24, 0);
        }
        if (r->dim) ui_set_color(sr.title[i], r->dim() ? COL_DIM : r->title_col ? r->title_col : COL_TEXT);
    }
}

/* Builds the rows into `l` from y; returns the y below them. */
static int sr_build(lv_obj_t *l, int y, const set_row_t *rows, int n) {
    sr.rows = rows;
    sr.n = n > SET_MAX ? SET_MAX : n;
    for (int i = 0; i < sr.n; i++) {
        const set_row_t *r = &rows[i];
        int h = r->desc1 ? ROW_H + 52 : ROW_H;
        lv_obj_t *row = sr.row[i] = ui_row(l, y, h, r->act ? sr_tap : NULL, i);
        sr.title[i] = ui_label_w(row, r->title, uf_body, r->title_col ? r->title_col : COL_TEXT,
                                 FB_W - 48 - (r->toggle || r->value ? 140 : 0));
        lv_obj_set_pos(sr.title[i], 24, 20);
        if (r->desc1) {
            lv_obj_t *d1 = ui_label_w(row, r->desc1, uf_small, COL_DIM, FB_W - 48);
            lv_obj_set_pos(d1, 24, ROW_H - 8);
            if (r->desc2) {
                lv_obj_t *d2 = ui_label_w(row, r->desc2, uf_small, COL_DIM, FB_W - 48);
                lv_obj_set_pos(d2, 24, ROW_H + 18);
            }
        }
        sr.sw[i] = r->toggle ? ui_switch(row, r->toggle(), h) : NULL;
        if (r->value) {
            sr.val[i] = ui_label(row, "", uf_small, COL_DIM);
            lv_obj_set_width(sr.val[i], LV_SIZE_CONTENT);
        } else {
            sr.val[i] = NULL;
        }
        y += h;
    }
    return y;
}

static void ui_open(screen_t s) { screen = s; reset_scroll(); }

/* ---- Settings --------------------------------------------------------------- */

static int t_lock(void) { return button_lock_enabled; }
static void a_lock(void) { button_lock_enabled = !button_lock_enabled; save_conf(); }
static int t_usbbypass(void) { return usb_bypass_enabled; }
static void a_usbbypass(void) { usb_bypass_enabled = !usb_bypass_enabled; save_conf(); }
static int t_btautoplay(void) { return bt_autoplay_enabled; }
static void a_btautoplay(void) { bt_autoplay_enabled = !bt_autoplay_enabled; save_conf(); }
static void v_volsteps(char *b, size_t n, uint16_t *c) { snprintf(b, n, "%d", audio_bt_vol_steps()); *c = COL_ACCENT; }
static void a_volsteps(void) {
    bt_vol_step_idx = (bt_vol_step_idx + 1) % BT_VOL_STEP_CHOICE_N;
    audio_set_bt_vol_steps(BT_VOL_STEP_CHOICES[bt_vol_step_idx]);
    save_conf();
}
static void v_screento(char *b, size_t n, uint16_t *c) {
    if (idle_lock_s == 0)      snprintf(b, n, "Never");
    else if (idle_lock_s < 60) snprintf(b, n, "%d s", idle_lock_s);
    else                       snprintf(b, n, "%d min", idle_lock_s / 60);
    *c = COL_ACCENT;
}
static void a_screento(void) {
    int i = 0;
    while (i < SCREEN_TO_CHOICE_N && SCREEN_TO_CHOICES[i] != idle_lock_s) i++;
    idle_lock_s = SCREEN_TO_CHOICES[(i + 1) % SCREEN_TO_CHOICE_N];
    save_conf();
}
static void v_autooff(char *b, size_t n, uint16_t *c) {
    if (auto_off_minutes() == 0) snprintf(b, n, "Never");
    else snprintf(b, n, "%d min", auto_off_minutes());
    *c = COL_ACCENT;
}
static void a_autooff(void) { auto_off_idx = (auto_off_idx + 1) % AUTO_OFF_CHOICE_N; save_conf(); }
static void v_sleep(char *b, size_t n, uint16_t *c) {
    if (!deep_sleep_enabled || sleep_minutes() == 0) snprintf(b, n, "Off");
    else snprintf(b, n, "%d min", sleep_minutes());
    *c = COL_ACCENT;
}
static void a_sleep(void) {
    if (!deep_sleep_enabled || sleep_idx == 0) { deep_sleep_enabled = 1; sleep_idx = 1; }
    else if (sleep_idx == SLEEP_CHOICE_N - 1) deep_sleep_enabled = 0;
    else sleep_idx++;
    save_conf();
}
static void v_thememode(char *b, size_t n, uint16_t *c) { snprintf(b, n, "%s", THEME_MODE_NAMES[theme_mode]); *c = COL_ACCENT; }
static void a_thememode(void) { ui_open(SC_SETTINGS_THEMEMODE); }
static void v_tz(char *b, size_t n, uint16_t *c) { snprintf(b, n, "%s", TZ_PRESETS[tz_idx].name); *c = COL_ACCENT; }
static void a_tz(void) { ui_open(SC_SETTINGS_TIMEZONE); }
static void v_accent(char *b, size_t n, uint16_t *c) {
    snprintf(b, n, "%s", ACCENT_PRESETS[g_accent_idx].name);
    *c = ACCENT_PRESETS[g_accent_idx].color;
}
static void a_accent(void) { ui_open(SC_SETTINGS_THEME); }
static int t_coverpal(void) { return cover_palette_enabled; }
static void a_coverpal(void) {
    cover_palette_enabled = !cover_palette_enabled;
    np_palette_seq = -1;
    if (cover_palette_enabled) cover_prewarm_start();
    save_conf();
}
static int t_artround(void) { return art_round_enabled; }
static void a_artround(void) { art_round_enabled = !art_round_enabled; save_conf(); }
static void a_about(void) { ui_open(SC_SETTINGS_ABOUT); }
static void v_wifi(char *b, size_t n, uint16_t *c) {
    int on = st_wifi_shown();
    char nm[64] = "";
    if (on) st_wifi_ssid(nm, sizeof(nm));
    snprintf(b, n, "%s", on ? (nm[0] ? nm : st_wifi_joining() ? "connecting..." : "not connected")
                            : wifi_on_blocked() ? "off for Bluetooth" : "off");
    *c = COL_DIM;
}
static void a_wifi(void) { ui_open(SC_SETTINGS_WIFI); }
static void v_bt(char *b, size_t n, uint16_t *c) {
    int on = st_bt_on();
    char nm[64] = "";
    if (on) st_bt_name(nm, sizeof(nm));
    snprintf(b, n, "%s", on ? (nm[0] ? nm : "not connected") : "off");
    *c = COL_DIM;
}
static void a_bt(void) { ui_open(SC_SETTINGS_BT); }
static void v_usb(char *b, size_t n, uint16_t *c) {
    int m = st_usb_mode();
    snprintf(b, n, "%s", m == 0 ? "ADB" : m == 1 ? "USB Storage" : "Unplugged");
    *c = COL_DIM;
}
static void a_usb(void) { ui_open(SC_SETTINGS_USB); }
static int d_scan(void) { return st_usb_mode() == 1; }
static void v_scan(char *b, size_t n, uint16_t *c) {
    int scanned = 0, written = 0;
    int started = scanner_scan_progress(&scanned, &written);
    if (st_usb_mode() == 1) snprintf(b, n, "USB Storage active");
    else if (scanner_scan_running() || index_scan_running()) snprintf(b, n, "Scanning\xe2\x80\xa6 %d", scanned);
    else if (started) snprintf(b, n, "%d files found", scanned);
    else snprintf(b, n, "Not started");
    *c = COL_DIM;
}
static void a_scan(void) { if (st_usb_mode() != 1) scanner_rescan_now(); }

static const set_row_t SETTINGS_ROWS[] = {
    { "Power button lock", "Double-press power to lock the screen and",
      "disable buttons. Double-press again to undo.", t_lock, NULL, a_lock, NULL, 0 },
    { "USB Transport Mode", "Disables PEQ/MSEB/Bluetooth and locks volume",
      "at 50% while output is USB. Restored after.", t_usbbypass, NULL, a_usbbypass, NULL, 0 },
    { "Play on headset connect", "Resume playback automatically when a",
      "Bluetooth headset reconnects.", t_btautoplay, NULL, a_btautoplay, NULL, 0 },
    { "Volume steps", NULL, NULL, NULL, v_volsteps, a_volsteps, NULL, 0 },
    { "Screen timeout", NULL, NULL, NULL, v_screento, a_screento, NULL, 0 },
    { "Auto shutdown", "Powers the device off when locked with",
      "nothing playing. Tap to change.", NULL, v_autooff, a_autooff, NULL, 0 },
    { "Sleep", "Suspends when locked with nothing playing;",
      "power button wakes it. Experimental.", NULL, v_sleep, a_sleep, NULL, 0 },
    { "Theme", NULL, NULL, NULL, v_thememode, a_thememode, NULL, 0 },
    { "Timezone", NULL, NULL, NULL, v_tz, a_tz, NULL, 0 },
    { "Accent colour", NULL, NULL, NULL, v_accent, a_accent, NULL, 0 },
    { "Cover colours", "Now Playing's background and accent follow",
      "the current album art (Music only).", t_coverpal, NULL, a_coverpal, NULL, 0 },
    { "Rounded art corners", NULL, NULL, t_artround, NULL, a_artround, NULL, 0 },
    { "About", NULL, NULL, NULL, NULL, a_about, NULL, 0 },
    { "Wi-Fi", NULL, NULL, NULL, v_wifi, a_wifi, NULL, 0 },
    { "Bluetooth", NULL, NULL, NULL, v_bt, a_bt, NULL, 0 },
    { "USB working mode", NULL, NULL, NULL, v_usb, a_usb, NULL, 0 },
    { "Scan library", NULL, NULL, NULL, v_scan, a_scan, d_scan, 0 },
};

static void ui_build_settings(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    sr_build(l, 0, SETTINGS_ROWS, (int)(sizeof(SETTINGS_ROWS) / sizeof(SETTINGS_ROWS[0])));
    ui.refresh = sr_refresh;
}

/* ---- pickers: accent, timezone, theme, USB mode ----------------------------- */

static lv_obj_t *ui_tick(lv_obj_t *row) {
    lv_obj_t *t = ui_label(row, LV_SYMBOL_OK, (lv_font_t *)&lv_font_montserrat_28, COL_ACCENT);
    lv_obj_align(t, LV_ALIGN_RIGHT_MID, -30, 0);
    return t;
}

static void ui_accent_tap(lv_event_t *e) {
    int idx = ui_idx(e);
    if (idx >= 0 && idx < ACCENT_N) {
        g_accent_idx = idx;
        g_accent = ACCENT_PRESETS[idx].color;
        save_conf();
    }
    UI_ACTED();
}

static void ui_build_settings_theme(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    for (int i = 0; i < ACCENT_N; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_accent_tap, i);
        ui_circle(row, 44, ROW_H / 2, 14, ACCENT_PRESETS[i].color, 0);
        lv_obj_t *t = ui_label(row, ACCENT_PRESETS[i].name, uf_body, COL_TEXT);
        lv_obj_set_pos(t, 72, 20);
        if (i == g_accent_idx) ui_tick(row);
    }
}

static void ui_tz_tap(lv_event_t *e) {
    int idx = ui_idx(e);
    if (idx >= 0 && idx < TZ_N) { tz_idx = idx; save_conf(); ui_rebuild_req = 1; }
    UI_ACTED();
}

static void ui_build_settings_timezone(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    for (int i = 0; i < TZ_N; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_tz_tap, i);
        lv_obj_t *t = ui_label_w(row, TZ_PRESETS[i].name, uf_body, COL_TEXT, FB_W - 124);
        lv_obj_set_pos(t, 24, 20);
        if (i == tz_idx) ui_tick(row);
    }
}

static void ui_thememode_tap(lv_event_t *e) {
    int idx = ui_idx(e);
    if (idx >= 0 && idx < THEME_MODE_N) { theme_mode = idx; apply_theme(); save_conf(); }
    UI_ACTED();
}

static void ui_build_settings_thememode(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    for (int i = 0; i < THEME_MODE_N; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_thememode_tap, i);
        lv_obj_t *t = ui_label(row, THEME_MODE_NAMES[i], uf_body, COL_TEXT);
        lv_obj_set_pos(t, 24, 20);
        if (i == theme_mode) ui_tick(row);
    }
}

static void ui_usbmode_tap(lv_event_t *e) {
    int idx = ui_idx(e);
    if (idx >= 0 && idx < 2) set_usb_storage_mode(idx);
    ui_rebuild_req = 1;
    UI_ACTED();
}

static void ui_build_settings_usb(void) {
    static const char *names[2] = { "ADB", "USB Storage" };
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    int cur = st_usb_mode();
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_usbmode_tap, i);
        lv_obj_t *t = ui_label(row, names[i], uf_body, COL_TEXT);
        lv_obj_set_pos(t, 24, 20);
        if (i == cur) ui_tick(row);
    }
    lv_obj_t *d = ui_label(l, "Switching to USB Storage disconnects ADB --\n"
                              "the two need the USB port exclusively.", uf_small, COL_DIM);
    lv_obj_set_pos(d, 24, 2 * ROW_H + 20);
}

/* ---- About -------------------------------------------------------------------- */

static void ui_build_settings_about(void) {
    char kernel[64], firmware[32], serial[40], sd_free[24], ram_free[24];
    about_kernel(kernel, sizeof(kernel));
    about_firmware(firmware, sizeof(firmware));
    about_serial(serial, sizeof(serial));
    about_sd_free(sd_free, sizeof(sd_free));
    about_ram_free(ram_free, sizeof(ram_free));
    const char *rows[][2] = {
        { "Libra version", LIBRARY_VERSION " (LVGL)" },
        { "Firmware version", firmware },
        { "Kernel version", kernel },
        { "Serial number", serial },
        { "SD card", sd_free },
        { "Memory", ram_free },
    };
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    for (int i = 0; i < 6; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, NULL, i);
        lv_obj_t *t = ui_label(row, rows[i][0], uf_body, COL_TEXT);
        lv_obj_set_pos(t, 24, 20);
        ui_right(row, rows[i][1], COL_DIM, 22);
    }
}

/* ---- Wi-Fi ---------------------------------------------------------------------- */

static uint32_t wifi_list_hash;

static int t_wifi_on(void) { return st_wifi_shown(); }
static void a_wifi_on(void) {
    if (!st_wifi_shown() && wifi_on_blocked()) { wifi_on_refused("Settings"); return; }
    int on = !st_wifi_shown();
    st_wifi_set(on);
    mlog("[music] wifi: %s from Settings\n", on ? "on" : "off");
    wifi_pref = on;
    wifi_bt_parked = 0;
    if (on && net_held()) wifi_bt_override = 1;
    save_conf();
}
static void v_wifi_status(char *b, size_t n, uint16_t *c) {
    int on = st_wifi_shown();
    char nm[64] = "";
    if (on && st_wifi_carrier()) st_wifi_ssid(nm, sizeof(nm));
    if (!on && wifi_block_note_until > time(NULL)) wifi_block_text(b, n, 0);
    else if (!on && wifi_bt_parked) snprintf(b, n, "off while playing over Bluetooth");
    else if (!on)               snprintf(b, n, "off");
    else if (!nm[0])            snprintf(b, n, st_wifi_joining() ? "on, connecting..." : "on, not connected");
    else if (!st_wifi_has_ip()) snprintf(b, n, "%s, getting an address", nm);
    else                        snprintf(b, n, "%s", nm);
    *c = COL_DIM;
}
static int t_wifi_idle(void) { return wifi_auto_off_enabled; }
static void a_wifi_idle(void) { wifi_auto_off_enabled = !wifi_auto_off_enabled; save_conf(); }
static int t_wifi_bt(void) { return wifi_bt_off_enabled; }
static void a_wifi_bt(void) { wifi_bt_off_enabled = !wifi_bt_off_enabled; save_conf(); }
static void a_wifi_scan(void) { wifi_scan_start(); }
static void a_wifi_manual(void) { kb_open("Network name (SSID)", KB_PURPOSE_WIFI_SSID_MANUAL, ""); }

static const set_row_t WIFI_ROWS[] = {
    { "Wi-Fi", NULL, NULL, t_wifi_on, NULL, a_wifi_on, NULL, 0 },
    { "Status", NULL, NULL, NULL, v_wifi_status, NULL, NULL, 0 },
    { "Turn off after 15 min idle", NULL, NULL, t_wifi_idle, NULL, a_wifi_idle, NULL, 0 },
    { "Off during Bluetooth playback", NULL, NULL, t_wifi_bt, NULL, a_wifi_bt, NULL, 0 },
    { "Scan for networks", NULL, NULL, NULL, NULL, a_wifi_scan, NULL, 1 },
    { "Add network manually", NULL, NULL, NULL, NULL, a_wifi_manual, NULL, 1 },
};

static void ui_wifi_net_tap(lv_event_t *e) {
    int idx = ui_idx(e);
    if (idx >= 0 && idx < wifi_net_n) {
        if (wifi_is_saved(wifi_nets[idx].ssid)) {
            snprintf(wifi_connecting_ssid, sizeof(wifi_connecting_ssid), "%s", wifi_nets[idx].ssid);
            wifi_connecting_since = time(NULL);
            wifi_select_saved(wifi_nets[idx].ssid);
        } else if (wifi_nets[idx].open) {
            snprintf(wifi_connecting_ssid, sizeof(wifi_connecting_ssid), "%s", wifi_nets[idx].ssid);
            wifi_connecting_since = time(NULL);
            wifi_connect(wifi_nets[idx].ssid, "");
        } else {
            snprintf(kb_wifi_target_ssid, sizeof(kb_wifi_target_ssid), "%s", wifi_nets[idx].ssid);
            kb_open("Password for this network", KB_PURPOSE_WIFI_PASSWORD, "");
        }
    }
    ui_rebuild_req = 1;
    UI_ACTED();
}

static void ui_wifi_net_hold(lv_event_t *e) {
    int idx = ui_idx(e);
    if (idx >= 0 && idx < wifi_net_n && wifi_is_saved(wifi_nets[idx].ssid)) {
        snprintf(sheet_wifi_ssid, sizeof(sheet_wifi_ssid), "%s", wifi_nets[idx].ssid);
        sheet_wifi_secured = !wifi_nets[idx].open;
        sheet_open = 7;
    }
    lv_indev_reset(lv_indev_active(), NULL);   /* no tap on release */
    UI_ACTED();
}

static const char *wifi_tag(int i, int on, const char *nm) {
    int connecting = wifi_connecting_ssid[0] && !strcmp(wifi_nets[i].ssid, wifi_connecting_ssid);
    return connecting ? "Connecting..."
         : (on && nm[0] && !strcmp(nm, wifi_nets[i].ssid)) ? "connected"
         : wifi_is_saved(wifi_nets[i].ssid) ? "saved"
         : wifi_nets[i].open ? "open" : "secured";
}

static uint32_t wifi_hash(void) {
    int on = st_wifi_shown();
    char nm[64] = "";
    if (on && st_wifi_carrier()) st_wifi_ssid(nm, sizeof(nm));
    uint32_t h = ui_hash_int(2166136261u, wifi_net_n);
    for (int i = 0; i < wifi_net_n; i++) {
        h = ui_hash_str(h, wifi_nets[i].ssid);
        h = ui_hash_str(h, wifi_tag(i, on, nm));
    }
    return h;
}

static void ui_wifi_refresh(void) {
    time_t now = time(NULL);
    if (now - wifi_nets_refresh_at >= 2) {
        wifi_net_n = wifi_scan_results(wifi_nets, 10);
        wifi_saved_refresh();
        wifi_nets_refresh_at = now;
    }
    if (wifi_hash() != wifi_list_hash) { ui_rebuild_req = 1; return; }
    sr_refresh();
}

static void ui_build_settings_wifi(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    int y = sr_build(l, 0, WIFI_ROWS, (int)(sizeof(WIFI_ROWS) / sizeof(WIFI_ROWS[0])));
    time_t now = time(NULL);
    if (now - wifi_nets_refresh_at >= 2) {
        wifi_net_n = wifi_scan_results(wifi_nets, 10);
        wifi_saved_refresh();
        wifi_nets_refresh_at = now;
    }
    wifi_list_hash = wifi_hash();
    int on = st_wifi_shown();
    char nm[64] = "";
    if (on && st_wifi_carrier()) st_wifi_ssid(nm, sizeof(nm));
    if (wifi_net_n == 0) {
        lv_obj_t *t = ui_label(l, "No networks found yet", uf_small, COL_DIM);
        lv_obj_set_pos(t, 24, y + 24);
    }
    for (int i = 0; i < wifi_net_n; i++) {
        lv_obj_t *row = ui_row(l, y + i * ROW_H, ROW_H, ui_wifi_net_tap, i);
        lv_obj_add_event_cb(row, ui_wifi_net_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        int connecting = wifi_connecting_ssid[0] && !strcmp(wifi_nets[i].ssid, wifi_connecting_ssid);
        lv_obj_t *t = ui_label_w(row, wifi_nets[i].ssid, uf_body, connecting ? COL_ACCENT : COL_TEXT,
                                 FB_W - 90 - 100);
        lv_obj_set_pos(t, 24, 20);
        ui_right(row, wifi_tag(i, on, nm), COL_DIM, 22);
    }
    ui.refresh = ui_wifi_refresh;
}

/* ---- Bluetooth ------------------------------------------------------------------ */

static uint32_t bt_list_hash;

static int t_bt_on(void) { return st_bt_on(); }
static void a_bt_on(void) { int on = !st_bt_on(); st_bt_set(on); bt_pref = on; save_conf(); }
static void v_bt_status(char *b, size_t n, uint16_t *c) {
    int on = st_bt_on();
    char nm[64] = "";
    if (on) st_bt_name(nm, sizeof(nm));
    snprintf(b, n, "%s", on ? (nm[0] ? nm : "not connected") : "off");
    *c = COL_DIM;
}
static void v_bt_robust(char *b, size_t n, uint16_t *c) {
    snprintf(b, n, "%s", BT_ROBUST_NAMES[bt_robust_mode]);
    *c = COL_ACCENT;
}
static void a_bt_robust(void) {
    bt_robust_mode = (bt_robust_mode + 1) % 3;
    audio_bt_set_robust(bt_robust_mode);
    save_conf();
    mlog("[bt] robust connection: %s\n", BT_ROBUST_NAMES[bt_robust_mode]);
}
static int t_bt_rssi(void) { return bt_show_rssi; }
static void a_bt_rssi(void) { bt_show_rssi = !bt_show_rssi; save_conf(); }
static void a_bt_scan(void) { bt_scan_start(); }

static const set_row_t BT_ROWS[] = {
    { "Bluetooth", NULL, NULL, t_bt_on, NULL, a_bt_on, NULL, 0 },
    { "Status", NULL, NULL, NULL, v_bt_status, NULL, NULL, 0 },
    { "Robust connection", NULL, NULL, NULL, v_bt_robust, a_bt_robust, NULL, 0 },
    { "Show RSSI", NULL, NULL, t_bt_rssi, NULL, a_bt_rssi, NULL, 0 },
    { "Scan for devices", NULL, NULL, NULL, NULL, a_bt_scan, NULL, 1 },
};

static void ui_bt_dev_tap(lv_event_t *e) {
    int idx = ui_idx(e);
    /* As Libra's: a tap while that device is still connecting is ignored,
     * one attempt at a time. */
    if (idx >= 0 && idx < bt_dev_n &&
        !(!strcmp(bt_connecting_mac, bt_devs[idx].mac) &&
          time(NULL) - bt_connecting_since < BT_CONNECT_TIMEOUT_SEC)) {
        snprintf(bt_connecting_mac, sizeof(bt_connecting_mac), "%s", bt_devs[idx].mac);
        bt_connecting_since = time(NULL);
        bt_pair(bt_devs[idx].mac);
    }
    ui_rebuild_req = 1;
    UI_ACTED();
}

static void ui_bt_dev_hold(lv_event_t *e) {
    int idx = ui_idx(e);
    if (idx >= 0 && idx < bt_dev_n && bt_devs[idx].paired) {
        snprintf(sheet_bt_mac, sizeof(sheet_bt_mac), "%s", bt_devs[idx].mac);
        snprintf(sheet_bt_name, sizeof(sheet_bt_name), "%s",
                 bt_devs[idx].name[0] ? bt_devs[idx].name : bt_devs[idx].mac);
        sheet_open = 8;
    }
    lv_indev_reset(lv_indev_active(), NULL);
    UI_ACTED();
}

static void bt_dev_right(int idx, char *b, size_t n) {
    int connecting = bt_connecting_mac[0] && !strcmp(bt_devs[idx].mac, bt_connecting_mac);
    if (connecting) snprintf(b, n, "Connecting...");
    else if (bt_devs[idx].rssi) snprintf(b, n, "%d dBm", bt_devs[idx].rssi);
    else snprintf(b, n, "--");
}

static uint32_t bt_hash(void) {
    uint32_t h = ui_hash_int(2166136261u, bt_dev_n * 64 + bt_paired_n);
    char buf[32];
    for (int i = 0; i < bt_dev_n; i++) {
        h = ui_hash_str(h, bt_devs[i].mac);
        h = ui_hash_str(h, bt_devs[i].name);
        bt_dev_right(i, buf, sizeof(buf));
        h = ui_hash_str(h, buf);
    }
    return h;
}

static void ui_bt_scan_list(void) {
    time_t now = time(NULL);
    if (now - bt_devs_refresh_at >= 2) {
        bt_dev_n = bt_scan_devices(bt_devs, BT_DEV_MAX);
        bt_fill_details(bt_devs, bt_dev_n);
        bt_paired_n = bt_partition(bt_devs, bt_dev_n, bt_order);
        bt_devs_refresh_at = now;
    }
}

static void ui_bt_refresh(void) {
    ui_bt_scan_list();
    if (bt_hash() != bt_list_hash) { ui_rebuild_req = 1; return; }
    sr_refresh();
}

static void ui_build_settings_bt(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    int y = sr_build(l, 0, BT_ROWS, (int)(sizeof(BT_ROWS) / sizeof(BT_ROWS[0])));
    /* The robust row's description says what the mode does. */
    lv_obj_t *d = ui_label_w(sr.row[2], bt_robust_mode == 2 ? "SBC instead of aptX, always"
                             : bt_robust_mode == 1 ? "SBC when the link keeps stalling"
                             : "Always the best codec", uf_small, COL_DIM, FB_W - 164);
    lv_obj_set_pos(d, 24, 44);
    lv_obj_set_y(sr.title[2], 8);
    lv_obj_t *d2 = ui_label_w(sr.row[3], "Signal reading next to the codec", uf_small, COL_DIM, FB_W - 164);
    lv_obj_set_pos(d2, 24, 44);
    lv_obj_set_y(sr.title[3], 8);
    ui_bt_scan_list();
    bt_list_hash = bt_hash();
    char buf[32];
    if (bt_dev_n == 0) {
        lv_obj_t *t = ui_label(l, "No devices found yet", uf_small, COL_DIM);
        lv_obj_set_pos(t, 24, y + 24);
    } else {
        int rows_n = bt_row_n() - BT_FIXED_ROWS;
        for (int r = 0; r < rows_n; r++) {
            int idx = -1;
            int kind = bt_row_kind(r, &idx);
            int ry = y + r * ROW_H;
            if (kind == BT_ROW_HEADER_PAIRED || kind == BT_ROW_HEADER_SCAN) {
                int paired = kind == BT_ROW_HEADER_PAIRED;
                lv_obj_t *band = ui_plain(l);
                lv_obj_set_pos(band, 0, ry);
                lv_obj_set_size(band, FB_W, ROW_H);
                ui_bg(band, COL_HEADER);
                lv_obj_set_style_border_side(band, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_width(band, 2, 0);
                lv_obj_set_style_border_color(band, c16(COL_LINE), 0);
                snprintf(buf, sizeof(buf), "%s  (%d)", paired ? "PAIRED" : "OTHER DEVICES",
                         paired ? bt_paired_n : bt_dev_n - bt_paired_n);
                lv_obj_t *t = ui_label(band, buf, uf_small, COL_ACCENT);
                lv_obj_set_pos(t, 24, 22);
            } else if (kind == BT_ROW_DEVICE) {
                lv_obj_t *row = ui_row(l, ry, ROW_H, ui_bt_dev_tap, idx);
                lv_obj_add_event_cb(row, ui_bt_dev_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);
                int connecting = bt_connecting_mac[0] && !strcmp(bt_devs[idx].mac, bt_connecting_mac);
                /* Nameless: said so, dimmed, with the address's tail. */
                char unnamed[40];
                snprintf(unnamed, sizeof(unnamed), "Unnamed device \xc2\xb7 %s", bt_devs[idx].mac + 9);
                lv_obj_t *t = ui_label_w(row, bt_devs[idx].name[0] ? bt_devs[idx].name : unnamed, uf_body,
                                         connecting ? COL_ACCENT : bt_devs[idx].name[0] ? COL_TEXT : COL_DIM,
                                         FB_W - 48 - 130);
                lv_obj_set_pos(t, 24, 20);
                bt_dev_right(idx, buf, sizeof(buf));
                ui_right(row, buf, COL_DIM, 22);
            }
        }
    }
    ui.refresh = ui_bt_refresh;
}

/* ---- sliders --------------------------------------------------------------- */

/* A Libra-style slider: COL_LINE track, accent fill, a 12px knob. Values are
 * integers; the callers scale. */
static lv_obj_t *ui_slider(lv_obj_t *parent, int x, int y, int w, int min, int max, int val) {
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_add_flag(s, LV_OBJ_FLAG_USER_1);       /* owns its drags (Libra's eq_dragging) */
    lv_obj_set_pos(s, x, y);
    lv_obj_set_size(s, w, 6);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, c16(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, c16(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, c16(COL_ACCENT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 20);
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    return s;
}

/* ---- the EQ ------------------------------------------------------------------ */

static struct { lv_obj_t *preamp_v, *curve, *sw; } eq_ui;

static void ui_eq_curve_draw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cgw = lv_area_get_width(&a), cgh = lv_area_get_height(&a);
    float freqs[28], resp[28];
    for (int i = 0; i < 28; i++) freqs[i] = eq_freq_from_t((float)i / 27.0f);
    eq_response_db(freqs, resp, 28);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c16(COL_LINE);
    d.width = 1;
    d.p1.x = a.x1; d.p1.y = a.y1 + cgh / 2;
    d.p2.x = a.x2; d.p2.y = a.y1 + cgh / 2;
    lv_draw_line(layer, &d);
    d.color = c16(COL_ACCENT);
    d.width = 2;
    for (int i = 0; i < 27; i++) {
        float d0 = resp[i], d1 = resp[i + 1];
        if (d0 > 18) d0 = 18; if (d0 < -18) d0 = -18;
        if (d1 > 18) d1 = 18; if (d1 < -18) d1 = -18;
        d.p1.x = a.x1 + cgw * i / 27;
        d.p1.y = a.y1 + cgh / 2 - (int)(d0 / 18.0f * (cgh / 2));
        d.p2.x = a.x1 + cgw * (i + 1) / 27;
        d.p2.y = a.y1 + cgh / 2 - (int)(d1 / 18.0f * (cgh / 2));
        lv_draw_line(layer, &d);
    }
}

static void ui_eq_tap(lv_event_t *e) {
    switch (ui_idx(e)) {
    case 0: eq_set_enabled(!eq_enabled()); save_conf(); break;
    case 1: eq_profile_n = ep_scan(eq_profiles, EP_MAX_PROFILES); sheet_open = 3; break;
    case 2: screen = SC_EQ_BANDS; reset_scroll(); total = eq_cur.band_n; break;
    }
    UI_ACTED();
}

static void ui_eq_preamp(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target_obj(e);
    float db = lv_slider_get_value(s) / 10.0f;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f dB", db);
    ui_set_text(eq_ui.preamp_v, buf);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        eq_cur.preamp_db = db;
        eq_set_preamp(eq_cur.preamp_db);
        eq_save_current();
        lv_obj_invalidate(eq_ui.curve);
    }
}

static void ui_eq_refresh(void) {
    ui_switch_set(eq_ui.sw, eq_enabled());
}

static void ui_build_eq(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    char buf[48];
    int y = 0;
    lv_obj_t *r0 = ui_row(l, y, ROW_H, ui_eq_tap, 0);
    lv_obj_t *t = ui_label(r0, "Enabled", uf_body, COL_TEXT);
    lv_obj_set_pos(t, 24, 20);
    eq_ui.sw = ui_switch(r0, eq_enabled(), ROW_H);
    y += ROW_H;
    lv_obj_t *r1 = ui_row(l, y, ROW_H, ui_eq_tap, 1);
    t = ui_label(r1, "Profile", uf_body, COL_TEXT);
    lv_obj_set_pos(t, 24, 20);
    ui_right(r1, eq_cur_path[0] ? eq_cur.name : "None", COL_DIM, 22);
    y += ROW_H + 20;
    t = ui_label(l, "Preamp", uf_small, COL_DIM);
    lv_obj_set_pos(t, 24, y);
    snprintf(buf, sizeof(buf), "%.1f dB", eq_cur.preamp_db);
    eq_ui.preamp_v = ui_label(l, buf, uf_small, COL_DIM);
    lv_obj_align(eq_ui.preamp_v, LV_ALIGN_TOP_RIGHT, -24, y);
    lv_obj_t *s = ui_slider(l, 24, y + 36, FB_W - 48, -120, 60, (int)(eq_cur.preamp_db * 10));
    lv_obj_add_event_cb(s, ui_eq_preamp, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, ui_eq_preamp, LV_EVENT_RELEASED, NULL);
    y += 76;
    eq_ui.curve = ui_plain(l);
    lv_obj_set_pos(eq_ui.curve, 24, y);
    lv_obj_set_size(eq_ui.curve, FB_W - 48, 50);
    lv_obj_add_event_cb(eq_ui.curve, ui_eq_curve_draw, LV_EVENT_DRAW_MAIN, NULL);
    y += 70;
    lv_obj_t *r2 = ui_row(l, y, ROW_H, ui_eq_tap, 2);
    t = ui_label(r2, "Bands", uf_body, COL_TEXT);
    lv_obj_set_pos(t, 24, 20);
    snprintf(buf, sizeof(buf), "%d", eq_cur.band_n);
    ui_right(r2, buf, COL_DIM, 22);
    ui.refresh = ui_eq_refresh;
}

static void ui_eq_band_row_tap(lv_event_t *e) {
    int i = ui_idx(e);
    if (i < 0 || i >= eq_cur.band_n) return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (p.x > FB_W - 100) {
        eq_band_t *b = &eq_cur.band[i];
        b->on = !b->on;
        eq_set_band(i, b);
        eq_save_current();
        ui_rebuild_req = 1;
    } else {
        eq_editing_band = i;
        screen = SC_EQ_BAND; reset_scroll();
    }
    UI_ACTED();
}

static void ui_build_eq_bands(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    char buf[96];
    for (int i = 0; i < eq_cur.band_n; i++) {
        eq_band_t *b = &eq_cur.band[i];
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_eq_band_row_tap, i);
        const char *tn = b->type == EQ_PEAK ? "Peak" : b->type == EQ_LOW_SHELF ? "Low shelf" : "High shelf";
        snprintf(buf, sizeof(buf), "Band %d \xc2\xb7 %s", i + 1, tn);
        lv_obj_t *t = ui_label_w(row, buf, uf_body, b->on ? COL_TEXT : COL_DIM, FB_W - 110 - 24);
        lv_obj_set_pos(t, 24, 8);
        snprintf(buf, sizeof(buf), "%.0f Hz  \xc2\xb7  %+.1f dB  \xc2\xb7  Q %.2f", b->fc, b->gain_db, b->q);
        lv_obj_t *d = ui_label_w(row, buf, uf_small, COL_DIM, FB_W - 110 - 24);
        lv_obj_set_pos(d, 24, 42);
        ui_switch(row, b->on, ROW_H);
    }
    if (eq_cur.band_n == 0) ui_empty_note(l, 0, "No bands in this profile");
}

/* One band's editor: enabled, the filter type, and three sliders. */
static struct { lv_obj_t *v[3]; } eb_ui;

static void ui_eq_band_slider(lv_event_t *e) {
    int which = ui_idx(e);            /* 0 frequency, 1 gain, 2 Q */
    eq_band_t *b = (eq_editing_band >= 0 && eq_editing_band < eq_cur.band_n)
                 ? &eq_cur.band[eq_editing_band] : NULL;
    if (!b) return;
    lv_obj_t *s = lv_event_get_target_obj(e);
    int v = lv_slider_get_value(s);
    char buf[24];
    float fc = b->fc, gain = b->gain_db, q = b->q;
    if (which == 0)      { fc = eq_freq_from_t(v / 1000.0f); snprintf(buf, sizeof(buf), "%.0f Hz", fc); }
    else if (which == 1) { gain = v / 10.0f; snprintf(buf, sizeof(buf), "%+.1f dB", gain); }
    else                 { q = 0.1f + 9.9f * v / 1000.0f; snprintf(buf, sizeof(buf), "%.2f", q); }
    ui_set_text(eb_ui.v[which], buf);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        b->fc = fc; b->gain_db = gain; b->q = q;
        eq_set_band(eq_editing_band, b);
        eq_save_current();
    }
}

static void ui_eq_band_tap(lv_event_t *e) {
    eq_band_t *b = (eq_editing_band >= 0 && eq_editing_band < eq_cur.band_n)
                 ? &eq_cur.band[eq_editing_band] : NULL;
    if (!b) return;
    int k = ui_idx(e);
    if (k == 3) b->on = !b->on;
    else        b->type = (eq_type_t)k;
    eq_set_band(eq_editing_band, b);
    eq_save_current();
    ui_rebuild_req = 1;
    UI_ACTED();
}

static void ui_build_eq_band(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    eq_band_t *b = (eq_editing_band >= 0 && eq_editing_band < eq_cur.band_n)
                 ? &eq_cur.band[eq_editing_band] : NULL;
    if (!b) return;
    char buf[32];
    int y = 0;
    lv_obj_t *r = ui_row(l, y, ROW_H, ui_eq_band_tap, 3);
    lv_obj_t *t = ui_label(r, "Enabled", uf_body, COL_TEXT);
    lv_obj_set_pos(t, 24, 20);
    ui_switch(r, b->on, ROW_H);
    y += ROW_H + 30;
    t = ui_label(l, "Filter type", uf_small, COL_DIM);
    lv_obj_set_pos(t, 24, y);
    y += 30;
    int segw = (FB_W - 48) / 3;
    const char *names[3] = { "Low shelf", "Peak", "High shelf" };
    for (int s = 0; s < 3; s++) {
        lv_obj_t *seg = ui_plain(l);
        lv_obj_set_pos(seg, 24 + segw * s + 2, y);
        lv_obj_set_size(seg, segw - 4, 34);
        int sel = (int)b->type == s;
        if (sel) ui_bg(seg, COL_ACCENT);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(seg, ui_eq_band_tap, LV_EVENT_CLICKED, (void *)(intptr_t)s);
        lv_obj_t *lb = ui_label(seg, names[s], uf_small, sel ? COL_BG : COL_DIM);
        lv_obj_center(lb);
    }
    y += 70;
    const char *labels[3] = { "Frequency", "Gain", "Q" };
    int vals[3] = { (int)(eq_t_from_freq(b->fc) * 1000), (int)(b->gain_db * 10),
                    (int)((b->q - 0.1f) / 9.9f * 1000) };
    int mins[3] = { 0, -120, 0 }, maxs[3] = { 1000, 120, 1000 };
    for (int k = 0; k < 3; k++) {
        t = ui_label(l, labels[k], uf_small, COL_DIM);
        lv_obj_set_pos(t, 24, y);
        if (k == 0) snprintf(buf, sizeof(buf), "%.0f Hz", b->fc);
        else if (k == 1) snprintf(buf, sizeof(buf), "%+.1f dB", b->gain_db);
        else snprintf(buf, sizeof(buf), "%.2f", b->q);
        eb_ui.v[k] = ui_label(l, buf, uf_small, COL_DIM);
        lv_obj_align(eb_ui.v[k], LV_ALIGN_TOP_RIGHT, -24, y);
        lv_obj_t *s = ui_slider(l, 24, y + 36, FB_W - 48, mins[k], maxs[k], vals[k]);
        lv_obj_add_event_cb(s, ui_eq_band_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
        lv_obj_add_event_cb(s, ui_eq_band_slider, LV_EVENT_RELEASED, (void *)(intptr_t)k);
        y += 96;
    }
}

/* ---- MSEB ---------------------------------------------------------------------- */

static struct { lv_obj_t *v[MSEB_BAND_N], *sw; } ms_ui;

static void ui_mseb_slider(lv_event_t *e) {
    int i = ui_idx(e);
    lv_obj_t *s = lv_event_get_target_obj(e);
    float g = lv_slider_get_value(s) / 10.0f;
    char buf[24];
    snprintf(buf, sizeof(buf), "%+.1f dB", g);
    ui_set_text(ms_ui.v[i], buf);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        mseb_gain[i] = g;
        eq_set_mseb(mseb_on, mseb_gain);
        mseb_save(mseb_gain, mseb_on);
    }
}

static void ui_mseb_toggle(lv_event_t *e) {
    (void)e;
    mseb_on = !mseb_on;
    eq_set_mseb(mseb_on, mseb_gain);
    mseb_save(mseb_gain, mseb_on);
    ui_rebuild_req = 1;
    UI_ACTED();
}

static void ui_build_mseb(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    lv_obj_t *r = ui_row(l, 0, ROW_H, ui_mseb_toggle, 0);
    lv_obj_t *t = ui_label(r, "Enabled", uf_body, COL_TEXT);
    lv_obj_set_pos(t, 24, 20);
    ms_ui.sw = ui_switch(r, mseb_on, ROW_H);
    char buf[24];
    uint16_t c = mseb_on ? COL_TEXT : COL_DIM;
    for (int i = 0; i < MSEB_BAND_N; i++) {
        int by = ROW_H + i * MSEB_ROW_H;
        lv_obj_t *n = ui_label_w(l, EQ_MSEB_BANDS[i].name, uf_body, c, FB_W - 48 - 130);
        lv_obj_set_pos(n, 24, by + 10);
        snprintf(buf, sizeof(buf), "%+.1f dB", mseb_gain[i]);
        ms_ui.v[i] = ui_label(l, buf, uf_body, c);
        lv_obj_align(ms_ui.v[i], LV_ALIGN_TOP_RIGHT, -24, by + 10);
        lv_obj_t *f = ui_label(l, EQ_MSEB_BANDS[i].freq_label, uf_small, COL_DIM);
        lv_obj_set_pos(f, 24, by + 48);
        lv_obj_t *mark = ui_plain(l);
        lv_obj_set_pos(mark, 24 + (FB_W - 48) / 2, by + MSEB_SLIDER_Y - 8);
        lv_obj_set_size(mark, 2, MSEB_PILL_H + 16);
        ui_bg(mark, COL_DIM);
        lv_obj_t *s = ui_slider(l, 24, by + MSEB_SLIDER_Y, FB_W - 48, -120, 120, (int)(mseb_gain[i] * 10));
        lv_slider_set_mode(s, LV_SLIDER_MODE_SYMMETRICAL);
        if (!mseb_on) {
            lv_obj_set_style_bg_color(s, c16(COL_DIM), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s, c16(COL_DIM), LV_PART_KNOB);
        }
        lv_obj_add_event_cb(s, ui_mseb_slider, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s, ui_mseb_slider, LV_EVENT_RELEASED, (void *)(intptr_t)i);
        if (i < MSEB_BAND_N - 1) ui_hline(l, by + MSEB_ROW_H - 1, COL_LINE);
    }
}

/* ---- Stats ---------------------------------------------------------------------- */

static void v_stat_batt(char *b, size_t n, uint16_t *c) {
    int p = st_battery_pct();
    if (p >= 0) snprintf(b, n, "%d%%", p);
    *c = COL_DIM;
}
static void v_stat_storage(char *b, size_t n, uint16_t *c) {
    struct statfs sfs;
    if (statfs(STOR_ROOT, &sfs) == 0) {
        char fr[24];
        stor_fmt(fr, sizeof(fr), (long long)sfs.f_bavail * (long long)sfs.f_bsize);
        snprintf(b, n, "%s free", fr);
    }
    *c = COL_DIM;
}
static void v_stat_listen(char *b, size_t n, uint16_t *c) {
    char wd[24];
    long long wsum = 0;
    for (int k = 0; k < 4; k++) wsum += listen_week_secs(k);
    listen_fmt(wd, sizeof(wd), wsum);
    snprintf(b, n, "%s this week", wd);
    *c = COL_DIM;
}
static void a_stat_batt(void) { ui_open(SC_STATS_BATTERY); }
static void a_stat_storage(void) { ui_open(SC_STATS_STORAGE); stor_request(); }
static void a_stat_listen(void) { ui_open(SC_STATS_LISTEN); }

static const set_row_t STATS_ROWS[] = {
    { "Battery", NULL, NULL, NULL, v_stat_batt, a_stat_batt, NULL, 0 },
    { "Storage", NULL, NULL, NULL, v_stat_storage, a_stat_storage, NULL, 0 },
    { "Listening time", NULL, NULL, NULL, v_stat_listen, a_stat_listen, NULL, 0 },
};

static void ui_build_stats(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    sr_build(l, 0, STATS_ROWS, 3);
    ui.refresh = sr_refresh;
}

/* ---- the three Stats pages -------------------------------------------- */

/* Listening time: this week and all time, per kind, and the total. */
static void ui_build_stats_listen(void) {
    static const char *const names[LC_N + 1] = { "Music", "Audiobooks", "Podcasts", "Radio", "Total" };
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    int colw = 150, x_all = FB_W - 24, x_wk = x_all - colw;
    lv_obj_t *h1 = ui_label(l, "Last 7 days", uf_small, COL_DIM);
    lv_obj_align(h1, LV_ALIGN_TOP_RIGHT, -(FB_W - x_wk), 24);
    lv_obj_t *h2 = ui_label(l, "All time", uf_small, COL_DIM);
    lv_obj_align(h2, LV_ALIGN_TOP_RIGHT, -24, 24);
    int y = 60;
    ui_hline(l, y - 1, COL_LINE);
    long long wk_tot = 0, all_tot = 0;
    char v[24];
    for (int i = 0; i <= LC_N; i++) {
        long long wk, al;
        if (i < LC_N) { wk = listen_week_secs(i); al = listen_all_secs(i); wk_tot += wk; all_tot += al; }
        else          { wk = wk_tot; al = all_tot; }
        uint16_t lc = i == LC_N ? COL_ACCENT : COL_TEXT;
        lv_obj_t *row = ui_row(l, y, ROW_H, NULL, i);
        lv_obj_t *n = ui_label(row, names[i], uf_body, lc);
        lv_obj_set_pos(n, 24, 20);
        listen_fmt(v, sizeof(v), wk);
        lv_obj_t *a = ui_label(row, v, uf_body, lc);
        lv_obj_align(a, LV_ALIGN_TOP_RIGHT, -(FB_W - x_wk), 20);
        listen_fmt(v, sizeof(v), al);
        lv_obj_t *b = ui_label(row, v, uf_body, lc);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -24, 20);
        y += ROW_H;
    }
    if (listen_first_ymd) {
        char t[48];
        snprintf(t, sizeof(t), "Counting since %u-%02u-%02u", listen_first_ymd / 10000,
                 (listen_first_ymd / 100) % 100, listen_first_ymd % 100);
        lv_obj_t *f = ui_label(l, t, uf_small, COL_DIM);
        lv_obj_set_pos(f, 24, y + 14);
    }
}

/* Storage: what each kind of content takes on the card, walked on a
 * thread (stor_request()); rows fill in when it finishes. */
static struct { lv_obj_t *v[6], *foot; int state; } stor_ui;

static void ui_stats_storage_refresh(void) {
    if (stor_state == 0) stor_request();
    struct statfs sfs;
    long long total = 0, freeb = 0;
    if (statfs(STOR_ROOT, &sfs) == 0) {
        total = (long long)sfs.f_blocks * (long long)sfs.f_bsize;
        freeb = (long long)sfs.f_bavail * (long long)sfs.f_bsize;
    }
    int done = stor_state == 2;
    long long used = total - freeb, known = stor_music + stor_audio + stor_pod + stor_rec;
    long long val[6] = { stor_music, stor_audio, stor_pod, stor_rec, used > known ? used - known : 0, freeb };
    char v[24];
    for (int i = 0; i < 6; i++) {
        if (i == 5 || done) stor_fmt(v, sizeof(v), val[i]);
        else snprintf(v, sizeof(v), "...");
        ui_set_text(stor_ui.v[i], v);
    }
    if (card_released && !done) {
        ui_set_text(stor_ui.foot, "Not measured in USB Storage Mode");
    } else if (total > 0) {
        char t[40], tt[24];
        stor_fmt(tt, sizeof(tt), total);
        snprintf(t, sizeof(t), "Card %s", tt);
        ui_set_text(stor_ui.foot, t);
    }
    if (stor_walking()) ui_dirty = 1;     /* look again until the walk ends */
}

static void ui_build_stats_storage(void) {
    static const char *const names[6] = { "Music", "Audiobooks", "Podcasts", "Radio Recordings", "Other", "Free" };
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    for (int i = 0; i < 6; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, NULL, i);
        lv_obj_t *n = ui_label(row, names[i], uf_body, COL_TEXT);
        lv_obj_set_pos(n, 24, 20);
        stor_ui.v[i] = ui_right(row, "", COL_DIM, 22);
    }
    stor_ui.foot = ui_label(l, "", uf_small, COL_DIM);
    lv_obj_set_pos(stor_ui.foot, 24, 6 * ROW_H + 14);
    ui.refresh = ui_stats_storage_refresh;
}

/* Battery: level against time since the last full charge, as Libra plots it
 * (the same series, interpolation and fills), drawn with LVGL's primitives. */
static batt_sample_t bs_ser[BATT_MAX + 1];
static int bs_n, bs_have_full;

static void bs_series(void) {
    int pct = st_battery_pct(), chg = st_charging();
    time_t now = time(NULL);
    int start = 0, last_low = batt_n - 1;
    while (last_low >= 0 && batt_log[last_low].pct >= 100) last_low--;
    bs_have_full = 0;
    for (int i = last_low; i >= 0; i--)
        if (batt_log[i].pct >= 100) { start = i; bs_have_full = 1; break; }
    bs_n = 0;
    for (int i = start; i < batt_n; i++) bs_ser[bs_n++] = batt_log[i];
    if (pct >= 0 && now >= 1700000000 && (bs_n == 0 || (uint32_t)now > bs_ser[bs_n - 1].t)) {
        bs_ser[bs_n].t = (uint32_t)now;
        bs_ser[bs_n].pct = (uint8_t)(pct > 100 ? 100 : pct);
        bs_ser[bs_n].chg = (uint8_t)chg;
        bs_n++;
    }
}

static void bs_plot_draw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target_obj(e), &a);
    int px0 = a.x1 + 40, px1 = a.x2, py0 = a.y1, py1 = a.y2 - 40;
    int pw = px1 - px0, ph = py1 - py0;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER;
    lv_draw_label_dsc_t t;
    lv_draw_label_dsc_init(&t);
    t.font = uf_small;
    t.color = c16(COL_DIM);
    char lb[24];
    for (int g = 0; g <= 4; g++) {
        int gy = py1 - ph * g / 4;
        r.bg_color = c16(COL_LINE);
        lv_area_t ln = { px0, gy, px1, gy };
        lv_draw_rect(layer, &r, &ln);
        snprintf(lb, sizeof(lb), "%d", g * 25);
        t.text = lb;
        t.align = LV_TEXT_ALIGN_RIGHT;
        lv_area_t la = { a.x1, gy - 14, px0 - 10, gy + 16 };
        lv_draw_label(layer, &t, &la);
    }
    t.align = LV_TEXT_ALIGN_LEFT;
    if (bs_n < 2 || (long)bs_ser[bs_n - 1].t - (long)bs_ser[0].t < 600) {
        t.text = "Collecting battery history...";
        t.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t ma = { a.x1, py0 + ph / 2 - 14, a.x2, py0 + ph / 2 + 16 };
        lv_draw_label(layer, &t, &ma);
        return;
    }
    long t0 = bs_ser[0].t, span = (long)bs_ser[bs_n - 1].t - t0;
    lv_color_t acc = c16(COL_ACCENT);
    lv_color_t fill_dis = lv_color_mix(acc, c16(COL_BG), 55), fill_chg = lv_color_mix(acc, c16(COL_BG), 110);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = acc;
    d.width = 2;
    int j = 0, prev_y = -1, last_x = px0, last_y = py1;
    for (int x = 0; x <= pw; x++) {
        long tt = t0 + span * x / (pw > 0 ? pw : 1);
        while (j < bs_n - 2 && (long)bs_ser[j + 1].t < tt) j++;
        long ta = bs_ser[j].t, tb = bs_ser[j + 1].t;
        int la = bs_ser[j].pct, lb2 = bs_ser[j + 1].pct;
        int lvl100 = tb > ta ? (int)((la * 100L) + (long)(lb2 - la) * 100L * (tt - ta) / (tb - ta)) : la * 100;
        if (lvl100 < 0) lvl100 = 0;
        if (lvl100 > 10000) lvl100 = 10000;
        int yv = py1 - (int)((long)ph * lvl100 / 10000);
        int cx = px0 + x;
        if (py1 > yv) {
            r.bg_color = bs_ser[j].chg ? fill_chg : fill_dis;
            lv_area_t col = { cx, yv, cx, py1 - 1 };
            lv_draw_rect(layer, &r, &col);
        }
        if (prev_y >= 0) {
            d.p1.x = cx - 1; d.p1.y = prev_y; d.p2.x = cx; d.p2.y = yv;
            lv_draw_line(layer, &d);
        }
        prev_y = yv; last_x = cx; last_y = yv;
    }
    r.bg_color = acc;
    r.radius = LV_RADIUS_CIRCLE;
    lv_area_t dot = { last_x - 7, last_y - 7, last_x + 7, last_y + 7 };
    lv_draw_rect(layer, &r, &dot);
    r.radius = 0;
    r.bg_color = c16(COL_DIM);
    lv_area_t base = { px0, py1, px1, py1 };
    lv_draw_rect(layer, &r, &base);
    for (int k = 0; k < 3; k++) {
        time_t tt = (time_t)(t0 + span * k / 2);
        struct tm tmv;
        localtime_r(&tt, &tmv);
        strftime(lb, sizeof(lb), span > 20 * 3600 ? "%a %H:%M" : "%H:%M", &tmv);
        t.text = lb;
        int cxk = px0 + pw * k / 2;
        lv_area_t ta = { k == 0 ? cxk : k == 1 ? cxk - 100 : cxk - 200, py1 + 12,
                         k == 0 ? cxk + 200 : k == 1 ? cxk + 100 : cxk, py1 + 40 };
        t.align = k == 0 ? LV_TEXT_ALIGN_LEFT : k == 1 ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_RIGHT;
        lv_draw_label(layer, &t, &ta);
    }
}

static struct { lv_obj_t *big, *state, *hist, *plot; int last_n; } bat_ui;

static void ui_stats_battery_refresh(void) {
    bs_series();
    int pct = st_battery_pct(), chg = st_charging();
    char big[16] = "--", line[48] = "";
    if (pct >= 0) snprintf(big, sizeof(big), "%d%%", pct);
    ui_set_text(bat_ui.big, big);
    ui_set_text(bat_ui.state, chg ? "Charging" : (pct >= 100 ? "Full" : "On battery"));
    lv_obj_align_to(bat_ui.state, bat_ui.big, LV_ALIGN_OUT_RIGHT_MID, 16, 2);
    if (bs_n >= 2 && (long)bs_ser[bs_n - 1].t - (long)bs_ser[0].t >= 600) {
        char d[24];
        batt_fmt_dur(d, sizeof(d), (long)bs_ser[bs_n - 1].t - (long)bs_ser[0].t);
        snprintf(line, sizeof(line), bs_have_full ? "%s since full" : "%s history", d);
    }
    ui_set_text(bat_ui.hist, line);
    lv_obj_align(bat_ui.hist, LV_ALIGN_TOP_RIGHT, -24, CONTENT_Y + 14);
    int bot = FB_H - (mini_visible() ? MINI_H : 0);
    lv_obj_set_height(bat_ui.plot, bot - (CONTENT_Y + 64));
    lv_obj_invalidate(bat_ui.plot);
}

static void ui_build_stats_battery(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    bat_ui.big = ui_label(scr, "", uf_title, COL_ACCENT);
    lv_obj_set_pos(bat_ui.big, 24, CONTENT_Y + 6);
    bat_ui.state = ui_label(scr, "", uf_body, COL_DIM);
    bat_ui.hist = ui_label(scr, "", uf_small, COL_DIM);
    bat_ui.plot = ui_plain(scr);
    lv_obj_set_pos(bat_ui.plot, 24, CONTENT_Y + 64);
    lv_obj_set_size(bat_ui.plot, FB_W - 24 - 20, FB_H - CONTENT_Y - 64);
    lv_obj_add_event_cb(bat_ui.plot, bs_plot_draw, LV_EVENT_DRAW_MAIN, NULL);
    ui.refresh = ui_stats_battery_refresh;
}

/* ---- a podcast's own settings ----------------------------------------------- */

static struct { lv_obj_t *val[2], *auto_st[2]; } ps_ui;

static void ui_pod_set_step(lv_event_t *e) {
    int k = ui_idx(e);                 /* row * 2 + (plus ? 1 : 0) */
    int row = k / 2, dir = (k & 1) ? +1 : -1;
    int *v = row == 0 ? &pod_set_intro : &pod_set_outro;
    int nv = *v + dir * POD_SKIP_STEP;
    if (nv < 0) nv = 0;
    if (nv > POD_SKIP_MAX) nv = POD_SKIP_MAX;
    if (nv != *v) {
        *v = nv;
        pod_skip_store(cur_feed, pod_set_intro, pod_set_outro);
        if (podcast_mode && !strcmp(pod_play_feed, cur_feed)) {
            pod_play_intro_s = pod_set_intro;
            pod_play_outro_s = pod_set_outro;
        }
    }
    UI_ACTED();
}

static void ui_pod_set_auto(lv_event_t *e) {
    pod_auto_start(ui_idx(e));
    UI_ACTED();
}

static void ui_pod_settings_refresh(void) {
    int vals[2] = { pod_set_intro, pod_set_outro };
    char vb[16];
    for (int i = 0; i < 2; i++) {
        if (vals[i] > 0) snprintf(vb, sizeof(vb), "%d s", vals[i]);
        else             snprintf(vb, sizeof(vb), "Off");
        ui_set_text(ps_ui.val[i], vb);
        ui_set_color(ps_ui.val[i], vals[i] > 0 ? COL_TEXT : COL_DIM);
        lv_obj_align(ps_ui.val[i], LV_ALIGN_TOP_MID,
                     (POD_SET_MINUS_X + POD_SET_PLUS_X) / 2 - FB_W / 2, 20);
    }
    for (int tail = 0; tail <= 1; tail++) {
        char st[40] = "";
        int busy = pod_auto_state == 1;
        int mine = !strcmp(pod_auto_feed, cur_feed) && pod_auto_tail == tail;
        if (busy && mine) snprintf(st, sizeof(st), "Listening...");
        else if (pod_auto_state >= 2 && mine) {
            int r = pod_auto_result;
            if (r > 0)               snprintf(st, sizeof(st), "Found %d s", r);
            else if (r == AUTO_FEW)  snprintf(st, sizeof(st), "Needs 2 downloaded");
            else if (r == AUTO_LONG) snprintf(st, sizeof(st), "Over a minute alike");
            else                     snprintf(st, sizeof(st), "None found");
        }
        ui_set_text(ps_ui.auto_st[tail], st);
        lv_obj_align(ps_ui.auto_st[tail], LV_ALIGN_TOP_RIGHT, -24, 22);
    }
}

static void ui_build_pod_settings(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    lv_obj_t *t = ui_label_w(l, cur_feed, uf_body, COL_TEXT, FB_W - 48);
    lv_obj_set_pos(t, 24, 14);
    t = ui_label(l, "For every episode of this podcast", uf_small, COL_DIM);
    lv_obj_set_pos(t, 24, 52);
    int row0 = POD_SET_ROW0 - CONTENT_Y;
    ui_hline(l, row0 - 1, COL_LINE);
    static const char *labels[2] = { "Skip intro", "Skip outro" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = ui_row(l, row0 + i * ROW_H, ROW_H, NULL, i);
        lv_obj_t *lb = ui_label(row, labels[i], uf_body, COL_TEXT);
        lv_obj_set_pos(lb, 24, 20);
        lv_obj_t *minus = ui_circle(row, POD_SET_MINUS_X, ROW_H / 2, 22, COL_ROW, 0);
        lv_obj_t *ml = ui_label(minus, LV_SYMBOL_MINUS, (lv_font_t *)&lv_font_montserrat_28, COL_ACCENT);
        lv_obj_center(ml);
        lv_obj_add_flag(minus, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(minus, 18);
        lv_obj_add_event_cb(minus, ui_pod_set_step, LV_EVENT_CLICKED, (void *)(intptr_t)(i * 2));
        lv_obj_t *plus = ui_circle(row, POD_SET_PLUS_X, ROW_H / 2, 22, COL_ROW, 0);
        lv_obj_t *pl = ui_label(plus, LV_SYMBOL_PLUS, (lv_font_t *)&lv_font_montserrat_28, COL_ACCENT);
        lv_obj_center(pl);
        lv_obj_add_flag(plus, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(plus, 18);
        lv_obj_add_event_cb(plus, ui_pod_set_step, LV_EVENT_CLICKED, (void *)(intptr_t)(i * 2 + 1));
        ps_ui.val[i] = ui_label(row, "", uf_body, COL_TEXT);
    }
    for (int tail = 0; tail <= 1; tail++) {
        lv_obj_t *row = ui_row(l, row0 + (2 + tail) * ROW_H, ROW_H, ui_pod_set_auto, tail);
        lv_obj_t *lb = ui_label(row, tail ? "Auto outro" : "Auto intro", uf_body, COL_ACCENT);
        lv_obj_set_pos(lb, 24, 20);
        ps_ui.auto_st[tail] = ui_label(row, "", uf_small, COL_DIM);
    }
    t = ui_label(l, "Auto compares the last 3 episodes.\n\n"
                    "Intro: an episode started from the beginning\nstarts this far in.\n\n"
                    "Outro: an episode counts as finished this\nlong before its end, and stops there.",
                 uf_small, COL_DIM);
    lv_obj_set_pos(t, 24, row0 + 4 * ROW_H + 16);
    ui.refresh = ui_pod_settings_refresh;
}

/* ---- the keyboard --------------------------------------------------------
 * Libra's own T9 keypad, as it draws it: the field with its caret, the shift
 * toggle (letters only), a 3x4 grid of keys -- and every key press is
 * Libra's own (kb_apply_key()'s multi-tap cycling, kb_backspace(), the
 * mode key, kb_commit() for Done). */
static struct { lv_obj_t *field, *caret, *shift, *shift_l, *key_l[12], *digit_l[12]; } kb_ui;

static void ui_kb_refresh(void) {
    ui_set_text(kb_ui.field, kb_buf);
    int cx = kb_caret_x(kb_cursor);
    if (cx > FB_W - 28) cx = FB_W - 28;
    lv_obj_set_x(kb_ui.caret, cx);
    if (kb_mode == KB_MODE_LETTERS) {
        ui_set_text(kb_ui.shift_l, kb_shift_label());
        ui_set_color(kb_ui.shift_l, kb_shift != KB_SHIFT_OFF ? COL_ACCENT : COL_DIM);
        ui_bg(kb_ui.shift, COL_ROW);
    } else {
        ui_set_text(kb_ui.shift_l, kb_shift_label());
        ui_set_color(kb_ui.shift_l, COL_LINE);
        lv_obj_set_style_bg_opa(kb_ui.shift, LV_OPA_TRANSP, 0);
    }
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 3; col++) {
            int k = row * 3 + col, key = KB_GRID[row][col];
            const char *label = key == -1 ? kb_mode_key_label() : key == -2 ? "<-"
                              : kb_key_label(kb_mode, key);
            ui_set_text(kb_ui.key_l[k], label);
            ui_set_color(kb_ui.key_l[k], key < 0 ? COL_ACCENT : COL_TEXT);
            lv_obj_center(kb_ui.key_l[k]);
            if (kb_ui.digit_l[k]) {
                if (key >= 0 && kb_mode != KB_MODE_NUMBERS) lv_obj_remove_flag(kb_ui.digit_l[k], LV_OBJ_FLAG_HIDDEN);
                else                                        lv_obj_add_flag(kb_ui.digit_l[k], LV_OBJ_FLAG_HIDDEN);
            }
        }
}

static void ui_kb_key(lv_event_t *e) {
    int k = ui_idx(e);
    int key = KB_GRID[k / 3][k % 3];
    if (key == -1) { kb_mode = (kb_mode + 1) % KB_MODE_N; kb_last_key = -1; }
    else if (key == -2) kb_backspace();
    else kb_apply_key(key);
    ui_kb_refresh();
    UI_ACTED();
}

static void ui_kb_shift(lv_event_t *e) {
    (void)e;
    if (kb_mode == KB_MODE_LETTERS) kb_shift = (kb_shift + 1) % 3;
    ui_kb_refresh();
    UI_ACTED();
}

static void ui_kb_field(lv_event_t *e) {
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    kb_set_cursor_from_x(p.x);
    ui_kb_refresh();
    UI_ACTED();
}

static void ui_kb_head(lv_event_t *e) {
    if (ui_idx(e)) kb_commit();               /* Done */
    else screen = kb_return_screen;           /* Cancel */
    UI_ACTED();
}

static void ui_build_keyboard(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *cancel = ui_label(scr, "Cancel", uf_small, COL_DIM);
    lv_obj_set_pos(cancel, 24, 20);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(cancel, 24);
    lv_obj_add_event_cb(cancel, ui_kb_head, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *done = ui_label(scr, "Done", uf_small, COL_ACCENT);
    lv_obj_align(done, LV_ALIGN_TOP_RIGHT, -24, 20);
    lv_obj_add_flag(done, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(done, 24);
    lv_obj_add_event_cb(done, ui_kb_head, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *title = ui_label_w(scr, kb_title, uf_body, COL_TEXT, FB_W - 180);
    lv_obj_set_pos(title, 24, 56);

    /* The field: the text, and a solid caret (Libra does not blink it). */
    int fy = 100;
    lv_obj_t *f = ui_plain(scr);
    lv_obj_set_pos(f, 24, fy);
    lv_obj_set_size(f, FB_W - 48, 70);
    ui_bg(f, COL_ROW);
    lv_obj_set_style_border_side(f, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(f, 1, 0);
    lv_obj_set_style_border_color(f, c16(COL_LINE), 0);
    lv_obj_add_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(f, ui_kb_field, LV_EVENT_CLICKED, NULL);
    kb_ui.field = ui_label(scr, "", uf_body, COL_TEXT);
    lv_label_set_long_mode(kb_ui.field, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(kb_ui.field, FB_W - 24 - KB_FIELD_X, lv_font_get_line_height(uf_body));
    lv_obj_set_pos(kb_ui.field, KB_FIELD_X, fy + 22);
    kb_ui.caret = ui_plain(scr);
    lv_obj_set_size(kb_ui.caret, 2, TEXT_PX_BODY + 10);
    lv_obj_set_y(kb_ui.caret, fy + 16);
    ui_bg(kb_ui.caret, COL_ACCENT);

    /* Shift: off, once, locked (R73); inert outside letters mode. */
    kb_ui.shift = ui_plain(scr);
    lv_obj_set_pos(kb_ui.shift, KB_SHIFT_X, KB_SHIFT_Y);
    lv_obj_set_size(kb_ui.shift, KB_SHIFT_W, KB_SHIFT_H);
    lv_obj_add_flag(kb_ui.shift, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(kb_ui.shift, 8);
    lv_obj_add_event_cb(kb_ui.shift, ui_kb_shift, LV_EVENT_CLICKED, NULL);
    kb_ui.shift_l = ui_label(kb_ui.shift, "", uf_small, COL_DIM);
    lv_obj_center(kb_ui.shift_l);

    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 3; col++) {
            int k = row * 3 + col, key = KB_GRID[row][col];
            lv_obj_t *b = ui_plain(scr);
            lv_obj_set_pos(b, kb_key_x(col) + 4, kb_key_y(row) + 4);
            lv_obj_set_size(b, KB_KEY_W - 8, KB_KEY_H - 8);
            ui_bg(b, COL_ROW);
            lv_obj_set_style_bg_color(b, c16(COL_LINE), LV_STATE_PRESSED);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(b, ui_kb_key, LV_EVENT_CLICKED, (void *)(intptr_t)k);
            kb_ui.key_l[k] = ui_label(b, "", uf_body, COL_TEXT);
            kb_ui.digit_l[k] = NULL;
            if (key >= 0) {
                char d[2] = { (char)('0' + key), 0 };
                kb_ui.digit_l[k] = ui_label(b, d, uf_small, COL_DIM);
                lv_obj_set_pos(kb_ui.digit_l[k], KB_KEY_W - 8 - 22, 6);
            }
        }
    ui.refresh = ui_kb_refresh;
}
