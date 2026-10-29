/* ui_player.c — Now Playing (music, podcast, audiobook, radio, a radio
 * recording) and the queue. Part of ui_libra.c's translation unit; the
 * layout numbers are Libra's own (title_y(), bar_y(), CTRL_NUDGE_PX...) so
 * this sits exactly where the framebuffer version did. */

static struct {
    lv_obj_t *art, *notes, *title, *artist, *album, *count;
    lv_obj_t *bar_bg, *bar_fill, *knob, *wave, *clock_l, *clock_r;
    lv_obj_t *play_bg, *play, *mode_ring, *mode_icon, *queue_ring;
    lv_obj_t *speed, *book_bar, *book_fill, *book_left, *chap_left, *rec_dot, *rec_ring;
    int art_seq, wave_played, scrub_dur;
    int wave_top, max_h, wave_cy, clock_y;
    uintptr_t radio_art;
} np;

/* Radio art has no sequence number; a new bitmap is a new pointer. */
static ui_art_t ui_radio_art = { .seq = -1 };
static void ui_radio_art_sync(void) {
    pthread_mutex_lock(&radio_art_lock);
    uintptr_t p = (uintptr_t)radio_art_bits;
    pthread_mutex_unlock(&radio_art_lock);
    ui_art_take(&ui_radio_art, &radio_art_lock, &radio_art_bits, (int)(p & 0x7FFFFFFF));
}

/* ---- the seek bar: tap or drag to a position ---------------------------- */

/* Duration and position as Now Playing shows them (BG80: an outstanding seek
 * wins over a stale position). */
static void np_pos_dur(int *pos, int *dur) {
    lib_track_t *t = (cur_track >= 0 && cur_track < queue_n) ? &queue[cur_track] : NULL;
    int pending = audio_seek_pending_ms();
    *pos = pending >= 0 ? pending : audio_pos_ms();
    *dur = audio_dur_ms();
    if (*dur <= 0 && t) *dur = t->dur_ms;
}

static int np_bar_px(void) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int px = p.x - 24, span = FB_W - 48;
    if (px < 0) px = 0;
    if (px > span) px = span;
    return px;
}

static void np_bar_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    int span = FB_W - 48;
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        int pos, dur;
        np_pos_dur(&pos, &dur);
        np.scrub_dur = dur;
        scrub_active = 1;
        live_x = np_bar_px() + 24;          /* scrub_ms() reads the finger from here */
        UI_ACTED();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        scrub_active = 0;
        int px = np_bar_px();
        if (audiobook_mode) {
            const ab_chapter_t *ch = (cur_track >= 0 && cur_track < ab_book.chap_n)
                                   ? &ab_book.chap[cur_track] : NULL;
            int dur = ch ? (int)ch->dur_ms : 0;
            if (dur <= 0) dur = audio_dur_ms();
            if (dur > 0)
                audio_seek_ms((int)((ch ? ch->file_start_ms : 0) + (int64_t)dur * px / span));
        } else if (np.scrub_dur > 0) {
            audio_seek_ms((int)((int64_t)np.scrub_dur * px / span));
        }
        UI_ACTED();
    }
}

/* The waveform: Libra's cached loudness columns, played part in the accent. */
static void np_wave_draw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER;
    lv_color_t acc = c16(np_col_accent()), line = c16(np_col_line());
    int wave_w = FB_W - 48;
    for (int c = 0; c < WAVE_BUCKETS; c++) {
        int h = np.max_h * wave_buckets[c] / 255;
        if (h < 2) h = 2;
        int cx0 = a.x1 + c * wave_w / WAVE_BUCKETS;
        int cx1 = a.x1 + (c + 1) * wave_w / WAVE_BUCKETS;
        int w = cx1 - cx0 > 1 ? cx1 - cx0 - 1 : 1;
        int cy = (a.y1 + a.y2) / 2;
        lv_area_t col = { cx0, cy - h / 2, cx0 + w - 1, cy - h / 2 + h - 1 };
        r.bg_color = c <= np.wave_played ? acc : line;
        lv_draw_rect(layer, &r, &col);
    }
}

/* ---- transport taps ------------------------------------------------------- */

enum { NP_PLAY, NP_PREV, NP_NEXT, NP_MODE, NP_QUEUE, NP_BACK10, NP_FWD10, NP_FWD30,
       NP_SPEED, NP_NOTES, NP_RECORD };

static void np_tap(lv_event_t *e) {
    switch (ui_idx(e)) {
    case NP_PLAY: audio_toggle(); break;
    case NP_PREV: play_index(prev_track_index()); break;
    case NP_NEXT: play_index(next_track_index()); break;
    case NP_MODE:
        if (shuffle_enabled) { shuffle_enabled = 0; repeat_mode = REPEAT_ONE; }
        else if (repeat_mode == REPEAT_ONE) repeat_mode = REPEAT_OFF;
        else shuffle_enabled = 1;
        if (shuffle_enabled) shuffle_regenerate();
        queue_follower();
        save_conf();
        break;
    case NP_QUEUE:
        qd_begin_open(0);
        qd_finish_open();
        break;
    case NP_BACK10:
        if (radio_mode) {
            if (audio_radio_offset_ms() < audio_radio_max_rewind_ms())
                audio_radio_seek_relative_ms(-10000);
        } else {
            audio_seek_ms(audio_pos_ms() - 10000);
        }
        break;
    case NP_FWD10:
        if (radio_mode) {
            if (audio_radio_offset_ms() > 0) audio_radio_seek_relative_ms(+10000);
        } else {
            audio_seek_ms(audio_pos_ms() + 10000);
        }
        break;
    case NP_FWD30: audio_seek_ms(audio_pos_ms() + 30000); break;
    case NP_SPEED:
        if (audiobook_mode) {
            ab_speed_permille += 100;
            if (ab_speed_permille > 2000) ab_speed_permille = 1000;
            audio_set_speed(ab_speed_permille);
        } else {
            pod_speed_permille += 100;
            if (pod_speed_permille > 2000) pod_speed_permille = 1000;
            audio_set_speed(pod_speed_permille);
            pod_speed_store(cur_feed, pod_speed_permille);
        }
        save_conf();
        break;
    case NP_NOTES:
        pod_notes_showing = !pod_notes_showing;
        pod_notes_scroll_px = 0;
        break;
    case NP_RECORD:
        if (rb_is_recording()) {
            rb_recording_stop();
        } else {
            const char *ext = rb_current_kind() == RB_KIND_ADTS ? "aac" : "mp3";
            radio_recording_new_path(radio_name, ext, radio_recording_path_buf,
                                     sizeof(radio_recording_path_buf));
            if (rb_recording_start(radio_recording_path_buf) != 0) {
                np_toast("Can't record");
                mlog("[music] recording could not start: %s\n", radio_recording_path_buf);
            }
        }
        break;
    }
    UI_ACTED();
}

/* A round target of radius r centred at (cx, cy): drawn as a ring, a disc,
 * or nothing (just the touch area). */
static lv_obj_t *np_button(lv_obj_t *scr, int cx, int cy, int r, int action) {
    lv_obj_t *b = ui_plain(scr);
    lv_obj_set_size(b, 2 * r, 2 * r);
    lv_obj_set_pos(b, cx - r, cy - r);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, 14);
    lv_obj_add_event_cb(b, np_tap, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    return b;
}

/* R59: holding a skip button repeats it, the jump growing the longer it is
 * held (skip_hold_amount()), every SKIP_REPEAT_MS. Books and podcasts. A
 * held button's release is not also a tap: the buttons act on
 * SHORT_CLICKED, which LVGL does not send after a long press. */
static struct { int dir; uint64_t t0; long next_ms; } sk;

static void np_skip_hold(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    int action = ui_idx(e);
    if (!audiobook_mode && !podcast_mode) return;
    if (code == LV_EVENT_PRESSED) {
        sk.dir = 0;
        sk.t0 = us_now();
    } else if (code == LV_EVENT_LONG_PRESSED || code == LV_EVENT_PRESSING) {
        long held = (long)((us_now() - sk.t0) / 1000);
        if (held < HOLD_MS) return;
        if (sk.dir == 0) {
            sk.dir = action == NP_BACK10 ? -1 : +1;
            sk.next_ms = 0;
        }
        if (held >= sk.next_ms) {
            int amt = skip_hold_amount(held);
            audio_seek_ms(audio_pos_ms() + sk.dir * amt);
            seek_toast(sk.dir * amt);
            sk.next_ms = held + SKIP_REPEAT_MS;
            UI_ACTED();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        sk.dir = 0;
    }
}

/* -10s/+10s/+30s: the curved-arrow icon with the seconds inside. */
static lv_obj_t *np_skip(lv_obj_t *scr, int cx, int cy, int back, const char *secs,
                         uint16_t col, int action) {
    lv_obj_t *b = np_button(scr, cx, cy, 34, action);
    if (audiobook_mode || podcast_mode) {
        /* Tap on a short click only, so a hold's release does not skip again. */
        lv_obj_remove_event_cb(b, np_tap);
        lv_obj_add_event_cb(b, np_tap, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)action);
        lv_obj_add_event_cb(b, np_skip_hold, LV_EVENT_ALL, (void *)(intptr_t)action);
    }
    lv_obj_t *ic = ui_icon_img(b, back ? &icon_skip_back : &icon_skip_forward, col);
    lv_obj_center(ic);
    lv_obj_t *n = ui_label(b, secs, uf_small, col);
    lv_obj_align(n, LV_ALIGN_CENTER, 0, 2);
    return b;
}

/* A ring control: the mode, queue, speed and info buttons. */
static lv_obj_t *np_ring(lv_obj_t *scr, int cx, int cy, int r, uint16_t ring_col, int action) {
    lv_obj_t *b = np_button(scr, cx, cy, r, action);
    lv_obj_set_style_border_width(b, r == 26 ? 1 : 2, 0);
    lv_obj_set_style_border_color(b, c16(ring_col), 0);
    return b;
}

/* ---- the screen ----------------------------------------------------------- */

static void np_refresh(void);

static void np_build_common_art(lv_obj_t *scr, uint16_t placeholder, uint16_t bg) {
    (void)bg;
    np.art = ui_art_box(scr, 0, 0, ART_PX, placeholder);
    np.art_seq = -2;
}

static void np_build_bar(lv_obj_t *scr, int by, int bh) {
    int top = by - 26, h = 52;
    lv_obj_t *zone = ui_plain(scr);           /* the generous touch band (52px) */
    lv_obj_set_pos(zone, 0, top);
    lv_obj_set_size(zone, FB_W, h);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_USER_1);    /* owns its drags: never a back/home swipe */
    lv_obj_add_event_cb(zone, np_bar_event, LV_EVENT_ALL, NULL);
    np.bar_bg = ui_plain(scr);
    lv_obj_set_pos(np.bar_bg, 24, by - bh / 2);
    lv_obj_set_size(np.bar_bg, FB_W - 48, bh);
    np.bar_fill = ui_plain(scr);
    lv_obj_set_pos(np.bar_fill, 24, by - bh / 2);
    lv_obj_set_size(np.bar_fill, 0, bh);
    np.knob = ui_circle(scr, 24, by, 13, COL_ACCENT, 0);
    lv_obj_add_flag(np.knob, LV_OBJ_FLAG_HIDDEN);
}

static void ui_build_playing(void) {
    memset(&np, 0, sizeof(np));
    np.art_seq = -2;
    np.wave_played = -1;
    if (cover_palette_enabled && !audiobook_mode && !radio_mode && !recording_playback_mode)
        compute_cover_palette();
    uint16_t bg = audiobook_mode ? COL_BG : np_col_bg();
    uint16_t fg = audiobook_mode ? COL_TEXT : np_col_fg();
    uint16_t dim = audiobook_mode ? COL_DIM : np_col_dim();
    uint16_t acc = audiobook_mode ? COL_ACCENT : np_col_accent();
    lv_obj_t *scr = ui.scr = ui_new_screen(bg);
    int mid = FB_W / 2;
    int ty = title_y();

    if (podcast_mode && pod_notes_showing) {
        np.notes = ui_plain(scr);
        lv_obj_set_size(np.notes, ART_PX, ART_PX);
        ui_bg(np.notes, COL_ROW);
        lv_obj_add_flag(np.notes, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(np.notes, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scroll_dir(np.notes, LV_DIR_VER);
        lv_obj_set_style_pad_all(np.notes, 16, 0);
        lv_obj_t *txt = ui_label(np.notes, pod_notes_text, (lv_font_t *)uf_small, COL_TEXT);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(txt, ART_PX - 32);
    } else {
        np_build_common_art(scr, recording_playback_mode || radio_mode ? COL_ROW : COL_ROW, bg);
    }

    /* A long title is dragged sideways under the finger, as Libra's: a
     * strip one line high that scrolls horizontally, holding the title at
     * its full width. */
    lv_obj_t *tb = ui_plain(scr);
    lv_obj_set_pos(tb, 24, ty);
    lv_obj_set_size(tb, FB_W - 48, lv_font_get_line_height(uf_title));
    lv_obj_add_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tb, LV_OBJ_FLAG_USER_1);      /* owns its drags (Libra's title_dragging) */
    lv_obj_set_scroll_dir(tb, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tb, LV_SCROLLBAR_MODE_OFF);
    np.title = ui_label(tb, "", uf_title, fg);
    np.artist = ui_label_w(scr, "", uf_body, dim, FB_W - 48);
    lv_obj_set_pos(np.artist, 24, ty + 44);

    if (recording_playback_mode) {
        int cyy = bar_y() + 70 + CTRL_NUDGE_PX;
        np.play_bg = np_button(scr, mid, cyy, 42, NP_PLAY);
        ui_bg(np.play_bg, COL_ACCENT);
        np.play = ui_glyph(np.play_bg, G_PAUSE, 36, COL_BG);
        lv_obj_center(np.play);
        ui.refresh = np_refresh;
        return;
    }

    if (radio_mode) {
        np.count = ui_label(scr, "", uf_small, dim);
        int cyy = bar_y() + 70 + CTRL_NUDGE_PX;
        np_skip(scr, mid - 96, cyy, 1, "10s", fg, NP_BACK10);
        np_skip(scr, mid + 96, cyy, 0, "10s", fg, NP_FWD10);
        np.play_bg = np_button(scr, mid, cyy, 42, NP_PLAY);
        ui_bg(np.play_bg, acc);
        np.play = ui_glyph(np.play_bg, G_PAUSE, 36, bg);
        lv_obj_center(np.play);
        np.rec_ring = np_ring(scr, mid + 96 + 70, cyy, 22, dim, NP_RECORD);
        np.rec_dot = ui_circle(np.rec_ring, 20, 20, 10, fg, 0);
        lv_obj_set_pos(np.rec_dot, 22 - 10 - 2, 22 - 10 - 2);
        ui.refresh = np_refresh;
        return;
    }

    if (audiobook_mode) {
        np.count = ui_label(scr, "", uf_small, COL_DIM);
        int bby = ab_book_bar_y(), cby = ab_chapter_bar_y();
        lv_obj_t *bl = ui_label(scr, "Book", uf_small, COL_DIM);
        lv_obj_set_pos(bl, 24, bby - 26);
        np.book_left = ui_label(scr, "", uf_small, COL_DIM);
        np.book_bar = ui_plain(scr);
        lv_obj_set_pos(np.book_bar, 24, bby);
        lv_obj_set_size(np.book_bar, FB_W - 48, 4);
        ui_bg(np.book_bar, COL_LINE);
        np.book_fill = ui_plain(scr);
        lv_obj_set_pos(np.book_fill, 24, bby);
        lv_obj_set_size(np.book_fill, 0, 4);
        ui_bg(np.book_fill, COL_DIM);
        lv_obj_t *cl = ui_label(scr, "Chapter", uf_small, COL_DIM);
        lv_obj_set_pos(cl, 24, cby - 26);
        np.chap_left = ui_label(scr, "", uf_small, COL_DIM);
        np_build_bar(scr, cby + 3, 6);
        int cyy = cby + 58 + CTRL_NUDGE_PX;
        np_skip(scr, mid - 96, cyy, 1, "10s", COL_TEXT, NP_BACK10);
        np_skip(scr, mid + 96, cyy, 0, "10s", COL_TEXT, NP_FWD10);
        np.play_bg = np_button(scr, mid, cyy, 42, NP_PLAY);
        ui_bg(np.play_bg, COL_ACCENT);
        np.play = ui_glyph(np.play_bg, G_PAUSE, 36, COL_BG);
        lv_obj_center(np.play);
        lv_obj_t *sp = np_ring(scr, mid - 96 - 70, cyy, 22, COL_LINE, NP_SPEED);
        np.speed = ui_label(sp, "", uf_small, COL_TEXT);
        lv_obj_center(np.speed);
        lv_obj_t *q = np_button(scr, FB_W - 24 - 13, FB_H - 32 + 9, 24, NP_QUEUE);
        lv_obj_t *qi = ui_glyph(q, G_QUEUE, 26, COL_DIM);
        lv_obj_center(qi);
        ui.refresh = np_refresh;
        return;
    }

    /* Music and podcasts. */
    np.album = ui_label_w(scr, "", uf_small, dim, FB_W - 110 - 24);
    lv_obj_set_pos(np.album, 24, ty + 82);
    np.count = ui_label(scr, "", uf_small, dim);
    int by = bar_y();
    np.wave_top = ty + 82 + TEXT_PX_SMALL + 12;
    int wave_bot = (by + 70 + CTRL_NUDGE_PX) - 42 - 10;
    np.clock_y = wave_bot - 6;
    np.max_h = (np.clock_y - 10) - np.wave_top;
    if (np.max_h < 16) np.max_h = 16;
    np.wave_cy = np.wave_top + np.max_h / 2;
    np.wave = ui_plain(scr);
    lv_obj_set_pos(np.wave, 24, np.wave_cy - np.max_h / 2);
    lv_obj_set_size(np.wave, FB_W - 48, np.max_h);
    lv_obj_add_event_cb(np.wave, np_wave_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(np.wave, LV_OBJ_FLAG_HIDDEN);
    np_build_bar(scr, np.wave_cy, 10);
    np.clock_l = ui_label(scr, "", uf_small, dim);
    lv_obj_set_pos(np.clock_l, 24, np.clock_y);
    np.clock_r = ui_label(scr, "", uf_small, dim);

    int cyy = by + 70 + CTRL_NUDGE_PX;
    if (podcast_mode) {
        np_skip(scr, mid - POD_SKIP_OFF, cyy, 1, "10s", fg, NP_BACK10);
        np_skip(scr, mid + POD_SKIP_OFF, cyy, 0, "30s", fg, NP_FWD30);
        lv_obj_t *sp = np_ring(scr, pod_speed_x(mid), cyy, 22, COL_LINE, NP_SPEED);
        np.speed = ui_label(sp, "", uf_small, fg);
        lv_obj_center(np.speed);
        if (pod_notes_avail) {
            lv_obj_t *info = np_ring(scr, pod_info_x(mid), cyy, 22, COL_LINE, NP_NOTES);
            lv_obj_t *i = ui_label(info, "i", uf_body, fg);
            lv_obj_center(i);
        }
        lv_obj_t *q = np_button(scr, FB_W - 24 - 13, FB_H - 32 + 9, 24, NP_QUEUE);
        lv_obj_t *qi = ui_glyph(q, G_QUEUE, 26, COL_DIM);
        lv_obj_center(qi);
    } else {
        lv_obj_t *prev = np_button(scr, mid - 96, cyy, 30, NP_PREV);
        lv_obj_t *g = ui_glyph(prev, G_PREV, 34, fg);
        lv_obj_center(g);
        lv_obj_t *next = np_button(scr, mid + 96, cyy, 30, NP_NEXT);
        g = ui_glyph(next, G_NEXT, 34, fg);
        lv_obj_center(g);
        np.mode_ring = np_ring(scr, mid - 96 - 82, cyy, 26, np_col_circle(), NP_MODE);
        np.mode_icon = ui_icon_img(np.mode_ring, &icon_mode_off, np_col_circle());
        lv_obj_center(np.mode_icon);
        np.queue_ring = np_ring(scr, mid + 96 + 82, cyy, 26, np_col_circle(), NP_QUEUE);
        lv_obj_t *qi = ui_glyph(np.queue_ring, G_QUEUE, 26, np_col_circle());
        lv_obj_center(qi);
    }
    np.play_bg = np_button(scr, mid, cyy, 42, NP_PLAY);
    ui_bg(np.play_bg, acc);
    np.play = ui_glyph(np.play_bg, G_PAUSE, 36, bg);
    lv_obj_center(np.play);
    ui.refresh = np_refresh;
}

static void np_fmt_clock(char *buf, size_t n, int ms, int neg) {
    if (ms < 0) ms = 0;
    snprintf(buf, n, "%s%d:%02d", neg ? "-" : "", ms / 60000, (ms / 1000) % 60);
}

static void np_refresh(void) {
    char buf[48];
    int mid = FB_W / 2;
    int ty = title_y();
    (void)mid;
    if (cover_palette_enabled && !audiobook_mode && !radio_mode && !recording_playback_mode) {
        compute_cover_palette();
        /* The palette is the page's background: a new cover means a new page. */
        static int pal_seq = -1;
        int s = ui_np_art.seq;
        if (pal_seq != s && np.art_seq != -2) { pal_seq = s; ui_rebuild_req = 1; return; }
        pal_seq = s;
    }
    uint16_t bg = audiobook_mode ? COL_BG : np_col_bg();

    if (np.play) ui_glyph_set(np.play, audio_is_paused() ? G_PLAY : G_PAUSE,
                              recording_playback_mode ? COL_BG : bg);

    if (recording_playback_mode) {
        ui_set_text(np.title, recording_playback_name);
        ui_set_text(np.artist, "Recording");
        return;
    }

    if (radio_mode) {
        ui_radio_art_sync();
        if (np.art && np.art_seq != ui_radio_art.seq) {
            np.art_seq = ui_radio_art.seq;
            ui_art_box_set(np.art, &ui_radio_art);
        }
        char art_title[160];
        pthread_mutex_lock(&radio_art_lock);
        snprintf(art_title, sizeof(art_title), "%s", radio_art_title);
        pthread_mutex_unlock(&radio_art_lock);
        ui_set_text(np.title, radio_name);
        ui_set_text(np.artist, art_title[0] ? art_title : "Internet radio");
        long behind_ms = audio_radio_offset_ms();
        if (behind_ms > 0) {
            snprintf(buf, sizeof(buf), "-%ld:%02ld", behind_ms / 60000, (behind_ms / 1000) % 60);
            ui_set_color(np.count, np_col_dim());
        } else {
            snprintf(buf, sizeof(buf), "%s", audio_is_active() ? "LIVE" : "stopped");
            ui_set_color(np.count, audio_is_active() ? np_col_accent() : np_col_dim());
        }
        ui_set_text(np.count, buf);
        lv_obj_align(np.count, LV_ALIGN_TOP_RIGHT, -24, ty + 82);
        int recording = rb_is_recording();
        int blink_on = !recording || ((g_tick / REC_BLINK_TICKS) & 1) == 0;
        lv_obj_set_style_border_color(np.rec_ring, c16(recording ? RGB(230, 80, 70) : np_col_dim()), 0);
        ui_bg(np.rec_dot, recording ? np_col_accent() : np_col_fg());
        if (blink_on) lv_obj_remove_flag(np.rec_dot, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(np.rec_dot, LV_OBJ_FLAG_HIDDEN);
        if (recording) ui_dirty = 1;   /* keep the blink going */
        return;
    }

    lib_track_t *t = (cur_track >= 0 && cur_track < queue_n) ? &queue[cur_track] : NULL;
    if (!t) return;
    if (np.art && np.art_seq != ui_np_art.seq) {
        np.art_seq = ui_np_art.seq;
        ui_art_box_set(np.art, &ui_np_art);
    }
    if (strcmp(lv_label_get_text(np.title), t->name)) {
        lv_label_set_text(np.title, t->name);
        lv_obj_scroll_to_x(lv_obj_get_parent(np.title), 0, LV_ANIM_OFF);   /* a new title starts at its start */
    }

    if (audiobook_mode) {
        ui_set_text(np.artist, q_album);
        snprintf(buf, sizeof(buf), "%d of %d", cur_track + 1, queue_n);
        ui_set_text(np.count, buf);
        lv_obj_align(np.count, LV_ALIGN_TOP_RIGHT, -24, ty + 44);
        const ab_chapter_t *ch = (cur_track >= 0 && cur_track < ab_book.chap_n)
                               ? &ab_book.chap[cur_track] : NULL;
        int64_t book_total = ab_book.total_ms;
        int chap_dur = ch ? (int)ch->dur_ms : 0;
        if (chap_dur <= 0) { chap_dur = audio_dur_ms(); if (book_total <= 0) book_total = chap_dur; }
        int64_t chap_start = ch ? ch->file_start_ms : 0;
        int pending = audio_seek_pending_ms();
        int64_t live_pos = pending >= 0 ? pending : audio_pos_ms();
        int chap_pos = scrub_active ? scrub_ms(chap_dur) : (int)(live_pos - chap_start);
        if (chap_pos < 0) chap_pos = 0;
        int64_t book_pos = (ch ? ch->book_start_ms : 0) + chap_pos;
        double speed = ab_speed_permille / 1000.0;
        buf[0] = '\0';
        if (book_total > 0) fmt_left(buf, sizeof(buf), (int64_t)((book_total - book_pos) / speed));
        ui_set_text(np.book_left, buf);
        lv_obj_align(np.book_left, LV_ALIGN_TOP_RIGHT, -24, ab_book_bar_y() - 26);
        int bw = book_total > 0 ? (int)((FB_W - 48) * book_pos / book_total) : 0;
        if (bw > FB_W - 48) bw = FB_W - 48;
        lv_obj_set_width(np.book_fill, bw);
        buf[0] = '\0';
        if (chap_dur > 0) fmt_left(buf, sizeof(buf), (int64_t)((chap_dur - chap_pos) / speed));
        ui_set_text(np.chap_left, buf);
        lv_obj_align(np.chap_left, LV_ALIGN_TOP_RIGHT, -24, ab_chapter_bar_y() - 26);
        ui_bg(np.bar_bg, COL_LINE);
        ui_bg(np.bar_fill, COL_ACCENT);
        int cw = chap_dur > 0 ? (int)((int64_t)(FB_W - 48) * chap_pos / chap_dur) : 0;
        if (cw > FB_W - 48) cw = FB_W - 48;
        lv_obj_set_width(np.bar_fill, cw);
        if (scrub_active) {
            lv_obj_remove_flag(np.knob, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_x(np.knob, 24 + cw - 13);
        } else {
            lv_obj_add_flag(np.knob, LV_OBJ_FLAG_HIDDEN);
        }
        snprintf(buf, sizeof(buf), "%.1f\xc3\x97", speed);
        ui_set_text(np.speed, buf);
        return;
    }

    if (podcast_mode) {
        ui_set_text(np.artist, cur_feed);
        ui_set_text(np.album, "");
        ui_set_text(np.count, "");
        snprintf(buf, sizeof(buf), "%.1f\xc3\x97", pod_speed_permille / 1000.0);
        ui_set_text(np.speed, buf);
    } else {
        ui_set_text(np.artist, t->artist[0] ? t->artist : q_artist);
        char albuf[LIB_NAME_LEN + 12];
        if (q_is_playlist) snprintf(albuf, sizeof(albuf), "Playlist: %s", q_album);
        else               snprintf(albuf, sizeof(albuf), "%s", q_album);
        ui_set_text(np.album, albuf);
        snprintf(buf, sizeof(buf), "%d of %d", cur_track + 1, queue_n);
        ui_set_text(np.count, buf);
        lv_obj_align(np.count, LV_ALIGN_TOP_RIGHT, -24, ty + 82);
        int pm_on = shuffle_enabled || repeat_mode != REPEAT_OFF;
        ui_icon_set(np.mode_icon, shuffle_enabled ? &icon_mode_shuffle
                                 : repeat_mode != REPEAT_OFF ? &icon_mode_repeat : &icon_mode_off,
                    pm_on ? np_col_accent() : np_col_circle());
    }

    int pos, dur;
    np_pos_dur(&pos, &dur);
    if (scrub_active) pos = scrub_ms(dur);
    int waved = wave_loaded && !strcmp(t->path, wave_path);
    int span = FB_W - 48;
    int w = dur > 0 ? (int)((int64_t)span * pos / dur) : 0;
    if (w > span) w = span;
    if (waved) {
        lv_obj_remove_flag(np.wave, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(np.bar_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(np.bar_fill, LV_OBJ_FLAG_HIDDEN);
        int played = dur > 0 ? (int)((int64_t)WAVE_BUCKETS * pos / dur) : 0;
        if (played != np.wave_played) { np.wave_played = played; lv_obj_invalidate(np.wave); }
    } else {
        lv_obj_add_flag(np.wave, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(np.bar_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(np.bar_fill, LV_OBJ_FLAG_HIDDEN);
        int bh = scrub_active ? 14 : 10;
        lv_obj_set_height(np.bar_bg, bh);
        lv_obj_set_height(np.bar_fill, bh);
        lv_obj_set_y(np.bar_bg, np.wave_cy - bh / 2);
        lv_obj_set_y(np.bar_fill, np.wave_cy - bh / 2);
        ui_bg(np.bar_bg, np_col_line());
        ui_bg(np.bar_fill, np_col_accent());
        lv_obj_set_width(np.bar_fill, w);
    }
    if (scrub_active) {
        lv_obj_remove_flag(np.knob, LV_OBJ_FLAG_HIDDEN);
        ui_bg(np.knob, np_col_accent());
        lv_obj_set_pos(np.knob, 24 + w - 13, np.wave_cy - 13);
    } else {
        lv_obj_add_flag(np.knob, LV_OBJ_FLAG_HIDDEN);
    }
    double spd = podcast_mode ? pod_speed_permille / 1000.0 : 1.0;
    np_fmt_clock(buf, sizeof(buf), (int)(pos / spd), 0);
    ui_set_text(np.clock_l, buf);
    int rem = dur - pos;
    np_fmt_clock(buf, sizeof(buf), (int)((rem < 0 ? 0 : rem) / spd), 1);
    ui_set_text(np.clock_r, buf);
    lv_obj_align(np.clock_r, LV_ALIGN_TOP_RIGHT, -24, np.clock_y);
}

/* ---- the queue ------------------------------------------------------------ */

static struct { lv_obj_t *summary, *clear; } q_ui;

static void ui_queue_remove(int d) { queue_remove_display(d); }

static void ui_queue_tap(lv_event_t *e) {
    if (!ui_tap_ok()) return;
    int display_i = ui_idx(e);
    int qidx = queue_display_index(display_i);
    if (qidx >= 0) play_request(RECQ_QUEUE, qidx);
    UI_ACTED();
}

static void ui_queue_clear(lv_event_t *e) {
    (void)e;
    queue_clear_rest();
    ui_rebuild_req = 1;
    UI_ACTED();
}

static void ui_queue_hold(lv_event_t *e);   /* ui_overlays.c */

static void ui_queue_refresh(void) {
    int qth = g_queue_theme;
    uint16_t q_text = qth ? np_col_fg() : COL_TEXT, q_acc = qth ? np_col_accent() : COL_ACCENT,
             q_row = qth ? np_blend(np_bg, np_fg, 0.12f) : COL_ROW,
             q_mute = qth ? np_blend(np_bg, np_fg, 0.45f) : COL_DIM,
             q_dim = qth ? np_col_dim() : COL_DIM;
    char cbuf[24], dbuf[16], line[48];
    snprintf(cbuf, sizeof(cbuf), "%d track%s", queue_n, queue_n == 1 ? "" : "s");
    int64_t remain_ms = 0;
    int from = (cur_track >= 0 && cur_track < queue_n) ? cur_track : 0;
    for (int i = from; i < queue_n; i++) remain_ms += queue[i].dur_ms;
    fmt_dur(dbuf, sizeof(dbuf), remain_ms);
    snprintf(line, sizeof(line), "%s \xc2\xb7 %s total", cbuf, dbuf);
    ui_set_text(q_ui.summary, line);
    ui_set_color(q_ui.summary, q_dim);
    ui_set_color(q_ui.clear, queue_has_more() ? q_acc : q_mute);
    for (int d = 0; d < ui_rown && d < queue_n; d++) {
        int idx = queue_display_index(d);
        lv_obj_t *row = ui_rowv[d];
        if (!row || idx < 0) continue;
        ui_row_playing(row, lv_obj_get_child(row, 0), audio_is_active() && idx == cur_track,
                       q_text, q_acc, q_row);
    }
}

static void ui_build_queue(void) {
    g_queue_theme = np_chrome_themed() && np_palette_valid;
    int qth = g_queue_theme;
    uint16_t q_text = qth ? np_col_fg() : COL_TEXT, q_dim = qth ? np_col_dim() : COL_DIM,
             q_line = qth ? np_col_line() : COL_LINE, q_bar = qth ? np_col_bg() : COL_HEADER,
             q_mute = qth ? np_blend(np_bg, np_fg, 0.45f) : COL_DIM;
    lv_obj_t *scr = ui.scr = ui_new_screen(qth ? np_col_bg() : COL_BG);
    lv_obj_t *bar = ui_plain(scr);
    lv_obj_set_pos(bar, 0, CONTENT_Y);
    lv_obj_set_size(bar, FB_W, QUEUE_BAR_H);
    ui_bg(bar, q_bar);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, c16(q_line), 0);
    lv_obj_t *title = ui_label(bar, "Queue", uf_title, q_text);
    lv_obj_set_pos(title, 24, 4);
    q_ui.summary = ui_label(bar, "", uf_small, q_dim);
    lv_obj_set_pos(q_ui.summary, 24, 46);
    q_ui.clear = ui_label(bar, "Clear", uf_body, q_mute);
    lv_obj_align(q_ui.clear, LV_ALIGN_RIGHT_MID, -24, -2);
    lv_obj_add_flag(q_ui.clear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(q_ui.clear, 24);
    lv_obj_add_event_cb(q_ui.clear, ui_queue_clear, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = ui_list(scr, CONTENT_Y + QUEUE_BAR_H);
    char buf[16];
    ui_rows_alloc(queue_n);
    for (int d = 0; d < queue_n; d++) {
        int idx = queue_display_index(d);
        if (idx < 0) break;
        lib_track_t *t = &queue[idx];
        lv_obj_t *row = ui_row(l, d * ROW_H, ROW_H, ui_queue_tap, d);
        lv_obj_set_style_border_color(row, c16(q_line), 0);
        lv_obj_add_event_cb(row, ui_queue_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)d);
        ui_row_swipeable(row, d, ui_queue_remove);
        ui_row_grip(row, d, queue_n, queue_move_display, q_mute);
        ui_rowv[d] = row;
        lv_obj_t *n = ui_label_w(row, t->name, uf_body, q_text, FB_W - 150 - 24);
        lv_obj_set_pos(n, 24, 20);
        if (t->dur_ms > 0) {
            fmt_dur(buf, sizeof(buf), t->dur_ms);
            lv_obj_t *dl = ui_label(row, buf, uf_small, q_dim);
            lv_obj_align(dl, LV_ALIGN_TOP_RIGHT, -24 - 40, 22);
        }
    }
    ui.refresh = ui_queue_refresh;
}
