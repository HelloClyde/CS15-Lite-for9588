/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_MOVEMENT_H
#define CS15_LITE_MOVEMENT_H

#include <stdint.h>

#include "world/map.h"

#define C15_MOVE_FORWARD (1u << 0)
#define C15_MOVE_BACK    (1u << 1)
#define C15_MOVE_LEFT    (1u << 2)
#define C15_MOVE_RIGHT   (1u << 3)
#define C15_MOVE_JUMP    (1u << 4)
#define C15_MOVE_CROUCH  (1u << 5)

typedef struct c15_player {
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t z_q8;
    int32_t velocity_z_q8;
    int32_t maximum_z;
    uint32_t blocked_steps;
    uint8_t yaw;
    int8_t pitch;
    uint16_t yaw_q8;
    int16_t pitch_q8;
    uint8_t grounded;
    uint8_t hull;
} c15_player_t;

void c15_player_spawn(
    c15_player_t *player, const c15_camera_t *spawn_camera
);
void c15_player_step(
    c15_player_t *player, const c15_map_t *map, uint32_t controls
);
void c15_player_step_speed(
    c15_player_t *player,
    const c15_map_t *map,
    uint32_t controls,
    uint32_t move_per_tick
);
void c15_player_camera(
    const c15_player_t *player, c15_camera_t *camera
);
void c15_player_look(c15_player_t *player, int dx, int dy);

#endif
