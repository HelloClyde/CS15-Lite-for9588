#include "world/map.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const c15_pak_entry_t *c15_pak_find(
    const c15_pak_t *pak, const char *name
)
{
    (void)pak;
    (void)name;
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
    (void)relative_offset;
    (void)destination;
    (void)size;
    return 0;
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
    uint8_t node_data[24] = {0};
    uint8_t leaf_data[56] = {0};
    uint8_t model_data[48] = {0};
    c15_map_section_t planes = {0};
    c15_map_section_t nodes = {0};
    c15_map_section_t leaves = {0};
    c15_map_section_t models = {0};
    c15_map_t map;

    memset(&map, 0, sizeof(map));
    put_i16(plane_data, 16384);
    put_i32(node_data, 0);
    put_i16(node_data + 4, -1);
    put_i16(node_data + 6, -2);
    put_i32(leaf_data, -2);
    put_i32(leaf_data + 28, -1);
    put_i32(model_data + 20, 0);

    planes.data = plane_data;
    planes.count = 1u;
    planes.stride = 16u;
    nodes.data = node_data;
    nodes.count = 1u;
    nodes.stride = 24u;
    leaves.data = leaf_data;
    leaves.count = 2u;
    leaves.stride = 28u;
    models.data = model_data;
    models.count = 1u;
    models.stride = 48u;
    map.plane_section = &planes;
    map.node_section = &nodes;
    map.leaf_section = &leaves;
    map.model_section = &models;

    assert(c15_map_hull_contents(&map, 0u, 10, 0, 0) == -2);
    assert(c15_map_hull_contents(&map, 0u, -10, 0, 0) == -1);
    assert(c15_map_hull_contents(&map, 4u, -10, 0, 0) == -2);
    puts("map_contents_test: PASS");
    return 0;
}
