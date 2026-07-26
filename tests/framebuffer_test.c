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
