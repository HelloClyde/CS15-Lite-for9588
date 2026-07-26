/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_MEMORY_H
#define CS15_LITE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

typedef struct lite_arena {
    uint8_t *base;
    size_t capacity;
    size_t used;
    size_t peak;
    uint32_t allocations;
    uint32_t failures;
} lite_arena_t;

void lite_arena_init(lite_arena_t *arena, void *memory, size_t capacity);
void *lite_arena_alloc(lite_arena_t *arena, size_t size, size_t alignment);
void lite_arena_reset(lite_arena_t *arena);
void lite_arena_rewind(lite_arena_t *arena, size_t used);
size_t lite_arena_available(const lite_arena_t *arena);

#endif
