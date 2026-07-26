/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_MODEL_H
#define CS15_LITE_MODEL_H

#include <stdint.h>

#include "assets/pak.h"
#include "core/memory.h"

#define C15_MODEL_MAX_TEXTURES 16u
#define C15_VIEW_ANIMATION_COUNT 4u
#define C15_WORLD_ANIMATION_COUNT 2u
#define C15_MODEL_ANIMATION_MAX_SEQUENCES C15_VIEW_ANIMATION_COUNT

enum c15_view_animation_action {
    C15_VIEW_ANIMATION_IDLE,
    C15_VIEW_ANIMATION_FIRE,
    C15_VIEW_ANIMATION_RELOAD,
    C15_VIEW_ANIMATION_DRAW
};

typedef struct c15_model_texture {
    uint16_t width;
    uint16_t height;
    uint16_t flags;
    const uint16_t *palette;
    const uint8_t *pixels;
} c15_model_texture_t;

typedef struct c15_model {
    const uint8_t *chunk;
    uint8_t *vertices;
    const uint8_t *triangles;
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t texture_count;
    c15_model_texture_t textures[C15_MODEL_MAX_TEXTURES];
    int loaded;
} c15_model_t;

typedef struct c15_model_animation_sequence {
    uint32_t frame_offset;
    uint16_t frame_count;
    uint16_t frame_ms;
} c15_model_animation_sequence_t;

typedef struct c15_model_animation {
    const c15_pak_entry_t *entry;
    uint32_t vertex_count;
    uint32_t frame_stride;
    uint32_t sequence_count;
    c15_model_animation_sequence_t
        sequences[C15_MODEL_ANIMATION_MAX_SEQUENCES];
    int loaded;
} c15_model_animation_t;

int c15_model_load(
    c15_model_t *model,
    const c15_pak_t *pak,
    const char *name,
    lite_arena_t *arena,
    void *scratch,
    uint32_t scratch_size
);
int c15_model_animation_open(
    c15_model_animation_t *animation,
    const c15_model_t *model,
    const c15_pak_t *pak,
    const char *name,
    void *scratch,
    uint32_t scratch_size
);
int c15_model_locomotion_open(
    c15_model_animation_t *animation,
    const c15_model_t *model,
    const c15_pak_t *pak,
    const char *name,
    void *scratch,
    uint32_t scratch_size
);
int c15_model_animation_apply(
    const c15_model_animation_t *animation,
    c15_model_t *model,
    const c15_pak_t *pak,
    uint32_t action,
    uint32_t frame,
    void *scratch,
    uint32_t scratch_size
);

#endif
