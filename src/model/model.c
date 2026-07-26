/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "model/model.h"

#include "bda_memory.h"

#define MODEL_HEADER_BYTES 64u
#define MODEL_VERTEX_BYTES 8u
#define MODEL_TRIANGLE_BYTES 8u
#define TEX_HEADER_BYTES 24u
#define TEX_PALETTE_BYTES 512u
#define ANIMATION_HEADER_BYTES 32u
#define ANIMATION_SEQUENCE_BYTES 24u
#define ANIMATION_POSITION_BYTES 6u

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

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

int c15_model_load(
    c15_model_t *model,
    const c15_pak_t *pak,
    const char *name,
    lite_arena_t *arena,
    void *scratch,
    uint32_t scratch_size
)
{
    c15_pak_entry_t entry;
    uint8_t *chunk;
    uint32_t vertex_offset;
    uint32_t triangle_offset;
    uint32_t reference_offset;
    uint32_t index;
    if (!model || !pak || !name || !arena || !scratch ||
        scratch_size == 0u) {
        return 0;
    }
    bda_memset(model, 0, sizeof(*model));
    if (!c15_pak_find(pak, name, &entry) ||
        entry.type != C15_FOURCC('M','D','L','0') ||
        entry.packed_size < MODEL_HEADER_BYTES ||
        !c15_pak_validate_entry(pak, &entry, scratch, scratch_size)) {
        return 0;
    }
    chunk = (uint8_t *)lite_arena_alloc(
        arena, entry.packed_size, 16u
    );
    if (!chunk ||
        !c15_pak_read(pak, &entry, 0u, chunk, entry.packed_size) ||
        !bytes_equal(chunk, "C15MDL1\0", 8u) ||
        read_u32(chunk + 8) != 1u ||
        read_u32(chunk + 12) != entry.packed_size) {
        return 0;
    }
    model->chunk = chunk;
    model->vertex_count = read_u32(chunk + 20);
    model->triangle_count = read_u32(chunk + 24);
    model->texture_count = read_u32(chunk + 28);
    vertex_offset = read_u32(chunk + 32);
    triangle_offset = read_u32(chunk + 36);
    reference_offset = read_u32(chunk + 40);
    if (model->vertex_count == 0u ||
        model->triangle_count == 0u ||
        model->texture_count == 0u ||
        model->texture_count > C15_MODEL_MAX_TEXTURES ||
        model->vertex_count >
            entry.packed_size / MODEL_VERTEX_BYTES ||
        model->triangle_count >
            entry.packed_size / MODEL_TRIANGLE_BYTES ||
        vertex_offset > entry.packed_size -
            model->vertex_count * MODEL_VERTEX_BYTES ||
        triangle_offset > entry.packed_size -
            model->triangle_count * MODEL_TRIANGLE_BYTES ||
        reference_offset > entry.packed_size -
            model->texture_count * 4u) {
        return 0;
    }
    model->vertices = chunk + vertex_offset;
    model->triangles = chunk + triangle_offset;
    for (index = 0u; index < model->texture_count; ++index) {
        c15_model_texture_t *texture = &model->textures[index];
        uint32_t identifier = read_u32(chunk + reference_offset + index * 4u);
        c15_pak_entry_t texture_entry;
        uint8_t header[TEX_HEADER_BYTES];
        uint32_t pixel_bytes;
        uint32_t resident;
        uint8_t *storage;
        if (!c15_pak_find_id(pak, identifier, &texture_entry) ||
            texture_entry.type != C15_FOURCC('T','E','X','0') ||
            !c15_pak_read(
                pak, &texture_entry, 0u, header, sizeof(header)) ||
            !bytes_equal(header, "CTX1", 4u)) {
            return 0;
        }
        texture->width = read_u16(header + 4);
        texture->height = read_u16(header + 6);
        texture->flags = read_u16(header + 8);
        pixel_bytes = read_u32(header + 12);
        resident = TEX_PALETTE_BYTES + pixel_bytes;
        if (texture->width == 0u || texture->height == 0u ||
            read_u16(header + 10) != 256u ||
            pixel_bytes !=
                (uint32_t)texture->width * texture->height ||
            texture_entry.packed_size != TEX_HEADER_BYTES + resident) {
            return 0;
        }
        storage = (uint8_t *)lite_arena_alloc(arena, resident, 16u);
        if (!storage ||
            !c15_pak_read(
                pak, &texture_entry, TEX_HEADER_BYTES,
                storage, resident)) {
            return 0;
        }
        texture->palette = (const uint16_t *)storage;
        texture->pixels = storage + TEX_PALETTE_BYTES;
    }
    for (index = 0u; index < model->triangle_count; ++index) {
        const uint8_t *triangle =
            model->triangles + index * MODEL_TRIANGLE_BYTES;
        if (read_u16(triangle) >= model->vertex_count ||
            read_u16(triangle + 2) >= model->vertex_count ||
            read_u16(triangle + 4) >= model->vertex_count ||
            triangle[6] >= model->texture_count) {
            return 0;
        }
    }
    model->loaded = 1;
    return 1;
}

static int animation_action(
    uint32_t tag, const uint32_t *tags, uint32_t count
)
{
    uint32_t index;
    for (index = 0u; index < count; ++index) {
        if (tag == tags[index]) {
            return (int)index;
        }
    }
    return -1;
}

static int model_animation_open_profile(
    c15_model_animation_t *animation,
    const c15_model_t *model,
    const c15_pak_t *pak,
    const char *name,
    void *scratch,
    uint32_t scratch_size,
    const uint32_t *tags,
    uint32_t expected_count
)
{
    c15_pak_entry_t entry;
    uint8_t *header = (uint8_t *)scratch;
    uint32_t sequence_count;
    uint32_t table_bytes;
    uint32_t frame_stride;
    uint32_t index;
    uint32_t seen = 0u;
    if (!animation || !model || !model->loaded || !pak || !name ||
        !scratch || scratch_size < ANIMATION_HEADER_BYTES ||
        !tags || expected_count == 0u ||
        expected_count > C15_MODEL_ANIMATION_MAX_SEQUENCES) {
        return 0;
    }
    bda_memset(animation, 0, sizeof(*animation));
    if (!c15_pak_find(pak, name, &entry) ||
        entry.type != C15_FOURCC('A','N','M','0') ||
        entry.packed_size < ANIMATION_HEADER_BYTES ||
        !c15_pak_validate_entry(pak, &entry, scratch, scratch_size) ||
        !c15_pak_read(
            pak, &entry, 0u, header, ANIMATION_HEADER_BYTES) ||
        !bytes_equal(header, "C15ANM1\0", 8u) ||
        read_u32(header + 8) != 1u ||
        read_u32(header + 12) != entry.packed_size ||
        read_u32(header + 16) != read_u32(model->chunk + 16) ||
        read_u32(header + 20) != model->vertex_count) {
        return 0;
    }
    sequence_count = read_u32(header + 24);
    frame_stride = read_u32(header + 28);
    table_bytes = ANIMATION_HEADER_BYTES +
        sequence_count * ANIMATION_SEQUENCE_BYTES;
    if (sequence_count != expected_count ||
        frame_stride !=
            model->vertex_count * ANIMATION_POSITION_BYTES ||
        table_bytes > scratch_size ||
        table_bytes > entry.packed_size ||
        !c15_pak_read(pak, &entry, 0u, header, table_bytes)) {
        return 0;
    }
    for (index = 0u; index < sequence_count; ++index) {
        const uint8_t *record = header + ANIMATION_HEADER_BYTES +
            index * ANIMATION_SEQUENCE_BYTES;
        int action = animation_action(
            read_u32(record), tags, expected_count
        );
        uint32_t frame_count = read_u16(record + 4);
        uint32_t frame_ms = read_u16(record + 6);
        uint32_t frame_offset = read_u32(record + 20);
        uint32_t frame_bytes;
        if (action < 0 || (seen & (1u << (uint32_t)action)) != 0u ||
            frame_count == 0u || frame_ms == 0u ||
            frame_count > 0xffffffffu / frame_stride) {
            return 0;
        }
        frame_bytes = frame_count * frame_stride;
        if (frame_offset < table_bytes ||
            frame_offset > entry.packed_size - frame_bytes) {
            return 0;
        }
        animation->sequences[action].frame_offset = frame_offset;
        animation->sequences[action].frame_count =
            (uint16_t)frame_count;
        animation->sequences[action].frame_ms = (uint16_t)frame_ms;
        seen |= 1u << (uint32_t)action;
    }
    if (seen != (1u << expected_count) - 1u) {
        return 0;
    }
    animation->entry = entry;
    animation->vertex_count = model->vertex_count;
    animation->frame_stride = frame_stride;
    animation->sequence_count = expected_count;
    animation->loaded = 1;
    return 1;
}

int c15_model_animation_open(
    c15_model_animation_t *animation,
    const c15_model_t *model,
    const c15_pak_t *pak,
    const char *name,
    void *scratch,
    uint32_t scratch_size
)
{
    static const uint32_t tags[C15_VIEW_ANIMATION_COUNT] = {
        C15_FOURCC('I','D','L','E'),
        C15_FOURCC('F','I','R','E'),
        C15_FOURCC('R','L','O','D'),
        C15_FOURCC('D','R','A','W')
    };
    return model_animation_open_profile(
        animation, model, pak, name, scratch, scratch_size,
        tags, C15_VIEW_ANIMATION_COUNT
    );
}

int c15_model_locomotion_open(
    c15_model_animation_t *animation,
    const c15_model_t *model,
    const c15_pak_t *pak,
    const char *name,
    void *scratch,
    uint32_t scratch_size
)
{
    static const uint32_t tags[C15_WORLD_ANIMATION_COUNT] = {
        C15_FOURCC('I','D','L','E'),
        C15_FOURCC('W','A','L','K')
    };
    return model_animation_open_profile(
        animation, model, pak, name, scratch, scratch_size,
        tags, C15_WORLD_ANIMATION_COUNT
    );
}

int c15_model_animation_apply(
    const c15_model_animation_t *animation,
    c15_model_t *model,
    const c15_pak_t *pak,
    uint32_t action,
    uint32_t frame,
    void *scratch,
    uint32_t scratch_size
)
{
    const c15_model_animation_sequence_t *sequence;
    uint8_t *input = (uint8_t *)scratch;
    uint32_t records_per_read;
    uint32_t vertex = 0u;
    uint32_t source_offset;
    if (!animation || !animation->loaded || !model || !model->loaded ||
        !pak || action >= animation->sequence_count ||
        !scratch || scratch_size < ANIMATION_POSITION_BYTES ||
        animation->vertex_count != model->vertex_count) {
        return 0;
    }
    sequence = &animation->sequences[action];
    if (frame >= sequence->frame_count) {
        return 0;
    }
    records_per_read = scratch_size / ANIMATION_POSITION_BYTES;
    source_offset = sequence->frame_offset +
        frame * animation->frame_stride;
    while (vertex < animation->vertex_count) {
        uint32_t count = animation->vertex_count - vertex;
        uint32_t bytes;
        uint32_t index;
        if (count > records_per_read) {
            count = records_per_read;
        }
        bytes = count * ANIMATION_POSITION_BYTES;
        if (!c15_pak_read(
                pak, &animation->entry, source_offset,
                input, bytes)) {
            return 0;
        }
        for (index = 0u; index < count; ++index) {
            uint8_t *target =
                model->vertices + (vertex + index) * MODEL_VERTEX_BYTES;
            const uint8_t *position =
                input + index * ANIMATION_POSITION_BYTES;
            target[0] = position[0];
            target[1] = position[1];
            target[2] = position[2];
            target[3] = position[3];
            target[4] = position[4];
            target[5] = position[5];
        }
        vertex += count;
        source_offset += bytes;
    }
    return 1;
}
