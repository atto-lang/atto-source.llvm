#ifndef ORESLANG_NATIVE_BLOCKING_FILE_IO_H
#define ORESLANG_NATIVE_BLOCKING_FILE_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hosted POSIX runtime ABI for compiler-owned Oreslang blocking file intrinsics.
// File and directory descriptors are capabilities provisioned by the launcher.
// Mutating operations and checked reads return 1 on success and 0 on failure.
// file_read_i64 writes the payload only after all eight bytes are available.
// file_size returns a nonnegative byte length, or -1 on failure.
int64_t oreslang_file_read_i64_blocking(int64_t fd, int64_t offset, int64_t* out_value);
int64_t oreslang_file_write_i64_blocking(int64_t fd, int64_t offset, int64_t value);
int64_t oreslang_file_size_blocking(int64_t fd);
int64_t oreslang_file_truncate_blocking(int64_t fd, int64_t size);
int64_t oreslang_file_sync_blocking(int64_t fd);
int64_t oreslang_dir_sync_blocking(int64_t fd);

#ifdef __cplusplus
}
#endif

#endif
