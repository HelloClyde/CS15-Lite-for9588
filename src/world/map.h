/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_MAP_H
#define CS15_LITE_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "assets/pak.h"
#include "core/memory.h"

#define C15_MAP_MAX_SECTIONS 16u
#define C15_MAP_MAX_TEXTURES 64u
#define C15_MAP_VISIBLE_BYTES 704u
#define C15_TEXTURE_LIGHT_LEVELS 4u

typedef struct c15_map_section {
    uint32_t type;
    uint32_t size;
    uint32_t count;
    uint16_t stride;
    uint16_t flags;
    const uint8_t *data;
} c15_map_section_t;

typedef struct c15_texture {
    uint16_t width;
    uint16_t height;
    uint16_t flags;
    const uint16_t *palette;
    const uint16_t *shaded_palettes;
    const uint8_t *pixels;
    uint16_t width_mask;
    uint16_t height_mask;
    uint16_t width_reciprocal;
    uint16_t height_reciprocal;
    char name[17];
    uint8_t special;
} c15_texture_t;

typedef struct c15_camera {
    int32_t x;
    int32_t y;
    int32_t z;
    uint8_t yaw;
    int8_t pitch;
    uint16_t yaw_q8;
    int16_t pitch_q8;
} c15_camera_t;

typedef struct c15_surface {
    uint32_t first_vertex;
    uint16_t vertex_count;
    uint16_t texture_id;
    uint16_t plane_id;
    uint16_t flags;
    uint8_t light;
    uint8_t light_style;
} c15_surface_t;

typedef struct c15_surface_vertex {
    int16_t x;
    int16_t y;
    int16_t z;
    int16_t u;
    int16_t v;
} c15_surface_vertex_t;

typedef struct c15_plane {
    int16_t nx;
    int16_t ny;
    int16_t nz;
    int32_t distance_q4;
    uint8_t type;
    uint8_t signbits;
} c15_plane_t;

typedef struct c15_map {
    const c15_pak_t *pak;
    const c15_pak_entry_t *entry;
    c15_map_section_t sections[C15_MAP_MAX_SECTIONS];
    uint32_t section_count;
    const c15_map_section_t *vertex_section;
    const c15_map_section_t *surface_section;
    const c15_map_section_t *plane_section;
    const c15_map_section_t *node_section;
    const c15_map_section_t *leaf_section;
    const c15_map_section_t *mark_section;
    const c15_map_section_t *visibility_section;
    const c15_map_section_t *clip_section;
    const c15_map_section_t *model_section;
    c15_texture_t textures[C15_MAP_MAX_TEXTURES];
    uint32_t texture_count;
    c15_camera_t spawn;
    uint32_t source_crc32;
    int loaded;
} c15_map_t;

int c15_map_load(
    c15_map_t *map,
    const c15_pak_t *pak,
    const char *map_name,
    lite_arena_t *map_arena,
    lite_arena_t *texture_arena,
    void *scratch,
    uint32_t scratch_size
);
const c15_map_section_t *c15_map_section(
    const c15_map_t *map, uint32_t type
);
int c15_map_surface(
    const c15_map_t *map, uint32_t index, c15_surface_t *surface
);
int c15_map_vertex(
    const c15_map_t *map, uint32_t index, c15_surface_vertex_t *vertex
);
int c15_map_plane(
    const c15_map_t *map, uint32_t index, c15_plane_t *plane
);
int c15_map_build_visible(
    const c15_map_t *map,
    const c15_camera_t *camera,
    uint8_t *surface_bits,
    uint32_t surface_bits_size,
    uint32_t *visible_leaf_count
);
int c15_map_camera_leaf(
    const c15_map_t *map, const c15_camera_t *camera
);
int c15_map_hull_contents(
    const c15_map_t *map,
    uint32_t hull,
    int32_t x,
    int32_t y,
    int32_t z
);
uint32_t c15_map_spawn_count(const c15_map_t *map);
int c15_map_spawn(
    const c15_map_t *map,
    uint32_t index,
    c15_camera_t *camera,
    uint8_t *team
);
int16_t c15_sin_q14(uint8_t angle);
int16_t c15_cos_q14(uint8_t angle);

#endif
