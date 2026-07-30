/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "app/damage.h"

uint32_t c15_damage_range_adjusted(
    uint32_t damage,
    uint16_t range_modifier_q8,
    uint32_t distance
)
{
    uint32_t steps = distance / 500u;
    while (steps-- != 0u) {
        damage = (damage * range_modifier_q8 + 128u) >> 8;
    }
    return damage == 0u ? 1u : damage;
}

uint32_t c15_damage_apply_bullet(
    uint32_t base,
    uint8_t hitgroup,
    uint16_t *armor,
    uint8_t helmet,
    uint16_t armor_ratio_q8
)
{
    uint32_t damage = base;
    int protected_hit = 0;

    if (hitgroup == 1u) {
        damage *= 4u;
        protected_hit = helmet != 0u;
    } else if (hitgroup == 3u) {
        damage = (damage * 3u + 3u) / 4u;
    } else {
        protected_hit = 1;
    }

    if (armor && *armor != 0u && protected_hit) {
        uint32_t health_damage;
        uint32_t prevented;
        uint32_t armor_cost;

        if (armor_ratio_q8 > 256u) {
            armor_ratio_q8 = 256u;
        }
        health_damage = (damage * armor_ratio_q8) >> 8;
        if (health_damage == 0u) {
            health_damage = 1u;
        }
        prevented = damage - health_damage;
        armor_cost = (prevented + 1u) / 2u;

        if (armor_cost > *armor) {
            uint32_t armor_prevention = (uint32_t)*armor * 2u;
            health_damage = damage > armor_prevention ?
                damage - armor_prevention : 1u;
            *armor = 0u;
        } else {
            *armor = (uint16_t)(*armor - armor_cost);
        }
        damage = health_damage;
    }
    return damage == 0u ? 1u : damage;
}
