/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_SOUND_H
#define CS15_LITE_SOUND_H

#include <stdint.h>

#include "assets/pak.h"

#define C15_SOUND_CUE_RELOAD 5u
#define C15_SOUND_CUE_COUNT 6u
#define C15_SOUND_CHANNEL_PLAYER 0u
#define C15_SOUND_CHANNEL_BOT 1u

typedef struct c15_sound_voice {
    uint32_t offset;
    uint32_t remaining;
} c15_sound_voice_t;

typedef struct c15_audio {
    const c15_pak_entry_t *bank;
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
