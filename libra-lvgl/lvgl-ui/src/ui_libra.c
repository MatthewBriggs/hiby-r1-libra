/* ui_libra.c — Libra's screens, drawn by LVGL.
 *
 * #included at the very end of app/music_hook.c when LIBRA_LVGL is defined,
 * so this is the same translation unit as Libra's engine: every screen here
 * reads Libra's own state (screen, queue[], cur_album, the settings...) and
 * every tap runs Libra's own action code, copied from the old tap handler
 * with its hit-testing taken away. Nothing about how Libra plays, syncs,
 * parks Wi-Fi or reads its keys changes; only what draws the screen and
 * reads the touchscreen does.
 *
 * The shape:
 *   - `screen` (and the variables naming what it shows) stays the source of
 *     truth. ui_sync() rebuilds the LVGL screen whenever that identity
 *     changes, by any route: a tap here, a hardware key, go_back(), resume.
 *   - Between rebuilds, a screen's refresh() updates what moves (the clock,
 *     the playing row) whenever Libra marks the frame dirty.
 *   - The status strip and the mini player live on LVGL's top layer, so a
 *     screen change slides only the content under them.
 *   - The touchscreen is read here, not by LVGL's evdev driver, so the edge
 *     swipes Libra has always had (back, home, queue, quick settings) are
 *     seen before LVGL turns the same touch into a scroll or a tap.
 */

#include "lvgl.h"
#include "r1_display.h"
#include "src/misc/cache/instance/lv_image_cache.h"

/* ---- colours, fonts, images --------------------------------------------- */

static lv_color_t c16(uint16_t c) {
    return lv_color_make((uint8_t)(((c >> 11) & 31) * 255 / 31),
                         (uint8_t)(((c >> 5) & 63) * 255 / 63),
                         (uint8_t)((c & 31) * 255 / 31));
}

static lv_font_t *uf_title, *uf_body, *uf_small, *uf_status, *uf_big;

static void *ui_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Libra's text.c sizes a font with stbtt_ScaleForPixelHeight(): `px` is
 * the font's whole ascent-to-descent height. LVGL's tiny_ttf sizes by the
 * em square (stbtt_ScaleForMappingEmToPixels()), which for Noto Sans is
 * about a third larger at the same number. The LVGL size that draws exactly
 * what Libra's `px` did, from the font's own hhea/head tables. */
static uint16_t ttf_u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static int ui_ttf_size(const void *data, size_t n, int px) {
    const uint8_t *f = data;
    if (n < 12) return px;
    int tables = ttf_u16(f + 4), upem = 0, asc = 0, desc = 0;
    for (int i = 0; i < tables && 12 + (size_t)i * 16 + 16 <= n; i++) {
        const uint8_t *e = f + 12 + i * 16;
        uint32_t off = (uint32_t)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11];
        if (off + 20 > n) continue;
        if (!memcmp(e, "head", 4)) upem = ttf_u16(f + off + 18);
        if (!memcmp(e, "hhea", 4)) {
            asc = (int16_t)ttf_u16(f + off + 4);
            desc = (int16_t)ttf_u16(f + off + 6);
        }
    }
    if (upem <= 0 || asc - desc <= 0) return px;
    return (px * upem + (asc - desc) / 2) / (asc - desc);
}

static int ui_fonts_init(void) {
    static const char *paths[] = {
        "/usr/data/podcast_res/font.ttf",   /* Noto Sans, as text.c */
        "/usr/resource/fonts/msyh.ttf",
        "/usr/resource/fonts/Korean.ttf",
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        size_t n;
        void *ttf = ui_read_file(paths[i], &n);
        if (!ttf) continue;
        /* No kerning: Libra's text.c has none, so the text matches it, and
         * a list's letter pairs outnumber the kerning cache -- every frame
         * of a scroll or slide looked them up in the font again. */
#define UI_FONT(px) lv_tiny_ttf_create_data_ex(ttf, n, ui_ttf_size(ttf, n, (px)), \
                                               LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT)
        uf_title  = UI_FONT(TEXT_PX_TITLE);
        uf_body   = UI_FONT(TEXT_PX_BODY);
        uf_small  = UI_FONT(TEXT_PX_SMALL);
        uf_status = UI_FONT(STATUS_TEXT_PX);
        uf_big    = UI_FONT(44);
#undef UI_FONT
        /* LVGL's symbols (arrows, ticks, the keyboard's keys) are not in
         * Noto Sans: they come from its built-in font instead. */
        uf_title->fallback = uf_body->fallback = uf_big->fallback = &lv_font_montserrat_28;
        uf_small->fallback = uf_status->fallback = &lv_font_montserrat_28;
        mlog("[lvgl] font %s: Libra's %d px is LVGL size %d\n", paths[i], TEXT_PX_BODY,
             ui_ttf_size(ttf, n, TEXT_PX_BODY));
        return 0;
    }
    mlog("[lvgl] no font found\n");
    return -1;
}

/* An icons_data.h bitmap is 8-bit coverage: LVGL's A8, drawn in whatever
 * colour the image's recolour says. One descriptor per icon, made on first
 * use and kept. */
static const lv_image_dsc_t *ui_icon(const icon_t *ic) {
    static struct { const icon_t *ic; lv_image_dsc_t d; } cache[96];
    static int n;
    for (int i = 0; i < n; i++) if (cache[i].ic == ic) return &cache[i].d;
    if (n == (int)(sizeof(cache) / sizeof(cache[0]))) n = 0;   /* never in practice */
    lv_image_dsc_t *d = &cache[n].d;
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_A8;
    d->header.w = ic->w;
    d->header.h = ic->h;
    d->header.stride = ic->w;
    d->data = ic->a;
    d->data_size = (uint32_t)(ic->w * ic->h);
    cache[n++].ic = ic;
    return d;
}

static lv_obj_t *ui_icon_img(lv_obj_t *parent, const icon_t *ic, uint16_t col) {
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, ui_icon(ic));
    lv_obj_set_style_image_recolor(img, c16(col), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

static void ui_icon_set(lv_obj_t *img, const icon_t *ic, uint16_t col) {
    lv_image_set_src(img, ui_icon(ic));
    lv_obj_set_style_image_recolor(img, c16(col), 0);
}

/* A bare object. Not tappable unless made so: LVGL objects are by default,
 * and a decoration inside a row (a divider, a bar, the cover box) would then
 * take the tap meant for the row. Rows, buttons, scrolling lists and
 * overlays that must block what is under them opt in. */
static lv_obj_t *ui_plain(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *ui_label(lv_obj_t *parent, const char *text, lv_font_t *font, uint16_t col) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c16(col), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* One line, cut with an ellipsis at `w` pixels. */
static lv_obj_t *ui_label_w(lv_obj_t *parent, const char *text, lv_font_t *font,
                            uint16_t col, int w) {
    lv_obj_t *l = ui_label(parent, text, font, col);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    return l;
}

/* Only touches the label when the text really changed: setting it always
 * invalidates, and refresh() runs once a second on every screen. */
static void ui_set_text(lv_obj_t *l, const char *text) {
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (cur && text && !strcmp(cur, text)) return;
    lv_label_set_text(l, text ? text : "");
}

static void ui_set_color(lv_obj_t *o, uint16_t col) {
    lv_color_t c = c16(col);
    if (!lv_color_eq(lv_obj_get_style_text_color(o, 0), c))
        lv_obj_set_style_text_color(o, c, 0);
}

static void ui_bg(lv_obj_t *o, uint16_t col) {
    lv_obj_set_style_bg_color(o, c16(col), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

/* A full-width divider at `y`, Libra's 1px COL_LINE. */
static lv_obj_t *ui_hline(lv_obj_t *parent, int y, uint16_t col) {
    lv_obj_t *o = ui_plain(parent);
    lv_obj_set_size(o, FB_W, 1);
    lv_obj_set_pos(o, 0, y);
    ui_bg(o, col);
    return o;
}

/* ---- art: Libra's RGB565 bitmaps as LVGL images --------------------------- *
 * The decode threads replace art_bits & co. under their locks; the screens
 * here show a copy, refreshed when the bitmap's sequence number moves. */
typedef struct {
    uint16_t *px;          /* ART_PX * ART_PX, or NULL for "nothing" */
    lv_image_dsc_t dsc;
    int seq;
} ui_art_t;

static ui_art_t ui_np_art = { .seq = -1 }, ui_view_art = { .seq = -1 },
                ui_artist_art = { .seq = -1 };

/* 1 when the copy changed. */
static int ui_art_take(ui_art_t *a, pthread_mutex_t *lock, uint16_t *const *bits, int seq) {
    if (seq == a->seq) return 0;
    a->seq = seq;
    pthread_mutex_lock(lock);
    if (*bits) {
        if (!a->px) a->px = malloc((size_t)ART_PX * ART_PX * 2);
        if (a->px) memcpy(a->px, *bits, (size_t)ART_PX * ART_PX * 2);
    } else if (a->px) {
        free(a->px);
        a->px = NULL;
    }
    pthread_mutex_unlock(lock);
    if (a->px) {
        lv_image_cache_drop(&a->dsc);
        memset(&a->dsc, 0, sizeof(a->dsc));
        a->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        a->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        a->dsc.header.w = ART_PX;
        a->dsc.header.h = ART_PX;
        a->dsc.header.stride = ART_PX * 2;
        a->dsc.data = (const uint8_t *)a->px;
        a->dsc.data_size = (uint32_t)ART_PX * ART_PX * 2;
    }
    return 1;
}

static void ui_art_sync_all(void) {
    ui_art_take(&ui_np_art, &art_lock, &art_bits, art_seq());
    ui_art_take(&ui_view_art, &view_art_lock, &view_art_bits, view_art_seq());
    ui_art_take(&ui_artist_art, &artist_art_lock, &artist_art_bits, artist_art_seq());
}

/* The cover box: a placeholder square holding the image, rounded with the
 * Rounded art corners setting as everywhere else. */
static lv_obj_t *ui_art_box(lv_obj_t *parent, int x, int y, int size, uint16_t placeholder) {
    lv_obj_t *box = ui_plain(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, size, size);
    ui_bg(box, placeholder);
    if (art_round_enabled) {
        lv_obj_set_style_radius(box, ART_CORNER_R, 0);
        lv_obj_set_style_clip_corner(box, true, 0);
    }
    lv_obj_t *img = lv_image_create(box);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    /* The size, kept: lv_obj_get_width() reads 0 until LVGL lays it out. */
    lv_obj_set_user_data(box, (void *)(intptr_t)size);
    return box;
}

static void ui_art_box_set(lv_obj_t *box, ui_art_t *a) {
    lv_obj_t *img = lv_obj_get_child(box, 0);
    if (!img) return;
    if (a->px) {
        lv_image_set_src(img, &a->dsc);
        int size = (int)(intptr_t)lv_obj_get_user_data(box);
        if (size != ART_PX) lv_image_set_scale(img, (uint32_t)(256 * size / ART_PX));
        lv_image_set_pivot(img, 0, 0);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(img);
    } else {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The mini player's thumbnail: scaled once when the cover changes rather
 * than by LVGL on every repaint of the bar. */
#define UI_THUMB (MINI_H + 8)
static uint16_t ui_thumb_px[UI_THUMB * UI_THUMB];
static lv_image_dsc_t ui_thumb_dsc;
static int ui_thumb_valid;

static void ui_thumb_make(void) {
    ui_thumb_valid = ui_np_art.px != NULL;
    if (!ui_thumb_valid) return;
    for (int y = 0; y < UI_THUMB; y++) {
        const uint16_t *src = ui_np_art.px + (size_t)(y * ART_PX / UI_THUMB) * ART_PX;
        for (int x = 0; x < UI_THUMB; x++)
            ui_thumb_px[y * UI_THUMB + x] = src[x * ART_PX / UI_THUMB];
    }
    lv_image_cache_drop(&ui_thumb_dsc);
    memset(&ui_thumb_dsc, 0, sizeof(ui_thumb_dsc));
    ui_thumb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ui_thumb_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    ui_thumb_dsc.header.w = UI_THUMB;
    ui_thumb_dsc.header.h = UI_THUMB;
    ui_thumb_dsc.header.stride = UI_THUMB * 2;
    ui_thumb_dsc.data = (const uint8_t *)ui_thumb_px;
    ui_thumb_dsc.data_size = sizeof(ui_thumb_px);
}

/* ---- glyphs Libra draws as shapes: play, pause, previous, next ---------- */
enum { G_PLAY, G_PAUSE, G_PREV, G_NEXT, G_QUEUE };

/* Libra's own geometry (fill_triangle(), and the bars beside it), scaled by
 * the glyph's size s, kept in the object's user data beside the kind. The
 * object is half as big again each way, so the skip bar beyond the
 * triangle's tip and the queue's triangle are inside it and not clipped
 * away by LVGL, which only draws within it:
 *   play  -- a triangle 0.87 s wide, s tall, nudged right by s/9 so it sits
 *            optically centred in its circle (Libra's +4 at 36);
 *   pause -- two bars 10/36 s wide with the same gap (Libra's 10/10/36);
 *   prev/next -- the play triangle centred, and a bar 5/34 s wide (at least
 *            3) one pixel past its tip (Libra's 5 px at 34);
 *   queue -- draw_queue_icon(): three bars, the last short, and a play
 *            triangle, at s/26 of Libra's size, centred (Libra's sits a few
 *            pixels low and right of its ring's centre). */
static void ui_glyph_draw(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    int ud = (int)(intptr_t)lv_obj_get_user_data(o);
    int kind = ud & 0xff, s = ud >> 8;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = (a.x1 + a.x2 + 1) / 2, cy = (a.y1 + a.y2 + 1) / 2;
    int half = s / 2, w = s * 87 / 100;
    lv_color_t col = lv_obj_get_style_text_color(o, 0);
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = col;
    t.opa = LV_OPA_COVER;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = col;
    r.bg_opa = LV_OPA_COVER;
    if (kind == G_QUEUE) {
#define Q(v) ((v) * s / 26)
        int x = cx - Q(14), y = cy - Q(12);   /* its 28 x 24 box, centred */
        lv_area_t b1 = { x, y, x + Q(26) - 1, y + Q(3) - 1 };
        lv_area_t b2 = { x, y + Q(8), x + Q(26) - 1, y + Q(8) + Q(3) - 1 };
        lv_area_t b3 = { x, y + Q(16), x + Q(15) - 1, y + Q(16) + Q(3) - 1 };
        lv_draw_rect(layer, &r, &b1);
        lv_draw_rect(layer, &r, &b2);
        lv_draw_rect(layer, &r, &b3);
        int th = Q(13), tw = th * 87 / 100, tx = x + Q(23), ty = y + Q(17);
        t.p[0].x = tx - tw / 2;        t.p[0].y = ty - th / 2;
        t.p[1].x = tx - tw / 2;        t.p[1].y = ty - th / 2 + th;
        t.p[2].x = tx + (tw - tw / 2); t.p[2].y = ty;
        lv_draw_triangle(layer, &t);
#undef Q
        return;
    }
    if (kind == G_PAUSE) {
        int bw = s * 10 / 36 > 2 ? s * 10 / 36 : 2, gap = bw;
        lv_area_t b1 = { cx - gap / 2 - bw, cy - half, cx - gap / 2 - 1, cy - half + s - 1 };
        lv_area_t b2 = { cx + (gap + 1) / 2, cy - half, cx + (gap + 1) / 2 + bw - 1, cy - half + s - 1 };
        lv_draw_rect(layer, &r, &b1);
        lv_draw_rect(layer, &r, &b2);
        return;
    }
    int dir = kind == G_PREV ? -1 : 1;
    int tcx = kind == G_PLAY ? cx + s / 9 : cx;   /* the triangle's centre */
    int base = tcx - dir * w / 2, tip = tcx + dir * (w - w / 2);
    t.p[0].x = base; t.p[0].y = cy - half;
    t.p[1].x = base; t.p[1].y = cy - half + s;
    t.p[2].x = tip;  t.p[2].y = cy;
    lv_draw_triangle(layer, &t);
    if (kind == G_PLAY) return;
    int bw = s * 5 / 34 > 3 ? s * 5 / 34 : 3;
    int bx = dir > 0 ? tip + 1 : tip - 1 - bw;
    lv_area_t bar = { bx, cy - half, bx + bw - 1, cy - half + s - 1 };
    lv_draw_rect(layer, &r, &bar);
}

/* size: the glyph's height, as Libra's fill_triangle() takes it (the
 * queue's: Libra's 26). */
static lv_obj_t *ui_glyph(lv_obj_t *parent, int kind, int size, uint16_t col) {
    lv_obj_t *o = ui_plain(parent);
    lv_obj_set_size(o, size * 3 / 2, size * 3 / 2);
    lv_obj_set_user_data(o, (void *)(intptr_t)(kind | size << 8));
    lv_obj_set_style_text_color(o, c16(col), 0);
    lv_obj_add_event_cb(o, ui_glyph_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void ui_glyph_set(lv_obj_t *o, int kind, uint16_t col) {
    int ud = (int)(intptr_t)lv_obj_get_user_data(o);
    if ((ud & 0xff) != kind) {
        lv_obj_set_user_data(o, (void *)(intptr_t)(kind | (ud & ~0xff)));
        lv_obj_invalidate(o);
    }
    lv_obj_set_style_text_color(o, c16(col), 0);
}

/* A filled circle (Libra's fill_circle), optionally a ring of `ring` px. */
static lv_obj_t *ui_circle(lv_obj_t *parent, int cx, int cy, int r, uint16_t col, int ring) {
    lv_obj_t *o = ui_plain(parent);
    lv_obj_set_size(o, 2 * r, 2 * r);
    lv_obj_set_pos(o, cx - r, cy - r);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    if (ring) {
        lv_obj_set_style_border_width(o, ring, 0);
        lv_obj_set_style_border_color(o, c16(col), 0);
    } else {
        ui_bg(o, col);
    }
    return o;
}

/* An on/off switch in Libra's style, at the right of a row of height h. */
static lv_obj_t *ui_switch(lv_obj_t *row, int on, int h) {
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_set_pos(sw, FB_W - 24 - 64, (h - 34) / 2);
    lv_obj_set_style_bg_color(sw, c16(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, c16(COL_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, c16(COL_TEXT), LV_PART_KNOB);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);   /* the row takes the tap */
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static void ui_switch_set(lv_obj_t *sw, int on) {
    if (!sw) return;
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

/* ---- the screen being shown ---------------------------------------------- */

typedef struct {
    lv_obj_t *scr;          /* the LVGL screen */
    lv_obj_t *list;         /* its scrolling content, if it has one */
    uint32_t ident;         /* which Libra screen this is a picture of */
    void (*refresh)(void);
    int status;             /* draws under the status strip */
    int nav_n;              /* nav_n when built: smaller next time = back */
    int screen;             /* Libra's screen when built */
} ui_shown_t;

static ui_shown_t ui;
static int ui_rebuild_req;          /* a screen's data changed under it */

static uint32_t ui_hash_str(uint32_t h, const char *s) {
    while (s && *s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return (h ^ 0xFF) * 16777619u;
}
static uint32_t ui_hash_int(uint32_t h, int v) {
    for (int i = 0; i < 4; i++) { h ^= (uint8_t)(v >> (i * 8)); h *= 16777619u; }
    return h;
}

/* Everything that makes one screen a different page from another, for the
 * rebuild check. What merely moves on a page (the playing track, the clock)
 * is refresh()'s business, not this. */
static uint32_t ui_ident(void) {
    uint32_t h = ui_hash_int(2166136261u, (int)screen);
    switch (screen) {
    case SC_ARTISTS:
        h = ui_hash_str(h, cur_facet);
        h = ui_hash_int(h, total);
        break;
    case SC_ALBUMS:
        h = ui_hash_str(h, cur_facet);
        h = ui_hash_str(h, cur_artist);
        h = ui_hash_int(h, recent_mode);
        h = ui_hash_int(h, total);
        break;
    case SC_TRACKS:
        h = ui_hash_int(h, pod_list * 2 + ab_list);
        h = ui_hash_int(h, browsing_is_playlist);
        h = ui_hash_str(h, cur_album);
        h = ui_hash_str(h, cur_artist);
        h = ui_hash_str(h, cur_feed);
        h = ui_hash_int(h, track_n);
        break;
    case SC_ARTIST_PAGE:
        h = ui_hash_str(h, artist_page_name);
        h = ui_hash_int(h, ac_cur);
        h = ui_hash_int(h, artist_page_album_n);
        break;
    case SC_PLAYING:
        h = ui_hash_int(h, audiobook_mode | radio_mode << 1 | podcast_mode << 2 |
                           recording_playback_mode << 3);
        h = ui_hash_int(h, pod_notes_showing);
        break;
    case SC_QUEUE:
        h = ui_hash_int(h, queue_n);
        h = ui_hash_int(h, shuffle_enabled);
        break;
    case SC_PLAYLISTS:    h = ui_hash_int(h, playlist_n); break;
    case SC_RADIO:        h = ui_hash_int(h, station_n); break;
    case SC_RADIO_RECORDINGS: h = ui_hash_int(h, radio_recording_n); break;
    case SC_AUDIOBOOKS:   h = ui_hash_int(h, ab_book_n); break;
    case SC_PODCASTS:     h = ui_hash_int(h, pod_feed_n); break;
    case SC_EQ_BANDS:     h = ui_hash_int(h, eq_cur.band_n); break;
    case SC_EQ_BAND:      h = ui_hash_int(h, eq_editing_band); break;
    case SC_KEYBOARD:     h = ui_hash_str(h, kb_title); break;
    case SC_POD_SETTINGS: h = ui_hash_str(h, cur_feed); break;
    case SC_SETTINGS_THEME: case SC_SETTINGS_THEMEMODE: case SC_SETTINGS:
        h = ui_hash_int(h, g_accent_idx * 16 + theme_mode);   /* colours change everything */
        break;
    default: break;
    }
    return h;
}

/* Screens with a list keep Libra's `scroll`/`scroll_px` in step with where
 * the list really is, so the back history (nav_capture()) puts you back at
 * the same row. */
static void ui_list_scrolled(lv_event_t *e) {
    lv_obj_t *l = lv_event_get_target_obj(e);
    int y = lv_obj_get_scroll_y(l);
    if (y < 0) y = 0;
    scroll = y / ROW_H;
    scroll_px = y % ROW_H;
}

/* A screen's scrolling content: everything from `top` down. */
static lv_obj_t *ui_list(lv_obj_t *scr, int top) {
    lv_obj_t *l = ui_plain(scr);
    lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);   /* a drag on empty space still scrolls */
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(l, 0, top);
    lv_obj_set_size(l, FB_W, FB_H - top);
    lv_obj_add_event_cb(l, ui_list_scrolled, LV_EVENT_SCROLL, NULL);
    ui.list = l;
    return l;
}

/* Room under the content for the mini player, when it is up. */
static void ui_list_pad(void) {
    if (!ui.list) return;
    int want = mini_visible() ? MINI_H : 0;
    if (lv_obj_get_style_pad_bottom(ui.list, 0) != want)
        lv_obj_set_style_pad_bottom(ui.list, want, 0);
}

/* Put the list back where Libra's scroll says it was (a back, a resume). */
static void ui_list_restore(void) {
    if (!ui.list) return;
    lv_obj_update_layout(ui.list);
    int y = scroll * ROW_H + scroll_px;
    if (y > 0) lv_obj_scroll_to_y(ui.list, y, LV_ANIM_OFF);
}

/* A row: a tappable strip of height h at y in `parent`, with Libra's
 * divider under it. idx goes to the callback. */
static lv_obj_t *ui_row(lv_obj_t *parent, int y, int h, lv_event_cb_t cb, int idx) {
    lv_obj_t *row = ui_plain(parent);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, FB_W, h);
    lv_obj_set_style_bg_color(row, c16(COL_ROW), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, c16(COL_LINE), 0);
    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    }
    return row;
}

static int ui_idx(lv_event_t *e) { return (int)(intptr_t)lv_event_get_user_data(e); }

/* After any action: the main loop's next pass sees what changed. */
static int ui_dirty;
#define UI_ACTED() (ui_dirty = 1)

/* ---- the status strip and the mini player (top layer) -------------------- */

static struct {
    lv_obj_t *bar, *back, *sec, *vol_ic, *vol, *batt_ic, *batt, *spin, *action;
    const icon_t *sec_icon;
    const char *action_text;
} st_ui;

static void ui_status_back(lv_event_t *e) {
    (void)e;
    if (g_header_show_back) { go_back(); UI_ACTED(); }
}

/* A screen's one header action (MSEB's Reset), left of the battery. */
static void ui_status_action(lv_event_t *e) {
    (void)e;
    if (screen == SC_MSEB) {
        for (int i = 0; i < MSEB_BAND_N; i++) mseb_gain[i] = 0.0f;
        eq_set_mseb(mseb_on, mseb_gain);
        mseb_save(mseb_gain, mseb_on);
        ui_rebuild_req = 1;
    }
    UI_ACTED();
}

static void ui_status_build(void) {
    lv_obj_t *bar = ui_plain(lv_layer_top());
    st_ui.bar = bar;
    lv_obj_set_size(bar, FB_W, STATUS_H);
    ui_bg(bar, COL_HEADER);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, c16(COL_LINE), 0);

    st_ui.back = ui_plain(bar);       /* the arrow and section icon, one target */
    lv_obj_set_size(st_ui.back, BACK_ARROW_TAP_W, STATUS_H);
    lv_obj_add_flag(st_ui.back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(st_ui.back, ui_status_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *arrow = ui_label(st_ui.back, LV_SYMBOL_LEFT, (lv_font_t *)&lv_font_montserrat_28, COL_ACCENT);
    lv_obj_align(arrow, LV_ALIGN_LEFT_MID, BACK_ARROW_X - 4, 0);
    st_ui.sec = lv_image_create(st_ui.back);
    lv_obj_align(st_ui.sec, LV_ALIGN_LEFT_MID, BACK_ARROW_X + BACK_ARROW_W + 10, 0);

    st_ui.vol_ic = lv_image_create(bar);
    st_ui.vol = ui_label(bar, "", uf_status, COL_DIM);
    st_ui.batt_ic = lv_image_create(bar);
    st_ui.batt = ui_label(bar, "", uf_status, COL_DIM);
    st_ui.spin = lv_spinner_create(bar);
    lv_obj_set_size(st_ui.spin, 20, 20);
    lv_obj_set_style_arc_width(st_ui.spin, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(st_ui.spin, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(st_ui.spin, c16(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(st_ui.spin, c16(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_add_flag(st_ui.spin, LV_OBJ_FLAG_HIDDEN);
    st_ui.action = ui_label(bar, "", uf_status, COL_ACCENT);
    lv_obj_add_flag(st_ui.action, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(st_ui.action, 16);
    lv_obj_add_event_cb(st_ui.action, ui_status_action, LV_EVENT_CLICKED, NULL);
}

static void ui_status_refresh(void) {
    if (!st_ui.bar) return;
    int shown = status_strip_shown() && screen != SC_KEYBOARD;
    if (!shown) { lv_obj_add_flag(st_ui.bar, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(st_ui.bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(st_ui.bar);

    g_header_show_back = screen != SC_MENU;
    const icon_t *sec = g_header_show_back ? status_section_icon() : NULL;
    if (sec) {
        lv_obj_remove_flag(st_ui.back, LV_OBJ_FLAG_HIDDEN);
        if (sec != st_ui.sec_icon) { st_ui.sec_icon = sec; ui_icon_set(st_ui.sec, sec, COL_ACCENT); }
    } else {
        lv_obj_add_flag(st_ui.back, LV_OBJ_FLAG_HIDDEN);
    }
    int lx = sec ? BACK_ARROW_X + BACK_ARROW_W + 10 + sec->w + 22 : 24;

    char buf[24];
    int vol = audio_volume();
    const icon_t *vic = vol <= 0 ? &icon_vol_mute_lg : vol < 34 ? &icon_vol_low_lg
                       : vol < 67 ? &icon_vol_mid_lg : &icon_vol_high_lg;
    ui_icon_set(st_ui.vol_ic, vic, COL_DIM);
    lv_obj_align(st_ui.vol_ic, LV_ALIGN_LEFT_MID, lx, 0);
    int steps = audio_bt_vol_steps();
    int cur = (vol * steps + 50) / 100;
    if (cur < 0) cur = 0;
    if (cur > steps) cur = steps;
    snprintf(buf, sizeof(buf), "%d/%d", cur, steps);
    ui_set_text(st_ui.vol, buf);
    lv_obj_align(st_ui.vol, LV_ALIGN_LEFT_MID, lx + vic->w + 9, 0);

    int pct = st_battery_pct();
    const icon_t *bic = pct > 87 ? &icon_batt_100_lg : pct > 62 ? &icon_batt_75_lg
                      : pct > 37 ? &icon_batt_50_lg : pct > 12 ? &icon_batt_25_lg : &icon_batt_0_lg;
    uint16_t bc = st_charging() ? COL_ACCENT : (pct >= 0 && pct <= 15 ? RGB(230, 80, 70) : COL_DIM);
    ui_icon_set(st_ui.batt_ic, bic, bc);
    lv_obj_align(st_ui.batt_ic, LV_ALIGN_RIGHT_MID, -20, 0);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else buf[0] = '\0';
    ui_set_text(st_ui.batt, buf);
    lv_obj_align_to(st_ui.batt, st_ui.batt_ic, LV_ALIGN_OUT_LEFT_MID, -11, 0);

    lv_obj_update_layout(st_ui.batt);
    int right_x = lv_obj_get_x(st_ui.batt);
    if (status_busy()) {
        lv_obj_remove_flag(st_ui.spin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(st_ui.spin, right_x - 8 - 20, (STATUS_H - 20) / 2);
        right_x -= 8 + 20;
    } else {
        lv_obj_add_flag(st_ui.spin, LV_OBJ_FLAG_HIDDEN);
    }
    const char *act = screen == SC_MSEB ? "Reset" : "";
    ui_set_text(st_ui.action, act);
    lv_obj_update_layout(st_ui.action);
    lv_obj_set_pos(st_ui.action, right_x - 28 - lv_obj_get_width(st_ui.action),
                   (STATUS_H - lv_font_get_line_height(uf_status)) / 2);
}

static struct {
    lv_obj_t *bar, *thumb, *title, *sub, *play_bg, *play, *side, *back_l, *fwd_l, *prog;
    int thumb_seq;
} mini_ui = { .thumb_seq = -1 };

/* One tap target for the whole bar, split by x exactly as Libra's tap
 * handler splits it (MINI_ZONE_* / MINI_RADIO_ZONE_*). */
static void ui_mini_tap(lv_event_t *e) {
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    int x = p.x;
    if (radio_mode) {
        if (x > MINI_RADIO_ZONE_PLAY) audio_toggle();
        else if (x > MINI_RADIO_ZONE_BACK) {
            if (audio_radio_offset_ms() < audio_radio_max_rewind_ms()) {
                audio_radio_seek_relative_ms(-10000);
                seek_toast(-10000);
            }
        } else screen = SC_PLAYING;
    } else if (audiobook_mode) {
        if (x > MINI_ZONE_SIDE)      audio_seek_ms(audio_pos_ms() + 10000);
        else if (x > MINI_ZONE_PLAY) audio_toggle();
        else if (x > MINI_ZONE_BACK) audio_seek_ms(audio_pos_ms() - 10000);
        else                         screen = SC_PLAYING;
    } else if (podcast_mode) {
        if (x > MINI_ZONE_SIDE)      audio_seek_ms(audio_pos_ms() + 30000);
        else if (x > MINI_ZONE_PLAY) audio_toggle();
        else if (x > MINI_ZONE_BACK) audio_seek_ms(audio_pos_ms() - 30000);
        else                         screen = SC_PLAYING;
    } else if (recording_playback_mode) {
        if (x > MINI_RADIO_ZONE_PLAY) audio_toggle();
        else                          screen = SC_PLAYING;
    } else {
        if (x > MINI_ZONE_SIDE)      play_index(next_track_index());
        else if (x > MINI_ZONE_PLAY) audio_toggle();
        else { screen = SC_PLAYING; played_from_browse = 0; }
    }
    UI_ACTED();
}

static void ui_mini_build(void) {
    lv_obj_t *bar = ui_plain(lv_layer_top());
    mini_ui.bar = bar;
    lv_obj_set_size(bar, FB_W, MINI_H);
    lv_obj_set_pos(bar, 0, FB_H - MINI_H);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);   /* the thumb overhangs */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bar, ui_mini_tap, LV_EVENT_CLICKED, NULL);
    ui_bg(bar, COL_HEADER);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    mini_ui.thumb = lv_image_create(bar);
    lv_obj_set_pos(mini_ui.thumb, 0, MINI_H - UI_THUMB);
    mini_ui.title = ui_label_w(bar, "", uf_small, COL_TEXT, 200);
    mini_ui.sub = ui_label_w(bar, "", uf_small, COL_DIM, 200);
    mini_ui.play_bg = ui_circle(bar, MINI_PLAY_CX, MINI_H / 2, MINI_BTN_R, COL_ACCENT, 0);
    /* Libra's sizes: play/pause pr - 4 tall in its circle, next 22. */
    mini_ui.play = ui_glyph(mini_ui.play_bg, G_PLAY, MINI_BTN_R - 4, COL_BG);
    lv_obj_center(mini_ui.play);
    mini_ui.side = ui_glyph(bar, G_NEXT, 22, COL_TEXT);
    /* Triangle and bar together centred on MINI_SIDE_CX. */
    lv_obj_set_pos(mini_ui.side, MINI_SIDE_CX - 2 - 33 / 2, MINI_H / 2 - 33 / 2);
    mini_ui.back_l = ui_label(bar, "-10", uf_small, COL_TEXT);
    mini_ui.fwd_l = ui_label(bar, "+10", uf_small, COL_TEXT);
    mini_ui.prog = ui_plain(lv_layer_top());
    lv_obj_set_size(mini_ui.prog, 0, 6);
    lv_obj_set_pos(mini_ui.prog, 0, FB_H - 6);
    lv_obj_add_flag(mini_ui.prog, LV_OBJ_FLAG_HIDDEN);
}

/* A small label centred on x, in the bar's middle line. */
static void ui_mini_at(lv_obj_t *l, const char *s, int cx, uint16_t col) {
    ui_set_text(l, s);
    ui_set_color(l, col);
    lv_obj_update_layout(l);
    lv_obj_set_pos(l, cx - lv_obj_get_width(l) / 2, MINI_H / 2 - lv_font_get_line_height(uf_small) / 2);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
}

static void ui_mini_refresh(void) {
    if (!mini_ui.bar) return;
    if (!mini_visible() || screen == SC_KEYBOARD) {
        lv_obj_add_flag(mini_ui.bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mini_ui.prog, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(mini_ui.bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mini_ui.bar);
    /* Libra kept the palette current for the mini player too (BG: a track
     * change while browsing left it stale). */
    if (cover_palette_enabled && !audiobook_mode && !radio_mode && !recording_playback_mode)
        compute_cover_palette();
    int themed = np_chrome_themed();
    uint16_t bg = themed ? np_col_bg() : COL_HEADER;
    uint16_t fg = themed ? np_col_fg() : COL_TEXT;
    uint16_t dim = themed ? np_col_dim() : COL_DIM;
    uint16_t acc = themed ? np_col_accent() : COL_ACCENT;
    uint16_t pbg = themed ? np_col_bg() : COL_BG;
    ui_bg(mini_ui.bar, bg);
    lv_obj_set_style_border_color(mini_ui.bar, c16(themed ? np_col_line() : COL_LINE), 0);

    int rec = recording_playback_mode;
    int text_x = 20;
    if (!radio_mode) {
        if (mini_ui.thumb_seq != ui_np_art.seq) {
            mini_ui.thumb_seq = ui_np_art.seq;
            ui_thumb_make();
            if (ui_thumb_valid) lv_image_set_src(mini_ui.thumb, &ui_thumb_dsc);
        }
        if (ui_thumb_valid && !rec) lv_obj_remove_flag(mini_ui.thumb, LV_OBJ_FLAG_HIDDEN);
        else                        lv_obj_add_flag(mini_ui.thumb, LV_OBJ_FLAG_HIDDEN);
        text_x = UI_THUMB + 16;
    } else {
        lv_obj_add_flag(mini_ui.thumb, LV_OBJ_FLAG_HIDDEN);
    }

    /* The text, cut before whichever control cluster this mode has. */
    int text_edge = radio_mode ? MINI_RADIO_BACK_CX - 20
                  : rec ? MINI_RADIO_ZONE_PLAY - 20
                  : audiobook_mode ? MINI_ZONE_BACK - 20
                  : MINI_ZONE_PLAY - 20;
    const char *title = "", *sub = "";
    char subbuf[LIB_NAME_LEN];
    uint16_t sub_col = dim;
    if (rec) {
        title = recording_playback_name;
        sub = "Recording";
    } else if (radio_mode) {
        title = radio_name;
        long behind_ms = audio_radio_offset_ms();
        if (behind_ms > 0)
            snprintf(subbuf, sizeof(subbuf), "-%ld:%02ld", behind_ms / 60000, (behind_ms / 1000) % 60);
        else
            snprintf(subbuf, sizeof(subbuf), "%s", audio_is_active() ? "LIVE" : "stopped");
        sub = subbuf;
        if (behind_ms <= 0 && audio_is_active()) sub_col = acc;
    } else if (cur_track >= 0 && cur_track < queue_n) {
        lib_track_t *t = &queue[cur_track];
        title = t->name;
        if (audiobook_mode) sub = q_album;
        else { snprintf(subbuf, sizeof(subbuf), "%s", t->artist[0] ? t->artist : q_artist); sub = subbuf; }
    }
    lv_obj_set_width(mini_ui.title, text_edge - text_x);
    lv_obj_set_width(mini_ui.sub, text_edge - text_x);
    ui_set_text(mini_ui.title, title);
    ui_set_text(mini_ui.sub, sub);
    ui_set_color(mini_ui.title, fg);
    ui_set_color(mini_ui.sub, sub_col);
    lv_obj_set_pos(mini_ui.title, text_x, 12);
    lv_obj_set_pos(mini_ui.sub, text_x, 42);

    /* Play/pause in the accent: radio and recordings keep the larger one. */
    int big = radio_mode || rec;
    int cx = big ? FB_W - 46 : MINI_PLAY_CX, pr = big ? 26 : MINI_BTN_R;
    lv_obj_set_size(mini_ui.play_bg, 2 * pr, 2 * pr);
    lv_obj_set_pos(mini_ui.play_bg, cx - pr, MINI_H / 2 - pr);
    ui_bg(mini_ui.play_bg, acc);
    lv_obj_set_size(mini_ui.play, 2 * (pr - 4), 2 * (pr - 4));
    lv_obj_center(mini_ui.play);
    ui_glyph_set(mini_ui.play, audio_is_paused() ? G_PLAY : G_PAUSE, pbg);

    lv_obj_add_flag(mini_ui.side, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mini_ui.back_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mini_ui.fwd_l, LV_OBJ_FLAG_HIDDEN);
    if (audiobook_mode) {
        ui_mini_at(mini_ui.back_l, "-10", MINI_BACK_CX, COL_TEXT);
        ui_mini_at(mini_ui.fwd_l, "+10", MINI_SIDE_CX, COL_TEXT);
    } else if (radio_mode) {
        ui_mini_at(mini_ui.back_l, "-10", MINI_RADIO_BACK_CX, fg);
    } else if (!rec) {
        lv_obj_remove_flag(mini_ui.side, LV_OBJ_FLAG_HIDDEN);
        ui_glyph_set(mini_ui.side, G_NEXT, fg);
    }

    /* Position along the very bottom edge, corner to corner (R32). */
    if (!radio_mode && !rec && cur_track >= 0 && cur_track < queue_n) {
        int pending = audio_seek_pending_ms();
        int pos = pending >= 0 ? pending : audio_pos_ms(), dur;
        if (audiobook_mode && cur_track < ab_book.chap_n) {
            const ab_chapter_t *ch = &ab_book.chap[cur_track];
            dur = (int)ch->dur_ms;
            if (dur <= 0) dur = audio_dur_ms();
            pos -= (int)ch->file_start_ms;
            if (pos < 0) pos = 0;
        } else {
            dur = audio_dur_ms();
            if (dur <= 0) dur = queue[cur_track].dur_ms;
        }
        int w = dur > 0 ? (int)((int64_t)FB_W * pos / dur) : 0;
        if (w > FB_W) w = FB_W;
        lv_obj_remove_flag(mini_ui.prog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(mini_ui.prog, w);
        ui_bg(mini_ui.prog, acc);
        lv_obj_move_foreground(mini_ui.prog);
    } else {
        lv_obj_add_flag(mini_ui.prog, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- screens -------------------------------------------------------------- */

static lv_obj_t *ui_new_screen(uint16_t bg) {
    lv_obj_t *scr = ui_plain(NULL);
    ui_bg(scr, bg);
    return scr;
}

/* A plain titled message, for anything with nothing to list. */
static lv_obj_t *ui_empty_note(lv_obj_t *parent, int y, const char *text) {
    lv_obj_t *l = ui_label_w(parent, text, uf_body, COL_DIM, FB_W - 48);
    lv_obj_set_pos(l, 24, y + 20);
    return l;
}

static int ui_tfd = -1;
static lv_indev_t *ui_touch;
static struct { int x, y, down, sx, sy, edge, fired, swallow, activity, latched; } tch;
/* The home swipe went to Home: fade there, where back slides. */
static int ui_home_jump;
/* The screen change under way was made by an edge swipe (back, home): its
 * animation runs at half the time. */
static int ui_swipe_nav;
/* Timing of each screen change, logged once its animation has run: see
 * ui_perf_done(). */
static struct {
    uint64_t up, t0, built, refr0, first, last, draw_sum;
    int on, frames, draw_max, build_ms, anim_ms, scr;
} perf;

/* Any touch counts as use, for Libra's screen timeout (its own `idle`). */
static int ui_touch_activity(void) {
    int a = tch.activity || tch.down;
    tch.activity = 0;
    return a;
}


static void ui_sync(int dirty);

/* While the dropdown is being pulled: how far, or -1. ui_overlays.c moves
 * the panel with it; let go past a third of its height and it opens. */
static int ui_qs_pull = -1;
/* The same for the queue drawer, pulled in from the right edge. */
static int ui_qd_pull = -1;
static int ui_qd_opened;          /* the pull just opened it: no slide on top */

/* ---- swipe a row sideways to remove it (Libra's R70) ----------------------
 * Past 120px either way, released, and the row's remove action runs. The row
 * turns the accent colour and its content follows the finger. Not from the
 * left 100px (the back swipe's) or the right-edge queue zone, as Libra's
 * ROW_SWIPE_BLOCKED. The list stops scrolling while a swipe is on, and the
 * tap that ends a swipe is not also a tap on the row. */
typedef void (*ui_swipe_fn)(int idx);
typedef struct { ui_swipe_fn fn; int idx; } ui_swipe_t;   /* one per swipeable row */
static struct { lv_obj_t *row; int x0, y0, dx, active, ate_tap; } swp;

static void ui_swipe_shift(lv_obj_t *row, int dx) {
    uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; i++)
        lv_obj_set_style_translate_x(lv_obj_get_child(row, (int32_t)i), dx, 0);
}

static void ui_swipe_end(lv_obj_t *row) {
    ui_swipe_shift(row, 0);
    lv_obj_remove_local_style_prop(row, LV_STYLE_BG_OPA, 0);
    lv_obj_remove_local_style_prop(row, LV_STYLE_BG_COLOR, 0);
    lv_obj_set_user_data(row, NULL);           /* ui_row_playing() re-applies */
    if (ui.list) lv_obj_add_flag(ui.list, LV_OBJ_FLAG_SCROLLABLE);
    swp.active = 0;
    swp.row = NULL;
}

static void ui_swipe_event(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);
    ui_swipe_t *s = lv_event_get_user_data(e);
    if (code == LV_EVENT_DELETE) { lv_free(s); return; }
    lv_point_t p;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        swp.row = row; swp.x0 = p.x; swp.y0 = p.y; swp.dx = 0; swp.active = 0; swp.ate_tap = 0;
    } else if (code == LV_EVENT_PRESSING && swp.row == row) {
        int dx = p.x - swp.x0, dy = p.y - swp.y0;
        if (!swp.active && abs(dx) > 24 && abs(dx) > 2 * abs(dy) &&
            swp.x0 >= 100 && swp.x0 < FB_W - QD_EDGE_ZONE) {
            swp.active = 1;
            if (ui.list) lv_obj_remove_flag(ui.list, LV_OBJ_FLAG_SCROLLABLE);
            ui_bg(row, COL_ACCENT);
        }
        if (swp.active) { swp.dx = dx; ui_swipe_shift(row, dx); }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && swp.row == row) {
        if (swp.active) {
            swp.ate_tap = 1;
            int go = code == LV_EVENT_RELEASED && (swp.dx <= -120 || swp.dx >= 120);
            ui_swipe_end(row);
            if (go) {
                s->fn(s->idx);
                ui_rebuild_req = 1;
                UI_ACTED();
            }
        } else {
            swp.row = NULL;
        }
    }
}

/* A tap's handler asks this first: 0 when the release ended a swipe. */
static int ui_tap_ok(void) {
    if (!swp.ate_tap) return 1;
    swp.ate_tap = 0;
    return 0;
}

static void ui_row_swipeable(lv_obj_t *row, int idx, ui_swipe_fn fn) {
    ui_swipe_t *s = lv_malloc(sizeof(*s));
    if (!s) return;
    s->fn = fn;
    s->idx = idx;
    lv_obj_add_event_cb(row, ui_swipe_event, LV_EVENT_ALL, s);
}

/* ---- drag a row by its grip to reorder (Libra's R70) ----------------------
 * The grip on the right of a queue row or a playlist track: the row follows
 * the finger and, let go, moves to the slot it was dropped on. */
typedef void (*ui_move_fn)(int from, int to);
typedef struct { ui_move_fn fn; int idx, n; } ui_grip_t;
static struct { int y0, dy; lv_obj_t *row; } grp;

static void ui_grip_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    ui_grip_t *g = lv_event_get_user_data(e);
    if (code == LV_EVENT_DELETE) { lv_free(g); return; }
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_current_target_obj(e));
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        grp.row = row; grp.y0 = p.y; grp.dy = 0;
        if (ui.list) lv_obj_remove_flag(ui.list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_foreground(row);
        ui_bg(row, COL_ROW);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, c16(COL_ACCENT), 0);
    } else if (code == LV_EVENT_PRESSING && grp.row == row) {
        grp.dy = p.y - grp.y0;
        lv_obj_set_style_translate_y(row, grp.dy, 0);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && grp.row == row) {
        int steps = (grp.dy + (grp.dy >= 0 ? ROW_H / 2 : -ROW_H / 2)) / ROW_H;
        int to = g->idx + steps;
        if (to < 0) to = 0;
        if (to >= g->n) to = g->n - 1;
        lv_obj_set_style_translate_y(row, 0, 0);
        if (ui.list) lv_obj_add_flag(ui.list, LV_OBJ_FLAG_SCROLLABLE);
        grp.row = NULL;
        if (code == LV_EVENT_RELEASED && to != g->idx) g->fn(g->idx, to);
        ui_rebuild_req = 1;        /* the new order, or the row put back */
        UI_ACTED();
    }
}

static void ui_row_grip(lv_obj_t *row, int idx, int n, ui_move_fn fn, uint16_t col) {
    ui_grip_t *g = lv_malloc(sizeof(*g));
    if (!g) return;
    g->fn = fn; g->idx = idx; g->n = n;
    lv_obj_t *grip = ui_plain(row);
    lv_obj_set_pos(grip, FB_W - 64, 0);
    lv_obj_set_size(grip, 64, ROW_H);
    lv_obj_add_flag(grip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(grip, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_t *ic = ui_label(grip, LV_SYMBOL_BARS, (lv_font_t *)&lv_font_montserrat_28, col);
    lv_obj_center(ic);
    lv_obj_add_event_cb(grip, ui_grip_event, LV_EVENT_ALL, g);
}

#include "ui_lists.c"
#include "ui_player.c"
#include "ui_settings.c"
#include "ui_overlays.c"

static void ui_build(void) {
    perf.t0 = us_now();
    ui_shown_t old = ui;
    memset(&ui, 0, sizeof(ui));
    ui.ident = ui_ident();
    ui.nav_n = nav_n;
    ui.screen = (int)screen;
    ui.status = status_strip_shown();

    switch (screen) {
    case SC_MENU:            ui_build_home(); break;
    case SC_MUSIC_MENU:      ui_build_music_menu(); break;
    case SC_ARTISTS:
    case SC_ALBUMS:          ui_build_library_list(); break;
    case SC_TRACKS:
        if (pod_list && !ab_list) ui_build_podcast_page();
        else if (ab_list)         ui_build_chapters();
        else                      ui_build_album_page();
        break;
    case SC_ARTIST_PAGE:     ui_build_artist_page(); break;
    case SC_PLAYING:         ui_build_playing(); break;
    case SC_QUEUE:           ui_build_queue(); break;
    case SC_PLAYLISTS:       ui_build_playlists(); break;
    case SC_RADIO:           ui_build_radio(); break;
    case SC_RADIO_RECORDINGS: ui_build_recordings(); break;
    case SC_AUDIOBOOKS:      ui_build_audiobooks(); break;
    case SC_PODCASTS:        ui_build_podcasts(); break;
    case SC_POD_SYNC:        ui_build_pod_sync(); break;
    case SC_POD_SETTINGS:    ui_build_pod_settings(); break;
    case SC_EQ:              ui_build_eq(); break;
    case SC_EQ_BANDS:        ui_build_eq_bands(); break;
    case SC_EQ_BAND:         ui_build_eq_band(); break;
    case SC_MSEB:            ui_build_mseb(); break;
    case SC_SETTINGS:        ui_build_settings(); break;
    case SC_SETTINGS_THEME:  ui_build_settings_theme(); break;
    case SC_SETTINGS_TIMEZONE: ui_build_settings_timezone(); break;
    case SC_SETTINGS_THEMEMODE: ui_build_settings_thememode(); break;
    case SC_SETTINGS_ABOUT:  ui_build_settings_about(); break;
    case SC_SETTINGS_WIFI:   ui_build_settings_wifi(); break;
    case SC_SETTINGS_BT:     ui_build_settings_bt(); break;
    case SC_SETTINGS_USB:    ui_build_settings_usb(); break;
    case SC_KEYBOARD:        ui_build_keyboard(); break;
    case SC_STATS:           ui_build_stats(); break;
    case SC_STATS_BATTERY:   ui_build_stats_battery(); break;
    case SC_STATS_STORAGE:   ui_build_stats_storage(); break;
    case SC_STATS_LISTEN:    ui_build_stats_listen(); break;
    }
    if (!ui.scr) {
        ui.scr = ui_new_screen(COL_BG);
        ui_empty_note(ui.scr, CONTENT_Y, "Not on LVGL yet");
    }
    if (ui.refresh) ui.refresh();
    ui_list_pad();
    ui_list_restore();

    /* Slide forward or back -- back to Home too, from Music or Podcasts --
     * a fade only for the home swipe, which jumps there from anywhere, and
     * no animation at all for the first screen. */
    lv_screen_load_anim_t anim = LV_SCREEN_LOAD_ANIM_NONE;
    if (old.scr && old.ident != ui.ident) {
        /* The queue drawer slides in over the page from the right and back
         * out again, as Libra's did. */
        if (screen == SC_QUEUE && qd_is_drawer && old.screen != SC_QUEUE)
            anim = ui_qd_opened ? LV_SCREEN_LOAD_ANIM_NONE : LV_SCREEN_LOAD_ANIM_OVER_LEFT;
        else if (old.screen == SC_QUEUE && screen != SC_QUEUE && qd_active)
            anim = LV_SCREEN_LOAD_ANIM_OUT_RIGHT;
        else if (screen == SC_MENU)
            anim = ui_home_jump ? LV_SCREEN_LOAD_ANIM_FADE_IN : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
        else if (screen == SC_ARTIST_PAGE && old.screen == SC_ARTIST_PAGE && ui_artist_slide_dir)
            anim = ui_artist_slide_dir < 0 ? LV_SCREEN_LOAD_ANIM_MOVE_LEFT : LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
        else if (nav_n < old.nav_n)  anim = LV_SCREEN_LOAD_ANIM_MOVE_RIGHT;
        else                         anim = LV_SCREEN_LOAD_ANIM_MOVE_LEFT;
    }
    ui_artist_slide_dir = 0;
    ui_qd_opened = 0;
    ui_home_jump = 0;
    /* A screen change made by a swipe animates in half the time a tap's
     * does: the finger has already done half the moving. */
    int ms = anim == LV_SCREEN_LOAD_ANIM_NONE ? 0
           : anim == LV_SCREEN_LOAD_ANIM_OVER_LEFT || anim == LV_SCREEN_LOAD_ANIM_OUT_RIGHT ? 220 : 180;
    if (ui_swipe_nav) ms /= 2;
    ui_swipe_nav = 0;
    lv_screen_load_anim(ui.scr, anim, (uint32_t)ms, 0, true);
    perf.built = us_now();
    perf.build_ms = (int)((perf.built - perf.t0) / 1000);
    perf.anim_ms = ms;
    perf.scr = (int)screen;
    perf.frames = 0; perf.draw_sum = 0; perf.draw_max = 0;
    perf.on = old.scr != NULL;
}

static void ui_sync(int dirty) {
    ui_art_sync_all();
    if (ui_rebuild_req || ui_ident() != ui.ident) {
        ui_rebuild_req = 0;
        ui_build();
        dirty = 1;               /* the strip and mini player follow the page */
    } else if (dirty && ui.refresh) {
        ui.refresh();
    }
    if (dirty || !ui.scr) {
        ui_status_refresh();
        ui_mini_refresh();
        ui_list_pad();
    }
    ui_overlays_sync(dirty);
}

/* ---- the touchscreen -------------------------------------------------------
 * Read here rather than by LVGL's evdev driver so the edge swipes can be told
 * apart from everything else while the finger is still down: Libra's own
 * rules (read_gesture()'s EDGE_ZONE, HOME_EDGE_ZONE, QD_EDGE_ZONE, the
 * status strip's pull), tested before LVGL is shown the touch at all.
 *
 * Back, home and closing quick settings act when the finger lifts, as in
 * Libra: once the touch has gone DRAG_MIN the gesture's way it latches --
 * LVGL's handling of it is cancelled, so no tap or scroll, and the hint grows --
 * and on release it acts only if the finger is then past the gesture's
 * travel, so pulling back before letting go cancels it. The queue drawer
 * and quick settings' pull follow the finger and settle on release. */
enum { EDGE_NONE, EDGE_BACK, EDGE_HOME, EDGE_QUEUE, EDGE_QS, EDGE_QS_CLOSE };

/* Whether the press lands on something that owns its own drags -- the
 * scrub bar, a slider, the sideways-scrolling title (marked
 * LV_OBJ_FLAG_USER_1 where they are made) -- which Libra keeps out of the
 * back and home swipes (scrub_active, eq_dragging, title_dragging): the
 * scrub bar and the sliders start at x = 24, inside EDGE_ZONE. */
static int ui_owns_drag(int x, int y) {
    lv_point_t p = { x, y };
    lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), &p);
    if (!o) o = lv_indev_search_obj(lv_screen_active(), &p);
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1)) return 1;
    return 0;
}


static int ui_edge_at(int x, int y) {
    if (qs_open && y < FB_H - 24) return EDGE_QS_CLOSE;
    if (sheet_open && y < FB_H - 24) return EDGE_NONE;
    /* Libra's zones: a back swipe may start anywhere in the left EDGE_ZONE
     * (100 px), home in the bottom HOME_EDGE_ZONE (40 px). Neither takes
     * the touch until it has travelled far enough the right way, so a tap
     * or scroll that starts in a zone is still an ordinary one. */
    if (x < EDGE_ZONE && screen != SC_MENU && screen != SC_KEYBOARD) return EDGE_BACK;
    if (y >= FB_H - HOME_EDGE_ZONE && screen != SC_MENU) return EDGE_HOME;
    if (x >= FB_W - QD_EDGE_ZONE && qd_pull_allowed()) return EDGE_QUEUE;
    if (y < STATUS_H && screen != SC_KEYBOARD) return EDGE_QS;
    return EDGE_NONE;
}

static void ui_edge_fire(int edge) {
    ui_swipe_nav = edge == EDGE_BACK || edge == EDGE_HOME;
    switch (edge) {
    case EDGE_BACK:
        go_back();
        break;
    case EDGE_HOME:
        sheet_open = 0;
        qs_open = 0;
        ui_home_jump = 1;
        screen = SC_MENU;
        reset_scroll();
        break;
    case EDGE_QUEUE:
        qd_begin_open(0);
        qd_finish_open();
        break;
    case EDGE_QS_CLOSE:
        qs_open = 0;
        break;
    }
    UI_ACTED();
}

static void ui_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    r1_input_event_t ev;
    while (ui_tfd >= 0 && read(ui_tfd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) tch.x = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) tch.y = ev.value;
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value) {
                tch.down = 1; tch.fired = 0; tch.swallow = 0; tch.latched = 0;
                ui_swipe_nav = 0;   /* a swipe that changed no screen does not carry over */
                tch.sx = tch.x; tch.sy = tch.y;
                tch.edge = locked ? EDGE_NONE : ui_edge_at(tch.x, tch.y);
                if ((tch.edge == EDGE_BACK || tch.edge == EDGE_HOME) && ui_owns_drag(tch.x, tch.y))
                    tch.edge = EDGE_NONE;
                tch.activity = 1;
            } else {
                tch.down = 0;
                perf.up = us_now();
                /* A latched back, home or close acts now, if the finger is
                 * past its travel (Libra's read_gesture() release tests). */
                if (tch.latched && !tch.fired) {
                    int dx = tch.x - tch.sx, dy = tch.y - tch.sy, go = 0;
                    switch (tch.edge) {
                    case EDGE_BACK:     go = dx > EDGE_TRAVEL && dx > abs(dy); break;
                    case EDGE_HOME:     go = -dy > HOME_EDGE_TRAVEL && -dy > abs(dx); break;
                    case EDGE_QS_CLOSE: go = -dy > QS_PULL && -dy > abs(dx); break;
                    }
                    tch.fired = 1;
                    if (go) ui_edge_fire(tch.edge);
                }
                if (ui_qs_pull >= 0) {          /* the pull let go */
                    if (ui_qs_pull >= QS_H / 3) qs_open = 1;
                    ui_qs_pull = -1;
                    UI_ACTED();
                }
                if (ui_qd_pull >= 0) {
                    if (ui_qd_pull >= FB_W / 3) { qd_begin_open(0); qd_finish_open(); ui_qd_opened = 1; }
                    ui_qd_pull = -1;
                    UI_ACTED();
                }
            }
        }
    }
    /* Back, home and close latch once the finger is DRAG_MIN the gesture's
     * way, and act on release, above. Libra's edge_active latches at 4 px
     * only for its hint: a release short of the travel is still a tap there
     * unless the finger moved DRAG_MIN, and cancelling LVGL's press at 4 px
     * would lose every tap in the 100 px zone that rolled a little. */
    if (tch.down && tch.edge != EDGE_NONE && !tch.fired) {
        int dx = tch.x - tch.sx, dy = tch.y - tch.sy;
        int latch = 0;
        switch (tch.edge) {
        case EDGE_BACK:     latch = dx > DRAG_MIN && dx > abs(dy); break;
        case EDGE_HOME:     latch = -dy > DRAG_MIN && -dy > abs(dx); break;
        case EDGE_QS_CLOSE: latch = -dy > DRAG_MIN && -dy > abs(dx); break;
        case EDGE_QUEUE:
            /* Not fired: the queue follows the finger in from the edge. */
            if (-dx > 20 && -dx > 2 * abs(dy)) {
                tch.fired = 1;
                tch.swallow = 1;
                lv_indev_reset(indev, NULL);
                ui_qd_pull = -dx;
                UI_ACTED();
            }
            break;
        case EDGE_QS:
            /* Not fired: once it is a pull, the panel follows the finger. */
            if (dy > QS_PULL && dy > 2 * abs(dx)) {
                tch.fired = 1;
                tch.swallow = 1;
                lv_indev_reset(indev, NULL);
                ui_qs_pull = dy;
                UI_ACTED();
            }
            break;
        }
        if (latch && !tch.latched) {
            tch.latched = 1;
            tch.swallow = 1;
            lv_indev_reset(indev, NULL);   /* no tap, no scroll from this touch */
        }
    }
    if (tch.down && tch.edge == EDGE_QUEUE && ui_qd_pull >= 0) {
        int d = tch.sx - tch.x;
        ui_qd_pull = d < 0 ? 0 : d > FB_W ? FB_W : d;
    }
    if (tch.down && tch.edge == EDGE_QS && ui_qs_pull >= 0) {
        int d = tch.y - tch.sy;
        ui_qs_pull = d < 0 ? 0 : d > QS_H ? QS_H : d;
    }
    data->point.x = tch.x;
    data->point.y = tch.y;
    data->state = (tch.down && !tch.swallow && !locked) ? LV_INDEV_STATE_PRESSED
                                                         : LV_INDEV_STATE_RELEASED;
}

static void ui_touch_open(void) {
    char node[32], path[48];
    if (!input_node_by_name("hyn_ts", node, sizeof(node)))
        snprintf(node, sizeof(node), "event1");
    snprintf(path, sizeof(path), "/dev/input/%s", node);
    ui_tfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (ui_tfd >= 0 && ioctl(ui_tfd, EVIOCGRAB, 1) < 0)
        mlog("[lvgl] touch grab failed: %s\n", strerror(errno));
    mlog("[lvgl] touch on %s (%s)\n", node, ui_tfd >= 0 ? "open" : strerror(errno));
    ui_touch = lv_indev_create();
    lv_indev_set_type(ui_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ui_touch, ui_touch_read);
    /* Libra's feel: a hold is HOLD_MS, and a tap may wander a little before
     * it counts as a scroll (its DRAG_MIN was 24px; LVGL's default is 10). */
    lv_indev_set_long_press_time(ui_touch, HOLD_MS);
    lv_indev_set_scroll_limit(ui_touch, 18);
}

/* ---- the two hooks music_entry() calls ----------------------------------- */

static uint32_t ui_tick_ms(void) {
    return (uint32_t)(us_now() / 1000);
}

/* Where a swipe's lag goes: finger up to the new screen built, the build
 * itself, built to its first frame on the panel, and the frames of the
 * transition with what each took to draw and flush. */
static void ui_perf_refr(lv_event_t *e) {
    uint64_t now = us_now();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) { perf.refr0 = now; return; }
    if (!perf.on || !perf.refr0) return;
    int d = (int)((now - perf.refr0) / 1000);
    perf.draw_sum += (uint64_t)d;
    if (d > perf.draw_max) perf.draw_max = d;
    if (!perf.frames++) perf.first = now;
    perf.last = now;
}

static void ui_perf_done(void) {
    if (!perf.on || us_now() - perf.built < (uint64_t)(perf.anim_ms + 120) * 1000) return;
    if (lv_anim_count_running() > 0) return;
    perf.on = 0;
    int up_ms = perf.up && perf.up < perf.t0 ? (int)((perf.t0 - perf.up) / 1000) : -1;
    mlog("[lvgl] screen %d: finger up -> build %d ms, build %d ms, first frame +%d ms, "
         "%d frames over %d ms (%d ms anim), draw avg %d max %d ms\n",
         perf.scr, up_ms, perf.build_ms,
         perf.frames ? (int)((perf.first - perf.built) / 1000) : -1,
         perf.frames, perf.frames ? (int)((perf.last - perf.first) / 1000) : 0, perf.anim_ms,
         perf.frames ? (int)(perf.draw_sum / (uint64_t)perf.frames) : 0, perf.draw_max);
    perf.up = 0;
}

/* A tap or swipe that changes the screen (not one that merely acted on
 * it): the main loop starts its next pass at once rather than at the end of
 * this 33 ms one, so the new screen is built up to a frame sooner. */
static int ui_lv_acted(void) {
    return ui_dirty && !locked && (ui_rebuild_req || ui_ident() != ui.ident);
}

static int ui_lv_open(void) {
    /* standalone_main.c calls music_entry() again whenever it returns: the
     * display, fonts and top layer outlive that. */
    static int opened = -1;
    if (opened >= 0) { ui_rebuild_req = 1; return opened; }
    lv_init();
    lv_tick_set_cb(ui_tick_ms);
    lv_display_t *disp = r1_display_create("/dev/fb0");
    if (!disp) return -1;
    lv_display_add_event_cb(disp, ui_perf_refr, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, ui_perf_refr, LV_EVENT_REFR_READY, NULL);
    if (ui_fonts_init() != 0) return -1;
    lv_obj_t *top = lv_layer_top();
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
    ui_touch_open();
    ui_status_build();
    ui_mini_build();
    ui_overlays_build();
    mlog("[lvgl] up\n");
    opened = r1_display_fd();
    return opened;
}

/* The sliver at the screen edge that grows and warms as a back, home or
 * queue swipe travels (Libra's draw_back_hint()/draw_home_hint()): barely
 * there for most of the way, the accent once letting go will act. */
static lv_obj_t *ui_hint;

static void ui_hint_update(void) {
    if (!ui_hint) {
        ui_hint = ui_plain(lv_layer_top());
        lv_obj_set_style_radius(ui_hint, 4, 0);
        lv_obj_add_flag(ui_hint, LV_OBJ_FLAG_HIDDEN);
    }
    /* Back and home only once latched, as Libra draws its hints only while
     * edge_active: a tap inside the 100 px zone shows nothing. */
    int edge = tch.down && !tch.fired ? tch.edge : EDGE_NONE;
    if ((edge == EDGE_BACK || edge == EDGE_HOME) && !tch.latched) edge = EDGE_NONE;
    if (edge != EDGE_BACK && edge != EDGE_HOME && edge != EDGE_QUEUE) {
        lv_obj_add_flag(ui_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int travel = edge == EDGE_BACK ? tch.x - tch.sx : edge == EDGE_QUEUE ? tch.sx - tch.x : tch.sy - tch.y;
    int full = edge == EDGE_HOME ? HOME_EDGE_TRAVEL : EDGE_TRAVEL;
    int t = travel * 256 / full;
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    int w = HINT_MAX_W * t / 256;
    if (w < 2) w = 2;
    int th = qc_popup_themed();
    ui_bg(ui_hint, mix565(th ? qc_line() : COL_LINE, th ? qc_accent() : COL_ACCENT, t * t / 256));
    if (edge == EDGE_HOME) {
        int left = tch.sx - HINT_H / 2;
        if (left < 0) left = 0;
        if (left + HINT_H > FB_W) left = FB_W - HINT_H;
        lv_obj_set_size(ui_hint, HINT_H, w);
        lv_obj_set_pos(ui_hint, left, FB_H - w);
    } else {
        int top = tch.y - HINT_H / 2;
        if (top < CONTENT_Y) top = CONTENT_Y;
        if (top + HINT_H > FB_H) top = FB_H - HINT_H;
        lv_obj_set_size(ui_hint, w, HINT_H);
        lv_obj_set_pos(ui_hint, edge == EDGE_BACK ? 0 : FB_W - w, top);
    }
    lv_obj_remove_flag(ui_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui_hint);
}

/* Between main-loop passes: LVGL's own timers only (touch, animations,
 * rendering), none of Libra's state. */
static uint32_t ui_lv_pump(void) {
    if (locked) return 100;
    ui_hint_update();
    uint32_t wait = lv_timer_handler();
    ui_perf_done();
    if (tch.down || lv_anim_count_running() > 0) wait = 10;
    return wait;
}

static uint32_t ui_lv_frame(int dirty) {
    static int was_locked;
    if (locked) { was_locked = 1; return 100; }
    if (was_locked) {
        was_locked = 0;
        dirty = 1;
        lv_obj_invalidate(lv_screen_active());
    }
    if (ui_dirty) { dirty = 1; ui_dirty = 0; }
    ui_sync(dirty);
    ui_hint_update();
    uint32_t wait = lv_timer_handler();
    /* A finger down or something animating: come back for the next frame. */
    if (tch.down || lv_anim_count_running() > 0) wait = 10;
    return wait;
}
