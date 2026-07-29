/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_MAP_H
#define CS15_LITE_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "assets/pak.h"
#include "core/memory.h"

#define C15_MAP_MAX_SECTIONS 24u
#define C15_MAP_MAX_TEXTURES 256u
#define C15_MAP_VISIBLE_BYTES 1280u
#define C15_MAP_MAX_DYNAMIC_ENTITIES 32u

typedef struct c15_map_section {
    uint32_t type;
    uint32_t size;
    uint32_t count;
    uint32_t offset;
    uint16_t stride;
    uint16_t flags;
    const uint8_t *data;
} c15_map_section_t;

typedef struct c15_texture {
    uint32_t entry_offset;
    uint32_t entry_size;
    uint32_t resident_bytes;
    uint16_t width;
    uint16_t height;
    uint16_t flags;
    uint16_t palette_count;
    const uint16_t *palette;
    const uint8_t *pixels;
    uint16_t width_mask;
    uint16_t height_mask;
    uint16_t width_reciprocal;
    uint16_t height_reciprocal;
    char name[17];
    uint8_t special;
    uint8_t loaded;
} c15_texture_t;

typedef struct c15_camera {
    int32_t x;
    int32_t y;
    int32_t z;
    uint8_t yaw;
    int8_t pitch;
    uint16_t yaw_q8;
    int16_t pitch_q8;
    uint16_t focal_length;
} c15_camera_t;

enum c15_dynamic_kind {
    C15_DYNAMIC_DOOR = 1,
    C15_DYNAMIC_BUTTON = 2,
    C15_DYNAMIC_BREAKABLE = 3,
    C15_DYNAMIC_PLATFORM = 4
};

typedef struct c15_dynamic_entity {
    int16_t minimum_x;
    int16_t minimum_y;
    int16_t minimum_z;
    int16_t maximum_x;
    int16_t maximum_y;
    int16_t maximum_z;
    uint32_t target_hash;
    uint32_t targetname_hash;
    uint16_t model;
    uint8_t kind;
    uint8_t flags;
} c15_dynamic_entity_t;

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
    lite_arena_t *texture_arena;
    c15_pak_entry_t entry;
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
    const c15_map_section_t *bomb_site_section;
    const c15_map_section_t *hostage_section;
    const c15_map_section_t *rescue_zone_section;
    const c15_map_section_t *buy_zone_section;
    const c15_map_section_t *ladder_section;
    const c15_map_section_t *dynamic_section;
    c15_texture_t textures[C15_MAP_MAX_TEXTURES];
    uint32_t texture_count;
    c15_camera_t spawn;
    uint32_t source_crc32;
    uint32_t texture_resident_bytes;
    uint32_t texture_cache_bytes;
    uint32_t texture_cache_reloads;
    uint32_t dynamic_open_bits;
    uint32_t dynamic_broken_bits;
    uint8_t dynamic_damage[C15_MAP_MAX_DYNAMIC_ENTITIES];
    uint8_t dynamic_position[C15_MAP_MAX_DYNAMIC_ENTITIES];
    uint8_t stream_textures;
    uint8_t load_error;
    int loaded;
} c15_map_t;

int c15_map_load(
    c15_map_t *map,
    const c15_pak_t *pak,
    const char *map_name,
    lite_arena_t *map_arena,
    lite_arena_t *texture_arena,
    int stream_textures,
    void *scratch,
    uint32_t scratch_size
);
int c15_map_prepare_visible_textures(
    c15_map_t *map,
    const uint8_t *surface_bits,
    uint32_t surface_bits_size
);
int c15_map_ensure_texture(c15_map_t *map, uint32_t texture_index);
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
uint32_t c15_map_bomb_site_count(const c15_map_t *map);
int c15_map_bomb_site(
    const c15_map_t *map,
    uint32_t index,
    int32_t *x,
    int32_t *y,
    int32_t *z
);
uint32_t c15_map_hostage_count(const c15_map_t *map);
int c15_map_hostage(
    const c15_map_t *map,
    uint32_t index,
    int32_t *x,
    int32_t *y,
    int32_t *z
);
int c15_map_in_rescue_zone(
    const c15_map_t *map, int32_t x, int32_t y, int32_t z
);
int c15_map_in_buy_zone(
    const c15_map_t *map,
    uint8_t team,
    int32_t x,
    int32_t y,
    int32_t z
);
int c15_map_on_ladder(
    const c15_map_t *map, int32_t x, int32_t y, int32_t z
);
uint32_t c15_map_dynamic_count(const c15_map_t *map);
int c15_map_dynamic(
    const c15_map_t *map,
    uint32_t index,
    c15_dynamic_entity_t *entity
);
int c15_map_use_dynamic(
    c15_map_t *map, int32_t x, int32_t y, int32_t z
);
int c15_map_damage_breakable(
    c15_map_t *map, int32_t x, int32_t y, int32_t z
);
void c15_map_dynamic_tick(c15_map_t *map);
int16_t c15_sin_q14(uint8_t angle);
int16_t c15_cos_q14(uint8_t angle);

#endif
