/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/bbk9588.h"

#include "core/log.h"
#include "platform/display.h"

#include "bda_graphics.h"
#include "bda_input.h"
#include "bda_memory.h"
#include "bda_time.h"
#include "bda_types.h"
#include "bda_window.h"

#define CLOSE_PUMP_LIMIT 128u
#define ESCAPE_HOLD_TICKS 32u
#define RAW_EVENT_MAX_PER_PUMP 8u
#define RAW_SCREEN_WIDTH 240u
#define RAW_SCREEN_HEIGHT 320u
#define C200_GUI_SCREEN_BUFFER 0x6b0u
#define C200_GUI_SCREEN_LAYOUT_HINT 0x738u
#define FRAMEBUFFER_BYTES \
    (RAW_SCREEN_WIDTH * RAW_SCREEN_HEIGHT * (uint32_t)sizeof(uint16_t))
#define MIPS_PHYS_MASK 0x1fffffffu

static bda_handle_t g_frame;
static bda_handle_t g_draw;
static bda_handle_t g_draw_owner;
static volatile uint16_t *g_direct_framebuffer;
static int g_direct_framebuffer_rotate_180;
static uint32_t g_direct_framebuffer_layout_hint;
static int g_direct_framebuffer_rejection_logged;
static int g_direct_framebuffer_refresh_logged;
static uint32_t g_previous_input;
static uint32_t g_touch_control;
static uint32_t g_touch_pressed_pending;
static uint32_t g_escape_start_tick;
static uint32_t g_first_millisecond;
static int g_touch_x;
static int g_touch_y;
static int g_touch_dx_pending;
static int g_touch_dy_pending;
static int g_touch_down;
static int g_suppress_touch_escape;
static int g_escape_down;
static int g_exit_requested;
static int g_detached;
static int g_timer_started;
static lite_touch_debug_t g_touch_debug_current;
static lite_touch_debug_t g_touch_debug_ready;
static uint32_t g_touch_debug_next_id;
static uint32_t g_touch_debug_start_ms;
static uint32_t g_touch_debug_last_event_ms;
static uint32_t g_touch_debug_last_pump_ms;
static int g_touch_debug_active;
static int g_touch_debug_release_pending;
static int g_touch_debug_ready_available;

static uint32_t hit_test(int x, int y);

static int pointer_valid(const void *pointer)
{
    uint32_t address = (uint32_t)(uintptr_t)pointer;
    return pointer != 0 && address != 0xffffffffu;
}

static void *gui_call_pointer0(uint32_t offset)
{
    typedef void *(*function_t)(void);
    void *table = bda_sdk_internal_gui();
    void *function;

    if (!pointer_valid(table)) {
        return 0;
    }
    function = bda_sdk_internal_api(table, offset);
    if (!pointer_valid(function)) {
        return 0;
    }
    return ((function_t)function)();
}

static uint32_t gui_call_u32_0(uint32_t offset, uint32_t fallback)
{
    typedef uint32_t (*function_t)(void);
    void *table = bda_sdk_internal_gui();
    void *function;

    if (!pointer_valid(table)) {
        return fallback;
    }
    function = bda_sdk_internal_api(table, offset);
    if (!pointer_valid(function)) {
        return fallback;
    }
    return ((function_t)function)();
}

static int refresh_direct_framebuffer(int initial)
{
    void *candidate = gui_call_pointer0(C200_GUI_SCREEN_BUFFER);
    uint32_t address = (uint32_t)(uintptr_t)candidate;
    uint32_t uncached_address;
    uint32_t layout_hint = gui_call_u32_0(
        C200_GUI_SCREEN_LAYOUT_HINT, 0xffffffffu
    );
    int rotate_180 = layout_hint != 0x131u;
    int changed;

    if (!lite_display_resolve_mips_framebuffer(
            address, FRAMEBUFFER_BYTES, &uncached_address)) {
        if (!g_direct_framebuffer_rejection_logged) {
            lite_log_line("video_submit=direct_framebuffer_rejected");
            lite_log_hex32("framebuffer_candidate", address);
            lite_log_hex32("framebuffer_layout_hint", layout_hint);
            g_direct_framebuffer_rejection_logged = 1;
        }
        return 0;
    }

    changed =
        (uint32_t)(uintptr_t)g_direct_framebuffer != uncached_address ||
        g_direct_framebuffer_rotate_180 != rotate_180 ||
        g_direct_framebuffer_layout_hint != layout_hint;
    g_direct_framebuffer =
        (volatile uint16_t *)(uintptr_t)uncached_address;
    g_direct_framebuffer_rotate_180 = rotate_180;
    g_direct_framebuffer_layout_hint = layout_hint;

    if (!initial && changed && !g_direct_framebuffer_refresh_logged) {
        lite_log_line("video_submit=direct_framebuffer_refreshed");
        lite_log_hex32("framebuffer_pointer", uncached_address);
        lite_log_hex32("framebuffer_layout_hint", layout_hint);
        g_direct_framebuffer_refresh_logged = 1;
    }
    if (!initial) {
        return 1;
    }

    lite_log_line("video_submit=direct_framebuffer");
    lite_log_line("framebuffer_address_policy=gui_dynamic");
    lite_log_hex32(
        "framebuffer_pointer",
        (uint32_t)(uintptr_t)g_direct_framebuffer
    );
    lite_log_hex32(
        "framebuffer_physical",
        uncached_address & MIPS_PHYS_MASK
    );
    lite_log_hex32("framebuffer_layout_hint", layout_hint);
    lite_log_u32(
        "framebuffer_rotate_180",
        (uint32_t)g_direct_framebuffer_rotate_180
    );
    return 1;
}

static uint32_t absolute_i32(int value)
{
    return value < 0 ? 0u - (uint32_t)value : (uint32_t)value;
}

static uint32_t touch_debug_now(void)
{
    if (!g_timer_started) {
        return 0u;
    }
    return bda_gui_millisecond_count() - g_first_millisecond;
}

static void touch_debug_begin(int x, int y)
{
    uint32_t now = touch_debug_now();
    bda_memset(&g_touch_debug_current, 0, sizeof(g_touch_debug_current));
    ++g_touch_debug_next_id;
    if (g_touch_debug_next_id == 0u) {
        ++g_touch_debug_next_id;
    }
    g_touch_debug_current.gesture_id = g_touch_debug_next_id;
    g_touch_debug_current.coordinate_events = 1u;
    g_touch_debug_current.start_x = x;
    g_touch_debug_current.start_y = y;
    g_touch_debug_current.end_x = x;
    g_touch_debug_current.end_y = y;
    g_touch_debug_start_ms = now;
    g_touch_debug_last_event_ms = now;
    g_touch_debug_last_pump_ms = now;
    g_touch_debug_active = 1;
    g_touch_debug_release_pending = 0;
}

static void touch_debug_gap(uint32_t gap)
{
    lite_touch_debug_t *debug = &g_touch_debug_current;
    ++debug->gap_count;
    debug->gap_sum_ms += gap;
    if (debug->gap_count == 1u || gap < debug->gap_min_ms) {
        debug->gap_min_ms = gap;
    }
    if (gap > debug->gap_max_ms) {
        debug->gap_max_ms = gap;
    }
    if (gap <= 10u) {
        ++debug->gap_le_10_ms;
    } else if (gap <= 20u) {
        ++debug->gap_le_20_ms;
    } else if (gap <= 40u) {
        ++debug->gap_le_40_ms;
    } else if (gap <= 80u) {
        ++debug->gap_le_80_ms;
    } else {
        ++debug->gap_over_80_ms;
    }
}

static void touch_debug_delta(int dx, int dy)
{
    lite_touch_debug_t *debug = &g_touch_debug_current;
    uint32_t absolute_x = absolute_i32(dx);
    uint32_t absolute_y = absolute_i32(dy);
    uint32_t maximum = absolute_x > absolute_y ? absolute_x : absolute_y;

    debug->net_dx += dx;
    debug->net_dy += dy;
    debug->absolute_dx += absolute_x;
    debug->absolute_dy += absolute_y;
    if (absolute_x > debug->maximum_step_dx) {
        debug->maximum_step_dx = absolute_x;
    }
    if (absolute_y > debug->maximum_step_dy) {
        debug->maximum_step_dy = absolute_y;
    }
    if (maximum == 0u) {
        ++debug->delta_zero;
    } else {
        ++debug->nonzero_moves;
        if (maximum == 1u) {
            ++debug->delta_one;
        } else if (maximum <= 3u) {
            ++debug->delta_two_three;
        } else if (maximum <= 7u) {
            ++debug->delta_four_seven;
        } else {
            ++debug->delta_eight_plus;
        }
    }
}

static void touch_debug_move(int x, int y, int dx, int dy)
{
    uint32_t now;
    if (!g_touch_debug_active) {
        return;
    }
    now = touch_debug_now();
    ++g_touch_debug_current.coordinate_events;
    touch_debug_gap(now - g_touch_debug_last_event_ms);
    touch_debug_delta(dx, dy);
    g_touch_debug_current.end_x = x;
    g_touch_debug_current.end_y = y;
    g_touch_debug_last_event_ms = now;
}

static void touch_debug_release(int x, int y, int dx, int dy)
{
    uint32_t now;
    if (!g_touch_debug_active) {
        return;
    }
    now = touch_debug_now();
    touch_debug_gap(now - g_touch_debug_last_event_ms);
    touch_debug_delta(dx, dy);
    g_touch_debug_current.release_dx = dx;
    g_touch_debug_current.release_dy = dy;
    g_touch_debug_current.end_x = x;
    g_touch_debug_current.end_y = y;
    g_touch_debug_current.duration_ms = now - g_touch_debug_start_ms;
    g_touch_debug_last_event_ms = now;
    g_touch_debug_release_pending = 1;
}

static void logical_touch_from_raw(u16 raw_x, u16 raw_y, int *x, int *y)
{
    *x = (int)RAW_SCREEN_HEIGHT - 1 - raw_y;
    *y = raw_x;
}

static int latest_touch_position(int *x, int *y)
{
    u16 raw_x;
    u16 raw_y;

    bda_gui_touch_position(&raw_x, &raw_y);
    logical_touch_from_raw(raw_x, raw_y, x, y);
    return *x >= 0 && *x < (int)LITE_SCREEN_WIDTH &&
        *y >= 0 && *y < (int)LITE_SCREEN_HEIGHT;
}

static void update_touch_position(void)
{
    int x;
    int y;

    if (!latest_touch_position(&x, &y)) {
        if (g_touch_debug_active) {
            ++g_touch_debug_current.invalid_events;
        }
        return;
    }
    if (!g_touch_down) {
        g_touch_down = 1;
        g_suppress_touch_escape = 1;
        g_touch_control = hit_test(x, y);
        if (g_touch_control == LITE_INPUT_LOOK_ZONE) {
            touch_debug_begin(x, y);
        } else {
            g_touch_pressed_pending |= g_touch_control;
        }
    } else if (g_touch_control == LITE_INPUT_LOOK_ZONE) {
        int dx = x - g_touch_x;
        int dy = y - g_touch_y;
        g_touch_dx_pending += dx;
        g_touch_dy_pending += dy;
        touch_debug_move(x, y, dx, dy);
    }
    g_touch_x = x;
    g_touch_y = y;
}

static void release_touch_position(void)
{
    int x = g_touch_x;
    int y = g_touch_y;
    int valid = latest_touch_position(&x, &y);

    if (!g_touch_down) {
        return;
    }
    if (g_touch_control == LITE_INPUT_LOOK_ZONE) {
        int dx = 0;
        int dy = 0;
        if (valid) {
            dx = x - g_touch_x;
            dy = y - g_touch_y;
            g_touch_dx_pending += dx;
            g_touch_dy_pending += dy;
            g_touch_x = x;
            g_touch_y = y;
        } else {
            ++g_touch_debug_current.invalid_events;
        }
        touch_debug_release(g_touch_x, g_touch_y, dx, dy);
    } else if (valid) {
        g_touch_x = x;
        g_touch_y = y;
    }
    g_touch_down = 0;
    g_touch_control = 0u;
}

static void release_draw_context(void)
{
    bda_handle_t draw = g_draw;
    if (!draw || (s32)draw == -1) {
        g_draw = 0;
        g_draw_owner = 0;
        return;
    }
    g_draw = 0;
    g_draw_owner = 0;
    bda_gui_end_draw(draw);
}

static int acquire_draw_context(bda_handle_t owner)
{
    if (g_draw && g_draw_owner == owner) {
        return 1;
    }
    release_draw_context();
    g_draw = bda_gui_current_draw(owner);
    if (!g_draw || (s32)g_draw == -1) {
        g_draw = 0;
        return 0;
    }
    g_draw_owner = owner;
    return 1;
}

static int app_window_proc(
    bda_handle_t handle, u32 message, u32 wparam, u32 lparam
)
{
    if (message == BDA_MSG_DRAW_CONTEXT_ATTACH) {
        g_frame = handle;
        (void)acquire_draw_context(handle);
    } else if (message == BDA_MSG_DRAW_CONTEXT_DETACH) {
        if (!g_draw_owner || g_draw_owner == handle) {
            release_draw_context();
        }
        g_direct_framebuffer = 0;
        g_direct_framebuffer_rotate_180 = 0;
        g_touch_down = 0;
        g_touch_control = 0u;
        g_detached = 1;
    }
    return bda_gui_default_proc(handle, message, wparam, lparam);
}

static uint32_t hit_test(int x, int y)
{
    if (x < 0 || y < 0 ||
        x >= (int)LITE_SCREEN_WIDTH || y >= (int)LITE_SCREEN_HEIGHT) {
        return 0u;
    }
    if (x >= LITE_BUTTON_X &&
        x < LITE_BUTTON_X + LITE_BUTTON_WIDTH) {
        if (y >= LITE_BUTTON_USE_Y &&
            y < LITE_BUTTON_USE_Y + LITE_BUTTON_HEIGHT) {
            return LITE_INPUT_USE;
        }
        if (y >= LITE_BUTTON_RELOAD_Y &&
            y < LITE_BUTTON_RELOAD_Y + LITE_BUTTON_HEIGHT) {
            return LITE_INPUT_RELOAD;
        }
        if (y >= LITE_BUTTON_WEAPON_Y &&
            y < LITE_BUTTON_WEAPON_Y + LITE_BUTTON_HEIGHT) {
            return LITE_INPUT_WEAPON;
        }
        if (y >= LITE_BUTTON_JUMP_Y &&
            y < LITE_BUTTON_JUMP_Y + LITE_BUTTON_HEIGHT) {
            return LITE_INPUT_JUMP;
        }
        if (y >= LITE_BUTTON_CROUCH_Y &&
            y < LITE_BUTTON_CROUCH_Y + LITE_BUTTON_HEIGHT) {
            return LITE_INPUT_MENU;
        }
        if (y >= LITE_BUTTON_SCORE_Y &&
            y < LITE_BUTTON_SCORE_Y + LITE_BUTTON_HEIGHT) {
            return LITE_INPUT_SCORE;
        }
        if (y >= LITE_BUTTON_ALT_Y &&
            y < LITE_BUTTON_ALT_Y + LITE_BUTTON_HEIGHT) {
            return LITE_INPUT_ALT;
        }
    }
    if (x < (int)LITE_DISPLAY_VIEW_WIDTH &&
        y < (int)LITE_DISPLAY_VIEW_HEIGHT) {
        return LITE_INPUT_LOOK_ZONE;
    }
    return 0u;
}

static uint32_t physical_input(void)
{
    bda_gui_input_packet_t packet;
    uint32_t mask = 0u;
    uint32_t now = bda_gui_tick_count_25ms();
    int escape;

    (void)bda_gui_input_packet(&packet);
    /*
     * The framebuffer is shown ccw90.  Rotate the portrait keypad into the
     * same landscape coordinate system so the visually upper key means
     * forward and the visually left/right keys strafe left/right.
     */
    if (bda_gui_input_packet_key_pressed(&packet, BDA_KEY_LEFT)) {
        mask |= LITE_INPUT_UP;
    }
    if (bda_gui_input_packet_key_pressed(&packet, BDA_KEY_RIGHT)) {
        mask |= LITE_INPUT_DOWN;
    }
    if (bda_gui_input_packet_key_pressed(&packet, BDA_KEY_DOWN)) {
        mask |= LITE_INPUT_LEFT;
    }
    if (bda_gui_input_packet_key_pressed(&packet, BDA_KEY_UP)) {
        mask |= LITE_INPUT_RIGHT;
    }
    if (bda_gui_input_packet_key_pressed(&packet, BDA_KEY_ENTER)) {
        mask |= LITE_INPUT_FIRE;
    }
    escape = bda_gui_input_packet_key_pressed(&packet, BDA_KEY_ESCAPE);
    if (g_touch_down || g_suppress_touch_escape) {
        if (!g_touch_down && !escape) {
            g_suppress_touch_escape = 0;
        }
        escape = 0;
    }
    if (escape) {
        if (!g_escape_down) {
            g_escape_down = 1;
            g_escape_start_tick = now;
        } else if (bda_gui_tick_elapsed_25ms(
                       g_escape_start_tick, now) >= ESCAPE_HOLD_TICKS) {
            g_exit_requested = 1;
        }
    } else {
        g_escape_down = 0;
    }
    return mask;
}

int lite_platform_open(void)
{
    bda_frame_desc_t descriptor;

    bda_memset(&descriptor, 0, sizeof(descriptor));

    g_frame = 0;
    g_draw = 0;
    g_draw_owner = 0;
    g_direct_framebuffer = 0;
    g_direct_framebuffer_rotate_180 = 0;
    g_direct_framebuffer_layout_hint = 0xffffffffu;
    g_direct_framebuffer_rejection_logged = 0;
    g_direct_framebuffer_refresh_logged = 0;
    g_previous_input = 0u;
    g_touch_control = 0u;
    g_touch_pressed_pending = 0u;
    g_touch_x = 0;
    g_touch_y = 0;
    g_touch_dx_pending = 0;
    g_touch_dy_pending = 0;
    g_touch_down = 0;
    g_suppress_touch_escape = 0;
    g_escape_down = 0;
    g_exit_requested = 0;
    g_detached = 0;
    g_timer_started = 0;
    bda_memset(&g_touch_debug_current, 0, sizeof(g_touch_debug_current));
    bda_memset(&g_touch_debug_ready, 0, sizeof(g_touch_debug_ready));
    g_touch_debug_next_id = 0u;
    g_touch_debug_start_ms = 0u;
    g_touch_debug_last_event_ms = 0u;
    g_touch_debug_last_pump_ms = 0u;
    g_touch_debug_active = 0;
    g_touch_debug_release_pending = 0;
    g_touch_debug_ready_available = 0;

    descriptor.title = "CS Lite";
    descriptor.wndproc = app_window_proc;
    descriptor.height = LITE_SCREEN_HEIGHT;
    descriptor.width = LITE_SCREEN_WIDTH;
    descriptor.surface = 0u;
    g_frame = bda_gui_register_frame_desc(&descriptor);
    if (!g_frame || (s32)g_frame == -1) {
        g_frame = 0;
        return 0;
    }
    (void)bda_gui_frame_activate(g_frame, 0x100u);
    (void)acquire_draw_context(g_frame);
    if (!g_draw || !refresh_direct_framebuffer(1)) {
        lite_platform_close();
        return 0;
    }
    bda_gui_millisecond_timer_start();
    g_timer_started = 1;
    g_first_millisecond = bda_gui_millisecond_count();
    return 1;
}

int lite_platform_pump(lite_input_t *input)
{
    bda_gui_raw_event_t event;
    uint32_t down;
    uint32_t events_pumped = 0u;
    uint32_t now;
    int move_pending = 0;

    if (!input || !g_frame || g_detached || g_exit_requested) {
        return 0;
    }
    /*
     * Match the proven GBA frontend: the runtime owns the raw input stream,
     * consumes a bounded batch, and reads only the latest coordinate for a
     * group of MOVE events. The window pump is reserved for final detach.
     */
    while (events_pumped < RAW_EVENT_MAX_PER_PUMP) {
        if (bda_gui_raw_event_fetch(&event) < 0) {
            break;
        }
        ++events_pumped;
        switch ((uint32_t)event.code) {
            case BDA_INPUT_EVENT_TOUCH_DOWN:
                if (!g_touch_down) {
                    update_touch_position();
                } else if (g_touch_debug_active) {
                    ++g_touch_debug_current.raw_ignored_events;
                }
                move_pending = 0;
                break;
            case BDA_INPUT_EVENT_TOUCH_MOVE:
                if (g_touch_down) {
                    move_pending = 1;
                    if (g_touch_debug_active) {
                        ++g_touch_debug_current.move_events;
                    }
                } else if (g_touch_debug_active) {
                    ++g_touch_debug_current.raw_ignored_events;
                }
                break;
            case BDA_INPUT_EVENT_TOUCH_UP:
                if (g_touch_down) {
                    release_touch_position();
                    move_pending = 0;
                } else if (g_touch_debug_active) {
                    ++g_touch_debug_current.raw_ignored_events;
                }
                break;
            default:
                if (g_touch_debug_active) {
                    ++g_touch_debug_current.raw_ignored_events;
                }
                break;
        }
    }
    if (move_pending && g_touch_down) {
        update_touch_position();
    }
    if (events_pumped == RAW_EVENT_MAX_PER_PUMP &&
        g_touch_debug_active) {
        ++g_touch_debug_current.raw_event_cap_hits;
    }
    input->touch_x = g_touch_x;
    input->touch_y = g_touch_y;
    input->touch_dx = g_touch_dx_pending;
    input->touch_dy = g_touch_dy_pending;
    input->touch_down = g_touch_down;
    now = touch_debug_now();
    if (g_touch_debug_active) {
        uint32_t pump_gap = now - g_touch_debug_last_pump_ms;
        uint32_t application_dx = absolute_i32(input->touch_dx);
        uint32_t application_dy = absolute_i32(input->touch_dy);
        ++g_touch_debug_current.pump_count;
        if (pump_gap > g_touch_debug_current.maximum_pump_gap_ms) {
            g_touch_debug_current.maximum_pump_gap_ms = pump_gap;
        }
        if (events_pumped >
            g_touch_debug_current.maximum_events_per_pump) {
            g_touch_debug_current.maximum_events_per_pump = events_pumped;
        }
        if (application_dx != 0u || application_dy != 0u) {
            ++g_touch_debug_current.application_pumps;
            if (application_dx >
                g_touch_debug_current.maximum_application_dx) {
                g_touch_debug_current.maximum_application_dx =
                    application_dx;
            }
            if (application_dy >
                g_touch_debug_current.maximum_application_dy) {
                g_touch_debug_current.maximum_application_dy =
                    application_dy;
            }
        }
        g_touch_debug_last_pump_ms = now;
        if (g_touch_debug_release_pending) {
            g_touch_debug_ready = g_touch_debug_current;
            g_touch_debug_ready_available = 1;
            g_touch_debug_active = 0;
            g_touch_debug_release_pending = 0;
        }
    }
    g_touch_dx_pending = 0;
    g_touch_dy_pending = 0;
    down = physical_input() | g_touch_control;
    input->down = down;
    input->pressed =
        (down & ~g_previous_input) | g_touch_pressed_pending;
    input->released = g_previous_input & ~down;
    g_touch_pressed_pending = 0u;
    g_previous_input = down;
    return !g_detached && !g_exit_requested;
}

int lite_platform_touch_debug_take(lite_touch_debug_t *debug)
{
    if (!debug || !g_touch_debug_ready_available) {
        return 0;
    }
    *debug = g_touch_debug_ready;
    g_touch_debug_ready_available = 0;
    return 1;
}

int lite_platform_present(const uint16_t *rgb565)
{
    if (!rgb565 || g_detached || !refresh_direct_framebuffer(0)) {
        return 0;
    }
    lite_display_present_landscape_rgb565(
        rgb565,
        g_direct_framebuffer,
        g_direct_framebuffer_rotate_180
    );
    return 1;
}

int lite_platform_present_top(const uint16_t *rgb565)
{
    return lite_platform_present(rgb565);
}

uint32_t lite_platform_milliseconds(void)
{
    return bda_gui_millisecond_count() - g_first_millisecond;
}

void lite_platform_delay(uint32_t units)
{
    bda_sys_delay(units);
}

void lite_platform_close(void)
{
    bda_gui_message_t message;
    uint32_t pumps = 0u;

    g_direct_framebuffer = 0;
    g_direct_framebuffer_rotate_180 = 0;
    if (g_timer_started) {
        bda_gui_millisecond_timer_stop();
        g_timer_started = 0;
    }
    if (!g_frame) {
        release_draw_context();
        return;
    }
    bda_memset(&message, 0, sizeof(message));
    (void)bda_gui_frame_stop(g_frame);
    (void)bda_gui_frame_release(g_frame);
    while (!g_detached && pumps < CLOSE_PUMP_LIMIT) {
        if (!bda_gui_event_pump_frame_once(&message, g_frame)) {
            break;
        }
        ++pumps;
        bda_sys_delay(1u);
    }
    release_draw_context();
    bda_gui_close_frame(g_frame);
    g_frame = 0;
}

uint16_t lite_rgb565(uint32_t red, uint32_t green, uint32_t blue)
{
    if (red > 255u) red = 255u;
    if (green > 255u) green = 255u;
    if (blue > 255u) blue = 255u;
    return (uint16_t)(((red & 0xf8u) << 8) |
                      ((green & 0xfcu) << 3) |
                      (blue >> 3));
}
