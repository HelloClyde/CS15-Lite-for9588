/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "assets/pak.h"

#include "bda_filesystem.h"
#include "bda_memory.h"

#define PAK_HEADER_BYTES 64u
#define PAK_ENTRY_BYTES 64u
#define PAK_ALIGNMENT 4096u
#define PAK_VERSION 1u
#define PAK_ENDIAN 0x12345678u

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
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

static int names_equal(const char *left, const char *right)
{
    while (*left && *right && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

uint32_t c15_crc32_update(uint32_t state, const void *data, uint32_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t index;
    uint32_t crc = state;
    for (index = 0u; index < size; ++index) {
        uint32_t bit;
        crc ^= bytes[index];
        for (bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return crc;
}

uint32_t c15_crc32(const void *data, uint32_t size)
{
    return c15_crc32_update(0xffffffffu, data, size) ^ 0xffffffffu;
}

uint32_t c15_asset_id(const char *name)
{
    uint32_t value = 2166136261u;
    while (name && *name) {
        value ^= (uint8_t)*name++;
        value *= 16777619u;
    }
    return value;
}

int c15_pak_open(c15_pak_t *pak, const char *path)
{
    uint8_t header[PAK_HEADER_BYTES];
    uint8_t record[PAK_ENTRY_BYTES];
    uint32_t directory_offset;
    uint32_t data_offset;
    uint32_t index;
    int end;

    if (!pak || !path) {
        return 0;
    }
    bda_memset(pak, 0, sizeof(*pak));
    pak->file = bda_fs_fopen_raw(path, "rb");
    if (!bda_fs_file_is_valid(pak->file)) {
        pak->file = 0;
        return 0;
    }
    end = bda_fs_seek_raw(pak->file, 0, BDA_SEEK_END);
    if (end < (int)PAK_HEADER_BYTES ||
        bda_fs_seek_raw(pak->file, 0, BDA_SEEK_SET) != 0 ||
        bda_fs_read_raw(pak->file, header, sizeof(header)) !=
            (int)sizeof(header)) {
        c15_pak_close(pak);
        return 0;
    }
    if (!bytes_equal(header, "C15PAK1\0", 8u) ||
        read_u32(header + 8) != PAK_VERSION ||
        read_u32(header + 12) != PAK_ENDIAN ||
        read_u32(header + 16) != (uint32_t)end) {
        c15_pak_close(pak);
        return 0;
    }
    pak->file_size = (uint32_t)end;
    pak->entry_count = read_u32(header + 20);
    directory_offset = read_u32(header + 24);
    data_offset = read_u32(header + 28);
    if (pak->entry_count == 0u ||
        pak->entry_count > C15_PAK_MAX_ENTRIES ||
        directory_offset > pak->file_size -
            pak->entry_count * PAK_ENTRY_BYTES ||
        data_offset < directory_offset +
            pak->entry_count * PAK_ENTRY_BYTES) {
        c15_pak_close(pak);
        return 0;
    }
    for (index = 0u; index < pak->entry_count; ++index) {
        c15_pak_entry_t *entry = &pak->entries[index];
        uint32_t seek = directory_offset + index * PAK_ENTRY_BYTES;
        uint32_t name_index;
        if (bda_fs_seek_raw(pak->file, (s32)seek, BDA_SEEK_SET) !=
                (int)seek ||
            bda_fs_read_raw(pak->file, record, sizeof(record)) !=
                (int)sizeof(record)) {
            c15_pak_close(pak);
            return 0;
        }
        entry->type = read_u32(record);
        entry->flags = read_u32(record + 4);
        entry->asset_id = read_u32(record + 8);
        entry->offset = read_u32(record + 12);
        entry->packed_size = read_u32(record + 16);
        entry->unpacked_size = read_u32(record + 20);
        entry->crc32 = read_u32(record + 24);
        for (name_index = 0u; name_index < 32u; ++name_index) {
            entry->name[name_index] = (char)record[28u + name_index];
        }
        entry->name[32] = 0;
        if (entry->asset_id != c15_asset_id(entry->name) ||
            entry->offset < data_offset ||
            (entry->offset & (PAK_ALIGNMENT - 1u)) != 0u ||
            entry->packed_size != entry->unpacked_size ||
            entry->offset > pak->file_size - entry->packed_size) {
            c15_pak_close(pak);
            return 0;
        }
    }
    return 1;
}

void c15_pak_close(c15_pak_t *pak)
{
    if (!pak) {
        return;
    }
    if (bda_fs_file_is_valid(pak->file)) {
        (void)bda_fs_close_raw(pak->file);
    }
    pak->file = 0;
    pak->file_size = 0u;
    pak->entry_count = 0u;
}

const c15_pak_entry_t *c15_pak_find(
    const c15_pak_t *pak, const char *name
)
{
    uint32_t identifier;
    uint32_t index;
    if (!pak || !name) {
        return 0;
    }
    identifier = c15_asset_id(name);
    for (index = 0u; index < pak->entry_count; ++index) {
        if (pak->entries[index].asset_id == identifier &&
            names_equal(pak->entries[index].name, name)) {
            return &pak->entries[index];
        }
    }
    return 0;
}

const c15_pak_entry_t *c15_pak_find_id(
    const c15_pak_t *pak, uint32_t asset_id
)
{
    uint32_t index;
    if (!pak) {
        return 0;
    }
    for (index = 0u; index < pak->entry_count; ++index) {
        if (pak->entries[index].asset_id == asset_id) {
            return &pak->entries[index];
        }
    }
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
    uint32_t absolute;
    if (!pak || !entry || !destination ||
        !bda_fs_file_is_valid(pak->file) ||
        relative_offset > entry->packed_size ||
        size > entry->packed_size - relative_offset) {
        return 0;
    }
    absolute = entry->offset + relative_offset;
    if (bda_fs_seek_raw(pak->file, (s32)absolute, BDA_SEEK_SET) !=
        (int)absolute) {
        return 0;
    }
    return bda_fs_read_raw(pak->file, destination, size) == (int)size;
}

int c15_pak_validate_entry(
    const c15_pak_t *pak,
    const c15_pak_entry_t *entry,
    void *scratch,
    uint32_t scratch_size
)
{
    uint32_t offset = 0u;
    uint32_t crc = 0xffffffffu;
    if (!scratch || scratch_size == 0u) {
        return 0;
    }
    while (offset < entry->packed_size) {
        uint32_t amount = entry->packed_size - offset;
        if (amount > scratch_size) {
            amount = scratch_size;
        }
        if (!c15_pak_read(pak, entry, offset, scratch, amount)) {
            return 0;
        }
        crc = c15_crc32_update(crc, scratch, amount);
        offset += amount;
    }
    return (crc ^ 0xffffffffu) == entry->crc32;
}
