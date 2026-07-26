/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_DISPLAY_H
#define CS15_LITE_DISPLAY_H

#include <stdint.h>

/* Rotate one logical 320x240 frame into a cached 240x320 staging buffer. */
void lite_display_copy_landscape_rgb565(
    const uint16_t *source,
    volatile uint16_t *destination,
    int rotate_180
);

/*
 * Submit a staged 240x320 frame to the firmware scan buffer with contiguous
 * 32-bit stores. rotate_180 matches the C200 GUI+0x738 convention used by
 * the original GAMEBOY.BDA and ps-for9588.
 */
void lite_display_copy_portrait_rgb565(
    const uint16_t *source,
    volatile uint16_t *destination,
    int rotate_180
);

#endif
