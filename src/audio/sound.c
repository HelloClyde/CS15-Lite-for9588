/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "audio/sound.h"

#include "bda_audio.h"
#include "bda_memory.h"

#define C15_SOUND_HEADER_BYTES \
    (16u + C15_SOUND_CUE_COUNT * 8u)
#define C15_SOUND_BLOCK_BYTES 1024u

static uint16_t sound_u16(const uint8_t *bytes)
{
    return (uint16_t)(
        (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8)
    );
}

static uint32_t sound_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static int sound_magic(const uint8_t *bytes)
{
    static const char expected[8] = {
        'C','1','5','S','N','D','1','\0'
    };
    uint32_t index;
    for (index = 0u; index < 8u; ++index) {
        if (bytes[index] != (uint8_t)expected[index]) {
            return 0;
        }
    }
    return 1;
}

int c15_audio_init(
    c15_audio_t *audio,
    const c15_pak_t *pak,
    void *scratch,
    uint32_t scratch_size
)
{
    c15_pak_entry_t entry;
    uint8_t *header = (uint8_t *)scratch;
    uint32_t index;
    bda_memset(audio, 0, sizeof(*audio));
    if (!c15_pak_find(pak, "sound/game", &entry) ||
        entry.type != C15_FOURCC('S','N','D','0') ||
        scratch_size < C15_SOUND_HEADER_BYTES ||
        entry.packed_size < C15_SOUND_HEADER_BYTES ||
        !c15_pak_read(
            pak, &entry, 0u, header, C15_SOUND_HEADER_BYTES) ||
        !sound_magic(header) ||
        sound_u32(header + 8u) != 11025u ||
        sound_u16(header + 12u) != C15_SOUND_CUE_COUNT ||
        sound_u16(header + 14u) != 0u) {
        return 0;
    }
    for (index = 0u; index < C15_SOUND_CUE_COUNT; ++index) {
        uint32_t offset = sound_u32(header + 16u + index * 8u);
        uint32_t size = sound_u32(header + 20u + index * 8u);
        if ((offset & 1u) != 0u || (size & 1u) != 0u ||
            offset < C15_SOUND_HEADER_BYTES ||
            offset > entry.packed_size ||
            size > entry.packed_size - offset) {
            return 0;
        }
        audio->cue_offsets[index] = offset;
        audio->cue_sizes[index] = size;
    }
    audio->bank = entry;
    audio->loaded = 1u;
    return 1;
}

void c15_audio_set_enabled(
    c15_audio_t *audio,
    int enabled,
    void *scratch,
    uint32_t scratch_size
)
{
    if (!enabled) {
        c15_audio_stop(audio, scratch, scratch_size);
        audio->enabled = 0u;
        return;
    }
    audio->enabled = 1u;
    if (audio->loaded && !audio->opened) {
        audio->original_attenuation = bda_audio_get_attenuation();
        audio->playback_attenuation =
            audio->original_attenuation < 0 ||
            audio->original_attenuation >
                (int)BDA_AUDIO_ATTENUATION_HALF_SCALE ?
            (int)BDA_AUDIO_ATTENUATION_HALF_SCALE :
            audio->original_attenuation;
        bda_audio_open_pcm(
            BDA_AUDIO_SAMPLE_RATE_22050,
            BDA_AUDIO_BITS_16,
            BDA_AUDIO_CHANNELS_MONO
        );
        bda_audio_set_attenuation(
            (uint32_t)audio->playback_attenuation
        );
        audio->opened = 1u;
    }
}

void c15_audio_play(
    c15_audio_t *audio, uint32_t cue, uint32_t channel
)
{
    if (!audio->enabled || !audio->opened ||
        cue >= C15_SOUND_CUE_COUNT || channel >= 2u) {
        return;
    }
    audio->voices[channel].offset = audio->cue_offsets[cue];
    audio->voices[channel].remaining = audio->cue_sizes[cue];
}

int c15_audio_service(
    c15_audio_t *audio,
    const c15_pak_t *pak,
    void *scratch,
    uint32_t scratch_size
)
{
    int16_t *output = (int16_t *)scratch;
    int16_t *source = (int16_t *)(
        (uint8_t *)scratch + C15_SOUND_BLOCK_BYTES
    );
    uint32_t advances[2] = {0u, 0u};
    uint32_t channel;
    uint32_t active = 0u;
    int written;
    if (!audio->enabled || !audio->opened || !audio->loaded ||
        scratch_size < C15_SOUND_BLOCK_BYTES * 2u) {
        return 0;
    }
    if (audio->voices[0].remaining == 0u &&
        audio->voices[1].remaining == 0u) {
        return 0;
    }
    if (!bda_audio_ready()) {
        ++audio->ready_polls;
        return 0;
    }
    bda_memset(output, 0, C15_SOUND_BLOCK_BYTES);
    for (channel = 0u; channel < 2u; ++channel) {
        c15_sound_voice_t *voice = &audio->voices[channel];
        uint32_t bytes;
        uint32_t sample;
        if (voice->remaining == 0u) {
            continue;
        }
        bytes = voice->remaining < C15_SOUND_BLOCK_BYTES / 2u ?
            voice->remaining : C15_SOUND_BLOCK_BYTES / 2u;
        if (!c15_pak_read(
                pak, &audio->bank, voice->offset, source, bytes)) {
            voice->remaining = 0u;
            continue;
        }
        for (sample = 0u; sample < bytes / 2u; ++sample) {
            uint32_t first = sample * 2u;
            uint32_t second = first + 1u;
            int32_t mixed =
                (int32_t)output[first] + source[sample];
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            output[first] = (int16_t)mixed;
            mixed = (int32_t)output[second] + source[sample];
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            output[second] = (int16_t)mixed;
        }
        advances[channel] = bytes;
        ++active;
    }
    if (active == 0u) {
        return 0;
    }
    written = bda_audio_write(output, C15_SOUND_BLOCK_BYTES);
    if (written != (int)C15_SOUND_BLOCK_BYTES) {
        ++audio->short_writes;
        return -1;
    }
    for (channel = 0u; channel < 2u; ++channel) {
        c15_sound_voice_t *voice = &audio->voices[channel];
        voice->offset += advances[channel];
        voice->remaining -= advances[channel];
    }
    ++audio->blocks_written;
    return 1;
}

void c15_audio_stop(
    c15_audio_t *audio, void *scratch, uint32_t scratch_size
)
{
    uint32_t polls = 0u;
    audio->voices[0].remaining = 0u;
    audio->voices[1].remaining = 0u;
    if (audio->opened) {
        bda_audio_set_attenuation(
            (uint32_t)audio->original_attenuation
        );
        if (scratch && scratch_size >= C15_SOUND_BLOCK_BYTES) {
            while (!bda_audio_ready() && polls < 65535u) {
                ++polls;
            }
            if (bda_audio_ready()) {
                bda_memset(scratch, 0, C15_SOUND_BLOCK_BYTES);
                (void)bda_audio_write(
                    scratch, C15_SOUND_BLOCK_BYTES
                );
            }
        }
        bda_audio_stop();
        audio->opened = 0u;
    }
}
