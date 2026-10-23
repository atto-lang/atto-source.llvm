#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeStringRegion.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    for (unsigned iteration = 0; iteration != 1000; ++iteration) {
        assert(ores_string_region_begin(0) == 0);
        assert(ores_string_region_begin(0) == 1); /* cannot overwrite live owners */
        int64_t a = ores_string_literal("a\0b", 3);
        int64_t b = ores_string_literal("c", 1);
        assert(a > 0 && b > a);
        assert(ores_string_concat(a, b) > b);
        assert(ores_string_region_end() == 0);
        assert(ores_string_region_end() == 1);
    }
    assert(ores_string_region_begin(0) == 0);
    assert(ores_string_concat(-1, 1) == 0);
    assert(ores_string_region_end() == 1);
    assert(ores_string_region_begin(0) == 0);
    /* Oversized borrowed data is rejected before accessing any payload. */
    assert(ores_string_literal("", 64 * 1024 * 1024 + 1) == 0);
    assert(ores_string_region_end() == 1);
    assert(ores_string_region_begin(0) == 0);
    for (unsigned i = 0; i != 65536; ++i) assert(ores_string_literal("", 0) > 0);
    assert(ores_string_literal("", 0) == 0);
    assert(ores_string_region_end() == 1);
    assert(ores_string_region_begin(0) == 0);
    assert(ores_string_region_end() == 0); /* failures do not poison next root */

    int saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    int output[2];
    assert(pipe(output) == 0);
    assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
    close(output[1]);
    for (unsigned i = 0; i != 500; ++i) {
        assert(ores_string_region_begin(1) == 0);
        int64_t last = ores_string_literal("z", 1);
        assert(ores_string_print(last, 0) == 0);
        assert(ores_string_region_end() == 0);
    }
    assert(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);
    char bytes[512];
    size_t received = 0;
    ssize_t count;
    while ((count = read(output[0], bytes, sizeof bytes)) > 0) {
        for (ssize_t i = 0; i != count; ++i) assert(bytes[i] == 'z');
        received += (size_t)count;
    }
    assert(count == 0 && received == 500); /* final output survives immediate end */
    close(output[0]);

    assert(pipe(output) == 0);
    assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
    close(output[1]); close(output[0]); /* broken output is failure, not SIGPIPE death */
    assert(ores_string_region_begin(1) == 0);
    assert(ores_string_print(ores_string_literal("closed", 6), 0) == 0);
    assert(ores_string_region_end() == 1);
    assert(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);

    char path[] = "native-string-regular-output.XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0);
    assert(unlink(path) == 0);
    assert(dup2(file, STDOUT_FILENO) == STDOUT_FILENO);
    close(file);
    assert(ores_string_region_begin(1) == 1); /* strict NIO: no regular-file fallback */
    assert(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved);
    return 0;
}
