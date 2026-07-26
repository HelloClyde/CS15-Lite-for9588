#include "world/map.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t streamed_visibility[] = {0u, 1u};
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
    (void)entry;
    if (relative_offset != expected_visibility_offset ||
        size != sizeof(streamed_visibility)) {
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
    (void)arena;
    (void)size;
    (void)alignment;
    return 0;
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
    uint8_t surface_bits[1] = {0};
    c15_map_section_t planes = {0};
    c15_map_section_t nodes = {0};
    c15_map_section_t leaves = {0};
    c15_map_section_t models = {0};
    c15_map_section_t marks = {0};
    c15_map_section_t surfaces = {0};
    c15_map_section_t visibility = {0};
    c15_pak_t pak = {0};
    c15_pak_entry_t entry = {0};
    c15_camera_t camera = {0};
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
    visibility.size = sizeof(streamed_visibility);
    visibility.count = sizeof(streamed_visibility);
    visibility.stride = 1u;
    visibility.flags = 1u;
    map.plane_section = &planes;
    map.node_section = &nodes;
    map.leaf_section = &leaves;
    map.model_section = &models;
    map.mark_section = &marks;
    map.surface_section = &surfaces;
    map.visibility_section = &visibility;
    map.pak = &pak;
    map.entry = entry;

    assert(c15_map_hull_contents(&map, 0u, 10, 0, 0) == -2);
    assert(c15_map_hull_contents(&map, 0u, -10, 0, 0) == -1);
    assert(c15_map_hull_contents(&map, 4u, -10, 0, 0) == -2);
    camera.x = -10;
    expected_visibility_offset = 100u;
    assert(c15_map_build_visible(
        &map, &camera, surface_bits, sizeof(surface_bits), &visible_leaves
    ));
    assert(surface_bits[0] == 1u);
    assert(visible_leaves == 1u);
    puts("map_contents_test: PASS");
    return 0;
}
