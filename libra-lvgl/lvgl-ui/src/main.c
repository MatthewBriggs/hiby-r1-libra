/* main.c — Libra on LVGL: the R1 build (framebuffer + touchscreen) and the
 * Mac simulator (an SDL window) share everything but this file's setup.
 *
 *   R1:   libra_lvgl [FONT]     default /usr/data/podcast_res/font.ttf
 *   Mac:  libra_lvgl_sim [FONT] default ../fonts/font.ttf
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "ui.h"

#if LV_LIBRA_R1
#include "r1_display.h"
#define DEFAULT_FONT "/usr/data/podcast_res/font.ttf"
#else
#define DEFAULT_FONT "../fonts/font.ttf"
#endif

static uint32_t tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

#if LV_LIBRA_R1
/* The touchscreen by name, as app/music_hook.c finds it: the event numbers
 * move when Bluetooth headsets add input devices. */
static int input_node_by_name(const char *want, char *out, size_t n) {
    for (int i = 0; i < 16; i++) {
        char p[64], name[64] = "";
        snprintf(p, sizeof(p), "/sys/class/input/event%d/device/name", i);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
        if (strcmp(name, want) == 0) {
            snprintf(out, n, "/dev/input/event%d", i);
            return 1;
        }
    }
    return 0;
}
#endif

/* LIBRA_SHOT=file.ppm [LIBRA_SHOT_MS=n]: write the screen as a PPM after n
 * ms (default 1500) and exit -- for checking a layout without watching it. */
static void shot_cb(lv_timer_t *t) {
    const char *path = lv_timer_get_user_data(t);
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    FILE *f = snap ? fopen(path, "wb") : NULL;
    if (f) {
        int w = snap->header.w, h = snap->header.h;
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++) {
            const uint8_t *row = snap->data + (size_t)y * snap->header.stride;
            for (int x = 0; x < w; x++) {   /* LVGL's RGB888 is B, G, R */
                uint8_t px[3] = { row[x * 3 + 2], row[x * 3 + 1], row[x * 3] };
                fwrite(px, 1, 3, f);
            }
        }
        fclose(f);
        printf("libra_lvgl: wrote %s\n", path);
    }
    exit(f ? 0 : 1);
}

int main(int argc, char **argv) {
    const char *font = argc > 1 ? argv[1] : DEFAULT_FONT;
    lv_init();
    lv_tick_set_cb(tick_ms);

#if LV_LIBRA_R1
    if (!r1_display_create("/dev/fb0")) return 1;
    char touch[32];
    if (!input_node_by_name("hyn_ts", touch, sizeof(touch)))
        snprintf(touch, sizeof(touch), "/dev/input/event1");
    printf("libra_lvgl: touch on %s\n", touch);
    lv_indev_t *ts = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch);
    if (!ts) fprintf(stderr, "libra_lvgl: no touch on %s\n", touch);
#else
    lv_display_t *disp = lv_sdl_window_create(480, 800);
    lv_sdl_window_set_title(disp, "Libra (LVGL)");
    lv_sdl_mouse_create();
#endif

    if (ui_init(font) != 0) return 1;

    const char *shot = getenv("LIBRA_SHOT");
    if (shot) {
        const char *ms = getenv("LIBRA_SHOT_MS");
        lv_timer_create(shot_cb, ms ? (uint32_t)atoi(ms) : 1500, (void *)shot);
    }

    for (;;) {
        uint32_t idle = lv_timer_handler();
        if (idle > 16) idle = 16;
        usleep(idle * 1000);
    }
}
