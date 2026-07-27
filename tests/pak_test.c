#include "assets/pak.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PAK_BYTES 12289u

typedef struct test_asset {
    const char *name;
    uint32_t id;
    uint32_t offset;
    uint8_t value;
} test_asset_t;

static void put_u32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)(value >> 8);
    target[2] = (uint8_t)(value >> 16);
    target[3] = (uint8_t)(value >> 24);
}

static int compare_asset(const void *left, const void *right)
{
    const test_asset_t *a = (const test_asset_t *)left;
    const test_asset_t *b = (const test_asset_t *)right;
    return a->id < b->id ? -1 : (a->id != b->id);
}

int main(void)
{
    static const char path[] = "build/host-tests/pak_test.c15pak";
    test_asset_t assets[] = {
        {"test/alpha", 0u, 0u, 0x11u},
        {"test/bravo", 0u, 0u, 0x22u},
        {"test/charlie", 0u, 0u, 0x33u}
    };
    uint8_t *file_data = (uint8_t *)calloc(TEST_PAK_BYTES, 1u);
    c15_pak_t pak;
    c15_pak_entry_t entry;
    uint8_t scratch[16];
    uint8_t value;
    FILE *output;
    uint32_t index;

    assert(file_data);
    for (index = 0u; index < 3u; ++index) {
        assets[index].id = c15_asset_id(assets[index].name);
    }
    qsort(assets, 3u, sizeof(assets[0]), compare_asset);
    memcpy(file_data, "C15PAK1\0", 8u);
    put_u32(file_data + 8u, 2u);
    put_u32(file_data + 12u, 0x12345678u);
    put_u32(file_data + 16u, TEST_PAK_BYTES);
    put_u32(file_data + 20u, 3u);
    put_u32(file_data + 24u, 64u);
    put_u32(file_data + 28u, 4096u);
    for (index = 0u; index < 3u; ++index) {
        uint8_t *record = file_data + 64u + index * 64u;
        uint32_t offset = 4096u + index * 4096u;
        assets[index].offset = offset;
        put_u32(record, C15_FOURCC('T','E','S','T'));
        put_u32(record + 8u, assets[index].id);
        put_u32(record + 12u, offset);
        put_u32(record + 16u, 1u);
        put_u32(record + 20u, 1u);
        file_data[offset] = assets[index].value;
        put_u32(record + 24u, c15_crc32(file_data + offset, 1u));
        memcpy(record + 28u, assets[index].name, strlen(assets[index].name));
    }
    output = fopen(path, "wb");
    assert(output);
    assert(fwrite(file_data, 1u, TEST_PAK_BYTES, output) == TEST_PAK_BYTES);
    assert(fclose(output) == 0);
    free(file_data);

    assert(sizeof(pak) <= 40u);
    assert(c15_pak_open(&pak, path));
    assert(pak.entry_count == 3u);
    for (index = 0u; index < 3u; ++index) {
        assert(c15_pak_find(&pak, assets[index].name, &entry));
        assert(entry.asset_id == assets[index].id);
        assert(c15_pak_find_id(&pak, assets[index].id, &entry));
        assert(c15_pak_read(&pak, &entry, 0u, &value, 1u));
        assert(value == assets[index].value);
        assert(c15_pak_validate_entry(
            &pak, &entry, scratch, sizeof(scratch)
        ));
    }
    assert(!c15_pak_find(&pak, "test/missing", &entry));
    c15_pak_close(&pak);
    assert(remove(path) == 0);
    puts("pak_test: PASS");
    return 0;
}
