/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_BBK9588_H
#define CS15_LITE_BBK9588_H

#include <stdint.h>

#define LITE_SCREEN_WIDTH 320u
#define LITE_SCREEN_HEIGHT 240u
#define LITE_VIEW_WIDTH 320u
#define LITE_VIEW_HEIGHT 240u
#define LITE_DISPLAY_VIEW_WIDTH 320u
#define LITE_DISPLAY_VIEW_HEIGHT 240u

#define LITE_BUTTON_X 266
#define LITE_BUTTON_WIDTH 50
#define LITE_BUTTON_HEIGHT 28
#define LITE_BUTTON_USE_Y 6
#define LITE_BUTTON_RELOAD_Y 38
#define LITE_BUTTON_WEAPON_Y 70
#define LITE_BUTTON_JUMP_Y 102
#define LITE_BUTTON_CROUCH_Y 134
#define LITE_BUTTON_SCORE_Y 166
#define LITE_BUTTON_ALT_Y 198

#define LITE_INPUT_UP        (1u << 0)
#define LITE_INPUT_DOWN      (1u << 1)
#define LITE_INPUT_LEFT      (1u << 2)
#define LITE_INPUT_RIGHT     (1u << 3)
#define LITE_INPUT_FIRE      (1u << 4)
#define LITE_INPUT_USE       (1u << 5)
#define LITE_INPUT_JUMP      (1u << 6)
#define LITE_INPUT_MENU      (1u << 7)
#define LITE_INPUT_LOOK_ZONE (1u << 8)
#define LITE_INPUT_RELOAD    (1u << 9)
#define LITE_INPUT_WEAPON    (1u << 10)
#define LITE_INPUT_SCORE     (1u << 11)
#define LITE_INPUT_ALT       (1u << 12)

typedef struct lite_input {
    uint32_t down;
    uint32_t pressed;
    uint32_t released;
    int touch_x;
    int touch_y;
    int touch_dx;
    int touch_dy;
    int touch_down;
} lite_input_t;

typedef struct lite_touch_debug {
    uint32_t gesture_id;
    uint32_t duration_ms;
    uint32_t coordinate_events;
    uint32_t move_events;
    uint32_t nonzero_moves;
    uint32_t invalid_events;
    int32_t start_x;
    int32_t start_y;
    int32_t end_x;
    int32_t end_y;
    int32_t net_dx;
    int32_t net_dy;
    uint32_t absolute_dx;
    uint32_t absolute_dy;
    uint32_t maximum_step_dx;
    uint32_t maximum_step_dy;
    int32_t release_dx;
    int32_t release_dy;
    uint32_t gap_count;
    uint32_t gap_sum_ms;
    uint32_t gap_min_ms;
    uint32_t gap_max_ms;
    uint32_t gap_le_10_ms;
    uint32_t gap_le_20_ms;
    uint32_t gap_le_40_ms;
    uint32_t gap_le_80_ms;
    uint32_t gap_over_80_ms;
    uint32_t delta_zero;
    uint32_t delta_one;
    uint32_t delta_two_three;
    uint32_t delta_four_seven;
    uint32_t delta_eight_plus;
    uint32_t application_pumps;
    uint32_t maximum_application_dx;
    uint32_t maximum_application_dy;
    uint32_t pump_count;
    uint32_t maximum_pump_gap_ms;
    uint32_t maximum_events_per_pump;
    uint32_t raw_event_cap_hits;
    uint32_t raw_ignored_events;
} lite_touch_debug_t;

int lite_platform_open(void);
int lite_platform_pump(lite_input_t *input);
int lite_platform_touch_debug_take(lite_touch_debug_t *debug);
int lite_platform_present(const uint16_t *rgb565);
int lite_platform_present_top(const uint16_t *rgb565);
uint32_t lite_platform_milliseconds(void);
void lite_platform_delay(uint32_t units);
void lite_platform_close(void);
uint16_t lite_rgb565(uint32_t red, uint32_t green, uint32_t blue);

#endif
