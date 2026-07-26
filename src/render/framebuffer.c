/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "render/framebuffer.h"

/* 5x7 row bitmaps: digits followed by A-Z. */
static const uint8_t k_font[36][7] = {
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14},{14,17,17,15,1,1,14},
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14},{7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};

static void put_pixel(lite_framebuffer_t *fb, int x, int y, uint16_t color)
{
    if (fb && fb->pixels && x >= 0 && y >= 0 &&
        x < fb->width && y < fb->height) {
        fb->pixels[y * fb->stride + x] = color;
    }
}

void lite_fb_clear(lite_framebuffer_t *fb, uint16_t color)
{
    lite_fb_rect(fb, 0, 0, fb ? fb->width : 0, fb ? fb->height : 0, color);
}

void lite_fb_rect(
    lite_framebuffer_t *fb,
    int x, int y, int width, int height, uint16_t color
)
{
    int right;
    int bottom;
    int row;
    int column;
    if (!fb || !fb->pixels || width <= 0 || height <= 0) {
        return;
    }
    right = x + width;
    bottom = y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > fb->width) right = fb->width;
    if (bottom > fb->height) bottom = fb->height;
    for (row = y; row < bottom; ++row) {
        for (column = x; column < right; ++column) {
            fb->pixels[row * fb->stride + column] = color;
        }
    }
}

void lite_fb_blend_rect(
    lite_framebuffer_t *fb,
    int x, int y, int width, int height, uint16_t color
)
{
    int right;
    int bottom;
    int row;
    int column;
    uint16_t source = (uint16_t)((color & 0xf7deu) >> 1);
    if (!fb || !fb->pixels || width <= 0 || height <= 0) {
        return;
    }
    right = x + width;
    bottom = y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > fb->width) right = fb->width;
    if (bottom > fb->height) bottom = fb->height;
    for (row = y; row < bottom; ++row) {
        for (column = x; column < right; ++column) {
            uint16_t *pixel = &fb->pixels[row * fb->stride + column];
            *pixel = (uint16_t)(((*pixel & 0xf7deu) >> 1) + source);
        }
    }
}

void lite_fb_frame(
    lite_framebuffer_t *fb,
    int x, int y, int width, int height, uint16_t color
)
{
    lite_fb_rect(fb, x, y, width, 1, color);
    lite_fb_rect(fb, x, y + height - 1, width, 1, color);
    lite_fb_rect(fb, x, y, 1, height, color);
    lite_fb_rect(fb, x + width - 1, y, 1, height, color);
}

void lite_fb_line(
    lite_framebuffer_t *fb,
    int x0, int y0, int x1, int y1, uint16_t color
)
{
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy_abs = y1 >= y0 ? y1 - y0 : y0 - y1;
    int dy = -dy_abs;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        int twice;
        put_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static const uint8_t *glyph(char character)
{
    static const uint8_t colon[7] = {0,4,4,0,4,4,0};
    static const uint8_t dot[7] = {0,0,0,0,0,4,4};
    static const uint8_t dash[7] = {0,0,0,31,0,0,0};
    static const uint8_t slash[7] = {1,2,2,4,8,8,16};
    static const uint8_t question[7] = {14,17,1,2,4,0,4};
    if (character >= '0' && character <= '9') {
        return k_font[(uint32_t)(character - '0')];
    }
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }
    if (character >= 'A' && character <= 'Z') {
        return k_font[10u + (uint32_t)(character - 'A')];
    }
    if (character == ':') return colon;
    if (character == '.') return dot;
    if (character == '-') return dash;
    if (character == '/') return slash;
    return question;
}

static void draw_glyph(
    lite_framebuffer_t *fb,
    int x, int y, char character, int scale, uint16_t color
)
{
    const uint8_t *rows;
    int row;
    int column;
    if (character == ' ') {
        return;
    }
    rows = glyph(character);
    for (row = 0; row < 7; ++row) {
        for (column = 0; column < 5; ++column) {
            if ((rows[row] & (uint8_t)(1u << (4 - column))) != 0u) {
                lite_fb_rect(
                    fb, x + column * scale, y + row * scale,
                    scale, scale, color
                );
            }
        }
    }
}

void lite_fb_text(
    lite_framebuffer_t *fb,
    int x, int y, const char *text, int scale, uint16_t color
)
{
    if (!text || scale <= 0) {
        return;
    }
    while (*text) {
        draw_glyph(fb, x, y, *text++, scale, color);
        x += 6 * scale;
    }
}

void lite_fb_u32(
    lite_framebuffer_t *fb,
    int x, int y, uint32_t value, int scale, uint16_t color
)
{
    char digits[10];
    uint32_t count = 0u;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u && count < sizeof(digits));
    while (count != 0u) {
        draw_glyph(fb, x, y, digits[--count], scale, color);
        x += 6 * scale;
    }
}

void lite_fb_upscale_in_place(
    lite_framebuffer_t *fb, int source_width, int source_height
)
{
    int y;
    if (!fb || !fb->pixels || source_width <= 0 || source_height <= 0 ||
        source_width > fb->width || source_height > fb->height) {
        return;
    }
    /*
     * Walk backwards so a larger destination never overwrites a source
     * pixel that has not been consumed yet.  The source uses fb->stride in
     * the upper-left corner of the same allocation.
     */
    for (y = fb->height - 1; y >= 0; --y) {
        int source_y = (y * source_height) / fb->height;
        int x;
        for (x = fb->width - 1; x >= 0; --x) {
            int source_x = (x * source_width) / fb->width;
            fb->pixels[y * fb->stride + x] =
                fb->pixels[source_y * fb->stride + source_x];
        }
    }
}
