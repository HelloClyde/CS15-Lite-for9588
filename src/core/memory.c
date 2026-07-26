/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/memory.h"

static int is_power_of_two(size_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

void lite_arena_init(lite_arena_t *arena, void *memory, size_t capacity)
{
    if (!arena) {
        return;
    }
    arena->base = (uint8_t *)memory;
    arena->capacity = memory ? capacity : 0u;
    arena->used = 0u;
    arena->peak = 0u;
    arena->allocations = 0u;
    arena->failures = 0u;
}

void *lite_arena_alloc(lite_arena_t *arena, size_t size, size_t alignment)
{
    size_t aligned;
    size_t mask;

    if (!arena || !arena->base || size == 0u ||
        !is_power_of_two(alignment)) {
        if (arena) {
            ++arena->failures;
        }
        return 0;
    }
    mask = alignment - 1u;
    if (arena->used > (size_t)-1 - mask) {
        ++arena->failures;
        return 0;
    }
    aligned = (arena->used + mask) & ~mask;
    if (aligned > arena->capacity || size > arena->capacity - aligned) {
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

void lite_arena_rewind(lite_arena_t *arena, size_t used)
{
    if (arena && used <= arena->used) {
        arena->used = used;
    }
}

size_t lite_arena_available(const lite_arena_t *arena)
{
    if (!arena || arena->used > arena->capacity) {
        return 0u;
    }
    return arena->capacity - arena->used;
}
