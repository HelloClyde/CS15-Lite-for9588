#include "app/damage.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    uint16_t armor;
    uint32_t damage;

    /* AWP keeps its classic one-shot torso kill through full Kevlar. */
    armor = 100u;
    damage = c15_damage_apply_bullet(
        115u, 2u, &armor, 1u, 250u
    );
    assert(damage == 112u);
    assert(armor == 98u);

    /* It also remains lethal near the end of the Lite hitscan range. */
    damage = c15_damage_range_adjusted(115u, 253u, 2048u);
    assert(damage == 111u);
    armor = 100u;
    damage = c15_damage_apply_bullet(
        damage, 2u, &armor, 1u, 250u
    );
    assert(damage == 108u);

    /* A leg hit is intentionally not a one-shot and does not use armor. */
    armor = 100u;
    damage = c15_damage_apply_bullet(
        115u, 3u, &armor, 1u, 250u
    );
    assert(damage == 87u);
    assert(armor == 100u);

    /* AK helmet headshots kill; M4 helmet headshots do not. */
    armor = 100u;
    damage = c15_damage_apply_bullet(
        36u, 1u, &armor, 1u, 198u
    );
    assert(damage == 111u);
    armor = 100u;
    damage = c15_damage_apply_bullet(
        32u, 1u, &armor, 1u, 179u
    );
    assert(damage == 89u);

    /* Kevlar does not protect an unhelmeted head. */
    armor = 100u;
    damage = c15_damage_apply_bullet(
        36u, 1u, &armor, 0u, 198u
    );
    assert(damage == 144u);
    assert(armor == 100u);

    puts("damage_test: PASS");
    return 0;
}
