#include "render/framebuffer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint16_t pixels[16 * 12] = {0};
    lite_framebuffer_t fb = {pixels, 16, 12, 16};

    lite_fb_clear(&fb, 1u);
    assert(pixels[0] == 1u);
    assert(pixels[16 * 12 - 1] == 1u);
    lite_fb_rect(&fb, 2, 3, 4, 2, 7u);
    assert(pixels[3 * 16 + 2] == 7u);
    assert(pixels[4 * 16 + 5] == 7u);
    assert(pixels[5 * 16 + 5] == 1u);
    lite_fb_rect(&fb, 1, 5, 7, 2, 8u);
    assert(pixels[5 * 16] == 1u);
    assert(pixels[5 * 16 + 1] == 8u);
    assert(pixels[6 * 16 + 7] == 8u);
    assert(pixels[6 * 16 + 8] == 1u);
    lite_fb_rect(&fb, -8, 2, 3, 2, 99u);
    lite_fb_rect(&fb, 20, 2, 3, 2, 99u);
    assert(pixels[2 * 16] == 1u);
    assert(pixels[2 * 16 + 15] == 1u);
    pixels[6 * 16 + 6] = 0xffffu;
    lite_fb_blend_rect(&fb, 6, 6, 1, 1, 0u);
    assert(pixels[6 * 16 + 6] == 0x7befu);
    lite_fb_line(&fb, 0, 0, 3, 3, 9u);
    assert(pixels[0] == 9u);
    assert(pixels[1 * 16 + 1] == 9u);
    assert(pixels[3 * 16 + 3] == 9u);
    lite_fb_frame(&fb, -2, -2, 6, 6, 11u);
    assert(pixels[3] == 11u);
    assert(pixels[3 * 16] == 11u);
    lite_fb_clear(&fb, 0u);
    lite_fb_text(&fb, 0, 0, "_+", 1, 13u);
    assert(pixels[6 * 16] == 13u);
    assert(pixels[6 * 16 + 4] == 13u);
    assert(pixels[0] == 0u);
    assert(pixels[3 * 16 + 6] == 13u);
    assert(pixels[3 * 16 + 10] == 13u);
    assert(pixels[1 * 16 + 8] == 13u);
    {
        uint16_t text_pixels[64 * 8] = {0};
        lite_framebuffer_t text_fb = {
            text_pixels, 64, 8, 64
        };
        lite_fb_text(&text_fb, 0, 0, "YOU>BOT 7", 1, 15u);
        /*
         * The kill-feed separator is a real greater-than glyph. The former
         * question-mark fallback would light columns 1..3 on its first row.
         */
        assert(text_pixels[18] == 15u);
        assert(text_pixels[19] == 0u);
        assert(text_pixels[6 * 64 + 18] == 15u);
    }
    {
        uint16_t source[12 * 9];
        int y;
        int x;
        for (y = 0; y < 9; ++y) {
            for (x = 0; x < 12; ++x) {
                uint16_t value = (uint16_t)(y * 100 + x);
                source[y * 12 + x] = value;
                pixels[y * 16 + x] = value;
            }
        }
        lite_fb_upscale_in_place(&fb, 12, 9);
        for (y = 0; y < 12; ++y) {
            for (x = 0; x < 16; ++x) {
                assert(
                    pixels[y * 16 + x] ==
                    source[(y * 9 / 12) * 12 + (x * 12 / 16)]
                );
            }
        }
    }
    puts("framebuffer_test: PASS");
    return 0;
}
