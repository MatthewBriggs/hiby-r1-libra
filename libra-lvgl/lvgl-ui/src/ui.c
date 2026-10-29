/* ui.c — proof of life: Libra's Home screen rebuilt on LVGL.
 *
 * Same palette, font (Noto Sans) and icons as app/music_hook.c, so the two
 * can be compared side by side on the device. Nothing here talks to the
 * player yet: the status bar and mini player show fixed text, and every
 * tile opens the same long list, which is there to judge scrolling.
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>

#include "icons.h"

/* app/music_hook.c's dark palette and default accent. */
#define C_BG      lv_color_make(16, 16, 20)
#define C_HEADER  lv_color_make(26, 26, 33)
#define C_TEXT    lv_color_make(240, 240, 245)
#define C_DIM     lv_color_make(125, 125, 136)
#define C_ROW     lv_color_make(26, 26, 33)
#define C_LINE    lv_color_make(38, 38, 46)
#define C_ACCENT  lv_color_make(240, 138, 42)

#define STATUS_H  48
#define MINI_H    78

static lv_font_t *f_status, *f_tile, *f_row, *f_title;
static lv_obj_t *home;

/* An app/icons_data.h bitmap is 8-bit coverage, exactly LVGL's A8 format:
 * drawn in whatever colour the image's recolour style says. */
static lv_image_dsc_t *icon_dsc(const icon_t *ic) {
    lv_image_dsc_t *d = calloc(1, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_A8;
    d->header.w = ic->w;
    d->header.h = ic->h;
    d->header.stride = ic->w;
    d->data = ic->a;
    d->data_size = (uint32_t)(ic->w * ic->h);
    return d;
}

static lv_obj_t *icon_img(lv_obj_t *parent, const icon_t *ic, lv_color_t c) {
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, icon_dsc(ic));
    lv_obj_set_style_image_recolor(img, c, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, lv_font_t *font, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    return o;
}

static void status_bar(lv_obj_t *scr) {
    lv_obj_t *bar = plain(scr);
    lv_obj_set_size(bar, 480, STATUS_H);
    lv_obj_set_style_bg_color(bar, C_HEADER, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_t *vol = icon_img(bar, &icon_vol_low_lg, C_DIM);
    lv_obj_align(vol, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_t *v = label(bar, "5/16", f_status, C_DIM);
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 62, 0);
    lv_obj_t *batt = icon_img(bar, &icon_batt_100_lg, C_DIM);
    lv_obj_align(batt, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_t *pct = label(bar, "100%", f_status, C_DIM);
    lv_obj_align(pct, LV_ALIGN_RIGHT_MID, -70, 0);
}

/* ---- the list screen ------------------------------------------------- */

static void back_cb(lv_event_t *e) {
    (void)e;
    lv_screen_load_anim(home, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
}

static void open_list(const char *title) {
    lv_obj_t *scr = plain(NULL);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    status_bar(scr);

    lv_obj_t *hdr = plain(scr);
    lv_obj_set_size(hdr, 480, 64);
    lv_obj_set_y(hdr, STATUS_H);
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hdr, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = label(hdr, title, f_title, C_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_t *b = label(hdr, "Back", f_row, C_ACCENT);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -24, 0);

    lv_obj_t *list = plain(scr);
    lv_obj_set_size(list, 480, 800 - STATUS_H - 64);
    lv_obj_set_y(list, STATUS_H + 64);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);

    char buf[64];
    for (int i = 1; i <= 300; i++) {
        lv_obj_t *row = plain(list);
        lv_obj_set_size(row, 480, 72);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, C_LINE, 0);
        lv_obj_set_style_bg_color(row, C_ROW, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        snprintf(buf, sizeof(buf), "%s item %d", title, i);
        lv_obj_t *l = label(row, buf, f_row, C_TEXT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 24, -10);
        snprintf(buf, sizeof(buf), "%d tracks", (i * 7) % 23 + 1);
        lv_obj_t *s = label(row, buf, f_status, C_DIM);
        lv_obj_align(s, LV_ALIGN_LEFT_MID, 24, 16);
    }
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

/* ---- Home ------------------------------------------------------------ */

static const struct { const char *name; const icon_t *icon; } TILES[] = {
    { "Music",         &icon_home_music },
    { "Audiobooks",    &icon_home_audiobooks },
    { "Podcasts",      &icon_home_podcasts },
    { "Radio",         &icon_home_radio },
    { "Parametric EQ", &icon_home_eq },
    { "MSEB",          &icon_home_mseb },
    { "Stats",         &icon_home_stats },
    { "Settings",      &icon_home_settings },
};

static void tile_cb(lv_event_t *e) {
    open_list(TILES[(intptr_t)lv_event_get_user_data(e)].name);
}

static void mini_player(lv_obj_t *scr) {
    lv_obj_t *mp = plain(scr);
    lv_obj_set_size(mp, 480, MINI_H);
    lv_obj_set_y(mp, 800 - MINI_H);
    lv_obj_set_style_bg_color(mp, C_HEADER, 0);
    lv_obj_set_style_bg_opa(mp, LV_OPA_COVER, 0);
    lv_obj_t *art = plain(mp);
    lv_obj_set_size(art, MINI_H, MINI_H);
    lv_obj_set_style_bg_color(art, C_LINE, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_t *t = label(mp, "838 - And I'm All Outta Dinosaurs", f_status, C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(t, 260, lv_font_get_line_height(f_status));
    lv_obj_align(t, LV_ALIGN_LEFT_MID, MINI_H + 18, 0);
    lv_obj_t *play = plain(mp);
    lv_obj_set_size(play, 44, 44);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play, C_TEXT, 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
    lv_obj_align(play, LV_ALIGN_RIGHT_MID, -68, 0);
    lv_obj_t *pi = label(play, LV_SYMBOL_PLAY, (lv_font_t *)&lv_font_montserrat_28, C_BG);
    lv_obj_align(pi, LV_ALIGN_CENTER, 2, 0);
    lv_obj_t *next = label(mp, LV_SYMBOL_NEXT, (lv_font_t *)&lv_font_montserrat_28, C_TEXT);
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, -20, 0);
}

static void build_home(void) {
    home = plain(NULL);
    lv_obj_set_style_bg_color(home, C_BG, 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    status_bar(home);

    int top = STATUS_H + 8, rows = 4, h = (800 - MINI_H - top) / rows;
    for (int i = 0; i < 8; i++) {
        lv_obj_t *tile = plain(home);
        lv_obj_set_size(tile, 240, h);
        lv_obj_set_pos(tile, (i % 2) * 240, top + (i / 2) * h);
        lv_obj_set_style_border_color(tile, C_LINE, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_side(tile, (i / 2 < rows - 1 ? LV_BORDER_SIDE_BOTTOM : 0) |
                                           (i % 2 == 0 ? LV_BORDER_SIDE_RIGHT : 0), 0);
        lv_obj_set_style_bg_color(tile, C_ROW, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *ic = icon_img(tile, TILES[i].icon, C_ACCENT);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 18);
        lv_obj_t *l = label(tile, TILES[i].name, f_tile, C_TEXT);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -14);
    }
    mini_player(home);
    lv_screen_load(home);
}

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int ui_init(const char *font_path) {
    size_t n;
    void *ttf = read_file(font_path, &n);
    if (!ttf) {
        fprintf(stderr, "ui: cannot read font %s\n", font_path);
        return -1;
    }
    f_status = lv_tiny_ttf_create_data(ttf, n, 22);
    f_tile   = lv_tiny_ttf_create_data(ttf, n, 24);
    f_row    = lv_tiny_ttf_create_data(ttf, n, 24);
    f_title  = lv_tiny_ttf_create_data(ttf, n, 30);
    build_home();
    return 0;
}
