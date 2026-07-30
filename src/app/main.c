/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bda_types.h"
#include "bda_memory.h"

#include "assets/pak.h"
#include "app/damage.h"
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
/* The historical 320x240 RGB565 menu splash occupies 153600 bytes. */
#define MENU_TEXTURE_ARENA_BYTES 160000u
/*
 * True-device probing found a safe 10.8125 MiB contiguous allocation and
 * 11.0625 MiB simultaneously held firmware heap. Keep a conservative 4 MiB
 * guard while making the selected map's geometry, PVS and textures resident.
 */
#define MAP_WORKING_SET_LIMIT_BYTES (4u * 1024u * 1024u)
#define AUDIO_READY_RETRY_MS 2u
/*
 * The two shared team skins retain a 64-pixel authored mip. A 96 KiB arena
 * leaves room for them, both held weapons and the largest animated view model
 * without runtime texture conversion or per-frame model streaming.
 */
#define MODEL_ARENA_BYTES (96u * 1024u)
/*
 * Four shared world locomotion chunks use about 57 KiB and the largest
 * current view animation uses about 211 KiB. Keep both resident so changing
 * an animation frame never seeks the FAT resource pack during play.
 */
#define ANIMATION_ARENA_BYTES (320u * 1024u)
#define VIEW_CACHE_ARENA_BYTES (4u * 1024u * 1024u)
#define LOAD_SCRATCH_BYTES 2048u
#define FRAME_INTERVAL_MS 40u
#define LOGIC_INTERVAL_MS 40u
#define LOGIC_MAX_CATCHUP_STEPS 2u
#define METRIC_INTERVAL_MS 5000u
#define FIRE_FEEDBACK_MS 160u
#define HIT_FEEDBACK_MS 140u
#define ROUND_END_MS 2500u
#define FREEZE_TIME_MS 5000u
#define ROUND_TIME_MS 90000u
#define BOMB_TIME_MS 35000u
#define BOMB_PLANT_MS 3000u
#define BOMB_DEFUSE_MS 5000u
#define BOMB_USE_DISTANCE 144u
#define BUY_TIME_MS 20000u
#define HOSTAGE_MAX 4u
#define GRENADE_MAX 6u
#define CORPSE_MAX 8u
#define IMPACT_MAX 8u
#define DROPPED_WEAPON_MAX 12u
#define WEAPON_PICKUP_DISTANCE 88u
#define GRENADE_HE 0u
#define GRENADE_FLASH 1u
#define GRENADE_SMOKE 2u
#define GRENADE_KIND_COUNT 3u
#define HOSTAGE_FOLLOW_NONE 0xffu
#define HOSTAGE_FOLLOW_PLAYER 0xfeu
#define DEFAULT_FOCAL_LENGTH 160u
#define ZOOM_FOCAL_LENGTH 280u
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
    SCREEN_BUY_ITEMS,
    SCREEN_PLAY,
    SCREEN_ROUND_END,
    SCREEN_PAUSE
};

enum bot_difficulty {
    BOT_EASY,
    BOT_NORMAL,
    BOT_HARD,
    BOT_DIFFICULTY_COUNT
};

enum bot_role {
    BOT_ROLE_ENTRY,
    BOT_ROLE_SUPPORT,
    BOT_ROLE_ANCHOR
};

enum round_end_reason {
    ROUND_REASON_NONE,
    ROUND_REASON_ELIMINATION,
    ROUND_REASON_TARGET_BOMBED,
    ROUND_REASON_BOMB_DEFUSED,
    ROUND_REASON_TARGET_SAVED,
    ROUND_REASON_HOSTAGES_RESCUED,
    ROUND_REASON_TIME_EXPIRED
};

enum map_id {
    MAP_DE_DUST2,
    MAP_FY_ICEWORLD,
    MAP_CS_ASSAULT,
    MAP_CS_ITALY,
    MAP_DE_INFERNO,
    MAP_DE_NUKE,
    MAP_CS_OFFICE,
    MAP_COUNT
};

enum weapon_id {
    WEAPON_KNIFE,
    WEAPON_GLOCK,
    WEAPON_USP,
    WEAPON_P228,
    WEAPON_DEAGLE,
    WEAPON_ELITE,
    WEAPON_FIVESEVEN,
    WEAPON_M3,
    WEAPON_XM1014,
    WEAPON_MAC10,
    WEAPON_TMP,
    WEAPON_MP5,
    WEAPON_UMP45,
    WEAPON_P90,
    WEAPON_AK47,
    WEAPON_SG552,
    WEAPON_M4A1,
    WEAPON_AUG,
    WEAPON_SCOUT,
    WEAPON_AWP,
    WEAPON_G3SG1,
    WEAPON_SG550,
    WEAPON_M249,
    WEAPON_COUNT
};

enum weapon_slot {
    WEAPON_SLOT_KNIFE,
    WEAPON_SLOT_PISTOL,
    WEAPON_SLOT_PRIMARY
};

enum buy_item {
    BUY_ITEM_HE = WEAPON_COUNT,
    BUY_ITEM_FLASH,
    BUY_ITEM_SMOKE,
    BUY_ITEM_ARMOR,
    BUY_ITEM_HELMET,
    BUY_ITEM_DEFUSE,
    BUY_ITEM_AMMO,
    BUY_ITEM_START,
    BUY_ITEM_COUNT
};

enum buy_category {
    BUY_CATEGORY_PISTOLS,
    BUY_CATEGORY_SHOTGUNS,
    BUY_CATEGORY_SMGS,
    BUY_CATEGORY_RIFLES,
    BUY_CATEGORY_MACHINE_GUN,
    BUY_CATEGORY_EQUIPMENT,
    BUY_CATEGORY_COUNT
};

#define BUY_CATEGORY_MAX_ITEMS 8u
#define BUY_ACTION_BACK BUY_ITEM_COUNT

typedef struct c15_bot {
    c15_player_t mover;
    uint32_t next_fire;
    uint32_t next_decision;
    uint32_t next_visibility_check;
    uint32_t flash_until;
    uint32_t last_enemy_seen;
    int32_t last_enemy_x;
    int32_t last_enemy_y;
    uint16_t health;
    uint16_t aim_seed;
    uint16_t armor;
    uint16_t money;
    uint16_t ammo;
    uint8_t team;
    uint8_t weapon;
    uint8_t alive;
    uint8_t target_visible_last;
    uint8_t visibility_cached;
    uint8_t visibility_target;
    uint8_t stuck_turn;
    uint8_t nav_index;
    uint8_t nav_stalls;
    uint8_t moving;
    uint8_t strafe_right;
    uint8_t burst_left;
    uint8_t combat;
    uint8_t bomb_carrier;
    uint8_t kills;
    uint8_t deaths;
    uint8_t helmet;
    uint8_t grenade;
    uint8_t nav_lane;
    uint8_t heard_shot;
    uint8_t role;
} c15_bot_t;

enum bomb_action {
    BOMB_ACTION_NONE,
    BOMB_ACTION_PLANT,
    BOMB_ACTION_DEFUSE
};

typedef struct c15_bomb_state {
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t action_started;
    uint32_t explode_at;
    uint32_t next_beep;
    uint8_t enabled;
    uint8_t player_carrier;
    uint8_t planted;
    uint8_t defused;
    uint8_t site;
    uint8_t action;
    uint8_t action_owner;
    uint8_t dropped;
} c15_bomb_state_t;

typedef struct c15_hostage {
    c15_player_t mover;
    uint16_t health;
    uint8_t active;
    uint8_t rescued;
    uint8_t follower;
    uint8_t moving;
} c15_hostage_t;

typedef struct c15_grenade {
    int32_t x_q8;
    int32_t y_q8;
    int32_t z_q8;
    int32_t vx_q8;
    int32_t vy_q8;
    int32_t vz_q8;
    uint32_t detonate_at;
    uint8_t kind;
    uint8_t owner_team;
    uint8_t owner;
    uint8_t active;
    uint8_t bounced;
} c15_grenade_t;

typedef struct c15_corpse {
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t died_at;
    uint8_t team;
    uint8_t active;
} c15_corpse_t;

typedef struct c15_impact {
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t until;
    uint8_t kind;
    uint8_t active;
} c15_impact_t;

typedef struct c15_dropped_weapon {
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t dropped_at;
    uint16_t ammo;
    uint16_t reserve;
    uint8_t weapon;
    uint8_t active;
} c15_dropped_weapon_t;

typedef struct c15_kill_feed {
    uint32_t until;
    uint8_t killer;
    uint8_t victim;
    uint8_t weapon;
    uint8_t headshot;
} c15_kill_feed_t;

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
    "maps/de_dust2",
    "maps/fy_iceworld",
    "maps/cs_assault",
    "maps/cs_italy",
    "maps/de_inferno",
    "maps/de_nuke",
    "maps/cs_office"
};

static const char *const g_map_labels[MAP_COUNT] = {
    "DE_DUST2",
    "FY_ICEWORLD",
    "CS_ASSAULT",
    "CS_ITALY",
    "DE_INFERNO",
    "DE_NUKE",
    "CS_OFFICE"
};

static const char *const g_map_loading[MAP_COUNT] = {
    "LOADING DE_DUST2 + PLAYERS",
    "LOADING FY_ICEWORLD + PLAYERS",
    "LOADING CS_ASSAULT + PLAYERS",
    "LOADING CS_ITALY + PLAYERS",
    "LOADING DE_INFERNO + PLAYERS",
    "LOADING DE_NUKE + PLAYERS",
    "LOADING CS_OFFICE + PLAYERS"
};

/*
 * All BSP sections, including PVS visibility, stay resident for the selected
 * map. This removes mid-round FAT seeks and still keeps the largest map
 * allocation (Office) around 1.3 MiB.
 */
static const uint32_t g_map_arena_bytes[MAP_COUNT] = {
    720000u, 40000u, 400000u, 1100000u,
    960000u, 1020000u, 1300000u
};

/*
 * Runtime texture storage is allocated only after a map is selected. Every
 * historical 64-pixel/256-colour map texture is loaded up front; the largest
 * complete set is about 520 KiB, well inside these existing safety arenas.
 */
static const uint32_t g_texture_arena_bytes[MAP_COUNT] = {
    1100000u, 155648u, 1300000u, 1200000u,
    1280000u, 1240000u, 1000000u
};

/* No release map performs texture paging during play. */
static const uint8_t g_map_texture_streaming[MAP_COUNT] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u
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

static const c15_nav_point_3d_t g_assault_route[] = {
    { 640,  160,  48}, { 640,  640,  48}, { 320,  960,  48},
    {   0, 1280,  48}, {-288, 1600, 176}, {-288, 1728, 176},
    {-496, 2352, 176}, {-528, 2384, 380}
};

static const c15_nav_point_3d_t g_italy_route[] = {
    {-768,-1856,-203}, {-768,-1312,-203}, {-512,-1312,-203},
    {-480,-1152,-190}, {-416,-1024,-171}, {-384, -704,-123},
    {-288, -416,-115}, { 160, -416,-115}, { 256,  128,-123},
    { 480,  384,-123}, { 704,  576,-107}, { 704,  832, -75},
    { 704, 1088, -43}, { 704, 1344, -11}, { 672, 1600,  21},
    { 640, 1888,  37}, { 320, 2112,  37}
};

static const c15_nav_point_3d_t g_iceworld_route[] = {
    { 128,   64,-195}, { 128, -192,-195}, { 128, -448,-195},
    { 256, -672,-195}, { 512, -896,-195}, { 512,-1216,-195}
};

static const c15_nav_point_3d_t g_inferno_route[] = {
    {2400,2208,125}, {2304,1728,125}, {2208,1216,125},
    {2024, 376,189}, {1472, 416,125}, { 832, 512, 93},
    { 192, 640, 61}, {-512, 736, -3}, {-1088, 800,-19},
    {-1664, 720,-19}
};

static const c15_nav_point_3d_t g_nuke_route[] = {
    {3216,-528,-311}, {2688,-576,-311}, {2144,-640,-311},
    {1600,-704,-311}, {1088,-736,-343}, { 680,-776,-371},
    { 128,-832,-371}, {-512,-896,-371}, {-1280,-960,-371},
    {-2080,-1024,-371}, {-2720,-1088,-371}
};

static const c15_nav_point_3d_t g_office_route[] = {
    {-1064,-1496,-303}, {-896,-1152,-303}, {-640,-832,-251},
    {-384,-512,-219}, { -64,-256,-187}, { 320, -64,-155},
    { 704, 128,-127}, {1152, 328,-127}, {1536, 472,-127}
};

#define DUST2_ROUTE_COUNT \
    ((uint32_t)(sizeof(g_dust2_route) / sizeof(g_dust2_route[0])))
#define ASSAULT_ROUTE_COUNT \
    ((uint32_t)(sizeof(g_assault_route) / sizeof(g_assault_route[0])))
#define ITALY_ROUTE_COUNT \
    ((uint32_t)(sizeof(g_italy_route) / sizeof(g_italy_route[0])))
#define ICEWORLD_ROUTE_COUNT \
    ((uint32_t)(sizeof(g_iceworld_route) / sizeof(g_iceworld_route[0])))
#define INFERNO_ROUTE_COUNT \
    ((uint32_t)(sizeof(g_inferno_route) / sizeof(g_inferno_route[0])))
#define NUKE_ROUTE_COUNT \
    ((uint32_t)(sizeof(g_nuke_route) / sizeof(g_nuke_route[0])))
#define OFFICE_ROUTE_COUNT \
    ((uint32_t)(sizeof(g_office_route) / sizeof(g_office_route[0])))

static uint32_t map_route_count(uint32_t map_id)
{
    if (map_id == MAP_DE_DUST2) return DUST2_ROUTE_COUNT;
    if (map_id == MAP_FY_ICEWORLD) return ICEWORLD_ROUTE_COUNT;
    if (map_id == MAP_CS_ASSAULT) return ASSAULT_ROUTE_COUNT;
    if (map_id == MAP_CS_ITALY) return ITALY_ROUTE_COUNT;
    if (map_id == MAP_DE_INFERNO) return INFERNO_ROUTE_COUNT;
    if (map_id == MAP_DE_NUKE) return NUKE_ROUTE_COUNT;
    return OFFICE_ROUTE_COUNT;
}

static int map_route_point(
    uint32_t map_id,
    uint8_t index,
    int32_t *x,
    int32_t *y,
    int32_t *z
)
{
    if (map_id == MAP_DE_DUST2 && index < DUST2_ROUTE_COUNT) {
        *x = g_dust2_route[index].x;
        *y = g_dust2_route[index].y;
        *z = g_dust2_route[index].z;
        return 1;
    }
    if (map_id == MAP_FY_ICEWORLD && index < ICEWORLD_ROUTE_COUNT) {
        *x = g_iceworld_route[index].x;
        *y = g_iceworld_route[index].y;
        *z = g_iceworld_route[index].z;
        return 1;
    }
    if (map_id == MAP_CS_ASSAULT && index < ASSAULT_ROUTE_COUNT) {
        *x = g_assault_route[index].x;
        *y = g_assault_route[index].y;
        *z = g_assault_route[index].z;
        return 1;
    }
    if (map_id == MAP_CS_ITALY && index < ITALY_ROUTE_COUNT) {
        *x = g_italy_route[index].x;
        *y = g_italy_route[index].y;
        *z = g_italy_route[index].z;
        return 1;
    }
    if (map_id == MAP_DE_INFERNO && index < INFERNO_ROUTE_COUNT) {
        *x = g_inferno_route[index].x;
        *y = g_inferno_route[index].y;
        *z = g_inferno_route[index].z;
        return 1;
    }
    if (map_id == MAP_DE_NUKE && index < NUKE_ROUTE_COUNT) {
        *x = g_nuke_route[index].x;
        *y = g_nuke_route[index].y;
        *z = g_nuke_route[index].z;
        return 1;
    }
    if (map_id == MAP_CS_OFFICE && index < OFFICE_ROUTE_COUNT) {
        *x = g_office_route[index].x;
        *y = g_office_route[index].y;
        *z = g_office_route[index].z;
        return 1;
    }
    return 0;
}

static int map_route_lane_point(
    uint32_t map_id,
    uint8_t index,
    uint8_t lane,
    int32_t *x,
    int32_t *y,
    int32_t *z
)
{
    int32_t previous_x;
    int32_t previous_y;
    int32_t previous_z;
    int32_t offset;
    int32_t route_dx;
    int32_t route_dy;
    if (!map_route_point(map_id, index, x, y, z)) {
        return 0;
    }
    if (lane == 1u || index == 0u ||
        !map_route_point(
            map_id, (uint8_t)(index - 1u),
            &previous_x, &previous_y, &previous_z)) {
        return 1;
    }
    (void)previous_z;
    offset = lane == 0u ? -56 : 56;
    route_dx = *x - previous_x;
    route_dy = *y - previous_y;
    if (route_dx < 0) route_dx = -route_dx;
    if (route_dy < 0) route_dy = -route_dy;
    if (route_dx >= route_dy) {
        *y += offset;
    } else {
        *x += offset;
    }
    return 1;
}

static uint16_t g_screen[LITE_SCREEN_WIDTH * LITE_SCREEN_HEIGHT];
static uint16_t g_depth[LITE_VIEW_WIDTH * LITE_VIEW_HEIGHT]
    __attribute__((aligned(4)));
static uint8_t g_model_memory[MODEL_ARENA_BYTES];
static uint8_t g_animation_memory[ANIMATION_ARENA_BYTES]
    __attribute__((aligned(16)));
static uint8_t g_load_scratch[LOAD_SCRATCH_BYTES]
    __attribute__((aligned(4)));
static uint8_t g_visible_surfaces[C15_MAP_VISIBLE_BYTES];
static c15_pak_t g_pak;
static c15_map_t g_map;
static c15_model_t g_view_model;
static c15_model_animation_t g_view_animation;
static c15_model_t g_view_models[WEAPON_COUNT];
static c15_model_animation_t g_view_animations[WEAPON_COUNT];
static c15_model_t g_t_model;
static c15_model_t g_ct_model;
static c15_model_t g_t_weapon_model;
static c15_model_t g_ct_weapon_model;
static c15_model_animation_t g_t_locomotion;
static c15_model_animation_t g_ct_locomotion;
static c15_model_animation_t g_t_weapon_locomotion;
static c15_model_animation_t g_ct_weapon_locomotion;
static lite_arena_t g_animation_arena;
static size_t g_persistent_animation_bytes;
static c15_muzzle_sprite_t g_muzzle;
static c15_muzzle_sprite_t g_muzzle_sprites[WEAPON_COUNT];
static int g_view_cache_loaded;
static const uint16_t *g_menu_background;

static const char *const g_weapon_assets[WEAPON_COUNT] = {
    "mdl/v_knife",
    "mdl/v_glock18",
    "mdl/v_usp",
    "mdl/v_p228",
    "mdl/v_deagle",
    "mdl/v_elite",
    "mdl/v_fiveseven",
    "mdl/v_m3",
    "mdl/v_xm1014",
    "mdl/v_mac10",
    "mdl/v_tmp",
    "mdl/v_mp5",
    "mdl/v_ump45",
    "mdl/v_p90",
    "mdl/v_ak47",
    "mdl/v_sg552",
    "mdl/v_m4a1",
    "mdl/v_aug",
    "mdl/v_scout",
    "mdl/v_awp",
    "mdl/v_g3sg1",
    "mdl/v_sg550",
    "mdl/v_m249"
};

static const char *const g_weapon_animation_assets[WEAPON_COUNT] = {
    "anim/v_knife",
    "anim/v_glock18",
    "anim/v_usp",
    "anim/v_p228",
    "anim/v_deagle",
    "anim/v_elite",
    "anim/v_fiveseven",
    "anim/v_m3",
    "anim/v_xm1014",
    "anim/v_mac10",
    "anim/v_tmp",
    "anim/v_mp5",
    "anim/v_ump45",
    "anim/v_p90",
    "anim/v_ak47",
    "anim/v_sg552",
    "anim/v_m4a1",
    "anim/v_aug",
    "anim/v_scout",
    "anim/v_awp",
    "anim/v_g3sg1",
    "anim/v_sg550",
    "anim/v_m249"
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
    "muzzle/v_usp",
    "muzzle/v_p228",
    "muzzle/v_deagle",
    "muzzle/v_elite",
    "muzzle/v_fiveseven",
    "muzzle/v_m3",
    "muzzle/v_xm1014",
    "muzzle/v_mac10",
    "muzzle/v_tmp",
    "muzzle/v_mp5",
    "muzzle/v_ump45",
    "muzzle/v_p90",
    "muzzle/v_ak47",
    "muzzle/v_sg552",
    "muzzle/v_m4a1",
    "muzzle/v_aug",
    "muzzle/v_scout",
    "muzzle/v_awp",
    "muzzle/v_g3sg1",
    "muzzle/v_sg550",
    "muzzle/v_m249"
};

static int resource_pack_has(
    const char *name,
    uint32_t expected_type
)
{
    c15_pak_entry_t entry;
    if (c15_pak_find(&g_pak, name, &entry) &&
        entry.type == expected_type) {
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
        "meta/m20", C15_FOURCC('V','E','R','0')
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
    "KNIFE", "GLOCK", "USP", "P228", "DEAGLE", "ELITE", "FIVE7",
    "M3", "XM1014", "MAC10", "TMP", "MP5", "UMP45", "P90",
    "AK47", "SG552", "M4A1", "AUG", "SCOUT", "AWP", "G3SG1",
    "SG550", "M249"
};

static const uint16_t g_weapon_capacity[WEAPON_COUNT] = {
    0u, 20u, 12u, 13u, 7u, 30u, 20u, 8u, 7u, 30u, 30u,
    30u, 25u, 50u, 30u, 30u, 30u, 30u, 10u, 10u, 20u, 30u,
    100u
};

static const uint16_t g_weapon_interval_ms[WEAPON_COUNT] = {
    350u, 240u, 220u, 220u, 250u, 180u, 220u, 875u, 250u,
    85u, 90u, 85u, 95u, 70u, 110u, 95u, 105u, 95u, 1250u,
    1450u, 250u, 250u, 80u
};

static const uint8_t g_weapon_damage[WEAPON_COUNT] = {
    50u, 25u, 34u, 32u, 54u, 36u, 20u, 20u, 20u, 29u, 20u,
    26u, 30u, 21u, 36u, 33u, 32u, 32u, 75u, 115u, 80u, 70u,
    32u
};

static const uint16_t g_weapon_price[WEAPON_COUNT] = {
    0u, 400u, 500u, 600u, 650u, 800u, 750u, 1700u, 3000u,
    1400u, 1250u, 1500u, 1700u, 2350u, 2500u, 3500u, 3100u,
    3500u, 2750u, 4750u, 5000u, 4200u, 5750u
};

static const char *const g_equipment_labels[
    BUY_ITEM_COUNT - WEAPON_COUNT
] = {
    "HE GRENADE", "FLASHBANG", "SMOKE",
    "KEVLAR", "KEVLAR+HELMET", "DEFUSE KIT", "AMMO", "START ROUND"
};

static const uint16_t g_equipment_price[
    BUY_ITEM_COUNT - WEAPON_COUNT
] = {
    300u, 200u, 300u, 650u, 1000u, 200u, 300u, 0u
};

static const char *const g_buy_category_labels[BUY_CATEGORY_COUNT] = {
    "PISTOLS",
    "SHOTGUNS",
    "SUBMACHINE GUNS",
    "RIFLES",
    "MACHINE GUN",
    "EQUIPMENT"
};

static const uint8_t g_buy_category_item_count[BUY_CATEGORY_COUNT] = {
    6u, 2u, 5u, 8u, 1u, 7u
};

static const uint8_t g_buy_category_items[
    BUY_CATEGORY_COUNT
][BUY_CATEGORY_MAX_ITEMS] = {
    {
        WEAPON_GLOCK, WEAPON_USP, WEAPON_P228, WEAPON_DEAGLE,
        WEAPON_ELITE, WEAPON_FIVESEVEN
    },
    {WEAPON_M3, WEAPON_XM1014},
    {
        WEAPON_MAC10, WEAPON_TMP, WEAPON_MP5, WEAPON_UMP45,
        WEAPON_P90
    },
    {
        WEAPON_AK47, WEAPON_SG552, WEAPON_M4A1, WEAPON_AUG,
        WEAPON_SCOUT, WEAPON_AWP, WEAPON_G3SG1, WEAPON_SG550
    },
    {WEAPON_M249},
    {
        BUY_ITEM_HE, BUY_ITEM_FLASH, BUY_ITEM_SMOKE, BUY_ITEM_ARMOR,
        BUY_ITEM_HELMET, BUY_ITEM_DEFUSE, BUY_ITEM_AMMO
    }
};

static uint32_t buy_category_item(uint32_t category, uint32_t index)
{
    if (category >= BUY_CATEGORY_COUNT ||
        index >= g_buy_category_item_count[category]) {
        return 0xffffffffu;
    }
    return g_buy_category_items[category][index];
}

static uint32_t equipment_price(
    uint32_t item, uint16_t player_armor
)
{
    if (item == BUY_ITEM_HELMET && player_armor != 0u) {
        return 350u;
    }
    return g_equipment_price[item - WEAPON_COUNT];
}

static const uint8_t g_weapon_automatic[WEAPON_COUNT] = {
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 1u,
    1u, 1u, 1u, 1u, 1u, 1u, 0u, 0u, 1u, 1u, 1u
};

static const uint8_t g_weapon_slot[WEAPON_COUNT] = {
    [WEAPON_KNIFE] = WEAPON_SLOT_KNIFE,
    [WEAPON_GLOCK] = WEAPON_SLOT_PISTOL,
    [WEAPON_USP] = WEAPON_SLOT_PISTOL,
    [WEAPON_P228] = WEAPON_SLOT_PISTOL,
    [WEAPON_DEAGLE] = WEAPON_SLOT_PISTOL,
    [WEAPON_ELITE] = WEAPON_SLOT_PISTOL,
    [WEAPON_FIVESEVEN] = WEAPON_SLOT_PISTOL,
    [WEAPON_M3] = WEAPON_SLOT_PRIMARY,
    [WEAPON_XM1014] = WEAPON_SLOT_PRIMARY,
    [WEAPON_MAC10] = WEAPON_SLOT_PRIMARY,
    [WEAPON_TMP] = WEAPON_SLOT_PRIMARY,
    [WEAPON_MP5] = WEAPON_SLOT_PRIMARY,
    [WEAPON_UMP45] = WEAPON_SLOT_PRIMARY,
    [WEAPON_P90] = WEAPON_SLOT_PRIMARY,
    [WEAPON_AK47] = WEAPON_SLOT_PRIMARY,
    [WEAPON_SG552] = WEAPON_SLOT_PRIMARY,
    [WEAPON_M4A1] = WEAPON_SLOT_PRIMARY,
    [WEAPON_AUG] = WEAPON_SLOT_PRIMARY,
    [WEAPON_SCOUT] = WEAPON_SLOT_PRIMARY,
    [WEAPON_AWP] = WEAPON_SLOT_PRIMARY,
    [WEAPON_G3SG1] = WEAPON_SLOT_PRIMARY,
    [WEAPON_SG550] = WEAPON_SLOT_PRIMARY,
    [WEAPON_M249] = WEAPON_SLOT_PRIMARY
};

/* 0 = both teams, 1 = Terrorist, 2 = Counter-Terrorist. */
static const uint8_t g_weapon_team[WEAPON_COUNT] = {
    [WEAPON_GLOCK] = TEAM_T,
    [WEAPON_USP] = TEAM_CT,
    [WEAPON_ELITE] = TEAM_T,
    [WEAPON_FIVESEVEN] = TEAM_CT,
    [WEAPON_MAC10] = TEAM_T,
    [WEAPON_TMP] = TEAM_CT,
    [WEAPON_AK47] = TEAM_T,
    [WEAPON_SG552] = TEAM_T,
    [WEAPON_M4A1] = TEAM_CT,
    [WEAPON_AUG] = TEAM_CT,
    [WEAPON_G3SG1] = TEAM_T,
    [WEAPON_SG550] = TEAM_CT
};

static const uint16_t g_weapon_reserve_max[WEAPON_COUNT] = {
    0u, 120u, 100u, 52u, 35u, 120u, 100u, 32u, 32u,
    100u, 120u, 120u, 100u, 100u, 90u, 90u, 90u, 90u,
    90u, 30u, 90u, 90u, 200u
};

/* Angular spread and recoil use the camera's Q8-angle representation. */
static const uint16_t g_weapon_spread_q8[WEAPON_COUNT] = {
    0u, 130u, 100u, 115u, 85u, 150u, 105u, 820u, 700u,
    165u, 150u, 125u, 145u, 140u, 100u, 95u, 88u, 92u,
    70u, 64u, 80u, 78u, 150u
};

static const uint16_t g_weapon_recoil_q8[WEAPON_COUNT] = {
    0u, 85u, 70u, 75u, 145u, 65u, 68u, 180u, 145u,
    42u, 38u, 40u, 46u, 34u, 58u, 52u, 46u, 50u,
    110u, 190u, 78u, 76u, 48u
};

static const uint8_t g_weapon_pellets[WEAPON_COUNT] = {
    [WEAPON_KNIFE] = 1u,
    [WEAPON_GLOCK] = 1u, [WEAPON_USP] = 1u,
    [WEAPON_P228] = 1u, [WEAPON_DEAGLE] = 1u,
    [WEAPON_ELITE] = 1u, [WEAPON_FIVESEVEN] = 1u,
    [WEAPON_M3] = 9u, [WEAPON_XM1014] = 7u,
    [WEAPON_MAC10] = 1u, [WEAPON_TMP] = 1u,
    [WEAPON_MP5] = 1u, [WEAPON_UMP45] = 1u,
    [WEAPON_P90] = 1u, [WEAPON_AK47] = 1u,
    [WEAPON_SG552] = 1u, [WEAPON_M4A1] = 1u,
    [WEAPON_AUG] = 1u, [WEAPON_SCOUT] = 1u,
    [WEAPON_AWP] = 1u, [WEAPON_G3SG1] = 1u,
    [WEAPON_SG550] = 1u, [WEAPON_M249] = 1u
};

static const uint8_t g_weapon_penetration[WEAPON_COUNT] = {
    [WEAPON_DEAGLE] = 1u,
    [WEAPON_MP5] = 1u,
    [WEAPON_UMP45] = 1u,
    [WEAPON_AK47] = 2u,
    [WEAPON_SG552] = 2u,
    [WEAPON_M4A1] = 2u,
    [WEAPON_AUG] = 2u,
    [WEAPON_SCOUT] = 1u,
    [WEAPON_AWP] = 3u,
    [WEAPON_G3SG1] = 2u,
    [WEAPON_SG550] = 2u,
    [WEAPON_M249] = 2u
};

/* Damage retained for every 500 map units. */
static const uint16_t g_weapon_range_modifier_q8[WEAPON_COUNT] = {
    256u, 192u, 202u, 205u, 207u, 192u, 227u, 256u, 256u,
    210u, 218u, 215u, 210u, 227u, 251u, 244u, 248u, 246u,
    251u, 253u, 251u, 251u, 248u
};

/*
 * GoldSrc starts at a 0.5 health-damage ratio through Kevlar, then applies
 * a weapon-specific multiplier. These Q8 values preserve classic behavior:
 * AWP and AK torso/head hits are not flattened by a generic 40% reduction.
 */
static const uint16_t g_weapon_armor_ratio_q8[WEAPON_COUNT] = {
    218u, 134u, 128u, 160u, 192u, 134u, 192u, 128u, 128u,
    122u, 128u, 128u, 128u, 192u, 198u, 179u, 179u, 179u,
    218u, 250u, 211u, 186u, 192u
};

static uint32_t weapon_base_damage(uint32_t weapon, int silenced)
{
    if (weapon == WEAPON_USP && silenced) {
        return 30u;
    }
    if (weapon == WEAPON_M4A1 && silenced) {
        return 33u;
    }
    return g_weapon_damage[weapon];
}

static uint16_t weapon_range_modifier_q8(
    uint32_t weapon, int silenced
)
{
    if (weapon == WEAPON_M4A1 && silenced) {
        return 243u;
    }
    return g_weapon_range_modifier_q8[weapon];
}

static const c15_model_view_t g_weapon_views[WEAPON_COUNT] = {
    {160, 120, 160, 0, 80},
    {160, 120, 160, 0, 80},
    {160, 120, 160, 0, 80},
    {160, 120, 160, 0, 80},
    {160, 120, 160, 0, 80},
    {154, 108, 155, 16, 62},
    {160, 120, 160, 0, 80},
    {148, 98, 155, 24, 56},
    {148, 98, 155, 24, 56},
    {150, 102, 155, 22, 58},
    {150, 102, 155, 22, 58},
    {150, 102, 155, 22, 58},
    {150, 102, 155, 22, 58},
    {150, 98, 155, 24, 56},
    {145, 88, 155, 40, 48},
    {146, 92, 155, 34, 50},
    {148, 94, 155, 30, 52},
    {146, 92, 155, 34, 50},
    {146, 92, 155, 34, 50},
    {146, 90, 155, 36, 48},
    {146, 90, 155, 36, 48},
    {146, 90, 155, 36, 48},
    {146, 90, 155, 36, 48}
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

typedef struct c15_timing_accumulator {
    uint32_t total_ms;
    uint32_t maximum_ms;
    uint32_t samples;
} c15_timing_accumulator_t;

typedef struct c15_render_frame_timing {
    uint32_t world_ms;
    uint32_t world_clear_ms;
    uint32_t entities_ms;
    uint32_t view_ms;
    uint32_t hud_ms;
    uint32_t entity_pvs_culled;
} c15_render_frame_timing_t;

static void timing_add(
    c15_timing_accumulator_t *timing, uint32_t elapsed_ms
)
{
    timing->total_ms += elapsed_ms;
    if (elapsed_ms > timing->maximum_ms) {
        timing->maximum_ms = elapsed_ms;
    }
    ++timing->samples;
}

static uint32_t timing_average_x10(
    const c15_timing_accumulator_t *timing
)
{
    if (timing->samples == 0u) {
        return 0u;
    }
    return (timing->total_ms * 10u + timing->samples / 2u) /
        timing->samples;
}

static void timing_reset(c15_timing_accumulator_t *timing)
{
    bda_memset(timing, 0, sizeof(*timing));
}

static int time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static int time_active(uint32_t now, uint32_t deadline)
{
    return (int32_t)(deadline - now) > 0;
}

static void service_audio_throttled(
    c15_audio_t *audio,
    const c15_pak_t *pak,
    void *scratch,
    uint32_t scratch_size,
    uint32_t now,
    uint32_t *next_service,
    uint32_t *pending_ms
)
{
    uint32_t started;
    int result;
    if (!time_reached(now, *next_service)) {
        return;
    }
    started = lite_platform_milliseconds();
    result = c15_audio_service(
        audio, pak, scratch, scratch_size
    );
    *pending_ms += lite_platform_milliseconds() - started;
    /*
     * A successful write may leave another firmware queue slot available,
     * so permit the second service point in this loop to fill it. When the
     * queue is not ready, avoid polling it thousands of times in the same
     * millisecond; two milliseconds is still far below a 1024-byte block's
     * roughly 23 ms playback duration.
     */
    *next_service = result > 0 ? now : now + AUDIO_READY_RETRY_MS;
}

static void pause_shift_timestamp(uint32_t *timestamp, uint32_t elapsed)
{
    if (timestamp && *timestamp != 0u) {
        *timestamp += elapsed;
    }
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

static void draw_controls(
    lite_framebuffer_t *fb,
    const lite_input_t *input,
    const char *alt_label
)
{
    draw_button(
        fb, LITE_BUTTON_X, LITE_BUTTON_USE_Y,
        LITE_BUTTON_WIDTH, LITE_BUTTON_HEIGHT, "USE",
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
    draw_button(
        fb, LITE_BUTTON_X, LITE_BUTTON_ALT_Y,
        LITE_BUTTON_WIDTH, LITE_BUTTON_HEIGHT, alt_label,
        (input->down & LITE_INPUT_ALT) != 0u
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

static const char *weapon_alt_label(
    uint32_t weapon, int zoomed, int silenced, int glock_burst
)
{
    if (weapon == WEAPON_KNIFE) return "STAB";
    if (weapon == WEAPON_GLOCK) {
        return glock_burst ? "BURST" : "SEMI";
    }
    if (weapon == WEAPON_USP || weapon == WEAPON_M4A1) {
        return silenced ? "SILENT" : "LOUD";
    }
    if (weapon == WEAPON_SG552 || weapon == WEAPON_AUG ||
        weapon == WEAPON_SCOUT || weapon == WEAPON_AWP ||
        weapon == WEAPON_G3SG1 || weapon == WEAPON_SG550) {
        return zoomed ? "ZOOM" : "SCOPE";
    }
    return "GRENADE";
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
    c15_pak_entry_t entry;
    uint8_t header[16];
    uint32_t pixel_bytes;
    uint16_t *pixels;
    if (!c15_pak_find(&g_pak, "ui/menu_splash", &entry) ||
        entry.type != C15_FOURCC('I','M','G','0') ||
        entry.packed_size < sizeof(header) ||
        !c15_pak_validate_entry(
            &g_pak, &entry, g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_pak_read(
            &g_pak, &entry, 0u, header, sizeof(header)) ||
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
        entry.packed_size != sizeof(header) + pixel_bytes) {
        return 0;
    }
    pixels = (uint16_t *)lite_arena_alloc(
        texture_arena, pixel_bytes, 16u
    );
    if (!pixels || !c15_pak_read(
            &g_pak, &entry, sizeof(header), pixels, pixel_bytes)) {
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

static uint32_t map_window_start(uint32_t selection)
{
    uint32_t total = MAP_COUNT + 1u;
    uint32_t start = selection > 2u ? selection - 2u : 0u;
    if (start + 5u > total) {
        start = total - 5u;
    }
    return start;
}

static void draw_map_menu(lite_framebuffer_t *fb, uint32_t selection)
{
    uint16_t pale = lite_rgb565(211u, 214u, 190u);
    uint32_t start = map_window_start(selection);
    uint32_t row;
    draw_menu_chrome(fb, "CREATE GAME - SELECT MAP");
    lite_fb_text(fb, 35, 68, "AVAILABLE MAPS", 1, pale);
    for (row = 0u; row < 5u; ++row) {
        uint32_t item = start + row;
        draw_button(
            fb, 34, 82 + (int)row * 27, 176, 24,
            item == MAP_COUNT ? "BACK" : g_map_labels[item],
            selection == item
        );
    }
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

static uint32_t buy_item_window_start(
    uint32_t selection, uint32_t total
)
{
    uint32_t start = selection > 1u ? selection - 1u : 0u;
    if (total <= 4u) {
        return 0u;
    }
    if (start + 4u > total) {
        start = total - 4u;
    }
    return start;
}

static void draw_buy_category_menu(
    lite_framebuffer_t *fb,
    uint32_t money,
    uint32_t selection
)
{
    uint16_t pale = lite_rgb565(211u, 214u, 190u);
    uint16_t gold = lite_rgb565(224u, 168u, 54u);
    uint32_t row;
    draw_menu_chrome(fb, "BUY MENU - SELECT CATEGORY");
    lite_fb_text(fb, 28, 70, "MONEY", 1, pale);
    lite_fb_u32(fb, 68, 70, money, 1, gold);
    for (row = 0u; row < BUY_CATEGORY_COUNT; ++row) {
        int y = 84 + (int)row * 20;
        draw_button(
            fb, 28, y, 190, 18, g_buy_category_labels[row],
            selection == row
        );
        lite_fb_u32(
            fb, 226, y + 6, g_buy_category_item_count[row], 1,
            row == selection ? gold : pale
        );
    }
    draw_button(fb, 232, 205, 82, 27, "START ROUND", 0);
}

static void draw_buy_item_menu(
    lite_framebuffer_t *fb,
    uint32_t money,
    uint32_t selection,
    uint32_t category,
    const uint8_t owned[WEAPON_COUNT],
    uint8_t player_team,
    uint16_t player_armor
)
{
    uint16_t pale = lite_rgb565(211u, 214u, 190u);
    uint16_t gold = lite_rgb565(224u, 168u, 54u);
    uint32_t total = category < BUY_CATEGORY_COUNT ?
        g_buy_category_item_count[category] : 0u;
    uint32_t start = buy_item_window_start(selection, total);
    uint32_t row;
    draw_menu_chrome(fb, "BUY MENU - SELECT ITEM");
    if (category < BUY_CATEGORY_COUNT) {
        lite_fb_text(fb, 28, 70, g_buy_category_labels[category], 1, gold);
    }
    lite_fb_text(fb, 188, 70, "MONEY", 1, pale);
    lite_fb_u32(fb, 230, 70, money, 1, gold);
    for (row = 0u; row < 4u && start + row < total; ++row) {
        uint32_t category_index = start + row;
        uint32_t item = buy_category_item(category, category_index);
        int y = 88 + (int)row * 27;
        if (item < WEAPON_COUNT) {
            draw_button(
                fb, 28, y, 158, 23, g_weapon_labels[item],
                selection == category_index
            );
            if (g_weapon_team[item] != 0u &&
                g_weapon_team[item] != player_team) {
                lite_fb_text(fb, 192, y + 8, "LOCK", 1, pale);
            } else if (owned[item]) {
                lite_fb_text(fb, 192, y + 8, "OWN", 1, gold);
            } else {
                lite_fb_u32(
                    fb, 192, y + 8, g_weapon_price[item], 1, pale
                );
            }
        } else {
            uint32_t equipment = item - WEAPON_COUNT;
            draw_button(
                fb, 28, y, 158, 23,
                g_equipment_labels[equipment],
                selection == category_index
            );
            if (item == BUY_ITEM_DEFUSE &&
                player_team != TEAM_CT) {
                lite_fb_text(fb, 192, y + 8, "LOCK", 1, pale);
            } else {
                lite_fb_u32(
                    fb, 192, y + 8,
                    equipment_price(item, player_armor), 1, pale
                );
            }
        }
    }
    draw_button(fb, 28, 205, 82, 27, "BACK", 0);
    draw_button(fb, 232, 205, 82, 27, "START ROUND", 0);
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
    c15_muzzle_sprite_t *muzzle,
    lite_arena_t *arena,
    uint32_t weapon
)
{
    c15_pak_entry_t entry;
    uint8_t *chunk;
    uint32_t frame_bytes;
    uint32_t expected;
    if (!muzzle || !arena) {
        return 0;
    }
    bda_memset(muzzle, 0, sizeof(*muzzle));
    if (weapon == WEAPON_KNIFE) {
        return 1;
    }
    if (!c15_pak_find(
            &g_pak, g_weapon_muzzle_assets[weapon], &entry) ||
        entry.type != C15_FOURCC('M','S','P','0') ||
        entry.packed_size < 44u || entry.packed_size > 2048u) {
        return 0;
    }
    chunk = (uint8_t *)lite_arena_alloc(
        arena, entry.packed_size, 16u
    );
    if (!chunk ||
        !c15_pak_read(
            &g_pak, &entry, 0u, chunk, entry.packed_size) ||
        !bytes_equal(chunk, "MSP1", 4u) ||
        read_u16(chunk + 4) != entry.packed_size) {
        return 0;
    }
    muzzle->width = chunk[6];
    muzzle->height = chunk[7];
    muzzle->frame_count = chunk[8];
    muzzle->anchor_count = chunk[9];
    muzzle->display_size = chunk[10];
    frame_bytes = (
        (uint32_t)muzzle->width * muzzle->height + 1u
    ) / 2u;
    expected = 44u + (uint32_t)muzzle->anchor_count * 6u +
        (uint32_t)muzzle->frame_count * frame_bytes;
    if (muzzle->width == 0u || muzzle->width > 32u ||
        muzzle->height == 0u || muzzle->height > 32u ||
        muzzle->frame_count == 0u || muzzle->frame_count > 4u ||
        muzzle->anchor_count == 0u || muzzle->anchor_count > 16u ||
        muzzle->display_size == 0u || muzzle->display_size > 64u ||
        chunk[11] != 0u || expected != entry.packed_size) {
        bda_memset(muzzle, 0, sizeof(*muzzle));
        return 0;
    }
    muzzle->chunk = chunk;
    muzzle->anchors = chunk + 44u;
    muzzle->pixels = muzzle->anchors +
        (uint32_t)muzzle->anchor_count * 6u;
    muzzle->loaded = 1u;
    return 1;
}

static int load_view_cache(lite_arena_t *arena)
{
    uint32_t weapon;
    if (!arena) {
        return 0;
    }
    lite_arena_reset(arena);
    g_view_cache_loaded = 0;
    bda_memset(g_view_models, 0, sizeof(g_view_models));
    bda_memset(g_view_animations, 0, sizeof(g_view_animations));
    bda_memset(g_muzzle_sprites, 0, sizeof(g_muzzle_sprites));
    for (weapon = 0u; weapon < WEAPON_COUNT; ++weapon) {
        if (!c15_model_load(
                &g_view_models[weapon], &g_pak,
                g_weapon_assets[weapon], arena,
                g_load_scratch, sizeof(g_load_scratch)) ||
            !c15_model_animation_open(
                &g_view_animations[weapon],
                &g_view_models[weapon], &g_pak,
                g_weapon_animation_assets[weapon],
                g_load_scratch, sizeof(g_load_scratch)) ||
            !c15_model_animation_make_resident(
                &g_view_animations[weapon], &g_pak, arena) ||
            !load_muzzle_sprite(
                &g_muzzle_sprites[weapon], arena, weapon)) {
            lite_log_line("view cache load failed");
            lite_log_u32("view_cache_failed_weapon", weapon);
            lite_log_u32(
                "view_cache_arena_used", (uint32_t)arena->used
            );
            lite_log_u32(
                "view_cache_arena_failures", arena->failures
            );
            return 0;
        }
    }
    g_view_cache_loaded = 1;
    return 1;
}

static void change_weapon(
    lite_arena_t *model_arena,
    size_t persistent_models,
    uint32_t *weapon,
    uint32_t requested
)
{
    if (g_view_cache_loaded && requested < WEAPON_COUNT) {
        *weapon = requested;
        g_view_model = g_view_models[requested];
        g_view_animation = g_view_animations[requested];
        g_muzzle = g_muzzle_sprites[requested];
        return;
    }
    lite_arena_rewind(model_arena, persistent_models);
    lite_arena_rewind(
        &g_animation_arena, g_persistent_animation_bytes
    );
    *weapon = requested;
    if (!c15_model_load(
            &g_view_model, &g_pak, g_weapon_assets[requested],
            model_arena, g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_animation_open(
            &g_view_animation, &g_view_model, &g_pak,
            g_weapon_animation_assets[requested],
            g_load_scratch, sizeof(g_load_scratch)) ||
        !c15_model_animation_make_resident(
            &g_view_animation, &g_pak, &g_animation_arena) ||
        !load_muzzle_sprite(&g_muzzle, model_arena, requested)) {
        lite_log_line("view model load failed");
        bda_memset(&g_view_model, 0, sizeof(g_view_model));
        bda_memset(&g_view_animation, 0, sizeof(g_view_animation));
        bda_memset(&g_muzzle, 0, sizeof(g_muzzle));
    }
}

static int weapon_allowed_for_team(uint32_t weapon, uint8_t team)
{
    return weapon < WEAPON_COUNT &&
        (g_weapon_team[weapon] == 0u ||
         g_weapon_team[weapon] == team);
}

static void own_weapon_in_slot(
    uint8_t owned[WEAPON_COUNT], uint32_t weapon
)
{
    uint32_t index;
    uint8_t slot = g_weapon_slot[weapon];
    for (index = 0u; index < WEAPON_COUNT; ++index) {
        if (index != WEAPON_KNIFE && g_weapon_slot[index] == slot) {
            owned[index] = 0u;
        }
    }
    owned[weapon] = 1u;
}

static uint32_t owned_weapon_in_slot(
    const uint8_t owned[WEAPON_COUNT], uint8_t slot
)
{
    uint32_t index;
    for (index = 0u; index < WEAPON_COUNT; ++index) {
        if (owned[index] && g_weapon_slot[index] == slot) {
            return index;
        }
    }
    return WEAPON_COUNT;
}

static void drop_weapon(
    c15_dropped_weapon_t dropped[DROPPED_WEAPON_MAX],
    uint32_t weapon,
    uint16_t ammo,
    uint16_t reserve,
    int32_t x,
    int32_t y,
    int32_t z,
    uint32_t now
)
{
    uint32_t index;
    uint32_t selected = DROPPED_WEAPON_MAX;
    uint32_t oldest = 0xffffffffu;
    if (weapon == WEAPON_KNIFE || weapon >= WEAPON_COUNT) {
        return;
    }
    for (index = 0u; index < DROPPED_WEAPON_MAX; ++index) {
        if (!dropped[index].active) {
            selected = index;
            break;
        }
        if (dropped[index].dropped_at < oldest) {
            oldest = dropped[index].dropped_at;
            selected = index;
        }
    }
    dropped[selected].x = x;
    dropped[selected].y = y;
    dropped[selected].z = z;
    dropped[selected].dropped_at = now;
    dropped[selected].ammo = ammo;
    dropped[selected].reserve = reserve;
    dropped[selected].weapon = (uint8_t)weapon;
    dropped[selected].active = 1u;
}

static int pickup_weapon(
    c15_dropped_weapon_t dropped[DROPPED_WEAPON_MAX],
    const c15_player_t *player,
    uint8_t owned[WEAPON_COUNT],
    uint16_t ammo[WEAPON_COUNT],
    uint16_t reserve[WEAPON_COUNT],
    uint32_t *weapon,
    uint32_t now
)
{
    uint32_t index;
    uint32_t selected = DROPPED_WEAPON_MAX;
    uint32_t nearest = 0xffffffffu;
    c15_dropped_weapon_t pickup;
    uint32_t replaced;
    for (index = 0u; index < DROPPED_WEAPON_MAX; ++index) {
        uint32_t distance;
        if (!dropped[index].active ||
            abs_i32(dropped[index].z - player->z) > 80u) {
            continue;
        }
        distance = distance_squared(
            player->x, player->y,
            dropped[index].x, dropped[index].y
        );
        if (distance <= WEAPON_PICKUP_DISTANCE *
                        WEAPON_PICKUP_DISTANCE &&
            distance < nearest) {
            nearest = distance;
            selected = index;
        }
    }
    if (selected == DROPPED_WEAPON_MAX) {
        return 0;
    }
    pickup = dropped[selected];
    dropped[selected].active = 0u;
    replaced = owned_weapon_in_slot(
        owned, g_weapon_slot[pickup.weapon]
    );
    if (replaced < WEAPON_COUNT && replaced != pickup.weapon) {
        drop_weapon(
            dropped, replaced, ammo[replaced], reserve[replaced],
            player->x, player->y, player->z, now
        );
        owned[replaced] = 0u;
        ammo[replaced] = 0u;
        reserve[replaced] = 0u;
    }
    own_weapon_in_slot(owned, pickup.weapon);
    ammo[pickup.weapon] = pickup.ammo != 0u ?
        pickup.ammo : g_weapon_capacity[pickup.weapon];
    reserve[pickup.weapon] = pickup.reserve;
    *weapon = pickup.weapon;
    return 1;
}

static uint8_t next_spectator_target(
    const c15_bot_t bots[BOT_COUNT],
    uint8_t player_team,
    uint8_t current
)
{
    uint32_t pass;
    for (pass = 0u; pass < 2u; ++pass) {
        uint32_t step;
        for (step = 1u; step <= BOT_COUNT; ++step) {
            uint32_t index = current == 0xffu ?
                step - 1u : ((uint32_t)current + step) % BOT_COUNT;
            if (bots[index].alive &&
                (pass != 0u || bots[index].team == player_team)) {
                return (uint8_t)index;
            }
        }
    }
    return 0xffu;
}

static void spectator_camera(
    c15_camera_t *camera,
    const c15_bot_t bots[BOT_COUNT],
    uint8_t target
)
{
    if (target >= BOT_COUNT || !bots[target].alive) {
        return;
    }
    c15_player_camera(&bots[target].mover, camera);
    camera->focal_length = DEFAULT_FOCAL_LENGTH;
}

static void bot_pickup_weapons(
    c15_bot_t bots[BOT_COUNT],
    c15_dropped_weapon_t dropped[DROPPED_WEAPON_MAX]
)
{
    uint32_t bot_index;
    for (bot_index = 0u; bot_index < BOT_COUNT; ++bot_index) {
        uint32_t drop_index;
        c15_bot_t *bot = &bots[bot_index];
        if (!bot->alive) continue;
        for (drop_index = 0u;
             drop_index < DROPPED_WEAPON_MAX; ++drop_index) {
            c15_dropped_weapon_t *item = &dropped[drop_index];
            if (!item->active ||
                g_weapon_slot[item->weapon] != WEAPON_SLOT_PRIMARY ||
                abs_i32(item->z - bot->mover.z) > 80u ||
                distance_squared(
                    item->x, item->y,
                    bot->mover.x, bot->mover.y
                ) > 64u * 64u) {
                continue;
            }
            {
                c15_dropped_weapon_t pickup = *item;
                item->active = 0u;
            drop_weapon(
                dropped, bot->weapon, bot->ammo,
                g_weapon_reserve_max[bot->weapon] / 2u,
                bot->mover.x, bot->mover.y, bot->mover.z,
                pickup.dropped_at + 1u
            );
            bot->weapon = pickup.weapon;
            bot->ammo = pickup.ammo != 0u ?
                pickup.ammo : g_weapon_capacity[pickup.weapon];
            }
            break;
        }
    }
}

static uint32_t dropped_weapon_count(
    const c15_dropped_weapon_t dropped[DROPPED_WEAPON_MAX]
)
{
    uint32_t index;
    uint32_t count = 0u;
    for (index = 0u; index < DROPPED_WEAPON_MAX; ++index) {
        if (dropped[index].active) ++count;
    }
    return count;
}

static void complete_reload(
    uint32_t weapon,
    uint16_t clips[WEAPON_COUNT],
    uint16_t reserve[WEAPON_COUNT]
)
{
    uint16_t need;
    uint16_t transfer;
    if (weapon == WEAPON_KNIFE ||
        clips[weapon] >= g_weapon_capacity[weapon]) {
        return;
    }
    need = (uint16_t)(g_weapon_capacity[weapon] - clips[weapon]);
    transfer = reserve[weapon] < need ? reserve[weapon] : need;
    clips[weapon] = (uint16_t)(clips[weapon] + transfer);
    reserve[weapon] = (uint16_t)(reserve[weapon] - transfer);
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
    lite_arena_reset(&g_animation_arena);
    g_persistent_animation_bytes = 0u;
    /*
     * The historical splash allocation is released before this function.
     * Clear the pointer so the in-game buy menu cannot sample the new map
     * texture allocation as if it were a 320x240 background image.
     */
    g_menu_background = 0;
    if (!c15_map_load(
            &g_map, &g_pak, g_map_assets[map_id],
            map_arena, texture_arena,
            g_map_texture_streaming[map_id],
            g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("load failed: map");
        lite_log_u32("map_load_error", g_map.load_error);
        lite_log_u32(
            "pak_seek_expected", g_pak.last_seek_expected
        );
        lite_log_i32("pak_seek_result", g_pak.last_seek_result);
        lite_log_u32(
            "pak_read_expected", g_pak.last_read_expected
        );
        lite_log_i32("pak_read_result", g_pak.last_read_result);
        lite_log_u32("map_arena_used", (uint32_t)map_arena->used);
        lite_log_u32(
            "map_arena_failures", (uint32_t)map_arena->failures
        );
        lite_log_u32(
            "texture_arena_used", (uint32_t)texture_arena->used
        );
        lite_log_u32(
            "texture_arena_failures",
            (uint32_t)texture_arena->failures
        );
        return 0;
    }
    if (!c15_model_load(
            &g_t_model, &g_pak, "mdl/player_terror",
            model_arena, g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("load failed: mdl/player_terror");
        return 0;
    }
    if (!c15_model_load(
            &g_ct_model, &g_pak, "mdl/player_urban",
            model_arena, g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("load failed: mdl/player_urban");
        return 0;
    }
    if (!c15_model_load(
            &g_t_weapon_model, &g_pak, "mdl/p_ak47",
            model_arena, g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("load failed: mdl/p_ak47");
        return 0;
    }
    if (!c15_model_load(
            &g_ct_weapon_model, &g_pak, "mdl/p_m4a1",
            model_arena, g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("load failed: mdl/p_m4a1");
        return 0;
    }
    if (!c15_model_locomotion_open(
            &g_t_locomotion, &g_t_model, &g_pak,
            "anim/player_terror",
            g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("load failed: anim/player_terror");
        return 0;
    }
    if (!c15_model_locomotion_open(
            &g_ct_locomotion, &g_ct_model, &g_pak,
            "anim/player_urban",
            g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("load failed: anim/player_urban");
        return 0;
    }
    if (!c15_model_locomotion_open(
            &g_t_weapon_locomotion, &g_t_weapon_model, &g_pak,
            "anim/p_ak47",
            g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("load failed: anim/p_ak47");
        return 0;
    }
    if (!c15_model_locomotion_open(
            &g_ct_weapon_locomotion, &g_ct_weapon_model, &g_pak,
            "anim/p_m4a1",
            g_load_scratch, sizeof(g_load_scratch))) {
        lite_log_line("load failed: anim/p_m4a1");
        return 0;
    }
    if (!c15_model_animation_make_resident(
            &g_t_locomotion, &g_pak, &g_animation_arena) ||
        !c15_model_animation_make_resident(
            &g_ct_locomotion, &g_pak, &g_animation_arena) ||
        !c15_model_animation_make_resident(
            &g_t_weapon_locomotion, &g_pak, &g_animation_arena) ||
        !c15_model_animation_make_resident(
            &g_ct_weapon_locomotion, &g_pak, &g_animation_arena)) {
        lite_log_line("load failed: resident world animations");
        lite_log_u32(
            "animation_arena_used",
            (uint32_t)g_animation_arena.used
        );
        lite_log_u32(
            "animation_arena_failures",
            g_animation_arena.failures
        );
        return 0;
    }
    g_persistent_animation_bytes = g_animation_arena.used;
    *persistent_models = model_arena->used;
    change_weapon(
        model_arena, *persistent_models, weapon,
        team == TEAM_T ? WEAPON_GLOCK : WEAPON_USP
    );
    if (!g_view_model.loaded || !g_view_animation.loaded ||
        !g_muzzle.loaded) {
        lite_log_line("load failed: initial view weapon");
        lite_log_u32(
            "model_arena_used", (uint32_t)model_arena->used
        );
        lite_log_u32(
            "model_arena_failures", (uint32_t)model_arena->failures
        );
    }
    return g_view_model.loaded && g_muzzle.loaded;
}

static void initialize_round(
    c15_player_t *player,
    c15_bot_t bots[BOT_COUNT],
    c15_bomb_state_t *bomb,
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
        uint8_t previous_kills = bots[index].kills;
        uint8_t previous_deaths = bots[index].deaths;
        uint8_t team = index < BOT_TEAMMATES ?
            player_team : enemy_team;
        uint32_t ordinal = index < BOT_TEAMMATES ?
            index + 1u : index - BOT_TEAMMATES;
        if (!find_team_spawn(&g_map, team, ordinal, &spawn)) {
            spawn = g_map.spawn;
        }
        bda_memset(&bots[index], 0, sizeof(bots[index]));
        bots[index].kills = previous_kills;
        bots[index].deaths = previous_deaths;
        c15_player_spawn(&bots[index].mover, &spawn);
        bots[index].team = team;
        if (team == TEAM_T) {
            static const uint8_t weapons[] = {
                WEAPON_AK47, WEAPON_SG552, WEAPON_AWP, WEAPON_M249
            };
            bots[index].weapon =
                weapons[index & 3u];
        } else {
            static const uint8_t weapons[] = {
                WEAPON_M4A1, WEAPON_AUG, WEAPON_MP5, WEAPON_SG550
            };
            bots[index].weapon =
                weapons[index & 3u];
        }
        bots[index].health = 100u;
        bots[index].ammo =
            g_weapon_capacity[bots[index].weapon];
        bots[index].armor = 100u;
        bots[index].helmet = 1u;
        bots[index].money = 800u;
        bots[index].grenade = (uint8_t)(index % GRENADE_KIND_COUNT);
        bots[index].nav_lane = (uint8_t)(index % 3u);
        bots[index].role = (uint8_t)(index % 3u);
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
        bots[index].next_visibility_check =
            now + (index & 1u) * 40u;
        bots[index].visibility_target = 0xffu;
        bots[index].strafe_right = (uint8_t)(index & 1u);
    }
    bda_memset(bomb, 0, sizeof(*bomb));
    bomb->enabled = (uint8_t)(
        (map_id == MAP_DE_DUST2 ||
         map_id == MAP_DE_INFERNO ||
         map_id == MAP_DE_NUKE) &&
        c15_map_bomb_site_count(&g_map) != 0u
    );
    if (bomb->enabled) {
        if (player_team == TEAM_T) {
            bomb->player_carrier = 1u;
        } else {
            for (index = 0u; index < BOT_COUNT; ++index) {
                if (bots[index].team == TEAM_T) {
                    bots[index].bomb_carrier = 1u;
                    break;
                }
            }
        }
    }
}

static void initialize_hostages(
    c15_hostage_t hostages[HOSTAGE_MAX]
)
{
    uint32_t count = c15_map_hostage_count(&g_map);
    uint32_t index;
    bda_memset(hostages, 0, sizeof(c15_hostage_t) * HOSTAGE_MAX);
    if (count > HOSTAGE_MAX) count = HOSTAGE_MAX;
    for (index = 0u; index < count; ++index) {
        c15_camera_t spawn;
        int32_t x;
        int32_t y;
        int32_t z;
        if (!c15_map_hostage(&g_map, index, &x, &y, &z)) {
            continue;
        }
        bda_memset(&spawn, 0, sizeof(spawn));
        spawn.x = x;
        spawn.y = y;
        spawn.z = z + 28;
        spawn.focal_length = DEFAULT_FOCAL_LENGTH;
        c15_player_spawn(&hostages[index].mover, &spawn);
        hostages[index].health = 100u;
        hostages[index].active = 1u;
        hostages[index].follower = HOSTAGE_FOLLOW_NONE;
    }
}

static uint32_t hostage_count_rescued(
    const c15_hostage_t hostages[HOSTAGE_MAX]
)
{
    uint32_t count = 0u;
    uint32_t index;
    for (index = 0u; index < HOSTAGE_MAX; ++index) {
        if (hostages[index].rescued) {
            ++count;
        }
    }
    return count;
}

static int clear_line(
    const c15_map_t *map,
    int32_t ax, int32_t ay, int32_t az,
    int32_t bx, int32_t by, int32_t bz
);
static void add_impact(
    c15_impact_t impacts[IMPACT_MAX],
    int32_t x, int32_t y, int32_t z,
    uint8_t kind, uint32_t now
);
static void add_corpse(
    c15_corpse_t corpses[CORPSE_MAX],
    int32_t x, int32_t y, int32_t z,
    uint8_t team, uint32_t now
);
static uint32_t apply_explosion_damage(
    uint32_t base, uint16_t *armor
);

static int hostage_player_use(
    c15_hostage_t hostages[HOSTAGE_MAX],
    const c15_player_t *player,
    uint8_t player_team,
    c15_audio_t *audio
)
{
    uint32_t nearest = 0xffffffffu;
    uint32_t selected = HOSTAGE_MAX;
    uint32_t index;
    if (player_team != TEAM_CT) {
        return 0;
    }
    for (index = 0u; index < HOSTAGE_MAX; ++index) {
        uint32_t distance;
        if (!hostages[index].active || hostages[index].rescued ||
            hostages[index].health == 0u) {
            continue;
        }
        distance = distance_squared(
            player->x, player->y,
            hostages[index].mover.x, hostages[index].mover.y
        );
        if (distance < nearest && distance <= 112u * 112u &&
            abs_i32(player->z - hostages[index].mover.z) <= 96u) {
            nearest = distance;
            selected = index;
        }
    }
    if (selected == HOSTAGE_MAX) {
        return 0;
    }
    hostages[selected].follower =
        hostages[selected].follower == HOSTAGE_FOLLOW_PLAYER ?
            HOSTAGE_FOLLOW_NONE : HOSTAGE_FOLLOW_PLAYER;
    c15_audio_play(
        audio, C15_SOUND_CUE_HOSTAGE, C15_SOUND_CHANNEL_BOT
    );
    return 1;
}

static void hostage_logic(
    c15_hostage_t hostages[HOSTAGE_MAX],
    c15_bot_t bots[BOT_COUNT],
    const c15_player_t *player,
    uint8_t player_team
)
{
    uint32_t index;
    for (index = 0u; index < HOSTAGE_MAX; ++index) {
        c15_hostage_t *hostage = &hostages[index];
        int32_t target_x;
        int32_t target_y;
        uint32_t distance;
        uint32_t controls = 0u;
        int has_follower = 0;
        if (!hostage->active || hostage->rescued ||
            hostage->health == 0u) {
            continue;
        }
        if (hostage->follower == HOSTAGE_FOLLOW_PLAYER) {
            if (player_team == TEAM_CT) {
                target_x = player->x;
                target_y = player->y;
                has_follower = 1;
            } else {
                hostage->follower = HOSTAGE_FOLLOW_NONE;
            }
        } else if (hostage->follower < BOT_COUNT) {
            c15_bot_t *bot = &bots[hostage->follower];
            if (bot->alive && bot->team == TEAM_CT) {
                target_x = bot->mover.x;
                target_y = bot->mover.y;
                has_follower = 1;
            } else {
                hostage->follower = HOSTAGE_FOLLOW_NONE;
            }
        }
        if (!has_follower) {
            uint32_t bot_index;
            uint32_t nearest = 96u * 96u + 1u;
            for (bot_index = 0u; bot_index < BOT_COUNT; ++bot_index) {
                uint32_t bot_distance;
                if (!bots[bot_index].alive ||
                    bots[bot_index].team != TEAM_CT) {
                    continue;
                }
                bot_distance = distance_squared(
                    hostage->mover.x, hostage->mover.y,
                    bots[bot_index].mover.x, bots[bot_index].mover.y
                );
                if (bot_distance < nearest) {
                    nearest = bot_distance;
                    hostage->follower = (uint8_t)bot_index;
                }
            }
            continue;
        }
        distance = distance_squared(
            hostage->mover.x, hostage->mover.y, target_x, target_y
        );
        if (distance > 56u * 56u) {
            hostage->mover.yaw = yaw_from_delta(
                target_x - hostage->mover.x,
                target_y - hostage->mover.y
            );
            hostage->mover.yaw_q8 =
                (uint16_t)((uint16_t)hostage->mover.yaw << 8);
            controls = C15_MOVE_FORWARD;
        }
        {
            int32_t before_x = hostage->mover.x;
            int32_t before_y = hostage->mover.y;
            c15_player_step_speed(
                &hostage->mover, &g_map, controls, 3u
            );
            hostage->moving = (uint8_t)(
                hostage->mover.x != before_x ||
                hostage->mover.y != before_y
            );
        }
        if (c15_map_in_rescue_zone(
                &g_map,
                hostage->mover.x, hostage->mover.y,
                hostage->mover.z)) {
            hostage->rescued = 1u;
            hostage->active = 0u;
        }
    }
}

static int throw_grenade(
    c15_grenade_t grenades[GRENADE_MAX],
    uint8_t kind,
    uint8_t owner_team,
    uint8_t owner,
    const c15_camera_t *camera,
    uint32_t now
)
{
    uint32_t index;
    int32_t yaw_sine = sin_q14_q8(camera->yaw_q8);
    int32_t yaw_cosine = cos_q14_q8(camera->yaw_q8);
    int32_t pitch_sine = sin_q14_q8((uint16_t)camera->pitch_q8);
    int32_t pitch_cosine = cos_q14_q8((uint16_t)camera->pitch_q8);
    for (index = 0u; index < GRENADE_MAX; ++index) {
        c15_grenade_t *grenade = &grenades[index];
        int32_t forward_x;
        int32_t forward_y;
        if (grenade->active != 0u) {
            continue;
        }
        forward_x = (int32_t)(
            ((int64_t)yaw_cosine * pitch_cosine) >> 14
        );
        forward_y = (int32_t)(
            ((int64_t)yaw_sine * pitch_cosine) >> 14
        );
        bda_memset(grenade, 0, sizeof(*grenade));
        grenade->x_q8 = camera->x << 8;
        grenade->y_q8 = camera->y << 8;
        grenade->z_q8 = (camera->z - 6) << 8;
        grenade->vx_q8 = (int32_t)(
            ((int64_t)forward_x * 14 * 256) >> 14
        );
        grenade->vy_q8 = (int32_t)(
            ((int64_t)forward_y * 14 * 256) >> 14
        );
        grenade->vz_q8 = 1500 +
            (int32_t)(((int64_t)pitch_sine * 7 * 256) >> 14);
        grenade->detonate_at = now +
            (kind == GRENADE_SMOKE ? 1800u : 1500u);
        grenade->kind = kind;
        grenade->owner_team = owner_team;
        grenade->owner = owner;
        grenade->active = 1u;
        return 1;
    }
    return 0;
}

static int line_crosses_smoke(
    const c15_grenade_t grenades[GRENADE_MAX],
    int32_t ax,
    int32_t ay,
    int32_t bx,
    int32_t by
)
{
    uint32_t grenade_index;
    for (grenade_index = 0u;
         grenade_index < GRENADE_MAX; ++grenade_index) {
        const c15_grenade_t *grenade = &grenades[grenade_index];
        uint32_t step;
        int32_t smoke_x;
        int32_t smoke_y;
        if (grenade->active != 2u) {
            continue;
        }
        smoke_x = grenade->x_q8 >> 8;
        smoke_y = grenade->y_q8 >> 8;
        for (step = 1u; step < 8u; ++step) {
            int32_t x = ax + (bx - ax) * (int32_t)step / 8;
            int32_t y = ay + (by - ay) * (int32_t)step / 8;
            if (distance_squared(x, y, smoke_x, smoke_y) <=
                    128u * 128u) {
                return 1;
            }
        }
    }
    return 0;
}

static void grenade_logic(
    c15_grenade_t grenades[GRENADE_MAX],
    c15_bot_t bots[BOT_COUNT],
    c15_hostage_t hostages[HOSTAGE_MAX],
    uint8_t player_team,
    uint16_t *player_health,
    uint16_t *player_armor,
    uint8_t player_helmet,
    const c15_player_t *player,
    uint32_t *flash_until,
    c15_corpse_t corpses[CORPSE_MAX],
    c15_impact_t impacts[IMPACT_MAX],
    c15_kill_feed_t *kill_feed,
    uint32_t *money,
    uint32_t *player_kills,
    uint32_t *player_deaths,
    c15_audio_t *audio,
    uint32_t now
)
{
    uint32_t index;
    (void)player_helmet;
    for (index = 0u; index < GRENADE_MAX; ++index) {
        c15_grenade_t *grenade = &grenades[index];
        int32_t x;
        int32_t y;
        int32_t z;
        if (grenade->active == 0u) {
            continue;
        }
        if (grenade->active == 2u) {
            if (time_reached(now, grenade->detonate_at)) {
                grenade->active = 0u;
            }
            continue;
        }
        grenade->vz_q8 -= 180;
        {
            int32_t next_x_q8 = grenade->x_q8 + grenade->vx_q8;
            int32_t next_y_q8 = grenade->y_q8 + grenade->vy_q8;
            int32_t next_z_q8 = grenade->z_q8 + grenade->vz_q8;
            int32_t next_x = next_x_q8 >> 8;
            int32_t next_y = next_y_q8 >> 8;
            int32_t next_z = next_z_q8 >> 8;
            if (c15_map_hull_contents(
                    &g_map, 0u, next_x, next_y, next_z) == -2) {
                grenade->vx_q8 = -grenade->vx_q8 / 2;
                grenade->vy_q8 = -grenade->vy_q8 / 2;
                grenade->vz_q8 =
                    abs_i32(grenade->vz_q8) > 256u ?
                        (int32_t)abs_i32(grenade->vz_q8) / 2 : 0;
                if (!grenade->bounced) {
                    c15_audio_play(
                        audio, C15_SOUND_CUE_GRENADE_BOUNCE,
                        C15_SOUND_CHANNEL_BOT
                    );
                    grenade->bounced = 1u;
                }
            } else {
                grenade->x_q8 = next_x_q8;
                grenade->y_q8 = next_y_q8;
                grenade->z_q8 = next_z_q8;
            }
        }
        if (!time_reached(now, grenade->detonate_at)) {
            continue;
        }
        x = grenade->x_q8 >> 8;
        y = grenade->y_q8 >> 8;
        z = grenade->z_q8 >> 8;
        if (grenade->kind == GRENADE_SMOKE) {
            grenade->active = 2u;
            grenade->detonate_at = now + 15000u;
            grenade->vx_q8 = grenade->vy_q8 = grenade->vz_q8 = 0;
            c15_audio_play(
                audio, C15_SOUND_CUE_SMOKE, C15_SOUND_CHANNEL_BOT
            );
            continue;
        }
        add_impact(impacts, x, y, z, 2u, now);
        if (grenade->kind == GRENADE_FLASH) {
            uint32_t bot_index;
            uint32_t distance = distance_squared(
                x, y, player->x, player->y
            );
            if (distance <= 900u * 900u &&
                clear_line(
                    &g_map, x, y, z,
                    player->x, player->y, player->z + 28)) {
                uint32_t duration =
                    4500u - (distance / (900u * 900u / 3500u));
                if (duration < 800u) duration = 800u;
                *flash_until = now + duration;
            }
            for (bot_index = 0u;
                 bot_index < BOT_COUNT; ++bot_index) {
                c15_bot_t *bot = &bots[bot_index];
                distance = distance_squared(
                    x, y, bot->mover.x, bot->mover.y
                );
                if (bot->alive && distance <= 900u * 900u &&
                    clear_line(
                        &g_map, x, y, z,
                        bot->mover.x, bot->mover.y,
                        bot->mover.z + 28)) {
                    uint32_t duration =
                        3500u -
                        (distance / (900u * 900u / 2800u));
                    if (duration < 500u) duration = 500u;
                    bot->flash_until = now + duration;
                }
            }
            c15_audio_play(
                audio, C15_SOUND_CUE_FLASH_EXPLODE,
                C15_SOUND_CHANNEL_BOT
            );
            grenade->active = 0u;
            continue;
        }
        c15_audio_play(
            audio, C15_SOUND_CUE_HE_EXPLODE, C15_SOUND_CHANNEL_BOT
        );
        {
            uint32_t bot_index;
            for (bot_index = 0u; bot_index < BOT_COUNT; ++bot_index) {
                c15_bot_t *bot = &bots[bot_index];
                uint32_t distance;
                uint32_t damage;
                if (!bot->alive ||
                    (bot->team == grenade->owner_team &&
                     grenade->owner != bot_index + 1u)) {
                    continue;
                }
                distance = distance_squared(
                    x, y, bot->mover.x, bot->mover.y
                );
                if (distance > 360u * 360u ||
                    !clear_line(
                        &g_map, x, y, z,
                        bot->mover.x, bot->mover.y,
                        bot->mover.z + 28)) {
                    continue;
                }
                damage = 110u -
                    (distance * 90u) / (360u * 360u);
                damage = apply_explosion_damage(
                    damage, &bot->armor
                );
                if (bot->health <= damage) {
                    bot->health = 0u;
                    bot->alive = 0u;
                    ++bot->deaths;
                    add_corpse(
                        corpses, bot->mover.x, bot->mover.y,
                        bot->mover.z, bot->team, now
                    );
                    kill_feed->killer = grenade->owner;
                    kill_feed->victim = (uint8_t)(bot_index + 1u);
                    kill_feed->weapon = 0xffu;
                    kill_feed->headshot = 0u;
                    kill_feed->until = now + 3500u;
                    if (grenade->owner == 0u) {
                        ++*player_kills;
                        *money += 300u;
                        if (*money > 16000u) *money = 16000u;
                    } else if (grenade->owner <= BOT_COUNT) {
                        ++bots[grenade->owner - 1u].kills;
                    }
                } else {
                    bot->health = (uint16_t)(bot->health - damage);
                }
            }
        }
        if (grenade->owner == 0u ||
            grenade->owner_team != player_team) {
            uint32_t distance = distance_squared(
                x, y, player->x, player->y
            );
            if (distance <= 360u * 360u &&
                clear_line(
                    &g_map, x, y, z,
                    player->x, player->y, player->z + 28)) {
                uint32_t damage = 110u -
                    (distance * 90u) / (360u * 360u);
                damage = apply_explosion_damage(
                    damage, player_armor
                );
                if (*player_health <= damage) {
                    *player_health = 0u;
                    add_corpse(
                        corpses, player->x, player->y,
                        player->z, player_team, now
                    );
                    kill_feed->killer = grenade->owner;
                    kill_feed->victim = 0u;
                    kill_feed->weapon = 0xffu;
                    kill_feed->headshot = 0u;
                    kill_feed->until = now + 3500u;
                    ++*player_deaths;
                    if (grenade->owner != 0u &&
                        grenade->owner <= BOT_COUNT) {
                        ++bots[grenade->owner - 1u].kills;
                    }
                    c15_audio_play(
                        audio, C15_SOUND_CUE_DEATH,
                        C15_SOUND_CHANNEL_PLAYER
                    );
                } else {
                    *player_health =
                        (uint16_t)(*player_health - damage);
                }
            }
        }
        {
            uint32_t hostage_index;
            for (hostage_index = 0u;
                 hostage_index < HOSTAGE_MAX; ++hostage_index) {
                c15_hostage_t *hostage = &hostages[hostage_index];
                uint32_t distance;
                uint32_t damage;
                if (!hostage->active || hostage->rescued) {
                    continue;
                }
                distance = distance_squared(
                    x, y, hostage->mover.x, hostage->mover.y
                );
                if (distance > 360u * 360u) continue;
                damage = 110u -
                    (distance * 90u) / (360u * 360u);
                if (hostage->health <= damage) {
                    hostage->health = 0u;
                    hostage->active = 0u;
                    add_corpse(
                        corpses,
                        hostage->mover.x, hostage->mover.y,
                        hostage->mover.z, 0u, now
                    );
                    if (grenade->owner == 0u) {
                        if (*money > 2250u) *money -= 2250u;
                        else *money = 0u;
                    }
                } else {
                    hostage->health =
                        (uint16_t)(hostage->health - damage);
                }
            }
        }
        grenade->active = 0u;
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

static uint16_t next_random(uint16_t *seed)
{
    *seed = (uint16_t)(*seed * 25173u + 13849u);
    return *seed;
}

static void add_impact(
    c15_impact_t impacts[IMPACT_MAX],
    int32_t x,
    int32_t y,
    int32_t z,
    uint8_t kind,
    uint32_t now
)
{
    uint32_t index;
    uint32_t selected = 0u;
    for (index = 0u; index < IMPACT_MAX; ++index) {
        if (!impacts[index].active ||
            time_reached(now, impacts[index].until)) {
            selected = index;
            break;
        }
        if (impacts[index].until < impacts[selected].until) {
            selected = index;
        }
    }
    impacts[selected].x = x;
    impacts[selected].y = y;
    impacts[selected].z = z;
    impacts[selected].kind = kind;
    impacts[selected].until = now + (
        kind == 0u ? 5000u : (kind == 1u ? 700u : 900u)
    );
    impacts[selected].active = 1u;
}

static void add_corpse(
    c15_corpse_t corpses[CORPSE_MAX],
    int32_t x,
    int32_t y,
    int32_t z,
    uint8_t team,
    uint32_t now
)
{
    uint32_t index;
    uint32_t selected = 0u;
    for (index = 0u; index < CORPSE_MAX; ++index) {
        if (!corpses[index].active) {
            selected = index;
            break;
        }
        if (corpses[index].died_at < corpses[selected].died_at) {
            selected = index;
        }
    }
    corpses[selected].x = x;
    corpses[selected].y = y;
    corpses[selected].z = z;
    corpses[selected].team = team;
    corpses[selected].died_at = now;
    corpses[selected].active = 1u;
}

static uint32_t apply_explosion_damage(
    uint32_t base,
    uint16_t *armor
)
{
    uint32_t damage = base;
    if (armor && *armor != 0u) {
        uint32_t absorbed = (damage * 2u + 4u) / 5u;
        if (absorbed > *armor) {
            absorbed = *armor;
        }
        *armor = (uint16_t)(*armor - absorbed);
        damage -= absorbed;
    }
    return damage == 0u ? 1u : damage;
}

static int bullet_line_clear(
    c15_map_t *map,
    int32_t ax,
    int32_t ay,
    int32_t az,
    int32_t bx,
    int32_t by,
    int32_t bz,
    uint8_t penetration,
    int damage_breakables,
    int32_t *impact_x,
    int32_t *impact_y,
    int32_t *impact_z
)
{
    uint32_t span = abs_i32(bx - ax);
    uint32_t segments;
    uint32_t step;
    uint32_t layers = 0u;
    int in_solid = 0;
    if (abs_i32(by - ay) > span) span = abs_i32(by - ay);
    if (abs_i32(bz - az) > span) span = abs_i32(bz - az);
    segments = span / 24u + 1u;
    if (segments < 2u) segments = 2u;
    if (segments > 96u) segments = 96u;
    for (step = 1u; step <= segments; ++step) {
        int32_t x = ax + (bx - ax) * (int32_t)step /
            (int32_t)segments;
        int32_t y = ay + (by - ay) * (int32_t)step /
            (int32_t)segments;
        int32_t z = az + (bz - az) * (int32_t)step /
            (int32_t)segments;
        int solid = c15_map_hull_contents(map, 0u, x, y, z) == -2;
        if (damage_breakables && solid && !in_solid &&
            c15_map_damage_breakable(map, x, y, z)) {
            solid = 0;
        }
        if (solid && !in_solid) {
            ++layers;
            if (layers > penetration) {
                if (impact_x) *impact_x = x;
                if (impact_y) *impact_y = y;
                if (impact_z) *impact_z = z;
                return 0;
            }
        }
        in_solid = solid;
    }
    return 1;
}

static int nearest_bomb_site(
    const c15_map_t *map,
    int32_t x,
    int32_t y,
    uint8_t *site_out,
    int32_t *x_out,
    int32_t *y_out,
    int32_t *z_out
)
{
    uint32_t count = c15_map_bomb_site_count(map);
    uint32_t nearest = 0xffffffffu;
    uint32_t index;
    int found = 0;
    for (index = 0u; index < count; ++index) {
        int32_t site_x;
        int32_t site_y;
        int32_t site_z;
        uint32_t distance;
        if (!c15_map_bomb_site(
                map, index, &site_x, &site_y, &site_z)) {
            continue;
        }
        distance = distance_squared(x, y, site_x, site_y);
        if (distance < nearest) {
            nearest = distance;
            *site_out = (uint8_t)index;
            *x_out = site_x;
            *y_out = site_y;
            *z_out = site_z;
            found = 1;
        }
    }
    return found;
}

static void bot_logic(
    c15_bot_t bots[BOT_COUNT],
    c15_grenade_t grenades[GRENADE_MAX],
    c15_hostage_t hostages[HOSTAGE_MAX],
    c15_player_t *player,
    uint8_t player_team,
    uint16_t *player_health,
    uint16_t *player_armor,
    uint8_t player_helmet,
    uint8_t difficulty,
    uint32_t map_id,
    const c15_bomb_state_t *bomb,
    uint32_t now,
    uint32_t *shots,
    uint32_t *hits,
    uint32_t *visibility_traces,
    uint32_t *sound_weapon,
    uint32_t *player_deaths,
    c15_corpse_t corpses[CORPSE_MAX],
    c15_kill_feed_t *kill_feed
)
{
    uint32_t route_count = map_route_count(map_id);
    uint32_t index;
    for (index = 0u; index < BOT_COUNT; ++index) {
        c15_bot_t *bot = &bots[index];
        uint32_t nearest = 0xffffffffu;
        uint32_t nearest_friend = 0xffffffffu;
        int nearest_friend_index = -1;
        int target = -2;
        int32_t target_x = 0;
        int32_t target_y = 0;
        int32_t target_z = 0;
        int32_t move_x;
        int32_t move_y;
        uint32_t move_distance;
        uint32_t controls = 0u;
        int target_visible;
        int visibility_candidate;
        uint8_t visibility_target;
        int32_t nav_x = 0;
        int32_t nav_y = 0;
        int32_t nav_z = 0;
        int has_nav = 0;
        uint8_t target_yaw;
        uint32_t other;
        uint32_t before_blocked;
        int32_t before_x;
        int32_t before_y;
        int objective_active = 0;
        int32_t objective_x = 0;
        int32_t objective_y = 0;
        int returning_hostage = 0;
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
                    nearest_friend_index = (int)other;
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
        visibility_candidate =
            nearest < BOT_SIGHT_DISTANCE * BOT_SIGHT_DISTANCE &&
            abs_i32(bot->mover.z - target_z) < 160u &&
            (nearest < BOT_CLOSE_AWARE_DISTANCE *
                       BOT_CLOSE_AWARE_DISTANCE ||
             yaw_distance(target_yaw, bot->mover.yaw) <= 56u) &&
            !time_active(now, bot->flash_until);
        visibility_target = target < 0 ?
            0u : (uint8_t)(target + 1);
        if (!visibility_candidate) {
            bot->visibility_cached = 0u;
            bot->visibility_target = visibility_target;
        } else if (bot->visibility_target != visibility_target ||
                   time_reached(now, bot->next_visibility_check)) {
            bot->visibility_cached = (uint8_t)(
                clear_line(
                    &g_map,
                    bot->mover.x, bot->mover.y, bot->mover.z + 28,
                    target_x, target_y, target_z + 28
                ) &&
                !line_crosses_smoke(
                    grenades,
                    bot->mover.x, bot->mover.y, target_x, target_y
                )
            );
            bot->visibility_target = visibility_target;
            bot->next_visibility_check =
                now + 80u + (index & 1u) * 20u;
            ++*visibility_traces;
        }
        target_visible = bot->visibility_cached != 0u;
        if (target_visible) {
            bot->last_enemy_x = target_x;
            bot->last_enemy_y = target_y;
            bot->last_enemy_seen = now;
        }
        if (bomb && bomb->enabled) {
            if (bomb->planted && bot->team == TEAM_CT) {
                objective_active = 1;
                objective_x = bomb->x;
                objective_y = bomb->y;
            } else if (bomb->dropped && bot->team == TEAM_T) {
                objective_active = 1;
                objective_x = bomb->x;
                objective_y = bomb->y;
            } else if (!bomb->planted && bot->bomb_carrier &&
                       bot->team == TEAM_T &&
                       bot->nav_index == 0u) {
                uint8_t objective_site = 0u;
                int32_t objective_z;
                objective_active = nearest_bomb_site(
                    &g_map, bot->mover.x, bot->mover.y,
                    &objective_site, &objective_x,
                    &objective_y, &objective_z
                );
                (void)objective_site;
                (void)objective_z;
            }
        } else if (bot->team == TEAM_CT) {
            for (other = 0u; other < HOSTAGE_MAX; ++other) {
                if (hostages[other].active &&
                    hostages[other].follower == index) {
                    returning_hostage = 1;
                    break;
                }
            }
        }
        move_x = target_x;
        move_y = target_y;
        if (!target_visible) {
            uint8_t previous_nav_index = bot->nav_index;
            int pursuing_last_position =
                bot->last_enemy_seen != 0u &&
                time_active(now, bot->last_enemy_seen + 2200u);
            int supporting_friend =
                bot->role == BOT_ROLE_SUPPORT &&
                nearest_friend_index >= 0 &&
                nearest_friend > 220u * 220u;
            if (!objective_active && !pursuing_last_position &&
                !supporting_friend) {
                has_nav = map_route_lane_point(
                    map_id, bot->nav_index, bot->nav_lane,
                    &nav_x, &nav_y, &nav_z
                );
            }
            if (has_nav &&
                distance_squared(
                    bot->mover.x, bot->mover.y,
                    nav_x, nav_y) < 32u * 32u &&
                abs_i32(bot->mover.z - nav_z) < 32u) {
                if (returning_hostage &&
                    bot->nav_index != 0u) {
                    --bot->nav_index;
                } else if (bot->team == TEAM_CT &&
                    bot->nav_index + 1u < route_count) {
                    ++bot->nav_index;
                } else if (bot->team == TEAM_T &&
                           bot->nav_index != 0u) {
                    --bot->nav_index;
                }
                has_nav = map_route_lane_point(
                    map_id, bot->nav_index, bot->nav_lane,
                    &nav_x, &nav_y, &nav_z
                );
            }
            if (bot->nav_index != previous_nav_index) {
                bot->nav_stalls = 0u;
            }
            if (has_nav) {
                move_x = nav_x;
                move_y = nav_y;
            } else if (pursuing_last_position) {
                move_x = bot->last_enemy_x;
                move_y = bot->last_enemy_y;
            } else if (objective_active) {
                move_x = objective_x;
                move_y = objective_y;
            } else if (supporting_friend) {
                move_x = bots[(uint32_t)nearest_friend_index].mover.x;
                move_y = bots[(uint32_t)nearest_friend_index].mover.y;
            }
            bot->mover.yaw = yaw_from_delta(
                move_x - bot->mover.x, move_y - bot->mover.y
            );
            controls = C15_MOVE_FORWARD;
            if (!objective_active &&
                bot->role == BOT_ROLE_ANCHOR &&
                ((bot->team == TEAM_CT &&
                  bot->nav_index >= route_count / 2u) ||
                 (bot->team == TEAM_T &&
                  bot->nav_index <= route_count / 2u))) {
                controls = 0u;
            }
            bot->combat = 0u;
        } else {
            uint32_t hold_min = BOT_HOLD_MIN_DISTANCE;
            uint32_t hold_max = BOT_HOLD_MAX_DISTANCE;
            bot->mover.yaw = target_yaw;
            bot->combat = 1u;
            if (bot->role == BOT_ROLE_ENTRY) {
                hold_min = 170u;
                hold_max = 430u;
            } else if (bot->role == BOT_ROLE_SUPPORT) {
                hold_min = 300u;
                hold_max = 680u;
            } else {
                hold_min = 380u;
                hold_max = 760u;
            }
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
            if (bot->health < 30u) {
                controls = C15_MOVE_BACK |
                    (bot->strafe_right ?
                        C15_MOVE_RIGHT : C15_MOVE_LEFT);
            } else if (nearest > hold_max * hold_max) {
                controls = C15_MOVE_FORWARD;
            } else if (nearest < hold_min * hold_min) {
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
            if (bot->stuck_turn == 0u) {
                (void)c15_map_use_dynamic(
                    &g_map, bot->mover.x, bot->mover.y,
                    bot->mover.z + 28
                );
            }
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
        if (target_visible && bot->grenade < GRENADE_KIND_COUNT &&
            nearest > 360u * 360u && nearest < 900u * 900u &&
            time_reached(now, bot->next_decision)) {
            c15_camera_t grenade_camera;
            bda_memset(&grenade_camera, 0, sizeof(grenade_camera));
            grenade_camera.x = bot->mover.x;
            grenade_camera.y = bot->mover.y;
            grenade_camera.z = bot->mover.z + 28;
            grenade_camera.yaw = bot->mover.yaw;
            grenade_camera.yaw_q8 = bot->mover.yaw_q8;
            grenade_camera.focal_length = DEFAULT_FOCAL_LENGTH;
            if (throw_grenade(
                    grenades, bot->grenade, bot->team,
                    (uint8_t)(index + 1u),
                    &grenade_camera, now)) {
                bot->grenade = 0xffu;
                bot->next_decision = now + 2500u;
            }
        }
        if (target_visible &&
            time_reached(now, bot->next_fire) &&
            nearest < BOT_SIGHT_DISTANCE * BOT_SIGHT_DISTANCE) {
            uint16_t *health = target < 0 ?
                player_health : &bots[(uint32_t)target].health;
            uint16_t *armor = target < 0 ?
                player_armor : &bots[(uint32_t)target].armor;
            uint8_t helmet = target < 0 ?
                player_helmet : bots[(uint32_t)target].helmet;
            if (bot->ammo == 0u) {
                bot->ammo = g_weapon_capacity[bot->weapon];
                bot->next_fire = now + 1900u;
                bot->burst_left = 0u;
                continue;
            }
            --bot->ammo;
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
                {
                    uint8_t hitgroup =
                        (bot->aim_seed & 15u) == 0u ? 1u : 2u;
                    uint32_t damage = c15_damage_apply_bullet(
                        g_bot_damage[difficulty], hitgroup,
                        armor, helmet,
                        g_weapon_armor_ratio_q8[bot->weapon]
                    );
                if (*health <= damage) {
                    *health = 0u;
                    ++bot->kills;
                    if (target >= 0) {
                        bots[(uint32_t)target].alive = 0u;
                        ++bots[(uint32_t)target].deaths;
                        add_corpse(
                            corpses,
                            bots[(uint32_t)target].mover.x,
                            bots[(uint32_t)target].mover.y,
                            bots[(uint32_t)target].mover.z,
                            bots[(uint32_t)target].team, now
                        );
                    } else {
                        ++*player_deaths;
                        add_corpse(
                            corpses, player->x, player->y,
                            player->z, player_team, now
                        );
                    }
                    kill_feed->killer = (uint8_t)(index + 1u);
                    kill_feed->victim = target < 0 ?
                        0u : (uint8_t)(target + 1);
                    kill_feed->weapon = bot->weapon;
                    kill_feed->headshot = hitgroup == 1u;
                    kill_feed->until = now + 3500u;
                } else {
                    *health = (uint16_t)(
                        *health - damage
                    );
                }
                }
            }
        }
    }
}

static int bomb_near(
    int32_t ax, int32_t ay, int32_t az,
    int32_t bx, int32_t by, int32_t bz
)
{
    return distance_squared(ax, ay, bx, by) <=
            BOMB_USE_DISTANCE * BOMB_USE_DISTANCE &&
        abs_i32(az - bz) <= 128u;
}

static void bomb_complete_plant(
    c15_bomb_state_t *bomb,
    int32_t x,
    int32_t y,
    int32_t z,
    uint32_t now,
    c15_audio_t *audio
)
{
    bomb->x = x;
    bomb->y = y;
    bomb->z = z;
    bomb->planted = 1u;
    bomb->player_carrier = 0u;
    bomb->dropped = 0u;
    bomb->action = BOMB_ACTION_NONE;
    bomb->action_owner = 0u;
    bomb->explode_at = now + BOMB_TIME_MS;
    bomb->next_beep = now;
    c15_audio_play(
        audio, C15_SOUND_CUE_BOMB_PLANT,
        C15_SOUND_CHANNEL_PLAYER
    );
}

static void bomb_player_update(
    c15_bomb_state_t *bomb,
    const c15_player_t *player,
    uint8_t player_team,
    uint16_t player_health,
    uint8_t has_defuse_kit,
    const lite_input_t *input,
    uint32_t now,
    c15_audio_t *audio
)
{
    uint8_t site = 0u;
    int32_t site_x = 0;
    int32_t site_y = 0;
    int32_t site_z = 0;
    uint8_t wanted_action = BOMB_ACTION_NONE;
    uint32_t duration = 0u;
    if (!bomb->enabled || player_health == 0u) {
        if (bomb->action_owner == 1u) {
            bomb->action = BOMB_ACTION_NONE;
            bomb->action_owner = 0u;
        }
        return;
    }
    if (bomb->dropped && player_team == TEAM_T &&
        bomb_near(
            player->x, player->y, player->z,
            bomb->x, bomb->y, bomb->z)) {
        bomb->dropped = 0u;
        bomb->player_carrier = 1u;
    }
    if (bomb->planted && !bomb->defused &&
        player_team == TEAM_CT &&
        bomb_near(
            player->x, player->y, player->z,
            bomb->x, bomb->y, bomb->z)) {
        wanted_action = BOMB_ACTION_DEFUSE;
        duration = has_defuse_kit ? BOMB_DEFUSE_MS / 2u :
            BOMB_DEFUSE_MS;
    } else if (!bomb->planted && bomb->player_carrier &&
               player_team == TEAM_T &&
               nearest_bomb_site(
                   &g_map, player->x, player->y,
                   &site, &site_x, &site_y, &site_z) &&
               bomb_near(
                   player->x, player->y, player->z,
                   site_x, site_y, site_z)) {
        wanted_action = BOMB_ACTION_PLANT;
        duration = BOMB_PLANT_MS;
    }
    if (wanted_action == BOMB_ACTION_NONE ||
        (input->down & LITE_INPUT_USE) == 0u) {
        if (bomb->action_owner == 1u) {
            bomb->action = BOMB_ACTION_NONE;
            bomb->action_owner = 0u;
        }
        return;
    }
    if (bomb->action != wanted_action || bomb->action_owner != 1u) {
        bomb->action = wanted_action;
        bomb->action_owner = 1u;
        bomb->action_started = now;
        if (wanted_action == BOMB_ACTION_DEFUSE) {
            c15_audio_play(
                audio, C15_SOUND_CUE_BOMB_DISARM,
                C15_SOUND_CHANNEL_PLAYER
            );
        }
        return;
    }
    if (time_reached(now, bomb->action_started + duration)) {
        if (wanted_action == BOMB_ACTION_PLANT) {
            bomb->site = site;
            bomb_complete_plant(
                bomb, site_x, site_y, site_z, now, audio
            );
        } else {
            bomb->defused = 1u;
            bomb->action = BOMB_ACTION_NONE;
            bomb->action_owner = 0u;
            c15_audio_play(
                audio, C15_SOUND_CUE_BOMB_DISARMED,
                C15_SOUND_CHANNEL_PLAYER
            );
        }
    }
}

static void bomb_bot_update(
    c15_bomb_state_t *bomb,
    c15_bot_t bots[BOT_COUNT],
    const c15_player_t *player,
    uint8_t player_team,
    uint16_t player_health,
    uint32_t now,
    c15_audio_t *audio
)
{
    uint32_t index;
    if (!bomb->enabled || bomb->defused) {
        return;
    }
    if (bomb->player_carrier && player_team == TEAM_T &&
        player_health == 0u) {
        bomb->player_carrier = 0u;
        bomb->dropped = 1u;
        bomb->x = player->x;
        bomb->y = player->y;
        bomb->z = player->z;
    }
    if (!bomb->planted && !bomb->player_carrier) {
        c15_bot_t *carrier = 0;
        for (index = 0u; index < BOT_COUNT; ++index) {
            if (bots[index].bomb_carrier && bots[index].alive) {
                carrier = &bots[index];
                break;
            }
            if (bots[index].bomb_carrier && !bots[index].alive) {
                bomb->dropped = 1u;
                bomb->x = bots[index].mover.x;
                bomb->y = bots[index].mover.y;
                bomb->z = bots[index].mover.z;
            }
            bots[index].bomb_carrier = 0u;
        }
        if (!carrier && bomb->dropped) {
            for (index = 0u; index < BOT_COUNT; ++index) {
                if (bots[index].alive &&
                    bots[index].team == TEAM_T &&
                    bomb_near(
                        bots[index].mover.x, bots[index].mover.y,
                        bots[index].mover.z,
                        bomb->x, bomb->y, bomb->z)) {
                    bots[index].bomb_carrier = 1u;
                    carrier = &bots[index];
                    bomb->dropped = 0u;
                    break;
                }
            }
        }
        if (carrier) {
            uint8_t site = 0u;
            int32_t site_x;
            int32_t site_y;
            int32_t site_z;
            if (nearest_bomb_site(
                    &g_map, carrier->mover.x, carrier->mover.y,
                    &site, &site_x, &site_y, &site_z) &&
                bomb_near(
                    carrier->mover.x, carrier->mover.y,
                    carrier->mover.z, site_x, site_y, site_z)) {
                if (bomb->action != BOMB_ACTION_PLANT ||
                    bomb->action_owner != 2u) {
                    bomb->action = BOMB_ACTION_PLANT;
                    bomb->action_owner = 2u;
                    bomb->action_started = now;
                } else if (time_reached(
                               now,
                               bomb->action_started + BOMB_PLANT_MS)) {
                    bomb->site = site;
                    carrier->bomb_carrier = 0u;
                    bomb_complete_plant(
                        bomb, site_x, site_y, site_z, now, audio
                    );
                }
            } else if (bomb->action_owner == 2u) {
                bomb->action = BOMB_ACTION_NONE;
                bomb->action_owner = 0u;
            }
        }
    } else if (bomb->planted) {
        c15_bot_t *defuser = 0;
        for (index = 0u; index < BOT_COUNT; ++index) {
            if (bots[index].alive && bots[index].team == TEAM_CT &&
                bomb_near(
                    bots[index].mover.x, bots[index].mover.y,
                    bots[index].mover.z,
                    bomb->x, bomb->y, bomb->z)) {
                defuser = &bots[index];
                break;
            }
        }
        if (defuser && bomb->action_owner != 1u) {
            if (bomb->action != BOMB_ACTION_DEFUSE ||
                bomb->action_owner != 2u) {
                bomb->action = BOMB_ACTION_DEFUSE;
                bomb->action_owner = 2u;
                bomb->action_started = now;
            } else if (time_reached(
                           now,
                           bomb->action_started + BOMB_DEFUSE_MS / 2u)) {
                bomb->defused = 1u;
                bomb->action = BOMB_ACTION_NONE;
                bomb->action_owner = 0u;
                c15_audio_play(
                    audio, C15_SOUND_CUE_BOMB_DISARMED,
                    C15_SOUND_CHANNEL_BOT
                );
            }
        } else if (bomb->action_owner == 2u) {
            bomb->action = BOMB_ACTION_NONE;
            bomb->action_owner = 0u;
        }
    }
}

static void bomb_apply_explosion(
    const c15_bomb_state_t *bomb,
    c15_bot_t bots[BOT_COUNT],
    c15_hostage_t hostages[HOSTAGE_MAX],
    const c15_player_t *player,
    uint16_t *player_health,
    uint32_t *player_deaths,
    c15_corpse_t corpses[CORPSE_MAX],
    c15_kill_feed_t *kill_feed,
    uint32_t now
)
{
    uint32_t index;
    uint32_t distance;
    uint32_t damage;
    distance = distance_squared(
        bomb->x, bomb->y, player->x, player->y
    );
    if (*player_health != 0u && distance <= 900u * 900u &&
        clear_line(
            &g_map, bomb->x, bomb->y, bomb->z,
            player->x, player->y, player->z + 28)) {
        damage = 500u -
            (distance * 400u) / (900u * 900u);
        if (*player_health <= damage) {
            *player_health = 0u;
            ++*player_deaths;
            add_corpse(
                corpses, player->x, player->y,
                player->z, 0u, now
            );
            kill_feed->killer = 0xffu;
            kill_feed->victim = 0u;
            kill_feed->weapon = 0xffu;
            kill_feed->headshot = 0u;
            kill_feed->until = now + 3500u;
        } else {
            *player_health =
                (uint16_t)(*player_health - damage);
        }
    }
    for (index = 0u; index < BOT_COUNT; ++index) {
        c15_bot_t *bot = &bots[index];
        if (!bot->alive) continue;
        distance = distance_squared(
            bomb->x, bomb->y, bot->mover.x, bot->mover.y
        );
        if (distance > 900u * 900u ||
            !clear_line(
                &g_map, bomb->x, bomb->y, bomb->z,
                bot->mover.x, bot->mover.y,
                bot->mover.z + 28)) {
            continue;
        }
        damage = 500u -
            (distance * 400u) / (900u * 900u);
        if (bot->health <= damage) {
            bot->health = 0u;
            bot->alive = 0u;
            ++bot->deaths;
            add_corpse(
                corpses, bot->mover.x, bot->mover.y,
                bot->mover.z, bot->team, now
            );
            kill_feed->killer = 0xffu;
            kill_feed->victim = (uint8_t)(index + 1u);
            kill_feed->weapon = 0xffu;
            kill_feed->headshot = 0u;
            kill_feed->until = now + 3500u;
        } else {
            bot->health = (uint16_t)(bot->health - damage);
        }
    }
    for (index = 0u; index < HOSTAGE_MAX; ++index) {
        c15_hostage_t *hostage = &hostages[index];
        if (!hostage->active) continue;
        distance = distance_squared(
            bomb->x, bomb->y,
            hostage->mover.x, hostage->mover.y
        );
        if (distance > 900u * 900u) continue;
        damage = 500u -
            (distance * 400u) / (900u * 900u);
        if (hostage->health <= damage) {
            hostage->health = 0u;
            hostage->active = 0u;
            add_corpse(
                corpses,
                hostage->mover.x, hostage->mover.y,
                hostage->mover.z, 0u, now
            );
        } else {
            hostage->health =
                (uint16_t)(hostage->health - damage);
        }
    }
}

static int player_fire_hit(
    c15_bot_t bots[BOT_COUNT],
    c15_hostage_t hostages[HOSTAGE_MAX],
    const c15_camera_t *camera,
    uint8_t player_team,
    uint32_t weapon,
    uint16_t shot_yaw_q8,
    int16_t shot_pitch_q8,
    int secondary_attack,
    int silenced,
    uint32_t *money,
    c15_corpse_t corpses[CORPSE_MAX],
    c15_impact_t impacts[IMPACT_MAX],
    c15_kill_feed_t *kill_feed,
    uint32_t now,
    c15_audio_t *audio
)
{
    int32_t yaw_sine = sin_q14_q8(shot_yaw_q8);
    int32_t yaw_cosine = cos_q14_q8(shot_yaw_q8);
    int32_t pitch_sine = sin_q14_q8(
        (uint16_t)shot_pitch_q8
    );
    int32_t pitch_cosine = cos_q14_q8(
        (uint16_t)shot_pitch_q8
    );
    uint32_t nearest = 0xffffffffu;
    int selected = -1;
    int selected_hostage = -1;
    int32_t selected_hit_x = camera->x;
    int32_t selected_hit_y = camera->y;
    int32_t selected_hit_z = camera->z;
    int32_t selected_minimum_z = 0;
    int32_t selected_maximum_z = 0;
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
            !bullet_line_clear(
                &g_map,
                camera->x, camera->y, camera->z,
                hit_x, hit_y, hit_z,
                g_weapon_penetration[weapon], 0, 0, 0, 0)) {
            continue;
        }
        nearest = distance;
        selected = (int)index;
        selected_hostage = -1;
        selected_hit_x = hit_x;
        selected_hit_y = hit_y;
        selected_hit_z = hit_z;
        selected_minimum_z = minimum_z;
        selected_maximum_z = maximum_z;
    }
    for (index = 0u; index < HOSTAGE_MAX; ++index) {
        int32_t dx;
        int32_t dy;
        int32_t side;
        int32_t forward;
        int32_t minimum_z;
        int32_t maximum_z;
        int32_t minimum_view_y;
        int32_t maximum_view_y;
        int32_t hit_x;
        int32_t hit_y;
        int32_t hit_z;
        uint32_t distance;
        if (!hostages[index].active || hostages[index].rescued ||
            hostages[index].health == 0u) {
            continue;
        }
        dx = hostages[index].mover.x - camera->x;
        dy = hostages[index].mover.y - camera->y;
        side = (int32_t)(
            ((int64_t)yaw_sine * dx -
             (int64_t)yaw_cosine * dy) >> 14
        );
        forward = (int32_t)(
            ((int64_t)yaw_cosine * dx +
             (int64_t)yaw_sine * dy) >> 14
        );
        if (forward <= 0 || abs_i32(side) > 18u) {
            continue;
        }
        minimum_z = hostages[index].mover.z;
        maximum_z = hostages[index].mover.z + 72;
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
            hostages[index].mover.x, hostages[index].mover.y
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
            !bullet_line_clear(
                &g_map,
                camera->x, camera->y, camera->z,
                hit_x, hit_y, hit_z,
                g_weapon_penetration[weapon], 0, 0, 0, 0)) {
            continue;
        }
        nearest = distance;
        selected = -1;
        selected_hostage = (int)index;
        selected_hit_x = hit_x;
        selected_hit_y = hit_y;
        selected_hit_z = hit_z;
        selected_minimum_z = minimum_z;
        selected_maximum_z = maximum_z;
    }
    if (selected < 0 && selected_hostage < 0) {
        int32_t shot_range =
            weapon == WEAPON_KNIFE ? 90 : 2048;
        int32_t direction_x = (int32_t)(
            ((int64_t)yaw_cosine * pitch_cosine) >> 14
        );
        int32_t direction_y = (int32_t)(
            ((int64_t)yaw_sine * pitch_cosine) >> 14
        );
        int32_t end_x = camera->x +
            (int32_t)(((int64_t)direction_x * shot_range) >> 14);
        int32_t end_y = camera->y +
            (int32_t)(((int64_t)direction_y * shot_range) >> 14);
        int32_t end_z = camera->z +
            (int32_t)(((int64_t)pitch_sine * shot_range) >> 14);
        int32_t impact_x = end_x;
        int32_t impact_y = end_y;
        int32_t impact_z = end_z;
        if (!bullet_line_clear(
                &g_map,
                camera->x, camera->y, camera->z,
                end_x, end_y, end_z,
                g_weapon_penetration[weapon],
                1,
                &impact_x, &impact_y, &impact_z)) {
            add_impact(
                impacts, impact_x, impact_y, impact_z, 0u, now
            );
            c15_audio_play(
                audio, C15_SOUND_CUE_RICOCHET,
                C15_SOUND_CHANNEL_BOT
            );
        }
        return 0;
    }
    {
        uint32_t height =
            (uint32_t)(selected_maximum_z - selected_minimum_z);
        uint8_t hitgroup =
            selected_hit_z >= selected_maximum_z - (int32_t)(height / 5u) ?
                1u :
            selected_hit_z <= selected_minimum_z +
                    (int32_t)(height * 7u / 20u) ? 3u : 2u;
        uint32_t distance = abs_i32(selected_hit_x - camera->x);
        uint32_t axis_distance = abs_i32(selected_hit_y - camera->y);
        uint32_t damage;
        if (axis_distance > distance) distance = axis_distance;
        damage = c15_damage_range_adjusted(
            weapon == WEAPON_KNIFE && secondary_attack ?
                65u : weapon_base_damage(weapon, silenced),
            weapon_range_modifier_q8(weapon, silenced),
            distance
        );
        add_impact(
            impacts, selected_hit_x, selected_hit_y,
            selected_hit_z, 1u, now
        );
        c15_audio_play(
            audio,
            hitgroup == 1u ?
                C15_SOUND_CUE_HEADSHOT : C15_SOUND_CUE_HIT_FLESH,
            C15_SOUND_CHANNEL_BOT
        );
        if (selected_hostage >= 0) {
            c15_hostage_t *hostage =
                &hostages[(uint32_t)selected_hostage];
            damage = c15_damage_apply_bullet(
                damage, hitgroup, 0, 0u,
                g_weapon_armor_ratio_q8[weapon]
            );
            if (hostage->health <= damage) {
                hostage->health = 0u;
                hostage->active = 0u;
                if (*money > 2250u) *money -= 2250u;
                else *money = 0u;
                add_corpse(
                    corpses,
                    hostage->mover.x, hostage->mover.y,
                    hostage->mover.z,
                    0u, now
                );
                kill_feed->killer = 0u;
                kill_feed->victim =
                    (uint8_t)(0x80u + (uint32_t)selected_hostage);
                kill_feed->weapon = (uint8_t)weapon;
                kill_feed->headshot = hitgroup == 1u;
                kill_feed->until = now + 3500u;
                c15_audio_play(
                    audio, C15_SOUND_CUE_DEATH,
                    C15_SOUND_CHANNEL_BOT
                );
                return 3;
            }
            hostage->health = (uint16_t)(hostage->health - damage);
            return 1;
        } else {
            c15_bot_t *bot = &bots[(uint32_t)selected];
            damage = c15_damage_apply_bullet(
                damage, hitgroup, &bot->armor, bot->helmet,
                g_weapon_armor_ratio_q8[weapon]
            );
            if (bot->health <= damage) {
                bot->health = 0u;
                bot->alive = 0u;
                ++bot->deaths;
                *money += 300u;
                if (*money > 16000u) {
                    *money = 16000u;
                }
                add_corpse(
                    corpses, bot->mover.x, bot->mover.y,
                    bot->mover.z, bot->team, now
                );
                kill_feed->killer = 0u;
                kill_feed->victim = (uint8_t)(selected + 1);
                kill_feed->weapon = (uint8_t)weapon;
                kill_feed->headshot = hitgroup == 1u;
                kill_feed->until = now + 3500u;
                c15_audio_play(
                    audio, C15_SOUND_CUE_DEATH,
                    C15_SOUND_CHANNEL_BOT
                );
                return 2;
            }
            bot->health = (uint16_t)(bot->health - damage);
        }
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

static void draw_pause_menu(
    lite_framebuffer_t *fb,
    const c15_bot_t bots[BOT_COUNT],
    uint8_t player_team,
    uint32_t player_kills,
    uint32_t player_deaths,
    uint32_t rounds_t,
    uint32_t rounds_ct,
    uint32_t selection
)
{
    uint16_t white = lite_rgb565(238u, 244u, 239u);
    uint16_t gold = lite_rgb565(244u, 181u, 46u);
    uint16_t red = lite_rgb565(235u, 78u, 64u);
    uint16_t green = lite_rgb565(82u, 203u, 122u);
    uint32_t index;
    lite_fb_clear(fb, lite_rgb565(8u, 14u, 14u));
    lite_fb_frame(fb, 18, 8, 284, 224, gold);
    lite_fb_text(fb, 121, 15, "GAME PAUSED", 1, gold);
    lite_fb_text(fb, 36, 34, "NAME", 1, white);
    lite_fb_text(fb, 151, 34, "TEAM", 1, white);
    lite_fb_text(fb, 210, 34, "K", 1, white);
    lite_fb_text(fb, 250, 34, "D", 1, white);
    lite_fb_line(fb, 32, 45, 287, 45, lite_rgb565(68u, 78u, 67u));
    lite_fb_text(fb, 36, 51, "YOU", 1, gold);
    lite_fb_text(
        fb, 151, 51, player_team == TEAM_T ? "T" : "CT", 1,
        player_team == TEAM_T ? red : green
    );
    lite_fb_u32(fb, 210, 51, player_kills, 1, white);
    lite_fb_u32(fb, 250, 51, player_deaths, 1, white);
    for (index = 0u; index < BOT_COUNT; ++index) {
        int y = 66 + (int)index * 15;
        lite_fb_text(fb, 36, y, "BOT", 1, white);
        lite_fb_u32(fb, 57, y, index + 1u, 1, white);
        lite_fb_text(
            fb, 151, y, bots[index].team == TEAM_T ? "T" : "CT", 1,
            bots[index].team == TEAM_T ? red : green
        );
        lite_fb_u32(fb, 210, y, bots[index].kills, 1, white);
        lite_fb_u32(fb, 250, y, bots[index].deaths, 1, white);
    }
    lite_fb_text(fb, 76, 174, "T", 1, red);
    lite_fb_u32(fb, 88, 174, rounds_t, 1, white);
    lite_fb_text(fb, 199, 174, "CT", 1, green);
    lite_fb_u32(fb, 217, 174, rounds_ct, 1, white);
    draw_button(fb, 38, 195, 110, 27, "RESUME", selection == 0u);
    draw_button(fb, 172, 195, 110, 27, "MAIN MENU", selection == 1u);
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

static int project_world_point(
    const c15_camera_t *camera,
    int32_t world_x,
    int32_t world_y,
    int32_t world_z,
    int *screen_x,
    int *screen_y,
    int32_t *depth_out
)
{
    int32_t dx = world_x - camera->x;
    int32_t dy = world_y - camera->y;
    int32_t sine = sin_q14_q8(camera->yaw_q8);
    int32_t cosine = cos_q14_q8(camera->yaw_q8);
    int32_t pitch_sine =
        sin_q14_q8((uint16_t)camera->pitch_q8);
    int32_t pitch_cosine =
        cos_q14_q8((uint16_t)camera->pitch_q8);
    int32_t side = (int32_t)(
        ((int64_t)sine * dx - (int64_t)cosine * dy) >> 14
    );
    int32_t forward = (int32_t)(
        ((int64_t)cosine * dx + (int64_t)sine * dy) >> 14
    );
    int32_t vertical = world_z - camera->z;
    int32_t view_y = (int32_t)(
        ((int64_t)pitch_cosine * vertical -
         (int64_t)pitch_sine * forward) >> 14
    );
    int32_t view_z = (int32_t)(
        ((int64_t)pitch_sine * vertical +
         (int64_t)pitch_cosine * forward) >> 14
    );
    int32_t focal = camera->focal_length != 0u ?
        camera->focal_length : DEFAULT_FOCAL_LENGTH;
    if (view_z < 8) {
        return 0;
    }
    *screen_x = (int)LITE_VIEW_WIDTH / 2 +
        (int)(side * focal / view_z);
    *screen_y = (int)LITE_VIEW_HEIGHT / 2 -
        (int)(view_y * focal / view_z);
    *depth_out = view_z;
    return *screen_x >= 0 && *screen_x < (int)LITE_VIEW_WIDTH &&
        *screen_y >= 0 && *screen_y < (int)LITE_VIEW_HEIGHT;
}

static int world_point_visible(int x, int y, int32_t depth)
{
    uint16_t scene_depth;
    if (x < 0 || x >= (int)LITE_VIEW_WIDTH ||
        y < 0 || y >= (int)LITE_VIEW_HEIGHT ||
        depth < 0 || depth > 65535) {
        return 0;
    }
    scene_depth = g_depth[y * LITE_VIEW_WIDTH + x];
    return scene_depth == 0xffffu ||
        depth <= (int32_t)scene_depth + 20;
}

static void draw_world_gameplay(
    lite_framebuffer_t *fb,
    const c15_camera_t *camera,
    const c15_hostage_t hostages[HOSTAGE_MAX],
    const c15_grenade_t grenades[GRENADE_MAX],
    const c15_corpse_t corpses[CORPSE_MAX],
    const c15_dropped_weapon_t dropped[DROPPED_WEAPON_MAX],
    const c15_impact_t impacts[IMPACT_MAX],
    const c15_bomb_state_t *bomb,
    uint32_t now
)
{
    uint16_t t_color = lite_rgb565(189u, 116u, 54u);
    uint16_t ct_color = lite_rgb565(64u, 126u, 190u);
    uint16_t hostage_color = lite_rgb565(221u, 207u, 151u);
    uint16_t smoke_color = lite_rgb565(104u, 111u, 105u);
    uint16_t hot = lite_rgb565(244u, 181u, 46u);
    uint16_t red = lite_rgb565(235u, 78u, 64u);
    uint32_t index;
    for (index = 0u; index < HOSTAGE_MAX; ++index) {
        int x;
        int y;
        int32_t depth;
        int height;
        if (!hostages[index].active ||
            !project_world_point(
                camera,
                hostages[index].mover.x,
                hostages[index].mover.y,
                hostages[index].mover.z + 38,
                &x, &y, &depth) ||
            !world_point_visible(x, y, depth)) {
            continue;
        }
        height = (int)(72 * camera->focal_length / depth);
        if (height < 7) height = 7;
        if (height > 60) height = 60;
        lite_fb_rect(fb, x - height / 8, y - height / 2,
                     height / 4 + 1, height / 5 + 1,
                     hostage_color);
        lite_fb_line(fb, x, y - height / 3, x, y + height / 2,
                     hostage_color);
        lite_fb_line(fb, x, y, x - height / 4, y + height / 4,
                     hostage_color);
        lite_fb_line(fb, x, y, x + height / 4, y + height / 4,
                     hostage_color);
    }
    for (index = 0u; index < GRENADE_MAX; ++index) {
        int x;
        int y;
        int32_t depth;
        const c15_grenade_t *grenade = &grenades[index];
        if (grenade->active == 0u ||
            !project_world_point(
                camera, grenade->x_q8 >> 8,
                grenade->y_q8 >> 8, grenade->z_q8 >> 8,
                &x, &y, &depth) ||
            !world_point_visible(x, y, depth)) {
            continue;
        }
        if (grenade->active == 2u) {
            int radius = (int)(
                128 * camera->focal_length / depth
            );
            if (radius < 8) radius = 8;
            if (radius > 45) radius = 45;
            lite_fb_blend_rect(
                fb, x - radius, y - radius / 2,
                radius * 2, radius, smoke_color
            );
        } else {
            uint16_t color = grenade->kind == GRENADE_HE ? red :
                (grenade->kind == GRENADE_FLASH ?
                    lite_rgb565(236u, 236u, 222u) :
                    lite_rgb565(112u, 168u, 102u));
            lite_fb_rect(fb, x - 2, y - 2, 5, 5, color);
        }
    }
    for (index = 0u; index < CORPSE_MAX; ++index) {
        int x;
        int y;
        int32_t depth;
        int half;
        uint16_t color;
        if (!corpses[index].active ||
            !project_world_point(
                camera, corpses[index].x, corpses[index].y,
                corpses[index].z + 4, &x, &y, &depth) ||
            !world_point_visible(x, y, depth)) {
            continue;
        }
        half = (int)(34 * camera->focal_length / depth);
        if (half < 3) half = 3;
        if (half > 32) half = 32;
        color = corpses[index].team == TEAM_T ?
            t_color : (corpses[index].team == TEAM_CT ?
                ct_color : hostage_color);
        lite_fb_line(fb, x - half, y, x + half, y + half / 3, color);
        if (now - corpses[index].died_at < 900u) {
            lite_fb_line(fb, x - half / 2, y - 2,
                         x + half / 2, y + half / 4, red);
        }
    }
    for (index = 0u; index < DROPPED_WEAPON_MAX; ++index) {
        int x;
        int y;
        int32_t depth;
        int half;
        if (!dropped[index].active ||
            !project_world_point(
                camera, dropped[index].x, dropped[index].y,
                dropped[index].z + 4, &x, &y, &depth) ||
            !world_point_visible(x, y, depth)) {
            continue;
        }
        half = (int)(22 * camera->focal_length / depth);
        if (half < 3) half = 3;
        if (half > 20) half = 20;
        lite_fb_line(fb, x - half, y + 2, x + half, y - 2, hot);
        lite_fb_line(fb, x, y - 3, x + half / 2, y + 4, hot);
        if (depth < 220) {
            lite_fb_text(
                fb, x - 12, y - 14,
                g_weapon_labels[dropped[index].weapon], 1, hot
            );
        }
    }
    for (index = 0u; index < IMPACT_MAX; ++index) {
        int x;
        int y;
        int32_t depth;
        int radius;
        const c15_impact_t *impact = &impacts[index];
        if (!impact->active || time_reached(now, impact->until) ||
            !project_world_point(
                camera, impact->x, impact->y, impact->z,
                &x, &y, &depth) ||
            !world_point_visible(x, y, depth)) {
            continue;
        }
        radius = impact->kind == 2u ? 12 : 3;
        lite_fb_line(fb, x - radius, y, x + radius, y,
                     impact->kind == 1u ? red : hot);
        lite_fb_line(fb, x, y - radius, x, y + radius,
                     impact->kind == 1u ? red : hot);
    }
    for (index = 0u; index < c15_map_dynamic_count(&g_map); ++index) {
        c15_dynamic_entity_t entity;
        int x;
        int y;
        int32_t depth;
        int width;
        int height;
        int32_t horizontal_extent;
        uint16_t color;
        if (!c15_map_dynamic(&g_map, index, &entity) ||
            entity.kind == C15_DYNAMIC_BUTTON ||
            g_map.dynamic_position[index] >= 8u ||
            (entity.kind == C15_DYNAMIC_BREAKABLE &&
             (g_map.dynamic_broken_bits & (1u << index)) != 0u) ||
            !project_world_point(
                camera,
                ((int32_t)entity.minimum_x + entity.maximum_x) / 2,
                ((int32_t)entity.minimum_y + entity.maximum_y) / 2,
                ((int32_t)entity.minimum_z + entity.maximum_z) / 2 +
                    (((int32_t)entity.maximum_z - entity.minimum_z) *
                     g_map.dynamic_position[index] / 8),
                &x, &y, &depth) ||
            !world_point_visible(x, y, depth)) {
            continue;
        }
        horizontal_extent =
            (int32_t)entity.maximum_x - entity.minimum_x;
        if ((int32_t)entity.maximum_y - entity.minimum_y >
            horizontal_extent) {
            horizontal_extent =
                (int32_t)entity.maximum_y - entity.minimum_y;
        }
        width = horizontal_extent * camera->focal_length / depth;
        height = (entity.maximum_z - entity.minimum_z) *
            camera->focal_length / depth;
        if (width < 4) width = 4;
        if (width > 100) width = 100;
        if (height < 5) height = 5;
        if (height > 140) height = 140;
        color = entity.kind == C15_DYNAMIC_BREAKABLE ?
            lite_rgb565(144u, 95u, 48u) :
            lite_rgb565(105u, 112u, 103u);
        lite_fb_frame(
            fb, x - width / 2, y - height / 2,
            width, height, color
        );
    }
    if (bomb->enabled &&
        ((bomb->planted && !bomb->defused) || bomb->dropped)) {
        int x;
        int y;
        int32_t depth;
        if (project_world_point(
                camera, bomb->x, bomb->y, bomb->z + 5,
                &x, &y, &depth) &&
            world_point_visible(x, y, depth)) {
            lite_fb_rect(fb, x - 5, y - 3, 11, 7,
                         bomb->planted ? red : hot);
            lite_fb_text(fb, x - 6, y - 13, "C4", 1,
                         bomb->planted ? red : hot);
        }
    }
}

static void draw_radar(
    lite_framebuffer_t *fb,
    const c15_camera_t *camera,
    const c15_bot_t bots[BOT_COUNT],
    const c15_hostage_t hostages[HOSTAGE_MAX],
    const c15_bomb_state_t *bomb,
    uint8_t player_team
)
{
    uint16_t green = lite_rgb565(82u, 203u, 122u);
    uint16_t gold = lite_rgb565(244u, 181u, 46u);
    uint16_t gray = lite_rgb565(182u, 184u, 165u);
    int32_t sine = sin_q14_q8(camera->yaw_q8);
    int32_t cosine = cos_q14_q8(camera->yaw_q8);
    uint32_t index;
    lite_fb_blend_rect(fb, 4, 25, 49, 49, lite_rgb565(12u, 22u, 22u));
    lite_fb_frame(fb, 4, 25, 49, 49, green);
    lite_fb_line(fb, 28, 47, 28, 52, green);
    for (index = 0u; index < BOT_COUNT; ++index) {
        int32_t dx;
        int32_t dy;
        int32_t side;
        int32_t forward;
        int x;
        int y;
        if (!bots[index].alive ||
            bots[index].team != player_team) {
            continue;
        }
        dx = bots[index].mover.x - camera->x;
        dy = bots[index].mover.y - camera->y;
        side = (int32_t)(((int64_t)sine * dx -
                          (int64_t)cosine * dy) >> 14);
        forward = (int32_t)(((int64_t)cosine * dx +
                             (int64_t)sine * dy) >> 14);
        if (side < -800) side = -800;
        if (side > 800) side = 800;
        if (forward < -800) forward = -800;
        if (forward > 800) forward = 800;
        x = 28 + (int)(side * 21 / 800);
        y = 49 - (int)(forward * 21 / 800);
        lite_fb_rect(
            fb, x - 1, y - 1, 3, 3,
            green
        );
    }
    for (index = 0u; index < HOSTAGE_MAX; ++index) {
        int32_t dx;
        int32_t dy;
        int32_t side;
        int32_t forward;
        int x;
        int y;
        if (!hostages[index].active) continue;
        dx = hostages[index].mover.x - camera->x;
        dy = hostages[index].mover.y - camera->y;
        side = (int32_t)(((int64_t)sine * dx -
                          (int64_t)cosine * dy) >> 14);
        forward = (int32_t)(((int64_t)cosine * dx +
                             (int64_t)sine * dy) >> 14);
        if (side < -800) side = -800;
        if (side > 800) side = 800;
        if (forward < -800) forward = -800;
        if (forward > 800) forward = 800;
        x = 28 + (int)(side * 21 / 800);
        y = 49 - (int)(forward * 21 / 800);
        lite_fb_rect(fb, x, y, 2, 2, gray);
    }
    if (bomb->enabled && (bomb->dropped || bomb->planted)) {
        int32_t dx = bomb->x - camera->x;
        int32_t dy = bomb->y - camera->y;
        int32_t side = (int32_t)(
            ((int64_t)sine * dx - (int64_t)cosine * dy) >> 14
        );
        int32_t forward = (int32_t)(
            ((int64_t)cosine * dx + (int64_t)sine * dy) >> 14
        );
        int x;
        int y;
        if (side < -800) side = -800;
        if (side > 800) side = 800;
        if (forward < -800) forward = -800;
        if (forward > 800) forward = 800;
        x = 28 + (int)(side * 21 / 800);
        y = 49 - (int)(forward * 21 / 800);
        lite_fb_rect(fb, x - 1, y - 1, 3, 3, gold);
    }
}

static void draw_kill_feed(
    lite_framebuffer_t *fb,
    const c15_kill_feed_t *feed,
    uint32_t now
)
{
    uint16_t white = lite_rgb565(238u, 244u, 239u);
    uint16_t gold = lite_rgb565(244u, 181u, 46u);
    uint16_t red = lite_rgb565(235u, 78u, 64u);
    if (!time_active(now, feed->until)) return;
    lite_fb_blend_rect(fb, 111, 27, 147, 14, lite_rgb565(18u, 31u, 31u));
    if (feed->killer == 0u) {
        lite_fb_text(fb, 115, 31, "YOU", 1, gold);
    } else if (feed->killer == 0xffu) {
        lite_fb_text(fb, 115, 31, "WORLD", 1, red);
    } else {
        lite_fb_text(fb, 115, 31, "BOT", 1, white);
        lite_fb_u32(fb, 136, 31, feed->killer, 1, white);
    }
    lite_fb_text(fb, 158, 31, ">", 1, feed->headshot ? red : gold);
    if (feed->victim == 0u) {
        lite_fb_text(fb, 174, 31, "YOU", 1, white);
    } else if (feed->victim >= 0x80u) {
        lite_fb_text(fb, 174, 31, "HOSTAGE", 1, white);
    } else {
        lite_fb_text(fb, 174, 31, "BOT", 1, white);
        lite_fb_u32(fb, 195, 31, feed->victim, 1, white);
    }
}

static void draw_scope(lite_framebuffer_t *fb)
{
    uint16_t black = lite_rgb565(0u, 0u, 0u);
    uint16_t gray = lite_rgb565(92u, 99u, 91u);
    lite_fb_rect(fb, 0, 0, 72, LITE_VIEW_HEIGHT, black);
    lite_fb_rect(fb, 248, 0, 72, LITE_VIEW_HEIGHT, black);
    lite_fb_rect(fb, 72, 0, 176, 32, black);
    lite_fb_rect(fb, 72, 208, 176, 32, black);
    lite_fb_line(fb, 72, 120, 248, 120, gray);
    lite_fb_line(fb, 160, 32, 160, 208, gray);
}

static const char *round_reason_label(uint8_t reason)
{
    if (reason == ROUND_REASON_TARGET_BOMBED) return "TARGET BOMBED";
    if (reason == ROUND_REASON_BOMB_DEFUSED) return "BOMB DEFUSED";
    if (reason == ROUND_REASON_TARGET_SAVED) return "TARGET SAVED";
    if (reason == ROUND_REASON_HOSTAGES_RESCUED) {
        return "HOSTAGES RESCUED";
    }
    if (reason == ROUND_REASON_TIME_EXPIRED) return "TIME EXPIRED";
    return "TEAM ELIMINATED";
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
    uint8_t spectator_target,
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
    uint32_t reserve_ammo,
    uint16_t player_armor,
    uint8_t player_defuse_kit,
    const uint8_t grenade_counts[GRENADE_KIND_COUNT],
    const c15_hostage_t hostages[HOSTAGE_MAX],
    const c15_grenade_t grenades[GRENADE_MAX],
    const c15_corpse_t corpses[CORPSE_MAX],
    const c15_dropped_weapon_t dropped[DROPPED_WEAPON_MAX],
    const c15_impact_t impacts[IMPACT_MAX],
    const c15_kill_feed_t *kill_feed,
    const c15_bomb_state_t *bomb,
    uint32_t round_deadline,
    uint32_t freeze_until,
    uint32_t crosshair_spread_q8,
    uint32_t flash_until,
    int zoomed,
    int silenced,
    int glock_burst,
    enum game_screen screen,
    uint8_t round_winner,
    uint8_t round_reason,
    c15_render_stats_t *stats,
    c15_model_render_stats_t *view_stats,
    c15_model_render_stats_t *entity_stats,
    c15_render_frame_timing_t *timing
)
{
    uint16_t white = lite_rgb565(238u, 244u, 239u);
    uint16_t green = lite_rgb565(82u, 203u, 122u);
    uint16_t accent = lite_rgb565(244u, 181u, 46u);
    uint16_t red = lite_rgb565(235u, 78u, 64u);
    uint32_t t_alive;
    uint32_t ct_alive;
    uint32_t index;
    uint32_t phase_started;
    uint32_t phase_finished;
    int bob_q4 = view_bob_q4(input, now);
    (void)player;
    bda_memset(timing, 0, sizeof(*timing));
    phase_started = lite_platform_milliseconds();
    c15_render_world(
        &g_map, camera, view, g_depth, g_visible_surfaces,
        sizeof(g_visible_surfaces), stats
    );
    phase_finished = lite_platform_milliseconds();
    timing->world_ms = phase_finished - phase_started;
    timing->world_clear_ms = stats->clear_ms;
    phase_started = phase_finished;
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
        if (!c15_render_world_point_visible(
                &g_map,
                bots[index].mover.x,
                bots[index].mover.y,
                bots[index].mover.z + 28)) {
            ++timing->entity_pvs_culled;
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
        /*
         * Beyond 900 world units the held weapon is only a few pixels tall.
         * Keep the animated body silhouette but avoid a second full model
         * transform/raster pass for detail the 320x240 display cannot show.
         */
        if (distance <= 900u * 900u) {
            c15_render_world_model(
                held, view, g_depth, camera,
                bots[index].mover.x, bots[index].mover.y,
                bots[index].mover.z,
                (uint8_t)(bots[index].mover.yaw - 64u),
                entity_stats
            );
        }
    }
    draw_world_gameplay(
        fb, camera, hostages, grenades, corpses, dropped,
        impacts, bomb, now
    );
    phase_finished = lite_platform_milliseconds();
    timing->entities_ms = phase_finished - phase_started;
    phase_started = phase_finished;
    if (player_health != 0u && !zoomed) {
        c15_render_view_model(
            &g_view_model, view, g_depth,
            &g_weapon_views[weapon],
            firing ? 6 : 0, bob_q4, view_stats
        );
    } else if (player_health != 0u) {
        bda_memset(view_stats, 0, sizeof(*view_stats));
        draw_scope(fb);
    } else {
        bda_memset(view_stats, 0, sizeof(*view_stats));
    }
    phase_finished = lite_platform_milliseconds();
    timing->view_ms = phase_finished - phase_started;
    phase_started = phase_finished;
    {
        int gap = 4 + (int)(crosshair_spread_q8 / 80u);
        if (gap > 18) gap = 18;
        lite_fb_line(
            fb, 160 - gap - 7, 120, 160 - gap, 120,
            hit_confirmed ? red : accent
        );
        lite_fb_line(
            fb, 160 + gap, 120, 160 + gap + 7, 120,
            hit_confirmed ? red : accent
        );
        lite_fb_line(
            fb, 160, 120 - gap - 7, 160, 120 - gap,
            hit_confirmed ? red : accent
        );
        lite_fb_line(
            fb, 160, 120 + gap, 160, 120 + gap + 7,
            hit_confirmed ? red : accent
        );
    }
    draw_fps(fb, fps_x10, green, accent);
    lite_fb_text(fb, 4, 15, g_map_labels[map_id], 1, white);
    lite_fb_text(fb, 232, 4, g_weapon_labels[weapon], 1, accent);
    if (weapon != WEAPON_KNIFE) {
        lite_fb_text(fb, 232, 15, "AMMO", 1, white);
        lite_fb_u32(fb, 262, 15, ammo, 1, accent);
        lite_fb_text(fb, 280, 15, "/", 1, white);
        lite_fb_u32(fb, 289, 15, reserve_ammo, 1, accent);
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
    if (round_deadline != 0u) {
        uint32_t seconds = time_reached(now, round_deadline) ?
            0u : (round_deadline - now + 999u) / 1000u;
        lite_fb_u32(fb, 211, 4, seconds / 60u, 1, white);
        lite_fb_text(fb, 223, 4, ":", 1, white);
        if (seconds % 60u < 10u) {
            lite_fb_text(fb, 229, 4, "0", 1, white);
            lite_fb_u32(fb, 235, 4, seconds % 60u, 1, white);
        } else {
            lite_fb_u32(fb, 229, 4, seconds % 60u, 1, white);
        }
    }
    if (bomb->enabled) {
        if (bomb->planted && !bomb->defused) {
            uint32_t remaining = time_reached(now, bomb->explode_at) ?
                0u : (bomb->explode_at - now + 999u) / 1000u;
            lite_fb_text(fb, 70, 4, "C4", 1, red);
            lite_fb_u32(fb, 88, 4, remaining, 1, accent);
        } else if (bomb->player_carrier && player_team == TEAM_T) {
            lite_fb_text(fb, 70, 4, "C4", 1, accent);
        }
        if (bomb->action_owner == 1u &&
            bomb->action != BOMB_ACTION_NONE) {
            uint32_t duration = bomb->action == BOMB_ACTION_PLANT ?
                BOMB_PLANT_MS : (
                    player_defuse_kit ?
                        BOMB_DEFUSE_MS / 2u : BOMB_DEFUSE_MS
                );
            uint32_t elapsed = now - bomb->action_started;
            int width;
            if (elapsed > duration) elapsed = duration;
            width = (int)(elapsed * 120u / duration);
            lite_fb_blend_rect(
                fb, 74, 198, 128, 14, lite_rgb565(18u, 31u, 31u)
            );
            lite_fb_frame(fb, 74, 198, 128, 14, accent);
            lite_fb_rect(fb, 78, 202, width, 6, accent);
        }
    } else if (c15_map_hostage_count(&g_map) != 0u) {
        uint32_t total = c15_map_hostage_count(&g_map);
        if (total > HOSTAGE_MAX) total = HOSTAGE_MAX;
        lite_fb_text(fb, 70, 4, "H", 1, accent);
        lite_fb_u32(
            fb, 82, 4, hostage_count_rescued(hostages), 1, white
        );
        lite_fb_text(fb, 88, 4, "/", 1, white);
        lite_fb_u32(fb, 94, 4, total, 1, accent);
    }
    draw_radar(fb, camera, bots, hostages, bomb, player_team);
    draw_kill_feed(fb, kill_feed, now);
    lite_fb_blend_rect(fb, 2, 218, 225, 20, lite_rgb565(18u, 31u, 31u));
    lite_fb_text(fb, 7, 224, "HP", 1, green);
    lite_fb_u32(
        fb, 25, 224, player_health, 1,
        player_health < 30u ? red : white
    );
    lite_fb_text(fb, 63, 224, "MONEY", 1, green);
    lite_fb_u32(fb, 99, 224, money, 1, accent);
    lite_fb_text(fb, 142, 224, "AP", 1, green);
    lite_fb_u32(fb, 160, 224, player_armor, 1, white);
    lite_fb_text(fb, 187, 224, "G", 1, green);
    lite_fb_u32(
        fb, 199, 224,
        grenade_counts[0] + grenade_counts[1] + grenade_counts[2],
        1, accent
    );
    if (firing && !zoomed &&
        !(silenced &&
          (weapon == WEAPON_USP || weapon == WEAPON_M4A1))) {
        draw_fire_feedback(
            fb, weapon, muzzle_frame, fire_pose_frame,
            6, bob_q4, accent
        );
    }
    draw_controls(
        fb, input,
        weapon_alt_label(weapon, zoomed, silenced, glock_burst)
    );
    if (player_health == 0u) {
        lite_fb_blend_rect(fb, 82, 190, 156, 20, lite_rgb565(52u, 15u, 15u));
        lite_fb_text(fb, 92, 196, "SPECTATING", 1, white);
        if (spectator_target < BOT_COUNT) {
            lite_fb_text(fb, 164, 196, "BOT", 1, accent);
            lite_fb_u32(
                fb, 185, 196, spectator_target + 1u, 1, accent
            );
        }
    }
    if (time_active(now, freeze_until)) {
        uint32_t seconds = (freeze_until - now + 999u) / 1000u;
        lite_fb_blend_rect(fb, 74, 82, 172, 42, lite_rgb565(17u, 24u, 21u));
        lite_fb_frame(fb, 74, 82, 172, 42, accent);
        lite_fb_text(fb, 99, 93, "FREEZE TIME", 1, accent);
        lite_fb_u32(fb, 151, 109, seconds, 1, white);
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
        lite_fb_text(
            fb, 100, 113, round_reason_label(round_reason), 1, white
        );
        lite_fb_text(fb, 116, 129, "NEXT ROUND", 1, white);
    }
    if (time_active(now, flash_until)) {
        lite_fb_rect(
            fb, 0, 0, LITE_VIEW_WIDTH, LITE_VIEW_HEIGHT,
            lite_rgb565(255u, 255u, 245u)
        );
    }
    timing->hud_ms =
        lite_platform_milliseconds() - phase_started;
}

static uint32_t menu_touch_item(
    enum game_screen screen,
    int x,
    int y,
    uint32_t selection,
    uint32_t buy_category
)
{
    if ((screen == SCREEN_BUY || screen == SCREEN_BUY_ITEMS) &&
        x >= 230 && x < (int)LITE_SCREEN_WIDTH &&
        y >= 201 && y < (int)LITE_SCREEN_HEIGHT) {
        return BUY_ITEM_START;
    }
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
        if (y >= 80 && y < 218) {
            uint32_t item = map_window_start(selection) +
                (uint32_t)((y - 80) / 27);
            if (item <= MAP_COUNT) return item;
        }
    } else if (screen == SCREEN_TEAM) {
        if (y >= 78 && y < 120) return 0u;
        if (y >= 118 && y < 160) return 1u;
        if (y >= 158 && y < 191) return 2u;
        if (y >= 190 && y < 225) return 3u;
    } else if (screen == SCREEN_BUY) {
        if (y >= 82 && y < 204) {
            uint32_t category = (uint32_t)(y - 82) / 20u;
            return category < BUY_CATEGORY_COUNT ?
                category : 0xffffffffu;
        }
    } else if (screen == SCREEN_BUY_ITEMS) {
        uint32_t total = buy_category < BUY_CATEGORY_COUNT ?
            g_buy_category_item_count[buy_category] : 0u;
        if (x < 116 && y >= 201 &&
            y < (int)LITE_SCREEN_HEIGHT) {
            return BUY_ACTION_BACK;
        }
        if (y >= 86 && y < 196) {
            uint32_t row = (uint32_t)(y - 86) / 27u;
            uint32_t item = buy_item_window_start(
                selection, total
            ) + row;
            return item < total ? item : 0xffffffffu;
        }
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
    lite_log_i32("touch_release_dx", debug->release_dx);
    lite_log_i32("touch_release_dy", debug->release_dy);
    lite_log_u32(
        "touch_release_delta_suppressed",
        debug->release_delta_suppressed
    );
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
    lite_arena_t view_cache_arena;
    lite_input_t input;
    c15_camera_t camera;
    c15_player_t player;
    c15_bot_t bots[BOT_COUNT];
    c15_bomb_state_t bomb;
    c15_hostage_t hostages[HOSTAGE_MAX];
    c15_grenade_t grenades[GRENADE_MAX];
    c15_corpse_t corpses[CORPSE_MAX];
    c15_dropped_weapon_t dropped[DROPPED_WEAPON_MAX];
    c15_impact_t impacts[IMPACT_MAX];
    c15_kill_feed_t kill_feed;
    c15_render_stats_t stats = {0};
    c15_model_render_stats_t view_stats = {0};
    c15_model_render_stats_t entity_stats = {0};
    c15_render_frame_timing_t frame_timing = {0};
    c15_timing_accumulator_t logic_timing = {0};
    c15_timing_accumulator_t fire_logic_timing = {0};
    c15_timing_accumulator_t player_logic_timing = {0};
    c15_timing_accumulator_t bot_logic_timing = {0};
    c15_timing_accumulator_t objective_logic_timing = {0};
    c15_timing_accumulator_t logic_steps_timing = {0};
    c15_timing_accumulator_t audio_timing = {0};
    c15_timing_accumulator_t world_timing = {0};
    c15_timing_accumulator_t world_clear_timing = {0};
    c15_timing_accumulator_t entity_timing = {0};
    c15_timing_accumulator_t view_timing = {0};
    c15_timing_accumulator_t hud_timing = {0};
    c15_timing_accumulator_t present_timing = {0};
    c15_audio_t audio;
    c15_view_animation_state_t view_animation = {
        C15_VIEW_ANIMATION_IDLE, 0u, 0u
    };
    c15_world_animation_state_t t_animation = {0xffu, 0xffu};
    c15_world_animation_state_t ct_animation = {0xffu, 0xffu};
    enum game_screen screen = SCREEN_MAIN;
    enum game_screen paused_screen = SCREEN_PLAY;
    uint32_t selection = 0u;
    uint32_t menu_count = 3u;
    uint32_t paused_at = 0u;
    uint32_t paused_selection = 0u;
    uint32_t paused_menu_count = 0u;
    uint32_t next_frame;
    uint32_t next_logic = 0u;
    uint32_t next_audio_service = 0u;
    uint32_t next_metric;
    uint32_t fps_last_frame;
    uint32_t frame = 0u;
    uint32_t fps_x10 = 0u;
    uint32_t render_ms_x10 = 0u;
    uint32_t present_ms_x10 = 0u;
    uint32_t pending_logic_ms = 0u;
    uint32_t pending_fire_logic_ms = 0u;
    uint32_t pending_player_logic_ms = 0u;
    uint32_t pending_bot_logic_ms = 0u;
    uint32_t pending_objective_logic_ms = 0u;
    uint32_t pending_logic_steps = 0u;
    uint32_t pending_audio_ms = 0u;
    uint32_t pending_controls = 0u;
    uint32_t map_id = MAP_DE_DUST2;
    uint32_t buy_category = BUY_CATEGORY_PISTOLS;
    uint32_t weapon = WEAPON_GLOCK;
    uint32_t fire_until = 0u;
    uint32_t hit_until = 0u;
    uint32_t fire_started = 0u;
    uint32_t next_fire = 0u;
    uint32_t shots_fired = 0u;
    uint32_t shots_hit = 0u;
    uint32_t bot_shots = 0u;
    uint32_t bot_hits = 0u;
    uint32_t bot_visibility_traces = 0u;
    uint32_t logic_skipped_steps = 0u;
    uint32_t logic_skipped_steps_window = 0u;
    uint32_t bot_sound_weapon = WEAPON_COUNT;
    uint32_t money = 4000u;
    uint32_t round = 1u;
    uint32_t round_end_at = 0u;
    uint32_t round_deadline = 0u;
    uint32_t buy_deadline = 0u;
    uint32_t freeze_until = 0u;
    uint32_t flash_until = 0u;
    uint32_t next_step_sound = 0u;
    uint32_t weapon_press_started = 0u;
    uint32_t burst_next_fire = 0u;
    uint32_t crosshair_spread_q8 = 0u;
    uint32_t recoil_q8 = 0u;
    uint32_t rounds_t = 0u;
    uint32_t rounds_ct = 0u;
    uint32_t player_kills = 0u;
    uint32_t player_deaths = 0u;
    uint32_t reload_weapon = WEAPON_GLOCK;
    uint16_t weapon_ammo[WEAPON_COUNT];
    uint16_t weapon_reserve[WEAPON_COUNT];
    uint8_t owned[WEAPON_COUNT];
    uint8_t grenade_counts[GRENADE_KIND_COUNT];
    uint8_t player_team = TEAM_T;
    uint8_t bot_difficulty = BOT_EASY;
    uint8_t audio_enabled = 1u;
    uint16_t player_health = 100u;
    uint16_t player_armor = 0u;
    uint8_t round_winner = 0u;
    uint8_t round_reason = ROUND_REASON_NONE;
    uint8_t spectator_target = 0xffu;
    uint8_t last_bot_alive[BOT_COUNT];
    uint8_t player_was_alive = 0u;
    uint8_t weapon_hold_consumed = 0u;
    uint8_t player_helmet = 0u;
    uint8_t player_defuse_kit = 0u;
    uint8_t burst_remaining = 0u;
    uint16_t shot_seed = 0x5a3du;
    int previous_touch_down = 0;
    int game_loaded = 0;
    int reloading = 0;
    int zoomed = 0;
    int silenced = 0;
    int glock_burst = 0;
    int pending_alt_attack = 0;
    int running = 1;
    size_t persistent_models = 0u;
    uint8_t *map_memory = 0;
    uint32_t map_memory_bytes = 0u;
    uint8_t *texture_memory = 0;
    uint32_t texture_memory_bytes = 0u;
    uint8_t *audio_memory = 0;
    uint32_t audio_memory_bytes = 0u;
    uint8_t *view_cache_memory = 0;
    uint32_t view_cache_memory_bytes = 0u;

    bda_memset(&input, 0, sizeof(input));
    bda_memset(&audio, 0, sizeof(audio));
    bda_memset(&camera, 0, sizeof(camera));
    bda_memset(&player, 0, sizeof(player));
    bda_memset(bots, 0, sizeof(bots));
    bda_memset(&bomb, 0, sizeof(bomb));
    bda_memset(hostages, 0, sizeof(hostages));
    bda_memset(grenades, 0, sizeof(grenades));
    bda_memset(corpses, 0, sizeof(corpses));
    bda_memset(dropped, 0, sizeof(dropped));
    bda_memset(last_bot_alive, 0, sizeof(last_bot_alive));
    bda_memset(impacts, 0, sizeof(impacts));
    bda_memset(&kill_feed, 0, sizeof(kill_feed));
    bda_memset(weapon_ammo, 0, sizeof(weapon_ammo));
    bda_memset(weapon_reserve, 0, sizeof(weapon_reserve));
    bda_memset(owned, 0, sizeof(owned));
    bda_memset(grenade_counts, 0, sizeof(grenade_counts));
    camera.focal_length = DEFAULT_FOCAL_LENGTH;
    lite_log_reset();
    lite_log_line("CS15 Lite M20 performance pass 7 start");
    lite_log_u32("logic_catchup_limit", LOGIC_MAX_CATCHUP_STEPS);
    lite_log_line(
        "game_flow=freeze-buy-drops-spectator-objectives-scoreboard"
    );
    lite_log_line("renderer=fast-clip-scanline-direct-tile");
    lite_log_line("entity_visibility=bsp-pvs-conservative");
    lite_log_line("model_division=exact-q16-hardware-fast-path");
    lite_log_line("view_depth=lazy-8x8-tiles");
    lite_log_line(
        "bot_ai=roles-routes-last-seen-range-strafe-burst-grenades"
    );
    lite_log_line("map_memory=per-map-runtime-allocation");
    lite_log_line("texture_memory=per-map-runtime-allocation");
    lite_log_u32(
        "map_working_set_limit_bytes", MAP_WORKING_SET_LIMIT_BYTES
    );
    lite_log_line("bot_animation=goldsrc-hybrid-walk");
    lite_log_line("muzzle_flash=historical_additive_sprite");
    lite_log_line("audio=historical-pcm-resident-with-stream-fallback");
    lite_log_line("audio_ready_poll=throttled-2ms");
    lite_log_line("world_lighting=precomputed-rgb565-palettes");
    lite_log_line(
        "raster_inner_loop=opaque-specialized-power2-and-inrange-fast"
    );
    lite_log_line("render_compile=target-o2-hot-units");
    lite_log_line("framebuffer_fill=aligned-rgb565-pairs");
    lite_log_line("animation_frames=resident-all-view-and-world");
    lite_arena_init(&map_arena, 0, 0u);
    lite_arena_init(&texture_arena, 0, 0u);
    lite_arena_init(&model_arena, g_model_memory, sizeof(g_model_memory));
    lite_arena_init(&view_cache_arena, 0, 0u);
    lite_arena_init(
        &g_animation_arena,
        g_animation_memory, sizeof(g_animation_memory)
    );
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
    } else {
        audio_memory_bytes = c15_audio_resident_size(&audio);
        audio_memory = (uint8_t *)bda_alloc(audio_memory_bytes);
        if (!audio_memory ||
            !c15_audio_load_resident(
                &audio, &g_pak, audio_memory,
                audio_memory_bytes)) {
            if (audio_memory) {
                bda_free(audio_memory);
                audio_memory = 0;
            }
            audio_memory_bytes = 0u;
            lite_log_line("audio residency unavailable; streaming");
        }
    }
    lite_log_u32("audio_resident_bytes", audio_memory_bytes);
    draw_loading(
        &framebuffer, "CACHING WEAPONS",
        lite_rgb565(244u, 181u, 46u)
    );
    (void)lite_platform_present(g_screen);
    view_cache_memory = (uint8_t *)bda_alloc(VIEW_CACHE_ARENA_BYTES);
    if (view_cache_memory) {
        lite_arena_init(
            &view_cache_arena,
            view_cache_memory, VIEW_CACHE_ARENA_BYTES
        );
        if (load_view_cache(&view_cache_arena)) {
            view_cache_memory_bytes =
                (uint32_t)view_cache_arena.used;
        } else {
            bda_free(view_cache_memory);
            view_cache_memory = 0;
            lite_arena_init(&view_cache_arena, 0, 0u);
            lite_log_line(
                "view cache unavailable; load current weapon on demand"
            );
        }
    } else {
        lite_log_line(
            "view cache allocation failed; load current weapon on demand"
        );
    }
    lite_log_u32(
        "view_cache_resident_bytes", view_cache_memory_bytes
    );
    c15_audio_set_enabled(
        &audio, audio_enabled,
        g_load_scratch, sizeof(g_load_scratch)
    );
    texture_memory_bytes = MENU_TEXTURE_ARENA_BYTES;
    texture_memory = (uint8_t *)bda_alloc(texture_memory_bytes);
    if (texture_memory) {
        lite_arena_init(
            &texture_arena, texture_memory, texture_memory_bytes
        );
    } else {
        lite_log_line("menu texture allocation failed");
    }
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
        service_audio_throttled(
            &audio, &g_pak, g_load_scratch,
            sizeof(g_load_scratch), now,
            &next_audio_service, &pending_audio_ms
        );
        previous_touch_down = input.touch_down;
        if (lite_platform_touch_debug_take(&touch_debug)) {
            log_touch_gesture(&touch_debug);
        }
        {
            int resume_pause = 0;
            int leave_paused_game = 0;
            if ((input.pressed & LITE_INPUT_PAUSE) != 0u) {
                if (screen == SCREEN_PAUSE) {
                    resume_pause = 1;
                } else if (game_loaded &&
                           (screen == SCREEN_BUY ||
                            screen == SCREEN_BUY_ITEMS ||
                            screen == SCREEN_PLAY ||
                            screen == SCREEN_ROUND_END)) {
                    paused_screen = screen;
                    paused_selection = selection;
                    paused_menu_count = menu_count;
                    paused_at = now;
                    screen = SCREEN_PAUSE;
                    selection = 0u;
                    menu_count = 2u;
                    input.down = 0u;
                    input.pressed = 0u;
                } else if (screen == SCREEN_MAIN) {
                    running = 0;
                }
            }
            if (screen == SCREEN_PAUSE && !resume_pause) {
                if ((input.pressed &
                     (LITE_INPUT_UP | LITE_INPUT_LEFT)) != 0u) {
                    selection = selection == 0u ? 1u : 0u;
                }
                if ((input.pressed &
                     (LITE_INPUT_DOWN | LITE_INPUT_RIGHT)) != 0u) {
                    selection = selection == 0u ? 1u : 0u;
                }
                if (touch_new && input.touch_y >= 190 &&
                    input.touch_y < 230) {
                    if (input.touch_x >= 34 && input.touch_x < 154) {
                        selection = 0u;
                        resume_pause = 1;
                    } else if (input.touch_x >= 166 &&
                               input.touch_x < 288) {
                        selection = 1u;
                        leave_paused_game = 1;
                    }
                }
                if ((input.pressed & LITE_INPUT_FIRE) != 0u) {
                    if (selection == 0u) {
                        resume_pause = 1;
                    } else {
                        leave_paused_game = 1;
                    }
                }
            }
            if (resume_pause) {
                uint32_t elapsed = now - paused_at;
                uint32_t index;
                pause_shift_timestamp(&next_logic, elapsed);
                pause_shift_timestamp(&fire_until, elapsed);
                pause_shift_timestamp(&hit_until, elapsed);
                pause_shift_timestamp(&fire_started, elapsed);
                pause_shift_timestamp(&next_fire, elapsed);
                pause_shift_timestamp(&round_end_at, elapsed);
                pause_shift_timestamp(&round_deadline, elapsed);
                pause_shift_timestamp(&buy_deadline, elapsed);
                pause_shift_timestamp(&freeze_until, elapsed);
                pause_shift_timestamp(&flash_until, elapsed);
                pause_shift_timestamp(&next_step_sound, elapsed);
                pause_shift_timestamp(&weapon_press_started, elapsed);
                pause_shift_timestamp(&burst_next_fire, elapsed);
                pause_shift_timestamp(&view_animation.next_frame, elapsed);
                for (index = 0u; index < BOT_COUNT; ++index) {
                    pause_shift_timestamp(
                        &bots[index].next_fire, elapsed
                    );
                    pause_shift_timestamp(
                        &bots[index].next_decision, elapsed
                    );
                    pause_shift_timestamp(
                        &bots[index].next_visibility_check, elapsed
                    );
                    pause_shift_timestamp(
                        &bots[index].flash_until, elapsed
                    );
                    pause_shift_timestamp(
                        &bots[index].last_enemy_seen, elapsed
                    );
                }
                pause_shift_timestamp(&bomb.action_started, elapsed);
                pause_shift_timestamp(&bomb.explode_at, elapsed);
                pause_shift_timestamp(&bomb.next_beep, elapsed);
                for (index = 0u; index < GRENADE_MAX; ++index) {
                    pause_shift_timestamp(
                        &grenades[index].detonate_at, elapsed
                    );
                }
                for (index = 0u; index < CORPSE_MAX; ++index) {
                    pause_shift_timestamp(
                        &corpses[index].died_at, elapsed
                    );
                }
                for (index = 0u; index < DROPPED_WEAPON_MAX; ++index) {
                    pause_shift_timestamp(
                        &dropped[index].dropped_at, elapsed
                    );
                }
                for (index = 0u; index < IMPACT_MAX; ++index) {
                    pause_shift_timestamp(
                        &impacts[index].until, elapsed
                    );
                }
                pause_shift_timestamp(&kill_feed.until, elapsed);
                screen = paused_screen;
                selection = paused_selection;
                menu_count = paused_menu_count;
                paused_at = 0u;
                input.down = 0u;
                input.pressed = 0u;
            } else if (leave_paused_game) {
                g_menu_background = 0;
                if (map_memory) {
                    bda_free(map_memory);
                    map_memory = 0;
                }
                if (texture_memory) {
                    bda_free(texture_memory);
                    texture_memory = 0;
                }
                map_memory_bytes = 0u;
                texture_memory_bytes = MENU_TEXTURE_ARENA_BYTES;
                lite_arena_init(&map_arena, 0, 0u);
                texture_memory = (uint8_t *)bda_alloc(
                    texture_memory_bytes
                );
                if (texture_memory) {
                    lite_arena_init(
                        &texture_arena, texture_memory,
                        texture_memory_bytes
                    );
                    if (!load_menu_background(&texture_arena)) {
                        lite_log_line(
                            "historical menu splash unavailable"
                        );
                    }
                } else {
                    lite_arena_init(&texture_arena, 0, 0u);
                    lite_log_line("menu texture allocation failed");
                }
                game_loaded = 0;
                reloading = 0;
                burst_remaining = 0u;
                pending_controls = 0u;
                pending_alt_attack = 0;
                paused_at = 0u;
                screen = SCREEN_MAIN;
                selection = 0u;
                menu_count = 3u;
                input.down = 0u;
                input.pressed = 0u;
            }
        }
        if ((screen == SCREEN_BUY || screen == SCREEN_BUY_ITEMS) &&
            game_loaded &&
            buy_deadline != 0u &&
            time_reached(now, buy_deadline)) {
            screen = SCREEN_PLAY;
            selection = 0u;
            next_logic = now;
            if (round_deadline == 0u) {
                round_deadline = freeze_until + ROUND_TIME_MS;
            }
        }

        if (screen == SCREEN_MAIN || screen == SCREEN_OPTIONS ||
            screen == SCREEN_MAP ||
            screen == SCREEN_TEAM || screen == SCREEN_BUY ||
            screen == SCREEN_BUY_ITEMS) {
            if ((input.pressed & LITE_INPUT_UP) != 0u) {
                selection = selection == 0u ?
                    menu_count - 1u : selection - 1u;
            }
            if ((input.pressed & LITE_INPUT_DOWN) != 0u) {
                selection = (selection + 1u) % menu_count;
            }
            if (touch_new) {
                uint32_t item = menu_touch_item(
                    screen, input.touch_x, input.touch_y,
                    selection, buy_category
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
                        if (map_memory) {
                            bda_free(map_memory);
                            map_memory = 0;
                        }
                        map_memory_bytes = g_map_arena_bytes[map_id];
                        map_memory = (uint8_t *)bda_alloc(map_memory_bytes);
                        if (!map_memory) {
                            draw_loading(
                                &framebuffer, "NOT ENOUGH MAP MEMORY",
                                lite_rgb565(235u, 78u, 64u)
                            );
                            (void)lite_platform_present(g_screen);
                            lite_log_u32(
                                "map_alloc_failed", map_memory_bytes
                            );
                            running = 0;
                            continue;
                        }
                        if (texture_memory) {
                            bda_free(texture_memory);
                            texture_memory = 0;
                        }
                        texture_memory_bytes =
                            g_texture_arena_bytes[map_id];
                        if (map_memory_bytes + texture_memory_bytes >
                            MAP_WORKING_SET_LIMIT_BYTES) {
                            lite_log_line(
                                "map working set budget invalid"
                            );
                            running = 0;
                            continue;
                        }
                        texture_memory = (uint8_t *)bda_alloc(
                            texture_memory_bytes
                        );
                        if (!texture_memory) {
                            draw_loading(
                                &framebuffer,
                                "NOT ENOUGH TEXTURE MEMORY",
                                lite_rgb565(235u, 78u, 64u)
                            );
                            (void)lite_platform_present(g_screen);
                            lite_log_u32(
                                "texture_alloc_failed",
                                texture_memory_bytes
                            );
                            running = 0;
                            continue;
                        }
                        lite_arena_init(
                            &map_arena, map_memory, map_memory_bytes
                        );
                        lite_arena_init(
                            &texture_arena, texture_memory,
                            texture_memory_bytes
                        );
                        lite_log_u32(
                            "map_alloc_bytes", map_memory_bytes
                        );
                        lite_log_u32(
                            "texture_alloc_bytes",
                            texture_memory_bytes
                        );
                        lite_log_u32(
                            "map_working_set_bytes",
                            map_memory_bytes + texture_memory_bytes
                        );
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
                            lite_log_flush();
                            while (lite_platform_pump(&input)) {
                                lite_platform_delay(1u);
                            }
                            running = 0;
                            continue;
                        }
                        bda_memset(owned, 0, sizeof(owned));
                        bda_memset(
                            weapon_ammo, 0, sizeof(weapon_ammo)
                        );
                        bda_memset(
                            weapon_reserve, 0, sizeof(weapon_reserve)
                        );
                        bda_memset(
                            grenade_counts, 0, sizeof(grenade_counts)
                        );
                        bda_memset(grenades, 0, sizeof(grenades));
                        bda_memset(corpses, 0, sizeof(corpses));
                        bda_memset(dropped, 0, sizeof(dropped));
                        bda_memset(impacts, 0, sizeof(impacts));
                        bda_memset(&kill_feed, 0, sizeof(kill_feed));
                        owned[WEAPON_KNIFE] = 1u;
                        owned[WEAPON_GLOCK] =
                            player_team == TEAM_T;
                        owned[WEAPON_USP] =
                            player_team == TEAM_CT;
                        initialize_round(
                            &player, bots, &bomb, player_team,
                            bot_difficulty, map_id, now,
                            &player_health
                        );
                        freeze_until = now + FREEZE_TIME_MS;
                        round_reason = ROUND_REASON_NONE;
                        spectator_target = 0xffu;
                        player_was_alive = 1u;
                        {
                            uint32_t bot_index;
                            for (bot_index = 0u;
                                 bot_index < BOT_COUNT; ++bot_index) {
                                last_bot_alive[bot_index] =
                                    bots[bot_index].alive;
                            }
                        }
                        initialize_hostages(hostages);
                        g_map.dynamic_open_bits = 0u;
                        g_map.dynamic_broken_bits = 0u;
                        bda_memset(
                            g_map.dynamic_damage, 0,
                            sizeof(g_map.dynamic_damage)
                        );
                        bda_memset(
                            g_map.dynamic_position, 0,
                            sizeof(g_map.dynamic_position)
                        );
                        t_animation.action = 0xffu;
                        ct_animation.action = 0xffu;
                        update_world_animations(
                            &t_animation, &ct_animation, bots, now
                        );
                        c15_player_camera(&player, &camera);
                        camera.focal_length = DEFAULT_FOCAL_LENGTH;
                        weapon_ammo[weapon] =
                            g_weapon_capacity[weapon];
                        weapon_reserve[weapon] =
                            g_weapon_reserve_max[weapon] / 2u;
                        player_armor = 0u;
                        player_helmet = 0u;
                        player_defuse_kit = 0u;
                        zoomed = 0;
                        silenced = 0;
                        glock_burst = 0;
                        burst_remaining = 0u;
                        recoil_q8 = 0u;
                        flash_until = 0u;
                        reloading = 0;
                        start_view_animation(
                            &view_animation,
                            C15_VIEW_ANIMATION_DRAW, now
                        );
                        game_loaded = 1;
                        screen = SCREEN_BUY;
                        buy_category = BUY_CATEGORY_PISTOLS;
                        selection = buy_category;
                        menu_count = BUY_CATEGORY_COUNT;
                        next_logic = now;
                        buy_deadline = now + BUY_TIME_MS;
                        round_deadline = 0u;
                        lite_log_u32("pak_bytes", g_pak.file_size);
                        lite_log_u32("map_id", map_id);
                        lite_log_u32(
                            "map_source_crc32", g_map.source_crc32
                        );
                        lite_log_u32(
                            "map_peak_bytes", (uint32_t)map_arena.peak
                        );
                        lite_log_u32(
                            "map_capacity_bytes", map_memory_bytes
                        );
                        lite_log_u32(
                            "map_visibility_resident",
                            g_map.visibility_section &&
                            g_map.visibility_section->data ? 1u : 0u
                        );
                        lite_log_u32(
                            "visible_surface_capacity",
                            C15_MAP_VISIBLE_BYTES * 8u
                        );
                        lite_log_u32(
                            "texture_peak_bytes",
                            (uint32_t)texture_arena.peak
                        );
                        lite_log_u32(
                            "texture_capacity_bytes",
                            texture_memory_bytes
                        );
                        lite_log_u32(
                            "texture_resident_if_eager_bytes",
                            g_map.texture_resident_bytes
                        );
                        lite_log_u32(
                            "texture_streaming",
                            g_map.stream_textures
                        );
                        lite_log_u32(
                            "persistent_model_bytes",
                            (uint32_t)persistent_models
                        );
                        lite_log_u32(
                            "persistent_animation_bytes",
                            (uint32_t)g_persistent_animation_bytes
                        );
                        lite_log_u32(
                            "animation_resident_bytes",
                            (uint32_t)g_animation_arena.used
                        );
                    }
                } else if (screen == SCREEN_BUY) {
                    if (activated == BUY_ITEM_START) {
                        screen = SCREEN_PLAY;
                        selection = 0u;
                        next_logic = now;
                        if (round_deadline == 0u) {
                            round_deadline =
                                freeze_until + ROUND_TIME_MS;
                        }
                    } else if (activated < BUY_CATEGORY_COUNT) {
                        buy_category = activated;
                        screen = SCREEN_BUY_ITEMS;
                        selection = 0u;
                        menu_count =
                            g_buy_category_item_count[buy_category];
                    }
                } else if (screen == SCREEN_BUY_ITEMS) {
                    uint32_t buy_item = buy_category_item(
                        buy_category, activated
                    );
                    if (activated == BUY_ITEM_START) {
                        screen = SCREEN_PLAY;
                        selection = 0u;
                        next_logic = now;
                        if (round_deadline == 0u) {
                            round_deadline =
                                freeze_until + ROUND_TIME_MS;
                        }
                    } else if (activated == BUY_ACTION_BACK) {
                        screen = SCREEN_BUY;
                        selection = buy_category;
                        menu_count = BUY_CATEGORY_COUNT;
                    } else if (buy_item < WEAPON_COUNT) {
                        if (weapon_allowed_for_team(
                                buy_item, player_team) &&
                            (!owned[buy_item] ||
                             weapon_ammo[buy_item] == 0u) &&
                            money >= g_weapon_price[buy_item]) {
                            money -= g_weapon_price[buy_item];
                            own_weapon_in_slot(owned, buy_item);
                            weapon_ammo[buy_item] =
                                g_weapon_capacity[buy_item];
                            weapon_reserve[buy_item] =
                                g_weapon_reserve_max[buy_item];
                        }
                        if (owned[buy_item]) {
                            change_weapon(
                                &model_arena, persistent_models,
                                &weapon, buy_item
                            );
                            reloading = 0;
                            zoomed = 0;
                            silenced = 0;
                            pending_alt_attack = 0;
                            camera.focal_length =
                                DEFAULT_FOCAL_LENGTH;
                            start_view_animation(
                                &view_animation,
                                C15_VIEW_ANIMATION_DRAW, now
                            );
                        }
                    } else if (buy_item < BUY_ITEM_START) {
                        uint32_t price =
                            equipment_price(
                                buy_item, player_armor
                            );
                        int allowed = 1;
                        if (buy_item == BUY_ITEM_DEFUSE &&
                            player_team != TEAM_CT) {
                            allowed = 0;
                        }
                        if (buy_item == BUY_ITEM_HE &&
                            grenade_counts[GRENADE_HE] >= 1u) {
                            allowed = 0;
                        }
                        if (buy_item == BUY_ITEM_FLASH &&
                            grenade_counts[GRENADE_FLASH] >= 2u) {
                            allowed = 0;
                        }
                        if (buy_item == BUY_ITEM_SMOKE &&
                            grenade_counts[GRENADE_SMOKE] >= 1u) {
                            allowed = 0;
                        }
                        if (buy_item == BUY_ITEM_ARMOR &&
                            player_armor >= 100u) {
                            allowed = 0;
                        }
                        if (buy_item == BUY_ITEM_HELMET &&
                            player_helmet) {
                            allowed = 0;
                        }
                        if (buy_item == BUY_ITEM_DEFUSE &&
                            player_defuse_kit) {
                            allowed = 0;
                        }
                        if (allowed && money >= price) {
                            money -= price;
                            if (buy_item == BUY_ITEM_HE) {
                                ++grenade_counts[GRENADE_HE];
                            } else if (buy_item == BUY_ITEM_FLASH) {
                                ++grenade_counts[GRENADE_FLASH];
                            } else if (buy_item == BUY_ITEM_SMOKE) {
                                ++grenade_counts[GRENADE_SMOKE];
                            } else if (buy_item == BUY_ITEM_ARMOR) {
                                player_armor = 100u;
                                c15_audio_play(
                                    &audio, C15_SOUND_CUE_ARMOR,
                                    C15_SOUND_CHANNEL_PLAYER
                                );
                            } else if (buy_item == BUY_ITEM_HELMET) {
                                player_helmet = 1u;
                                if (player_armor < 100u) {
                                    player_armor = 100u;
                                }
                            } else if (buy_item == BUY_ITEM_DEFUSE) {
                                player_defuse_kit = 1u;
                            } else if (buy_item == BUY_ITEM_AMMO) {
                                uint32_t ammo_weapon;
                                for (ammo_weapon = 0u;
                                     ammo_weapon < WEAPON_COUNT;
                                     ++ammo_weapon) {
                                    if (owned[ammo_weapon]) {
                                        weapon_reserve[ammo_weapon] =
                                            g_weapon_reserve_max[
                                                ammo_weapon
                                            ];
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (game_loaded && screen != SCREEN_PAUSE) {
            uint32_t logic_started = lite_platform_milliseconds();
            uint32_t logic_steps = 0u;
            int completed_animation = update_view_animation(
                &view_animation, now
            );
            if (completed_animation == C15_VIEW_ANIMATION_RELOAD &&
                reloading) {
                complete_reload(
                    reload_weapon, weapon_ammo, weapon_reserve
                );
                reloading = 0;
            }
            if (player_health != 0u &&
                (input.touch_dx != 0 || input.touch_dy != 0)) {
                c15_player_look(
                    &player, input.touch_dx, input.touch_dy
                );
                c15_player_camera(&player, &camera);
            }
            if (screen == SCREEN_PLAY && player_health == 0u &&
                (input.pressed & LITE_INPUT_FIRE) != 0u) {
                spectator_target = next_spectator_target(
                    bots, player_team, spectator_target
                );
            }
            if (screen == SCREEN_PLAY) {
                bomb_player_update(
                    &bomb, &player, player_team, player_health,
                    player_defuse_kit, &input, now, &audio
                );
            }
            if (screen == SCREEN_PLAY &&
                (input.pressed & LITE_INPUT_USE) != 0u &&
                player_health != 0u &&
                bomb.action_owner != 1u) {
                int used = pickup_weapon(
                    dropped, &player, owned,
                    weapon_ammo, weapon_reserve, &weapon, now
                );
                if (used) {
                    change_weapon(
                        &model_arena, persistent_models,
                        &weapon, weapon
                    );
                    reloading = 0;
                    zoomed = 0;
                    silenced = 0;
                    camera.focal_length = DEFAULT_FOCAL_LENGTH;
                    start_view_animation(
                        &view_animation,
                        C15_VIEW_ANIMATION_DRAW, now
                    );
                }
                if (!used) {
                    used = hostage_player_use(
                        hostages, &player, player_team, &audio
                    );
                }
                if (!used) {
                    used = c15_map_use_dynamic(
                        &g_map, player.x, player.y, player.z + 28
                    );
                }
                if (!used &&
                    time_active(now, buy_deadline) &&
                    c15_map_in_buy_zone(
                        &g_map, player_team,
                        player.x, player.y, player.z)) {
                    screen = SCREEN_BUY;
                    selection = buy_category;
                    menu_count = BUY_CATEGORY_COUNT;
                }
            }
            if (screen == SCREEN_PLAY &&
                (input.pressed & LITE_INPUT_JUMP) != 0u) {
                pending_controls |= C15_MOVE_JUMP;
            }
            if (screen == SCREEN_PLAY && player_health != 0u &&
                (input.pressed & LITE_INPUT_WEAPON) != 0u) {
                weapon_press_started = now;
                weapon_hold_consumed = 0u;
            }
            if (screen == SCREEN_PLAY && player_health != 0u &&
                !weapon_hold_consumed &&
                (input.down & LITE_INPUT_WEAPON) != 0u &&
                time_reached(now, weapon_press_started + 700u) &&
                weapon != WEAPON_KNIFE) {
                drop_weapon(
                    dropped, weapon, weapon_ammo[weapon],
                    weapon_reserve[weapon],
                    player.x, player.y, player.z, now
                );
                owned[weapon] = 0u;
                weapon_ammo[weapon] = 0u;
                weapon_reserve[weapon] = 0u;
                change_weapon(
                    &model_arena, persistent_models,
                    &weapon, WEAPON_KNIFE
                );
                weapon_hold_consumed = 1u;
                reloading = 0;
                start_view_animation(
                    &view_animation, C15_VIEW_ANIMATION_DRAW, now
                );
            }
            if (screen == SCREEN_PLAY && player_health != 0u &&
                !weapon_hold_consumed &&
                (input.released & LITE_INPUT_WEAPON) != 0u) {
                uint32_t candidate = weapon;
                do {
                    candidate =
                        (candidate + 1u) % WEAPON_COUNT;
                } while (!owned[candidate]);
                change_weapon(
                    &model_arena, persistent_models,
                    &weapon, candidate
                );
                reloading = 0;
                start_view_animation(
                    &view_animation, C15_VIEW_ANIMATION_DRAW, now
                );
                zoomed = 0;
                silenced = 0;
                pending_alt_attack = 0;
                camera.focal_length = DEFAULT_FOCAL_LENGTH;
                burst_remaining = 0u;
                fire_until = 0u;
                fire_started = 0u;
            }
            if (screen == SCREEN_PLAY &&
                (input.pressed & LITE_INPUT_RELOAD) != 0u &&
                weapon != WEAPON_KNIFE &&
                weapon_ammo[weapon] < g_weapon_capacity[weapon] &&
                weapon_reserve[weapon] != 0u) {
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
                (input.pressed & LITE_INPUT_ALT) != 0u) {
                if (weapon == WEAPON_KNIFE) {
                    pending_alt_attack = 1;
                } else if (weapon == WEAPON_GLOCK) {
                    glock_burst ^= 1;
                    burst_remaining = 0u;
                } else if (weapon == WEAPON_USP ||
                           weapon == WEAPON_M4A1) {
                    silenced ^= 1;
                    start_view_animation(
                        &view_animation,
                        C15_VIEW_ANIMATION_DRAW, now
                    );
                } else if (weapon == WEAPON_SG552 ||
                           weapon == WEAPON_AUG ||
                           weapon == WEAPON_SCOUT ||
                           weapon == WEAPON_AWP ||
                           weapon == WEAPON_G3SG1 ||
                           weapon == WEAPON_SG550) {
                    zoomed ^= 1;
                    camera.focal_length = zoomed ?
                        ZOOM_FOCAL_LENGTH : DEFAULT_FOCAL_LENGTH;
                } else {
                    uint32_t kind;
                    for (kind = 0u;
                         kind < GRENADE_KIND_COUNT; ++kind) {
                        if (grenade_counts[kind] != 0u &&
                            throw_grenade(
                                grenades, (uint8_t)kind,
                                player_team, 0u, &camera, now)) {
                            --grenade_counts[kind];
                            break;
                        }
                    }
                }
            }
            if (weapon == WEAPON_GLOCK && glock_burst &&
                (input.pressed & LITE_INPUT_FIRE) != 0u &&
                burst_remaining == 0u) {
                burst_remaining = 3u;
                burst_next_fire = now;
            }
            {
                uint32_t fire_logic_started =
                    lite_platform_milliseconds();
                int secondary_attack = pending_alt_attack;
                int fire_requested = secondary_attack;
                if (burst_remaining != 0u &&
                    time_reached(now, burst_next_fire)) {
                    fire_requested = 1;
                } else if (!glock_burst ||
                           weapon != WEAPON_GLOCK) {
                    fire_requested =
                        (input.down & LITE_INPUT_FIRE) != 0u &&
                        ((input.pressed & LITE_INPUT_FIRE) != 0u ||
                         g_weapon_automatic[weapon]);
                }
                if (screen == SCREEN_PLAY &&
                    player_health != 0u &&
                    !time_active(now, freeze_until) &&
                    !reloading &&
                    fire_requested &&
                    time_reached(now, next_fire) &&
                    (weapon == WEAPON_KNIFE ||
                     weapon_ammo[weapon] != 0u)) {
                    uint32_t pellet;
                    uint32_t spread = g_weapon_spread_q8[weapon] +
                        recoil_q8;
                    int any_hit = 0;
                    if ((input.down & (LITE_INPUT_UP |
                                      LITE_INPUT_DOWN |
                                      LITE_INPUT_LEFT |
                                      LITE_INPUT_RIGHT)) != 0u) {
                        spread += g_weapon_spread_q8[weapon] / 2u;
                    }
                    if (weapon == WEAPON_SG552 ||
                        weapon == WEAPON_AUG ||
                        weapon == WEAPON_SCOUT ||
                        weapon == WEAPON_AWP ||
                        weapon == WEAPON_G3SG1 ||
                        weapon == WEAPON_SG550) {
                        spread = zoomed ? spread / 3u :
                            spread * 4u;
                    }
                    if (silenced &&
                        (weapon == WEAPON_USP ||
                         weapon == WEAPON_M4A1)) {
                        spread = spread * 3u / 4u;
                    }
                    if (weapon != WEAPON_KNIFE) {
                        --weapon_ammo[weapon];
                    }
                    ++shots_fired;
                    c15_audio_play(
                        &audio, weapon, C15_SOUND_CHANNEL_PLAYER
                    );
                    start_view_animation(
                        &view_animation,
                        C15_VIEW_ANIMATION_FIRE, now
                    );
                    for (pellet = 0u;
                         pellet < g_weapon_pellets[weapon]; ++pellet) {
                        int32_t random_yaw =
                            (int32_t)next_random(&shot_seed) - 32768;
                        int32_t random_pitch =
                            (int32_t)next_random(&shot_seed) - 32768;
                        uint16_t shot_yaw = (uint16_t)(
                            camera.yaw_q8 +
                            random_yaw * (int32_t)spread / 32768
                        );
                        int16_t shot_pitch = (int16_t)(
                            camera.pitch_q8 +
                            random_pitch * (int32_t)spread / 32768
                        );
                        int hit = player_fire_hit(
                            bots, hostages, &camera,
                            player_team, weapon,
                            shot_yaw, shot_pitch, secondary_attack,
                            silenced,
                            &money, corpses, impacts, &kill_feed,
                            now, &audio
                        );
                        if (hit != 0) any_hit = 1;
                        if (hit == 2) ++player_kills;
                    }
                    if (any_hit) {
                        ++shots_hit;
                        hit_until = now + HIT_FEEDBACK_MS;
                    }
                    recoil_q8 += g_weapon_recoil_q8[weapon];
                    if (recoil_q8 > 1000u) recoil_q8 = 1000u;
                    if (weapon != WEAPON_KNIFE) {
                        int32_t pitch = player.pitch_q8 -
                            (int32_t)g_weapon_recoil_q8[weapon] / 4;
                        if (pitch < -(32 * 256)) pitch = -(32 * 256);
                        player.pitch_q8 = (int16_t)pitch;
                        player.pitch = (int8_t)(pitch >> 8);
                        c15_player_camera(&player, &camera);
                    }
                    crosshair_spread_q8 = spread;
                    fire_started = now;
                    fire_until = now + FIRE_FEEDBACK_MS;
                    if (burst_remaining != 0u) {
                        --burst_remaining;
                        burst_next_fire = now + 90u;
                        next_fire = burst_remaining != 0u ?
                            burst_next_fire :
                            now + g_weapon_interval_ms[weapon];
                    } else {
                        next_fire = now + (
                            secondary_attack &&
                            weapon == WEAPON_KNIFE ?
                                900u :
                                g_weapon_interval_ms[weapon]
                        );
                    }
                    pending_alt_attack = 0;
                }
                pending_fire_logic_ms +=
                    lite_platform_milliseconds() - fire_logic_started;
            }
            while (screen == SCREEN_PLAY &&
                   time_reached(now, next_logic) &&
                   logic_steps < LOGIC_MAX_CATCHUP_STEPS) {
                uint32_t t_alive;
                uint32_t ct_alive;
                uint32_t phase_started =
                    lite_platform_milliseconds();
                if (player_health != 0u &&
                    !time_active(now, freeze_until)) {
                    int32_t old_x = player.x;
                    int32_t old_y = player.y;
                    c15_player_step(
                        &player, &g_map,
                        movement_controls(&input) | pending_controls
                    );
                    pending_controls = 0u;
                    c15_player_camera(&player, &camera);
                    if ((player.x != old_x || player.y != old_y) &&
                        time_reached(now, next_step_sound) &&
                        audio.voices[
                            C15_SOUND_CHANNEL_PLAYER
                        ].remaining == 0u) {
                        c15_audio_play(
                            &audio, C15_SOUND_CUE_FOOTSTEP,
                            C15_SOUND_CHANNEL_PLAYER
                        );
                        next_step_sound = now + 360u;
                    }
                }
                if (recoil_q8 > 38u) recoil_q8 -= 38u;
                else recoil_q8 = 0u;
                crosshair_spread_q8 = recoil_q8;
                pending_player_logic_ms +=
                    lite_platform_milliseconds() - phase_started;
                phase_started = lite_platform_milliseconds();
                c15_map_dynamic_tick(&g_map);
                hostage_logic(
                    hostages, bots, &player, player_team
                );
                grenade_logic(
                    grenades, bots, hostages, player_team,
                    &player_health, &player_armor, player_helmet,
                    &player, &flash_until, corpses, impacts,
                    &kill_feed, &money, &player_kills,
                    &player_deaths, &audio, now
                );
                pending_objective_logic_ms +=
                    lite_platform_milliseconds() - phase_started;
                phase_started = lite_platform_milliseconds();
                bot_sound_weapon = WEAPON_COUNT;
                {
                    uint16_t health_before_bots = player_health;
                if (!time_active(now, freeze_until)) {
                    bot_logic(
                        bots, grenades, hostages, &player, player_team,
                        &player_health, &player_armor, player_helmet,
                        bot_difficulty, map_id, &bomb, now,
                        &bot_shots, &bot_hits, &bot_visibility_traces,
                        &bot_sound_weapon,
                        &player_deaths, corpses, &kill_feed
                    );
                }
                    if (player_health < health_before_bots) {
                        c15_audio_play(
                            &audio,
                            player_health == 0u ?
                                C15_SOUND_CUE_DEATH :
                                C15_SOUND_CUE_PAIN,
                            C15_SOUND_CHANNEL_PLAYER
                        );
                    }
                }
                bomb_bot_update(
                    &bomb, bots, &player, player_team, player_health,
                    now, &audio
                );
                if (bomb.planted && !bomb.defused &&
                    time_reached(now, bomb.next_beep)) {
                    c15_audio_play(
                        &audio, C15_SOUND_CUE_BOMB_BEEP,
                        C15_SOUND_CHANNEL_PLAYER
                    );
                    bomb.next_beep = now + 1000u;
                }
                if (bot_sound_weapon < WEAPON_COUNT) {
                    c15_audio_play(
                        &audio, bot_sound_weapon,
                        C15_SOUND_CHANNEL_BOT
                    );
                }
                pending_bot_logic_ms +=
                    lite_platform_milliseconds() - phase_started;
                phase_started = lite_platform_milliseconds();
                if (player_was_alive && player_health == 0u) {
                    drop_weapon(
                        dropped, weapon, weapon_ammo[weapon],
                        weapon_reserve[weapon],
                        player.x, player.y, player.z, now
                    );
                    if (weapon != WEAPON_KNIFE) {
                        owned[weapon] = 0u;
                        weapon_ammo[weapon] = 0u;
                        weapon_reserve[weapon] = 0u;
                    }
                    spectator_target = next_spectator_target(
                        bots, player_team, 0xffu
                    );
                }
                player_was_alive = player_health != 0u;
                {
                    uint32_t bot_index;
                    for (bot_index = 0u;
                         bot_index < BOT_COUNT; ++bot_index) {
                        if (last_bot_alive[bot_index] &&
                            !bots[bot_index].alive) {
                            drop_weapon(
                                dropped, bots[bot_index].weapon,
                                bots[bot_index].ammo,
                                g_weapon_reserve_max[
                                    bots[bot_index].weapon
                                ] / 2u,
                                bots[bot_index].mover.x,
                                bots[bot_index].mover.y,
                                bots[bot_index].mover.z, now
                            );
                        }
                        last_bot_alive[bot_index] =
                            bots[bot_index].alive;
                    }
                }
                bot_pickup_weapons(bots, dropped);
                update_world_animations(
                    &t_animation, &ct_animation, bots, now
                );
                team_counts(
                    bots, player_team, player_health,
                    &t_alive, &ct_alive
                );
                {
                    int round_finished = 0;
                    uint32_t hostage_total =
                        c15_map_hostage_count(&g_map);
                    uint32_t hostages_rescued =
                        hostage_count_rescued(hostages);
                    if (hostage_total > HOSTAGE_MAX) {
                        hostage_total = HOSTAGE_MAX;
                    }
                    if (bomb.enabled) {
                        if (bomb.defused) {
                            round_winner = TEAM_CT;
                            round_reason = ROUND_REASON_BOMB_DEFUSED;
                            round_finished = 1;
                        } else if (bomb.planted &&
                                   time_reached(now, bomb.explode_at)) {
                            round_winner = TEAM_T;
                            round_reason = ROUND_REASON_TARGET_BOMBED;
                            round_finished = 1;
                            c15_audio_play(
                                &audio, C15_SOUND_CUE_BOMB_EXPLODE,
                                C15_SOUND_CHANNEL_PLAYER
                            );
                            bomb_apply_explosion(
                                &bomb, bots, hostages, &player,
                                &player_health, &player_deaths,
                                corpses, &kill_feed, now
                            );
                            add_impact(
                                impacts, bomb.x, bomb.y,
                                bomb.z + 12, 2u, now
                            );
                        } else if (ct_alive == 0u) {
                            round_winner = TEAM_T;
                            round_reason = ROUND_REASON_ELIMINATION;
                            round_finished = 1;
                        } else if (!bomb.planted && t_alive == 0u) {
                            round_winner = TEAM_CT;
                            round_reason = ROUND_REASON_ELIMINATION;
                            round_finished = 1;
                        } else if (!bomb.planted &&
                                   round_deadline != 0u &&
                                   time_reached(now, round_deadline)) {
                            round_winner = TEAM_CT;
                            round_reason = ROUND_REASON_TARGET_SAVED;
                            round_finished = 1;
                        }
                    } else if (hostage_total != 0u &&
                               hostages_rescued >= hostage_total) {
                        round_winner = TEAM_CT;
                        round_reason = ROUND_REASON_HOSTAGES_RESCUED;
                        round_finished = 1;
                    } else if (t_alive == 0u || ct_alive == 0u ||
                               (round_deadline != 0u &&
                                time_reached(now, round_deadline))) {
                        if (t_alive == 0u || ct_alive == 0u) {
                            round_winner =
                                t_alive != 0u ? TEAM_T : TEAM_CT;
                            round_reason = ROUND_REASON_ELIMINATION;
                        } else if (hostage_total != 0u) {
                            round_winner = TEAM_T;
                            round_reason = ROUND_REASON_TIME_EXPIRED;
                        } else {
                            round_winner =
                                t_alive >= ct_alive ? TEAM_T : TEAM_CT;
                            round_reason = ROUND_REASON_TIME_EXPIRED;
                        }
                        round_finished = 1;
                    }
                    if (round_finished) {
                        /*
                         * The bomb explosion is evaluated after the regular
                         * death-transition pass. Capture any new deaths now
                         * so their weapons still exist during the end camera.
                         */
                        if (player_was_alive && player_health == 0u) {
                            drop_weapon(
                                dropped, weapon, weapon_ammo[weapon],
                                weapon_reserve[weapon],
                                player.x, player.y, player.z, now
                            );
                            player_was_alive = 0u;
                            spectator_target = next_spectator_target(
                                bots, player_team, 0xffu
                            );
                        }
                        {
                            uint32_t bot_index;
                            for (bot_index = 0u;
                                 bot_index < BOT_COUNT; ++bot_index) {
                                if (last_bot_alive[bot_index] &&
                                    !bots[bot_index].alive) {
                                    drop_weapon(
                                        dropped,
                                        bots[bot_index].weapon,
                                        bots[bot_index].ammo,
                                        g_weapon_reserve_max[
                                            bots[bot_index].weapon
                                        ] / 2u,
                                        bots[bot_index].mover.x,
                                        bots[bot_index].mover.y,
                                        bots[bot_index].mover.z, now
                                    );
                                    last_bot_alive[bot_index] = 0u;
                                }
                            }
                        }
                        if (round_winner == TEAM_T) ++rounds_t;
                        else ++rounds_ct;
                        if (round_winner == player_team) {
                            money += 3250u;
                        } else {
                            money += 1400u;
                        }
                        if (money > 16000u) money = 16000u;
                        c15_audio_play(
                            &audio,
                            round_winner == TEAM_T ?
                                C15_SOUND_CUE_T_WIN :
                                C15_SOUND_CUE_CT_WIN,
                            C15_SOUND_CHANNEL_PLAYER
                        );
                        screen = SCREEN_ROUND_END;
                        round_end_at = now + ROUND_END_MS;
                    }
                }
                pending_objective_logic_ms +=
                    lite_platform_milliseconds() - phase_started;
                next_logic += LOGIC_INTERVAL_MS;
                ++logic_steps;
            }
            if (screen == SCREEN_PLAY &&
                time_reached(now, next_logic)) {
                uint32_t skipped =
                    (now - next_logic) / LOGIC_INTERVAL_MS + 1u;
                next_logic += skipped * LOGIC_INTERVAL_MS;
                logic_skipped_steps += skipped;
                logic_skipped_steps_window += skipped;
            }
            pending_logic_steps += logic_steps;
            if (screen == SCREEN_ROUND_END &&
                time_reached(now, round_end_at)) {
                int player_survived = player_health != 0u;
                ++round;
                if (!player_survived) {
                    uint32_t inventory_index;
                    bda_memset(owned, 0, sizeof(owned));
                    bda_memset(
                        weapon_ammo, 0, sizeof(weapon_ammo)
                    );
                    bda_memset(
                        weapon_reserve, 0, sizeof(weapon_reserve)
                    );
                    bda_memset(
                        grenade_counts, 0, sizeof(grenade_counts)
                    );
                    owned[WEAPON_KNIFE] = 1u;
                    weapon = player_team == TEAM_T ?
                        WEAPON_GLOCK : WEAPON_USP;
                    owned[weapon] = 1u;
                    weapon_ammo[weapon] =
                        g_weapon_capacity[weapon];
                    weapon_reserve[weapon] =
                        g_weapon_reserve_max[weapon] / 2u;
                    player_armor = 0u;
                    player_helmet = 0u;
                    player_defuse_kit = 0u;
                    for (inventory_index = 0u;
                         inventory_index < WEAPON_COUNT;
                         ++inventory_index) {
                        if (!owned[inventory_index]) {
                            weapon_ammo[inventory_index] = 0u;
                            weapon_reserve[inventory_index] = 0u;
                        }
                    }
                    change_weapon(
                        &model_arena, persistent_models,
                        &weapon, weapon
                    );
                }
                initialize_round(
                    &player, bots, &bomb, player_team,
                    bot_difficulty, map_id, now,
                    &player_health
                );
                initialize_hostages(hostages);
                bda_memset(grenades, 0, sizeof(grenades));
                bda_memset(corpses, 0, sizeof(corpses));
                bda_memset(dropped, 0, sizeof(dropped));
                bda_memset(impacts, 0, sizeof(impacts));
                bda_memset(&kill_feed, 0, sizeof(kill_feed));
                g_map.dynamic_open_bits = 0u;
                g_map.dynamic_broken_bits = 0u;
                bda_memset(
                    g_map.dynamic_damage, 0,
                    sizeof(g_map.dynamic_damage)
                );
                bda_memset(
                    g_map.dynamic_position, 0,
                    sizeof(g_map.dynamic_position)
                );
                t_animation.action = 0xffu;
                ct_animation.action = 0xffu;
                update_world_animations(
                    &t_animation, &ct_animation, bots, now
                );
                c15_player_camera(&player, &camera);
                camera.focal_length = DEFAULT_FOCAL_LENGTH;
                freeze_until = now + FREEZE_TIME_MS;
                round_reason = ROUND_REASON_NONE;
                spectator_target = 0xffu;
                player_was_alive = 1u;
                {
                    uint32_t bot_index;
                    for (bot_index = 0u;
                         bot_index < BOT_COUNT; ++bot_index) {
                        last_bot_alive[bot_index] =
                            bots[bot_index].alive;
                    }
                }
                zoomed = 0;
                burst_remaining = 0u;
                recoil_q8 = 0u;
                flash_until = 0u;
                reloading = 0;
                start_view_animation(
                    &view_animation, C15_VIEW_ANIMATION_DRAW, now
                );
                screen = SCREEN_BUY;
                selection = buy_category;
                menu_count = BUY_CATEGORY_COUNT;
                next_logic = now;
                buy_deadline = now + BUY_TIME_MS;
                round_deadline = 0u;
            }
            pending_logic_ms +=
                lite_platform_milliseconds() - logic_started;
        }

        if (game_loaded && player_health == 0u &&
            (screen == SCREEN_PLAY || screen == SCREEN_ROUND_END)) {
            if (spectator_target >= BOT_COUNT ||
                !bots[spectator_target].alive) {
                spectator_target = next_spectator_target(
                    bots, player_team, spectator_target
                );
            }
            spectator_camera(&camera, bots, spectator_target);
        }

        if (time_reached(now, next_frame)) {
            uint32_t render_started;
            uint32_t render_finished;
            uint32_t frame_end;
            uint32_t frame_elapsed;
            uint32_t instant_fps_x10;
            int rendered_game = 0;
            ++frame;
            next_frame += FRAME_INTERVAL_MS;
            if (time_reached(now, next_frame)) {
                next_frame = now + FRAME_INTERVAL_MS;
            }
            render_started = lite_platform_milliseconds();
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
                draw_buy_category_menu(
                    &framebuffer, money, selection
                );
            } else if (screen == SCREEN_BUY_ITEMS) {
                draw_buy_item_menu(
                    &framebuffer, money, selection, buy_category,
                    owned, player_team, player_armor
                );
            } else if (screen == SCREEN_PAUSE) {
                draw_pause_menu(
                    &framebuffer, bots, player_team,
                    player_kills, player_deaths,
                    rounds_t, rounds_ct, selection
                );
            } else {
                rendered_game = 1;
                render_game(
                    &framebuffer, &view, &input, now, &camera, &player,
                    bots, player_team, player_health,
                    spectator_target, money, round,
                    fps_x10, map_id, weapon,
                    time_active(now, fire_until),
                    time_active(now, hit_until),
                    (now - fire_started) / 54u,
                    view_animation.action == C15_VIEW_ANIMATION_FIRE ?
                        view_animation.frame : 0u,
                    weapon_ammo[weapon], weapon_reserve[weapon],
                    player_armor, player_defuse_kit, grenade_counts,
                    hostages, grenades, corpses, dropped,
                    impacts, &kill_feed,
                    &bomb, round_deadline, freeze_until,
                    crosshair_spread_q8,
                    flash_until, zoomed, silenced, glock_burst,
                    screen, round_winner, round_reason,
                    &stats, &view_stats, &entity_stats,
                    &frame_timing
                );
            }
            render_finished = lite_platform_milliseconds();
            (void)lite_platform_present(g_screen);
            frame_end = lite_platform_milliseconds();
            {
                uint32_t render_sample =
                    (render_finished - render_started) * 10u;
                uint32_t present_sample =
                    (frame_end - render_finished) * 10u;
                render_ms_x10 = render_ms_x10 == 0u ?
                    render_sample :
                    (render_ms_x10 * 3u + render_sample) / 4u;
                present_ms_x10 = present_ms_x10 == 0u ?
                    present_sample :
                    (present_ms_x10 * 3u + present_sample) / 4u;
            }
            timing_add(&logic_timing, pending_logic_ms);
            timing_add(&fire_logic_timing, pending_fire_logic_ms);
            timing_add(&player_logic_timing, pending_player_logic_ms);
            timing_add(&bot_logic_timing, pending_bot_logic_ms);
            timing_add(
                &objective_logic_timing,
                pending_objective_logic_ms
            );
            timing_add(&logic_steps_timing, pending_logic_steps);
            timing_add(&audio_timing, pending_audio_ms);
            timing_add(
                &present_timing, frame_end - render_finished
            );
            pending_logic_ms = 0u;
            pending_fire_logic_ms = 0u;
            pending_player_logic_ms = 0u;
            pending_bot_logic_ms = 0u;
            pending_objective_logic_ms = 0u;
            pending_logic_steps = 0u;
            pending_audio_ms = 0u;
            if (rendered_game) {
                timing_add(&world_timing, frame_timing.world_ms);
                timing_add(
                    &world_clear_timing,
                    frame_timing.world_clear_ms
                );
                timing_add(
                    &entity_timing, frame_timing.entities_ms
                );
                timing_add(&view_timing, frame_timing.view_ms);
                timing_add(&hud_timing, frame_timing.hud_ms);
            }
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
            lite_log_u32("render_ms_x10", render_ms_x10);
            lite_log_u32("present_ms_x10", present_ms_x10);
            lite_log_u32(
                "logic_avg_ms_x10",
                timing_average_x10(&logic_timing)
            );
            lite_log_u32(
                "logic_max_ms", logic_timing.maximum_ms
            );
            lite_log_u32(
                "logic_fire_avg_ms_x10",
                timing_average_x10(&fire_logic_timing)
            );
            lite_log_u32(
                "logic_fire_max_ms",
                fire_logic_timing.maximum_ms
            );
            lite_log_u32(
                "logic_player_avg_ms_x10",
                timing_average_x10(&player_logic_timing)
            );
            lite_log_u32(
                "logic_player_max_ms",
                player_logic_timing.maximum_ms
            );
            lite_log_u32(
                "logic_bot_avg_ms_x10",
                timing_average_x10(&bot_logic_timing)
            );
            lite_log_u32(
                "logic_bot_max_ms",
                bot_logic_timing.maximum_ms
            );
            lite_log_u32(
                "logic_objective_avg_ms_x10",
                timing_average_x10(&objective_logic_timing)
            );
            lite_log_u32(
                "logic_objective_max_ms",
                objective_logic_timing.maximum_ms
            );
            lite_log_u32(
                "logic_steps_avg_x10",
                timing_average_x10(&logic_steps_timing)
            );
            lite_log_u32(
                "logic_steps_max", logic_steps_timing.maximum_ms
            );
            lite_log_u32(
                "logic_skipped_steps", logic_skipped_steps_window
            );
            lite_log_u32(
                "audio_avg_ms_x10",
                timing_average_x10(&audio_timing)
            );
            lite_log_u32(
                "audio_max_ms", audio_timing.maximum_ms
            );
            lite_log_u32(
                "world_avg_ms_x10",
                timing_average_x10(&world_timing)
            );
            lite_log_u32(
                "world_max_ms", world_timing.maximum_ms
            );
            lite_log_u32(
                "world_clear_avg_ms_x10",
                timing_average_x10(&world_clear_timing)
            );
            lite_log_u32(
                "world_clear_max_ms",
                world_clear_timing.maximum_ms
            );
            lite_log_u32(
                "entities_avg_ms_x10",
                timing_average_x10(&entity_timing)
            );
            lite_log_u32(
                "entities_max_ms", entity_timing.maximum_ms
            );
            lite_log_u32(
                "view_avg_ms_x10",
                timing_average_x10(&view_timing)
            );
            lite_log_u32(
                "view_max_ms", view_timing.maximum_ms
            );
            lite_log_u32(
                "hud_avg_ms_x10",
                timing_average_x10(&hud_timing)
            );
            lite_log_u32(
                "hud_max_ms", hud_timing.maximum_ms
            );
            lite_log_u32(
                "present_avg_ms_x10",
                timing_average_x10(&present_timing)
            );
            lite_log_u32(
                "present_max_ms", present_timing.maximum_ms
            );
            lite_log_u32("screen", (uint32_t)screen);
            lite_log_u32("round", round);
            lite_log_u32("rounds_t", rounds_t);
            lite_log_u32("rounds_ct", rounds_ct);
            lite_log_u32("player_health", player_health);
            lite_log_u32("player_armor", player_armor);
            lite_log_u32("money", money);
            lite_log_u32("shots_fired", shots_fired);
            lite_log_u32("shots_hit", shots_hit);
            lite_log_u32(
                "dropped_weapons", dropped_weapon_count(dropped)
            );
            lite_log_u32("spectator_target", spectator_target);
            lite_log_u32(
                "freeze_remaining_ms",
                time_active(now, freeze_until) ?
                    freeze_until - now : 0u
            );
            lite_log_u32("bot_shots", bot_shots);
            lite_log_u32("bot_hits", bot_hits);
            lite_log_u32(
                "bot_visibility_traces", bot_visibility_traces
            );
            lite_log_u32("bot_difficulty", bot_difficulty);
            lite_log_u32(
                "hostages_rescued",
                hostage_count_rescued(hostages)
            );
            lite_log_u32("dynamic_open_bits", g_map.dynamic_open_bits);
            lite_log_u32(
                "dynamic_broken_bits", g_map.dynamic_broken_bits
            );
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
            lite_log_u32("world_pixels", stats.drawn_pixels);
            lite_log_u32(
                "world_tested_pixels", stats.tested_pixels
            );
            lite_log_u32("visible_surfaces", stats.visible_surfaces);
            lite_log_u32("drawn_surfaces", stats.drawn_surfaces);
            lite_log_u32("entity_triangles", entity_stats.triangles);
            lite_log_u32("entity_pixels", entity_stats.pixels);
            lite_log_u32(
                "entity_pvs_culled",
                frame_timing.entity_pvs_culled
            );
            lite_log_u32("view_triangles", view_stats.triangles);
            lite_log_u32("view_pixels", view_stats.pixels);
            lite_log_u32(
                "texture_cache_bytes", g_map.texture_cache_bytes
            );
            lite_log_u32(
                "texture_cache_reloads", g_map.texture_cache_reloads
            );
            lite_log_i32("bot0_x", bots[0].mover.x);
            lite_log_i32("bot0_y", bots[0].mover.y);
            lite_log_u32("bot0_nav", bots[0].nav_index);
            lite_log_u32("bot0_health", bots[0].health);
            lite_log_i32("bot3_x", bots[3].mover.x);
            lite_log_i32("bot3_y", bots[3].mover.y);
            lite_log_u32("bot3_nav", bots[3].nav_index);
            lite_log_u32("bot3_health", bots[3].health);
            timing_reset(&logic_timing);
            timing_reset(&fire_logic_timing);
            timing_reset(&player_logic_timing);
            timing_reset(&bot_logic_timing);
            timing_reset(&objective_logic_timing);
            timing_reset(&logic_steps_timing);
            timing_reset(&audio_timing);
            timing_reset(&world_timing);
            timing_reset(&world_clear_timing);
            timing_reset(&entity_timing);
            timing_reset(&view_timing);
            timing_reset(&hud_timing);
            timing_reset(&present_timing);
            logic_skipped_steps_window = 0u;
            next_metric = now + METRIC_INTERVAL_MS;
        }
        service_audio_throttled(
            &audio, &g_pak, g_load_scratch,
            sizeof(g_load_scratch), now,
            &next_audio_service, &pending_audio_ms
        );
        lite_platform_delay(1u);
    }
    lite_log_u32("final_frame", frame);
    lite_log_u32("map_peak_bytes", (uint32_t)map_arena.peak);
    lite_log_u32(
        "texture_peak_bytes", (uint32_t)texture_arena.peak
    );
    lite_log_u32(
        "texture_cache_reloads", g_map.texture_cache_reloads
    );
    lite_log_u32("model_peak_bytes", (uint32_t)model_arena.peak);
    lite_log_u32(
        "animation_peak_bytes",
        (uint32_t)g_animation_arena.peak
    );
    lite_log_u32(
        "view_cache_resident_bytes", view_cache_memory_bytes
    );
    lite_log_u32("shots_fired", shots_fired);
    lite_log_u32("shots_hit", shots_hit);
    lite_log_u32(
        "dropped_weapons", dropped_weapon_count(dropped)
    );
    lite_log_u32("spectator_target", spectator_target);
    lite_log_u32("bot_shots", bot_shots);
    lite_log_u32("bot_hits", bot_hits);
    lite_log_u32("bot_visibility_traces", bot_visibility_traces);
    lite_log_u32("logic_skipped_steps", logic_skipped_steps);
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
    lite_log_line("CS15 Lite M20 performance pass 7 stop");
    lite_log_close();
    c15_pak_close(&g_pak);
    if (map_memory) {
        bda_free(map_memory);
    }
    if (texture_memory) {
        bda_free(texture_memory);
    }
    if (audio_memory) {
        bda_free(audio_memory);
    }
    if (view_cache_memory) {
        bda_free(view_cache_memory);
    }
    lite_platform_close();
    return 0;
}
