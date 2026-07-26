/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "render/model.h"

#include "bda_memory.h"
#include "platform/bbk9588.h"

/* The largest compiled historical view model is v_m4a1 at 971 vertices. */
#define MODEL_MAX_PROJECTED_VERTICES 976u
#define MODEL_WORLD_NEAR 8
#define MODEL_WORLD_FOCAL ((int)LITE_VIEW_WIDTH / 2)

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

static model_projected_vertex_t
    g_projected_vertices[MODEL_MAX_PROJECTED_VERTICES];

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
        (int32_t)(((int64_t)(inside->z - near) << 16) /
                  denominator) : 0;
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
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int dx1;
    int dy1;
    int dx2;
    int dy2;
    int32_t du_dx;
    int32_t du_dy;
    int32_t dv_dx;
    int32_t dv_dy;
    int32_t dz_dx;
    int32_t dz_dy;
    int32_t row_u;
    int32_t row_v;
    int32_t row_z;
    int row_bc;
    int row_ca;
    int row_ab;
    int step_x_bc;
    int step_x_ca;
    int step_x_ab;
    int step_y_bc;
    int step_y_ca;
    int step_y_ab;
    int y;
    /* Both GoldSrc camera paths use screen-right = -world/model Y. */
    if (area <= 0) {
        return;
    }
    min_x = a->x;
    if (b->x < min_x) min_x = b->x;
    if (c->x < min_x) min_x = c->x;
    max_x = a->x;
    if (b->x > max_x) max_x = b->x;
    if (c->x > max_x) max_x = c->x;
    min_y = a->y;
    if (b->y < min_y) min_y = b->y;
    if (c->y < min_y) min_y = c->y;
    max_y = a->y;
    if (b->y > max_y) max_y = b->y;
    if (c->y > max_y) max_y = c->y;
    if (max_x < 0 || max_y < 0 ||
        min_x >= fb->width || min_y >= fb->height) {
        return;
    }
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb->width) max_x = fb->width - 1;
    if (max_y >= fb->height) max_y = fb->height - 1;
    dx1 = (int)b->x - a->x;
    dy1 = (int)b->y - a->y;
    dx2 = (int)c->x - a->x;
    dy2 = (int)c->y - a->y;
    du_dx = (int32_t)(
        (((int64_t)((int)b->u - a->u) * dy2 -
          (int64_t)((int)c->u - a->u) * dy1) * 65536) / area
    );
    du_dy = (int32_t)(
        (((int64_t)dx1 * ((int)c->u - a->u) -
          (int64_t)dx2 * ((int)b->u - a->u)) * 65536) / area
    );
    dv_dx = (int32_t)(
        (((int64_t)((int)b->v - a->v) * dy2 -
          (int64_t)((int)c->v - a->v) * dy1) * 65536) / area
    );
    dv_dy = (int32_t)(
        (((int64_t)dx1 * ((int)c->v - a->v) -
          (int64_t)dx2 * ((int)b->v - a->v)) * 65536) / area
    );
    dz_dx = (int32_t)(
        (((int64_t)((int)b->z - a->z) * dy2 -
          (int64_t)((int)c->z - a->z) * dy1) * 65536) / area
    );
    dz_dy = (int32_t)(
        (((int64_t)dx1 * ((int)c->z - a->z) -
          (int64_t)dx2 * ((int)b->z - a->z)) * 65536) / area
    );
    row_u = ((int32_t)a->u << 16) +
        du_dx * (min_x - a->x) + du_dy * (min_y - a->y);
    row_v = ((int32_t)a->v << 16) +
        dv_dx * (min_x - a->x) + dv_dy * (min_y - a->y);
    row_z = ((int32_t)a->z << 16) +
        dz_dx * (min_x - a->x) + dz_dy * (min_y - a->y);
    row_bc = edge_value(b, c, min_x, min_y);
    row_ca = edge_value(c, a, min_x, min_y);
    row_ab = edge_value(a, b, min_x, min_y);
    step_x_bc = -((int)c->y - b->y);
    step_x_ca = -((int)a->y - c->y);
    step_x_ab = -((int)b->y - a->y);
    step_y_bc = (int)c->x - b->x;
    step_y_ca = (int)a->x - c->x;
    step_y_ab = (int)b->x - a->x;
    for (y = min_y; y <= max_y; ++y) {
        int32_t u = row_u;
        int32_t v = row_v;
        int32_t z = row_z;
        int edge_bc = row_bc;
        int edge_ca = row_ca;
        int edge_ab = row_ab;
        int x;
        for (x = min_x; x <= max_x; ++x) {
            if (edge_bc >= 0 && edge_ca >= 0 && edge_ab >= 0) {
                uint32_t depth_index =
                    (uint32_t)y * LITE_VIEW_WIDTH + (uint32_t)x;
                uint32_t depth_value = (uint32_t)(z >> 16);
                if (depth_value < depth[depth_index]) {
                    int tx = (u >> 16) % texture->width;
                    int ty = (v >> 16) % texture->height;
                    uint8_t texel;
                    if (tx < 0) tx += texture->width;
                    if (ty < 0) ty += texture->height;
                    texel = texture->pixels[
                        ty * texture->width + tx
                    ];
                    if ((texture->flags & 1u) == 0u || texel != 255u) {
                        depth[depth_index] = (uint16_t)depth_value;
                        fb->pixels[y * fb->stride + x] =
                            texture->palette[texel];
                        ++stats->pixels;
                    }
                }
            }
            u += du_dx;
            v += dv_dx;
            z += dz_dx;
            edge_bc += step_x_bc;
            edge_ca += step_x_ca;
            edge_ab += step_x_ab;
        }
        row_u += du_dy;
        row_v += dv_dy;
        row_z += dz_dy;
        row_bc += step_y_bc;
        row_ca += step_y_ca;
        row_ab += step_y_ab;
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
    for (index = 0u;
         index < LITE_VIEW_WIDTH * LITE_VIEW_HEIGHT; ++index) {
        depth[index] = 0xffffu;
    }
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
                (side_q4 * MODEL_WORLD_FOCAL) / view_z_q4;
            int32_t projected_y =
                (int32_t)LITE_VIEW_HEIGHT / 2 -
                (view_y_q4 * MODEL_WORLD_FOCAL) / view_z_q4;
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
