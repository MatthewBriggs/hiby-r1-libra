/* ui_lists.c — Home and the browsing screens. Part of ui_libra.c's
 * translation unit (see its top comment): actions are Libra's own, copied
 * from the old tap handler in music_hook.c with the hit-testing removed. */

/* Per-screen rows a refresh() revisits (the playing marker, a download's
 * progress). Freed by the next build. */
static lv_obj_t **ui_rowv;
static int ui_rown;

static void ui_rows_alloc(int n) {
    free(ui_rowv);
    ui_rowv = n > 0 ? calloc((size_t)n, sizeof(*ui_rowv)) : NULL;
    ui_rown = ui_rowv ? n : 0;
}

/* The playing-row treatment: COL_ROW behind, a 4px accent edge, the title
 * in the accent colour. Applied only when it changes. */
static void ui_row_playing(lv_obj_t *row, lv_obj_t *title, int playing,
                           uint16_t text, uint16_t acc, uint16_t rowbg) {
    int was = (int)(intptr_t)lv_obj_get_user_data(row);
    if (was == playing + 1) return;
    lv_obj_set_user_data(row, (void *)(intptr_t)(playing + 1));
    /* LVGL borders are one width all round, so the 4px accent edge is a
     * child of its own, made the first time and shown or hidden after. */
    if (playing) ui_bg(row, rowbg);
    else         lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_t *edge = NULL;
    uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
        if (lv_obj_get_user_data(c) == (void *)0xED6E) { edge = c; break; }
    }
    if (!edge && playing) {
        edge = ui_plain(row);
        lv_obj_set_user_data(edge, (void *)0xED6E);
        lv_obj_set_size(edge, 4, lv_pct(100));
        lv_obj_set_pos(edge, 0, 0);
    }
    if (edge) {
        ui_bg(edge, acc);
        if (playing) lv_obj_remove_flag(edge, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(edge, LV_OBJ_FLAG_HIDDEN);
    }
    if (title) ui_set_color(title, playing ? acc : text);
}

/* Right-aligned small text at the end of a row (Libra's draw_right()). */
static lv_obj_t *ui_right(lv_obj_t *row, const char *text, uint16_t col, int y) {
    lv_obj_t *l = ui_label(row, text, uf_small, col);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -24, y);
    return l;
}

/* ---- Home ------------------------------------------------------------------ */

static const icon_t *const home_icons[TOP_N] = {
    &icon_home_music, &icon_home_audiobooks, &icon_home_podcasts,
    &icon_home_eq, &icon_home_mseb, &icon_home_radio, &icon_home_stats, &icon_home_settings,
};
static struct { lv_obj_t *tile[TOP_N], *icon[TOP_N], *label[TOP_N], *spin[TOP_N], *usb; } home_ui;

static void ui_home_tap(lv_event_t *e) {
    int idx = ui_idx(e);
    int usb_storage = st_usb_mode() == 1;
    if (usb_storage && top_needs_card(idx)) return;    /* greyed out */
    if (idx == TOP_MUSIC) {
        screen = SC_MUSIC_MENU; reset_scroll();
    } else if (idx == TOP_AUDIOBOOKS) {
        ab_book_n = ab_scan_books(ab_books, AB_MAX_BOOKS);
        ab_rebuild_rows();
        screen = SC_AUDIOBOOKS; reset_scroll();
        total = ab_book_n;
        mlog("[music] %d audiobooks\n", ab_book_n);
    } else if (idx == TOP_PODCASTS) {
        pod_feed_n = pod_scan_feeds(pod_feeds, POD_MAX_FEEDS);
        pod_rebuild_rows();
        screen = SC_PODCASTS; reset_scroll();
        total = pod_feed_n;
        mlog("[music] %d podcast feeds\n", pod_feed_n);
    } else if (idx == TOP_EQ) {
        eq_profile_n = ep_scan(eq_profiles, EP_MAX_PROFILES);
        if (!eq_cur_path[0] && eq_profile_n > 0) {
            eq_switch_to(eq_profiles[0].path);
            save_conf();
        }
        screen = SC_EQ; reset_scroll();
        mlog("[music] %d EQ profiles\n", eq_profile_n);
    } else if (idx == TOP_MSEB) {
        screen = SC_MSEB; reset_scroll();
    } else if (idx == TOP_RADIO) {
        station_n = radio_load(stations, RADIO_MAX);
        screen = SC_RADIO; reset_scroll();
        mlog("[music] %d stations\n", station_n);
    } else if (idx == TOP_STATS) {
        screen = SC_STATS; reset_scroll();
    } else if (idx == TOP_SETTINGS) {
        screen = SC_SETTINGS; reset_scroll();
    }
    UI_ACTED();
}

static void ui_home_usb_done(lv_event_t *e) {
    (void)e;
    set_usb_storage_mode(0);
    UI_ACTED();
}

static void ui_home_refresh(void) {
    int usb_storage = st_usb_mode() == 1;
    for (int i = 0; i < TOP_N; i++) {
        int disabled = usb_storage && top_needs_card(i);
        int syncing = !disabled &&
                      ((i == TOP_PODCASTS && pod_update_running()) ||
                       (i == TOP_MUSIC && (scanner_scan_running() || index_scan_running())));
        if (syncing) {
            ui_bg(home_ui.tile[i], COL_ACCENT);
            lv_obj_add_flag(home_ui.icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(home_ui.spin[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_bg_opa(home_ui.tile[i], LV_OPA_TRANSP, 0);
            lv_obj_remove_flag(home_ui.icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(home_ui.spin[i], LV_OBJ_FLAG_HIDDEN);
            ui_icon_set(home_ui.icon[i], home_icons[i], disabled ? COL_DIM : COL_ACCENT);
        }
        ui_set_color(home_ui.label[i], syncing ? COL_BG : disabled ? COL_DIM : COL_TEXT);
    }
    if (usb_storage) lv_obj_remove_flag(home_ui.usb, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_add_flag(home_ui.usb, LV_OBJ_FLAG_HIDDEN);
}

static void ui_build_home(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    for (int i = 0; i < TOP_N; i++) {
        int tx, ty, tw, th;
        home_tile(i, &tx, &ty, &tw, &th);
        lv_obj_t *t = ui_plain(scr);
        home_ui.tile[i] = t;
        lv_obj_set_pos(t, tx, ty);
        lv_obj_set_size(t, tw, th);
        lv_obj_set_style_border_color(t, c16(COL_LINE), 0);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM |
                                     (tx + tw < FB_W ? LV_BORDER_SIDE_RIGHT : 0), 0);
        lv_obj_set_style_bg_color(t, c16(COL_ROW), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, ui_home_tap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        const icon_t *ic = home_icons[i];
        int gap = 14, iy = (th - (ic->h + gap + TEXT_PX_BODY)) / 2 + 4;
        home_ui.icon[i] = ui_icon_img(t, ic, COL_ACCENT);
        lv_obj_set_pos(home_ui.icon[i], (tw - ic->w) / 2, iy);
        lv_obj_t *sp = home_ui.spin[i] = lv_spinner_create(t);
        lv_obj_set_size(sp, ic->w - 16, ic->h - 16);
        lv_obj_set_pos(sp, (tw - ic->w) / 2 + 8, iy + 8);
        lv_obj_set_style_arc_width(sp, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 8, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, c16(COL_BG), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(sp, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp, c16(COL_BG), LV_PART_MAIN);
        lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(sp, LV_OBJ_FLAG_HIDDEN);
        home_ui.label[i] = ui_label(t, top_menu[i].label, uf_body, COL_TEXT);
        lv_obj_align(home_ui.label[i], LV_ALIGN_TOP_MID, 0, iy + ic->h + gap - 4);
    }
    /* USB Storage Mode, with Done to hand the card back (R81/R82). */
    lv_obj_t *usb = home_ui.usb = ui_plain(scr);
    lv_obj_set_size(usb, FB_W, 60);
    lv_obj_set_pos(usb, 0, FB_H - 60);
    ui_bg(usb, COL_ACCENT);
    lv_obj_t *m = ui_label(usb, "USB Storage Mode", uf_body, COL_BG);
    lv_obj_align(m, LV_ALIGN_LEFT_MID, 24, -2);
    lv_obj_t *d = ui_label(usb, "Done", uf_body, COL_BG);
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, -24, -2);
    lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(d, 24);
    lv_obj_add_event_cb(d, ui_home_usb_done, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(usb, LV_OBJ_FLAG_HIDDEN);
    ui.refresh = ui_home_refresh;
}

/* ---- the Music menu ---------------------------------------------------------- */

static void ui_music_menu_tap(lv_event_t *e) {
    int idx = ui_idx(e);
    if (idx == MENU_PLAYLISTS) {
        recent_mode = 0;
        playlist_n = pl_list(playlists, PL_MAX);
        screen = SC_PLAYLISTS; reset_scroll();
        mlog("[music] %d playlists\n", playlist_n);
    } else if (idx == MENU_RECENT_ADDED || idx == MENU_RECENT_HEARD) {
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
        mlog("[music] %s: %d\n", cur_facet_label, total);
    } else {
        recent_mode = 0;
        cur_facet = NULL;
        cur_facet_label = menu[idx].label;
        cur_artist[0] = '\0';
        albums_artist[0] = '\0';
        screen = SC_ALBUMS; reset_scroll();
        total = lib_albums_count(NULL, NULL);
        mlog("[music] all albums: %d\n", total);
    }
    UI_ACTED();
}

static void ui_build_music_menu(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    for (int i = 0; i < MENU_N; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_music_menu_tap, i);
        lv_obj_t *t = ui_label_w(row, menu[i].label, uf_body, COL_TEXT, FB_W - 48);
        lv_obj_set_pos(t, 24, 20);
    }
}

/* ---- Artists and Albums: virtual rows over the database -------------------
 * Thousands of rows: only those on screen (and a few either side) exist as
 * objects, rebound as the list scrolls. The data comes in windows of 96 from
 * lib_group()/lib_albums(), the same queries Libra's load_page() makes. */
#define VL_POOL 18
#define VL_WIN  96
static struct {
    lv_obj_t *row[VL_POOL], *name[VL_POOL], *count[VL_POOL];
    int idx[VL_POOL];
    lib_row_t win[VL_WIN];
    int win_base, win_n, total;
    lv_obj_t *spacer;
} vl;

static lib_row_t *vl_get(int i) {
    if (i < 0 || i >= vl.total) return NULL;
    if (recent_mode) return i < row_n ? &rows[i] : NULL;   /* computed once, in rows[] */
    if (i < vl.win_base || i >= vl.win_base + vl.win_n) {
        int base = i - VL_WIN / 3;
        if (base > vl.total - VL_WIN) base = vl.total - VL_WIN;
        if (base < 0) base = 0;
        vl.win_n = screen == SC_ARTISTS ? lib_group(cur_facet, vl.win, VL_WIN, base)
                                        : lib_albums(cur_facet, cur_artist, vl.win, VL_WIN, base);
        vl.win_base = base;
        if (vl.win_n < 0) vl.win_n = 0;
    }
    int local = i - vl.win_base;
    return local >= 0 && local < vl.win_n ? &vl.win[local] : NULL;
}

static void vl_bind(int slot, int i) {
    vl.idx[slot] = i;
    lib_row_t *r = vl_get(i);
    if (!r) { lv_obj_add_flag(vl.row[slot], LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(vl.row[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(vl.row[slot], i * ROW_H);
    ui_set_text(vl.name[slot], r->name);
    char buf[16] = "";
    if (r->count > 0) snprintf(buf, sizeof(buf), "%d", r->count);
    ui_set_text(vl.count[slot], buf);
}

static void vl_update(void) {
    if (!ui.list) return;
    int y = lv_obj_get_scroll_y(ui.list);
    int first = y / ROW_H - 3;
    if (first < 0) first = 0;
    for (int i = first; i < first + VL_POOL && i < vl.total; i++) {
        int slot = i % VL_POOL;
        if (vl.idx[slot] != i) vl_bind(slot, i);
    }
}

static void vl_scrolled(lv_event_t *e) {
    (void)e;
    vl_update();
}

static void vl_tap(lv_event_t *e) {
    int slot = ui_idx(e);
    int i = vl.idx[slot];
    lib_row_t *row = vl_get(i);
    if (!row) return;
    if (screen == SC_ARTISTS) {
        snprintf(cur_artist, sizeof(cur_artist), "%s",
                 row->owner[0] ? LIB_UNKNOWN_MARK : row->name);
        artists_scroll_saved = scroll;
        artists_scroll_px_saved = scroll_px;
        snprintf(artist_page_name, sizeof(artist_page_name), "%s", row->name);
        artist_page_from_list = 1;
        artist_page_col = cur_facet;
        snprintf(artist_page_val, sizeof(artist_page_val), "%s", cur_artist);
        artist_page_album_n = lib_albums(cur_facet, cur_artist,
                                         artist_page_albums, ARTIST_ALBUMS_MAX, 0);
        ac_regroup(-1);
        screen = SC_ARTIST_PAGE; reset_scroll();
        artist_art_request(artist_page_name);
        mlog("[music] %s -> %d albums\n", cur_artist, artist_page_album_n);
    } else {
        snprintf(cur_album, sizeof(cur_album), "%s", row->name);
        snprintf(cur_artist, sizeof(cur_artist), "%s", row->owner);
        browsing_is_playlist = 0;
        tracks_from_artist_page = 0;
        albums_scroll_saved = scroll;
        albums_scroll_px_saved = scroll_px;
        int count = row->count;
        screen = SC_TRACKS; reset_scroll();
        ab_list = 0;
        pod_list = 0;
        track_n = lib_tracks_for_album(cur_artist, cur_album,
                                       tracks, (int)(sizeof(tracks) / sizeof(tracks[0])), count);
        mlog("[music] %s -> %d tracks\n", cur_album, track_n);
        if (track_n > 0) view_art_request(tracks[0].path, cur_artist, cur_album);
    }
    UI_ACTED();
}

/* The A-Z strip down the right edge: drag along it and the list jumps to
 * that letter; slide left off it and it subdivides the letter under the
 * thumb into its second letters (Libra's INDEX_SUB_DEAD), back out returns
 * to A-Z. All of it is Libra's own index state and functions -- index_slot(),
 * index_jump(), the cached prefixes -- with the list then put where Libra's
 * `scroll` says. A big label beside the thumb shows the stop. */
static struct { lv_obj_t *strip, *lab[INDEX_N], *bubble, *bubble_l; int top, bot, sub_shown; } ix_ui;

static void ix_labels(void) {
    int n = index_stops();
    for (int i = 0; i < INDEX_N; i++) {
        if (i >= n) { lv_obj_add_flag(ix_ui.lab[i], LV_OBJ_FLAG_HIDDEN); continue; }
        char s[3];
        index_label(i, s);
        lv_label_set_text(ix_ui.lab[i], s);
        lv_obj_remove_flag(ix_ui.lab[i], LV_OBJ_FLAG_HIDDEN);
        int h = ix_ui.bot - ix_ui.top;
        lv_obj_align(ix_ui.lab[i], LV_ALIGN_TOP_RIGHT, -8, h * i / n + (h / n - 30) / 2);
    }
    ix_ui.sub_shown = index_sub_lock;
}

static void ix_reset(void) {
    index_shown = -1;
    index_sub_lock = 0;
    index_sub_n = 0;
    index_lock_end = -1;
    lv_obj_add_flag(ix_ui.bubble, LV_OBJ_FLAG_HIDDEN);
    if (ix_ui.sub_shown) ix_labels();
}

static void ix_go(void) {
    if (ui.list) lv_obj_scroll_to_y(ui.list, scroll * ROW_H, LV_ANIM_OFF);
    vl_update();
}

static void ix_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) { ix_reset(); return; }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    live_x = p.x;
    live_y = p.y;
    index_prepare();
    int slot = index_slot(live_y);
    if (slot != index_shown) {
        index_shown = slot;
        index_jump(live_y);
        ix_go();
    }
    int deep = live_x < FB_W - INDEX_TOUCH_W - INDEX_SUB_DEAD;
    int fits = index_lock_end >= 0 && index_lock_end - scroll <= vis_rows();
    if (deep && !index_sub_lock && index_shown > 0 && !fits) {
        char L = index_letter(index_shown);
        int n = 0;
        for (int k = 0; k < index_pfx_n && n < INDEX_SUB_MAX; k++)
            if (index_pfx[2 * k] == L) index_sub[n++] = index_pfx[2 * k + 1];
        if (n > 1) {
            index_sub_lock = L;
            index_sub_n = n;
            index_shown = -1;
            index_jump(live_y);
            ix_go();
            ix_labels();
        }
    } else if (!deep && index_sub_lock) {
        index_sub_lock = 0; index_sub_n = 0; index_lock_end = -1;
        index_shown = -1;
        index_jump(live_y);
        ix_go();
        ix_labels();
    }
    char s[3];
    int cur = index_slot(live_y);
    if (cur < 0) cur = 0;
    index_label(cur, s);
    lv_label_set_text(ix_ui.bubble_l, s);
    lv_obj_remove_flag(ix_ui.bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(ix_ui.bubble, p.y - 40 < CONTENT_Y ? CONTENT_Y : p.y - 40);
}

static void ix_build(lv_obj_t *scr) {
    ix_ui.top = CONTENT_Y + 4;
    ix_ui.bot = index_bottom() - 4;
    lv_obj_t *s = ix_ui.strip = ui_plain(scr);
    lv_obj_set_pos(s, FB_W - INDEX_TOUCH_W, ix_ui.top);
    lv_obj_set_size(s, INDEX_TOUCH_W, ix_ui.bot - ix_ui.top);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(s, LV_OBJ_FLAG_PRESS_LOCK);   /* keeps the thumb sliding left off it */
    lv_obj_add_event_cb(s, ix_event, LV_EVENT_ALL, NULL);
    for (int i = 0; i < INDEX_N; i++) ix_ui.lab[i] = ui_label(s, "", uf_small, COL_DIM);
    lv_obj_t *b = ix_ui.bubble = ui_plain(scr);
    lv_obj_set_size(b, 96, 80);
    lv_obj_set_x(b, FB_W - INDEX_TOUCH_W - 112);
    lv_obj_set_style_radius(b, 16, 0);
    ui_bg(b, COL_ACCENT);
    ix_ui.bubble_l = ui_label(b, "", uf_big, COL_BG);
    lv_obj_center(ix_ui.bubble_l);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    index_shown = -1;
    index_sub_lock = 0;
    index_sub_n = 0;
    ix_labels();
}

static void ui_build_library_list(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    memset(&vl, 0, sizeof(vl));
    vl.total = total > 0 ? total : 0;
    vl.win_base = 0;
    vl.win_n = 0;
    /* The scroll range: one invisible pixel at the end of the last row. */
    vl.spacer = ui_plain(l);
    lv_obj_set_size(vl.spacer, 1, 1);
    lv_obj_set_pos(vl.spacer, 0, vl.total * ROW_H - 1);
    for (int s = 0; s < VL_POOL; s++) {
        vl.row[s] = ui_row(l, 0, ROW_H, vl_tap, s);
        vl.name[s] = ui_label_w(vl.row[s], "", uf_body, COL_TEXT, FB_W - 110);
        lv_obj_set_pos(vl.name[s], 24, 20);
        vl.count[s] = ui_right(vl.row[s], "", COL_DIM, 22);
        if (index_visible()) lv_obj_align(vl.count[s], LV_ALIGN_TOP_RIGHT, -24 - INDEX_W, 22);
        vl.idx[s] = -1;
        lv_obj_add_flag(vl.row[s], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_event_cb(l, vl_scrolled, LV_EVENT_SCROLL, NULL);
    if (vl.total == 0)
        ui_empty_note(l, 0, recent_mode ? "Nothing here yet" : "No music found");
    vl_update();
    lv_obj_add_event_cb(l, vl_scrolled, LV_EVENT_SIZE_CHANGED, NULL);
    if (index_visible() && vl.total > 0) ix_build(scr);
}

/* ---- the album page -------------------------------------------------------- */

static struct { lv_obj_t *cover; int art_gone; int view_seq; } ap_ui;

static void ui_album_artist_tap(lv_event_t *e) {
    (void)e;
    if (!cur_artist[0]) return;
    snprintf(artist_page_back_album, sizeof(artist_page_back_album), "%s", cur_album);
    snprintf(artist_page_back_artist, sizeof(artist_page_back_artist), "%s", cur_artist);
    snprintf(artist_page_name, sizeof(artist_page_name), "%s", cur_artist);
    artist_page_from_list = 0;
    artist_page_col = "album_artist";
    snprintf(artist_page_val, sizeof(artist_page_val), "%s", artist_page_name);
    artist_page_album_n = lib_albums("album_artist", artist_page_name,
                                     artist_page_albums, ARTIST_ALBUMS_MAX, 0);
    ac_regroup(-1);
    screen = SC_ARTIST_PAGE;
    reset_scroll();
    artist_art_request(artist_page_name);
    UI_ACTED();
}

static void ui_track_tap(lv_event_t *e) {
    if (!ui_tap_ok()) return;
    int idx = ui_idx(e);
    if (idx >= 0 && idx < track_n) {
        audio_set_speed(1000);
        play_request(RECQ_TRACK, idx);
    }
    UI_ACTED();
}

static void ui_track_hold(lv_event_t *e);   /* the action sheet, ui_overlays.c */

static void ui_cover_hold(lv_event_t *e) {
    (void)e;
    art_refresh_request(tracks[0].path, cur_artist, cur_album, cur_artist, 1);
    snprintf(sheet_note, sizeof(sheet_note), "Refreshing artwork");
    lv_indev_reset(lv_indev_active(), NULL);
    UI_ACTED();
}

static void ui_note_refresh(uint16_t bg, uint16_t col);
static void ui_note_make(lv_obj_t *scr);

static void ui_album_refresh(void) {
    if (cover_palette_enabled) view_compute_cover_palette();
    /* The cover arriving (or being confirmed missing) changes the palette
     * and the header's layout: rebuilt, at the same scroll. */
    int gone = view_art_gone();
    if (gone != ap_ui.art_gone || ui_view_art.seq != ap_ui.view_seq) {
        ui_rebuild_req = 1;
        return;
    }
    for (int i = 0; i < ui_rown && i < track_n; i++) {
        lv_obj_t *row = ui_rowv[i];
        if (!row) continue;
        ui_row_playing(row, lv_obj_get_child(row, 1), album_row_playing(i),
                       np_view_col_fg(), np_view_col_accent(), COL_ROW);
    }
    ui_note_refresh(np_view_col_bg(), np_view_col_accent());
}

static void ui_build_album_page(void) {
    if (cover_palette_enabled) view_compute_cover_palette();
    g_view_art_gone_frame = view_art_gone();
    ap_ui.art_gone = g_view_art_gone_frame;
    ap_ui.view_seq = ui_view_art.seq;
    uint16_t bg = np_view_col_bg(), fg = np_view_col_fg(), dim = np_view_col_dim(),
             acc = np_view_col_accent(), line = np_view_col_line();
    lv_obj_t *scr = ui.scr = ui_new_screen(bg);
    lv_obj_t *l = ui_list(scr, 0);

    if (!g_view_art_gone_frame) {
        ap_ui.cover = ui_art_box(l, 0, 0, ART_PX, COL_ROW);
        ui_art_box_set(ap_ui.cover, &ui_view_art);
        /* Held: fetch the cover again (Libra's hold on the artwork). */
        if (!browsing_is_playlist && track_n > 0) {
            lv_obj_add_flag(ap_ui.cover, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(ap_ui.cover, ui_cover_hold, LV_EVENT_LONG_PRESSED, NULL);
        }
    }
    char hdr[LIB_NAME_LEN + 12];
    if (browsing_is_playlist) snprintf(hdr, sizeof(hdr), "Playlist: %s", cur_album);
    else                      snprintf(hdr, sizeof(hdr), "%s", cur_album);
    lv_obj_t *t = ui_label_w(l, hdr, uf_title, fg, FB_W - 48);
    lv_obj_set_pos(t, 24, tracks_hdr_title_y());
    if (cur_artist[0]) {
        lv_obj_t *a = ui_label_w(l, cur_artist, uf_body, dim, FB_W - 48);
        lv_obj_set_pos(a, 24, tracks_hdr_artist_y());
        lv_obj_add_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(a, 10);
        lv_obj_add_event_cb(a, ui_album_artist_tap, LV_EVENT_CLICKED, NULL);
    }
    {
        char cbuf[24], fbuf[64], dbuf[16], line_txt[140];
        snprintf(cbuf, sizeof(cbuf), "%d track%s", track_n, track_n == 1 ? "" : "s");
        lib_track_t *t0 = track_n > 0 ? &tracks[0] : NULL;
        int64_t total_ms = 0;
        int uniform = t0 != NULL;
        /* As Libra's: a lossy album is one format at varying bitrates and is
         * labelled by its average; a lossless one must agree on depth and
         * rate too. */
        int lossy = t0 && track_is_lossy(t0);
        int64_t kbps_sum = 0;
        int kbps_n = 0;
        for (int i = 0; i < track_n; i++) {
            total_ms += tracks[i].dur_ms;
            if (tracks[i].format != t0->format ||
                (!lossy && (tracks[i].bits != t0->bits || tracks[i].rate != t0->rate)))
                uniform = 0;
            if (tracks[i].bitrate > 0) { kbps_sum += tracks[i].bitrate / 1000; kbps_n++; }
        }
        fbuf[0] = '\0';
        if (uniform && lossy && kbps_n)
            snprintf(fbuf, sizeof(fbuf), "%s  %d kbps", track_format_name(t0),
                     (int)(kbps_sum / kbps_n));
        else if (uniform)
            fmt_track_quality(fbuf, sizeof(fbuf), t0);
        fmt_dur(dbuf, sizeof(dbuf), total_ms);
        /* The format in the accent colour, the rest dim: recolour markup. */
        char hex_dim[8], hex_acc[8];
        lv_color_t cd = c16(dim), ca = c16(acc);
        snprintf(hex_dim, sizeof(hex_dim), "%02X%02X%02X", cd.red, cd.green, cd.blue);
        snprintf(hex_acc, sizeof(hex_acc), "%02X%02X%02X", ca.red, ca.green, ca.blue);
        if (fbuf[0])
            snprintf(line_txt, sizeof(line_txt), "%s  \xc2\xb7  #%s %s#  \xc2\xb7  %s",
                     cbuf, hex_acc, fbuf, dbuf);
        else
            snprintf(line_txt, sizeof(line_txt), "%s  \xc2\xb7  %s", cbuf, dbuf);
        lv_obj_t *info = ui_label_w(l, line_txt, uf_small, dim, FB_W - 48);
        lv_label_set_recolor(info, true);
        lv_obj_set_pos(info, 24, tracks_hdr_info_y());
    }
    int header_h = tracks_hdr_h();
    ui_hline(l, header_h - 1, line);

    int multi_disc = tracks_multi_disc();
    int ry = header_h, last_disc = -999;
    char buf[32];
    ui_rows_alloc(track_n);
    for (int idx = 0; idx < track_n; idx++) {
        lib_track_t *tr = &tracks[idx];
        int disc = track_disc_num(idx);
        if (multi_disc && disc != last_disc) {
            int64_t disc_ms = 0;
            for (int j = idx; j < track_n && track_disc_num(j) == disc; j++) disc_ms += tracks[j].dur_ms;
            lv_obj_t *b = ui_plain(l);
            lv_obj_set_pos(b, 0, ry);
            lv_obj_set_size(b, FB_W, DISC_BANNER_H);
            ui_bg(b, COL_HEADER);
            lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_border_color(b, c16(line), 0);
            lv_obj_t *ic = ui_icon_img(b, &icon_disc, dim);
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, 20, 0);
            /* The disc's name when its tags give one, else its number. */
            const char *dname = disc_title_for(idx);
            if (dname[0]) snprintf(buf, sizeof(buf), "%s", dname);
            else          snprintf(buf, sizeof(buf), "Disc %d", disc);
            lv_obj_t *dl = ui_label(b, buf, uf_body, fg);
            lv_obj_align(dl, LV_ALIGN_LEFT_MID, 58, -2);
            fmt_dur(buf, sizeof(buf), disc_ms);
            lv_obj_t *dd = ui_label(b, buf, uf_small, dim);
            lv_obj_align(dd, LV_ALIGN_RIGHT_MID, -24, 0);
            ry += DISC_BANNER_H;
            last_disc = disc;
        }
        lv_obj_t *row = ui_row(l, ry, ROW_H, ui_track_tap, idx);
        lv_obj_set_style_border_color(row, c16(line), 0);
        lv_obj_add_event_cb(row, ui_track_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);
        if (browsing_is_playlist) {
            ui_row_swipeable(row, idx, playlist_remove_track);
            ui_row_grip(row, idx, track_n, playlist_move_track, dim);
        }
        ui_rowv[idx] = row;
        if (!browsing_is_playlist && tr->track > 0) snprintf(buf, sizeof(buf), "%d", tr->track);
        else buf[0] = '\0';
        lv_obj_t *num = ui_label(row, buf, uf_small, dim);
        lv_obj_set_pos(num, 20, 22);
        lv_obj_t *name = ui_label_w(row, tr->name, uf_body, fg, FB_W - 110 - 68);
        lv_obj_set_pos(name, 68, 20);
        if (tr->dur_ms > 0) {
            fmt_dur(buf, sizeof(buf), tr->dur_ms);
            lv_obj_t *dl = ui_right(row, buf, dim, 22);
            if (browsing_is_playlist) lv_obj_align(dl, LV_ALIGN_TOP_RIGHT, -24 - 40, 22);
        }
        ry += ROW_H;
    }
    ui_note_make(scr);
    ui.refresh = ui_album_refresh;
}

/* ---- an artist's page ------------------------------------------------------ */

static struct { int art_seq, loading, bio_len; } arp_ui;
static struct { int x0, y0, dx, on, nb, ate; int header_h; } acs;

static void ui_artist_cat_tap(lv_event_t *e) {
    int c = ui_idx(e);
    if (c >= 0 && c != ac_cur) ac_regroup(c);
    UI_ACTED();
}

static void ui_artist_album_tap(lv_event_t *e) {
    if (acs.ate) { acs.ate = 0; return; }
    int idx = ui_idx(e);
    if (idx < 0 || idx >= ap_view_n) return;
    lib_row_t *r = &artist_page_albums[ap_view[idx]];
    snprintf(cur_album, sizeof(cur_album), "%s", r->name);
    snprintf(cur_artist, sizeof(cur_artist), "%s", r->owner);
    browsing_is_playlist = 0;
    tracks_from_artist_page = 1;
    int count = r->count;
    screen = SC_TRACKS; reset_scroll();
    ab_list = 0;
    pod_list = 0;
    track_n = lib_tracks_for_album(cur_artist, cur_album,
                                   tracks, (int)(sizeof(tracks) / sizeof(tracks[0])), count);
    if (track_n > 0) view_art_request(tracks[0].path, cur_artist, cur_album);
    UI_ACTED();
}

static void ui_album_hold(lv_event_t *e);   /* ui_overlays.c */

static void ui_artist_refresh(void) {
    if (ui_artist_art.seq != arp_ui.art_seq || artist_art_loading != arp_ui.loading ||
        (int)strlen(artist_bio_text) != arp_ui.bio_len)
        ui_rebuild_req = 1;
    ui_note_refresh(COL_BG, COL_ACCENT);
}

/* Sideways across the album list: the neighbouring category slides in, as
 * Libra's ac_slide -- the rows follow the finger (a third as far with
 * nothing that way), and let go past a quarter of the width the page turns. */

static void acs_shift(int dx) {
    uint32_t n = lv_obj_get_child_count(ui.list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(ui.list, (int32_t)i);
        if (lv_obj_get_y(c) >= acs.header_h) lv_obj_set_style_translate_x(c, dx, 0);
    }
}

static int ui_artist_slide_dir;   /* for the page-turn animation: -1 left, +1 right */

static void ui_artist_slide(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || !ac_bar_shown()) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        acs.x0 = p.x; acs.y0 = p.y; acs.on = 0; acs.ate = 0;
    } else if (code == LV_EVENT_PRESSING) {
        int dx = p.x - acs.x0, dy = p.y - acs.y0;
        if (!acs.on && abs(dx) > AC_SLIDE_START && abs(dx) > 2 * abs(dy) &&
            acs.x0 >= EDGE_ZONE && acs.x0 < FB_W - QD_EDGE_ZONE) {
            acs.on = 1;
            lv_obj_remove_flag(ui.list, LV_OBJ_FLAG_SCROLLABLE);
        }
        if (acs.on) {
            acs.nb = ac_neighbour(dx < 0 ? +1 : -1);
            acs.dx = acs.nb >= 0 ? dx : dx / 3;
            acs_shift(acs.dx);
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && acs.on) {
        acs.on = 0;
        acs.ate = 1;
        lv_obj_add_flag(ui.list, LV_OBJ_FLAG_SCROLLABLE);
        if (code == LV_EVENT_RELEASED && acs.nb >= 0 && abs(acs.dx) > FB_W / 4) {
            ui_artist_slide_dir = acs.dx < 0 ? -1 : +1;
            ac_regroup(acs.nb);
            UI_ACTED();
        } else {
            acs_shift(0);
        }
    }
}

static void ui_build_artist_page(void) {
    arp_ui.art_seq = ui_artist_art.seq;
    arp_ui.loading = artist_art_loading;
    arp_ui.bio_len = (int)strlen(artist_bio_text);
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, 0);
    lv_obj_t *photo = ui_art_box(l, 0, 0, ART_PX, COL_ROW);
    ui_art_box_set(photo, &ui_artist_art);

    if (ac_bar_shown()) {
        ac_bar_layout();
        lv_obj_t *bar = ui_plain(l);
        lv_obj_set_pos(bar, 0, artist_page_bar_y());
        lv_obj_set_size(bar, FB_W, AC_BAR_H);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(bar, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(bar, 1, 0);
        lv_obj_set_style_border_color(bar, c16(COL_LINE), 0);
        lv_obj_t *cur_lbl = NULL;
        for (int i = 0; i < ac_bar_n; i++) {
            int cur = ac_bar_cat[i] == ac_cur;
            lv_obj_t *lb = ui_label(bar, ac_bar_label[i], uf_small, cur ? COL_ACCENT : COL_DIM);
            lv_obj_set_pos(lb, ac_bar_x0[i], 16);
            lv_obj_add_flag(lb, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(lb, 12);
            lv_obj_add_event_cb(lb, ui_artist_cat_tap, LV_EVENT_CLICKED, (void *)(intptr_t)ac_bar_cat[i]);
            if (cur) {
                lv_obj_t *u = ui_plain(bar);
                lv_obj_set_pos(u, ac_bar_x0[i], AC_BAR_H - 12);
                lv_obj_set_size(u, ac_bar_x1[i] - ac_bar_x0[i], 3);
                ui_bg(u, COL_ACCENT);
                cur_lbl = lb;
            }
        }
        if (cur_lbl) lv_obj_scroll_to_view(cur_lbl, LV_ANIM_OFF);
    }
    lv_obj_t *name = ui_label_w(l, artist_page_name, uf_title, COL_TEXT, FB_W - 48);
    lv_obj_set_pos(name, 24, artist_page_title_y());
    {
        int other = 0;
        for (int c = 0; c < AC_N; c++) if (c != AC_STUDIO && ac_count[c]) other = 1;
        char cbuf[32];
        snprintf(cbuf, sizeof(cbuf), "%d %s%s", artist_page_album_n,
                 other ? "release" : "album", artist_page_album_n == 1 ? "" : "s");
        lv_obj_t *info = ui_label(l, cbuf, uf_small, COL_DIM);
        lv_obj_set_pos(info, 24, artist_page_info_y());
    }
    int header_h = artist_page_hdr_h();
    ui_hline(l, header_h - 1, COL_LINE);
    acs.header_h = header_h;
    acs.on = 0;
    lv_obj_add_event_cb(l, ui_artist_slide, LV_EVENT_ALL, NULL);
    char buf[16];
    for (int idx = 0; idx < ap_view_n; idx++) {
        lib_row_t *r = &artist_page_albums[ap_view[idx]];
        lv_obj_t *row = ui_row(l, header_h + idx * ROW_H, ROW_H, ui_artist_album_tap, idx);
        lv_obj_add_event_cb(row, ui_album_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);   /* the list sees the slide */
        lv_obj_t *t = ui_label_w(row, r->name, uf_body, COL_TEXT, FB_W - 90 - 24);
        lv_obj_set_pos(t, 24, 20);
        snprintf(buf, sizeof(buf), "%d", r->count);
        ui_right(row, buf, COL_DIM, 22);
    }
    int bio_y = header_h + ap_view_n * ROW_H + 24;
    if (artist_art_loading) {
        lv_obj_t *ld = ui_label(l, "Loading artist info...", uf_small, COL_DIM);
        lv_obj_set_pos(ld, 24, bio_y);
    } else if (artist_bio_text[0]) {
        lv_obj_t *bio = ui_label(l, artist_bio_text, uf_small, COL_TEXT);
        lv_label_set_long_mode(bio, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(bio, FB_W - 48);
        lv_obj_set_style_text_line_space(bio, 32 - TEXT_PX_SMALL - 6, 0);
        lv_obj_set_pos(bio, 24, bio_y);
        /* Room to read the last line above the mini player. */
        lv_obj_set_style_pad_bottom(bio, 24, 0);
    }
    ui_note_make(scr);
    ui.refresh = ui_artist_refresh;
}

/* ---- a podcast's page ------------------------------------------------------ */

static struct { int view_seq, art_gone; } pp_ui;

static void ui_pod_gear_tap(lv_event_t *e) {
    (void)e;
    pod_skip_lookup(cur_feed, &pod_set_intro, &pod_set_outro);
    if (pod_auto_state != 1) pod_auto_state = 0;
    screen = SC_POD_SETTINGS; reset_scroll();
    UI_ACTED();
}

static void ui_episode_tap(lv_event_t *e) {
    if (!ui_tap_ok()) return;
    int pi = ui_idx(e);
    if (pi < 0 || pi >= pod_ep_n) return;
    sheet_note[0] = '\0';
    if (pod_eps[pi].downloaded) {
        play_request(RECQ_EPISODE, pi);
    } else if (net_held() || wifi_bt_parked) {
        if (pod_dl_waiting(cur_feed, pod_eps[pi].name)) {
            snprintf(sheet_note, sizeof(sheet_note), "Already waiting to download");
        } else if (pod_dl_wait_n >= POD_DL_WAIT_MAX) {
            snprintf(sheet_note, sizeof(sheet_note), "Too many downloads waiting");
        } else {
            snprintf(pod_dl_wait[pod_dl_wait_n].feed, sizeof(pod_dl_wait[0].feed), "%s", cur_feed);
            snprintf(pod_dl_wait[pod_dl_wait_n].name, sizeof(pod_dl_wait[0].name), "%s", pod_eps[pi].name);
            pod_dl_wait_n++;
            snprintf(sheet_note, sizeof(sheet_note), "Downloads when Bluetooth playback pauses");
        }
    } else if (!pod_download_active()) {
        pod_download_start(pi);
    }
    UI_ACTED();
}

/* What an episode row shows on its right: its length, or the download. */
static void ui_episode_right(int idx, char *buf, size_t n) {
    lib_track_t *t = &tracks[idx];
    if (!pod_eps[idx].downloaded) {
        if (pod_download_active() && !strcmp(pod_download_feed(), cur_feed) &&
            !strcmp(pod_download_name(), pod_eps[idx].name)) {
            long tot = pod_download_total(), got = pod_download_bytes();
            if (tot > 0 && got >= 0 && got <= tot)
                snprintf(buf, n, "%d%%", (int)((long long)got * 100 / tot));
            else
                snprintf(buf, n, "%ld KB", got / 1024);
        } else if (pod_dl_waiting(cur_feed, pod_eps[idx].name)) {
            snprintf(buf, n, "Waiting");
        } else {
            snprintf(buf, n, "Download");
        }
    } else if (t->dur_ms > 0) {
        fmt_dur(buf, n, t->dur_ms);
    } else {
        buf[0] = '\0';
    }
}

static lv_obj_t *ui_note_label;   /* sheet_note, over the foot of the page */

static void ui_note_refresh(uint16_t bg, uint16_t col) {
    if (!ui_note_label) return;
    if (!sheet_note[0]) { lv_obj_add_flag(ui_note_label, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(ui_note_label, LV_OBJ_FLAG_HIDDEN);
    ui_set_text(ui_note_label, sheet_note);
    ui_bg(ui_note_label, bg);
    ui_set_color(ui_note_label, col);
    lv_obj_set_y(ui_note_label, FB_H - 46 - (mini_visible() ? MINI_H : 0));
}

static void ui_note_make(lv_obj_t *scr) {
    ui_note_label = ui_label(scr, "", uf_small, COL_ACCENT);
    lv_obj_set_size(ui_note_label, FB_W, 40);
    lv_obj_set_style_pad_left(ui_note_label, 24, 0);
    lv_obj_set_style_pad_top(ui_note_label, 8, 0);
    lv_obj_add_flag(ui_note_label, LV_OBJ_FLAG_HIDDEN);
}

static void ui_podcast_page_refresh(void) {
    int th = cover_palette_enabled;
    if (th) view_compute_cover_palette();
    if (ui_view_art.seq != pp_ui.view_seq || view_art_gone() != pp_ui.art_gone) {
        ui_rebuild_req = 1;
        return;
    }
    int pod_th = th && np_view_palette_valid;
    uint16_t text = pod_th ? np_view_col_fg() : COL_TEXT, dim = pod_th ? np_view_col_dim() : COL_DIM,
             acc = pod_th ? np_view_col_accent() : COL_ACCENT,
             rowbg = pod_th ? np_blend(np_view_bg, np_view_fg, 0.12f) : COL_ROW,
             bg = pod_th ? np_view_col_bg() : COL_BG;
    char buf[32];
    for (int i = 0; i < ui_rown && i < track_n; i++) {
        lv_obj_t *row = ui_rowv[i];
        if (!row) continue;
        lib_track_t *t = &tracks[i];
        int playing = audio_is_active() && podcast_mode && cur_track == 0 &&
                      queue_n > 0 && t->path[0] && !strcmp(t->path, queue[0].path);
        lv_obj_t *name = lv_obj_get_child(row, 0);
        ui_row_playing(row, NULL, playing, text, acc, rowbg);
        ui_set_color(name, playing ? acc : !pod_eps[i].downloaded ? dim : text);
        ui_episode_right(i, buf, sizeof(buf));
        ui_set_text(lv_obj_get_child(row, 1), buf);
    }
    ui_note_refresh(bg, acc);
}

static void ui_build_podcast_page(void) {
    int th = cover_palette_enabled;
    /* The feed's cover: the first downloaded episode's, as Libra asks for. */
    {
        const char *ep = "";
        char cover[POD_NAME_LEN + 64];
        for (int i = 0; i < pod_ep_n; i++)
            if (pod_eps[i].downloaded && pod_eps[i].path[0]) { ep = pod_eps[i].path; break; }
        if (!ep[0]) {
            snprintf(cover, sizeof(cover), "/data/mnt/sd_0/Podcasts/%s/cover.jpg", cur_feed);
            ep = cover;
        }
        static char asked[POD_NAME_LEN];
        if (strcmp(asked, cur_feed)) {
            snprintf(asked, sizeof(asked), "%s", cur_feed);
            view_art_request(ep, "", cur_feed);
        }
    }
    if (th) view_compute_cover_palette();
    g_view_art_gone_frame = view_art_gone();
    pp_ui.view_seq = ui_view_art.seq;
    pp_ui.art_gone = g_view_art_gone_frame;
    int pod_th = th && np_view_palette_valid;
    g_pod_theme = pod_th;
    uint16_t text = pod_th ? np_view_col_fg() : COL_TEXT, dim = pod_th ? np_view_col_dim() : COL_DIM,
             line = pod_th ? np_view_col_line() : COL_LINE, bg = pod_th ? np_view_col_bg() : COL_BG,
             rowbg = pod_th ? np_blend(np_view_bg, np_view_fg, 0.12f) : COL_ROW;

    lv_obj_t *scr = ui.scr = ui_new_screen(bg);
    lv_obj_t *l = ui_list(scr, 0);
    if (!g_view_art_gone_frame) {
        lv_obj_t *c = ui_art_box(l, 0, 0, ART_PX, rowbg);
        ui_art_box_set(c, &ui_view_art);
    }
    lv_obj_t *t = ui_label_w(l, cur_feed, uf_title, text, POD_GEAR_X - 16 - 24);
    lv_obj_set_pos(t, 24, tracks_hdr_title_y());
    lv_obj_t *gear = ui_icon_img(l, &icon_home_settings_sb, dim);
    lv_obj_set_pos(gear, POD_GEAR_X, tracks_hdr_title_y() + 2);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(gear, 20);
    lv_obj_add_event_cb(gear, ui_pod_gear_tap, LV_EVENT_CLICKED, NULL);

    int down = 0;
    int64_t total_ms = 0, left_ms = 0;
    for (int i = 0; i < pod_ep_n; i++) {
        if (!pod_eps[i].downloaded) continue;
        down++;
        int64_t d = pod_eps[i].dur_ms > 0 ? pod_eps[i].dur_ms : 0;
        total_ms += d;
        if (pod_eps[i].resume_ms != POD_FINISHED) {
            int64_t at = pod_eps[i].resume_ms > 0 ? pod_eps[i].resume_ms : 0;
            if (d > at) left_ms += d - at;
        }
    }
    char l1[64], l2[96], t1[24], t2[24];
    snprintf(l1, sizeof(l1), "%d episode%s  \xc2\xb7  %d downloaded",
             pod_ep_n, pod_ep_n == 1 ? "" : "s", down);
    fmt_hm(t1, sizeof(t1), total_ms);
    fmt_hm(t2, sizeof(t2), left_ms);
    if (down) snprintf(l2, sizeof(l2), "%s on the card  \xc2\xb7  %s to hear", t1, t2);
    else      snprintf(l2, sizeof(l2), "Nothing downloaded yet");
    lv_obj_t *a = ui_label_w(l, l1, uf_body, dim, FB_W - 48);
    lv_obj_set_pos(a, 24, tracks_hdr_artist_y());
    lv_obj_t *b = ui_label_w(l, l2, uf_small, dim, FB_W - 48);
    lv_obj_set_pos(b, 24, tracks_hdr_info_y());
    int header_h = tracks_hdr_h();
    ui_hline(l, header_h - 1, line);

    char buf[32];
    ui_rows_alloc(track_n);
    for (int i = 0; i < track_n; i++) {
        lv_obj_t *row = ui_row(l, header_h + i * ROW_H, ROW_H, ui_episode_tap, i);
        lv_obj_set_style_border_color(row, c16(line), 0);
        lv_obj_add_event_cb(row, ui_track_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        if (pod_eps[i].downloaded) ui_row_swipeable(row, i, pod_remove_download);
        ui_rowv[i] = row;
        lv_obj_t *n = ui_label_w(row, tracks[i].name, uf_body, text, FB_W - 110 - 24);
        lv_obj_set_pos(n, 24, 20);
        ui_episode_right(i, buf, sizeof(buf));
        ui_right(row, buf, dim, 22);
    }
    ui_note_make(scr);
    ui.refresh = ui_podcast_page_refresh;
}

/* ---- an audiobook's chapters ---------------------------------------------- */

static void ui_chapter_tap(lv_event_t *e) {
    play_request(RECQ_CHAPTER, ui_idx(e));
    UI_ACTED();
}

static void ui_chapters_refresh(void) {
    for (int i = 0; i < ui_rown && i < track_n; i++) {
        lv_obj_t *row = ui_rowv[i];
        if (!row) continue;
        int playing = audio_is_active() && i == cur_track &&
                      !strcmp(cur_album, q_album) && !strcmp(cur_artist, q_artist);
        ui_row_playing(row, lv_obj_get_child(row, 1), playing, COL_TEXT, COL_ACCENT, COL_ROW);
    }
}

static void ui_build_chapters(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    char buf[32];
    ui_rows_alloc(track_n);
    for (int i = 0; i < track_n; i++) {
        lib_track_t *t = &tracks[i];
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_chapter_tap, i);
        ui_rowv[i] = row;
        if (t->track > 0) snprintf(buf, sizeof(buf), "%d", t->track);
        else buf[0] = '\0';
        lv_obj_t *num = ui_label(row, buf, uf_small, COL_DIM);
        lv_obj_set_pos(num, 20, 22);
        lv_obj_t *n = ui_label_w(row, t->name, uf_body, COL_TEXT, FB_W - 110 - 68);
        lv_obj_set_pos(n, 68, 20);
        if (t->dur_ms > 0) { fmt_dur(buf, sizeof(buf), t->dur_ms); ui_right(row, buf, COL_DIM, 22); }
    }
    ui.refresh = ui_chapters_refresh;
}

/* ---- playlists, radio, recordings, audiobooks, podcasts ------------------ */

static void ui_playlist_tap(lv_event_t *e) {
    int row = ui_idx(e);
    if (row == 0) {
        kb_new_playlist_add_track = -1;
        kb_open("Playlist name", KB_PURPOSE_NEW_PLAYLIST_NAME, "");
        UI_ACTED();
        return;
    }
    int pi = row - 1;
    if (pi < 0 || pi >= playlist_n) return;
    pl_path_t *paths = pl_scratch_get("open playlist");
    int got = 0;
    track_n = 0;
    if (paths) {
        got = pl_read(playlists[pi].path, paths, PL_SCRATCH_N);
        for (int k = 0; k < got; k++)
            if (lib_track_by_path(paths[k], &tracks[track_n]) == 0) track_n++;
        free(paths);
    }
    snprintf(cur_album, sizeof(cur_album), "%s", playlists[pi].name);
    cur_artist[0] = '\0';
    view_art_clear();
    browsing_is_playlist = 1;
    tracks_from_artist_page = 0;
    screen = SC_TRACKS; reset_scroll();
    ab_list = 0;
    pod_list = 0;
    mlog("[music] playlist %s: %d of %d found\n", playlists[pi].name, track_n, got);
    UI_ACTED();
}

static void ui_playlist_hold(lv_event_t *e);   /* ui_overlays.c */

static void ui_build_playlists(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    for (int i = 0; i <= playlist_n; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_playlist_tap, i);
        lv_obj_t *t = ui_label_w(row, i == 0 ? "New Playlist" : playlists[i - 1].name,
                                 uf_body, i == 0 ? COL_ACCENT : COL_TEXT, FB_W - 64);
        lv_obj_set_pos(t, 24, 20);
        if (i > 0) lv_obj_add_event_cb(row, ui_playlist_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(i - 1));
    }
}

static lv_obj_t *radio_msg_label;

static void ui_radio_tap(lv_event_t *e) {
    int r = ui_idx(e);
    if (r == station_n) {
        radio_recording_n = radio_recordings_load(radio_recordings, RADIO_REC_MAX);
        screen = SC_RADIO_RECORDINGS; reset_scroll();
    } else if (r >= 0 && r < station_n) {
        if (play_station(r)) screen = SC_PLAYING;
        else if (!radio_msg[0])
            snprintf(radio_msg, sizeof(radio_msg), "Could not reach that station");
    }
    UI_ACTED();
}

static void ui_radio_refresh(void) {
    for (int i = 0; i < ui_rown && i < station_n; i++) {
        lv_obj_t *row = ui_rowv[i];
        int playing = radio_mode && audio_is_active() && !strcmp(stations[i].name, radio_name);
        int have = stations[i].url[0] != '\0';
        ui_row_playing(row, lv_obj_get_child(row, 0), playing, have ? COL_TEXT : COL_DIM,
                       COL_ACCENT, COL_ROW);
    }
    const char *msg = !st_net_up() ? "Wi-Fi is off" : radio_msg;
    if (msg[0]) {
        lv_obj_remove_flag(radio_msg_label, LV_OBJ_FLAG_HIDDEN);
        ui_set_text(radio_msg_label, msg);
        lv_obj_set_y(radio_msg_label, FB_H - 34 - (mini_visible() ? MINI_H : 0));
    } else {
        lv_obj_add_flag(radio_msg_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_build_radio(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    ui_rows_alloc(station_n);
    for (int i = 0; i <= station_n; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_radio_tap, i);
        lv_obj_t *t = ui_label_w(row, i == station_n ? "Recordings" : stations[i].name, uf_body,
                                 i == station_n ? COL_ACCENT : COL_TEXT, FB_W - 64);
        lv_obj_set_pos(t, 24, 20);
        if (i < station_n) ui_rowv[i] = row;
    }
    if (station_n == 0) ui_empty_note(l, 0, "No stations configured");
    radio_msg_label = ui_label_w(scr, "", uf_small, RGB(230, 80, 70), FB_W - 48);
    lv_obj_set_x(radio_msg_label, 24);
    ui.refresh = ui_radio_refresh;
}

static void ui_recording_tap(lv_event_t *e) {
    if (!ui_tap_ok()) return;
    int r = ui_idx(e);
    if (r >= 0 && r < radio_recording_n && play_recording(r)) screen = SC_PLAYING;
    UI_ACTED();
}

static void ui_build_recordings(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    char buf[32];
    for (int i = 0; i < radio_recording_n; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_recording_tap, i);
        ui_row_swipeable(row, i, radio_recording_delete);
        lv_obj_t *t = ui_label_w(row, radio_recordings[i].name, uf_body, COL_TEXT, FB_W - 64);
        lv_obj_set_pos(t, 24, 8);
        long mb10 = radio_recordings[i].size_bytes / (1024L * 1024L / 10);
        snprintf(buf, sizeof(buf), "%ld.%ld MB", mb10 / 10, mb10 % 10);
        lv_obj_t *s = ui_label(row, buf, uf_small, COL_DIM);
        lv_obj_set_pos(s, 24, 42);
    }
    if (radio_recording_n == 0) ui_empty_note(l, 0, "No recordings yet");
}

static void ui_book_tap(lv_event_t *e) {
    int i = ui_idx(e);
    if (i < 0 || i >= ab_book_n) return;
    ab_book_t *b = &ab_books[i];
    if (audiobook_mode && !strcmp(b->dir, ab_book.dir)) {
        screen = SC_PLAYING;
    } else {
        ab_save_current_pos();
        ab_load_book(b);
        audiobook_mode = 1;
        audio_set_speed(ab_speed_permille);
        ab_playing[0] = '\0';
        queue_n = 0;
        screen = SC_PLAYING;
        ab_resume_book();
        mlog("[music] %s -> %d chapters in %d file(s)\n", cur_album, ab_book.chap_n, ab_book.file_n);
    }
    UI_ACTED();
}

static void ui_build_audiobooks(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    char buf[16];
    for (int i = 0; i < ab_book_n; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_book_tap, i);
        lv_obj_t *t = ui_label_w(row, ab_rows[i].name, uf_body, COL_TEXT, FB_W - 110 - 24);
        lv_obj_set_pos(t, 24, 20);
        if (ab_rows[i].count > 0) {
            snprintf(buf, sizeof(buf), "%d", ab_rows[i].count);
            ui_right(row, buf, COL_DIM, 22);
        }
    }
    if (ab_book_n == 0) ui_empty_note(l, 0, "No audiobooks found");
}

static lv_obj_t *pod_sync_bar, *pod_sync_lbl;

static void ui_podcast_feed_tap(lv_event_t *e) {
    int i = ui_idx(e);
    if (i < 0 || i >= pod_feed_n) return;
    snprintf(cur_feed, sizeof(cur_feed), "%s", pod_feeds[i].name);
    pod_ep_n = pod_load_episodes(cur_feed, pod_eps, POD_MAX_ITEMS);
    pod_rebuild_tracks();
    snprintf(cur_album, sizeof(cur_album), "%s", cur_feed);
    cur_artist[0] = '\0';
    browsing_is_playlist = 0;
    screen = SC_TRACKS; reset_scroll();
    ab_list = 0;
    pod_list = 1;
    mlog("[music] %s -> %d episodes\n", cur_feed, pod_ep_n);
    UI_ACTED();
}

static void ui_pod_sync_tap(lv_event_t *e) {
    (void)e;
    if (pod_update_running()) {
        screen = SC_POD_SYNC; reset_scroll();
    } else {
        pod_sync_log_n = 0;
        if (net_held()) pod_sync_deferred = 1;
        else            pod_update_start();
    }
    UI_ACTED();
}

static void ui_podcasts_refresh(void) {
    char buf[16];
    for (int i = 0; i < ui_rown && i < pod_feed_n; i++) {
        lv_obj_t *row = ui_rowv[i];
        lv_obj_t *icon = lv_obj_get_child(row, 2), *got_l = lv_obj_get_child(row, 3);
        int got = 0, st = pod_sync_status(pod_rows[i].name, &got);
        if (st == POD_SYNC_NONE) {
            lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(got_l, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        static const icon_t *const spin[6] = {
            &icon_sync_spin0, &icon_sync_spin1, &icon_sync_spin2,
            &icon_sync_spin3, &icon_sync_spin4, &icon_sync_spin5 };
        const icon_t *ic = st == POD_SYNC_BUSY ? spin[(g_tick / 2) % 6]
                         : st == POD_SYNC_OK ? &icon_sync_ok : &icon_sync_fail;
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
        ui_icon_set(icon, ic, st == POD_SYNC_FAIL ? RGB(230, 80, 70) : COL_ACCENT);
        if (st == POD_SYNC_OK) {
            snprintf(buf, sizeof(buf), "%d", got);
            ui_set_text(got_l, buf);
            lv_obj_remove_flag(got_l, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(got_l, LV_OBJ_FLAG_HIDDEN);
        }
    }
    int running = pod_update_running();
    ui_bg(pod_sync_bar, running ? COL_ACCENT : COL_HEADER);
    ui_set_text(pod_sync_lbl, running ? "Sync in progress"
                              : pod_sync_deferred ? "Sync waits for you to stop" : "Sync");
    ui_set_color(pod_sync_lbl, running ? COL_BG : pod_sync_deferred ? COL_DIM : COL_ACCENT);
    lv_obj_set_y(pod_sync_bar, pod_sync_bar_y());
    /* Podcasts' list ends at its own bar, not the mini player. */
    lv_obj_set_style_pad_bottom(ui.list, POD_SYNC_BAR_H + (mini_visible() ? MINI_H : 0), 0);
}

static void ui_build_podcasts(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    char buf[16];
    ui_rows_alloc(pod_feed_n);
    for (int i = 0; i < pod_feed_n; i++) {
        lv_obj_t *row = ui_row(l, i * ROW_H, ROW_H, ui_podcast_feed_tap, i);
        ui_rowv[i] = row;
        lv_obj_t *t = ui_label_w(row, pod_rows[i].name, uf_body, COL_TEXT, FB_W - 150 - 24);
        lv_obj_set_pos(t, 24, 20);
        buf[0] = '\0';
        if (pod_rows[i].count > 0) snprintf(buf, sizeof(buf), "%d", pod_rows[i].count);
        ui_right(row, buf, COL_DIM, 22);
        lv_obj_t *ic = ui_icon_img(row, &icon_sync_ok, COL_ACCENT);
        lv_obj_set_pos(ic, FB_W - 24 - 30, (ROW_H - 30) / 2);
        lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *got = ui_label(row, "", uf_body, COL_TEXT);
        lv_obj_align(got, LV_ALIGN_TOP_RIGHT, -24 - 30 - 10, 20);
        lv_obj_add_flag(got, LV_OBJ_FLAG_HIDDEN);
    }
    if (pod_feed_n == 0) ui_empty_note(l, 0, "No podcasts yet");
    pod_sync_bar = ui_plain(scr);
    lv_obj_set_size(pod_sync_bar, FB_W, POD_SYNC_BAR_H);
    lv_obj_set_style_border_side(pod_sync_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(pod_sync_bar, 1, 0);
    lv_obj_set_style_border_color(pod_sync_bar, c16(COL_LINE), 0);
    lv_obj_add_flag(pod_sync_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pod_sync_bar, ui_pod_sync_tap, LV_EVENT_CLICKED, NULL);
    pod_sync_lbl = ui_label(pod_sync_bar, "Sync", uf_body, COL_ACCENT);
    lv_obj_align(pod_sync_lbl, LV_ALIGN_CENTER, 0, -2);
    ui.refresh = ui_podcasts_refresh;
}

static lv_obj_t *pod_log_l, *pod_log_head;

static void ui_pod_sync_refresh(void) {
    ui_set_text(pod_log_head, pod_sync_deferred ? "Waits until you stop listening"
                             : pod_update_running() ? "Syncing every feed..."
                             : pod_update_died()    ? "Sync stopped early."
                                                    : "Sync finished.");
    ui_set_color(pod_log_head, pod_update_died() ? RGB(230, 80, 70) : COL_DIM);
    static char all[POD_SYNC_LOG_N * (POD_NAME_LEN + 1) + 16];
    size_t at = 0;
    all[0] = '\0';
    for (int i = 0; i < pod_sync_log_n && at < sizeof(all) - 2; i++)
        at += (size_t)snprintf(all + at, sizeof(all) - at, "%s\n", pod_sync_log[i]);
    if (pod_sync_log_n == 0 && !pod_sync_deferred) snprintf(all, sizeof(all), "Starting...");
    ui_set_text(pod_log_l, all);
}

static void ui_build_pod_sync(void) {
    lv_obj_t *scr = ui.scr = ui_new_screen(COL_BG);
    lv_obj_t *l = ui_list(scr, CONTENT_Y);
    pod_log_head = ui_label_w(l, "", uf_body, COL_DIM, FB_W - 48);
    lv_obj_set_pos(pod_log_head, 24, 6);
    ui_hline(l, 45, COL_LINE);
    pod_log_l = ui_label(l, "", uf_small, COL_TEXT);
    lv_label_set_long_mode(pod_log_l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(pod_log_l, FB_W - 48);
    lv_obj_set_style_text_line_space(pod_log_l, 40 - TEXT_PX_SMALL - 6, 0);
    lv_obj_set_pos(pod_log_l, 24, 60);
    ui.refresh = ui_pod_sync_refresh;
}
