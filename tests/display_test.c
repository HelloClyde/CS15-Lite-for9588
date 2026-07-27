#include "platform/display.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define SOURCE_WIDTH 320u
#define SOURCE_HEIGHT 240u
#define DESTINATION_WIDTH 240u
#define DESTINATION_HEIGHT 320u

static uint16_t source[SOURCE_WIDTH * SOURCE_HEIGHT];
static uint16_t destination[DESTINATION_WIDTH * DESTINATION_HEIGHT];
static uint16_t scanout[DESTINATION_WIDTH * DESTINATION_HEIGHT];

static uint16_t sample(uint32_t x, uint32_t y)
{
    return (uint16_t)(((x * 251u) ^ (y * 509u)) & 0xffffu);
}

static void fill_source(void)
{
    uint32_t y;
    for (y = 0u; y < SOURCE_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0u; x < SOURCE_WIDTH; ++x) {
            source[y * SOURCE_WIDTH + x] = sample(x, y);
        }
    }
}

static void verify_ccw(void)
{
    uint32_t y;
    lite_display_copy_landscape_rgb565(source, destination, 0);
    for (y = 0u; y < DESTINATION_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0u; x < DESTINATION_WIDTH; ++x) {
            assert(destination[y * DESTINATION_WIDTH + x] ==
                   sample(SOURCE_WIDTH - 1u - y, x));
        }
    }
}

static void verify_ccw_rotated_180(void)
{
    uint32_t y;
    lite_display_copy_landscape_rgb565(source, destination, 1);
    for (y = 0u; y < DESTINATION_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0u; x < DESTINATION_WIDTH; ++x) {
            assert(destination[y * DESTINATION_WIDTH + x] ==
                   sample(y, SOURCE_HEIGHT - 1u - x));
        }
    }
}

static void verify_contiguous_submit(void)
{
    uint32_t index;
    lite_display_copy_landscape_rgb565(source, scanout, 0);
    lite_display_copy_portrait_rgb565(scanout, destination, 0);
    for (index = 0u;
         index < DESTINATION_WIDTH * DESTINATION_HEIGHT; ++index) {
        assert(destination[index] == scanout[index]);
    }
    lite_display_copy_portrait_rgb565(scanout, destination, 1);
    for (index = 0u;
         index < DESTINATION_WIDTH * DESTINATION_HEIGHT; ++index) {
        assert(destination[index] ==
               scanout[
                   DESTINATION_WIDTH * DESTINATION_HEIGHT - 1u - index
               ]);
    }
}

static void verify_direct_submit(void)
{
    uint32_t index;
    uint32_t y;
    lite_display_copy_landscape_rgb565(source, scanout, 0);
    lite_display_present_landscape_rgb565(source, destination, 0);
    for (index = 0u;
         index < DESTINATION_WIDTH * DESTINATION_HEIGHT; ++index) {
        assert(destination[index] == scanout[index]);
    }
    lite_display_present_landscape_rgb565(source, destination, 1);
    for (y = 0u; y < DESTINATION_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0u; x < DESTINATION_WIDTH; ++x) {
            assert(destination[y * DESTINATION_WIDTH + x] ==
                   sample(y, SOURCE_HEIGHT - 1u - x));
        }
    }
}

int main(void)
{
    fill_source();
    verify_ccw();
    verify_ccw_rotated_180();
    verify_contiguous_submit();
    verify_direct_submit();
    puts("display_test: PASS");
    return 0;
}
