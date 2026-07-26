/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world/map.h"

#include "bda_memory.h"

#define BSP_HEADER_BYTES 64u
#define BSP_SECTION_BYTES 32u
#define BSP_VERSION 2u
#define TEX_HEADER_BYTES 24u
#define BSP_SECTION_STREAMED 1u

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static int16_t read_i16(const uint8_t *data)
{
    return (int16_t)read_u16(data);
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static int32_t read_i32(const uint8_t *data)
{
    return (int32_t)read_u32(data);
}

static int bytes_equal(const uint8_t *left, const char *right, uint32_t count)
{
    uint32_t index;
    for (index = 0u; index < count; ++index) {
        if (left[index] != (uint8_t)right[index]) {
            return 0;
        }
    }
    return 1;
}

static void build_name(char *out, const char *prefix, const char *name)
{
    while (*prefix) {
        *out++ = *prefix++;
    }
    while (*name) {
        *out++ = *name++;
    }
    *out = 0;
}

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix) {
        if (*text++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

static int is_power_of_two(uint16_t value)
{
    return value != 0u && (value & (uint16_t)(value - 1u)) == 0u;
}

const c15_map_section_t *c15_map_section(
    const c15_map_t *map, uint32_t type
)
{
    uint32_t index;
    if (!map) {
        return 0;
    }
    for (index = 0u; index < map->section_count; ++index) {
        if (map->sections[index].type == type) {
            return &map->sections[index];
        }
    }
    return 0;
}

static int load_sections(
    c15_map_t *map,
    lite_arena_t *arena,
    void *scratch,
    uint32_t scratch_size
)
{
    uint8_t header[BSP_HEADER_BYTES];
    uint8_t record[BSP_SECTION_BYTES];
    uint32_t count;
    uint32_t index;
    if (!c15_pak_read(
            map->pak, &map->entry, 0u, header, sizeof(header)) ||
        !bytes_equal(header, "C15BSP1\0", 8u) ||
        read_u32(header + 8) != BSP_VERSION ||
        read_u32(header + 16) != map->entry.packed_size) {
        return 0;
    }
    count = read_u32(header + 12);
    if (count == 0u || count > C15_MAP_MAX_SECTIONS ||
        BSP_HEADER_BYTES + count * BSP_SECTION_BYTES >
            map->entry.packed_size) {
        return 0;
    }
    map->source_crc32 = read_u32(header + 20);
    map->section_count = count;
    for (index = 0u; index < count; ++index) {
        c15_map_section_t *section = &map->sections[index];
        uint32_t offset;
        uint32_t checksum;
        uint8_t *destination;
        if (!c15_pak_read(
                map->pak, &map->entry,
                BSP_HEADER_BYTES + index * BSP_SECTION_BYTES,
                record, sizeof(record))) {
            return 0;
        }
        section->type = read_u32(record);
        offset = read_u32(record + 4);
        section->offset = offset;
        section->size = read_u32(record + 8);
        section->count = read_u32(record + 12);
        section->stride = read_u16(record + 16);
        section->flags = read_u16(record + 18);
        checksum = read_u32(record + 20);
        if ((offset & 15u) != 0u ||
            offset > map->entry.packed_size - section->size ||
            (section->stride != 0u &&
             section->count * section->stride != section->size)) {
            return 0;
        }
        if ((section->flags & BSP_SECTION_STREAMED) != 0u) {
            if (section->type != C15_FOURCC('V','I','S','I')) {
                return 0;
            }
            section->data = 0;
        } else {
            destination = (uint8_t *)lite_arena_alloc(
                arena, section->size ? section->size : 1u, 16u
            );
            if (!destination ||
                !c15_pak_read(
                    map->pak, &map->entry, offset,
                    destination, section->size) ||
                c15_crc32(destination, section->size) != checksum) {
                return 0;
            }
            section->data = destination;
        }
    }
    (void)scratch;
    (void)scratch_size;
    return 1;
}

static int cache_sections(c15_map_t *map)
{
    map->vertex_section = c15_map_section(
        map, C15_FOURCC('V','E','R','T')
    );
    map->surface_section = c15_map_section(
        map, C15_FOURCC('S','U','R','F')
    );
    map->plane_section = c15_map_section(
        map, C15_FOURCC('P','L','A','N')
    );
    map->node_section = c15_map_section(
        map, C15_FOURCC('N','O','D','E')
    );
    map->leaf_section = c15_map_section(
        map, C15_FOURCC('L','E','A','F')
    );
    map->mark_section = c15_map_section(
        map, C15_FOURCC('M','A','R','K')
    );
    map->visibility_section = c15_map_section(
        map, C15_FOURCC('V','I','S','I')
    );
    map->clip_section = c15_map_section(
        map, C15_FOURCC('C','L','I','P')
    );
    map->model_section = c15_map_section(
        map, C15_FOURCC('M','O','D','L')
    );
    map->bomb_site_section = c15_map_section(
        map, C15_FOURCC('B','S','I','T')
    );
    return map->vertex_section && map->surface_section &&
        map->plane_section && map->node_section && map->leaf_section &&
        map->mark_section && map->visibility_section &&
        map->clip_section && map->model_section &&
        map->bomb_site_section;
}

static int load_textures(c15_map_t *map, lite_arena_t *arena)
{
    const c15_map_section_t *names = c15_map_section(
        map, C15_FOURCC('T','N','A','M')
    );
    uint32_t index;
    if (!names || names->stride != 16u ||
        names->count > C15_MAP_MAX_TEXTURES) {
        return 0;
    }
    map->texture_count = names->count;
    for (index = 0u; index < names->count; ++index) {
        c15_texture_t *texture = &map->textures[index];
        const uint8_t *source_name = names->data + index * 16u;
        char asset_name[32];
        uint8_t header[TEX_HEADER_BYTES];
        c15_pak_entry_t entry;
        uint8_t *storage;
        uint32_t resident;
        uint32_t palette_bytes;
        uint32_t name_index;
        for (name_index = 0u; name_index < 16u; ++name_index) {
            texture->name[name_index] = (char)source_name[name_index];
        }
        texture->name[16] = 0;
        build_name(asset_name, "tex/", texture->name);
        if (!c15_pak_find(map->pak, asset_name, &entry) ||
            entry.type != C15_FOURCC('T','E','X','0') ||
            !c15_pak_read(
                map->pak, &entry, 0u, header, sizeof(header)) ||
            !bytes_equal(header, "CTX1", 4u)) {
            return 0;
        }
        texture->width = read_u16(header + 4);
        texture->height = read_u16(header + 6);
        texture->flags = read_u16(header + 8);
        texture->palette_count = read_u16(header + 10);
        palette_bytes = (uint32_t)texture->palette_count * 2u;
        resident = palette_bytes + read_u32(header + 12);
        if (texture->width == 0u || texture->height == 0u ||
            (texture->palette_count != 64u &&
             texture->palette_count != 256u) ||
            read_u32(header + 12) !=
                (uint32_t)texture->width * (uint32_t)texture->height ||
            entry.packed_size != TEX_HEADER_BYTES + resident) {
            return 0;
        }
        storage = (uint8_t *)lite_arena_alloc(arena, resident, 16u);
        if (!storage ||
            !c15_pak_read(
                map->pak, &entry, TEX_HEADER_BYTES, storage, resident)) {
            return 0;
        }
        texture->palette = (const uint16_t *)storage;
        texture->pixels = storage + palette_bytes;
        texture->width_mask = is_power_of_two(texture->width) ?
            (uint16_t)(texture->width - 1u) : 0u;
        texture->height_mask = is_power_of_two(texture->height) ?
            (uint16_t)(texture->height - 1u) : 0u;
        texture->width_reciprocal =
            (uint16_t)(65536u / texture->width);
        texture->height_reciprocal =
            (uint16_t)(65536u / texture->height);
        texture->special = (uint8_t)(
            starts_with(texture->name, "sky") ||
            starts_with(texture->name, "aaatrigger") ||
            starts_with(texture->name, "clip") ||
            starts_with(texture->name, "origin")
        );
    }
    return 1;
}

static int load_spawn(c15_map_t *map)
{
    const c15_map_section_t *spawns = c15_map_section(
        map, C15_FOURCC('S','P','W','N')
    );
    uint32_t index;
    const uint8_t *selected = 0;
    if (!spawns || spawns->stride != 10u || spawns->count == 0u) {
        return 0;
    }
    for (index = 0u; index < spawns->count; ++index) {
        const uint8_t *spawn = spawns->data + index * spawns->stride;
        if (!selected || spawn[8] == 1u) {
            selected = spawn;
        }
        if (spawn[8] == 1u) {
            break;
        }
    }
    map->spawn.x = read_i16(selected + 0);
    map->spawn.y = read_i16(selected + 2);
    map->spawn.z = read_i16(selected + 4) + 28;
    map->spawn.yaw = (uint8_t)(
        ((int32_t)read_i16(selected + 6) * 256) / 360
    );
    map->spawn.pitch = 0;
    map->spawn.yaw_q8 = (uint16_t)((uint16_t)map->spawn.yaw << 8);
    map->spawn.pitch_q8 = 0;
    return 1;
}

uint32_t c15_map_spawn_count(const c15_map_t *map)
{
    const c15_map_section_t *spawns = c15_map_section(
        map, C15_FOURCC('S','P','W','N')
    );
    if (!spawns || spawns->stride != 10u) {
        return 0u;
    }
    return spawns->count;
}

int c15_map_spawn(
    const c15_map_t *map,
    uint32_t index,
    c15_camera_t *camera,
    uint8_t *team
)
{
    const c15_map_section_t *spawns = c15_map_section(
        map, C15_FOURCC('S','P','W','N')
    );
    const uint8_t *spawn;
    if (!spawns || spawns->stride != 10u || index >= spawns->count ||
        !camera || !team) {
        return 0;
    }
    spawn = spawns->data + index * spawns->stride;
    camera->x = read_i16(spawn + 0);
    camera->y = read_i16(spawn + 2);
    camera->z = read_i16(spawn + 4) + 28;
    camera->yaw = (uint8_t)(
        ((int32_t)read_i16(spawn + 6) * 256) / 360
    );
    camera->pitch = 0;
    camera->yaw_q8 = (uint16_t)((uint16_t)camera->yaw << 8);
    camera->pitch_q8 = 0;
    *team = spawn[8];
    return *team == 1u || *team == 2u;
}

uint32_t c15_map_bomb_site_count(const c15_map_t *map)
{
    const c15_map_section_t *sites = map ? map->bomb_site_section : 0;
    if (!sites || sites->stride != 6u) {
        return 0u;
    }
    return sites->count;
}

int c15_map_bomb_site(
    const c15_map_t *map,
    uint32_t index,
    int32_t *x,
    int32_t *y,
    int32_t *z
)
{
    const c15_map_section_t *sites = map ? map->bomb_site_section : 0;
    const uint8_t *site;
    if (!sites || sites->stride != 6u || index >= sites->count ||
        !x || !y || !z) {
        return 0;
    }
    site = sites->data + index * sites->stride;
    *x = read_i16(site);
    *y = read_i16(site + 2);
    *z = read_i16(site + 4);
    return 1;
}

int c15_map_load(
    c15_map_t *map,
    const c15_pak_t *pak,
    const char *map_name,
    lite_arena_t *map_arena,
    lite_arena_t *texture_arena,
    void *scratch,
    uint32_t scratch_size
)
{
    if (!map || !pak || !map_name || !map_arena || !texture_arena ||
        !scratch || scratch_size == 0u) {
        return 0;
    }
    bda_memset(map, 0, sizeof(*map));
    map->pak = pak;
    if (!c15_pak_find(pak, map_name, &map->entry) ||
        map->entry.type != C15_FOURCC('B','S','P','0') ||
        !c15_pak_validate_entry(
            pak, &map->entry, scratch, scratch_size) ||
        !load_sections(map, map_arena, scratch, scratch_size) ||
        !cache_sections(map) ||
        !load_textures(map, texture_arena) ||
        !load_spawn(map)) {
        return 0;
    }
    map->loaded = 1;
    return 1;
}

int c15_map_surface(
    const c15_map_t *map, uint32_t index, c15_surface_t *surface
)
{
    const c15_map_section_t *section = map ? map->surface_section : 0;
    const uint8_t *data;
    if (!section || !surface || section->stride != 16u ||
        index >= section->count) {
        return 0;
    }
    data = section->data + index * section->stride;
    surface->first_vertex = read_u32(data);
    surface->vertex_count = read_u16(data + 4);
    surface->texture_id = read_u16(data + 6);
    surface->plane_id = read_u16(data + 8);
    surface->flags = read_u16(data + 10);
    surface->light = data[12];
    surface->light_style = data[13];
    return 1;
}

int c15_map_vertex(
    const c15_map_t *map, uint32_t index, c15_surface_vertex_t *vertex
)
{
    const c15_map_section_t *section = map ? map->vertex_section : 0;
    const uint8_t *data;
    if (!section || !vertex || section->stride != 10u ||
        index >= section->count) {
        return 0;
    }
    data = section->data + index * section->stride;
    vertex->x = read_i16(data);
    vertex->y = read_i16(data + 2);
    vertex->z = read_i16(data + 4);
    vertex->u = read_i16(data + 6);
    vertex->v = read_i16(data + 8);
    return 1;
}

int c15_map_plane(
    const c15_map_t *map, uint32_t index, c15_plane_t *plane
)
{
    const c15_map_section_t *section = map ? map->plane_section : 0;
    const uint8_t *data;
    if (!section || !plane || section->stride != 16u ||
        index >= section->count) {
        return 0;
    }
    data = section->data + index * section->stride;
    plane->nx = read_i16(data);
    plane->ny = read_i16(data + 2);
    plane->nz = read_i16(data + 4);
    plane->distance_q4 = read_i32(data + 6);
    plane->type = data[10];
    plane->signbits = data[11];
    return 1;
}

int c15_map_camera_leaf(
    const c15_map_t *map, const c15_camera_t *camera
)
{
    const c15_map_section_t *nodes = map ? map->node_section : 0;
    int32_t node_index = 0;
    uint32_t guard = 0u;
    if (!nodes || nodes->stride != 8u) {
        return 0;
    }
    while (node_index >= 0 && guard++ <= nodes->count) {
        const uint8_t *node;
        c15_plane_t plane;
        int64_t distance;
        int side;
        if ((uint32_t)node_index >= nodes->count) {
            return 0;
        }
        node = nodes->data + (uint32_t)node_index * nodes->stride;
        if (!c15_map_plane(map, read_u32(node), &plane)) {
            return 0;
        }
        distance =
            (int64_t)plane.nx * camera->x +
            (int64_t)plane.ny * camera->y +
            (int64_t)plane.nz * camera->z -
            ((int64_t)plane.distance_q4 << 10);
        side = distance < 0 ? 1 : 0;
        node_index = read_i16(node + 4 + side * 2);
    }
    return node_index < 0 ? -1 - node_index : 0;
}

static void mark_leaf_surfaces(
    const c15_map_section_t *leaves,
    const c15_map_section_t *marks,
    uint32_t leaf_index,
    uint8_t *surface_bits,
    uint32_t surface_bits_size
)
{
    const uint8_t *leaf;
    uint32_t first;
    uint32_t count;
    uint32_t index;
    if (leaf_index >= leaves->count) {
        return;
    }
    leaf = leaves->data + leaf_index * leaves->stride;
    first = read_u16(leaf + 8);
    count = read_u16(leaf + 10);
    if (first > marks->count || count > marks->count - first) {
        return;
    }
    for (index = 0u; index < count; ++index) {
        uint32_t surface = read_u16(marks->data + (first + index) * 2u);
        if ((surface >> 3) < surface_bits_size) {
            surface_bits[surface >> 3] |= (uint8_t)(1u << (surface & 7u));
        }
    }
}

int c15_map_build_visible(
    const c15_map_t *map,
    const c15_camera_t *camera,
    uint8_t *surface_bits,
    uint32_t surface_bits_size,
    uint32_t *visible_leaf_count
)
{
    const c15_map_section_t *surfaces =
        map ? map->surface_section : 0;
    const c15_map_section_t *leaves = map ? map->leaf_section : 0;
    const c15_map_section_t *marks = map ? map->mark_section : 0;
    const c15_map_section_t *visibility =
        map ? map->visibility_section : 0;
    const c15_map_section_t *models = map ? map->model_section : 0;
    uint8_t leaf_bits[256];
    uint8_t encoded_visibility[512];
    const uint8_t *visibility_data;
    uint32_t visibility_size;
    uint32_t row_bytes;
    uint32_t visibility_leaves;
    int leaf_index;
    int32_t visibility_offset;
    uint32_t input = 0u;
    uint32_t output = 0u;
    uint32_t leaf;
    if (!surfaces || !leaves || !marks || !visibility || !models ||
        leaves->stride != 12u || marks->stride != 2u ||
        models->stride != 48u || models->count == 0u ||
        surface_bits_size < (surfaces->count + 7u) / 8u) {
        return 0;
    }
    bda_memset(surface_bits, 0, surface_bits_size);
    bda_memset(leaf_bits, 0, sizeof(leaf_bits));
    visibility_leaves = (uint32_t)read_i32(models->data + 36u);
    row_bytes = (visibility_leaves + 7u) >> 3;
    if (visibility_leaves == 0u ||
        visibility_leaves >= leaves->count ||
        row_bytes > sizeof(leaf_bits)) {
        return 0;
    }
    leaf_index = c15_map_camera_leaf(map, camera);
    if (leaf_index <= 0 || (uint32_t)leaf_index >= leaves->count) {
        return 0;
    }
    visibility_offset = read_i32(
        leaves->data + (uint32_t)leaf_index * leaves->stride + 4
    );
    if (visibility_offset < 0) {
        for (output = 0u; output < row_bytes; ++output) {
            leaf_bits[output] = 0xffu;
        }
    } else {
        if (visibility->data) {
            visibility_data = visibility->data;
            visibility_size = visibility->size;
            input = (uint32_t)visibility_offset;
        } else {
            uint32_t read_size;
            if ((uint32_t)visibility_offset >= visibility->size) {
                return 0;
            }
            read_size = visibility->size -
                (uint32_t)visibility_offset;
            if (read_size > sizeof(encoded_visibility)) {
                read_size = sizeof(encoded_visibility);
            }
            if (!c15_pak_read(
                    map->pak, &map->entry,
                    visibility->offset + (uint32_t)visibility_offset,
                    encoded_visibility, read_size)) {
                return 0;
            }
            visibility_data = encoded_visibility;
            visibility_size = read_size;
            input = 0u;
        }
        output = 0u;
        while (output < row_bytes && input < visibility_size) {
            uint8_t value = visibility_data[input++];
            if (value != 0u) {
                leaf_bits[output++] = value;
            } else {
                uint32_t run;
                if (input >= visibility_size) {
                    return 0;
                }
                run = visibility_data[input++];
                if (run == 0u) {
                    return 0;
                }
                if (run > row_bytes - output) {
                    output = row_bytes;
                } else {
                    output += run;
                }
            }
        }
        if (output != row_bytes) {
            return 0;
        }
    }
    mark_leaf_surfaces(
        leaves, marks, (uint32_t)leaf_index,
        surface_bits, surface_bits_size
    );
    if (visible_leaf_count) {
        *visible_leaf_count = 1u;
    }
    for (leaf = 1u;
         leaf <= visibility_leaves && leaf < leaves->count; ++leaf) {
        if ((leaf_bits[(leaf - 1u) >> 3] &
             (uint8_t)(1u << ((leaf - 1u) & 7u))) != 0u) {
            mark_leaf_surfaces(
                leaves, marks, leaf, surface_bits, surface_bits_size
            );
            if (visible_leaf_count) {
                ++*visible_leaf_count;
            }
        }
    }
    return 1;
}

int c15_map_hull_contents(
    const c15_map_t *map,
    uint32_t hull,
    int32_t x,
    int32_t y,
    int32_t z
)
{
    const c15_map_section_t *clips = map ? map->clip_section : 0;
    const c15_map_section_t *nodes = map ? map->node_section : 0;
    const c15_map_section_t *leaves = map ? map->leaf_section : 0;
    const c15_map_section_t *models = map ? map->model_section : 0;
    int32_t node_index;
    uint32_t guard = 0u;
    if (!models || hull > 3u || models->stride != 48u ||
        models->count == 0u) {
        return -2;
    }
    if (hull == 0u) {
        if (!nodes || !leaves ||
            nodes->stride != 8u || leaves->stride != 12u) {
            return -2;
        }
        node_index = read_i32(models->data + 20u);
        while (node_index >= 0 && guard++ <= nodes->count) {
            const uint8_t *node;
            c15_plane_t plane;
            int64_t distance;
            int side;
            if ((uint32_t)node_index >= nodes->count) {
                return -2;
            }
            node = nodes->data + (uint32_t)node_index * nodes->stride;
            if (!c15_map_plane(map, read_u32(node), &plane)) {
                return -2;
            }
            distance =
                (int64_t)plane.nx * x +
                (int64_t)plane.ny * y +
                (int64_t)plane.nz * z -
                ((int64_t)plane.distance_q4 << 10);
            side = distance < 0 ? 1 : 0;
            node_index = read_i16(node + 4 + side * 2);
        }
        node_index = -1 - node_index;
        if (node_index < 0 || (uint32_t)node_index >= leaves->count) {
            return -2;
        }
        return read_i32(
            leaves->data + (uint32_t)node_index * leaves->stride
        );
    }
    if (!clips || clips->stride != 8u) {
        return -2;
    }
    node_index = read_i32(models->data + 20u + hull * 4u);
    while (node_index >= 0 && guard++ <= clips->count) {
        const uint8_t *node;
        c15_plane_t plane;
        int64_t distance;
        int side;
        if ((uint32_t)node_index >= clips->count) {
            return -2;
        }
        node = clips->data + (uint32_t)node_index * clips->stride;
        if (!c15_map_plane(map, (uint32_t)read_i32(node), &plane)) {
            return -2;
        }
        distance =
            (int64_t)plane.nx * x +
            (int64_t)plane.ny * y +
            (int64_t)plane.nz * z -
            ((int64_t)plane.distance_q4 << 10);
        side = distance < 0 ? 1 : 0;
        node_index = read_i16(node + 4 + side * 2);
    }
    return node_index < 0 ? node_index : -2;
}

static const int16_t k_sin_quarter[65] = {
    0,402,804,1205,1606,2006,2404,2801,3196,3590,3981,4370,4756,
    5139,5520,5897,6270,6639,7005,7366,7723,8076,8423,8765,9102,
    9434,9760,10080,10394,10702,11003,11297,11585,11866,12140,
    12406,12665,12916,13160,13395,13623,13842,14053,14256,14449,
    14635,14811,14978,15137,15286,15426,15557,15679,15791,15893,
    15986,16069,16143,16207,16261,16305,16340,16364,16379,16384
};

int16_t c15_sin_q14(uint8_t angle)
{
    uint8_t quadrant = angle >> 6;
    uint8_t position = angle & 63u;
    if (quadrant == 0u) return k_sin_quarter[position];
    if (quadrant == 1u) return k_sin_quarter[64u - position];
    if (quadrant == 2u) return (int16_t)-k_sin_quarter[position];
    return (int16_t)-k_sin_quarter[64u - position];
}

int16_t c15_cos_q14(uint8_t angle)
{
    return c15_sin_q14((uint8_t)(angle + 64u));
}
