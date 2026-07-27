/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_PAK_H
#define CS15_LITE_PAK_H

#include <stddef.h>
#include <stdint.h>

#define C15_PAK_MAX_ENTRIES 4096u
#define C15_FOURCC(a,b,c,d) \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

typedef struct c15_pak_entry {
    uint32_t type;
    uint32_t flags;
    uint32_t asset_id;
    uint32_t offset;
    uint32_t packed_size;
    uint32_t unpacked_size;
    uint32_t crc32;
    char name[33];
} c15_pak_entry_t;

typedef struct c15_pak {
    int file;
    uint32_t file_size;
    uint32_t entry_count;
    uint32_t directory_offset;
    uint32_t data_offset;
    uint32_t last_seek_expected;
    int32_t last_seek_result;
    uint32_t last_read_expected;
    int32_t last_read_result;
} c15_pak_t;

int c15_pak_open(c15_pak_t *pak, const char *path);
void c15_pak_close(c15_pak_t *pak);
int c15_pak_find(
    const c15_pak_t *pak,
    const char *name,
    c15_pak_entry_t *entry
);
int c15_pak_find_id(
    const c15_pak_t *pak,
    uint32_t asset_id,
    c15_pak_entry_t *entry
);
int c15_pak_read(
    const c15_pak_t *pak,
    const c15_pak_entry_t *entry,
    uint32_t relative_offset,
    void *destination,
    uint32_t size
);
int c15_pak_validate_entry(
    const c15_pak_t *pak,
    const c15_pak_entry_t *entry,
    void *scratch,
    uint32_t scratch_size
);
uint32_t c15_crc32(const void *data, uint32_t size);
uint32_t c15_crc32_update(uint32_t state, const void *data, uint32_t size);
uint32_t c15_asset_id(const char *name);

#endif
