/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_WORLD_RENDERER_H
#define CS15_LITE_WORLD_RENDERER_H

#include <stdint.h>

#include "render/framebuffer.h"
#include "world/map.h"

typedef struct c15_render_stats {
    uint32_t visible_leaves;
    uint32_t visible_surfaces;
    uint32_t drawn_surfaces;
    uint32_t drawn_triangles;
    uint32_t drawn_pixels;
} c15_render_stats_t;

void c15_render_world(
    c15_map_t *map,
    const c15_camera_t *camera,
    lite_framebuffer_t *framebuffer,
    uint16_t *depth,
    uint8_t *visible_surface_bits,
    uint32_t visible_surface_bytes,
    c15_render_stats_t *stats
);

#endif
