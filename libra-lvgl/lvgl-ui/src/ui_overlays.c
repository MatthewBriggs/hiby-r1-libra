/* ui_overlays.c — what sits over any screen: the press-and-hold action
 * sheets, the quick-settings dropdown, the volume popup, the seek toast and
 * the power-off countdown. All on LVGL's top layer, each rebuilt from Libra's
 * own flags (sheet_open, qs_open, vol_ticks...) whenever they change. Part of
 * ui_libra.c's translation unit. */

/* ---- press-and-hold: what opens a sheet ----------------------------------- */

#define SHEET_QUEUE 101          /* this port's own: remove a queued track */
static int q_hold_display;

/* A hold already did something: no tap on release. */
static void ui_hold_done(void) {
    if (swp.active) return;          /* a slow swipe is not a hold */
    lv_indev_reset(lv_indev_active(), NULL);
    UI_ACTED();
}

static void ui_track_hold(lv_event_t *e) {
    if (swp.active) return;
    int idx = ui_idx(e);
    if (idx >= 0 && idx < track_n) {
        sheet_open = 1;
        sheet_track = idx;
    }
    ui_hold_done();
}

static void ui_album_hold(lv_event_t *e) {
    if (swp.active) return;
    int idx = ui_idx(e);
    if (idx >= 0 && idx < ap_view_n) {
        sheet_ap_album = ap_view[idx];
        snprintf(sheet_ap_auto, sizeof(sheet_ap_auto), "Automatic (%s)",
                 ac_names[ac_auto(&artist_page_albums[sheet_ap_album])]);
        sheet_open = 9;
    }
    ui_hold_done();
}

static void ui_playlist_hold(lv_event_t *e) {
    if (swp.active) return;
    int pi = ui_idx(e);
    if (pi >= 0 && pi < playlist_n) {
        sheet_open = 4;
        sheet_playlist = pi;
    }
    ui_hold_done();
}

static void ui_queue_hold(lv_event_t *e) {
    if (swp.active) return;
    q_hold_display = ui_idx(e);
    sheet_open = SHEET_QUEUE;
    ui_hold_done();
}

/* ---- the sheet ------------------------------------------------------------- */

static struct { lv_obj_t *dim, *panel; int shown, key; } sh_ui;

static int sheet_count(void) {
    return sheet_open == SHEET_QUEUE ? 2 : sheet_rows();
}

static const char *sheet_label(int i, int *last, int *is_action) {
    *is_action = 0;
    if (sheet_open == SHEET_QUEUE) {
        *last = i == 1;
        *is_action = i == 0;
        return i == 0 ? "Remove from queue" : "Cancel";
    }
    if (sheet_open == 2) {
        *last = (i == playlist_n + 1);
        *is_action = (i == playlist_n);
        return *last ? "Cancel" : (i == playlist_n) ? "New Playlist..." : playlists[i].name;
    }
    if (sheet_open == 3) {
        *last = (i == eq_profile_n);
        return *last ? "Cancel" : eq_profiles[i].name;
    }
    if (sheet_open == 4) { *last = i == PLAYLIST_MENU_N - 1; *is_action = !*last; return playlist_menu_items[i]; }
    if (sheet_open == 5) { *last = i == PLAYLIST_DELETE_N - 1; *is_action = !*last; return playlist_delete_items[i]; }
    if (sheet_open == 6) { *last = i == REC_CONFIRM_N - 1; *is_action = !*last; return rec_confirm_items[i]; }
    if (sheet_open == 7) { *last = i == sheet_count() - 1; *is_action = !*last; return WIFI_MENU_ITEM(i); }
    if (sheet_open == 8) { *last = i == BT_MENU_N - 1; *is_action = !*last; return bt_menu_items[i]; }
    if (sheet_open == 9) {
        *last = i == AC_N + 1;
        *is_action = i < AC_N && sheet_ap_album >= 0 && sheet_ap_album < artist_page_album_n &&
                     i == ac_cat_of[sheet_ap_album];
        return *last ? "Cancel" : i == AC_N ? sheet_ap_auto : ac_names[i];
    }
    *last = i == SHEET_N - 1;
    return sheet_items[i];
}

static const char *sheet_caption(void) {
    if (sheet_open == SHEET_QUEUE) {
        int idx = queue_display_index(q_hold_display);
        return idx >= 0 && idx < queue_n ? queue[idx].name : "";
    }
    return sheet_open == 3 ? "Choose profile"
         : sheet_open == 6 ? "Recording in progress"
         : sheet_open == 7 ? sheet_wifi_ssid
         : sheet_open == 8 ? sheet_bt_name
         : sheet_open == 9 ? (sheet_ap_album >= 0 && sheet_ap_album < artist_page_album_n
                              ? artist_page_albums[sheet_ap_album].name : "")
         : (sheet_open == 4 || sheet_open == 5)
           ? (sheet_playlist >= 0 && sheet_playlist < playlist_n ? playlists[sheet_playlist].name : "")
         : (sheet_track >= 0 && sheet_track < track_n) ? tracks[sheet_track].name : "";
}

/* Libra's sheet tap handler, one row at a time. */
static void ui_sheet_pick(int i) {
    sheet_note[0] = '\0';
    if (sheet_open == SHEET_QUEUE) {
        sheet_open = 0;
        if (i == 0) { queue_remove_display(q_hold_display); ui_rebuild_req = 1; }
    } else if (sheet_open == 2) {
        if (i == playlist_n) {
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
        if (i >= 0 && i < eq_profile_n) {
            eq_switch_to(eq_profiles[i].path);
            save_conf();
            ui_rebuild_req = 1;
        }
        sheet_open = 0;
    } else if (sheet_open == 4) {
        if (i == 0 && sheet_playlist >= 0 && sheet_playlist < playlist_n) {
            snprintf(kb_rename_playlist_path, sizeof(kb_rename_playlist_path), "%s",
                     playlists[sheet_playlist].path);
            sheet_open = 0;
            kb_open("Playlist name", KB_PURPOSE_RENAME_PLAYLIST, playlists[sheet_playlist].name);
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
    } else if (sheet_open == 9) {
        sheet_open = 0;
        if (i >= 0 && i <= AC_N && sheet_ap_album >= 0 && sheet_ap_album < artist_page_album_n) {
            lib_row_t *r = &artist_page_albums[sheet_ap_album];
            ac_move(r, i < AC_N ? i : -1);
            ac_regroup(ac_cur);
            int to = ac_cat_of[sheet_ap_album];
            snprintf(sheet_note, sizeof(sheet_note), "%.40s: %s", r->name, ac_names[to]);
            ui_rebuild_req = 1;
        }
    } else if (sheet_open == 8) {
        sheet_open = 0;
        if (i == 0) {
            bt_disconnect(sheet_bt_mac);
            snprintf(sheet_note, sizeof(sheet_note), "Disconnected %.36s", sheet_bt_name);
            bt_devs_refresh_at = 0;
        } else if (i == 1) {
            bt_forget(sheet_bt_mac);
            if (!strcmp(bt_connecting_mac, sheet_bt_mac)) bt_connecting_mac[0] = '\0';
            snprintf(sheet_note, sizeof(sheet_note), "Forgot %.40s", sheet_bt_name);
            bt_devs_refresh_at = 0;
        }
    } else if (sheet_open == 7) {
        int k = i + (sheet_wifi_secured ? 0 : 1);
        sheet_open = 0;
        if (i >= 0 && k == 0) {
            snprintf(kb_wifi_target_ssid, sizeof(kb_wifi_target_ssid), "%s", sheet_wifi_ssid);
            kb_open("New password", KB_PURPOSE_WIFI_PASSWORD, "");
        } else if (i >= 0 && k == 1) {
            wifi_forget(sheet_wifi_ssid);
            wifi_nets_refresh_at = 0;
            snprintf(sheet_note, sizeof(sheet_note), "Forgot %.40s", sheet_wifi_ssid);
        }
    } else if (sheet_open == 6) {
        if (i == 0) recording_confirm_apply();
        else        rec_confirm_kind = RECQ_NONE;
        sheet_open = 0;
    } else if (i == 0 || i == 1) {
        if (sheet_track >= 0 && sheet_track < track_n && !tracks[sheet_track].path[0]) {
            snprintf(sheet_note, sizeof(sheet_note), "Not downloaded yet");
        } else if (queue_kind_conflict()) {
            snprintf(sheet_note, sizeof(sheet_note), "%s is playing",
                     audiobook_mode ? "An audiobook" : podcast_mode ? "A podcast" : "Music");
        } else if (i == 0) {
            snprintf(sheet_note, sizeof(sheet_note), "%s",
                     queue_play_next(sheet_track) ? "Playing next" : "Queue is full");
        } else {
            snprintf(sheet_note, sizeof(sheet_note), "%s",
                     queue_insert(sheet_track, 0) ? "Added to queue" : "Queue is full");
        }
        sheet_open = 0;
    } else if (i == 2) {
        playlist_n = pl_list(playlists, PL_MAX);
        sheet_open = 2;
    } else {
        sheet_open = 0;
    }
    UI_ACTED();
}

static void ui_sheet_row(lv_event_t *e) { ui_sheet_pick(ui_idx(e)); }
static void ui_sheet_away(lv_event_t *e) { (void)e; sheet_open = 0; UI_ACTED(); }

static void ui_sheet_close(void) {
    if (sh_ui.dim) { lv_obj_delete(sh_ui.dim); sh_ui.dim = NULL; }
    sh_ui.shown = 0;
}

static void ui_sheet_open(void) {
    ui_sheet_close();
    int rows = sheet_count();
    int top = FB_H - rows * SHEET_ROW - SHEET_HEAD;
    if (top < STATUS_H) top = STATUS_H;
    lv_obj_t *dim = sh_ui.dim = ui_plain(lv_layer_top());
    lv_obj_set_size(dim, FB_W, FB_H);
    lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_50, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dim, ui_sheet_away, LV_EVENT_CLICKED, NULL);
    lv_obj_t *p = sh_ui.panel = ui_plain(dim);
    lv_obj_set_pos(p, 0, top);
    lv_obj_set_size(p, FB_W, FB_H - top);
    ui_bg(p, COL_HEADER);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, c16(COL_LINE), 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);       /* taps on the panel stay on it */
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *cap = ui_label_w(p, sheet_caption(), uf_small, COL_DIM, FB_W - 48);
    lv_obj_set_pos(cap, 24, 14);
    for (int i = 0; i < rows; i++) {
        int last, act;
        const char *label = sheet_label(i, &last, &act);
        lv_obj_t *row = ui_plain(p);
        lv_obj_set_pos(row, 0, SHEET_HEAD + i * SHEET_ROW);
        lv_obj_set_size(row, FB_W, SHEET_ROW);
        lv_obj_set_style_bg_color(row, c16(COL_ROW), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ui_sheet_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *t = ui_label_w(row, label, uf_body, last ? COL_DIM : act ? COL_ACCENT : COL_TEXT,
                                 FB_W - 48);
        lv_obj_set_pos(t, 24, 16);
        if (!last) {
            lv_obj_t *ln = ui_plain(row);
            lv_obj_set_pos(ln, 24, SHEET_ROW - 1);
            lv_obj_set_size(ln, FB_W - 48, 1);
            ui_bg(ln, COL_LINE);
        }
    }
    sh_ui.shown = sheet_open;
}

/* ---- quick settings -------------------------------------------------------- */

static struct { lv_obj_t *panel, *dim; } qs_ui;

enum { QS_WIFI, QS_BT, QS_EQ_TOGGLE, QS_EQ_PICK, QS_MSEB, QS_COVER, QS_USB, QS_GEAR, QS_CLOSE };

static void ui_qs_tap(lv_event_t *e) {
    switch (ui_idx(e)) {
    case QS_WIFI:
        if (!qs_wifi && wifi_on_blocked()) { wifi_on_refused("the dropdown"); break; }
        qs_wifi = !qs_wifi;
        st_wifi_set(qs_wifi);
        mlog("[music] wifi: %s from the dropdown\n", qs_wifi ? "on" : "off");
        wifi_pref = qs_wifi;
        wifi_bt_parked = 0;
        if (qs_wifi && net_held()) wifi_bt_override = 1;
        save_conf();
        break;
    case QS_BT:
        qs_bt = !qs_bt;
        st_bt_set(qs_bt);
        bt_pref = qs_bt;
        save_conf();
        break;
    case QS_EQ_TOGGLE:
        eq_set_enabled(!eq_enabled());
        save_conf();
        break;
    case QS_EQ_PICK:
        if (card_released) break;
        qs_open = 0;
        eq_profile_n = ep_scan(eq_profiles, EP_MAX_PROFILES);
        sheet_open = 3;
        break;
    case QS_MSEB:
        if (card_released) break;
        mseb_on = !mseb_on;
        eq_set_mseb(mseb_on, mseb_gain);
        mseb_save(mseb_gain, mseb_on);
        break;
    case QS_COVER:
        cover_palette_enabled = !cover_palette_enabled;
        np_palette_seq = -1;
        np_view_palette_seq = -1;
        if (cover_palette_enabled) cover_prewarm_start();
        save_conf();
        ui_rebuild_req = 1;
        break;
    case QS_USB:
        qs_open = 0;
        screen = SC_SETTINGS_USB; reset_scroll();
        break;
    case QS_GEAR:
        qs_open = 0;
        screen = SC_SETTINGS; reset_scroll();
        break;
    case QS_CLOSE:
        qs_open = 0;
        break;
    }
    UI_ACTED();
}

static int qs_bright_drag;
static void ui_qs_build(void);

/* While brightness is dragged everything else on the panel goes, leaving
 * that one slider on a rounded backing, so the screen's change can be seen
 * (Libra's qs_dragging popup); the panel comes back on release. */
static void ui_qs_bright_drag(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *s = lv_event_get_target_obj(e);
    if (code == LV_EVENT_PRESSED && !qs_bright_drag) {
        qs_bright_drag = 1;
        uint32_t n = lv_obj_get_child_count(qs_ui.panel);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *c = lv_obj_get_child(qs_ui.panel, (int32_t)i);
            if (c != s && lv_obj_get_user_data(c) != (void *)0xB1) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_opa(qs_ui.panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(qs_ui.panel, 0, 0);
        lv_obj_set_style_bg_opa(qs_ui.dim, LV_OPA_TRANSP, 0);
        int ph = vol_pop_h();
        lv_obj_t *back = ui_plain(qs_ui.panel);
        lv_obj_set_pos(back, 6, qs_bar_y() + 4 - ph / 2);
        lv_obj_set_size(back, FB_W - 12, ph);
        lv_obj_set_style_radius(back, VOL_POP_R, 0);
        ui_bg(back, qc_panel_themed() ? qc_bg() : COL_HEADER);
        lv_obj_move_to_index(back, 0);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && qs_bright_drag) {
        qs_bright_drag = 0;
        ui_qs_build();
    }
}

static void ui_qs_slider(lv_event_t *e) {
    int which = ui_idx(e);   /* 0 volume, 1 brightness */
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    if (which == 0) {
        if (!usb_bypass_active) audio_volume_set(audio_volume_snap(v));
    } else {
        qs_bright = v < 1 ? 1 : v;
        st_brightness_set(qs_bright);
        if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
            saved_brightness = qs_bright;
            brightness_pref = qs_bright;
            save_conf();
        }
    }
    ui_dirty = 1;
}

/* Held, Wi-Fi or Bluetooth opens its own page instead of toggling. */
static void ui_qs_hold(lv_event_t *e) {
    qs_open = 0;
    screen = ui_idx(e) == QS_WIFI ? SC_SETTINGS_WIFI : SC_SETTINGS_BT;
    reset_scroll();
    lv_indev_reset(lv_indev_active(), NULL);
    UI_ACTED();
}

/* One cell of the grid: icon, a name, a line under it. */
static lv_obj_t *qs_cell(lv_obj_t *p, int col, int y, const icon_t *ic, int on,
                         const char *name, const char *sub, int action, uint16_t fg, uint16_t dim,
                         uint16_t acc) {
    int cw = qs_col_w(), x = qs_col_x(col);
    lv_obj_t *c = ui_plain(p);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, cw, QS_ROW_H);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, ui_qs_tap, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    if (action == QS_WIFI || action == QS_BT)
        lv_obj_add_event_cb(c, ui_qs_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)action);
    lv_obj_t *img = ui_icon_img(c, ic, on ? acc : dim);
    lv_obj_align(img, LV_ALIGN_LEFT_MID, (38 - ic->w) / 2, 0);
    int label_dx = QS_LABEL_X - 24;
    lv_obj_t *n = ui_label_w(c, name, uf_small, on ? fg : dim, cw - label_dx);
    lv_obj_set_pos(n, label_dx, 6);
    lv_obj_t *s = ui_label_w(c, sub, uf_small, dim, cw - label_dx);
    lv_obj_set_pos(s, label_dx, 32);
    return c;
}

static void ui_qs_close(void) {
    if (qs_ui.dim) { lv_obj_delete(qs_ui.dim); qs_ui.dim = NULL; }
}

static void ui_qs_build(void) {
    ui_qs_close();
    int themed = qc_panel_themed();
    uint16_t bg = themed ? qc_bg() : COL_HEADER, fg = themed ? qc_fg() : COL_TEXT,
             dim = themed ? qc_dim() : COL_DIM, acc = themed ? qc_accent() : COL_ACCENT,
             line = themed ? qc_line() : COL_LINE;
    lv_obj_t *d = qs_ui.dim = ui_plain(lv_layer_top());
    lv_obj_set_size(d, FB_W, FB_H);
    lv_obj_set_style_bg_color(d, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_40, 0);
    lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d, ui_qs_tap, LV_EVENT_CLICKED, (void *)QS_CLOSE);
    lv_obj_t *p = qs_ui.panel = ui_plain(d);
    lv_obj_set_size(p, FB_W, QS_H);
    ui_bg(p, bg);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, c16(line), 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);

    /* Volume, then brightness: icon, then the bar. */
    int vv = audio_volume();
    const icon_t *vic = vv <= 0 ? &icon_vol_mute : vv < 34 ? &icon_vol_low
                       : vv < 67 ? &icon_vol_mid : &icon_vol_high;
    lv_obj_t *vi = ui_icon_img(p, vic, dim);
    lv_obj_set_pos(vi, qs_bar_icon_x(), qs_vol_y() + 4 - VOL_ICON_W / 2);
    lv_obj_t *vs = ui_slider(p, qs_bar_x(), qs_vol_y(), qs_bar_w(), 0, 100, vv);
    lv_obj_set_height(vs, 8);
    if (usb_bypass_active) {
        lv_obj_set_style_bg_color(vs, c16(dim), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(vs, c16(dim), LV_PART_KNOB);
    } else {
        lv_obj_set_style_bg_color(vs, c16(acc), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(vs, c16(acc), LV_PART_KNOB);
    }
    lv_obj_set_style_bg_color(vs, c16(line), LV_PART_MAIN);
    lv_obj_add_event_cb(vs, ui_qs_slider, LV_EVENT_VALUE_CHANGED, (void *)0);
    lv_obj_t *bi = ui_icon_img(p, &icon_sun, dim);
    lv_obj_set_user_data(bi, (void *)0xB1);       /* stays when brightness is dragged */
    lv_obj_set_pos(bi, qs_bar_icon_x(), qs_bar_y() + 4 - VOL_ICON_W / 2);
    lv_obj_t *bs = ui_slider(p, qs_bar_x(), qs_bar_y(), qs_bar_w(), 1,
                             qs_bright_max > 0 ? qs_bright_max : 100, qs_bright);
    lv_obj_set_height(bs, 8);
    lv_obj_set_style_bg_color(bs, c16(acc), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bs, c16(acc), LV_PART_KNOB);
    lv_obj_set_style_bg_color(bs, c16(line), LV_PART_MAIN);
    lv_obj_add_event_cb(bs, ui_qs_slider, LV_EVENT_VALUE_CHANGED, (void *)1);
    lv_obj_add_event_cb(bs, ui_qs_slider, LV_EVENT_RELEASED, (void *)1);
    lv_obj_add_event_cb(bs, ui_qs_bright_drag, LV_EVENT_ALL, NULL);

    char nm[64], sub[64];
    /* Wi-Fi / Bluetooth. */
    st_wifi_ssid(nm, sizeof(nm));
    if (!qs_wifi && wifi_block_note_until > time(NULL)) wifi_block_text(sub, sizeof(sub), 1);
    else snprintf(sub, sizeof(sub), "%s", qs_wifi ? (nm[0] ? nm : st_wifi_joining() ? "connecting..." : "not connected")
                                                  : wifi_on_blocked() ? "off for Bluetooth" : "off");
    qs_cell(p, 0, qs_wifi_y(), &icon_wifi_qs, qs_wifi, "Wi-Fi", sub, QS_WIFI, fg, dim, acc);
    st_bt_name(nm, sizeof(nm));
    int bt_conn = qs_bt && nm[0];
    char bt_codec[32] = "";
    int bt_batt = -1, bt_bars = 0, bt_rssi = 0;
    if (bt_conn) {
        pthread_mutex_lock(&bt_lock);
        snprintf(bt_codec, sizeof(bt_codec), "%s", bt_codec_cached);
        bt_batt = bt_batt_cached;
        bt_rssi = bt_rssi_cached;
        if (bt_sig_at && time(NULL) - bt_sig_at < 30) bt_bars = st_bt_signal_bars(bt_rssi);
        pthread_mutex_unlock(&bt_lock);
        snprintf(sub, sizeof(sub), "%s", bt_codec[0] ? bt_codec : bt_bars ? "" : "connected");
        if (bt_show_rssi && bt_bars)
            snprintf(sub + strlen(sub), sizeof(sub) - strlen(sub), "%s%d", sub[0] ? " \xc2\xb7 " : "", bt_rssi);
    } else {
        snprintf(sub, sizeof(sub), "%s", qs_bt ? "not connected" : "off");
    }
    lv_obj_t *btc = qs_cell(p, 1, qs_bt_y(), &icon_bt_qs, qs_bt, bt_conn ? nm : "Bluetooth",
                            sub, QS_BT, fg, dim, acc);
    if (bt_bars) {
        /* Four rising bars ahead of the codec, as Libra draws them. */
        lv_obj_t *s = lv_obj_get_child(btc, 2);
        lv_obj_set_x(s, QS_LABEL_X - 24 + 30);
        for (int k = 0; k < 4; k++) {
            int h = 6 + 4 * k;
            lv_obj_t *b = ui_plain(btc);
            lv_obj_set_pos(b, QS_LABEL_X - 24 + k * 6, 50 - h);
            lv_obj_set_size(b, 4, h);
            ui_bg(b, k < bt_bars ? dim : line);
        }
    }
    /* Parametric EQ (icon toggles, the rest picks a profile) / MSEB. */
    int eq_on = eq_enabled() && !usb_bypass_active;
    lv_obj_t *eqc = qs_cell(p, 0, qs_eq_y(), &icon_home_eq_sb, eq_on, "Parametric EQ",
                            usb_bypass_active ? "USB Transport" : card_released ? "USB Storage Mode"
                            : eq_cur_path[0] ? eq_cur.name : "no profile", QS_EQ_PICK, fg, dim, acc);
    lv_obj_t *eqi = ui_plain(eqc);
    lv_obj_set_size(eqi, 50, QS_ROW_H);
    lv_obj_add_flag(eqi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eqi, ui_qs_tap, LV_EVENT_CLICKED, (void *)QS_EQ_TOGGLE);
    qs_cell(p, 1, qs_mseb_y(), &icon_home_mseb_sb, mseb_on && !usb_bypass_active, "MSEB",
            usb_bypass_active ? "USB Transport" : card_released ? "USB Storage Mode" : "HiBy tuning bands",
            QS_MSEB, fg, dim, acc);
    /* Cover colours / USB mode. */
    qs_cell(p, 0, qs_cover_y(), &icon_palette_qs, cover_palette_enabled, "Cover colours",
            "From album art", QS_COVER, fg, dim, acc);
    int usb_mode = st_usb_mode();
    lv_obj_t *uc = qs_cell(p, 1, qs_usb_y(), &icon_usb_qs, 0, "USB mode",
                           usb_mode == 0 ? "ADB" : usb_mode == 1 ? "USB Storage" : "unplugged",
                           QS_USB, fg, dim, acc);
    ui_set_color(lv_obj_get_child(uc, 1), fg);
    lv_obj_t *chev = ui_label(uc, LV_SYMBOL_RIGHT, (lv_font_t *)&lv_font_montserrat_14, dim);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -6, 0);

    /* What is playing, and the batteries. */
    char fmt[48], kbps_line[24];
    qs_format_info(fmt, sizeof(fmt), kbps_line, sizeof(kbps_line));
    int by5 = qs_codec_y();
    lv_obj_t *di = ui_icon_img(p, &icon_disc_qs, fmt[0] ? acc : dim);
    lv_obj_set_pos(di, qs_col_x(0), by5 + 9);
    lv_obj_t *f1 = ui_label_w(p, fmt[0] ? fmt : "Nothing playing", uf_small, fmt[0] ? acc : dim,
                              qs_col_w() - (QS_LABEL_X - 24));
    lv_obj_set_pos(f1, qs_col_x(0) + QS_LABEL_X - 24, by5 + 6);
    if (kbps_line[0]) {
        lv_obj_t *f2 = ui_label(p, kbps_line, uf_small, dim);
        lv_obj_set_pos(f2, qs_col_x(0) + QS_LABEL_X - 24, by5 + 32);
    }
    int pct = st_battery_pct();
    if (pct >= 0) {
        uint16_t bc = st_charging() ? acc : (pct <= 15 ? RGB(230, 80, 70) : dim);
        int bx = qs_col_x(1) + 10;
        lv_obj_t *bic = ui_icon_img(p, qs_batt_icon(pct), bc);
        lv_obj_set_pos(bic, bx, by5 + 18);
        char pb[8];
        snprintf(pb, sizeof(pb), "%d%%", pct);
        lv_obj_t *pl = ui_label(p, pb, bt_batt >= 0 ? uf_small : uf_body, bc);
        lv_obj_set_pos(pl, bx + 43, bt_batt >= 0 ? by5 + 17 : by5 + 12);
        if (bt_batt >= 0) {
            lv_obj_update_layout(pl);
            int hx = bx + 43 + lv_obj_get_width(pl) + 12;
            uint16_t hc = bt_batt <= 15 ? RGB(230, 80, 70) : dim;
            lv_obj_t *bti = ui_icon_img(p, &icon_bt_sm, dim);
            lv_obj_set_pos(bti, hx, by5 + 28 - icon_bt_sm.h / 2);
            hx += icon_bt_sm.w + 6;
            lv_obj_t *hic = ui_icon_img(p, qs_batt_icon(bt_batt), hc);
            lv_obj_set_pos(hic, hx, by5 + 18);
            snprintf(pb, sizeof(pb), "%d%%", bt_batt);
            lv_obj_t *hl = ui_label(p, pb, uf_small, hc);
            lv_obj_set_pos(hl, hx + 43, by5 + 17);
        }
    }
    /* Footer: the output route, and the way into Settings. */
    int fy = qs_footer_y();
    lv_obj_t *gear = ui_icon_img(p, &icon_gear_qs, dim);
    lv_obj_set_pos(gear, FB_W - 24 - 20, fy + 8);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(gear, 20);
    lv_obj_add_event_cb(gear, ui_qs_tap, LV_EVENT_CLICKED, (void *)QS_GEAR);
    const char *route = audio_output();
    const icon_t *ric = !strcmp(route, "Bluetooth") ? &icon_bt_sm : !strcmp(route, "USB") ? &icon_usb_sm : NULL;
    if (ric) {
        lv_obj_t *r = ui_icon_img(p, ric, dim);
        lv_obj_set_pos(r, FB_W - 24 - 20 - 16 - ric->w, fy + 8 + (20 - ric->h) / 2);
    } else {
        lv_obj_t *r = ui_label(p, route, uf_small, dim);
        lv_obj_align(r, LV_ALIGN_TOP_RIGHT, -24 - 20 - 16, fy + 6);
    }
    /* The grab handle; a tap on it, or anywhere below the panel, closes it. */
    lv_obj_t *h = ui_plain(p);
    lv_obj_set_pos(h, FB_W / 2 - 60, QS_H - 34);
    lv_obj_set_size(h, 120, 34);
    lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h, ui_qs_tap, LV_EVENT_CLICKED, (void *)QS_CLOSE);
    lv_obj_t *hb = ui_plain(h);
    lv_obj_set_pos(hb, 34, 20);
    lv_obj_set_size(hb, 52, 4);
    ui_bg(hb, line);
}

/* ---- the volume popup, the seek toast, the power hold ---------------------- */

static struct { lv_obj_t *vol, *vol_icon, *vol_bar, *vol_fill, *toast, *toast_l, *power, *power_t,
                *power_fill, *power_sub; } pop_ui;

static void ui_vol_tap(lv_event_t *e) {
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int bw = vol_bar_w();
    int v = (p.x - vol_bar_x()) * 100 / (bw > 0 ? bw : 1);
    audio_volume_set(v < 0 ? 0 : v > 100 ? 100 : v);
    vol_ticks = VOL_TICKS;
    UI_ACTED();
}

static void ui_overlays_build(void) {
    lv_obj_t *v = pop_ui.vol = ui_plain(lv_layer_top());
    lv_obj_set_pos(v, VOL_POP_X, vol_pop_top());
    lv_obj_set_size(v, FB_W - 2 * VOL_POP_X, vol_pop_h());
    lv_obj_set_style_radius(v, VOL_POP_R, 0);
    lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v, ui_vol_tap, LV_EVENT_CLICKED, NULL);
    pop_ui.vol_icon = ui_icon_img(v, &icon_vol_mid, COL_DIM);
    lv_obj_set_pos(pop_ui.vol_icon, VOL_POP_PAD, (vol_pop_h() - VOL_ICON_W) / 2);
    pop_ui.vol_bar = ui_plain(v);
    lv_obj_set_pos(pop_ui.vol_bar, vol_bar_x() - VOL_POP_X, vol_pop_h() / 2 - 4);
    lv_obj_set_size(pop_ui.vol_bar, vol_bar_w(), 8);
    lv_obj_set_style_radius(pop_ui.vol_bar, 4, 0);
    pop_ui.vol_fill = ui_plain(v);
    lv_obj_set_pos(pop_ui.vol_fill, vol_bar_x() - VOL_POP_X, vol_pop_h() / 2 - 4);
    lv_obj_set_size(pop_ui.vol_fill, 0, 8);
    lv_obj_set_style_radius(pop_ui.vol_fill, 4, 0);
    lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *t = pop_ui.toast = ui_plain(lv_layer_top());
    ui_bg(t, COL_HEADER);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, c16(COL_LINE), 0);
    lv_obj_set_style_pad_hor(t, 20, 0);
    lv_obj_set_style_pad_ver(t, 6, 0);
    lv_obj_set_size(t, LV_SIZE_CONTENT, 44);
    pop_ui.toast_l = ui_label(t, "", uf_body, COL_ACCENT);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *pw = pop_ui.power = ui_plain(lv_layer_top());
    lv_obj_set_size(pw, FB_W, FB_H);
    lv_obj_set_style_bg_color(pw, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pw, LV_OPA_70, 0);
    lv_obj_add_flag(pw, LV_OBJ_FLAG_CLICKABLE);   /* nothing under it takes a tap */
    lv_obj_t *box = ui_plain(pw);
    lv_obj_set_size(box, FB_W - 96, 220);
    lv_obj_center(box);
    ui_bg(box, COL_HEADER);
    pop_ui.power_t = ui_label(box, "", uf_body, COL_TEXT);
    lv_obj_align(pop_ui.power_t, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_t *bar = ui_plain(box);
    lv_obj_set_pos(bar, 40, 100);
    lv_obj_set_size(bar, FB_W - 96 - 80, 14);
    ui_bg(bar, COL_LINE);
    pop_ui.power_fill = ui_plain(bar);
    lv_obj_set_size(pop_ui.power_fill, 0, 14);
    ui_bg(pop_ui.power_fill, COL_ACCENT);
    pop_ui.power_sub = ui_label(box, "", uf_small, COL_DIM);
    lv_obj_align(pop_ui.power_sub, LV_ALIGN_TOP_MID, 0, 134);
    lv_obj_add_flag(pw, LV_OBJ_FLAG_HIDDEN);
}

static void ui_drawer_sync(void);

static void ui_overlays_sync(int dirty) {
    ui_drawer_sync();
    /* The action sheet. */
    if (sheet_open && screen != SC_KEYBOARD) {
        if (sheet_open != sh_ui.shown) ui_sheet_open();
    } else if (sh_ui.shown) {
        ui_sheet_close();
    }
    /* Quick settings: built on opening, rebuilt as what it shows changes. */
    static int qs_was, pulled;
    if ((qs_open || ui_qs_pull >= 0) && !qs_was) qs_refresh();
    if (ui_qs_pull >= 0) {
        /* Following the finger down: the panel slides, the shade deepens. */
        if (!qs_ui.dim) ui_qs_build();
        lv_obj_set_style_translate_y(qs_ui.panel, ui_qs_pull - QS_H, 0);
        lv_obj_set_style_bg_opa(qs_ui.dim, (lv_opa_t)(LV_OPA_40 * ui_qs_pull / QS_H), 0);
        pulled = 1;
    } else if (qs_open) {
        if (pulled) {
            lv_obj_set_style_translate_y(qs_ui.panel, 0, 0);
            lv_obj_set_style_bg_opa(qs_ui.dim, LV_OPA_40, 0);
            pulled = 0;
        }
        if (!qs_ui.dim || (dirty && !(tch.down) && !qs_bright_drag)) ui_qs_build();
    } else if (qs_ui.dim) {
        ui_qs_close();
        pulled = 0;
    }
    qs_was = qs_open || ui_qs_pull >= 0;

    /* Volume, after a key press (vol_ticks counts it down). */
    if (vol_ticks > 0 && !qs_open) {
        int themed = qc_popup_themed();
        lv_obj_remove_flag(pop_ui.vol, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(pop_ui.vol);
        ui_bg(pop_ui.vol, themed ? qc_bg() : COL_HEADER);
        int v = audio_volume();
        const icon_t *vic = v <= 0 ? &icon_vol_mute : v < 34 ? &icon_vol_low : v < 67 ? &icon_vol_mid : &icon_vol_high;
        ui_icon_set(pop_ui.vol_icon, vic, themed ? qc_dim() : COL_DIM);
        ui_bg(pop_ui.vol_bar, themed ? qc_line() : COL_LINE);
        ui_bg(pop_ui.vol_fill, usb_bypass_active ? COL_DIM : themed ? qc_accent() : COL_ACCENT);
        lv_obj_set_width(pop_ui.vol_fill, vol_bar_w() * v / 100);
    } else {
        lv_obj_add_flag(pop_ui.vol, LV_OBJ_FLAG_HIDDEN);
    }

    /* "+10 s" and friends on Now Playing. */
    if (screen == SC_PLAYING && seek_toast_ticks > 0 && !qs_open && vol_ticks <= 0) {
        ui_set_text(pop_ui.toast_l, seek_toast_text);
        lv_obj_remove_flag(pop_ui.toast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(pop_ui.toast);
        lv_obj_update_layout(pop_ui.toast);
        lv_obj_set_pos(pop_ui.toast, (FB_W - lv_obj_get_width(pop_ui.toast)) / 2, STATUS_H + 12);
    } else {
        lv_obj_add_flag(pop_ui.toast, LV_OBJ_FLAG_HIDDEN);
    }

    /* Holding power. */
    if (power_hold_ui_shown) {
        lv_obj_remove_flag(pop_ui.power, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(pop_ui.power);
        long elapsed_ms = power_shutdown_kind ? 0 : (long)((us_now() - power_key_down_us) / 1000);
        int past_safe = power_shutdown_kind == 1 || elapsed_ms >= POWER_HOLD_SAFE_MS;
        ui_set_text(pop_ui.power_t, power_shutdown_kind == 1 ? "Shutting down"
                                   : power_shutdown_kind == 2 ? "Forcing shutdown"
                                   : past_safe ? "Release to shut down" : "Hold to power off");
        long clamped = power_shutdown_kind == 2 ? POWER_HOLD_FORCE_MS
                     : power_shutdown_kind == 1 ? POWER_HOLD_SAFE_MS : elapsed_ms;
        if (clamped > POWER_HOLD_FORCE_MS) clamped = POWER_HOLD_FORCE_MS;
        lv_obj_set_width(pop_ui.power_fill, (int)((FB_W - 96 - 80) * clamped / POWER_HOLD_FORCE_MS));
        char sub[48] = "";
        if (!power_shutdown_kind) {
            if (!past_safe) snprintf(sub, sizeof(sub), "%.1fs", (POWER_HOLD_SAFE_MS - elapsed_ms) / 1000.0);
            else snprintf(sub, sizeof(sub), "Forcing in %.1fs", (POWER_HOLD_FORCE_MS - elapsed_ms) / 1000.0);
        }
        ui_set_text(pop_ui.power_sub, sub);
        ui_dirty = 1;
    } else {
        lv_obj_add_flag(pop_ui.power, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- the resume splash -----------------------------------------------------
 * As Libra: the last screen, blurred and dimmed, on the panel the moment the
 * app starts and through the slow library open; once restore knows what is
 * coming back, a fade to the same picture at full brightness; once that
 * track's cover has decoded, a fade to the real screen. On LVGL's system
 * layer, above everything, and driven with lv_refr_now() because the first
 * two happen before the main loop is running. */
#define RS_FADE_STEPS   8
#define RS_FADE_STEP_US 28000
static struct { lv_obj_t *dim, *bridge; uint16_t *dim_px, *bridge_px;
                lv_image_dsc_t dim_dsc, bridge_dsc; } rs_ui;

static lv_obj_t *rs_image(uint16_t *px, lv_image_dsc_t *d) {
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = FB_W;
    d->header.h = FB_H;
    d->header.stride = FB_W * 2;
    d->data = (const uint8_t *)px;
    d->data_size = (uint32_t)FB_W * FB_H * 2;
    lv_obj_t *img = lv_image_create(lv_layer_sys());
    lv_image_set_src(img, d);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);    /* nothing under it takes a tap */
    return img;
}

/* Fades `img` from opacity a to b, frame by frame, blocking. */
static void rs_fade(lv_obj_t *img, int a, int b) {
    for (int s = 1; s <= RS_FADE_STEPS; s++) {
        lv_obj_set_style_opa(img, (lv_opa_t)(a + (b - a) * s / RS_FADE_STEPS), 0);
        lv_refr_now(NULL);
        usleep(RS_FADE_STEP_US);
    }
}

static void ui_resume_splash(void) {
    if (access(RESUME_THUMB, R_OK) != 0) return;     /* nothing saved: no splash */
    rs_ui.dim_px = malloc((size_t)FB_W * FB_H * 2);
    if (!rs_ui.dim_px) return;
    resume_splash(rs_ui.dim_px);                      /* reads the thumb, dimmed */
    rs_ui.dim = rs_image(rs_ui.dim_px, &rs_ui.dim_dsc);
    lv_refr_now(NULL);
}

/* 1 when the bridge is up and the frame loop should wait for the cover. */
static int ui_resume_bridge(void) {
    if (!rs_ui.dim) return 0;
    if (art_done) { ui_resume_finish(); return 0; }
    rs_ui.bridge_px = malloc((size_t)FB_W * FB_H * 2);
    if (!rs_ui.bridge_px) return 1;                   /* stay on the dim one */
    resume_upscale(rs_ui.bridge_px, 0);               /* the same thumb, full brightness */
    rs_ui.bridge = rs_image(rs_ui.bridge_px, &rs_ui.bridge_dsc);
    rs_fade(rs_ui.bridge, LV_OPA_TRANSP, LV_OPA_COVER);
    return 1;
}

static void ui_resume_finish(void) {
    if (!rs_ui.dim && !rs_ui.bridge) return;
    ui_sync(1);                       /* the real screen, built under the splash */
    lv_refr_now(NULL);
    lv_obj_t *top = rs_ui.bridge ? rs_ui.bridge : rs_ui.dim;
    if (rs_ui.bridge && rs_ui.dim) { lv_obj_delete(rs_ui.dim); rs_ui.dim = NULL; }
    rs_fade(top, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_obj_delete(top);
    rs_ui.dim = rs_ui.bridge = NULL;
    lv_image_cache_drop(&rs_ui.dim_dsc);
    lv_image_cache_drop(&rs_ui.bridge_dsc);
    free(rs_ui.dim_px);
    free(rs_ui.bridge_px);
    rs_ui.dim_px = rs_ui.bridge_px = NULL;
}

/* ---- the queue drawer, following the finger ---------------------------------
 * A picture of the queue page, built off screen and snapshotted once at the
 * start of the pull (Libra cached its two images for the slide the same
 * way), moved with the finger over the page; let go past a third of the
 * width and the queue becomes the screen under it. */
static struct { lv_obj_t *img; lv_draw_buf_t *snap; } qd_ui;

static void ui_drawer_sync(void) {
    if (ui_qd_pull < 0) {
        if (qd_ui.img) {
            if (ui.screen == SC_QUEUE || screen == SC_QUEUE) lv_refr_now(NULL);   /* the real one first */
            lv_obj_delete(qd_ui.img);
            qd_ui.img = NULL;
            lv_draw_buf_destroy(qd_ui.snap);
            qd_ui.snap = NULL;
        }
        return;
    }
    if (!qd_ui.img) {
        /* Build the queue page as if it were opening, without disturbing
         * the page on screen: its per-screen state is set aside and put
         * back. */
        ui_shown_t keep = ui;
        lv_obj_t **keep_rows = ui_rowv;
        int keep_n = ui_rown;
        ui_rowv = NULL; ui_rown = 0;
        memset(&ui, 0, sizeof(ui));
        ui_build_queue();
        if (ui.refresh) ui.refresh();
        lv_obj_t *qscr = ui.scr;
        lv_obj_update_layout(qscr);
        qd_ui.snap = lv_snapshot_take(qscr, LV_COLOR_FORMAT_RGB565);
        lv_obj_delete(qscr);
        free(ui_rowv);
        ui_rowv = keep_rows; ui_rown = keep_n;
        ui = keep;
        if (!qd_ui.snap) { ui_qd_pull = -1; return; }
        qd_ui.img = lv_image_create(lv_layer_top());
        lv_image_set_src(qd_ui.img, qd_ui.snap);
        lv_obj_set_style_shadow_width(qd_ui.img, 24, 0);
        lv_obj_set_style_shadow_color(qd_ui.img, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(qd_ui.img, LV_OPA_50, 0);
    }
    lv_obj_set_pos(qd_ui.img, FB_W - ui_qd_pull, 0);
    lv_obj_move_foreground(qd_ui.img);
}
