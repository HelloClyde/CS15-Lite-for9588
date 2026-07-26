#include "world/movement.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int c15_map_hull_contents(
    const c15_map_t *map,
    uint32_t hull,
    int32_t x,
    int32_t y,
    int32_t z
)
{
    (void)map;
    (void)hull;
    (void)x;
    (void)y;
    (void)z;
    return -1;
}

int16_t c15_sin_q14(uint8_t angle)
{
    (void)angle;
    return 0;
}

int16_t c15_cos_q14(uint8_t angle)
{
    (void)angle;
    return 16384;
}

int main(void)
{
    c15_camera_t spawn = {0};
    c15_camera_t camera = {0};
    c15_player_t one_step;
    c15_player_t many_steps;
    int index;

    spawn.yaw = 10u;
    c15_player_spawn(&one_step, &spawn);
    assert(one_step.yaw_q8 == (10u << 8));
    assert(one_step.pitch_q8 == 0);

    c15_player_look(&one_step, 1, 1);
    assert(one_step.yaw_q8 == (10u << 8) - 64u);
    assert(one_step.pitch_q8 == -64);
    c15_player_camera(&one_step, &camera);
    assert(camera.yaw_q8 == one_step.yaw_q8);
    assert(camera.pitch_q8 == one_step.pitch_q8);

    c15_player_spawn(&many_steps, &spawn);
    for (index = 0; index < 4; ++index) {
        c15_player_look(&many_steps, 1, 1);
    }
    c15_player_spawn(&one_step, &spawn);
    c15_player_look(&one_step, 4, 4);
    assert(many_steps.yaw_q8 == one_step.yaw_q8);
    assert(many_steps.pitch_q8 == one_step.pitch_q8);

    c15_player_look(&one_step, 0, 1000);
    assert(one_step.pitch_q8 == -32 * 256);
    c15_player_look(&one_step, 0, -2000);
    assert(one_step.pitch_q8 == 32 * 256);

    spawn.yaw = 0u;
    c15_player_spawn(&one_step, &spawn);
    c15_player_look(&one_step, -1, 0);
    assert(one_step.yaw_q8 == 64u);

    c15_player_spawn(&one_step, &spawn);
    c15_player_step(&one_step, 0, C15_MOVE_RIGHT);
    assert(one_step.y == -8);
    c15_player_spawn(&one_step, &spawn);
    c15_player_step(&one_step, 0, C15_MOVE_LEFT);
    assert(one_step.y == 8);
    c15_player_spawn(&one_step, &spawn);
    c15_player_step_speed(&one_step, 0, C15_MOVE_RIGHT, 4u);
    assert(one_step.y == -4);

    puts("movement_test: PASS");
    return 0;
}
