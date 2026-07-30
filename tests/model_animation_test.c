/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "model/model.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t vertices[16];
    uint8_t resident[32];
    c15_model_t model;
    c15_model_animation_t animation;
    uint32_t index;

    memset(vertices, 0xcc, sizeof(vertices));
    memset(resident, 0, sizeof(resident));
    memset(&model, 0, sizeof(model));
    memset(&animation, 0, sizeof(animation));
    for (index = 0u; index < 12u; ++index) {
        resident[16u + index] = (uint8_t)(0x20u + index);
    }

    model.vertices = vertices;
    model.vertex_count = 2u;
    model.loaded = 1;
    animation.resident_chunk = resident;
    animation.resident_bytes = sizeof(resident);
    animation.vertex_count = 2u;
    animation.frame_stride = 12u;
    animation.sequence_count = 1u;
    animation.sequences[0].frame_offset = 4u;
    animation.sequences[0].frame_count = 2u;
    animation.sequences[0].frame_ms = 40u;
    animation.loaded = 1;

    assert(c15_model_animation_apply(
        &animation, &model, 0, 0u, 1u, 0, 0u
    ));
    for (index = 0u; index < 6u; ++index) {
        assert(vertices[index] == (uint8_t)(0x20u + index));
        assert(vertices[8u + index] == (uint8_t)(0x26u + index));
    }
    /* Animation positions never replace the model's UV bytes. */
    assert(vertices[6] == 0xccu && vertices[7] == 0xccu);
    assert(vertices[14] == 0xccu && vertices[15] == 0xccu);

    animation.resident_bytes = 27u;
    assert(!c15_model_animation_apply(
        &animation, &model, 0, 0u, 1u, 0, 0u
    ));
    puts("model_animation_test: PASS");
    return 0;
}
