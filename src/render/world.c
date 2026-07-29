/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "render/world.h"

#include "bda_memory.h"
#include "platform/bbk9588.h"

/* Current historical maps peak below 16 vertices; clipping can add five. */
#define MAX_POLYGON_VERTICES 20
#define NEAR_PLANE 8
#define DEFAULT_FOCAL_LENGTH ((int)LITE_VIEW_WIDTH / 2)
#define SURF_PLANEBACK 0x8000u
/* M19 peak visible set: cs_office; keep room for all converted surfaces. */
#define MAX_CACHED_VISIBLE_SURFACES 1536u

typedef struct view_vertex {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t u;
    int32_t v;
} view_vertex_t;

typedef struct screen_vertex {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t u;
    int32_t v;
} screen_vertex_t;

typedef struct scan_edge {
    int32_t x;
    int32_t dx;
} scan_edge_t;

typedef struct scan_gradient {
    int32_t du_dx;
    int32_t du_dy;
    int32_t dv_dx;
    int32_t dv_dy;
    int32_t dz_dx;
    int32_t dz_dy;
} scan_gradient_t;

typedef uint32_t depth_alias_u32 __attribute__((__may_alias__));

static const c15_map_t *g_cached_map;
static uint8_t *g_cached_surface_bits;
static uint16_t g_cached_surface_indices[MAX_CACHED_VISIBLE_SURFACES];
static uint32_t g_cached_surface_count;
static uint32_t g_cached_visible_leaves;
static int g_cached_leaf = -1;

static inline __attribute__((always_inline)) int wrap_coordinate(
    int value, uint16_t size, uint16_t mask, uint16_t reciprocal
)
{
    uint32_t quotient;
    uint32_t result;
    if (value < 0) {
        value = 0;
    }
    if (mask != 0u) {
        return value & mask;
    }
    quotient = ((uint32_t)value * reciprocal) >> 16;
    result = (uint32_t)value - quotient * size;
    while (result >= size) {
        result -= size;
    }
    return (int)result;
}

static int surface_front(
    const c15_map_t *map,
    const c15_camera_t *camera,
    const c15_surface_t *surface
)
{
    c15_plane_t plane;
    int64_t distance;
    int32_t axial_distance_q4;
    int back;
    if (!c15_map_plane(map, surface->plane_id, &plane)) {
        return 0;
    }
    back = (surface->flags & SURF_PLANEBACK) != 0u;
    if (plane.nx == 16384 && plane.ny == 0 && plane.nz == 0) {
        axial_distance_q4 = camera->x * 16 - plane.distance_q4;
        return back ? axial_distance_q4 < -1 : axial_distance_q4 > 1;
    }
    if (plane.nx == 0 && plane.ny == 16384 && plane.nz == 0) {
        axial_distance_q4 = camera->y * 16 - plane.distance_q4;
        return back ? axial_distance_q4 < -1 : axial_distance_q4 > 1;
    }
    if (plane.nx == 0 && plane.ny == 0 && plane.nz == 16384) {
        axial_distance_q4 = camera->z * 16 - plane.distance_q4;
        return back ? axial_distance_q4 < -1 : axial_distance_q4 > 1;
    }
    distance =
        (int64_t)plane.nx * camera->x +
        (int64_t)plane.ny * camera->y +
        (int64_t)plane.nz * camera->z -
        (int64_t)plane.distance_q4 * 1024;
    return back ? distance < -1024 : distance > 1024;
}

static view_vertex_t transform_vertex(
    const c15_surface_vertex_t *source,
    const c15_camera_t *camera,
    int32_t sine,
    int32_t cosine,
    int32_t pitch_sine,
    int32_t pitch_cosine
)
{
    view_vertex_t result;
    int32_t dx = (int32_t)source->x - camera->x;
    int32_t dy = (int32_t)source->y - camera->y;
    int32_t forward;
    int32_t vertical;
    /*
     * GoldSrc's view-right vector is (sin(yaw), -cos(yaw)). At yaw zero,
     * world -Y must therefore appear on the right side of the screen.
     */
    result.x = (int32_t)(
        ((int64_t)sine * dx - (int64_t)cosine * dy) >> 14
    );
    forward = (int32_t)(
        ((int64_t)cosine * dx + (int64_t)sine * dy) >> 14
    );
    vertical = (int32_t)source->z - camera->z;
    result.y = (int32_t)(
        ((int64_t)pitch_cosine * vertical -
         (int64_t)pitch_sine * forward) >> 14
    );
    result.z = (int32_t)(
        ((int64_t)pitch_sine * vertical +
         (int64_t)pitch_cosine * forward) >> 14
    );
    result.u = source->u;
    result.v = source->v;
    return result;
}

static int32_t sin_q14_q8(uint16_t angle_q8)
{
    uint8_t angle = (uint8_t)(angle_q8 >> 8);
    uint8_t fraction = (uint8_t)angle_q8;
    int32_t first = c15_sin_q14(angle);
    int32_t second = c15_sin_q14((uint8_t)(angle + 1u));
    return first + ((second - first) * fraction >> 8);
}

static int32_t cos_q14_q8(uint16_t angle_q8)
{
    return sin_q14_q8((uint16_t)(angle_q8 + (64u << 8)));
}

static int32_t clip_distance(
    const view_vertex_t *vertex, int plane, int focal
)
{
    switch (plane) {
        case 0: return vertex->z - NEAR_PLANE;
        case 1:
            return (int32_t)LITE_VIEW_WIDTH / 2 * vertex->z +
                focal * vertex->x;
        case 2:
            return (int32_t)LITE_VIEW_WIDTH / 2 * vertex->z -
                focal * vertex->x;
        case 3:
            return (int32_t)LITE_VIEW_HEIGHT / 2 * vertex->z -
                focal * vertex->y;
        default:
            return (int32_t)LITE_VIEW_HEIGHT / 2 * vertex->z +
                focal * vertex->y;
    }
}

static uint8_t clip_code(const view_vertex_t *vertex, int focal)
{
    uint8_t code = 0u;
    if (vertex->z < NEAR_PLANE) code |= 1u << 0;
    if ((int32_t)LITE_VIEW_WIDTH / 2 * vertex->z +
            focal * vertex->x < 0) code |= 1u << 1;
    if ((int32_t)LITE_VIEW_WIDTH / 2 * vertex->z -
            focal * vertex->x < 0) code |= 1u << 2;
    if ((int32_t)LITE_VIEW_HEIGHT / 2 * vertex->z -
            focal * vertex->y < 0) code |= 1u << 3;
    if ((int32_t)LITE_VIEW_HEIGHT / 2 * vertex->z +
            focal * vertex->y < 0) code |= 1u << 4;
    return code;
}

static view_vertex_t clip_intersection(
    const view_vertex_t *inside,
    const view_vertex_t *outside,
    int32_t inside_distance,
    int32_t outside_distance
)
{
    view_vertex_t result;
    int32_t denominator = inside_distance - outside_distance;
    int32_t fraction = denominator != 0 ?
        (int32_t)(((int64_t)inside_distance << 16) / denominator) : 0;
    result.x = inside->x +
        (int32_t)(((int64_t)(outside->x - inside->x) * fraction) >> 16);
    result.y = inside->y +
        (int32_t)(((int64_t)(outside->y - inside->y) * fraction) >> 16);
    result.z = inside->z +
        (int32_t)(((int64_t)(outside->z - inside->z) * fraction) >> 16);
    result.u = inside->u +
        (int32_t)(((int64_t)(outside->u - inside->u) * fraction) >> 16);
    result.v = inside->v +
        (int32_t)(((int64_t)(outside->v - inside->v) * fraction) >> 16);
    return result;
}

static int clip_plane(
    const view_vertex_t *input,
    int input_count,
    view_vertex_t *output,
    int plane,
    int focal
)
{
    int output_count = 0;
    int index;
    view_vertex_t previous = input[input_count - 1];
    int32_t previous_distance = clip_distance(
        &previous, plane, focal
    );
    int previous_inside = previous_distance >= 0;
    for (index = 0; index < input_count; ++index) {
        view_vertex_t current = input[index];
        int32_t current_distance = clip_distance(
            &current, plane, focal
        );
        int current_inside = current_distance >= 0;
        if (current_inside != previous_inside &&
            output_count < MAX_POLYGON_VERTICES) {
            output[output_count++] = current_inside ?
                clip_intersection(
                    &current, &previous,
                    current_distance, previous_distance
                ) :
                clip_intersection(
                    &previous, &current,
                    previous_distance, current_distance
                );
        }
        if (current_inside && output_count < MAX_POLYGON_VERTICES) {
            output[output_count++] = current;
        }
        previous = current;
        previous_distance = current_distance;
        previous_inside = current_inside;
    }
    return output_count;
}

static screen_vertex_t project_vertex(
    const view_vertex_t *source, int focal
)
{
    screen_vertex_t result;
    result.x = (int32_t)(LITE_VIEW_WIDTH / 2u) + (int32_t)(
        ((int64_t)source->x * focal) / source->z
    );
    result.y = (int32_t)(LITE_VIEW_HEIGHT / 2u) - (int32_t)(
        ((int64_t)source->y * focal) / source->z
    );
    result.z = source->z;
    result.u = source->u * 4096;
    result.v = source->v * 4096;
    return result;
}

static void edge_begin(
    scan_edge_t *edge,
    const screen_vertex_t *start,
    const screen_vertex_t *end,
    int start_y
)
{
    int dy = end->y - start->y;
    int advance = start_y - start->y;
    edge->x = (int32_t)((int64_t)start->x * 65536);
    if (dy != 0) {
        edge->dx = (int32_t)(
            ((int64_t)(end->x - start->x) * 65536) / dy
        );
    } else {
        edge->dx = 0;
    }
    edge->x += edge->dx * advance;
}

static void edge_step(scan_edge_t *edge)
{
    edge->x += edge->dx;
}

static inline __attribute__((always_inline)) uint16_t shade_world_texel(
    uint16_t color, uint8_t level
)
{
    switch (level) {
    case 0u:
        return (uint16_t)((color & 0xe79cu) >> 2);
    case 1u:
        return (uint16_t)((color & 0xf7deu) >> 1);
    case 2u:
        return (uint16_t)(
            ((color & 0xf7deu) >> 1) +
            ((color & 0xe79cu) >> 2)
        );
    default:
        return color;
    }
}

static void draw_half(
    lite_framebuffer_t *fb,
    uint16_t *depth,
    const c15_texture_t *texture,
    uint8_t light,
    scan_edge_t *first,
    scan_edge_t *second,
    const screen_vertex_t *anchor,
    const scan_gradient_t *gradient,
    int y_start,
    int y_end,
    c15_render_stats_t *stats
)
{
    int y;
    int32_t row_u;
    int32_t row_v;
    int32_t row_z;
    uint8_t light_level = (uint8_t)(light >> 6);
    if (y_start < 0) {
        int skip = -y_start;
        first->x += first->dx * skip;
        second->x += second->dx * skip;
        y_start = 0;
    }
    if (y_end > (int)LITE_VIEW_HEIGHT) {
        y_end = (int)LITE_VIEW_HEIGHT;
    }
    row_u = (int32_t)(
        (int64_t)anchor->u -
        (int64_t)gradient->du_dx * anchor->x +
        (int64_t)gradient->du_dy * (y_start - anchor->y)
    );
    row_v = (int32_t)(
        (int64_t)anchor->v -
        (int64_t)gradient->dv_dx * anchor->x +
        (int64_t)gradient->dv_dy * (y_start - anchor->y)
    );
    row_z = (int32_t)(
        (int64_t)anchor->z * 65536 -
        (int64_t)gradient->dz_dx * anchor->x +
        (int64_t)gradient->dz_dy * (y_start - anchor->y)
    );
    for (y = y_start; y < y_end; ++y) {
        scan_edge_t *left = first;
        scan_edge_t *right = second;
        int x0;
        int x1;
        int width;
        int x;
        int32_t u;
        int32_t v;
        int32_t z;
        uint16_t *frame_row;
        uint16_t *depth_row;
        if (left->x > right->x) {
            left = second;
            right = first;
        }
        x0 = (left->x + 0xffff) >> 16;
        x1 = right->x >> 16;
        width = x1 - x0;
        if (width > 0) {
            u = row_u + gradient->du_dx * x0;
            v = row_v + gradient->dv_dx * x0;
            z = row_z + gradient->dz_dx * x0;
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
            frame_row = fb->pixels + y * fb->stride;
            depth_row = depth + y * LITE_VIEW_WIDTH;
            for (x = x0; x <= x1; ++x) {
                uint32_t depth_value = (uint32_t)(z >> 16);
                if (depth_value > 0u && depth_value < depth_row[x]) {
                    int tx = wrap_coordinate(
                        u >> 16, texture->width,
                        texture->width_mask, texture->width_reciprocal
                    );
                    int ty = wrap_coordinate(
                        v >> 16, texture->height,
                        texture->height_mask, texture->height_reciprocal
                    );
                    uint8_t texel = texture->pixels[
                        ty * texture->width + tx
                    ];
                    if ((texture->flags & 1u) == 0u ||
                        texel + 1u != texture->palette_count) {
                        depth_row[x] = (uint16_t)depth_value;
                        frame_row[x] =
                            shade_world_texel(
                                texture->palette[texel], light_level
                            );
                        ++stats->drawn_pixels;
                    }
                }
                u += gradient->du_dx;
                v += gradient->dv_dx;
                z += gradient->dz_dx;
            }
        }
        edge_step(first);
        edge_step(second);
        row_u += gradient->du_dy;
        row_v += gradient->dv_dy;
        row_z += gradient->dz_dy;
    }
}

static void draw_triangle(
    lite_framebuffer_t *fb,
    uint16_t *depth,
    const c15_texture_t *texture,
    uint8_t light,
    screen_vertex_t a,
    screen_vertex_t b,
    screen_vertex_t c,
    c15_render_stats_t *stats
)
{
    screen_vertex_t anchor = a;
    screen_vertex_t temp;
    scan_edge_t long_edge;
    scan_edge_t short_edge;
    scan_gradient_t gradient;
    int area = ((b.x - a.x) * (c.y - a.y)) -
        ((b.y - a.y) * (c.x - a.x));
    int dx1;
    int dy1;
    int dx2;
    int dy2;
    if (area == 0) {
        return;
    }
    dx1 = b.x - a.x;
    dy1 = b.y - a.y;
    dx2 = c.x - a.x;
    dy2 = c.y - a.y;
    gradient.du_dx = (int32_t)(
        ((int64_t)(b.u - a.u) * dy2 -
         (int64_t)(c.u - a.u) * dy1) / area
    );
    gradient.du_dy = (int32_t)(
        ((int64_t)dx1 * (c.u - a.u) -
         (int64_t)dx2 * (b.u - a.u)) / area
    );
    gradient.dv_dx = (int32_t)(
        ((int64_t)(b.v - a.v) * dy2 -
         (int64_t)(c.v - a.v) * dy1) / area
    );
    gradient.dv_dy = (int32_t)(
        ((int64_t)dx1 * (c.v - a.v) -
         (int64_t)dx2 * (b.v - a.v)) / area
    );
    gradient.dz_dx = (int32_t)(
        (((int64_t)(b.z - a.z) * dy2 -
          (int64_t)(c.z - a.z) * dy1) * 65536) / area
    );
    gradient.dz_dy = (int32_t)(
        (((int64_t)dx1 * (c.z - a.z) -
          (int64_t)dx2 * (b.z - a.z)) * 65536) / area
    );
    if (a.y > b.y) { temp = a; a = b; b = temp; }
    if (b.y > c.y) { temp = b; b = c; c = temp; }
    if (a.y > b.y) { temp = a; a = b; b = temp; }
    if (a.y == c.y || c.y <= 0 || a.y >= (int)LITE_VIEW_HEIGHT) {
        return;
    }
    if (b.y > a.y) {
        edge_begin(&long_edge, &a, &c, a.y);
        edge_begin(&short_edge, &a, &b, a.y);
        draw_half(
            fb, depth, texture, light,
            &long_edge, &short_edge, &anchor, &gradient,
            a.y, b.y, stats
        );
    }
    if (c.y > b.y) {
        edge_begin(&long_edge, &a, &c, b.y);
        edge_begin(&short_edge, &b, &c, b.y);
        draw_half(
            fb, depth, texture, light,
            &long_edge, &short_edge, &anchor, &gradient,
            b.y, c.y, stats
        );
    }
    ++stats->drawn_triangles;
}

static void clear_depth_buffer(uint16_t *depth)
{
    depth_alias_u32 *words = (depth_alias_u32 *)depth;
    uint32_t remaining =
        (LITE_VIEW_WIDTH * LITE_VIEW_HEIGHT) / 2u;
    while (remaining >= 8u) {
        words[0] = 0xffffffffu;
        words[1] = 0xffffffffu;
        words[2] = 0xffffffffu;
        words[3] = 0xffffffffu;
        words[4] = 0xffffffffu;
        words[5] = 0xffffffffu;
        words[6] = 0xffffffffu;
        words[7] = 0xffffffffu;
        words += 8;
        remaining -= 8u;
    }
    while (remaining-- != 0u) {
        *words++ = 0xffffffffu;
    }
}

void c15_render_world(
    c15_map_t *map,
    const c15_camera_t *camera,
    lite_framebuffer_t *framebuffer,
    uint16_t *depth,
    uint8_t *visible_surface_bits,
    uint32_t visible_surface_bytes,
    c15_render_stats_t *stats
)
{
    const c15_map_section_t *surface_section =
        map ? map->surface_section : 0;
    uint32_t list_index;
    int visibility_changed = 0;
    int camera_leaf;
    int32_t sine = sin_q14_q8(camera->yaw_q8);
    int32_t cosine = cos_q14_q8(camera->yaw_q8);
    int32_t pitch_sine = sin_q14_q8((uint16_t)camera->pitch_q8);
    int32_t pitch_cosine = cos_q14_q8((uint16_t)camera->pitch_q8);
    int focal = camera->focal_length != 0u ?
        camera->focal_length : DEFAULT_FOCAL_LENGTH;

    bda_memset(stats, 0, sizeof(*stats));
    clear_depth_buffer(depth);
    lite_fb_rect(
        framebuffer, 0, 0, LITE_VIEW_WIDTH, LITE_VIEW_HEIGHT / 2,
        lite_rgb565(41u, 75u, 92u)
    );
    lite_fb_rect(
        framebuffer, 0, LITE_VIEW_HEIGHT / 2,
        LITE_VIEW_WIDTH, LITE_VIEW_HEIGHT / 2,
        lite_rgb565(45u, 41u, 35u)
    );
    if (!surface_section) {
        return;
    }
    camera_leaf = c15_map_camera_leaf(map, camera);
    if (camera_leaf <= 0) {
        return;
    }
    if (g_cached_map != map ||
        g_cached_surface_bits != visible_surface_bits ||
        g_cached_leaf != camera_leaf) {
        uint32_t surface_index;
        g_cached_surface_count = 0u;
        if (!c15_map_build_visible(
                map, camera, visible_surface_bits, visible_surface_bytes,
                &g_cached_visible_leaves)) {
            g_cached_leaf = -1;
            return;
        }
        for (surface_index = 0u;
             surface_index < surface_section->count; ++surface_index) {
            if ((visible_surface_bits[surface_index >> 3] &
                 (uint8_t)(1u << (surface_index & 7u))) != 0u) {
                if (g_cached_surface_count >=
                    MAX_CACHED_VISIBLE_SURFACES) {
                    g_cached_leaf = -1;
                    return;
                }
                g_cached_surface_indices[g_cached_surface_count++] =
                    (uint16_t)surface_index;
            }
        }
        g_cached_map = map;
        g_cached_surface_bits = visible_surface_bits;
        g_cached_leaf = camera_leaf;
        visibility_changed = 1;
    }
    if (visibility_changed &&
        map->stream_textures) {
        /*
         * The PVS includes back-facing surfaces. Remove those from the
         * texture prefetch set so original-resolution packs do not spend the
         * 2 MiB working set on materials that cannot be drawn from here.
         */
        for (list_index = 0u;
             list_index < g_cached_surface_count; ++list_index) {
            uint32_t surface_index =
                g_cached_surface_indices[list_index];
            c15_surface_t surface;
            if (!c15_map_surface(map, surface_index, &surface) ||
                surface.texture_id >= map->texture_count ||
                map->textures[surface.texture_id].special ||
                !surface_front(map, camera, &surface)) {
                visible_surface_bits[surface_index >> 3] &=
                    (uint8_t)~(1u << (surface_index & 7u));
            }
        }
        if (!c15_map_prepare_visible_textures(
                map, visible_surface_bits, visible_surface_bytes)) {
            g_cached_leaf = -1;
            return;
        }
    }
    stats->visible_leaves = g_cached_visible_leaves;
    stats->visible_surfaces = g_cached_surface_count;
    for (list_index = 0u;
         list_index < g_cached_surface_count; ++list_index) {
        uint32_t surface_index = g_cached_surface_indices[list_index];
        c15_surface_t surface;
        const c15_texture_t *texture;
        view_vertex_t input[MAX_POLYGON_VERTICES];
        view_vertex_t clipped[MAX_POLYGON_VERTICES];
        view_vertex_t *clip_input = input;
        view_vertex_t *clip_output = clipped;
        screen_vertex_t projected[MAX_POLYGON_VERTICES];
        int clipped_count;
        int plane;
        uint32_t vertex;
        uint8_t clip_or = 0u;
        uint8_t clip_and = 0x1fu;
        if (!c15_map_surface(map, surface_index, &surface) ||
            surface.vertex_count < 3u ||
            surface.vertex_count > MAX_POLYGON_VERTICES ||
            surface.texture_id >= map->texture_count ||
            !surface_front(map, camera, &surface)) {
            continue;
        }
        if (map->textures[surface.texture_id].special ||
            !c15_map_ensure_texture(map, surface.texture_id)) {
            continue;
        }
        texture = &map->textures[surface.texture_id];
        for (vertex = 0u; vertex < surface.vertex_count; ++vertex) {
            c15_surface_vertex_t source;
            if (!c15_map_vertex(
                    map, surface.first_vertex + vertex, &source)) {
                break;
            }
            input[vertex] = transform_vertex(
                &source, camera, sine, cosine,
                pitch_sine, pitch_cosine
            );
            {
                uint8_t code = clip_code(&input[vertex], focal);
                clip_or |= code;
                clip_and &= code;
            }
        }
        if (vertex != surface.vertex_count) {
            continue;
        }
        if (clip_and != 0u) {
            continue;
        }
        clipped_count = (int)surface.vertex_count;
        if (clip_or != 0u) {
            for (plane = 0; plane < 5 && clipped_count >= 3; ++plane) {
                view_vertex_t *temporary;
                if ((clip_or & (uint8_t)(1u << plane)) == 0u) {
                    continue;
                }
                clipped_count = clip_plane(
                    clip_input, clipped_count, clip_output, plane, focal
                );
                temporary = clip_input;
                clip_input = clip_output;
                clip_output = temporary;
            }
        }
        if (clipped_count < 3) continue;
        for (vertex = 0u; vertex < (uint32_t)clipped_count; ++vertex) {
            projected[vertex] = project_vertex(
                &clip_input[vertex], focal
            );
        }
        for (vertex = 1u; vertex + 1u < (uint32_t)clipped_count; ++vertex) {
            draw_triangle(
                framebuffer, depth, texture, surface.light,
                projected[0], projected[vertex], projected[vertex + 1u],
                stats
            );
        }
        ++stats->drawn_surfaces;
    }
}
