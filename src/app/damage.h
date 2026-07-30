/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_DAMAGE_H
#define CS15_LITE_DAMAGE_H

#include <stdint.h>

uint32_t c15_damage_range_adjusted(
    uint32_t damage,
    uint16_t range_modifier_q8,
    uint32_t distance
);

uint32_t c15_damage_apply_bullet(
    uint32_t base,
    uint8_t hitgroup,
    uint16_t *armor,
    uint8_t helmet,
    uint16_t armor_ratio_q8
);

#endif
