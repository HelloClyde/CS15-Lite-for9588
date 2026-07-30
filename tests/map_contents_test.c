#include "world/map.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t streamed_visibility[4] = {0u, 1u, 0u, 0u};
static uint32_t streamed_visibility_size = 2u;
static uint32_t expected_visibility_offset;

int c15_pak_find(
    const c15_pak_t *pak,
    const char *name,
    c15_pak_entry_t *entry
)
{
    (void)pak;
    (void)name;
    (void)entry;
    return 0;
}

int c15_pak_read(
    const c15_pak_t *pak,
    const c15_pak_entry_t *entry,
    uint32_t relative_offset,
    void *destination,
    uint32_t size
)
{
    (void)pak;
    if (relative_offset == 24u && size == 128u &&
        (entry->offset == 1000u || entry->offset == 2000u)) {
        memset(destination, (int)(entry->offset / 1000u), size);
        return 1;
    }
    if (relative_offset != expected_visibility_offset ||
        size != streamed_visibility_size) {
        return 0;
    }
    memcpy(destination, streamed_visibility, size);
    return 1;
}

int c15_pak_validate_entry(
    const c15_pak_t *pak,
    const c15_pak_entry_t *entry,
    void *scratch,
    uint32_t scratch_size
)
{
    (void)pak;
    (void)entry;
    (void)scratch;
    (void)scratch_size;
    return 0;
}

uint32_t c15_crc32(const void *data, uint32_t size)
{
    (void)data;
    (void)size;
    return 0u;
}

void *lite_arena_alloc(lite_arena_t *arena, size_t size, size_t alignment)
{
    size_t aligned;
    if (!arena || !arena->base || alignment == 0u) {
        return 0;
    }
    aligned = (arena->used + alignment - 1u) & ~(alignment - 1u);
    if (aligned > arena->capacity ||
        size > arena->capacity - aligned) {
        ++arena->failures;
        return 0;
    }
    arena->used = aligned + size;
    if (arena->used > arena->peak) {
        arena->peak = arena->used;
    }
    ++arena->allocations;
    return arena->base + aligned;
}

void lite_arena_reset(lite_arena_t *arena)
{
    if (arena) {
        arena->used = 0u;
        arena->allocations = 0u;
    }
}

static void put_i16(uint8_t *target, int16_t value)
{
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)((uint16_t)value >> 8);
}

static void put_i32(uint8_t *target, int32_t value)
{
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)((uint32_t)value >> 8);
    target[2] = (uint8_t)((uint32_t)value >> 16);
    target[3] = (uint8_t)((uint32_t)value >> 24);
}

int main(void)
{
    uint8_t plane_data[16] = {0};
    uint8_t node_data[8] = {0};
    uint8_t leaf_data[24] = {0};
    uint8_t model_data[48] = {0};
    uint8_t mark_data[2] = {0};
    uint8_t surface_data[16] = {0};
    uint8_t stream_surface_data[32] = {0};
    uint8_t texture_storage[160] = {0};
    uint8_t dynamic_data[48] = {0};
    uint8_t ladder_data[14] = {0};
    uint8_t rescue_data[14] = {0};
    uint8_t surface_bits[1] = {0};
    uint8_t visible_leaf_bits[C15_MAP_VISIBILITY_BYTES] = {0};
    c15_map_section_t planes = {0};
    c15_map_section_t nodes = {0};
    c15_map_section_t leaves = {0};
    c15_map_section_t models = {0};
    c15_map_section_t marks = {0};
    c15_map_section_t surfaces = {0};
    c15_map_section_t visibility = {0};
    c15_map_section_t dynamics = {0};
    c15_map_section_t ladders = {0};
    c15_map_section_t rescues = {0};
    c15_pak_t pak = {0};
    c15_pak_entry_t entry = {0};
    c15_camera_t camera = {0};
    lite_arena_t texture_arena = {0};
    uint32_t visible_leaves = 0u;
    c15_map_t map;

    memset(&map, 0, sizeof(map));
    put_i16(plane_data, 16384);
    put_i32(node_data, 0);
    put_i16(node_data + 4, -1);
    put_i16(node_data + 6, -2);
    put_i32(leaf_data, -2);
    put_i32(leaf_data + 12, -1);
    put_i32(leaf_data + 16, 0);
    put_i16(leaf_data + 20, 0);
    put_i16(leaf_data + 22, 1);
    put_i32(model_data + 20, 0);
    put_i32(model_data + 36, 1);
    dynamic_data[0] = C15_DYNAMIC_DOOR;
    put_i16(dynamic_data + 4, -12);
    put_i16(dynamic_data + 6, -2);
    put_i16(dynamic_data + 8, -2);
    put_i16(dynamic_data + 10, -8);
    put_i16(dynamic_data + 12, 2);
    put_i16(dynamic_data + 14, 2);
    dynamic_data[24] = C15_DYNAMIC_BREAKABLE;
    put_i16(dynamic_data + 28, -22);
    put_i16(dynamic_data + 30, -2);
    put_i16(dynamic_data + 32, -2);
    put_i16(dynamic_data + 34, -18);
    put_i16(dynamic_data + 36, 2);
    put_i16(dynamic_data + 38, 2);
    put_i16(ladder_data, 20);
    put_i16(ladder_data + 2, 20);
    put_i16(ladder_data + 4, 0);
    put_i16(ladder_data + 6, 22);
    put_i16(ladder_data + 8, 22);
    put_i16(ladder_data + 10, 80);
    put_i16(rescue_data, 30);
    put_i16(rescue_data + 2, 30);
    put_i16(rescue_data + 4, 0);
    put_i16(rescue_data + 6, 60);
    put_i16(rescue_data + 8, 60);
    put_i16(rescue_data + 10, 80);

    planes.data = plane_data;
    planes.count = 1u;
    planes.stride = 16u;
    nodes.data = node_data;
    nodes.count = 1u;
    nodes.stride = 8u;
    leaves.data = leaf_data;
    leaves.count = 2u;
    leaves.stride = 12u;
    models.data = model_data;
    models.count = 1u;
    models.stride = 48u;
    marks.data = mark_data;
    marks.count = 1u;
    marks.stride = 2u;
    surfaces.data = surface_data;
    surfaces.count = 1u;
    surfaces.stride = 16u;
    visibility.data = 0;
    visibility.offset = 100u;
    visibility.size = streamed_visibility_size;
    visibility.count = streamed_visibility_size;
    visibility.stride = 1u;
    visibility.flags = 1u;
    dynamics.data = dynamic_data;
    dynamics.count = 2u;
    dynamics.stride = 24u;
    ladders.data = ladder_data;
    ladders.count = 1u;
    ladders.stride = 14u;
    rescues.data = rescue_data;
    rescues.count = 1u;
    rescues.stride = 14u;
    map.plane_section = &planes;
    map.node_section = &nodes;
    map.leaf_section = &leaves;
    map.model_section = &models;
    map.mark_section = &marks;
    map.surface_section = &surfaces;
    map.visibility_section = &visibility;
    map.dynamic_section = &dynamics;
    map.ladder_section = &ladders;
    map.rescue_zone_section = &rescues;
    map.pak = &pak;
    map.entry = entry;

    assert(c15_map_hull_contents(&map, 0u, 10, 0, 0) == -2);
    assert(c15_map_hull_contents(&map, 0u, -30, 0, 0) == -1);
    assert(c15_map_hull_contents(&map, 4u, -30, 0, 0) == -2);
    assert(c15_map_hull_contents(&map, 0u, -10, 0, 0) == -2);
    assert(c15_map_use_dynamic(&map, -10, 0, 0));
    for (int tick = 0; tick < 8; ++tick) {
        c15_map_dynamic_tick(&map);
    }
    assert(c15_map_hull_contents(&map, 0u, -10, 0, 0) == -1);
    assert(!c15_map_damage_breakable(&map, -20, 0, 0));
    assert(!c15_map_damage_breakable(&map, -20, 0, 0));
    assert(c15_map_damage_breakable(&map, -20, 0, 0));
    assert((map.dynamic_broken_bits & 2u) != 0u);
    assert(c15_map_on_ladder(&map, 0, 21, 20));
    assert(!c15_map_on_ladder(&map, -10, -10, 20));
    assert(c15_map_in_rescue_zone(&map, 40, 40, 20));
    assert(!c15_map_in_rescue_zone(&map, 0, 0, 20));
    camera.x = -10;
    expected_visibility_offset = 100u;
    assert(c15_map_build_visible(
        &map, &camera, surface_bits, sizeof(surface_bits),
        visible_leaf_bits, sizeof(visible_leaf_bits), &visible_leaves
    ));
    assert(surface_bits[0] == 1u);
    assert(visible_leaves == 1u);
    assert(visible_leaf_bits[0] == 0u);
    put_i32(leaf_data + 16, -1);
    assert(c15_map_build_visible(
        &map, &camera, surface_bits, sizeof(surface_bits),
        visible_leaf_bits, sizeof(visible_leaf_bits), &visible_leaves
    ));
    assert(visible_leaf_bits[0] == 0xffu);
    assert(visible_leaves == 1u);
    put_i32(leaf_data + 16, 0);

    /*
     * A bounded cache must discard an older PVS texture when the next
     * visible set cannot fit beside it, then load the new set cleanly.
     */
    memset(&map, 0, sizeof(map));
    put_i16(stream_surface_data + 6, 0);
    put_i16(stream_surface_data + 16 + 6, 1);
    surfaces.data = stream_surface_data;
    surfaces.count = 2u;
    surfaces.stride = 16u;
    texture_arena.base = texture_storage;
    texture_arena.capacity = sizeof(texture_storage);
    map.pak = &pak;
    map.surface_section = &surfaces;
    map.texture_arena = &texture_arena;
    map.texture_count = 2u;
    map.stream_textures = 1u;
    map.loaded = 1;
    map.textures[0].entry_offset = 1000u;
    map.textures[0].entry_size = 152u;
    map.textures[0].resident_bytes = 128u;
    map.textures[0].palette_count = 64u;
    map.textures[1].entry_offset = 2000u;
    map.textures[1].entry_size = 152u;
    map.textures[1].resident_bytes = 128u;
    map.textures[1].palette_count = 64u;
    surface_bits[0] = 1u;
    assert(c15_map_prepare_visible_textures(
        &map, surface_bits, sizeof(surface_bits)
    ));
    assert(map.textures[0].loaded);
    assert(!map.textures[1].loaded);
    surface_bits[0] = 2u;
    assert(c15_map_prepare_visible_textures(
        &map, surface_bits, sizeof(surface_bits)
    ));
    assert(!map.textures[0].loaded);
    assert(map.textures[1].loaded);
    assert(map.texture_cache_reloads == 1u);
    surface_bits[0] = 3u;
    assert(c15_map_prepare_visible_textures(
        &map, surface_bits, sizeof(surface_bits)
    ));
    assert(map.textures[0].loaded);
    assert(!map.textures[1].loaded);
    assert(c15_map_ensure_texture(&map, 1u));
    assert(!map.textures[0].loaded);
    assert(map.textures[1].loaded);
    assert(map.texture_cache_reloads == 3u);

    /*
     * cs_italy carries 2131 visibility leaves. This compressed all-hidden
     * row verifies maps beyond the old 2048-leaf stack-buffer limit.
     */
    memset(&map, 0, sizeof(map));
    surfaces.data = surface_data;
    surfaces.count = 1u;
    surfaces.stride = 16u;
    map.plane_section = &planes;
    map.node_section = &nodes;
    map.leaf_section = &leaves;
    map.model_section = &models;
    map.mark_section = &marks;
    map.surface_section = &surfaces;
    map.visibility_section = &visibility;
    map.pak = &pak;
    map.entry = entry;
    put_i32(model_data + 36, 2131);
    leaves.count = 2132u;
    streamed_visibility[0] = 0u;
    streamed_visibility[1] = 255u;
    streamed_visibility[2] = 0u;
    streamed_visibility[3] = 12u;
    streamed_visibility_size = 4u;
    visibility.size = streamed_visibility_size;
    visibility.count = streamed_visibility_size;
    surface_bits[0] = 0u;
    visible_leaves = 0u;
    assert(c15_map_build_visible(
        &map, &camera, surface_bits, sizeof(surface_bits),
        0, 0u, &visible_leaves
    ));
    assert(surface_bits[0] == 1u);
    assert(visible_leaves == 1u);

    puts("map_contents_test: PASS");
    return 0;
}
