/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_FRAMEBUFFER_H
#define CS15_LITE_FRAMEBUFFER_H

#include <stdint.h>

typedef struct lite_framebuffer {
    uint16_t *pixels;
    int width;
    int height;
    int stride;
} lite_framebuffer_t;

void lite_fb_clear(lite_framebuffer_t *fb, uint16_t color);
void lite_fb_rect(
    lite_framebuffer_t *fb,
    int x, int y, int width, int height, uint16_t color
);
void lite_fb_blend_rect(
    lite_framebuffer_t *fb,
    int x, int y, int width, int height, uint16_t color
);
void lite_fb_frame(
    lite_framebuffer_t *fb,
    int x, int y, int width, int height, uint16_t color
);
void lite_fb_line(
    lite_framebuffer_t *fb,
    int x0, int y0, int x1, int y1, uint16_t color
);
void lite_fb_text(
    lite_framebuffer_t *fb,
    int x, int y, const char *text, int scale, uint16_t color
);
void lite_fb_u32(
    lite_framebuffer_t *fb,
    int x, int y, uint32_t value, int scale, uint16_t color
);
void lite_fb_upscale_in_place(
    lite_framebuffer_t *fb, int source_width, int source_height
);

#endif
