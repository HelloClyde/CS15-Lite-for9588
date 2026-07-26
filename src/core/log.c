/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/log.h"

#include "bda_filesystem.h"
#include "bda_types.h"

#define LITE_LOG_DIRECTORY \
    "A:\\\xd3\xa6\xd3\xc3\\\xca\xfd\xbe\xdd\\CS15LITE"
#define LITE_LOG_PATH \
    "A:\\\xd3\xa6\xd3\xc3\\\xca\xfd\xbe\xdd\\CS15LITE\\runtime.log"
#define LITE_LOG_BUFFER_BYTES 256u

static char g_log_buffer[LITE_LOG_BUFFER_BYTES];
static bda_size_t g_log_used;

static bda_size_t text_length(const char *text)
{
    const char *start = text;
    if (!text) {
        return 0u;
    }
    while (*text) {
        ++text;
    }
    return (bda_size_t)(text - start);
}

static void append_bytes(const char *bytes, bda_size_t length)
{
    bda_size_t index;
    if (!bytes || length == 0u) {
        return;
    }
    for (index = 0u; index < length; ++index) {
        if (g_log_used == sizeof(g_log_buffer)) {
            lite_log_flush();
        }
        if (g_log_used < sizeof(g_log_buffer)) {
            g_log_buffer[g_log_used++] = bytes[index];
        }
    }
}

void lite_log_reset(void)
{
    int file;
    (void)bda_fs_mkdir(LITE_LOG_DIRECTORY);
    g_log_used = 0u;
    file = bda_fs_fopen_raw(LITE_LOG_PATH, "wb");
    if (bda_fs_file_is_valid(file)) {
        (void)bda_fs_close_raw(file);
    }
}

void lite_log_flush(void)
{
    int file;
    if (g_log_used == 0u) {
        return;
    }
    file = bda_fs_fopen_raw(LITE_LOG_PATH, "ab");
    if (bda_fs_file_is_valid(file)) {
        if (bda_fs_write_raw(file, g_log_buffer, g_log_used) ==
            (int)g_log_used) {
            g_log_used = 0u;
        }
        (void)bda_fs_close_raw(file);
    }
}

void lite_log_close(void)
{
    lite_log_flush();
}

void lite_log_line(const char *message)
{
    append_bytes(message, text_length(message));
    append_bytes("\r\n", 2u);
}

void lite_log_u32(const char *name, uint32_t value)
{
    char digits[10];
    char output[64];
    uint32_t count = 0u;
    uint32_t out = 0u;

    while (name && *name && out + 1u < sizeof(output)) {
        output[out++] = *name++;
    }
    if (out + 1u < sizeof(output)) {
        output[out++] = '=';
    }
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u && count < sizeof(digits));
    while (count != 0u && out + 1u < sizeof(output)) {
        output[out++] = digits[--count];
    }
    if (out + 2u <= sizeof(output)) {
        output[out++] = '\r';
        output[out++] = '\n';
    }
    append_bytes(output, (bda_size_t)out);
}

void lite_log_i32(const char *name, int32_t value)
{
    char digits[10];
    char output[64];
    uint32_t magnitude = value < 0 ?
        0u - (uint32_t)value : (uint32_t)value;
    uint32_t count = 0u;
    uint32_t out = 0u;

    while (name && *name && out + 1u < sizeof(output)) {
        output[out++] = *name++;
    }
    if (out + 1u < sizeof(output)) {
        output[out++] = '=';
    }
    if (value < 0 && out + 1u < sizeof(output)) {
        output[out++] = '-';
    }
    do {
        digits[count++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u && count < sizeof(digits));
    while (count != 0u && out + 1u < sizeof(output)) {
        output[out++] = digits[--count];
    }
    if (out + 2u <= sizeof(output)) {
        output[out++] = '\r';
        output[out++] = '\n';
    }
    append_bytes(output, (bda_size_t)out);
}

void lite_log_hex32(const char *name, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    char output[64];
    uint32_t out = 0u;
    int shift;

    while (name && *name && out + 1u < sizeof(output)) {
        output[out++] = *name++;
    }
    if (out + 2u < sizeof(output)) {
        output[out++] = '=';
        output[out++] = '0';
        output[out++] = 'x';
    }
    for (shift = 28; shift >= 0 && out + 1u < sizeof(output); shift -= 4) {
        output[out++] = hex[(value >> (uint32_t)shift) & 15u];
    }
    if (out + 2u <= sizeof(output)) {
        output[out++] = '\r';
        output[out++] = '\n';
    }
    append_bytes(output, (bda_size_t)out);
}
