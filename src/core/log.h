/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CS15_LITE_LOG_H
#define CS15_LITE_LOG_H

#include <stdint.h>

void lite_log_reset(void);
void lite_log_flush(void);
void lite_log_close(void);
void lite_log_line(const char *message);
void lite_log_u32(const char *name, uint32_t value);
void lite_log_i32(const char *name, int32_t value);
void lite_log_hex32(const char *name, uint32_t value);

#endif
