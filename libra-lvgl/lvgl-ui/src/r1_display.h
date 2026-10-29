#ifndef R1_DISPLAY_H
#define R1_DISPLAY_H

#include "lvgl.h"

/* The R1's panel as an LVGL display, page-flipped (see r1_display.c). */
lv_display_t *r1_display_create(const char *path);
int r1_display_fd(void);   /* for FBIOBLANK */
const uint16_t *r1_display_front(void);   /* 480x800 RGB565, what is on the panel */

#endif
