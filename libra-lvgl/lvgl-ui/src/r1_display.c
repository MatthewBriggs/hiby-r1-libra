/* r1_display.c — LVGL display driver for the R1's panel.
 *
 * /dev/fb0 is 480x800 RGB565 with a virtual height of 1600: two pages, and
 * FBIOPAN_DISPLAY picks which one the panel scans out. Libra's own UI draws
 * a whole frame into the hidden page and pans to it, so nothing is ever seen
 * half-drawn. LVGL's own fbdev driver instead copies dirty areas onto the
 * page being shown, which can tear mid-scroll.
 *
 * So the two pages ARE LVGL's two draw buffers, in DIRECT mode: LVGL renders
 * only what changed straight into the hidden page (and copies the previous
 * frame's changes across from the other page first, so the hidden page is
 * never stale), and the last flush of a frame pans to it. No extra memory
 * and no copy of the frame.
 */
#include "r1_display.h"

#if LV_LIBRA_R1

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define R1_W 480
#define R1_H 800

static int fbfd = -1;
static uint8_t *fbmem;
static size_t page_bytes;
static struct fb_var_screeninfo vinfo;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
    (void)area;
    if (lv_display_flush_is_last(disp)) {
        vinfo.yoffset = (px == fbmem) ? 0 : R1_H;
        if (ioctl(fbfd, FBIOPAN_DISPLAY, &vinfo) < 0)
            LV_LOG_WARN("pan failed: %s", strerror(errno));
    }
    lv_display_flush_ready(disp);
}

int r1_display_fd(void) { return fbfd; }

/* The page the panel is showing now: LVGL's last finished frame. */
const uint16_t *r1_display_front(void) {
    return (const uint16_t *)(fbmem + (vinfo.yoffset ? page_bytes : 0));
}

lv_display_t *r1_display_create(const char *path) {
    struct fb_fix_screeninfo finfo;
    fbfd = open(path, O_RDWR | O_CLOEXEC);
    if (fbfd < 0) {
        fprintf(stderr, "r1_display: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "r1_display: screeninfo: %s\n", strerror(errno));
        return NULL;
    }
    if (vinfo.xres != R1_W || vinfo.yres != R1_H || vinfo.bits_per_pixel != 16 ||
        vinfo.yres_virtual < 2 * R1_H || finfo.line_length != R1_W * 2) {
        fprintf(stderr, "r1_display: unexpected mode %ux%u (virtual %u) %u bpp, line %u\n",
                vinfo.xres, vinfo.yres, vinfo.yres_virtual, vinfo.bits_per_pixel,
                finfo.line_length);
        return NULL;
    }
    page_bytes = (size_t)finfo.line_length * R1_H;
    fbmem = mmap(NULL, page_bytes * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbmem == MAP_FAILED) {
        fprintf(stderr, "r1_display: mmap: %s\n", strerror(errno));
        return NULL;
    }

    lv_display_t *disp = lv_display_create(R1_W, R1_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fbmem, fbmem + page_bytes, page_bytes,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    return disp;
}

#endif /* LV_LIBRA_R1 */
