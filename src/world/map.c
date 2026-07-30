/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world/map.h"

#include "bda_memory.h"

#define BSP_HEADER_BYTES 64u
#define BSP_SECTION_BYTES 32u
#define BSP_VERSION 3u
#define DEFAULT_FOCAL_LENGTH 160u
#define TEX_HEADER_BYTES 24u
#define BSP_SECTION_STREAMED 1u
#define C15_MAX_ENCODED_VISIBILITY_BYTES \
    (C15_MAP_VISIBILITY_BYTES * 2u)

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

static uint16_t shade_palette_color(uint16_t color, uint32_t level)
{
    if (level == 0u) {
        return (uint16_t)((color & 0xe79cu) >> 2);
    }
    if (level == 1u) {
        return (uint16_t)((color & 0xf7deu) >> 1);
    }
    return (uint16_t)(
        ((color & 0xf7deu) >> 1) +
        ((color & 0xe79cu) >> 2)
    );
}

static uint32_t texture_runtime_bytes(const c15_texture_t *texture)
{
    return texture->runtime_bytes != 0u ?
        texture->runtime_bytes : texture->resident_bytes;
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
            map->pak, &map->entry, 0u, header, sizeof(header))) {
        map->load_error = 2u;
        return 0;
    }
    if (!bytes_equal(header, "C15BSP1\0", 8u) ||
        read_u32(header + 8) != BSP_VERSION ||
        read_u32(header + 16) != map->entry.packed_size) {
        map->load_error = 3u;
        return 0;
    }
    count = read_u32(header + 12);
    if (count == 0u || count > C15_MAP_MAX_SECTIONS ||
        BSP_HEADER_BYTES + count * BSP_SECTION_BYTES >
            map->entry.packed_size) {
        map->load_error = 3u;
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
            map->load_error = 4u;
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
            map->load_error = 5u;
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
            if (!destination) {
                map->load_error = 6u;
                return 0;
            }
            if (!c15_pak_read(
                    map->pak, &map->entry, offset,
                    destination, section->size)) {
                map->load_error = 7u;
                return 0;
            }
            if (c15_crc32(destination, section->size) != checksum) {
                map->load_error = 8u;
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
    map->hostage_section = c15_map_section(
        map, C15_FOURCC('H','S','T','G')
    );
    map->rescue_zone_section = c15_map_section(
        map, C15_FOURCC('R','S','Q','Z')
    );
    map->buy_zone_section = c15_map_section(
        map, C15_FOURCC('B','Y','Z','N')
    );
    map->ladder_section = c15_map_section(
        map, C15_FOURCC('L','A','D','R')
    );
    map->dynamic_section = c15_map_section(
        map, C15_FOURCC('D','E','N','T')
    );
    return map->vertex_section && map->surface_section &&
        map->plane_section && map->node_section && map->leaf_section &&
        map->mark_section && map->visibility_section &&
        map->clip_section && map->model_section &&
        map->bomb_site_section && map->hostage_section &&
        map->rescue_zone_section && map->buy_zone_section &&
        map->ladder_section && map->dynamic_section;
}

static int load_texture_pixels(
    c15_map_t *map, c15_texture_t *texture
)
{
    c15_pak_entry_t entry;
    uint8_t *storage;
    uint32_t palette_bytes;
    uint32_t palette_index;
    uint32_t shade_level;
    if (texture->special) {
        texture->loaded = 1u;
        return 1;
    }
    if (texture->loaded) {
        return 1;
    }
    bda_memset(&entry, 0, sizeof(entry));
    entry.offset = texture->entry_offset;
    entry.packed_size = texture->entry_size;
    storage = (uint8_t *)lite_arena_alloc(
        map->texture_arena, texture_runtime_bytes(texture), 16u
    );
    if (!storage ||
        !c15_pak_read(
            map->pak, &entry, TEX_HEADER_BYTES,
            storage, texture->resident_bytes)) {
        return 0;
    }
    palette_bytes = (uint32_t)texture->palette_count * 2u;
    texture->palette = (const uint16_t *)storage;
    texture->pixels = storage + palette_bytes;
    texture->shade_palettes = 0;
    if (texture_runtime_bytes(texture) >=
        texture->resident_bytes + palette_bytes * 3u) {
        texture->shade_palettes = (const uint16_t *)(
            storage + texture->resident_bytes
        );
        for (shade_level = 0u; shade_level < 3u; ++shade_level) {
            uint16_t *destination = (uint16_t *)(uintptr_t)(
                texture->shade_palettes +
                shade_level * texture->palette_count
            );
            for (palette_index = 0u;
                 palette_index < texture->palette_count;
                 ++palette_index) {
                destination[palette_index] = shade_palette_color(
                    texture->palette[palette_index], shade_level
                );
            }
        }
    }
    texture->loaded = 1u;
    map->texture_cache_bytes =
        (uint32_t)map->texture_arena->used;
    return 1;
}

static void clear_loaded_textures(c15_map_t *map)
{
    uint32_t index;
    lite_arena_reset(map->texture_arena);
    for (index = 0u; index < map->texture_count; ++index) {
        c15_texture_t *texture = &map->textures[index];
        texture->palette = 0;
        texture->shade_palettes = 0;
        texture->pixels = 0;
        texture->loaded = texture->special;
    }
    map->texture_cache_bytes = 0u;
}

int c15_map_ensure_texture(c15_map_t *map, uint32_t texture_index)
{
    c15_texture_t *texture;
    size_t aligned_used;
    if (!map || !map->loaded || !map->texture_arena ||
        texture_index >= map->texture_count) {
        return 0;
    }
    texture = &map->textures[texture_index];
    if (texture->special || texture->loaded) {
        return 1;
    }
    if (texture_runtime_bytes(texture) >
        map->texture_arena->capacity) {
        map->load_error = 12u;
        return 0;
    }
    aligned_used =
        (map->texture_arena->used + 15u) & ~(size_t)15u;
    if (aligned_used > map->texture_arena->capacity ||
        texture_runtime_bytes(texture) >
            map->texture_arena->capacity - aligned_used) {
        clear_loaded_textures(map);
        ++map->texture_cache_reloads;
    }
    if (!load_texture_pixels(map, texture)) {
        map->load_error = 13u;
        return 0;
    }
    return 1;
}

static int load_textures(
    c15_map_t *map, lite_arena_t *arena, int stream_textures
)
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
        uint32_t resident;
        uint32_t palette_bytes;
        uint32_t name_index;
        for (name_index = 0u; name_index < 16u; ++name_index) {
            texture->name[name_index] = (char)source_name[name_index];
        }
        texture->name[16] = 0;
        texture->palette = 0;
        texture->shade_palettes = 0;
        texture->pixels = 0;
        texture->loaded = 0u;
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
        texture->entry_offset = entry.offset;
        texture->entry_size = entry.packed_size;
        texture->resident_bytes = resident;
        texture->runtime_bytes = resident + palette_bytes * 3u;
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
        if (!texture->special) {
            map->texture_resident_bytes += texture->runtime_bytes;
        }
    }
    map->texture_arena = arena;
    map->stream_textures = stream_textures ? 1u : 0u;
    if (!map->stream_textures) {
        for (index = 0u; index < map->texture_count; ++index) {
            if (!load_texture_pixels(map, &map->textures[index])) {
                return 0;
            }
        }
    }
    return 1;
}

int c15_map_prepare_visible_textures(
    c15_map_t *map,
    const uint8_t *surface_bits,
    uint32_t surface_bits_size
)
{
    uint8_t required[(C15_MAP_MAX_TEXTURES + 7u) / 8u];
    const c15_map_section_t *surfaces =
        map ? map->surface_section : 0;
    size_t projected_used;
    uint32_t surface_index;
    uint32_t texture_index;
    int complete = 1;
    if (!map || !map->loaded || !map->texture_arena ||
        !surface_bits || !surfaces ||
        surface_bits_size < (surfaces->count + 7u) / 8u) {
        return 0;
    }
    if (!map->stream_textures) {
        return 1;
    }
    bda_memset(required, 0, sizeof(required));
    for (surface_index = 0u;
         surface_index < surfaces->count; ++surface_index) {
        c15_surface_t surface;
        if ((surface_bits[surface_index >> 3] &
             (uint8_t)(1u << (surface_index & 7u))) == 0u ||
            !c15_map_surface(map, surface_index, &surface) ||
            surface.texture_id >= map->texture_count ||
            map->textures[surface.texture_id].special) {
            continue;
        }
        required[surface.texture_id >> 3] |=
            (uint8_t)(1u << (surface.texture_id & 7u));
    }
    projected_used = map->texture_arena->used;
    for (texture_index = 0u;
         texture_index < map->texture_count; ++texture_index) {
        c15_texture_t *texture = &map->textures[texture_index];
        if ((required[texture_index >> 3] &
             (uint8_t)(1u << (texture_index & 7u))) == 0u ||
            texture->loaded) {
            continue;
        }
        projected_used = (projected_used + 15u) & ~(size_t)15u;
        if (projected_used > map->texture_arena->capacity ||
            texture_runtime_bytes(texture) >
                map->texture_arena->capacity - projected_used) {
            complete = 0;
            break;
        }
        projected_used += texture_runtime_bytes(texture);
    }
    if (!complete) {
        clear_loaded_textures(map);
        ++map->texture_cache_reloads;
    }
    for (texture_index = 0u;
         texture_index < map->texture_count; ++texture_index) {
        c15_texture_t *texture = &map->textures[texture_index];
        size_t aligned_used;
        if ((required[texture_index >> 3] &
             (uint8_t)(1u << (texture_index & 7u))) == 0u ||
            texture->loaded) {
            continue;
        }
        if (texture_runtime_bytes(texture) >
            map->texture_arena->capacity) {
            map->load_error = 12u;
            return 0;
        }
        aligned_used =
            (map->texture_arena->used + 15u) & ~(size_t)15u;
        /*
         * A GoldSrc PVS is deliberately conservative and can reference more
         * original-resolution materials than the bounded cache can hold.
         * Leave the tail deferred; the renderer pages an actually drawn
         * material in with c15_map_ensure_texture().
         */
        if (aligned_used > map->texture_arena->capacity ||
            texture_runtime_bytes(texture) >
                map->texture_arena->capacity - aligned_used) {
            break;
        }
        if (!load_texture_pixels(map, texture)) {
            map->load_error = 13u;
            return 0;
        }
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
    map->spawn.focal_length = DEFAULT_FOCAL_LENGTH;
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
    camera->focal_length = DEFAULT_FOCAL_LENGTH;
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

uint32_t c15_map_hostage_count(const c15_map_t *map)
{
    const c15_map_section_t *hostages =
        map ? map->hostage_section : 0;
    if (!hostages || hostages->stride != 6u) {
        return 0u;
    }
    return hostages->count;
}

int c15_map_hostage(
    const c15_map_t *map,
    uint32_t index,
    int32_t *x,
    int32_t *y,
    int32_t *z
)
{
    const c15_map_section_t *hostages =
        map ? map->hostage_section : 0;
    const uint8_t *record;
    if (!hostages || hostages->stride != 6u ||
        index >= hostages->count || !x || !y || !z) {
        return 0;
    }
    record = hostages->data + index * hostages->stride;
    *x = read_i16(record);
    *y = read_i16(record + 2);
    *z = read_i16(record + 4);
    return 1;
}

static int zone_contains(
    const c15_map_section_t *section,
    uint32_t index,
    int32_t x,
    int32_t y,
    int32_t z,
    uint8_t team
)
{
    const uint8_t *zone;
    uint8_t zone_team;
    if (!section || section->stride != 14u ||
        index >= section->count) {
        return 0;
    }
    zone = section->data + index * section->stride;
    zone_team = zone[12];
    return (zone_team == 0u || team == 0u || zone_team == team) &&
        x >= read_i16(zone) && x <= read_i16(zone + 6) &&
        y >= read_i16(zone + 2) && y <= read_i16(zone + 8) &&
        z >= read_i16(zone + 4) - 48 &&
        z <= read_i16(zone + 10) + 48;
}

static int in_zone_section(
    const c15_map_section_t *section,
    uint8_t team,
    int32_t x,
    int32_t y,
    int32_t z
)
{
    uint32_t index;
    for (index = 0u; section && index < section->count; ++index) {
        if (zone_contains(section, index, x, y, z, team)) {
            return 1;
        }
    }
    return 0;
}

int c15_map_in_rescue_zone(
    const c15_map_t *map, int32_t x, int32_t y, int32_t z
)
{
    const c15_map_section_t *zones =
        map ? map->rescue_zone_section : 0;
    uint32_t index;
    if (zones && zones->count != 0u) {
        return in_zone_section(zones, 0u, x, y, z);
    }
    /*
     * cs_assault from Counter-Strike 1.5 has no explicit rescue brush.
     * GoldSrc still treats the CT start area as the extraction zone.
     */
    for (index = 0u; map && index < c15_map_spawn_count(map); ++index) {
        c15_camera_t spawn;
        uint8_t spawn_team;
        int32_t dx;
        int32_t dy;
        if (!c15_map_spawn(map, index, &spawn, &spawn_team) ||
            spawn_team != 2u) {
            continue;
        }
        dx = x - spawn.x;
        dy = y - spawn.y;
        if (dx >= -256 && dx <= 256 &&
            dy >= -256 && dy <= 256 &&
            z >= spawn.z - 160 && z <= spawn.z + 160) {
            return 1;
        }
    }
    return 0;
}

int c15_map_in_buy_zone(
    const c15_map_t *map,
    uint8_t team,
    int32_t x,
    int32_t y,
    int32_t z
)
{
    const c15_map_section_t *zones =
        map ? map->buy_zone_section : 0;
    uint32_t index;
    if (zones && zones->count != 0u) {
        return in_zone_section(zones, team, x, y, z);
    }
    /*
     * A few historical custom maps omit func_buyzone. Match GoldSrc's
     * practical fallback by accepting a compact area around team spawns.
     */
    for (index = 0u; map && index < c15_map_spawn_count(map); ++index) {
        c15_camera_t spawn;
        uint8_t spawn_team;
        int32_t dx;
        int32_t dy;
        if (!c15_map_spawn(map, index, &spawn, &spawn_team) ||
            spawn_team != team) {
            continue;
        }
        dx = x - spawn.x;
        dy = y - spawn.y;
        if (dx >= -256 && dx <= 256 &&
            dy >= -256 && dy <= 256 &&
            z >= spawn.z - 128 && z <= spawn.z + 128) {
            return 1;
        }
    }
    return 0;
}

int c15_map_on_ladder(
    const c15_map_t *map, int32_t x, int32_t y, int32_t z
)
{
    const c15_map_section_t *ladders =
        map ? map->ladder_section : 0;
    uint32_t index;
    for (index = 0u;
         ladders && ladders->stride == 14u &&
         index < ladders->count; ++index) {
        const uint8_t *zone =
            ladders->data + index * ladders->stride;
        if (x >= read_i16(zone) - 24 &&
            x <= read_i16(zone + 6) + 24 &&
            y >= read_i16(zone + 2) - 24 &&
            y <= read_i16(zone + 8) + 24 &&
            z >= read_i16(zone + 4) - 48 &&
            z <= read_i16(zone + 10) + 48) {
            return 1;
        }
    }
    return 0;
}

uint32_t c15_map_dynamic_count(const c15_map_t *map)
{
    const c15_map_section_t *section =
        map ? map->dynamic_section : 0;
    if (!section || section->stride != 24u) {
        return 0u;
    }
    return section->count < C15_MAP_MAX_DYNAMIC_ENTITIES ?
        section->count : C15_MAP_MAX_DYNAMIC_ENTITIES;
}

int c15_map_dynamic(
    const c15_map_t *map,
    uint32_t index,
    c15_dynamic_entity_t *entity
)
{
    const c15_map_section_t *section =
        map ? map->dynamic_section : 0;
    const uint8_t *record;
    if (!section || section->stride != 24u ||
        index >= c15_map_dynamic_count(map) || !entity) {
        return 0;
    }
    record = section->data + index * section->stride;
    entity->kind = record[0];
    entity->flags = record[1];
    entity->model = read_u16(record + 2);
    entity->minimum_x = read_i16(record + 4);
    entity->minimum_y = read_i16(record + 6);
    entity->minimum_z = read_i16(record + 8);
    entity->maximum_x = read_i16(record + 10);
    entity->maximum_y = read_i16(record + 12);
    entity->maximum_z = read_i16(record + 14);
    entity->target_hash = read_u32(record + 16);
    entity->targetname_hash = read_u32(record + 20);
    return 1;
}

static uint32_t squared_distance_to_dynamic(
    const c15_dynamic_entity_t *entity,
    int32_t x,
    int32_t y,
    int32_t z
)
{
    int32_t center_x =
        ((int32_t)entity->minimum_x + entity->maximum_x) / 2;
    int32_t center_y =
        ((int32_t)entity->minimum_y + entity->maximum_y) / 2;
    int32_t center_z =
        ((int32_t)entity->minimum_z + entity->maximum_z) / 2;
    int32_t dx = x - center_x;
    int32_t dy = y - center_y;
    int32_t dz = z - center_z;
    if (dx < -32767 || dx > 32767 ||
        dy < -32767 || dy > 32767 ||
        dz < -32767 || dz > 32767) {
        return 0xffffffffu;
    }
    return (uint32_t)(
        (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz
    );
}

int c15_map_use_dynamic(
    c15_map_t *map, int32_t x, int32_t y, int32_t z
)
{
    uint32_t count = c15_map_dynamic_count(map);
    uint32_t nearest = 0xffffffffu;
    uint32_t selected = count;
    uint32_t index;
    c15_dynamic_entity_t selected_entity = {0};
    for (index = 0u; index < count; ++index) {
        c15_dynamic_entity_t entity;
        uint32_t distance;
        if (!c15_map_dynamic(map, index, &entity) ||
            entity.kind == C15_DYNAMIC_BREAKABLE) {
            continue;
        }
        distance = squared_distance_to_dynamic(&entity, x, y, z);
        if (distance <= 176u * 176u && distance < nearest) {
            nearest = distance;
            selected = index;
            selected_entity = entity;
        }
    }
    if (selected == count) {
        return 0;
    }
    if (selected_entity.kind == C15_DYNAMIC_BUTTON &&
        selected_entity.target_hash != 0u) {
        for (index = 0u; index < count; ++index) {
            c15_dynamic_entity_t entity;
            if (c15_map_dynamic(map, index, &entity) &&
                entity.targetname_hash == selected_entity.target_hash) {
                map->dynamic_open_bits ^= 1u << index;
            }
        }
    } else {
        map->dynamic_open_bits ^= 1u << selected;
    }
    return 1;
}

int c15_map_damage_breakable(
    c15_map_t *map, int32_t x, int32_t y, int32_t z
)
{
    uint32_t index;
    uint32_t count = c15_map_dynamic_count(map);
    for (index = 0u; index < count; ++index) {
        c15_dynamic_entity_t entity;
        if (!c15_map_dynamic(map, index, &entity) ||
            entity.kind != C15_DYNAMIC_BREAKABLE ||
            (map->dynamic_broken_bits & (1u << index)) != 0u) {
            continue;
        }
        if (x >= entity.minimum_x && x <= entity.maximum_x &&
            y >= entity.minimum_y && y <= entity.maximum_y &&
            z >= entity.minimum_z && z <= entity.maximum_z) {
            if (map->dynamic_damage[index] < 3u) {
                ++map->dynamic_damage[index];
            }
            if (map->dynamic_damage[index] >= 3u) {
                map->dynamic_broken_bits |= 1u << index;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

void c15_map_dynamic_tick(c15_map_t *map)
{
    uint32_t index;
    uint32_t count = c15_map_dynamic_count(map);
    for (index = 0u; index < count; ++index) {
        uint8_t target =
            (map->dynamic_open_bits & (1u << index)) != 0u ?
                8u : 0u;
        if (map->dynamic_position[index] < target) {
            ++map->dynamic_position[index];
        } else if (map->dynamic_position[index] > target) {
            --map->dynamic_position[index];
        }
    }
}

int c15_map_load(
    c15_map_t *map,
    const c15_pak_t *pak,
    const char *map_name,
    lite_arena_t *map_arena,
    lite_arena_t *texture_arena,
    int stream_textures,
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
    /*
     * Do not rescan the complete BSP entry here. Large FAT files on the
     * device are read through a cluster chain and the redundant pass both
     * delays map entry and can fail after otherwise valid long seeks.
     * load_sections validates the BSP header and every resident section CRC.
     * The streamed visibility path remains for old development packs, but
     * release packs make VISI resident to avoid mid-round FAT seeks.
     */
    if (!c15_pak_find(pak, map_name, &map->entry) ||
        map->entry.type != C15_FOURCC('B','S','P','0')) {
        map->load_error = 1u;
        return 0;
    }
    if (!load_sections(map, map_arena, scratch, scratch_size)) {
        return 0;
    }
    if (!cache_sections(map)) {
        map->load_error = 9u;
        return 0;
    }
    if (!load_textures(map, texture_arena, stream_textures)) {
        map->load_error = 10u;
        return 0;
    }
    if (!load_spawn(map)) {
        map->load_error = 11u;
        return 0;
    }
    map->loaded = 1;
    if (map->stream_textures) {
        uint32_t visible_bytes =
            (map->surface_section->count + 7u) / 8u;
        uint32_t visible_leaves;
        if (visible_bytes > scratch_size ||
            !c15_map_build_visible(
                map, &map->spawn, (uint8_t *)scratch,
                visible_bytes, 0, 0u, &visible_leaves) ||
            !c15_map_prepare_visible_textures(
                map, (const uint8_t *)scratch, visible_bytes)) {
            map->loaded = 0;
            if (map->load_error == 0u) {
                map->load_error = 14u;
            }
            return 0;
        }
    }
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
    uint8_t *visible_leaf_bits,
    uint32_t visible_leaf_bits_size,
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
    /*
     * Historical cs_office is the largest supported PVS row. Allow up to
     * 4096 world visibility leaves and the worst-case zero-run encoding.
     */
    uint8_t leaf_bits[C15_MAP_VISIBILITY_BYTES];
    uint8_t encoded_visibility[C15_MAX_ENCODED_VISIBILITY_BYTES];
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
    if (visible_leaf_bits &&
        visible_leaf_bits_size < C15_MAP_VISIBILITY_BYTES) {
        return 0;
    }
    bda_memset(surface_bits, 0, surface_bits_size);
    bda_memset(leaf_bits, 0, sizeof(leaf_bits));
    if (visible_leaf_bits) {
        bda_memset(
            visible_leaf_bits, 0, visible_leaf_bits_size
        );
    }
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
    if (visible_leaf_bits) {
        for (output = 0u; output < row_bytes; ++output) {
            visible_leaf_bits[output] = leaf_bits[output];
        }
    }
    if (visible_leaf_count) {
        *visible_leaf_count = 1u;
    }
    for (leaf = 1u;
         leaf <= visibility_leaves && leaf < leaves->count; ++leaf) {
        if ((leaf_bits[(leaf - 1u) >> 3] &
             (uint8_t)(1u << ((leaf - 1u) & 7u))) != 0u) {
            if (leaf == (uint32_t)leaf_index) {
                continue;
            }
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
    uint32_t dynamic_index;
    for (dynamic_index = 0u;
         map && dynamic_index < c15_map_dynamic_count(map);
         ++dynamic_index) {
        c15_dynamic_entity_t entity;
        uint32_t mask = 1u << dynamic_index;
        int32_t horizontal_margin = hull == 0u ? 0 : 16;
        int32_t vertical_margin = hull == 0u ? 0 : 36;
        int32_t vertical_offset;
        if (!c15_map_dynamic(map, dynamic_index, &entity) ||
            entity.kind == C15_DYNAMIC_BUTTON ||
            map->dynamic_position[dynamic_index] >= 8u ||
            (map->dynamic_broken_bits & mask) != 0u) {
            continue;
        }
        vertical_offset =
            ((int32_t)entity.maximum_z - entity.minimum_z) *
            map->dynamic_position[dynamic_index] / 8;
        if (x >= entity.minimum_x - horizontal_margin &&
            x <= entity.maximum_x + horizontal_margin &&
            y >= entity.minimum_y - horizontal_margin &&
            y <= entity.maximum_y + horizontal_margin &&
            z >= entity.minimum_z + vertical_offset -
                vertical_margin &&
            z <= entity.maximum_z + vertical_offset +
                vertical_margin) {
            return -2;
        }
    }
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
                (int64_t)plane.distance_q4 * 1024;
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
            (int64_t)plane.distance_q4 * 1024;
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
