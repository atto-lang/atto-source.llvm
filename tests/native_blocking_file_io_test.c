#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeBlockingFileIo.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void require_ok(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main(void) {
    char path[] = "/tmp/oreslang-native-io-XXXXXX";
    int fd = mkstemp(path);
    require_ok(fd >= 0, "mkstemp");
    unlink(path);

    require_ok(oreslang_file_size_blocking(fd) == 0, "initial size");
    require_ok(oreslang_file_write_i64_blocking(fd, 0, INT64_C(0x0102030405060708)) == 1,
               "write fixed-width value");
    require_ok(oreslang_file_size_blocking(fd) == 8, "size after write");
    require_ok(oreslang_file_sync_blocking(fd) == 1, "fsync");

    unsigned char actual[8] = {0};
    require_ok(pread(fd, actual, sizeof(actual), 0) == 8, "pread");
    const unsigned char expected[8] = {8,7,6,5,4,3,2,1};
    require_ok(memcmp(actual, expected, sizeof(expected)) == 0,
               "stable little-endian encoding");

    int64_t decoded = 0;
    require_ok(oreslang_file_read_i64_blocking(fd, 0, &decoded) == 1,
               "checked read succeeds");
    require_ok(decoded == INT64_C(0x0102030405060708),
               "checked read preserves payload");

    require_ok(oreslang_file_write_i64_blocking(fd, 8, INT64_MIN) == 1,
               "write signed-min payload");
    decoded = 0;
    require_ok(oreslang_file_read_i64_blocking(fd, 8, &decoded) == 1,
               "checked read signed-min succeeds");
    require_ok(decoded == INT64_MIN, "checked read preserves all i64 bit patterns");

    require_ok(oreslang_file_truncate_blocking(fd, 4) == 1, "partial record truncate");
    decoded = INT64_C(1234567);
    require_ok(oreslang_file_read_i64_blocking(fd, 0, &decoded) == 0,
               "partial read fails closed");
    require_ok(decoded == INT64_C(1234567),
               "failed read does not publish partial output");

    require_ok(oreslang_file_truncate_blocking(fd, 0) == 1, "truncate");
    require_ok(oreslang_file_size_blocking(fd) == 0, "size after truncate");
    decoded = INT64_C(91);
    require_ok(oreslang_file_read_i64_blocking(-1, 0, &decoded) == 0, "bad read fd fails");
    require_ok(decoded == INT64_C(91), "bad read fd preserves output");
    require_ok(oreslang_file_read_i64_blocking(fd, -1, &decoded) == 0, "bad read offset fails");
    require_ok(decoded == INT64_C(91), "bad read offset preserves output");
    require_ok(oreslang_file_read_i64_blocking(fd, 0, NULL) == 0, "null read output fails");
    require_ok(oreslang_file_write_i64_blocking(-1, 0, 1) == 0, "bad fd fails");
    require_ok(oreslang_file_write_i64_blocking(fd, -1, 1) == 0, "bad offset fails");
    require_ok(oreslang_file_truncate_blocking(fd, -1) == 0, "bad size fails");
    require_ok(oreslang_file_size_blocking(-1) == -1, "bad size query fails");
    require_ok(oreslang_file_sync_blocking(-1) == 0, "bad fsync fails");
    require_ok(oreslang_dir_sync_blocking(-1) == 0, "bad dir fsync fails");

    close(fd);
    puts("ORESLANG_NATIVE_BLOCKING_FILE_IO_OK");
    return 0;
}
