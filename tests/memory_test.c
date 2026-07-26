#include "core/memory.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint8_t memory[128];
    lite_arena_t arena;
    void *first;
    void *second;

    lite_arena_init(&arena, memory, sizeof(memory));
    assert(arena.capacity == sizeof(memory));
    assert(lite_arena_available(&arena) == sizeof(memory));
    first = lite_arena_alloc(&arena, 3u, 1u);
    second = lite_arena_alloc(&arena, 8u, 8u);
    assert(first == memory);
    assert(second == memory + 8u);
    assert(arena.used == 16u);
    assert(arena.peak == 16u);
    assert(arena.allocations == 2u);
    assert(lite_arena_alloc(&arena, 200u, 4u) == 0);
    assert(arena.failures == 1u);
    assert(lite_arena_alloc(&arena, 1u, 3u) == 0);
    assert(arena.failures == 2u);
    lite_arena_reset(&arena);
    assert(arena.used == 0u);
    assert(arena.peak == 16u);
    puts("memory_test: PASS");
    return 0;
}
