/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "render/framebuffer.h"

typedef uint32_t framebuffer_alias_u32
    __attribute__((__may_alias__));

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
    if (!fb || !fb->pixels || width <= 0 || height <= 0) {
        return;
    }
    right = x + width;
    bottom = y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > fb->width) right = fb->width;
    if (bottom > fb->height) bottom = fb->height;
    if (right <= x || bottom <= y) {
        return;
    }
    for (row = y; row < bottom; ++row) {
        uint16_t *pixel = fb->pixels + row * fb->stride + x;
        int remaining = right - x;
        uint32_t pair =
            (uint32_t)color | ((uint32_t)color << 16);
        if ((((uintptr_t)pixel) & 2u) != 0u &&
            remaining > 0) {
            *pixel++ = color;
            --remaining;
        }
        {
            framebuffer_alias_u32 *words =
                (framebuffer_alias_u32 *)pixel;
            while (remaining >= 16) {
                words[0] = pair;
                words[1] = pair;
                words[2] = pair;
                words[3] = pair;
                words[4] = pair;
                words[5] = pair;
                words[6] = pair;
                words[7] = pair;
                words += 8;
                remaining -= 16;
            }
            while (remaining >= 2) {
                *words++ = pair;
                remaining -= 2;
            }
            pixel = (uint16_t *)words;
        }
        if (remaining != 0) {
            *pixel = color;
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
    static const uint8_t exclamation[7] = {4,4,4,4,4,0,4};
    static const uint8_t quote[7] = {10,10,10,0,0,0,0};
    static const uint8_t hash[7] = {10,31,10,10,31,10,0};
    static const uint8_t dollar[7] = {4,15,20,14,5,30,4};
    static const uint8_t percent[7] = {17,2,4,8,17,0,0};
    static const uint8_t ampersand[7] = {12,18,20,8,21,18,13};
    static const uint8_t apostrophe[7] = {4,4,8,0,0,0,0};
    static const uint8_t left_parenthesis[7] = {2,4,8,8,8,4,2};
    static const uint8_t right_parenthesis[7] = {8,4,2,2,2,4,8};
    static const uint8_t asterisk[7] = {0,21,14,31,14,21,0};
    static const uint8_t plus[7] = {0,4,4,31,4,4,0};
    static const uint8_t comma[7] = {0,0,0,0,0,4,8};
    static const uint8_t dash[7] = {0,0,0,31,0,0,0};
    static const uint8_t dot[7] = {0,0,0,0,0,4,4};
    static const uint8_t slash[7] = {1,2,2,4,8,8,16};
    static const uint8_t colon[7] = {0,4,4,0,4,4,0};
    static const uint8_t semicolon[7] = {0,4,4,0,4,4,8};
    static const uint8_t less[7] = {2,4,8,16,8,4,2};
    static const uint8_t equal[7] = {0,31,0,31,0,0,0};
    static const uint8_t greater[7] = {16,8,4,2,4,8,16};
    static const uint8_t question[7] = {14,17,1,2,4,0,4};
    static const uint8_t at[7] = {14,17,23,21,23,16,14};
    static const uint8_t left_bracket[7] = {14,8,8,8,8,8,14};
    static const uint8_t backslash[7] = {16,8,8,4,2,2,1};
    static const uint8_t right_bracket[7] = {14,2,2,2,2,2,14};
    static const uint8_t caret[7] = {4,10,17,0,0,0,0};
    static const uint8_t underscore[7] = {0,0,0,0,0,0,31};
    static const uint8_t backtick[7] = {8,4,2,0,0,0,0};
    static const uint8_t left_brace[7] = {2,4,4,8,4,4,2};
    static const uint8_t pipe[7] = {4,4,4,4,4,4,4};
    static const uint8_t right_brace[7] = {8,4,4,2,4,4,8};
    static const uint8_t tilde[7] = {0,0,9,22,0,0,0};
    if (character >= '0' && character <= '9') {
        return k_font[(uint32_t)(character - '0')];
    }
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - 'a' + 'A');
    }
    if (character >= 'A' && character <= 'Z') {
        return k_font[10u + (uint32_t)(character - 'A')];
    }
    switch (character) {
        case '!': return exclamation;
        case '"': return quote;
        case '#': return hash;
        case '$': return dollar;
        case '%': return percent;
        case '&': return ampersand;
        case '\'': return apostrophe;
        case '(': return left_parenthesis;
        case ')': return right_parenthesis;
        case '*': return asterisk;
        case '+': return plus;
        case ',': return comma;
        case '-': return dash;
        case '.': return dot;
        case '/': return slash;
        case ':': return colon;
        case ';': return semicolon;
        case '<': return less;
        case '=': return equal;
        case '>': return greater;
        case '?': return question;
        case '@': return at;
        case '[': return left_bracket;
        case '\\': return backslash;
        case ']': return right_bracket;
        case '^': return caret;
        case '_': return underscore;
        case '`': return backtick;
        case '{': return left_brace;
        case '|': return pipe;
        case '}': return right_brace;
        case '~': return tilde;
        default: return question;
    }
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
