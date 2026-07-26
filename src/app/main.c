/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bda_types.h"
#include "bda_memory.h"

#include "assets/pak.h"
#include "audio/sound.h"
#include "core/log.h"
#include "core/memory.h"
#include "model/model.h"
#include "platform/bbk9588.h"
#include "render/framebuffer.h"
#include "render/model.h"
#include "render/world.h"
#include "world/map.h"
#include "world/movement.h"

#define ASSET_PATH \
    "A:\\\xd3\xa6\xd3\xc3\\\xca\xfd\xbe\xdd\\CS15LITE\\CS15.C15PAK"
/*
 * Exact offline-verified high-water budgets for the three-map pack.
 * Dust2 peaks at 744688 map bytes and 220800 texture bytes.
 */
#define MAP_ARENA_BYTES 744704u
#define TEXTURE_ARENA_BYTES 220800u
/*
 * Player skins are prefiltered to 16x16 offline. This leaves both team
 * meshes, both held weapons and the largest animated view model below 64 KiB
 * without runtime texture conversion or per-frame model streaming.
 */
#define MODEL_ARENA_BYTES (64u * 1024u)
#define LOAD_SCRATCH_BYTES 2048u
#define FRAME_INTERVAL_MS 40u
#define LOGIC_INTERVAL_MS 40u
#define METRIC_INTERVAL_MS 5000u
#define FIRE_FEEDBACK_MS 160u
#define HIT_FEEDBACK_MS 140u
#define ROUND_END_MS 2500u
#define ROUND_TIME_MS 90000u
#define BOT_COUNT 7u
#define BOT_TEAMMATES 3u
#define BOT_MOVE_PER_TICK 4u
#define BOT_SIGHT_DISTANCE 1200u
#define BOT_CLOSE_AWARE_DISTANCE 180u
#define BOT_HOLD_MIN_DISTANCE 240u
#define BOT_HOLD_MAX_DISTANCE 560u
#define TEAM_T 1u
#define TEAM_CT 2u

enum game_screen {
    SCREEN_MAIN,
    SCREEN_OPTIONS,
    SCREEN_MAP,
    SCREEN_TEAM,
    SCREEN_BUY,
    SCREEN_PLAY,
    SCREEN_ROUND_END
};

enum bot_difficulty {
    BOT_EASY,
    BOT_NORMAL,
    BOT_HARD,
    BOT_DIFFICULTY_COUNT
};

enum map_id {
    MAP_DE_DUST,
    MAP_DE_DUST2,
    MAP_ICEWORLD,
    MAP_COUNT
};

enum weapon_id {
    WEAPON_KNIFE,
    WEAPON_GLOCK,
    WEAPON_AK47,
    WEAPON_M4A1,
    WEAPON_USP,
    WEAPON_COUNT
};

typedef struct c15_bot {
    c15_player_t mover;
    uint32_t next_fire;
    uint32_t next_decision;
    uint16_t health;
    uint16_t aim_seed;
    uint8_t team;
    uint8_t weapon;
    uint8_t alive;
    uint8_t target_visible_last;
    uint8_t stuck_turn;
    uint8_t nav_index;
    uint8_t nav_stalls;
    uint8_t moving;
    uint8_t strafe_right;
    uint8_t burst_left;
    uint8_t combat;
} c15_bot_t;

typedef struct c15_nav_point_3d {
    int16_t x;
    int16_t y;
    int16_t z;
} c15_nav_point_3d_t;

typedef struct c15_muzzle_sprite {
    const uint8_t *chunk;
    const uint8_t *anchors;
    const uint8_t *pixels;
    uint8_t width;
    uint8_t height;
    uint8_t frame_count;
    uint8_t anchor_count;
    uint8_t display_size;
    uint8_t loaded;
} c15_muzzle_sprite_t;

static const char *const g_map_assets[MAP_COUNT] = {
    "maps/de_dust",
    "maps/de_dust2",
    "maps/fy_iceworld"
};

static const char *const g_map_labels[MAP_COUNT] = {
    "DE_DUST",
    "DE_DUST2",
    "ICEWORLD"
};

static const char *const g_map_loading[MAP_COUNT] = {
    "LOADING DE_DUST + PLAYERS",
    "LOADING DE_DUST2 + PLAYERS",
    "LOADING ICEWORLD + PLAYERS"
};

/*
 * A 48-unit hull-safe centerline generated offline from de_dust hull 1.
 * It is shared by all bots; there is no per-bot path.
 */
#define DUST_ROUTE_COUNT 187u
/*
 * The 186 cardinal 48-unit steps use two bits each:
 * 0=+X, 1=-X, 2=+Y, 3=-Y.  This preserves the original hull-safe path
 * while reducing its resident footprint from 748 bytes to 47 bytes.
 */
static const uint8_t g_dust_route_steps[] = {
    0xaa,0x2a,0x02,0x00,0xa2,0xaa,0xaa,0xaa,0xaa,0xaa,0xaa,0x80,
    0xaa,0xaa,0xaa,0x2a,0x00,0x00,0xa0,0xa8,0xaa,0x5a,0x59,0x95,
    0x99,0xa9,0xaa,0xaa,0xaa,0x02,0x00,0x80,0xaa,0xaa,0xaa,0xaa,
    0x99,0x59,0x66,0xa9,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

/*
 * Compact hull-1 centerline from the CT spawn to the T spawn. It was
 * generated from the authorized de_dust2 BSP at a 32-unit grid spacing.
 */
static const c15_nav_point_3d_t g_dust2_route[] = {
    { 224, 2208, -91}, { -96, 2208, -91}, { -96, 2176, -91},
    {-416, 2176, -91}, {-416, 1632, -91}, {-352, 1632, -91},
    {-352, 1568, -91}, {-416, 1568, -91}, {-416, 1280, -91},
    {-416,  768,  37}, {-416, -480,  37}, {-384, -480,  37},
    {-384, -512,  37}, {-352, -512,  37}, {-352, -544,  37},
    {  32, -544,  37}, {  32, -672,  37}, {   0, -672,  37},
    {-512, -672, 165}, {-736, -672, 165}, {-736, -800, 165}
};

static const c15_nav_point_3d_t g_iceworld_route[] = {
    {384,   32, -195},
    {384, -128, -195},
    {384, -1152, -195},
    {384, -1280, -195}
};

#define DUST2_ROUTE_COUNT \
    ((uint32_t)(sizeof(g_dust2_route) / sizeof(g_dust2_route[0])))
#define ICEWORLD_ROUTE_COUNT \
    ((uint32_t)(sizeof(g_iceworld_route) / sizeof(g_iceworld_route[0])))

static int32_t dust_route_z(uint8_t index)
{
    if (index <= 4u) return 100;
    if (index == 5u) return 84;
    if (index == 6u) return 68;
    if (index <= 8u) return 52;
    if (index <= 77u) return 36;
    if (index == 78u) return 52;
    if (index <= 111u) return 68;
    if (index == 112u) return 52;
    if (index <= 163u) return 36;
    if (index <= 173u) {
        return 28 - (int32_t)(index - 164u) * 12;
    }
    return -92;
}

static uint32_t map_route_count(uint32_t map_id)
{
    if (map_id == MAP_DE_DUST) return DUST_ROUTE_COUNT;
    if (map_id == MAP_DE_DUST2) return DUST2_ROUTE_COUNT;
    return ICEWORLD_ROUTE_COUNT;
}

static int map_route_point(
    uint32_t map_id,
    uint8_t index,
    int32_t *x,
    int32_t *y,
    int32_t *z
)
{
    if (map_id == MAP_DE_DUST && index < DUST_ROUTE_COUNT) {
        uint8_t step;
        *x = -528;
        *y = -1728;
        for (step = 0u; step < index; ++step) {
            uint8_t direction = (uint8_t)(
                (g_dust_route_steps[step >> 2] >>
                 ((step & 3u) * 2u)) & 3u
            );
            if (direction == 0u) *x += 48;
            else if (direction == 1u) *x -= 48;
            else if (direction == 2u) *y += 48;
            else *y -= 48;
        }
        *z = dust_route_z(index);
        return 1;
    }
    if (map_id == MAP_DE_DUST2 && index < DUST2_ROUTE_COUNT) {
        *x = g_dust2_route[index].x;
        *y = g_dust2_route[index].y;
        *z = g_dust2_route[index].z;
        return 1;
    }
    if (map_id == MAP_ICEWORLD && index < ICEWORLD_ROUTE_COUNT) {
        *x = g_iceworld_route[index].x;
        *y = g_iceworld_route[index].y;
        *z = g_iceworld_route[index].z;
        return 1;
    }
    return 0;
}

static uint16_t g_screen[LITE_SCREEN_WIDTH * LITE_SCREEN_HEIGHT];
static uint16_t g_depth[LITE_VIEW_WIDTH * LITE_VIEW_HEIGHT];
static uint8_t g_map_memory[MAP_ARENA_BYTES];
static uint8_t g_texture_memory[TEXTURE_ARENA_BYTES];
static uint8_t g_model_memory[MODEL_ARENA_BYTES];
static uint8_t g_load_scratch[LOAD_SCRATCH_BYTES]
    __attribute__((aligned(4)));
static uint8_t g_visible_surfaces[C15_MAP_VISIBLE_BYTES];
static c15_pak_t g_pak;
static c15_map_t g_map;
static c15_model_t g_view_model;
static c15_model_animation_t g_view_animation;
static c15_model_t g_t_model;
static c15_model_t g_ct_model;
static c15_model_t g_t_weapon_model;
static c15_model_t g_ct_weapon_model;
static c15_model_animation_t g_t_locomotion;
static c15_model_animation_t g_ct_locomotion;
static c15_model_animation_t g_t_weapon_locomotion;
static c15_model_animation_t g_ct_weapon_locomotion;
static c15_muzzle_sprite_t g_muzzle;
static const uint16_t *g_menu_background;

static const char *const g_weapon_assets[WEAPON_COUNT] = {
    "mdl/v_knife",
    "mdl/v_glock18",
    "mdl/v_ak47",
    "mdl/v_m4a1",
    "mdl/v_usp"
};

static const char *const g_weapon_animation_assets[WEAPON_COUNT] = {
    "anim/v_knife",
    "anim/v_glock18",
    "anim/v_ak47",
    "anim/v_m4a1",
    "anim/v_usp"
};

static const char *const g_difficulty_labels[BOT_DIFFICULTY_COUNT] = {
    "DIFFICULTY  EASY",
    "DIFFICULTY  NORMAL",
    "DIFFICULTY  HARD"
};

static const uint16_t g_bot_reaction_ms[BOT_DIFFICULTY_COUNT] = {
    850u, 500u, 260u
};

static const uint16_t g_bot_burst_interval_ms[BOT_DIFFICULTY_COUNT] = {
    190u, 150u, 115u
};

static const uint16_t g_bot_burst_pause_ms[BOT_DIFFICULTY_COUNT] = {
    950u, 700u, 450u
};

static const uint16_t g_bot_accuracy[BOT_DIFFICULTY_COUNT][3] = {
    {0x5800u, 0x4000u, 0x2800u},
    {0x7000u, 0x5000u, 0x3400u},
    {0x9000u, 0x7000u, 0x5000u}
};

static const uint8_t g_bot_combat_speed[BOT_DIFFICULTY_COUNT] = {
    3u, 4u, 5u
};

static const uint8_t g_bot_damage[BOT_DIFFICULTY_COUNT] = {
    6u, 8u, 10u
};

static const char *const g_weapon_muzzle_assets[WEAPON_COUNT] = {
    "",
    "muzzle/v_glock18",
    "muzzle/v_ak47",
    "muzzle/v_m4a1",
    "muzzle/v_usp"
};

static int resource_pack_has(
    const char *name,
    uint32_t expected_type
)
{
    const c15_pak_entry_t *entry = c15_pak_find(&g_pak, name);
    if (entry && entry->type == expected_type) {
        return 1;
    }
    lite_log_line("resource pack missing asset");
    lite_log_line(name);
    return 0;
}

static int resource_pack_is_current(uint32_t map_id)
{
    static const char *const world_models[] = {
        "mdl/player_terror",
        "mdl/player_urban",
        "mdl/p_ak47",
        "mdl/p_m4a1"
    };
    static const char *const world_animations[] = {
        "anim/player_terror",
        "anim/player_urban",
        "anim/p_ak47",
        "anim/p_m4a1"
    };
    uint32_t index;
    int complete = resource_pack_has(
        "meta/m11", C15_FOURCC('V','E','R','0')
    );
    if (!resource_pack_has(
            "sound/game", C15_FOURCC('S','N','D','0'))) {
        complete = 0;
    }
    if (map_id >= MAP_COUNT ||
        !resource_pack_has(g_map_assets[map_id],
                           C15_FOURCC('B','S','P','0'))) {
        complete = 0;
    }
    for (index = 0u; index < WEAPON_COUNT; ++index) {
        if (!resource_pack_has(
                g_weapon_assets[index],
                C15_FOURCC('M','D','L','0')) ||
            !resource_pack_has(
                g_weapon_animation_assets[index],
                C15_FOURCC('A','N','M','0'))) {
            complete = 0;
        }
        if (index != WEAPON_KNIFE &&
            !resource_pack_has(
                g_weapon_muzzle_assets[index],
                C15_FOURCC('M','S','P','0'))) {
            complete = 0;
        }
    }
    for (index = 0u;
         index < sizeof(world_models) / sizeof(world_models[0]); ++index) {
        if (!resource_pack_has(
                world_models[index],
                C15_FOURCC('M','D','L','0')) ||
            !resource_pack_has(
                world_animations[index],
                C15_FOURCC('A','N','M','0'))) {
            complete = 0;
        }
    }
    return complete;
}

static const char *const g_weapon_labels[WEAPON_COUNT] = {
    "KNIFE", "GLOCK", "AK47", "M4A1", "USP"
};

static const uint16_t g_weapon_capacity[WEAPON_COUNT] = {
    0u, 20u, 30u, 30u, 12u
};

static const uint16_t g_weapon_interval_ms[WEAPON_COUNT] = {
    350u, 240u, 110u, 105u, 220u
};

static const uint8_t g_weapon_damage[WEAPON_COUNT] = {
    50u, 25u, 36u, 33u, 30u
};

static const c15_model_view_t g_weapon_views[WEAPON_COUNT] = {
    {160, 120, 160, 0, 80},
    {160, 120, 160, 0, 80},
    /*
     * The AK47 idle/fire poses extend much farther toward the camera than
     * the pistol poses.  A small depth bias keeps the receiver intact at
     * the near plane; the matching focal length and upper-left shift retain
     * the original first-person size while keeping the magazine and stock
     * inside the 320x240 view.
     */
    {145, 88, 155, 40, 48},
    {160, 120, 160, 0, 80},
    {160, 120, 160, 0, 80}
};

typedef struct c15_view_animation_state {
    uint32_t action;
    uint32_t frame;
    uint32_t next_frame;
} c15_view_animation_state_t;

typedef struct c15_world_animation_state {
    uint8_t action;
    uint8_t frame;
} c15_world_animation_state_t;

static int time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static int time_active(uint32_t now, uint32_t deadline)
{
    return (int32_t)(deadline - now) > 0;
}

static int bytes_equal(
    const uint8_t *left, const char *right, uint32_t count
)
{
    uint32_t index;
    for (index = 0u; index < count; ++index) {
        if (left[index] != (uint8_t)right[index]) {
            return 0;
        }
    }
    return 1;
}

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)(
        (uint16_t)data[0] | ((uint16_t)data[1] << 8)
    );
}

static int16_t read_i16(const uint8_t *data)
{
    return (int16_t)read_u16(data);
}

static uint32_t abs_i32(int32_t value)
{
    return (uint32_t)(value < 0 ? -value : value);
}

static uint32_t distance_squared(
    int32_t ax, int32_t ay, int32_t bx, int32_t by
)
{
    int32_t dx = ax - bx;
    int32_t dy = ay - by;
    uint32_t x = abs_i32(dx);
    uint32_t y = abs_i32(dy);
    if (x > 32767u || y > 32767u) {
        return 0xffffffffu;
    }
    return x * x + y * y;
}

static uint8_t yaw_from_delta(int32_t dx, int32_t dy)
{
    uint32_t ax = abs_i32(dx);
    uint32_t ay = abs_i32(dy);
    uint32_t angle;
    if (ax == 0u && ay == 0u) {
        return 0u;
    }
    if (ax >= ay) {
        angle = ax != 0u ? ay * 32u / ax : 0u;
    } else {
        angle = 64u - ax * 32u / ay;
    }
    if (dx >= 0 && dy >= 0) return (uint8_t)angle;
    if (dx < 0 && dy >= 0) return (uint8_t)(128u - angle);
    if (dx < 0 && dy < 0) return (uint8_t)(128u + angle);
    return (uint8_t)(256u - angle);
}

static uint32_t yaw_distance(uint8_t first, uint8_t second)
{
    int32_t difference = (int8_t)(first - second);
    return abs_i32(difference);
}

static int32_t sin_q14_q8(uint16_t angle_q8)
{
    uint8_t angle = (uint8_t)(angle_q8 >> 8);
    uint8_t fraction = (uint8_t)angle_q8;
    int32_t first = c15_sin_q14(angle);
    int32_t second = c15_sin_q14((uint8_t)(angle + 1u));
    return first + ((second - first) * fraction >> 8);
}

static int32_t cos_q14_q8(uint16_t angle_q8)
{
    return sin_q14_q8((uint16_t)(angle_q8 + (64u << 8)));
}

static void draw_button(
    lite_framebuffer_t *fb,
    int x, int y, int width, int height,
    const char *label, int active
)
{
    int label_width = 0;
    const char *cursor = label;
    uint16_t border = active ?
        lite_rgb565(235u, 174u, 55u) : lite_rgb565(82u, 96u, 82u);
    uint16_t body = active ?
        lite_rgb565(116u, 80u, 36u) : lite_rgb565(32u, 46u, 43u);
    while (*cursor++) {
        label_width += 6;
    }
    lite_fb_blend_rect(fb, x, y, width, height, body);
    lite_fb_frame(fb, x, y, width, height, border);
    lite_fb_text(
        fb, x + (width - label_width) / 2,
        y + (height - 7) / 2, label, 1,
        lite_rgb565(235u, 235u, 218u)
    );
}

static void draw_controls(lite_framebuffer_t *fb, const lite_input_t *input)
{
    draw_button(
        fb, LITE_BUTTON_X, LITE_BUTTON_USE_Y,
        LITE_BUTTON_WIDTH, LITE_BUTTON_HEIGHT, "BUY",
        (input->down & LITE_INPUT_USE) != 0u
    );
    draw_button(
        fb, LITE_BUTTON_X, LITE_BUTTON_RELOAD_Y,
        LITE_BUTTON_WIDTH, LITE_BUTTON_HEIGHT, "RELOAD",
        (input->down & LITE_INPUT_RELOAD) != 0u
    );
    draw_button(
        fb, LITE_BUTTON_X, LITE_BUTTON_WEAPON_Y,
        LITE_BUTTON_WIDTH, LITE_BUTTON_HEIGHT, "WEAPON",
        (input->down & LITE_INPUT_WEAPON) != 0u
    );
    draw_button(
        fb, LITE_BUTTON_X, LITE_BUTTON_JUMP_Y,
        LITE_BUTTON_WIDTH, LITE_BUTTON_HEIGHT, "JUMP",
        (input->down & LITE_INPUT_JUMP) != 0u
    );
    draw_button(
        fb, LITE_BUTTON_X, LITE_BUTTON_CROUCH_Y,
        LITE_BUTTON_WIDTH, LITE_BUTTON_HEIGHT, "CROUCH",
        (input->down & LITE_INPUT_MENU) != 0u
    );
}

static void draw_fps(
    lite_framebuffer_t *fb, uint32_t fps_x10,
    uint16_t label_color, uint16_t value_color
)
{
    uint32_t whole = fps_x10 / 10u;
    int digits = whole >= 100u ? 3 : (whole >= 10u ? 2 : 1);
    int value_x = 28;
    int dot_x = value_x + digits * 6;
    lite_fb_text(fb, 4, 4, "FPS", 1, label_color);
    lite_fb_u32(fb, value_x, 4, whole, 1, value_color);
    lite_fb_text(fb, dot_x, 4, ".", 1, value_color);
    lite_fb_u32(fb, dot_x + 6, 4, fps_x10 % 10u, 1, value_color);
}

static void draw_menu_chrome(
    lite_framebuffer_t *fb, const char *subtitle
)
{
    uint16_t background = lite_rgb565(12u, 20u, 19u);
    uint16_t grid = lite_rgb565(45u, 53u, 40u);
    uint16_t gold = lite_rgb565(224u, 168u, 54u);
    uint16_t pale = lite_rgb565(211u, 214u, 190u);
    int x;
    int y;
    if (g_menu_background) {
        uint32_t index;
        for (index = 0u;
             index < LITE_SCREEN_WIDTH * LITE_SCREEN_HEIGHT; ++index) {
            fb->pixels[index] = g_menu_background[index];
        }
        lite_fb_blend_rect(fb, 14, 48, 216, 180, background);
    } else {
        lite_fb_clear(fb, background);
        for (x = 228; x < 320; x += 12) {
            lite_fb_line(fb, x, 0, x - 70, 239, grid);
        }
        for (y = 0; y < 240; y += 12) {
            lite_fb_line(fb, 228, y, 319, y, grid);
        }
        lite_fb_text(fb, 24, 18, "COUNTER-STRIKE", 2, gold);
        lite_fb_text(fb, 25, 38, "1.5 LITE", 1, pale);
    }
    lite_fb_line(fb, 24, 52, 218, 52, gold);
    lite_fb_text(fb, 25, 59, subtitle, 1, pale);
    lite_fb_text(fb, 25, 226, "ARROWS  ENTER  TOUCH", 1, grid);
}

static int load_menu_background(lite_arena_t *texture_arena)
{
    const c15_pak_entry_t *entry =
        c15_pak_find(&g_pak, "ui/menu_splash");
    uint8_t header[16];
    uint32_t pixel_bytes;
    uint16_t *pixels;
    if (!entry || entry->type != C15_FOURCC('I','M','G','0') ||
        entry->packed_size < sizeof(header) ||
        !c15_pak_validate_entry(
            &g_pak, entry, g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_pak_read(
            &g_pak, entry, 0u, header, sizeof(header)) ||
        !bytes_equal(header, "C15IMG1\0", 8u) ||
        (uint16_t)(header[8] | ((uint16_t)header[9] << 8)) !=
            LITE_SCREEN_WIDTH ||
        (uint16_t)(header[10] | ((uint16_t)header[11] << 8)) !=
            LITE_SCREEN_HEIGHT) {
        return 0;
    }
    pixel_bytes = (uint32_t)header[12] |
        ((uint32_t)header[13] << 8) |
        ((uint32_t)header[14] << 16) |
        ((uint32_t)header[15] << 24);
    if (pixel_bytes !=
            LITE_SCREEN_WIDTH * LITE_SCREEN_HEIGHT * 2u ||
        entry->packed_size != sizeof(header) + pixel_bytes) {
        return 0;
    }
    pixels = (uint16_t *)lite_arena_alloc(
        texture_arena, pixel_bytes, 16u
    );
    if (!pixels || !c15_pak_read(
            &g_pak, entry, sizeof(header), pixels, pixel_bytes)) {
        return 0;
    }
    g_menu_background = pixels;
    return 1;
}

static void draw_main_menu(lite_framebuffer_t *fb, uint32_t selection)
{
    draw_menu_chrome(fb, "MAIN MENU");
    draw_button(fb, 34, 88, 176, 30, "NEW GAME", selection == 0u);
    draw_button(fb, 34, 126, 176, 30, "OPTIONS", selection == 1u);
    draw_button(fb, 34, 164, 176, 30, "QUIT", selection == 2u);
}

static void draw_options_menu(
    lite_framebuffer_t *fb,
    uint32_t selection,
    uint8_t difficulty,
    uint8_t audio_enabled
)
{
    draw_menu_chrome(fb, "OPTIONS");
    draw_button(
        fb, 34, 88, 184, 30,
        g_difficulty_labels[difficulty], selection == 0u
    );
    draw_button(
        fb, 34, 126, 184, 30,
        audio_enabled ? "AUDIO  ON" : "AUDIO  OFF",
        selection == 1u
    );
    draw_button(fb, 34, 172, 184, 28, "BACK", selection == 2u);
}

static void draw_map_menu(lite_framebuffer_t *fb, uint32_t selection)
{
    uint16_t pale = lite_rgb565(211u, 214u, 190u);
    draw_menu_chrome(fb, "CREATE GAME - SELECT MAP");
    lite_fb_text(fb, 35, 68, "AVAILABLE MAPS", 1, pale);
    draw_button(fb, 34, 84, 176, 27, "DE_DUST", selection == 0u);
    draw_button(fb, 34, 116, 176, 27, "DE_DUST2", selection == 1u);
    draw_button(fb, 34, 148, 176, 27, "ICEWORLD", selection == 2u);
    draw_button(fb, 34, 188, 176, 25, "BACK", selection == 3u);
}

static void draw_team_menu(lite_framebuffer_t *fb, uint32_t selection)
{
    draw_menu_chrome(fb, "SELECT A TEAM");
    draw_button(
        fb, 28, 85, 194, 30, "TERRORIST", selection == 0u
    );
    draw_button(
        fb, 28, 123, 194, 30, "COUNTER-TERRORIST", selection == 1u
    );
    draw_button(fb, 28, 161, 194, 26, "AUTO SELECT", selection == 2u);
    draw_button(fb, 28, 194, 194, 24, "BACK", selection == 3u);
}

static void draw_loading(
    lite_framebuffer_t *fb, const char *line, uint16_t color
)
{
    uint16_t white = lite_rgb565(225u, 229u, 210u);
    lite_fb_clear(fb, lite_rgb565(10u, 18u, 18u));
    lite_fb_text(fb, 38, 70, "COUNTER-STRIKE", 2, color);
    lite_fb_text(fb, 38, 96, "CS LITE", 2, white);
    lite_fb_text(fb, 38, 132, line, 1, white);
}

static void draw_buy_menu(
    lite_framebuffer_t *fb,
    uint8_t team,
    uint32_t money,
    uint32_t selection,
    uint8_t rifle_owned
)
{
    uint16_t pale = lite_rgb565(211u, 214u, 190u);
    uint16_t gold = lite_rgb565(224u, 168u, 54u);
    const char *pistol = team == TEAM_T ? "GLOCK  FREE" : "USP  FREE";
    const char *rifle = team == TEAM_T ? "AK47  2500" : "M4A1  3100";
    draw_menu_chrome(fb, "BUY EQUIPMENT");
    lite_fb_text(fb, 34, 78, "MONEY", 1, pale);
    lite_fb_u32(fb, 76, 78, money, 1, gold);
    draw_button(fb, 34, 96, 184, 30, pistol, selection == 0u);
    draw_button(fb, 34, 134, 184, 30, rifle, selection == 1u);
    if (rifle_owned) {
        lite_fb_text(fb, 224, 145, "OWNED", 1, gold);
    }
    draw_button(fb, 34, 180, 184, 30, "START ROUND", selection == 2u);
}

static uint32_t movement_controls(const lite_input_t *input)
{
    uint32_t controls = 0u;
    if ((input->down & LITE_INPUT_UP) != 0u) {
        controls |= C15_MOVE_FORWARD;
    }
    if ((input->down & LITE_INPUT_DOWN) != 0u) {
        controls |= C15_MOVE_BACK;
    }
    if ((input->down & LITE_INPUT_LEFT) != 0u) {
        controls |= C15_MOVE_LEFT;
    }
    if ((input->down & LITE_INPUT_RIGHT) != 0u) {
        controls |= C15_MOVE_RIGHT;
    }
    if ((input->down & LITE_INPUT_MENU) != 0u) {
        controls |= C15_MOVE_CROUCH;
    }
    return controls;
}

static int find_team_spawn(
    const c15_map_t *map,
    uint8_t wanted_team,
    uint32_t ordinal,
    c15_camera_t *camera
)
{
    uint32_t count = c15_map_spawn_count(map);
    uint32_t index;
    uint32_t found = 0u;
    for (index = 0u; index < count; ++index) {
        c15_camera_t candidate;
        uint8_t team;
        if (c15_map_spawn(map, index, &candidate, &team) &&
            team == wanted_team) {
            if (found == ordinal) {
                *camera = candidate;
                return 1;
            }
            ++found;
        }
    }
    if (ordinal != 0u) {
        return find_team_spawn(map, wanted_team, 0u, camera);
    }
    return 0;
}

static int load_muzzle_sprite(
    lite_arena_t *model_arena, uint32_t weapon
)
{
    const c15_pak_entry_t *entry;
    uint8_t *chunk;
    uint32_t frame_bytes;
    uint32_t expected;
    bda_memset(&g_muzzle, 0, sizeof(g_muzzle));
    if (weapon == WEAPON_KNIFE) {
        return 1;
    }
    entry = c15_pak_find(&g_pak, g_weapon_muzzle_assets[weapon]);
    if (!entry || entry->type != C15_FOURCC('M','S','P','0') ||
        entry->packed_size < 44u || entry->packed_size > 2048u) {
        return 0;
    }
    chunk = (uint8_t *)lite_arena_alloc(
        model_arena, entry->packed_size, 16u
    );
    if (!chunk ||
        !c15_pak_read(
            &g_pak, entry, 0u, chunk, entry->packed_size) ||
        !bytes_equal(chunk, "MSP1", 4u) ||
        read_u16(chunk + 4) != entry->packed_size) {
        return 0;
    }
    g_muzzle.width = chunk[6];
    g_muzzle.height = chunk[7];
    g_muzzle.frame_count = chunk[8];
    g_muzzle.anchor_count = chunk[9];
    g_muzzle.display_size = chunk[10];
    frame_bytes = (
        (uint32_t)g_muzzle.width * g_muzzle.height + 1u
    ) / 2u;
    expected = 44u + (uint32_t)g_muzzle.anchor_count * 6u +
        (uint32_t)g_muzzle.frame_count * frame_bytes;
    if (g_muzzle.width == 0u || g_muzzle.width > 32u ||
        g_muzzle.height == 0u || g_muzzle.height > 32u ||
        g_muzzle.frame_count == 0u || g_muzzle.frame_count > 4u ||
        g_muzzle.anchor_count == 0u || g_muzzle.anchor_count > 16u ||
        g_muzzle.display_size == 0u || g_muzzle.display_size > 64u ||
        chunk[11] != 0u || expected != entry->packed_size) {
        bda_memset(&g_muzzle, 0, sizeof(g_muzzle));
        return 0;
    }
    g_muzzle.chunk = chunk;
    g_muzzle.anchors = chunk + 44u;
    g_muzzle.pixels = g_muzzle.anchors +
        (uint32_t)g_muzzle.anchor_count * 6u;
    g_muzzle.loaded = 1u;
    return 1;
}

static void change_weapon(
    lite_arena_t *model_arena,
    size_t persistent_models,
    uint32_t *weapon,
    uint32_t requested
)
{
    lite_arena_rewind(model_arena, persistent_models);
    *weapon = requested;
    if (!c15_model_load(
            &g_view_model, &g_pak, g_weapon_assets[requested],
            model_arena, g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_animation_open(
            &g_view_animation, &g_view_model, &g_pak,
            g_weapon_animation_assets[requested],
            g_load_scratch, sizeof(g_load_scratch)) ||
        !load_muzzle_sprite(model_arena, requested)) {
        lite_log_line("view model load failed");
        bda_memset(&g_view_model, 0, sizeof(g_view_model));
        bda_memset(&g_view_animation, 0, sizeof(g_view_animation));
        bda_memset(&g_muzzle, 0, sizeof(g_muzzle));
    }
}

static void start_view_animation(
    c15_view_animation_state_t *state,
    uint32_t action,
    uint32_t now
)
{
    const c15_model_animation_sequence_t *sequence;
    if (!state || !g_view_animation.loaded ||
        action >= C15_VIEW_ANIMATION_COUNT) {
        return;
    }
    sequence = &g_view_animation.sequences[action];
    state->action = action;
    state->frame = 0u;
    state->next_frame = now + sequence->frame_ms;
    (void)c15_model_animation_apply(
        &g_view_animation, &g_view_model, &g_pak,
        action, 0u, g_load_scratch, sizeof(g_load_scratch)
    );
}

static int update_view_animation(
    c15_view_animation_state_t *state,
    uint32_t now
)
{
    const c15_model_animation_sequence_t *sequence;
    uint32_t completed;
    if (!state || !g_view_animation.loaded ||
        state->action >= C15_VIEW_ANIMATION_COUNT ||
        !time_reached(now, state->next_frame)) {
        return -1;
    }
    sequence = &g_view_animation.sequences[state->action];
    ++state->frame;
    if (state->frame >= sequence->frame_count) {
        completed = state->action;
        state->action = C15_VIEW_ANIMATION_IDLE;
        state->frame = 0u;
        sequence = &g_view_animation.sequences[state->action];
        state->next_frame = now + sequence->frame_ms;
        (void)c15_model_animation_apply(
            &g_view_animation, &g_view_model, &g_pak,
            state->action, state->frame,
            g_load_scratch, sizeof(g_load_scratch)
        );
        return (int)completed;
    }
    state->next_frame = now + sequence->frame_ms;
    (void)c15_model_animation_apply(
        &g_view_animation, &g_view_model, &g_pak,
        state->action, state->frame,
        g_load_scratch, sizeof(g_load_scratch)
    );
    return -1;
}

static int team_is_moving(
    const c15_bot_t bots[BOT_COUNT], uint8_t team
)
{
    uint32_t index;
    for (index = 0u; index < BOT_COUNT; ++index) {
        if (bots[index].alive && bots[index].team == team &&
            bots[index].moving) {
            return 1;
        }
    }
    return 0;
}

static void update_team_animation(
    c15_world_animation_state_t *state,
    c15_model_animation_t *body_animation,
    c15_model_t *body,
    c15_model_animation_t *weapon_animation,
    c15_model_t *weapon,
    int moving,
    uint32_t now
)
{
    uint32_t action = moving ? 1u : 0u;
    const c15_model_animation_sequence_t *sequence =
        &body_animation->sequences[action];
    uint32_t frame = action != 0u ?
        (now / sequence->frame_ms) % sequence->frame_count : 0u;
    if (state->action == action && state->frame == frame) {
        return;
    }
    if (!c15_model_animation_apply(
            body_animation, body, &g_pak, action, frame,
            g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_animation_apply(
            weapon_animation, weapon, &g_pak, action, frame,
            g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("world animation frame read failed");
        return;
    }
    state->action = (uint8_t)action;
    state->frame = (uint8_t)frame;
}

static void update_world_animations(
    c15_world_animation_state_t *t_state,
    c15_world_animation_state_t *ct_state,
    const c15_bot_t bots[BOT_COUNT],
    uint32_t now
)
{
    update_team_animation(
        t_state, &g_t_locomotion, &g_t_model,
        &g_t_weapon_locomotion, &g_t_weapon_model,
        team_is_moving(bots, TEAM_T), now
    );
    update_team_animation(
        ct_state, &g_ct_locomotion, &g_ct_model,
        &g_ct_weapon_locomotion, &g_ct_weapon_model,
        team_is_moving(bots, TEAM_CT), now
    );
}

static int load_game_resources(
    lite_arena_t *map_arena,
    lite_arena_t *texture_arena,
    lite_arena_t *model_arena,
    size_t *persistent_models,
    uint32_t *weapon,
    uint8_t team,
    uint32_t map_id
)
{
    lite_arena_reset(map_arena);
    lite_arena_reset(texture_arena);
    lite_arena_reset(model_arena);
    /*
     * The historical splash occupied the texture arena only while the
     * front-end menus were active. Map textures now reuse that memory;
     * clear the pointer so the in-game buy menu cannot sample map data
     * as if it were a 320x240 background image.
     */
    g_menu_background = 0;
    if (!c15_map_load(
            &g_map, &g_pak, g_map_assets[map_id],
            map_arena, texture_arena,
            g_load_scratch, sizeof(g_load_scratch))) {
        return 0;
    }
    if (!c15_model_load(
            &g_t_model, &g_pak, "mdl/player_terror",
            model_arena, g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_load(
            &g_ct_model, &g_pak, "mdl/player_urban",
            model_arena, g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_load(
            &g_t_weapon_model, &g_pak, "mdl/p_ak47",
            model_arena, g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_load(
            &g_ct_weapon_model, &g_pak, "mdl/p_m4a1",
            model_arena, g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_locomotion_open(
            &g_t_locomotion, &g_t_model, &g_pak,
            "anim/player_terror",
            g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_locomotion_open(
            &g_ct_locomotion, &g_ct_model, &g_pak,
            "anim/player_urban",
            g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_locomotion_open(
            &g_t_weapon_locomotion, &g_t_weapon_model, &g_pak,
            "anim/p_ak47",
            g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_locomotion_open(
            &g_ct_weapon_locomotion, &g_ct_weapon_model, &g_pak,
            "anim/p_m4a1",
            g_load_scratch, sizeof(g_load_scratch))) {
        return 0;
    }
    *persistent_models = model_arena->used;
    change_weapon(
        model_arena, *persistent_models, weapon,
        team == TEAM_T ? WEAPON_GLOCK : WEAPON_USP
    );
    return g_view_model.loaded && g_muzzle.loaded;
}

static void initialize_round(
    c15_player_t *player,
    c15_bot_t bots[BOT_COUNT],
    uint8_t player_team,
    uint8_t difficulty,
    uint32_t map_id,
    uint32_t now,
    uint16_t *player_health
)
{
    c15_camera_t spawn;
    uint8_t enemy_team = player_team == TEAM_T ? TEAM_CT : TEAM_T;
    uint32_t route_count = map_route_count(map_id);
    uint32_t index;
    if (!find_team_spawn(&g_map, player_team, 0u, &spawn)) {
        spawn = g_map.spawn;
    }
    c15_player_spawn(player, &spawn);
    *player_health = 100u;
    for (index = 0u; index < BOT_COUNT; ++index) {
        uint8_t team = index < BOT_TEAMMATES ?
            player_team : enemy_team;
        uint32_t ordinal = index < BOT_TEAMMATES ?
            index + 1u : index - BOT_TEAMMATES;
        if (!find_team_spawn(&g_map, team, ordinal, &spawn)) {
            spawn = g_map.spawn;
        }
        bda_memset(&bots[index], 0, sizeof(bots[index]));
        c15_player_spawn(&bots[index].mover, &spawn);
        bots[index].team = team;
        bots[index].weapon = team == TEAM_T ?
            WEAPON_AK47 : WEAPON_M4A1;
        bots[index].health = 100u;
        bots[index].aim_seed = (uint16_t)(
            (now >> 3) ^ (index * 977u + 0x5a3du)
        );
        bots[index].alive = 1u;
        bots[index].nav_index = team == TEAM_CT ?
            0u : (uint8_t)(route_count - 1u);
        bots[index].next_fire =
            now + g_bot_reaction_ms[difficulty] + index * 53u;
        bots[index].next_decision =
            now + 300u + index * 71u;
        bots[index].strafe_right = (uint8_t)(index & 1u);
    }
}

static int clear_line(
    const c15_map_t *map,
    int32_t ax, int32_t ay, int32_t az,
    int32_t bx, int32_t by, int32_t bz
)
{
    uint32_t span = abs_i32(bx - ax);
    uint32_t segments;
    uint32_t step;
    if (abs_i32(by - ay) > span) span = abs_i32(by - ay);
    if (abs_i32(bz - az) > span) span = abs_i32(bz - az);
    segments = span / 32u + 1u;
    if (segments < 2u) segments = 2u;
    if (segments > 32u) segments = 32u;
    for (step = 1u; step < segments; ++step) {
        int32_t x = ax + (bx - ax) * (int32_t)step /
            (int32_t)segments;
        int32_t y = ay + (by - ay) * (int32_t)step /
            (int32_t)segments;
        int32_t z = az + (bz - az) * (int32_t)step /
            (int32_t)segments;
        int contents = c15_map_hull_contents(map, 0u, x, y, z);
        /*
         * Player-clip brushes (-8) constrain movement but do not block
         * bullets or vision in GoldSrc. Treating them as opaque made bots
         * stand face-to-face at the route midpoint without firing.
         */
        if (contents == -2) {
            return 0;
        }
    }
    return 1;
}

static void bot_logic(
    c15_bot_t bots[BOT_COUNT],
    c15_player_t *player,
    uint8_t player_team,
    uint16_t *player_health,
    uint8_t difficulty,
    uint32_t map_id,
    uint32_t now,
    uint32_t *shots,
    uint32_t *hits,
    uint32_t *sound_weapon
)
{
    uint32_t route_count = map_route_count(map_id);
    uint32_t index;
    for (index = 0u; index < BOT_COUNT; ++index) {
        c15_bot_t *bot = &bots[index];
        uint32_t nearest = 0xffffffffu;
        uint32_t nearest_friend = 0xffffffffu;
        int target = -2;
        int32_t target_x = 0;
        int32_t target_y = 0;
        int32_t target_z = 0;
        int32_t move_x;
        int32_t move_y;
        uint32_t move_distance;
        uint32_t controls = 0u;
        int target_visible;
        int32_t nav_x = 0;
        int32_t nav_y = 0;
        int32_t nav_z = 0;
        int has_nav = 0;
        uint8_t target_yaw;
        uint32_t other;
        uint32_t before_blocked;
        int32_t before_x;
        int32_t before_y;
        if (!bot->alive) {
            bot->moving = 0u;
            continue;
        }
        if (bot->team != player_team && *player_health != 0u) {
            nearest = distance_squared(
                bot->mover.x, bot->mover.y, player->x, player->y
            );
            target = -1;
            target_x = player->x;
            target_y = player->y;
            target_z = player->z;
        }
        for (other = 0u; other < BOT_COUNT; ++other) {
            uint32_t distance;
            if (other == index || !bots[other].alive) {
                continue;
            }
            distance = distance_squared(
                bot->mover.x, bot->mover.y,
                bots[other].mover.x, bots[other].mover.y
            );
            if (bots[other].team == bot->team) {
                if (distance < nearest_friend) {
                    nearest_friend = distance;
                }
                continue;
            }
            if (distance < nearest) {
                nearest = distance;
                target = (int)other;
                target_x = bots[other].mover.x;
                target_y = bots[other].mover.y;
                target_z = bots[other].mover.z;
            }
        }
        if (target == -2) {
            bot->moving = 0u;
            continue;
        }
        target_yaw = yaw_from_delta(
            target_x - bot->mover.x, target_y - bot->mover.y
        );
        target_visible =
            nearest < BOT_SIGHT_DISTANCE * BOT_SIGHT_DISTANCE &&
            abs_i32(bot->mover.z - target_z) < 160u &&
            (nearest < BOT_CLOSE_AWARE_DISTANCE *
                       BOT_CLOSE_AWARE_DISTANCE ||
             yaw_distance(target_yaw, bot->mover.yaw) <= 56u) &&
            clear_line(
                 &g_map,
                 bot->mover.x, bot->mover.y, bot->mover.z + 28,
                 target_x, target_y, target_z + 28
            );
        move_x = target_x;
        move_y = target_y;
        if (!target_visible) {
            uint8_t previous_nav_index = bot->nav_index;
            has_nav = map_route_point(
                map_id, bot->nav_index, &nav_x, &nav_y, &nav_z
            );
            if (has_nav &&
                distance_squared(
                    bot->mover.x, bot->mover.y,
                    nav_x, nav_y) < 32u * 32u &&
                abs_i32(bot->mover.z - nav_z) < 32u) {
                if (bot->team == TEAM_CT &&
                    bot->nav_index + 1u < route_count) {
                    ++bot->nav_index;
                } else if (bot->team == TEAM_T &&
                           bot->nav_index != 0u) {
                    --bot->nav_index;
                }
                has_nav = map_route_point(
                    map_id, bot->nav_index, &nav_x, &nav_y, &nav_z
                );
            }
            if (bot->nav_index != previous_nav_index) {
                bot->nav_stalls = 0u;
            }
            if (has_nav) {
                move_x = nav_x;
                move_y = nav_y;
            }
            bot->mover.yaw = yaw_from_delta(
                move_x - bot->mover.x, move_y - bot->mover.y
            );
            controls = C15_MOVE_FORWARD;
            bot->combat = 0u;
        } else {
            bot->mover.yaw = target_yaw;
            bot->combat = 1u;
            if (time_reached(now, bot->next_decision)) {
                bot->aim_seed = (uint16_t)(
                    bot->aim_seed * 25173u + 13849u
                );
                bot->strafe_right =
                    (uint8_t)((bot->aim_seed >> 14) & 1u);
                bot->next_decision =
                    now + 650u - (uint32_t)difficulty * 150u +
                    (bot->aim_seed & 0x1ffu);
            }
            if (nearest > BOT_HOLD_MAX_DISTANCE *
                          BOT_HOLD_MAX_DISTANCE) {
                controls = C15_MOVE_FORWARD;
            } else if (nearest < BOT_HOLD_MIN_DISTANCE *
                                 BOT_HOLD_MIN_DISTANCE) {
                controls = C15_MOVE_BACK |
                    (bot->strafe_right ?
                        C15_MOVE_RIGHT : C15_MOVE_LEFT);
            } else if ((bot->aim_seed & 3u) != 0u) {
                controls = bot->strafe_right ?
                    C15_MOVE_RIGHT : C15_MOVE_LEFT;
            }
            if (nearest_friend < 72u * 72u) {
                bot->strafe_right ^= 1u;
                controls &= ~(C15_MOVE_LEFT | C15_MOVE_RIGHT);
                controls |= bot->strafe_right ?
                    C15_MOVE_RIGHT : C15_MOVE_LEFT;
            }
        }
        if (bot->stuck_turn != 0u && !target_visible) {
            bot->mover.yaw = (uint8_t)(
                bot->mover.yaw + 32u + index * 7u
            );
            --bot->stuck_turn;
        } else if (bot->stuck_turn != 0u) {
            controls &= ~(C15_MOVE_LEFT | C15_MOVE_RIGHT);
            controls |= bot->strafe_right ?
                C15_MOVE_RIGHT : C15_MOVE_LEFT;
            --bot->stuck_turn;
        }
        bot->mover.yaw_q8 =
            (uint16_t)((uint16_t)bot->mover.yaw << 8);
        before_blocked = bot->mover.blocked_steps;
        before_x = bot->mover.x;
        before_y = bot->mover.y;
        move_distance = distance_squared(
            bot->mover.x, bot->mover.y, move_x, move_y
        );
        if (target_visible || move_distance > 12u * 12u) {
            c15_player_step_speed(
                &bot->mover, &g_map, controls,
                target_visible ?
                    g_bot_combat_speed[difficulty] : BOT_MOVE_PER_TICK
            );
        } else {
            c15_player_step(&bot->mover, &g_map, 0u);
        }
        bot->moving = (uint8_t)(
            bot->mover.x != before_x || bot->mover.y != before_y
        );
        if (bot->mover.blocked_steps != before_blocked) {
            bot->stuck_turn = 8u;
            bot->strafe_right ^= 1u;
            ++bot->nav_stalls;
            if (bot->nav_stalls >= 6u && has_nav) {
                /*
                 * Recover to the adjacent hull-safe node instead of
                 * skipping nodes. This corrects small step-height drift
                 * without copying a nav mesh or crossing a long segment.
                 */
                bot->mover.x = nav_x;
                bot->mover.y = nav_y;
                bot->mover.z = nav_z;
                bot->mover.z_q8 = bot->mover.z << 8;
                bot->mover.velocity_z_q8 = 0;
                bot->mover.grounded = 1u;
                bot->stuck_turn = 0u;
                bot->nav_stalls = 0u;
            }
        }
        if (target_visible && !bot->target_visible_last) {
            bot->next_fire =
                now + g_bot_reaction_ms[difficulty] + index * 53u;
            bot->burst_left = 0u;
        }
        bot->target_visible_last = (uint8_t)target_visible;
        if (target_visible &&
            time_reached(now, bot->next_fire) &&
            nearest < BOT_SIGHT_DISTANCE * BOT_SIGHT_DISTANCE) {
            uint16_t *health = target < 0 ?
                player_health : &bots[(uint32_t)target].health;
            bot->aim_seed = (uint16_t)(
                bot->aim_seed * 25173u + 13849u
            );
            if (bot->burst_left == 0u) {
                bot->burst_left = (uint8_t)(
                    2u + ((bot->aim_seed >> 13) & 1u)
                );
            }
            ++*shots;
            *sound_weapon = bot->weapon;
            --bot->burst_left;
            if (bot->burst_left != 0u) {
                bot->next_fire =
                    now + g_bot_burst_interval_ms[difficulty] +
                    index * 7u;
            } else {
                bot->next_fire =
                    now + g_bot_burst_pause_ms[difficulty] +
                    (bot->aim_seed & 0x1ffu);
            }
            if (bot->aim_seed < (
                    nearest < 320u * 320u ?
                        g_bot_accuracy[difficulty][0] :
                    nearest < 800u * 800u ?
                        g_bot_accuracy[difficulty][1] :
                        g_bot_accuracy[difficulty][2])) {
                ++*hits;
                if (*health <= g_bot_damage[difficulty]) {
                    *health = 0u;
                    if (target >= 0) {
                        bots[(uint32_t)target].alive = 0u;
                    }
                } else {
                    *health = (uint16_t)(
                        *health - g_bot_damage[difficulty]
                    );
                }
            }
        }
    }
}

static int player_fire_hit(
    c15_bot_t bots[BOT_COUNT],
    const c15_camera_t *camera,
    uint8_t player_team,
    uint32_t weapon,
    uint32_t *money
)
{
    int32_t yaw_sine = sin_q14_q8(camera->yaw_q8);
    int32_t yaw_cosine = cos_q14_q8(camera->yaw_q8);
    int32_t pitch_sine = sin_q14_q8(
        (uint16_t)(int16_t)camera->pitch_q8
    );
    int32_t pitch_cosine = cos_q14_q8(
        (uint16_t)(int16_t)camera->pitch_q8
    );
    uint32_t nearest = 0xffffffffu;
    int selected = -1;
    uint32_t index;
    for (index = 0u; index < BOT_COUNT; ++index) {
        const c15_model_t *body;
        int32_t dx;
        int32_t dy;
        int32_t side;
        int32_t forward;
        int32_t radius_q4;
        int32_t radius;
        int32_t minimum_z;
        int32_t maximum_z;
        int32_t minimum_view_y;
        int32_t maximum_view_y;
        int32_t hit_x;
        int32_t hit_y;
        int32_t hit_z;
        uint32_t distance;
        if (!bots[index].alive || bots[index].team == player_team) {
            continue;
        }
        body = bots[index].team == TEAM_T ?
            &g_t_model : &g_ct_model;
        dx = bots[index].mover.x - camera->x;
        dy = bots[index].mover.y - camera->y;
        side = (int32_t)(
            ((int64_t)yaw_sine * dx -
             (int64_t)yaw_cosine * dy) >> 14
        );
        forward = (int32_t)(
            ((int64_t)yaw_cosine * dx +
             (int64_t)yaw_sine * dy) >> 14
        );
        if (forward <= 0 || !body->loaded || !body->chunk) {
            continue;
        }
        radius_q4 = abs_i32(read_i16(body->chunk + 44u));
        if ((int32_t)abs_i32(read_i16(body->chunk + 46u)) >
            radius_q4) {
            radius_q4 = abs_i32(read_i16(body->chunk + 46u));
        }
        if ((int32_t)abs_i32(read_i16(body->chunk + 50u)) >
            radius_q4) {
            radius_q4 = abs_i32(read_i16(body->chunk + 50u));
        }
        if ((int32_t)abs_i32(read_i16(body->chunk + 52u)) >
            radius_q4) {
            radius_q4 = abs_i32(read_i16(body->chunk + 52u));
        }
        radius = (radius_q4 + 15) >> 4;
        if ((int32_t)abs_i32(side) > radius) {
            continue;
        }
        minimum_z = bots[index].mover.z +
            (read_i16(body->chunk + 48u) >> 4);
        maximum_z = bots[index].mover.z +
            ((read_i16(body->chunk + 54u) + 15) >> 4);
        minimum_view_y = (int32_t)(
            ((int64_t)pitch_cosine * (minimum_z - camera->z) -
             (int64_t)pitch_sine * forward) >> 14
        );
        maximum_view_y = (int32_t)(
            ((int64_t)pitch_cosine * (maximum_z - camera->z) -
             (int64_t)pitch_sine * forward) >> 14
        );
        if (minimum_view_y > 0 || maximum_view_y < 0) {
            continue;
        }
        distance = distance_squared(
            camera->x, camera->y,
            bots[index].mover.x, bots[index].mover.y
        );
        hit_x = camera->x +
            (int32_t)(((int64_t)yaw_cosine * forward) >> 14);
        hit_y = camera->y +
            (int32_t)(((int64_t)yaw_sine * forward) >> 14);
        hit_z = camera->z + (int32_t)(
            ((int64_t)pitch_sine * forward) / pitch_cosine
        );
        if (hit_z < minimum_z) hit_z = minimum_z;
        if (hit_z > maximum_z) hit_z = maximum_z;
        if ((weapon == WEAPON_KNIFE && distance > 90u * 90u) ||
            distance >= nearest ||
            !clear_line(
                &g_map,
                camera->x, camera->y, camera->z,
                hit_x, hit_y, hit_z)) {
            continue;
        }
        nearest = distance;
        selected = (int)index;
    }
    if (selected < 0) {
        return 0;
    }
    if (bots[(uint32_t)selected].health <= g_weapon_damage[weapon]) {
        bots[(uint32_t)selected].health = 0u;
        bots[(uint32_t)selected].alive = 0u;
        *money += 300u;
        if (*money > 16000u) {
            *money = 16000u;
        }
    } else {
        bots[(uint32_t)selected].health = (uint16_t)(
            bots[(uint32_t)selected].health -
            g_weapon_damage[weapon]
        );
    }
    return 1;
}

static void team_counts(
    const c15_bot_t bots[BOT_COUNT],
    uint8_t player_team,
    uint16_t player_health,
    uint32_t *t_alive,
    uint32_t *ct_alive
)
{
    uint32_t index;
    *t_alive = 0u;
    *ct_alive = 0u;
    if (player_health != 0u) {
        if (player_team == TEAM_T) ++*t_alive;
        else ++*ct_alive;
    }
    for (index = 0u; index < BOT_COUNT; ++index) {
        if (!bots[index].alive) continue;
        if (bots[index].team == TEAM_T) ++*t_alive;
        else ++*ct_alive;
    }
}

static void draw_fire_feedback(
    lite_framebuffer_t *fb,
    uint32_t weapon,
    uint32_t sprite_frame,
    uint32_t anchor_frame,
    int recoil,
    int bob_q4,
    uint16_t hot
)
{
    const c15_model_view_t *view;
    const uint8_t *anchor;
    const uint8_t *pixels;
    uint32_t frame_bytes;
    int32_t x_q4;
    int32_t y_q4;
    int32_t z_q4;
    int center_x;
    int center_y;
    int size;
    int top;
    int left;
    int y;
    if (weapon == WEAPON_KNIFE) {
        lite_fb_line(fb, 148, 108, 156, 116, hot);
        lite_fb_line(fb, 172, 108, 164, 116, hot);
        lite_fb_line(fb, 148, 132, 156, 124, hot);
        lite_fb_line(fb, 172, 132, 164, 124, hot);
        return;
    }
    if (!g_muzzle.loaded) {
        return;
    }
    view = &g_weapon_views[weapon];
    if (anchor_frame >= g_muzzle.anchor_count) {
        anchor_frame = g_muzzle.anchor_count - 1u;
    }
    anchor = g_muzzle.anchors + anchor_frame * 6u;
    x_q4 = -read_i16(anchor + 2);
    y_q4 = read_i16(anchor + 4) - 16 + bob_q4;
    z_q4 = read_i16(anchor) + view->depth_bias_q4 +
        (bob_q4 * 2) / 5;
    if (z_q4 < view->near_depth_q4) {
        return;
    }
    center_x = view->center_x +
        (x_q4 * view->focal_length) / z_q4;
    center_y = view->origin_y - recoil -
        (y_q4 * view->focal_length) / z_q4;
    size = g_muzzle.display_size;
    left = center_x - size / 2;
    top = center_y - size / 2;
    sprite_frame %= g_muzzle.frame_count;
    frame_bytes = (
        (uint32_t)g_muzzle.width * g_muzzle.height + 1u
    ) / 2u;
    pixels = g_muzzle.pixels + sprite_frame * frame_bytes;
    for (y = 0; y < size; ++y) {
        int screen_y = top + y;
        int source_y;
        int x;
        if (screen_y < 0 || screen_y >= fb->height) {
            continue;
        }
        source_y = y * g_muzzle.height / size;
        for (x = 0; x < size; ++x) {
            int screen_x = left + x;
            uint32_t source_index;
            uint8_t palette_index;
            uint16_t source;
            uint16_t *destination;
            uint32_t red;
            uint32_t green;
            uint32_t blue;
            if (screen_x < 0 || screen_x >= fb->width) {
                continue;
            }
            source_index = (uint32_t)source_y * g_muzzle.width +
                (uint32_t)(x * g_muzzle.width / size);
            palette_index = (uint8_t)(
                (pixels[source_index >> 1] >>
                 (4u * (source_index & 1u))) & 15u
            );
            if (palette_index == 0u) {
                continue;
            }
            source = read_u16(g_muzzle.chunk + 12u +
                              (uint32_t)palette_index * 2u);
            destination = &fb->pixels[
                screen_y * fb->stride + screen_x
            ];
            red = ((*destination >> 11) & 31u) +
                ((source >> 11) & 31u);
            green = ((*destination >> 5) & 63u) +
                ((source >> 5) & 63u);
            blue = (*destination & 31u) + (source & 31u);
            if (red > 31u) red = 31u;
            if (green > 63u) green = 63u;
            if (blue > 31u) blue = 31u;
            *destination = (uint16_t)(
                (red << 11) | (green << 5) | blue
            );
        }
    }
}

static int view_bob_q4(const lite_input_t *input, uint32_t now)
{
    int32_t direction = 0;
    int32_t strafe = 0;
    int32_t amplitude_q4;
    int32_t factor_q14;
    int32_t bob_q4;
    uint8_t phase;
    if ((input->down & LITE_INPUT_UP) != 0u) ++direction;
    if ((input->down & LITE_INPUT_DOWN) != 0u) --direction;
    if ((input->down & LITE_INPUT_LEFT) != 0u) --strafe;
    if ((input->down & LITE_INPUT_RIGHT) != 0u) ++strafe;
    if (direction == 0 && strafe == 0) {
        return 0;
    }
    /*
     * GoldSrc defaults: cl_bobcycle=.8, cl_bob=.01,
     * cl_bobup=.5. Our player covers 8 units per 40 ms tick, or
     * 200 units/s, so speed * cl_bob is 2 units (32 in Q4).
     */
    amplitude_q4 = direction != 0 && strafe != 0 ? 45 : 32;
    phase = (uint8_t)(((now % 800u) * 256u) / 800u);
    factor_q14 = 4915 + ((int32_t)c15_sin_q14(phase) * 7) / 10;
    bob_q4 = (amplitude_q4 * factor_q14) >> 14;
    if (bob_q4 > 64) bob_q4 = 64;
    if (bob_q4 < -112) bob_q4 = -112;
    return bob_q4;
}

static void render_game(
    lite_framebuffer_t *fb,
    lite_framebuffer_t *view,
    const lite_input_t *input,
    uint32_t now,
    const c15_camera_t *camera,
    const c15_player_t *player,
    const c15_bot_t bots[BOT_COUNT],
    uint8_t player_team,
    uint16_t player_health,
    uint32_t money,
    uint32_t round,
    uint32_t fps_x10,
    uint32_t map_id,
    uint32_t weapon,
    int firing,
    int hit_confirmed,
    uint32_t muzzle_frame,
    uint32_t fire_pose_frame,
    uint32_t ammo,
    enum game_screen screen,
    uint8_t round_winner,
    c15_render_stats_t *stats,
    c15_model_render_stats_t *view_stats,
    c15_model_render_stats_t *entity_stats
)
{
    uint16_t white = lite_rgb565(238u, 244u, 239u);
    uint16_t green = lite_rgb565(82u, 203u, 122u);
    uint16_t accent = lite_rgb565(244u, 181u, 46u);
    uint16_t red = lite_rgb565(235u, 78u, 64u);
    uint32_t t_alive;
    uint32_t ct_alive;
    uint32_t index;
    int bob_q4 = view_bob_q4(input, now);
    (void)player;
    c15_render_world(
        &g_map, camera, view, g_depth, g_visible_surfaces,
        sizeof(g_visible_surfaces), stats
    );
    bda_memset(entity_stats, 0, sizeof(*entity_stats));
    for (index = 0u; index < BOT_COUNT; ++index) {
        const c15_model_t *body;
        const c15_model_t *held;
        uint32_t distance;
        if (!bots[index].alive) {
            continue;
        }
        distance = distance_squared(
            camera->x, camera->y,
            bots[index].mover.x, bots[index].mover.y
        );
        if (distance > 1800u * 1800u) {
            continue;
        }
        body = bots[index].team == TEAM_T ?
            &g_t_model : &g_ct_model;
        held = bots[index].team == TEAM_T ?
            &g_t_weapon_model : &g_ct_weapon_model;
        c15_render_world_model(
            body, view, g_depth, camera,
            bots[index].mover.x, bots[index].mover.y,
            bots[index].mover.z,
            (uint8_t)(bots[index].mover.yaw - 64u),
            entity_stats
        );
        c15_render_world_model(
            held, view, g_depth, camera,
            bots[index].mover.x, bots[index].mover.y,
            bots[index].mover.z,
            (uint8_t)(bots[index].mover.yaw - 64u),
            entity_stats
        );
    }
    c15_render_view_model(
        &g_view_model, view, g_depth,
        &g_weapon_views[weapon],
        firing ? 6 : 0, bob_q4, view_stats
    );
    lite_fb_line(
        fb, 152, 120, 168, 120,
        hit_confirmed ? red : accent
    );
    lite_fb_line(
        fb, 160, 112, 160, 128,
        hit_confirmed ? red : accent
    );
    draw_fps(fb, fps_x10, green, accent);
    lite_fb_text(fb, 4, 15, g_map_labels[map_id], 1, white);
    lite_fb_text(fb, 232, 4, g_weapon_labels[weapon], 1, accent);
    if (weapon != WEAPON_KNIFE) {
        lite_fb_text(fb, 232, 15, "AMMO", 1, white);
        lite_fb_u32(fb, 262, 15, ammo, 1, accent);
    }
    team_counts(
        bots, player_team, player_health, &t_alive, &ct_alive
    );
    lite_fb_text(fb, 119, 4, "T", 1, red);
    lite_fb_u32(fb, 131, 4, t_alive, 1, white);
    lite_fb_text(fb, 149, 4, "CT", 1, green);
    lite_fb_u32(fb, 167, 4, ct_alive, 1, white);
    lite_fb_text(fb, 188, 4, "R", 1, accent);
    lite_fb_u32(fb, 200, 4, round, 1, white);
    lite_fb_blend_rect(fb, 2, 218, 155, 20, lite_rgb565(18u, 31u, 31u));
    lite_fb_text(fb, 7, 224, "HP", 1, green);
    lite_fb_u32(
        fb, 25, 224, player_health, 1,
        player_health < 30u ? red : white
    );
    lite_fb_text(fb, 63, 224, "MONEY", 1, green);
    lite_fb_u32(fb, 99, 224, money, 1, accent);
    if (firing) {
        draw_fire_feedback(
            fb, weapon, muzzle_frame, fire_pose_frame,
            6, bob_q4, accent
        );
    }
    draw_controls(fb, input);
    if (player_health == 0u) {
        lite_fb_blend_rect(fb, 72, 92, 176, 44, lite_rgb565(52u, 15u, 15u));
        lite_fb_frame(fb, 72, 92, 176, 44, red);
        lite_fb_text(fb, 102, 108, "YOU ARE DEAD", 1, white);
    }
    if (screen == SCREEN_ROUND_END) {
        lite_fb_blend_rect(fb, 55, 80, 210, 64, lite_rgb565(17u, 24u, 21u));
        lite_fb_frame(fb, 55, 80, 210, 64, accent);
        lite_fb_text(
            fb, 85, 94,
            round_winner == TEAM_T ?
                "TERRORISTS WIN" : "COUNTER-TERRORISTS WIN",
            1, accent
        );
        lite_fb_text(fb, 98, 119, "NEXT ROUND", 1, white);
    }
}

static uint32_t menu_touch_item(
    enum game_screen screen, int x, int y
)
{
    if (x < 20 || x > 230) return 0xffffffffu;
    if (screen == SCREEN_MAIN) {
        if (y >= 84 && y < 122) return 0u;
        if (y >= 122 && y < 160) return 1u;
        if (y >= 160 && y < 205) return 2u;
    } else if (screen == SCREEN_OPTIONS) {
        if (y >= 84 && y < 122) return 0u;
        if (y >= 122 && y < 160) return 1u;
        if (y >= 166 && y < 207) return 2u;
    } else if (screen == SCREEN_MAP) {
        if (y >= 80 && y < 114) return 0u;
        if (y >= 112 && y < 146) return 1u;
        if (y >= 144 && y < 179) return 2u;
        if (y >= 184 && y < 218) return 3u;
    } else if (screen == SCREEN_TEAM) {
        if (y >= 78 && y < 120) return 0u;
        if (y >= 118 && y < 160) return 1u;
        if (y >= 158 && y < 191) return 2u;
        if (y >= 190 && y < 225) return 3u;
    } else if (screen == SCREEN_BUY) {
        if (y >= 90 && y < 130) return 0u;
        if (y >= 130 && y < 170) return 1u;
        if (y >= 174 && y < 220) return 2u;
    }
    return 0xffffffffu;
}

static void log_touch_gesture(const lite_touch_debug_t *debug)
{
    lite_log_line("touch_diag_begin");
    lite_log_u32("touch_gesture", debug->gesture_id);
    lite_log_u32("touch_duration_ms", debug->duration_ms);
    lite_log_u32("touch_move_events", debug->move_events);
    lite_log_u32("touch_application_pumps", debug->application_pumps);
    lite_log_i32("touch_net_dx", debug->net_dx);
    lite_log_i32("touch_net_dy", debug->net_dy);
    lite_log_u32("touch_raw_event_cap_hits", debug->raw_event_cap_hits);
    lite_log_line("touch_diag_end");
}

__attribute__((section(".text.bda_main")))
int bda_main(void)
{
    lite_framebuffer_t framebuffer;
    lite_framebuffer_t view;
    lite_arena_t map_arena;
    lite_arena_t texture_arena;
    lite_arena_t model_arena;
    lite_input_t input;
    c15_camera_t camera;
    c15_player_t player;
    c15_bot_t bots[BOT_COUNT];
    c15_render_stats_t stats = {0};
    c15_model_render_stats_t view_stats = {0};
    c15_model_render_stats_t entity_stats = {0};
    c15_audio_t audio;
    c15_view_animation_state_t view_animation = {
        C15_VIEW_ANIMATION_IDLE, 0u, 0u
    };
    c15_world_animation_state_t t_animation = {0xffu, 0xffu};
    c15_world_animation_state_t ct_animation = {0xffu, 0xffu};
    enum game_screen screen = SCREEN_MAIN;
    uint32_t selection = 0u;
    uint32_t menu_count = 3u;
    uint32_t next_frame;
    uint32_t next_logic = 0u;
    uint32_t next_metric;
    uint32_t fps_last_frame;
    uint32_t frame = 0u;
    uint32_t fps_x10 = 0u;
    uint32_t pending_controls = 0u;
    uint32_t map_id = MAP_DE_DUST;
    uint32_t weapon = WEAPON_GLOCK;
    uint32_t fire_until = 0u;
    uint32_t hit_until = 0u;
    uint32_t fire_started = 0u;
    uint32_t next_fire = 0u;
    uint32_t shots_fired = 0u;
    uint32_t shots_hit = 0u;
    uint32_t bot_shots = 0u;
    uint32_t bot_hits = 0u;
    uint32_t bot_sound_weapon = WEAPON_COUNT;
    uint32_t money = 4000u;
    uint32_t round = 1u;
    uint32_t round_end_at = 0u;
    uint32_t round_deadline = 0u;
    uint32_t rounds_t = 0u;
    uint32_t rounds_ct = 0u;
    uint32_t reload_weapon = WEAPON_GLOCK;
    uint16_t weapon_ammo[WEAPON_COUNT] = {0u, 20u, 30u, 30u, 12u};
    uint8_t owned[WEAPON_COUNT] = {1u, 0u, 0u, 0u, 0u};
    uint8_t player_team = TEAM_T;
    uint8_t bot_difficulty = BOT_EASY;
    uint8_t audio_enabled = 1u;
    uint16_t player_health = 100u;
    uint8_t round_winner = 0u;
    int previous_touch_down = 0;
    int game_loaded = 0;
    int reloading = 0;
    int running = 1;
    size_t persistent_models = 0u;

    bda_memset(&input, 0, sizeof(input));
    bda_memset(&audio, 0, sizeof(audio));
    bda_memset(&player, 0, sizeof(player));
    bda_memset(bots, 0, sizeof(bots));
    lite_log_reset();
    lite_log_line("CS15 Lite M11A start");
    lite_log_line("game_flow=main-map-team-buy-round");
    lite_log_line("bot_ai=range-strafe-burst");
    lite_log_line("bot_animation=goldsrc-hybrid-walk");
    lite_log_line("muzzle_flash=historical_additive_sprite");
    lite_log_line("audio=historical_pcm-stream");
    lite_arena_init(&map_arena, g_map_memory, sizeof(g_map_memory));
    lite_arena_init(
        &texture_arena, g_texture_memory, sizeof(g_texture_memory)
    );
    lite_arena_init(&model_arena, g_model_memory, sizeof(g_model_memory));
    framebuffer.pixels = g_screen;
    framebuffer.width = (int)LITE_SCREEN_WIDTH;
    framebuffer.height = (int)LITE_SCREEN_HEIGHT;
    framebuffer.stride = (int)LITE_SCREEN_WIDTH;
    view = framebuffer;

    if (!lite_platform_open()) {
        lite_log_line("platform open failed");
        lite_log_close();
        return 2;
    }
    if (!c15_pak_open(&g_pak, ASSET_PATH)) {
        draw_loading(
            &framebuffer, "CS15.C15PAK NOT FOUND",
            lite_rgb565(235u, 78u, 64u)
        );
        (void)lite_platform_present(g_screen);
        while (lite_platform_pump(&input)) {
            lite_platform_delay(1u);
        }
        lite_platform_close();
        lite_log_close();
        return 3;
    }
    if (!c15_audio_init(
            &audio, &g_pak, g_load_scratch,
            sizeof(g_load_scratch))) {
        lite_log_line("sound bank unavailable");
    }
    c15_audio_set_enabled(
        &audio, audio_enabled,
        g_load_scratch, sizeof(g_load_scratch)
    );
    if (!load_menu_background(&texture_arena)) {
        lite_log_line("historical menu splash unavailable");
    }
    next_frame = lite_platform_milliseconds();
    next_metric = next_frame + METRIC_INTERVAL_MS;
    fps_last_frame = next_frame;

    while (running && lite_platform_pump(&input)) {
        uint32_t now = lite_platform_milliseconds();
        int touch_new = input.touch_down && !previous_touch_down;
        uint32_t activated = 0xffffffffu;
        lite_touch_debug_t touch_debug;
        (void)c15_audio_service(
            &audio, &g_pak, g_load_scratch,
            sizeof(g_load_scratch)
        );
        previous_touch_down = input.touch_down;
        if (lite_platform_touch_debug_take(&touch_debug)) {
            log_touch_gesture(&touch_debug);
        }

        if (screen == SCREEN_MAIN || screen == SCREEN_OPTIONS ||
            screen == SCREEN_MAP ||
            screen == SCREEN_TEAM || screen == SCREEN_BUY) {
            if ((input.pressed & LITE_INPUT_UP) != 0u) {
                selection = selection == 0u ?
                    menu_count - 1u : selection - 1u;
            }
            if ((input.pressed & LITE_INPUT_DOWN) != 0u) {
                selection = (selection + 1u) % menu_count;
            }
            if (touch_new) {
                uint32_t item = menu_touch_item(
                    screen, input.touch_x, input.touch_y
                );
                if (item != 0xffffffffu) {
                    selection = item;
                    activated = item;
                }
            }
            if ((input.pressed & LITE_INPUT_FIRE) != 0u) {
                activated = selection;
            }
            if (activated != 0xffffffffu) {
                if (screen == SCREEN_MAIN) {
                    if (activated == 0u) {
                        screen = SCREEN_MAP;
                        selection = 0u;
                        menu_count = MAP_COUNT + 1u;
                    } else if (activated == 1u) {
                        screen = SCREEN_OPTIONS;
                        selection = 0u;
                        menu_count = 3u;
                    } else if (activated == 2u) {
                        running = 0;
                    }
                } else if (screen == SCREEN_OPTIONS) {
                    if (activated == 0u) {
                        bot_difficulty = (uint8_t)(
                            (bot_difficulty + 1u) %
                            BOT_DIFFICULTY_COUNT
                        );
                    } else if (activated == 1u) {
                        audio_enabled ^= 1u;
                        c15_audio_set_enabled(
                            &audio, audio_enabled,
                            g_load_scratch, sizeof(g_load_scratch)
                        );
                    } else {
                        screen = SCREEN_MAIN;
                        selection = 1u;
                        menu_count = 3u;
                    }
                } else if (screen == SCREEN_MAP) {
                    if (activated < MAP_COUNT) {
                        map_id = activated;
                        screen = SCREEN_TEAM;
                        selection = 0u;
                        menu_count = 4u;
                    } else {
                        screen = SCREEN_MAIN;
                        selection = 0u;
                        menu_count = 3u;
                    }
                } else if (screen == SCREEN_TEAM) {
                    if (activated == 3u) {
                        screen = SCREEN_MAP;
                        selection = 0u;
                        menu_count = MAP_COUNT + 1u;
                    } else {
                        player_team = activated == 0u ?
                            TEAM_T : (activated == 1u ? TEAM_CT :
                            (uint8_t)(now & 1u ? TEAM_T : TEAM_CT));
                        draw_loading(
                            &framebuffer, g_map_loading[map_id],
                            lite_rgb565(224u, 168u, 54u)
                        );
                        (void)lite_platform_present(g_screen);
                        if (!resource_pack_is_current(map_id)) {
                            draw_loading(
                                &framebuffer, "RESOURCE PACK OUTDATED",
                                lite_rgb565(235u, 78u, 64u)
                            );
                            lite_fb_text(
                                &framebuffer, 38, 151,
                                "COPY MATCHING CS15.C15PAK", 1,
                                lite_rgb565(225u, 229u, 210u)
                            );
                            (void)lite_platform_present(g_screen);
                            lite_log_line("resource pack preflight failed");
                            while (lite_platform_pump(&input)) {
                                lite_platform_delay(1u);
                            }
                            running = 0;
                            continue;
                        }
                        weapon = player_team == TEAM_T ?
                            WEAPON_GLOCK : WEAPON_USP;
                        if (!load_game_resources(
                                &map_arena, &texture_arena, &model_arena,
                                &persistent_models, &weapon,
                                player_team, map_id)) {
                            draw_loading(
                                &framebuffer, "RESOURCE LOAD FAILED",
                                lite_rgb565(235u, 78u, 64u)
                            );
                            (void)lite_platform_present(g_screen);
                            lite_log_line("resource load failed");
                            while (lite_platform_pump(&input)) {
                                lite_platform_delay(1u);
                            }
                            running = 0;
                            continue;
                        }
                        owned[WEAPON_GLOCK] =
                            player_team == TEAM_T;
                        owned[WEAPON_USP] =
                            player_team == TEAM_CT;
                        initialize_round(
                            &player, bots, player_team,
                            bot_difficulty, map_id, now,
                            &player_health
                        );
                        t_animation.action = 0xffu;
                        ct_animation.action = 0xffu;
                        update_world_animations(
                            &t_animation, &ct_animation, bots, now
                        );
                        c15_player_camera(&player, &camera);
                        weapon_ammo[weapon] =
                            g_weapon_capacity[weapon];
                        reloading = 0;
                        start_view_animation(
                            &view_animation,
                            C15_VIEW_ANIMATION_DRAW, now
                        );
                        game_loaded = 1;
                        screen = SCREEN_BUY;
                        selection = 0u;
                        menu_count = 3u;
                        next_logic = now;
                        lite_log_u32("pak_bytes", g_pak.file_size);
                        lite_log_u32("map_id", map_id);
                        lite_log_u32(
                            "map_source_crc32", g_map.source_crc32
                        );
                        lite_log_u32(
                            "map_peak_bytes", (uint32_t)map_arena.peak
                        );
                        lite_log_u32(
                            "texture_peak_bytes",
                            (uint32_t)texture_arena.peak
                        );
                        lite_log_u32(
                            "persistent_model_bytes",
                            (uint32_t)persistent_models
                        );
                    }
                } else if (screen == SCREEN_BUY) {
                    uint32_t pistol = player_team == TEAM_T ?
                        WEAPON_GLOCK : WEAPON_USP;
                    uint32_t rifle = player_team == TEAM_T ?
                        WEAPON_AK47 : WEAPON_M4A1;
                    uint32_t price = player_team == TEAM_T ? 2500u : 3100u;
                    if (activated == 0u) {
                        owned[pistol] = 1u;
                        change_weapon(
                            &model_arena, persistent_models,
                            &weapon, pistol
                        );
                        reloading = 0;
                        start_view_animation(
                            &view_animation,
                            C15_VIEW_ANIMATION_DRAW, now
                        );
                        weapon_ammo[pistol] = g_weapon_capacity[pistol];
                    } else if (activated == 1u) {
                        if (!owned[rifle] && money >= price) {
                            money -= price;
                            owned[rifle] = 1u;
                        }
                        if (owned[rifle]) {
                            change_weapon(
                                &model_arena, persistent_models,
                                &weapon, rifle
                            );
                            reloading = 0;
                            start_view_animation(
                                &view_animation,
                                C15_VIEW_ANIMATION_DRAW, now
                            );
                            weapon_ammo[rifle] =
                                g_weapon_capacity[rifle];
                        }
                    } else {
                        screen = SCREEN_PLAY;
                        selection = 0u;
                        next_logic = now;
                        if (round_deadline == 0u) {
                            round_deadline = now + ROUND_TIME_MS;
                        }
                    }
                }
            }
        } else if (game_loaded) {
            uint32_t logic_steps = 0u;
            int completed_animation = update_view_animation(
                &view_animation, now
            );
            if (completed_animation == C15_VIEW_ANIMATION_RELOAD &&
                reloading) {
                weapon_ammo[reload_weapon] =
                    g_weapon_capacity[reload_weapon];
                reloading = 0;
            }
            if (input.touch_dx != 0 || input.touch_dy != 0) {
                c15_player_look(
                    &player, input.touch_dx, input.touch_dy
                );
                c15_player_camera(&player, &camera);
            }
            if (screen == SCREEN_PLAY &&
                (input.pressed & LITE_INPUT_USE) != 0u &&
                player_health != 0u) {
                screen = SCREEN_BUY;
                selection = 0u;
                menu_count = 3u;
            }
            if (screen == SCREEN_PLAY &&
                (input.pressed & LITE_INPUT_JUMP) != 0u) {
                pending_controls |= C15_MOVE_JUMP;
            }
            if (screen == SCREEN_PLAY &&
                (input.pressed & LITE_INPUT_WEAPON) != 0u) {
                uint32_t candidate = weapon;
                do {
                    candidate = (candidate + 1u) % WEAPON_COUNT;
                } while (!owned[candidate]);
                change_weapon(
                    &model_arena, persistent_models,
                    &weapon, candidate
                );
                reloading = 0;
                start_view_animation(
                    &view_animation, C15_VIEW_ANIMATION_DRAW, now
                );
                fire_until = 0u;
                fire_started = 0u;
            }
            if (screen == SCREEN_PLAY &&
                (input.pressed & LITE_INPUT_RELOAD) != 0u &&
                weapon != WEAPON_KNIFE &&
                weapon_ammo[weapon] < g_weapon_capacity[weapon]) {
                reload_weapon = weapon;
                reloading = 1;
                start_view_animation(
                    &view_animation, C15_VIEW_ANIMATION_RELOAD, now
                );
                c15_audio_play(
                    &audio, C15_SOUND_CUE_RELOAD,
                    C15_SOUND_CHANNEL_PLAYER
                );
            }
            if (screen == SCREEN_PLAY && player_health != 0u &&
                !reloading &&
                (input.down & LITE_INPUT_FIRE) != 0u &&
                time_reached(now, next_fire) &&
                ((input.pressed & LITE_INPUT_FIRE) != 0u ||
                 weapon == WEAPON_AK47 || weapon == WEAPON_M4A1) &&
                (weapon == WEAPON_KNIFE ||
                 weapon_ammo[weapon] != 0u)) {
                if (weapon != WEAPON_KNIFE) {
                    --weapon_ammo[weapon];
                }
                ++shots_fired;
                c15_audio_play(
                    &audio, weapon, C15_SOUND_CHANNEL_PLAYER
                );
                start_view_animation(
                    &view_animation, C15_VIEW_ANIMATION_FIRE, now
                );
                if (player_fire_hit(
                        bots, &camera, player_team, weapon, &money)) {
                    ++shots_hit;
                    hit_until = now + HIT_FEEDBACK_MS;
                }
                fire_started = now;
                fire_until = now + FIRE_FEEDBACK_MS;
                next_fire = now + g_weapon_interval_ms[weapon];
            }
            while (screen == SCREEN_PLAY &&
                   time_reached(now, next_logic) &&
                   logic_steps < 4u) {
                uint32_t t_alive;
                uint32_t ct_alive;
                if (player_health != 0u) {
                    c15_player_step(
                        &player, &g_map,
                        movement_controls(&input) | pending_controls
                    );
                    pending_controls = 0u;
                    c15_player_camera(&player, &camera);
                }
                bot_sound_weapon = WEAPON_COUNT;
                bot_logic(
                    bots, &player, player_team,
                    &player_health, bot_difficulty, map_id, now,
                    &bot_shots, &bot_hits, &bot_sound_weapon
                );
                if (bot_sound_weapon < WEAPON_COUNT) {
                    c15_audio_play(
                        &audio, bot_sound_weapon,
                        C15_SOUND_CHANNEL_BOT
                    );
                }
                update_world_animations(
                    &t_animation, &ct_animation, bots, now
                );
                team_counts(
                    bots, player_team, player_health,
                    &t_alive, &ct_alive
                );
                if (t_alive == 0u || ct_alive == 0u ||
                    (round_deadline != 0u &&
                     time_reached(now, round_deadline))) {
                    if (t_alive == 0u || ct_alive == 0u) {
                        round_winner =
                            t_alive != 0u ? TEAM_T : TEAM_CT;
                    } else {
                        /*
                         * de_dust is a bomb map: if the simplified
                         * no-C4 round timer expires, CT wins a tie.
                         */
                        round_winner =
                            t_alive > ct_alive ? TEAM_T : TEAM_CT;
                    }
                    if (round_winner == TEAM_T) ++rounds_t;
                    else ++rounds_ct;
                    if (round_winner == player_team) {
                        money += 3250u;
                    } else {
                        money += 1400u;
                    }
                    if (money > 16000u) money = 16000u;
                    screen = SCREEN_ROUND_END;
                    round_end_at = now + ROUND_END_MS;
                }
                next_logic += LOGIC_INTERVAL_MS;
                ++logic_steps;
            }
            if (screen == SCREEN_ROUND_END &&
                time_reached(now, round_end_at)) {
                ++round;
                initialize_round(
                    &player, bots, player_team,
                    bot_difficulty, map_id, now,
                    &player_health
                );
                t_animation.action = 0xffu;
                ct_animation.action = 0xffu;
                update_world_animations(
                    &t_animation, &ct_animation, bots, now
                );
                c15_player_camera(&player, &camera);
                weapon_ammo[weapon] = g_weapon_capacity[weapon];
                reloading = 0;
                start_view_animation(
                    &view_animation, C15_VIEW_ANIMATION_DRAW, now
                );
                screen = SCREEN_BUY;
                selection = 0u;
                menu_count = 3u;
                next_logic = now;
                round_deadline = 0u;
            }
        }

        if (time_reached(now, next_frame)) {
            uint32_t frame_end;
            uint32_t frame_elapsed;
            uint32_t instant_fps_x10;
            ++frame;
            next_frame += FRAME_INTERVAL_MS;
            if (time_reached(now, next_frame)) {
                next_frame = now + FRAME_INTERVAL_MS;
            }
            if (screen == SCREEN_MAIN) {
                draw_main_menu(&framebuffer, selection);
            } else if (screen == SCREEN_OPTIONS) {
                draw_options_menu(
                    &framebuffer, selection,
                    bot_difficulty, audio_enabled
                );
            } else if (screen == SCREEN_MAP) {
                draw_map_menu(&framebuffer, selection);
            } else if (screen == SCREEN_TEAM) {
                draw_team_menu(&framebuffer, selection);
            } else if (screen == SCREEN_BUY) {
                uint32_t rifle = player_team == TEAM_T ?
                    WEAPON_AK47 : WEAPON_M4A1;
                draw_buy_menu(
                    &framebuffer, player_team, money,
                    selection, owned[rifle]
                );
            } else {
                render_game(
                    &framebuffer, &view, &input, now, &camera, &player,
                    bots, player_team, player_health, money, round,
                    fps_x10, map_id, weapon,
                    time_active(now, fire_until),
                    time_active(now, hit_until),
                    (now - fire_started) / 54u,
                    view_animation.action == C15_VIEW_ANIMATION_FIRE ?
                        view_animation.frame : 0u,
                    weapon_ammo[weapon], screen, round_winner,
                    &stats, &view_stats, &entity_stats
                );
            }
            (void)lite_platform_present(g_screen);
            frame_end = lite_platform_milliseconds();
            frame_elapsed = frame_end - fps_last_frame;
            if (frame_elapsed != 0u) {
                instant_fps_x10 = 10000u / frame_elapsed;
                fps_x10 = fps_x10 == 0u ?
                    instant_fps_x10 :
                    (fps_x10 * 3u + instant_fps_x10) / 4u;
            }
            fps_last_frame = frame_end;
            now = frame_end;
        }
        if (time_reached(now, next_metric)) {
            lite_log_u32("frame", frame);
            lite_log_u32("fps_x10", fps_x10);
            lite_log_u32("screen", (uint32_t)screen);
            lite_log_u32("round", round);
            lite_log_u32("rounds_t", rounds_t);
            lite_log_u32("rounds_ct", rounds_ct);
            lite_log_u32("player_health", player_health);
            lite_log_u32("money", money);
            lite_log_u32("shots_fired", shots_fired);
            lite_log_u32("shots_hit", shots_hit);
            lite_log_u32("bot_shots", bot_shots);
            lite_log_u32("bot_hits", bot_hits);
            lite_log_u32("bot_difficulty", bot_difficulty);
            lite_log_u32("audio_enabled", audio_enabled);
            lite_log_u32("audio_blocks", audio.blocks_written);
            lite_log_u32("audio_short_writes", audio.short_writes);
            lite_log_u32("audio_ready_polls", audio.ready_polls);
            lite_log_i32(
                "audio_original_attenuation",
                audio.original_attenuation
            );
            lite_log_i32(
                "audio_playback_attenuation",
                audio.playback_attenuation
            );
            lite_log_u32("world_triangles", stats.drawn_triangles);
            lite_log_u32("entity_triangles", entity_stats.triangles);
            lite_log_u32("view_triangles", view_stats.triangles);
            lite_log_i32("bot0_x", bots[0].mover.x);
            lite_log_i32("bot0_y", bots[0].mover.y);
            lite_log_u32("bot0_nav", bots[0].nav_index);
            lite_log_u32("bot0_health", bots[0].health);
            lite_log_i32("bot3_x", bots[3].mover.x);
            lite_log_i32("bot3_y", bots[3].mover.y);
            lite_log_u32("bot3_nav", bots[3].nav_index);
            lite_log_u32("bot3_health", bots[3].health);
            next_metric = now + METRIC_INTERVAL_MS;
        }
        (void)c15_audio_service(
            &audio, &g_pak, g_load_scratch,
            sizeof(g_load_scratch)
        );
        lite_platform_delay(1u);
    }
    lite_log_u32("final_frame", frame);
    lite_log_u32("map_peak_bytes", (uint32_t)map_arena.peak);
    lite_log_u32(
        "texture_peak_bytes", (uint32_t)texture_arena.peak
    );
    lite_log_u32("model_peak_bytes", (uint32_t)model_arena.peak);
    lite_log_u32("shots_fired", shots_fired);
    lite_log_u32("shots_hit", shots_hit);
    lite_log_u32("bot_shots", bot_shots);
    lite_log_u32("bot_hits", bot_hits);
    lite_log_u32("bot_difficulty", bot_difficulty);
    lite_log_u32("audio_enabled", audio_enabled);
    lite_log_u32("audio_blocks", audio.blocks_written);
    lite_log_u32("audio_short_writes", audio.short_writes);
    lite_log_u32("audio_ready_polls", audio.ready_polls);
    lite_log_i32(
        "audio_original_attenuation", audio.original_attenuation
    );
    lite_log_i32(
        "audio_playback_attenuation", audio.playback_attenuation
    );
    c15_audio_stop(
        &audio, g_load_scratch, sizeof(g_load_scratch)
    );
    lite_log_line("CS15 Lite M11A stop");
    lite_log_close();
    c15_pak_close(&g_pak);
    lite_platform_close();
    return 0;
}
