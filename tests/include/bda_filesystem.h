#ifndef CS15_LITE_HOST_BDA_FILESYSTEM_H
#define CS15_LITE_HOST_BDA_FILESYSTEM_H

#include <stdint.h>
#include <stdio.h>

typedef int32_t s32;
typedef uint32_t bda_size_t;

#define BDA_SEEK_SET 0
#define BDA_SEEK_CUR 1
#define BDA_SEEK_END 2

static FILE *host_bda_files[4];

static inline int bda_fs_fopen_raw(const char *path, const char *mode)
{
    int index;
    for (index = 1; index < 4; ++index) {
        if (!host_bda_files[index]) {
            host_bda_files[index] = fopen(path, mode);
            return host_bda_files[index] ? index : 0;
        }
    }
    return 0;
}

static inline int bda_fs_file_is_valid(int file)
{
    return file > 0 && file < 4 && host_bda_files[file] != 0;
}

static inline int bda_fs_close_raw(int file)
{
    int result;
    if (!bda_fs_file_is_valid(file)) {
        return -1;
    }
    result = fclose(host_bda_files[file]);
    host_bda_files[file] = 0;
    return result;
}

static inline int bda_fs_read_raw(
    int file, void *buffer, bda_size_t size
)
{
    if (!bda_fs_file_is_valid(file)) {
        return -1;
    }
    return (int)fread(buffer, 1u, size, host_bda_files[file]);
}

static inline int bda_fs_seek_raw(int file, s32 offset, int whence)
{
    int origin;
    long position;
    if (!bda_fs_file_is_valid(file)) {
        return -1;
    }
    origin = whence == BDA_SEEK_SET ? SEEK_SET :
        (whence == BDA_SEEK_CUR ? SEEK_CUR : SEEK_END);
    if (fseek(host_bda_files[file], offset, origin) != 0) {
        return -1;
    }
    position = ftell(host_bda_files[file]);
    return position < 0 ? -1 : (int)position;
}

#endif
