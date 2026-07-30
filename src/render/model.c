/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "render/model.h"

#include "bda_memory.h"
#include "platform/bbk9588.h"

/* M19 historical maximum: v_elite has 1402 bone-safe compact vertices. */
#define MODEL_MAX_PROJECTED_VERTICES 1408u
#define MODEL_WORLD_NEAR 8
#define MODEL_WORLD_DEFAULT_FOCAL ((int)LITE_VIEW_WIDTH / 2)
#define MODEL_ENTITY_CULL_RADIUS_Q4 (128 * 16)
#define MODEL_DEPTH_TILE_SHIFT 3
#define MODEL_DEPTH_TILE_SIZE (1 << MODEL_DEPTH_TILE_SHIFT)
#define MODEL_DEPTH_TILE_COLUMNS \
    (LITE_VIEW_WIDTH / MODEL_DEPTH_TILE_SIZE)
#define MODEL_DEPTH_TILE_ROWS \
    (LITE_VIEW_HEIGHT / MODEL_DEPTH_TILE_SIZE)

typedef struct model_projected_vertex {
    int16_t x;
    int16_t y;
    int16_t z;
    uint8_t u;
    uint8_t v;
} model_projected_vertex_t;

typedef struct model_view_vertex {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t u;
    int32_t v;
} model_view_vertex_t;

typedef struct model_scan_edge {
    int32_t x;
    int32_t dx;
} model_scan_edge_t;

typedef struct model_scan_gradient {
    int32_t du_dx;
    int32_t du_dy;
    int32_t dv_dx;
    int32_t dv_dy;
    int32_t dz_dx;
    int32_t dz_dy;
} model_scan_gradient_t;

static model_projected_vertex_t
    g_projected_vertices[MODEL_MAX_PROJECTED_VERTICES];
static uint16_t g_view_depth_tile_stamps[
    MODEL_DEPTH_TILE_COLUMNS * MODEL_DEPTH_TILE_ROWS
];
static uint16_t g_view_depth_generation;
static uint8_t g_view_depth_lazy;

static uint32_t fractional_divide_q16(
    uint32_t remainder, uint32_t denominator
)
{
    uint32_t result = 0u;
    uint32_t bit;
    for (bit = 0u; bit < 16u; ++bit) {
        result <<= 1;
        /*
         * Compare before doubling so remainder never overflows uint32_t.
         * This is the fractional half of exact restoring division.
         */
        if (remainder >= denominator - remainder) {
            remainder -= denominator - remainder;
            result |= 1u;
        } else {
            remainder += remainder;
        }
    }
    return result;
}

static int32_t fixed_divide_q16(
    int64_t numerator, uint32_t denominator
)
{
    int negative;
    uint32_t magnitude;
    uint32_t whole;
    uint32_t remainder;
    uint32_t result;
    if (denominator == 0u) {
        return 0;
    }
    if (numerator < -2147483647LL ||
        numerator > 2147483647LL) {
        return (int32_t)((numerator * 65536) / denominator);
    }
    negative = numerator < 0;
    magnitude = negative ?
        (uint32_t)(-numerator) : (uint32_t)numerator;
    whole = magnitude / denominator;
    if (whole > 32767u) {
        return (int32_t)((numerator * 65536) / denominator);
    }
    remainder = magnitude - whole * denominator;
    result = (whole << 16) |
        fractional_divide_q16(remainder, denominator);
    return negative ? (int32_t)(0u - result) : (int32_t)result;
}

static void begin_lazy_view_depth(void)
{
    ++g_view_depth_generation;
    if (g_view_depth_generation == 0u) {
        bda_memset(
            g_view_depth_tile_stamps, 0,
            sizeof(g_view_depth_tile_stamps)
        );
        g_view_depth_generation = 1u;
    }
    g_view_depth_lazy = 1u;
}

static void clear_view_depth_span(
    uint16_t *depth, int y, int x0, int x1
)
{
    uint32_t tile_y = (uint32_t)y >> MODEL_DEPTH_TILE_SHIFT;
    uint32_t first_tile =
        (uint32_t)x0 >> MODEL_DEPTH_TILE_SHIFT;
    uint32_t last_tile =
        (uint32_t)x1 >> MODEL_DEPTH_TILE_SHIFT;
    uint32_t tile;
    for (tile = first_tile; tile <= last_tile; ++tile) {
        uint32_t stamp_index =
            tile_y * MODEL_DEPTH_TILE_COLUMNS + tile;
        uint32_t row;
        uint32_t tile_x;
        if (g_view_depth_tile_stamps[stamp_index] ==
            g_view_depth_generation) {
            continue;
        }
        g_view_depth_tile_stamps[stamp_index] =
            g_view_depth_generation;
        tile_x = tile << MODEL_DEPTH_TILE_SHIFT;
        for (row = tile_y << MODEL_DEPTH_TILE_SHIFT;
             row < (tile_y + 1u) << MODEL_DEPTH_TILE_SHIFT;
             ++row) {
            uint32_t column;
            uint16_t *target =
                depth + row * LITE_VIEW_WIDTH + tile_x;
            for (column = 0u;
                 column < MODEL_DEPTH_TILE_SIZE; ++column) {
                target[column] = 0xffffu;
            }
        }
    }
}

static int16_t read_i16(const uint8_t *data)
{
    return (int16_t)(
        (uint16_t)data[0] | ((uint16_t)data[1] << 8)
    );
}

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)(
        (uint16_t)data[0] | ((uint16_t)data[1] << 8)
    );
}

static int32_t model_sin_q14_q8(uint16_t angle_q8)
{
    uint8_t angle = (uint8_t)(angle_q8 >> 8);
    uint8_t fraction = (uint8_t)angle_q8;
    int32_t first = c15_sin_q14(angle);
    int32_t second = c15_sin_q14((uint8_t)(angle + 1u));
    return first + ((second - first) * fraction >> 8);
}

static int32_t model_cos_q14_q8(uint16_t angle_q8)
{
    return model_sin_q14_q8(
        (uint16_t)(angle_q8 + (64u << 8))
    );
}

static int edge_value(
    const model_projected_vertex_t *a,
    const model_projected_vertex_t *b,
    int x,
    int y
)
{
    return ((int)b->x - a->x) * (y - a->y) -
        ((int)b->y - a->y) * (x - a->x);
}

static void model_edge_begin(
    model_scan_edge_t *edge,
    const model_projected_vertex_t *start,
    const model_projected_vertex_t *end,
    int start_y
)
{
    int dy = (int)end->y - start->y;
    int advance = start_y - start->y;
    edge->x = (int32_t)((int64_t)start->x * 65536);
    if (dy != 0) {
        edge->dx = fixed_divide_q16(
            (int64_t)((int)end->x - start->x), (uint32_t)dy
        );
    } else {
        edge->dx = 0;
    }
    edge->x += edge->dx * advance;
}

static void model_edge_step(model_scan_edge_t *edge)
{
    edge->x += edge->dx;
}

static inline __attribute__((always_inline)) int model_wrap_coordinate(
    int value, uint16_t size, uint16_t mask, uint16_t reciprocal
)
{
    uint32_t magnitude;
    uint32_t quotient;
    uint32_t result;
    int negative;
    if (size <= 1u) {
        return 0;
    }
    if ((uint32_t)value < size) {
        return value;
    }
    if (mask != 0u) {
        return value & mask;
    }
    negative = value < 0;
    magnitude = negative ? 0u - (uint32_t)value : (uint32_t)value;
    quotient = (magnitude * reciprocal) >> 16;
    result = magnitude - quotient * size;
    while (result >= size) {
        result -= size;
    }
    return negative && result != 0u ? (int)(size - result) : (int)result;
}

static void draw_model_half(
    lite_framebuffer_t *fb,
    uint16_t *depth,
    const c15_model_texture_t *texture,
    model_scan_edge_t *first,
    model_scan_edge_t *second,
    const model_projected_vertex_t *anchor,
    const model_scan_gradient_t *gradient,
    int y_start,
    int y_end,
    c15_model_render_stats_t *stats
)
{
    int y;
    int32_t row_u;
    int32_t row_v;
    int32_t row_z;
    uint32_t drawn_pixels = 0u;
    const uint8_t *texture_pixels = texture->pixels;
    const uint16_t *texture_palette = texture->palette;
    uint16_t texture_width = texture->width;
    uint16_t width_mask = texture->width_mask;
    uint16_t height_mask = texture->height_mask;
    int opaque = (texture->flags & 1u) == 0u;
    int opaque_power_of_two =
        opaque &&
        width_mask != 0u && height_mask != 0u;
    if (y_start < 0) {
        int skip = -y_start;
        first->x += first->dx * skip;
        second->x += second->dx * skip;
        y_start = 0;
    }
    if (y_end > fb->height) {
        y_end = fb->height;
    }
    row_u = (int32_t)(
        (int64_t)anchor->u * 65536 -
        (int64_t)gradient->du_dx * anchor->x +
        (int64_t)gradient->du_dy * (y_start - anchor->y)
    );
    row_v = (int32_t)(
        (int64_t)anchor->v * 65536 -
        (int64_t)gradient->dv_dx * anchor->x +
        (int64_t)gradient->dv_dy * (y_start - anchor->y)
    );
    row_z = (int32_t)(
        (int64_t)anchor->z * 65536 -
        (int64_t)gradient->dz_dx * anchor->x +
        (int64_t)gradient->dz_dy * (y_start - anchor->y)
    );
    for (y = y_start; y < y_end; ++y) {
        model_scan_edge_t *left = first;
        model_scan_edge_t *right = second;
        int x0;
        int x1;
        int width;
        if (left->x > right->x) {
            left = second;
            right = first;
        }
        x0 = (left->x + 0xffff) >> 16;
        x1 = right->x >> 16;
        width = x1 - x0;
        if (width > 0) {
            int32_t u = row_u + gradient->du_dx * x0;
            int32_t v = row_v + gradient->dv_dx * x0;
            int32_t z = row_z + gradient->dz_dx * x0;
            uint16_t *frame_row = fb->pixels + y * fb->stride;
            uint16_t *depth_row = depth + y * LITE_VIEW_WIDTH;
            if (x0 < 0) {
                int skip = -x0;
                u += gradient->du_dx * skip;
                v += gradient->dv_dx * skip;
                z += gradient->dz_dx * skip;
                x0 = 0;
            }
            if (x1 >= fb->width) {
                x1 = fb->width - 1;
            }
            if (g_view_depth_lazy && x0 <= x1) {
                clear_view_depth_span(depth, y, x0, x1);
            }
            if (x0 <= x1) {
                int remaining = x1 - x0 + 1;
                uint16_t *frame_pixel = frame_row + x0;
                uint16_t *depth_pixel = depth_row + x0;
                if (opaque_power_of_two) {
                    while (remaining-- > 0) {
                        uint32_t depth_value =
                            (uint32_t)(z >> 16);
                        if (depth_value > 0u &&
                            depth_value < *depth_pixel) {
                            int tx = (u >> 16) & width_mask;
                            int ty = (v >> 16) & height_mask;
                            uint8_t texel = texture_pixels[
                                ty * texture_width + tx
                            ];
                            *depth_pixel = (uint16_t)depth_value;
                            *frame_pixel = texture_palette[texel];
                            ++drawn_pixels;
                        }
                        ++frame_pixel;
                        ++depth_pixel;
                        u += gradient->du_dx;
                        v += gradient->dv_dx;
                        z += gradient->dz_dx;
                    }
                } else if (opaque) {
                    while (remaining-- > 0) {
                        uint32_t depth_value =
                            (uint32_t)(z >> 16);
                        if (depth_value > 0u &&
                            depth_value < *depth_pixel) {
                            int tx = model_wrap_coordinate(
                                u >> 16, texture_width,
                                width_mask,
                                texture->width_reciprocal
                            );
                            int ty = model_wrap_coordinate(
                                v >> 16, texture->height,
                                height_mask,
                                texture->height_reciprocal
                            );
                            uint8_t texel = texture_pixels[
                                ty * texture_width + tx
                            ];
                            *depth_pixel = (uint16_t)depth_value;
                            *frame_pixel = texture_palette[texel];
                            ++drawn_pixels;
                        }
                        ++frame_pixel;
                        ++depth_pixel;
                        u += gradient->du_dx;
                        v += gradient->dv_dx;
                        z += gradient->dz_dx;
                    }
                } else {
                    while (remaining-- > 0) {
                        uint32_t depth_value =
                            (uint32_t)(z >> 16);
                        if (depth_value > 0u &&
                            depth_value < *depth_pixel) {
                            int tx = model_wrap_coordinate(
                                u >> 16, texture_width,
                                width_mask,
                                texture->width_reciprocal
                            );
                            int ty = model_wrap_coordinate(
                                v >> 16, texture->height,
                                height_mask,
                                texture->height_reciprocal
                            );
                            uint8_t texel = texture_pixels[
                                ty * texture_width + tx
                            ];
                            if (texel != 255u) {
                                *depth_pixel =
                                    (uint16_t)depth_value;
                                *frame_pixel =
                                    texture_palette[texel];
                                ++drawn_pixels;
                            }
                        }
                        ++frame_pixel;
                        ++depth_pixel;
                        u += gradient->du_dx;
                        v += gradient->dv_dx;
                        z += gradient->dz_dx;
                    }
                }
            }
        }
        model_edge_step(first);
        model_edge_step(second);
        row_u += gradient->du_dy;
        row_v += gradient->dv_dy;
        row_z += gradient->dz_dy;
    }
    stats->pixels += drawn_pixels;
}

static model_view_vertex_t view_model_vertex(
    const uint8_t *source,
    const c15_model_view_t *view,
    int bob_q4
)
{
    model_view_vertex_t result;
    result.x = -read_i16(source + 2);
    result.y = read_i16(source + 4) - 16 + bob_q4;
    result.z = read_i16(source) + view->depth_bias_q4 +
        (bob_q4 * 2) / 5;
    result.u = (int32_t)source[6] << 8;
    result.v = (int32_t)source[7] << 8;
    return result;
}

static model_view_vertex_t view_model_intersection(
    const model_view_vertex_t *inside,
    const model_view_vertex_t *outside,
    int32_t near
)
{
    model_view_vertex_t result;
    int32_t denominator = inside->z - outside->z;
    int32_t fraction = denominator != 0 ?
        fixed_divide_q16(
            (int64_t)(inside->z - near),
            (uint32_t)denominator
        ) : 0;
    result.x = inside->x +
        (int32_t)(((int64_t)(outside->x - inside->x) * fraction) >> 16);
    result.y = inside->y +
        (int32_t)(((int64_t)(outside->y - inside->y) * fraction) >> 16);
    result.z = near;
    result.u = inside->u +
        (int32_t)(((int64_t)(outside->u - inside->u) * fraction) >> 16);
    result.v = inside->v +
        (int32_t)(((int64_t)(outside->v - inside->v) * fraction) >> 16);
    return result;
}

static uint32_t clip_view_model_triangle(
    const model_view_vertex_t input[3],
    model_view_vertex_t output[4],
    int32_t near
)
{
    uint32_t count = 0u;
    uint32_t index;
    model_view_vertex_t previous = input[2];
    int previous_inside = previous.z >= near;
    for (index = 0u; index < 3u; ++index) {
        model_view_vertex_t current = input[index];
        int current_inside = current.z >= near;
        if (current_inside != previous_inside) {
            output[count++] = current_inside ?
                view_model_intersection(&current, &previous, near) :
                view_model_intersection(&previous, &current, near);
        }
        if (current_inside) output[count++] = current;
        previous = current;
        previous_inside = current_inside;
    }
    return count;
}

static model_projected_vertex_t project_view_model_vertex(
    const model_view_vertex_t *source,
    const c15_model_view_t *view,
    int recoil
)
{
    model_projected_vertex_t result;
    result.x = (int16_t)(
        view->center_x +
        (source->x * view->focal_length) / source->z
    );
    result.y = (int16_t)(
        view->origin_y - recoil -
        (source->y * view->focal_length) / source->z
    );
    result.z = (int16_t)source->z;
    result.u = (uint8_t)((source->u + 128) >> 8);
    result.v = (uint8_t)((source->v + 128) >> 8);
    return result;
}

static void draw_model_triangle(
    lite_framebuffer_t *fb,
    uint16_t *depth,
    const c15_model_texture_t *texture,
    const model_projected_vertex_t *a,
    const model_projected_vertex_t *b,
    const model_projected_vertex_t *c,
    c15_model_render_stats_t *stats
)
{
    int area = edge_value(a, b, c->x, c->y);
    model_projected_vertex_t first = *a;
    model_projected_vertex_t second = *b;
    model_projected_vertex_t third = *c;
    model_projected_vertex_t temporary;
    model_scan_edge_t long_edge;
    model_scan_edge_t short_edge;
    model_scan_gradient_t gradient;
    int dx1;
    int dy1;
    int dx2;
    int dy2;
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    /* Both GoldSrc camera paths use screen-right = -world/model Y. */
    if (area <= 0) {
        return;
    }
    min_x = a->x < b->x ? a->x : b->x;
    if (c->x < min_x) min_x = c->x;
    max_x = a->x > b->x ? a->x : b->x;
    if (c->x > max_x) max_x = c->x;
    min_y = a->y < b->y ? a->y : b->y;
    if (c->y < min_y) min_y = c->y;
    max_y = a->y > b->y ? a->y : b->y;
    if (c->y > max_y) max_y = c->y;
    if (max_x < 0 || min_x >= fb->width ||
        max_y <= 0 || min_y >= fb->height) {
        return;
    }
    dx1 = (int)b->x - a->x;
    dy1 = (int)b->y - a->y;
    dx2 = (int)c->x - a->x;
    dy2 = (int)c->y - a->y;
    gradient.du_dx = fixed_divide_q16(
        (int64_t)((int)b->u - a->u) * dy2 -
            (int64_t)((int)c->u - a->u) * dy1,
        (uint32_t)area
    );
    gradient.du_dy = fixed_divide_q16(
        (int64_t)dx1 * ((int)c->u - a->u) -
            (int64_t)dx2 * ((int)b->u - a->u),
        (uint32_t)area
    );
    gradient.dv_dx = fixed_divide_q16(
        (int64_t)((int)b->v - a->v) * dy2 -
            (int64_t)((int)c->v - a->v) * dy1,
        (uint32_t)area
    );
    gradient.dv_dy = fixed_divide_q16(
        (int64_t)dx1 * ((int)c->v - a->v) -
            (int64_t)dx2 * ((int)b->v - a->v),
        (uint32_t)area
    );
    gradient.dz_dx = fixed_divide_q16(
        (int64_t)((int)b->z - a->z) * dy2 -
            (int64_t)((int)c->z - a->z) * dy1,
        (uint32_t)area
    );
    gradient.dz_dy = fixed_divide_q16(
        (int64_t)dx1 * ((int)c->z - a->z) -
            (int64_t)dx2 * ((int)b->z - a->z),
        (uint32_t)area
    );
    if (first.y > second.y) {
        temporary = first; first = second; second = temporary;
    }
    if (second.y > third.y) {
        temporary = second; second = third; third = temporary;
    }
    if (first.y > second.y) {
        temporary = first; first = second; second = temporary;
    }
    if (first.y == third.y || third.y <= 0 || first.y >= fb->height) {
        return;
    }
    if (second.y > first.y) {
        model_edge_begin(&long_edge, &first, &third, first.y);
        model_edge_begin(&short_edge, &first, &second, first.y);
        draw_model_half(
            fb, depth, texture, &long_edge, &short_edge,
            a, &gradient,
            first.y, second.y, stats
        );
    }
    if (third.y > second.y) {
        model_edge_begin(&long_edge, &first, &third, second.y);
        model_edge_begin(&short_edge, &second, &third, second.y);
        draw_model_half(
            fb, depth, texture, &long_edge, &short_edge,
            a, &gradient,
            second.y, third.y, stats
        );
    }
    ++stats->triangles;
}

void c15_render_view_model(
    const c15_model_t *model,
    lite_framebuffer_t *framebuffer,
    uint16_t *depth,
    const c15_model_view_t *view,
    int recoil,
    int bob_q4,
    c15_model_render_stats_t *stats
)
{
    uint32_t index;
    if (!model || !model->loaded || !framebuffer || !depth || !view ||
        !stats || view->focal_length <= 0 ||
        model->vertex_count > MODEL_MAX_PROJECTED_VERTICES) {
        return;
    }
    bda_memset(stats, 0, sizeof(*stats));
    begin_lazy_view_depth();
    for (index = 0u; index < model->vertex_count; ++index) {
        const uint8_t *source = model->vertices + index * 8u;
        model_projected_vertex_t *target = &g_projected_vertices[index];
        model_view_vertex_t transformed =
            view_model_vertex(source, view, bob_q4);
        /*
         * GoldSrc puts the view model at the camera origin and clips it
         * at the near plane. Never clamp a behind-camera vertex onto the
         * plane: that deforms the hand into a giant floating polygon.
         * Mark it for triangle rejection instead. The fixed -1 unit Z
         * offset above mirrors Valve's view->origin[2] -= 1.
         */
        if (transformed.z < view->near_depth_q4) {
            target->x = 0;
            target->y = 0;
            target->z = (int16_t)transformed.z;
            target->u = source[6];
            target->v = source[7];
            continue;
        }
        *target = project_view_model_vertex(
            &transformed, view, recoil
        );
    }
    for (index = 0u; index < model->triangle_count; ++index) {
        const uint8_t *triangle = model->triangles + index * 8u;
        uint32_t texture = triangle[6];
        uint16_t ia = read_u16(triangle);
        uint16_t ib = read_u16(triangle + 2);
        uint16_t ic = read_u16(triangle + 4);
        const model_projected_vertex_t *a =
            &g_projected_vertices[ia];
        const model_projected_vertex_t *b =
            &g_projected_vertices[ib];
        const model_projected_vertex_t *c =
            &g_projected_vertices[ic];
        if (a->z < view->near_depth_q4 ||
            b->z < view->near_depth_q4 ||
            c->z < view->near_depth_q4) {
            model_view_vertex_t input[3];
            model_view_vertex_t clipped[4];
            model_projected_vertex_t projected[4];
            uint32_t count;
            uint32_t vertex;
            input[0] = view_model_vertex(
                model->vertices + ia * 8u, view, bob_q4
            );
            input[1] = view_model_vertex(
                model->vertices + ib * 8u, view, bob_q4
            );
            input[2] = view_model_vertex(
                model->vertices + ic * 8u, view, bob_q4
            );
            count = clip_view_model_triangle(
                input, clipped, view->near_depth_q4
            );
            for (vertex = 0u; vertex < count; ++vertex) {
                projected[vertex] = project_view_model_vertex(
                    &clipped[vertex], view, recoil
                );
            }
            for (vertex = 1u; vertex + 1u < count; ++vertex) {
                draw_model_triangle(
                    framebuffer, depth, &model->textures[texture],
                    &projected[0], &projected[vertex],
                    &projected[vertex + 1u], stats
                );
            }
            continue;
        }
        draw_model_triangle(
            framebuffer, depth, &model->textures[texture],
            a, b, c, stats
        );
    }
    g_view_depth_lazy = 0u;
}

void c15_render_world_model(
    const c15_model_t *model,
    lite_framebuffer_t *framebuffer,
    uint16_t *depth,
    const c15_camera_t *camera,
    int32_t origin_x,
    int32_t origin_y,
    int32_t origin_z,
    uint8_t yaw,
    c15_model_render_stats_t *stats
)
{
    int32_t entity_sine;
    int32_t entity_cosine;
    int32_t camera_sine;
    int32_t camera_cosine;
    int32_t pitch_sine;
    int32_t pitch_cosine;
    int32_t focal;
    uint32_t index;
    if (!model || !model->loaded || !framebuffer || !depth || !camera ||
        !stats || model->vertex_count > MODEL_MAX_PROJECTED_VERTICES) {
        return;
    }
    entity_sine = c15_sin_q14(yaw);
    entity_cosine = c15_cos_q14(yaw);
    camera_sine = model_sin_q14_q8(camera->yaw_q8);
    camera_cosine = model_cos_q14_q8(camera->yaw_q8);
    pitch_sine = model_sin_q14_q8(
        (uint16_t)(int16_t)camera->pitch_q8
    );
    pitch_cosine = model_cos_q14_q8(
        (uint16_t)(int16_t)camera->pitch_q8
    );
    focal = camera->focal_length != 0u ?
        camera->focal_length : MODEL_WORLD_DEFAULT_FOCAL;
    {
        int32_t origin_dx_q4 = (origin_x - camera->x) * 16;
        int32_t origin_dy_q4 = (origin_y - camera->y) * 16;
        int32_t origin_side_q4 = (int32_t)(
            ((int64_t)camera_sine * origin_dx_q4 -
             (int64_t)camera_cosine * origin_dy_q4) >> 14
        );
        int32_t origin_forward_q4 = (int32_t)(
            ((int64_t)camera_cosine * origin_dx_q4 +
             (int64_t)camera_sine * origin_dy_q4) >> 14
        );
        if (origin_forward_q4 < -MODEL_ENTITY_CULL_RADIUS_Q4 ||
            (int64_t)origin_side_q4 * focal >
                (int64_t)origin_forward_q4 *
                    ((int32_t)LITE_VIEW_WIDTH / 2) +
                (int64_t)MODEL_ENTITY_CULL_RADIUS_Q4 * focal ||
            (int64_t)origin_side_q4 * focal <
                -(int64_t)origin_forward_q4 *
                    ((int32_t)LITE_VIEW_WIDTH / 2) -
                (int64_t)MODEL_ENTITY_CULL_RADIUS_Q4 * focal) {
            return;
        }
    }
    for (index = 0u; index < model->vertex_count; ++index) {
        const uint8_t *source = model->vertices + index * 8u;
        model_projected_vertex_t *target = &g_projected_vertices[index];
        int32_t model_x_q4 = read_i16(source);
        int32_t model_y_q4 = read_i16(source + 2);
        int32_t world_x_q4 = origin_x * 16 + (int32_t)(
            ((int64_t)entity_cosine * model_x_q4 -
             (int64_t)entity_sine * model_y_q4) >> 14
        );
        int32_t world_y_q4 = origin_y * 16 + (int32_t)(
            ((int64_t)entity_sine * model_x_q4 +
             (int64_t)entity_cosine * model_y_q4) >> 14
        );
        int32_t world_z_q4 = origin_z * 16 + read_i16(source + 4);
        int32_t dx_q4 = world_x_q4 - camera->x * 16;
        int32_t dy_q4 = world_y_q4 - camera->y * 16;
        int32_t side_q4 = (int32_t)(
            ((int64_t)camera_sine * dx_q4 -
             (int64_t)camera_cosine * dy_q4) >> 14
        );
        int32_t forward_q4 = (int32_t)(
            ((int64_t)camera_cosine * dx_q4 +
             (int64_t)camera_sine * dy_q4) >> 14
        );
        int32_t vertical_q4 = world_z_q4 - camera->z * 16;
        int32_t view_y_q4 = (int32_t)(
            ((int64_t)pitch_cosine * vertical_q4 -
             (int64_t)pitch_sine * forward_q4) >> 14
        );
        int32_t view_z_q4 = (int32_t)(
            ((int64_t)pitch_sine * vertical_q4 +
             (int64_t)pitch_cosine * forward_q4) >> 14
        );
        target->u = source[6];
        target->v = source[7];
        if (view_z_q4 < MODEL_WORLD_NEAR * 16) {
            target->x = 0;
            target->y = 0;
            target->z = 0;
        } else {
            int32_t projected_x =
                (int32_t)LITE_VIEW_WIDTH / 2 +
                (side_q4 * focal) / view_z_q4;
            int32_t projected_y =
                (int32_t)LITE_VIEW_HEIGHT / 2 -
                (view_y_q4 * focal) / view_z_q4;
            int32_t depth_value = view_z_q4 >> 4;
            if (projected_x < -32768) projected_x = -32768;
            if (projected_x > 32767) projected_x = 32767;
            if (projected_y < -32768) projected_y = -32768;
            if (projected_y > 32767) projected_y = 32767;
            if (depth_value > 32767) depth_value = 32767;
            target->x = (int16_t)projected_x;
            target->y = (int16_t)projected_y;
            target->z = (int16_t)depth_value;
        }
    }
    for (index = 0u; index < model->triangle_count; ++index) {
        const uint8_t *triangle = model->triangles + index * 8u;
        const model_projected_vertex_t *a =
            &g_projected_vertices[read_u16(triangle)];
        const model_projected_vertex_t *b =
            &g_projected_vertices[read_u16(triangle + 2)];
        const model_projected_vertex_t *c =
            &g_projected_vertices[read_u16(triangle + 4)];
        if (a->z < MODEL_WORLD_NEAR ||
            b->z < MODEL_WORLD_NEAR ||
            c->z < MODEL_WORLD_NEAR) {
            continue;
        }
        draw_model_triangle(
            framebuffer, depth, &model->textures[triangle[6]],
            a, b, c, stats
        );
    }
}
