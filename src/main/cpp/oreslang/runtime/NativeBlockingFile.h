#ifndef ORESLANG_NATIVE_BLOCKING_FILE_H
#define ORESLANG_NATIVE_BLOCKING_FILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stable scalar status contract for the first host-native file capability.
// Open returns a nonnegative fd on success. Other operations return 1.
// Reads return a nonnegative u63 value, or one of the negative statuses.
enum OresNativeFileStatus {
    ORES_NATIVE_FILE_EOF = -1,
    ORES_NATIVE_FILE_IO_ERROR = -2,
    ORES_NATIVE_FILE_CORRUPT = -3,
    ORES_NATIVE_FILE_INVALID = -4,
    ORES_NATIVE_FILE_SECURITY = -5
};

int64_t oreslang_file_open_read_blocking(const char* basename);
int64_t oreslang_file_open_append_blocking(const char* basename);
int64_t oreslang_file_open_replace_blocking(const char* basename);
int64_t oreslang_file_write_u63_blocking(int64_t fd, int64_t value);
int64_t oreslang_file_read_u63_blocking(int64_t fd);
int64_t oreslang_file_sync_blocking(int64_t fd);
int64_t oreslang_file_close_blocking(int64_t fd);
int64_t oreslang_file_rename_blocking(const char* from_basename, const char* to_basename);
int64_t oreslang_dir_sync_cwd_blocking(void);

#ifdef __cplusplus
}
#endif
#endif
