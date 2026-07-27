/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "world/movement.h"

#define PLAYER_STAND_HULL 1u
#define PLAYER_CROUCH_HULL 3u
#define PLAYER_STAND_EYE_HEIGHT 28
#define PLAYER_CROUCH_EYE_HEIGHT 12
#define PLAYER_CROUCH_CENTER_DROP 18
#define PLAYER_STEP_HEIGHT 18
#define PLAYER_MOVE_PER_TICK 8
#define PLAYER_JUMP_Q8 2765
#define PLAYER_GRAVITY_Q8 328
#define CONTENTS_EMPTY (-1)
#define CONTENTS_SOLID (-2)
#define CONTENTS_CLIP (-8)
/*
 * At the displayed focal length, 64 Q8 angle units move the scene by about
 * one screen pixel near the crosshair for one touch pixel. This gives direct
 * 1:1 drag tracking without an acceleration curve.
 */
#define LOOK_YAW_Q8_PER_PIXEL 64
#define LOOK_PITCH_Q8_PER_PIXEL 64
#define LOOK_PITCH_LIMIT_Q8 (32 * 256)

static int blocking_contents(int contents)
{
    return contents == CONTENTS_SOLID || contents == CONTENTS_CLIP;
}

static int position_blocked(
    const c15_map_t *map,
    uint32_t hull,
    int32_t x,
    int32_t y,
    int32_t z
)
{
    return blocking_contents(
        c15_map_hull_contents(map, hull, x, y, z)
    );
}

static int player_grounded(
    const c15_player_t *player, const c15_map_t *map
)
{
    return position_blocked(
        map, player->hull, player->x, player->y, player->z - 2
    );
}

static void set_player_z(c15_player_t *player, int32_t z)
{
    player->z = z;
    player->z_q8 = z << 8;
}

static int try_horizontal(
    c15_player_t *player,
    const c15_map_t *map,
    int32_t dx,
    int32_t dy
)
{
    int32_t target_x = player->x + dx;
    int32_t target_y = player->y + dy;
    int32_t step_z;
    if (!position_blocked(
            map, player->hull, target_x, target_y, player->z)) {
        player->x = target_x;
        player->y = target_y;
        return 1;
    }
    if (player->grounded) {
        step_z = player->z + PLAYER_STEP_HEIGHT;
        if (!position_blocked(
                map, player->hull, player->x, player->y, step_z) &&
            !position_blocked(
                map, player->hull, target_x, target_y, step_z)) {
            int32_t settle = step_z;
            while (settle > player->z &&
                   !position_blocked(
                       map, player->hull,
                       target_x, target_y, settle - 1)) {
                --settle;
            }
            player->x = target_x;
            player->y = target_y;
            set_player_z(player, settle);
            return 1;
        }
    }
    if (dx != 0 &&
        !position_blocked(
            map, player->hull, target_x, player->y, player->z)) {
        player->x = target_x;
        return 1;
    }
    if (dy != 0 &&
        !position_blocked(
            map, player->hull, player->x, target_y, player->z)) {
        player->y = target_y;
        return 1;
    }
    ++player->blocked_steps;
    return 0;
}

static void step_vertical(c15_player_t *player, const c15_map_t *map)
{
    int32_t target_z;
    int32_t direction;
    int32_t probe;

    if (player->grounded && player->velocity_z_q8 <= 0) {
        player->velocity_z_q8 = 0;
        player->z_q8 = player->z << 8;
        return;
    }
    player->velocity_z_q8 -= PLAYER_GRAVITY_Q8;
    player->z_q8 += player->velocity_z_q8;
    target_z = player->z_q8 >> 8;
    direction = target_z >= player->z ? 1 : -1;
    probe = player->z;
    while (probe != target_z) {
        int32_t next = probe + direction;
        if (position_blocked(
                map, player->hull, player->x, player->y, next)) {
            player->velocity_z_q8 = 0;
            set_player_z(player, probe);
            if (direction < 0) {
                player->grounded = 1u;
            }
            return;
        }
        probe = next;
    }
    player->z = target_z;
    player->grounded = (uint8_t)player_grounded(player, map);
    if (player->grounded && player->velocity_z_q8 < 0) {
        player->velocity_z_q8 = 0;
        player->z_q8 = player->z << 8;
    }
}

void c15_player_spawn(
    c15_player_t *player, const c15_camera_t *spawn_camera
)
{
    player->x = spawn_camera->x;
    player->y = spawn_camera->y;
    player->z = spawn_camera->z - PLAYER_STAND_EYE_HEIGHT;
    player->z_q8 = player->z << 8;
    player->velocity_z_q8 = 0;
    player->maximum_z = player->z;
    player->blocked_steps = 0u;
    player->yaw = spawn_camera->yaw;
    player->pitch = 0;
    player->yaw_q8 = (uint16_t)((uint16_t)spawn_camera->yaw << 8);
    player->pitch_q8 = 0;
    player->grounded = 0u;
    player->hull = PLAYER_STAND_HULL;
}

static void update_crouch(
    c15_player_t *player, const c15_map_t *map, int crouch
)
{
    if (crouch && player->hull == PLAYER_STAND_HULL) {
        int32_t target_z = player->z - PLAYER_CROUCH_CENTER_DROP;
        if (!position_blocked(
                map, PLAYER_CROUCH_HULL,
                player->x, player->y, target_z)) {
            player->hull = PLAYER_CROUCH_HULL;
            set_player_z(player, target_z);
        }
    } else if (!crouch && player->hull == PLAYER_CROUCH_HULL) {
        int32_t target_z = player->z + PLAYER_CROUCH_CENTER_DROP;
        if (!position_blocked(
                map, PLAYER_STAND_HULL,
                player->x, player->y, target_z)) {
            player->hull = PLAYER_STAND_HULL;
            set_player_z(player, target_z);
        }
    }
}

void c15_player_step_speed(
    c15_player_t *player,
    const c15_map_t *map,
    uint32_t controls,
    uint32_t move_per_tick
)
{
    int32_t direction = 0;
    int32_t strafe = 0;
    int on_ladder;
    if ((controls & C15_MOVE_FORWARD) != 0u) {
        direction += 1;
    }
    if ((controls & C15_MOVE_BACK) != 0u) {
        direction -= 1;
    }
    if ((controls & C15_MOVE_LEFT) != 0u) {
        strafe -= 1;
    }
    if ((controls & C15_MOVE_RIGHT) != 0u) {
        strafe += 1;
    }
    update_crouch(
        player, map, (controls & C15_MOVE_CROUCH) != 0u
    );
    on_ladder = c15_map_on_ladder(
        map, player->x, player->y, player->z
    );
    player->grounded = (uint8_t)player_grounded(player, map);
    if ((controls & C15_MOVE_JUMP) != 0u && player->grounded) {
        player->velocity_z_q8 = PLAYER_JUMP_Q8;
        player->grounded = 0u;
    }
    if (direction != 0 || strafe != 0) {
        int32_t forward;
        int32_t side;
        int32_t sine = c15_sin_q14(player->yaw);
        int32_t cosine = c15_cos_q14(player->yaw);
        if (move_per_tick > PLAYER_MOVE_PER_TICK) {
            move_per_tick = PLAYER_MOVE_PER_TICK;
        }
        forward = direction * (int32_t)move_per_tick;
        side = strafe * (int32_t)move_per_tick;
        int32_t dx = (cosine * forward + sine * side) >> 14;
        int32_t dy = (sine * forward - cosine * side) >> 14;
        (void)try_horizontal(player, map, dx, dy);
    }
    if (on_ladder) {
        int32_t climb = direction * 4;
        player->velocity_z_q8 = 0;
        player->grounded = 0u;
        if (climb != 0 &&
            !position_blocked(
                map, player->hull,
                player->x, player->y, player->z + climb)) {
            set_player_z(player, player->z + climb);
        }
    } else {
        step_vertical(player, map);
    }
    if (player->z > player->maximum_z) {
        player->maximum_z = player->z;
    }
}

void c15_player_step(
    c15_player_t *player, const c15_map_t *map, uint32_t controls
)
{
    c15_player_step_speed(
        player, map, controls, PLAYER_MOVE_PER_TICK
    );
}

void c15_player_camera(
    const c15_player_t *player, c15_camera_t *camera
)
{
    camera->x = player->x;
    camera->y = player->y;
    camera->z = player->z + (
        player->hull == PLAYER_CROUCH_HULL ?
            PLAYER_CROUCH_EYE_HEIGHT : PLAYER_STAND_EYE_HEIGHT
    );
    camera->yaw = player->yaw;
    camera->pitch = player->pitch;
    camera->yaw_q8 = player->yaw_q8;
    camera->pitch_q8 = player->pitch_q8;
}

void c15_player_look(c15_player_t *player, int dx, int dy)
{
    int32_t pitch_q8;
    if (!player) {
        return;
    }
    /*
     * Keep the touch-to-angle conversion in Q8. The renderer can therefore
     * use every one-pixel MOVE immediately instead of losing it to dx / 2.
     * uint16_t wrap is the full yaw circle (256 angle units * 256).
     */
    player->yaw_q8 = (uint16_t)(
        (int32_t)player->yaw_q8 - dx * LOOK_YAW_Q8_PER_PIXEL
    );
    pitch_q8 = (int32_t)player->pitch_q8 -
        dy * LOOK_PITCH_Q8_PER_PIXEL;
    if (pitch_q8 < -LOOK_PITCH_LIMIT_Q8) {
        pitch_q8 = -LOOK_PITCH_LIMIT_Q8;
    }
    if (pitch_q8 > LOOK_PITCH_LIMIT_Q8) {
        pitch_q8 = LOOK_PITCH_LIMIT_Q8;
    }
    player->pitch_q8 = (int16_t)pitch_q8;
    player->yaw = (uint8_t)(player->yaw_q8 >> 8);
    player->pitch = (int8_t)(player->pitch_q8 / 256);
}
