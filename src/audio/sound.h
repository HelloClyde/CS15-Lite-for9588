/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_SOUND_H
#define CS15_LITE_SOUND_H

#include <stdint.h>

#include "assets/pak.h"

#define C15_SOUND_CUE_RELOAD 23u
#define C15_SOUND_CUE_BOMB_PLANT 24u
#define C15_SOUND_CUE_BOMB_BEEP 25u
#define C15_SOUND_CUE_BOMB_EXPLODE 26u
#define C15_SOUND_CUE_BOMB_DISARM 27u
#define C15_SOUND_CUE_BOMB_DISARMED 28u
#define C15_SOUND_CUE_FOOTSTEP 29u
#define C15_SOUND_CUE_HIT_FLESH 30u
#define C15_SOUND_CUE_HEADSHOT 31u
#define C15_SOUND_CUE_DEATH 32u
#define C15_SOUND_CUE_RICOCHET 33u
#define C15_SOUND_CUE_GRENADE_BOUNCE 34u
#define C15_SOUND_CUE_HE_EXPLODE 35u
#define C15_SOUND_CUE_FLASH_EXPLODE 36u
#define C15_SOUND_CUE_SMOKE 37u
#define C15_SOUND_CUE_HOSTAGE 38u
#define C15_SOUND_CUE_CT_WIN 39u
#define C15_SOUND_CUE_T_WIN 40u
#define C15_SOUND_CUE_ARMOR 41u
#define C15_SOUND_CUE_PAIN 42u
#define C15_SOUND_CUE_COUNT 43u
#define C15_SOUND_CHANNEL_PLAYER 0u
#define C15_SOUND_CHANNEL_BOT 1u

typedef struct c15_sound_voice {
    uint32_t offset;
    uint32_t remaining;
} c15_sound_voice_t;

typedef struct c15_audio {
    c15_pak_entry_t bank;
    uint32_t cue_offsets[C15_SOUND_CUE_COUNT];
    uint32_t cue_sizes[C15_SOUND_CUE_COUNT];
    c15_sound_voice_t voices[2];
    uint32_t blocks_written;
    uint32_t short_writes;
    uint32_t ready_polls;
    int original_attenuation;
    int playback_attenuation;
    uint8_t loaded;
    uint8_t enabled;
    uint8_t opened;
} c15_audio_t;

int c15_audio_init(
    c15_audio_t *audio,
    const c15_pak_t *pak,
    void *scratch,
    uint32_t scratch_size
);
void c15_audio_set_enabled(
    c15_audio_t *audio,
    int enabled,
    void *scratch,
    uint32_t scratch_size
);
void c15_audio_play(
    c15_audio_t *audio, uint32_t cue, uint32_t channel
);
int c15_audio_service(
    c15_audio_t *audio,
    const c15_pak_t *pak,
    void *scratch,
    uint32_t scratch_size
);
void c15_audio_stop(
    c15_audio_t *audio, void *scratch, uint32_t scratch_size
);

#endif
