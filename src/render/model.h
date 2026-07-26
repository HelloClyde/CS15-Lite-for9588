/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_MODEL_RENDERER_H
#define CS15_LITE_MODEL_RENDERER_H

#include <stdint.h>

#include "model/model.h"
#include "render/framebuffer.h"
#include "world/map.h"

typedef struct c15_model_render_stats {
    uint32_t triangles;
    uint32_t pixels;
} c15_model_render_stats_t;

typedef struct c15_model_view {
    int16_t center_x;
    int16_t origin_y;
    int16_t focal_length;
    int16_t depth_bias_q4;
    int16_t near_depth_q4;
} c15_model_view_t;

void c15_render_view_model(
    const c15_model_t *model,
    lite_framebuffer_t *framebuffer,
    uint16_t *depth,
    const c15_model_view_t *view,
    int recoil,
    int bob_q4,
    c15_model_render_stats_t *stats
);
void c15_render_world_model(
    const c15_model_t *model,
    lite_framebuffer_t *framebuffer,
    uint16_t *depth,
    const c15_camera_t *camera,
    int32_t origin_x,
    int32_t origin_y,
    int32_t origin_z,
    uint8_t yaw,
    c15_model_render_stats_t *stats
);

#endif
