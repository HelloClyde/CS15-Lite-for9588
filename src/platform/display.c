/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/display.h"

#define SOURCE_WIDTH 320u
#define SOURCE_HEIGHT 240u
#define DESTINATION_WIDTH SOURCE_HEIGHT
#define DESTINATION_HEIGHT SOURCE_WIDTH
#define TILE_SIZE 8u
#define FRAME_PIXELS (DESTINATION_WIDTH * DESTINATION_HEIGHT)
#define FRAME_WORDS (FRAME_PIXELS / 2u)

typedef uint32_t lite_alias_u32 __attribute__((__may_alias__));

static void copy_ccw(
    const uint16_t *restrict source,
    volatile uint16_t *restrict destination
)
{
    uint32_t block_y;

    for (block_y = 0u; block_y < DESTINATION_HEIGHT;
         block_y += TILE_SIZE) {
        uint32_t block_x;
        volatile uint16_t *destination_block =
            destination + block_y * DESTINATION_WIDTH;

        for (block_x = 0u; block_x < DESTINATION_WIDTH;
             block_x += TILE_SIZE) {
            uint32_t column;
            for (column = 0u; column < TILE_SIZE; ++column) {
                const uint16_t *source_row =
                    source + (block_x + column) * SOURCE_WIDTH;
                volatile uint16_t *destination_column =
                    destination_block + block_x + column;
                uint32_t source_x =
                    SOURCE_WIDTH - 1u - block_y;

                destination_column[0u * DESTINATION_WIDTH] =
                    source_row[source_x - 0u];
                destination_column[1u * DESTINATION_WIDTH] =
                    source_row[source_x - 1u];
                destination_column[2u * DESTINATION_WIDTH] =
                    source_row[source_x - 2u];
                destination_column[3u * DESTINATION_WIDTH] =
                    source_row[source_x - 3u];
                destination_column[4u * DESTINATION_WIDTH] =
                    source_row[source_x - 4u];
                destination_column[5u * DESTINATION_WIDTH] =
                    source_row[source_x - 5u];
                destination_column[6u * DESTINATION_WIDTH] =
                    source_row[source_x - 6u];
                destination_column[7u * DESTINATION_WIDTH] =
                    source_row[source_x - 7u];
            }
        }
    }
}

static void copy_ccw_rotated_180(
    const uint16_t *restrict source,
    volatile uint16_t *restrict destination
)
{
    uint32_t block_y;

    for (block_y = 0u; block_y < DESTINATION_HEIGHT;
         block_y += TILE_SIZE) {
        uint32_t block_x;
        volatile uint16_t *destination_block =
            destination + block_y * DESTINATION_WIDTH;

        for (block_x = 0u; block_x < DESTINATION_WIDTH;
             block_x += TILE_SIZE) {
            uint32_t column;
            for (column = 0u; column < TILE_SIZE; ++column) {
                const uint16_t *source_row =
                    source +
                    (SOURCE_HEIGHT - 1u - block_x - column) *
                    SOURCE_WIDTH;
                volatile uint16_t *destination_column =
                    destination_block + block_x + column;

                destination_column[0u * DESTINATION_WIDTH] =
                    source_row[block_y + 0u];
                destination_column[1u * DESTINATION_WIDTH] =
                    source_row[block_y + 1u];
                destination_column[2u * DESTINATION_WIDTH] =
                    source_row[block_y + 2u];
                destination_column[3u * DESTINATION_WIDTH] =
                    source_row[block_y + 3u];
                destination_column[4u * DESTINATION_WIDTH] =
                    source_row[block_y + 4u];
                destination_column[5u * DESTINATION_WIDTH] =
                    source_row[block_y + 5u];
                destination_column[6u * DESTINATION_WIDTH] =
                    source_row[block_y + 6u];
                destination_column[7u * DESTINATION_WIDTH] =
                    source_row[block_y + 7u];
            }
        }
    }
}

void lite_display_copy_landscape_rgb565(
    const uint16_t *source,
    volatile uint16_t *destination,
    int rotate_180
)
{
    if (!source || !destination) {
        return;
    }
    if (rotate_180) {
        copy_ccw_rotated_180(source, destination);
    } else {
        copy_ccw(source, destination);
    }
#if defined(__mips__)
    __asm__ volatile("sync" ::: "memory");
#endif
}

void lite_display_copy_portrait_rgb565(
    const uint16_t *source,
    volatile uint16_t *destination,
    int rotate_180
)
{
    const lite_alias_u32 *source_words =
        (const lite_alias_u32 *)source;
    volatile lite_alias_u32 *destination_words =
        (volatile lite_alias_u32 *)destination;
    uint32_t remaining = FRAME_WORDS;

    if (!source || !destination) {
        return;
    }
    if (!rotate_180) {
        while (remaining != 0u) {
            *destination_words++ = *source_words++;
            --remaining;
        }
    } else {
        source_words += FRAME_WORDS;
        while (remaining != 0u) {
            uint32_t pair = *--source_words;
            *destination_words++ = (pair << 16) | (pair >> 16);
            --remaining;
        }
    }
#if defined(__mips__)
    __asm__ volatile("sync" ::: "memory");
#endif
}
