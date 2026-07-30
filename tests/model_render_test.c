/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "render/model.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIDTH 320
#define HEIGHT 240

static uint16_t frame[WIDTH * HEIGHT];
static uint16_t depth[WIDTH * HEIGHT] __attribute__((aligned(4)));
static uint16_t palette[256];
static uint8_t texels[16 * 16];
static uint8_t vertices[3 * 8];
static uint8_t triangles[8];

int16_t c15_sin_q14(uint8_t angle)
{
    (void)angle;
    return 0;
}

int16_t c15_cos_q14(uint8_t angle)
{
    (void)angle;
    return 16384;
}

static void write_i16(uint8_t *output, int16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)((uint16_t)value >> 8);
}

static void write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void set_vertex(
    uint32_t index,
    int16_t z_q4,
    int16_t negated_x_q4,
    int16_t y_plus_16_q4,
    uint8_t u,
    uint8_t v
)
{
    uint8_t *vertex = vertices + index * 8u;
    write_i16(vertex, z_q4);
    write_i16(vertex + 2, negated_x_q4);
    write_i16(vertex + 4, y_plus_16_q4);
    vertex[6] = u;
    vertex[7] = v;
}

int main(void)
{
    uint32_t index;
    c15_model_t model;
    c15_model_view_t view = {160, 120, 160, 0, 8};
    c15_model_render_stats_t stats;
    lite_framebuffer_t framebuffer = {
        frame, WIDTH, HEIGHT, WIDTH
    };

    memset(&model, 0, sizeof(model));
    memset(frame, 0, sizeof(frame));
    memset(depth, 0, sizeof(depth));
    memset(palette, 0, sizeof(palette));
    for (index = 0u; index < 256u; ++index) {
        texels[index] = (uint8_t)index;
        palette[index] = (uint16_t)(0x2000u + index);
    }

    /* Projects to a visible CCW triangle near (120,100)-(200,100)-(160,180). */
    set_vertex(0u, 100, 25, 29, 0u, 0u);
    set_vertex(1u, 100, -25, 29, 15u, 0u);
    set_vertex(2u, 100, 0, -21, 8u, 15u);
    write_u16(triangles, 0u);
    write_u16(triangles + 2, 1u);
    write_u16(triangles + 4, 2u);
    triangles[6] = 0u;
    triangles[7] = 0u;

    model.vertices = vertices;
    model.triangles = triangles;
    model.vertex_count = 3u;
    model.triangle_count = 1u;
    model.texture_count = 1u;
    model.textures[0].width = 16u;
    model.textures[0].height = 16u;
    model.textures[0].width_mask = 15u;
    model.textures[0].height_mask = 15u;
    model.textures[0].width_reciprocal = 4096u;
    model.textures[0].height_reciprocal = 4096u;
    model.textures[0].palette = palette;
    model.textures[0].pixels = texels;
    model.loaded = 1;

    c15_render_view_model(
        &model, &framebuffer, depth, &view, 0, 0, &stats
    );
    assert(stats.triangles == 1u);
    assert(stats.pixels > 1000u);
    /* Affine UV at (160,120) is approximately (7,3). */
    assert(frame[120 * WIDTH + 160] == palette[3u * 16u + 7u]);
    assert(depth[120 * WIDTH + 160] < 0xffffu);
    /*
     * View depth is cleared lazily in touched 8x8 tiles. World depth
     * outside the weapon footprint remains intact until the next frame.
     */
    assert(depth[0] == 0u);
    depth[120 * WIDTH + 160] = 0u;
    frame[120 * WIDTH + 160] = 0u;
    c15_render_view_model(
        &model, &framebuffer, depth, &view, 0, 0, &stats
    );
    assert(frame[120 * WIDTH + 160] == palette[3u * 16u + 7u]);
    puts("model_render_test: PASS");
    return 0;
}
