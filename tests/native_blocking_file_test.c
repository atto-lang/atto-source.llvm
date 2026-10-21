#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeBlockingFile.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void cleanup(void) {
    (void)unlink("ores-native-io-test.wal");
    (void)unlink("ores-native-io-test.tmp");
    (void)unlink("ores-native-io-test-link.wal");
    (void)unlink("ores-native-io-test-symlink.wal");
}

int main(void) {
    cleanup();

    int64_t fd = oreslang_file_open_append_blocking("ores-native-io-test.wal");
    assert(fd >= 0);
    assert(oreslang_file_write_u63_blocking(fd, 7) == 1);
    assert(oreslang_file_write_u63_blocking(fd, INT64_MAX) == 1);
    assert(oreslang_file_write_u63_blocking(fd, -1) == ORES_NATIVE_FILE_INVALID);
    assert(oreslang_file_sync_blocking(fd) == 1);
    assert(oreslang_file_close_blocking(fd) == 1);
    assert(oreslang_dir_sync_cwd_blocking() == 1);

    fd = oreslang_file_open_read_blocking("ores-native-io-test.wal");
    assert(fd >= 0);
    assert(oreslang_file_read_u63_blocking(fd) == 7);
    assert(oreslang_file_read_u63_blocking(fd) == INT64_MAX);
    assert(oreslang_file_read_u63_blocking(fd) == ORES_NATIVE_FILE_EOF);
    assert(oreslang_file_close_blocking(fd) == 1);

    fd = oreslang_file_open_replace_blocking("ores-native-io-test.tmp");
    assert(fd >= 0);
    assert(oreslang_file_write_u63_blocking(fd, 99) == 1);
    assert(oreslang_file_sync_blocking(fd) == 1);
    assert(oreslang_file_close_blocking(fd) == 1);
    assert(oreslang_file_rename_blocking("ores-native-io-test.tmp", "ores-native-io-test.wal") == 1);
    assert(oreslang_dir_sync_cwd_blocking() == 1);

    fd = oreslang_file_open_read_blocking("ores-native-io-test.wal");
    assert(fd >= 0);
    assert(oreslang_file_read_u63_blocking(fd) == 99);
    assert(oreslang_file_read_u63_blocking(fd) == ORES_NATIVE_FILE_EOF);
    assert(oreslang_file_close_blocking(fd) == 1);

    assert(oreslang_file_open_read_blocking("../escape") == ORES_NATIVE_FILE_INVALID);
    assert(oreslang_file_open_read_blocking("sub/path") == ORES_NATIVE_FILE_INVALID);

    assert(symlink("ores-native-io-test.wal", "ores-native-io-test-symlink.wal") == 0);
    assert(oreslang_file_open_read_blocking("ores-native-io-test-symlink.wal") == ORES_NATIVE_FILE_SECURITY);

    assert(link("ores-native-io-test.wal", "ores-native-io-test-link.wal") == 0);
    assert(oreslang_file_open_read_blocking("ores-native-io-test.wal") == ORES_NATIVE_FILE_SECURITY);

    cleanup();
    puts("ORES_NATIVE_BLOCKING_FILE_OK");
    return 0;
}
