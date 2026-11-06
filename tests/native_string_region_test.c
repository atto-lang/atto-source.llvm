#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeStringRegion.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

static long long now_ms(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void read_exact(int fd, const char *expected) {
    size_t size = strlen(expected), got = 0;
    char bytes[256];
    assert(size < sizeof bytes);
    while (got < size) {
        ssize_t count = read(fd, bytes + got, size - got);
        assert(count > 0);
        got += (size_t)count;
    }
    assert(memcmp(bytes, expected, size) == 0);
}

static char big[1 << 20];

int main(void) {
    for (unsigned iteration = 0; iteration != 1000; ++iteration) {
        assert(ores_string_region_begin(0) == 0);
        assert(ores_string_region_begin(0) == 1); /* cannot overwrite live owners */
        int64_t a = ores_string_literal("a\0b", 3);
        int64_t b = ores_string_literal("c", 1);
        assert(a > 0 && b > a);
        int64_t joined = ores_string_concat(a, b);
        assert(joined > b);
        int64_t expected_joined = ores_string_literal("a\0bc", 4);
        assert(expected_joined > joined && ores_string_equal(joined, expected_joined) == 1);
        int64_t same = ores_string_literal("a\0b", 3);
        assert(same != a && ores_string_equal(a, same) == 1);
        assert(ores_string_equal(a, b) == 0);
        assert(ores_string_equal(a, ores_string_literal("a", 1)) == 0);
        assert(ores_string_equal(ores_string_literal(NULL, 0), ores_string_literal("", 0)) == 1);
        assert(ores_string_region_end() == 0);
        assert(ores_string_region_end() == 1);
    }
    assert(ores_string_region_begin(0) == 0);
    assert(ores_string_equal(0, ores_string_literal("", 0)) == 0);
    assert(ores_string_region_end() == 1); /* invalid handles cannot forge equality */
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

    /* A panic drains output queued before it, and int/bool text is exact. */
    assert(pipe(output) == 0);
    assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
    close(output[1]);
    assert(ores_string_region_begin(1) == 0);
    assert(ores_string_print(ores_string_from_i64(INT64_MIN), 1) == 0);
    assert(ores_string_print(ores_string_from_i64(0), 0) == 0);
    assert(ores_string_print(ores_string_from_bool(1), 0) == 0);
    assert(ores_string_print(ores_string_from_bool(0), 1) == 0);
    ores_string_panic(NULL, 0);
    assert(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);
    read_exact(output[0], "-9223372036854775808\n0truefalse\n");
    assert(ores_string_region_end() == 0);
    close(output[0]);

    /* A stdout nobody reads cannot stall a panic past its 1 s drain budget
     * (the per-write timeout alone would wait 5 s). */
    assert(pipe(output) == 0);
    assert(dup2(output[1], STDOUT_FILENO) == STDOUT_FILENO);
    close(output[1]);
    memset(big, 'x', sizeof big);
    assert(ores_string_region_begin(1) == 0);
    assert(ores_string_print(ores_string_literal(big, (int64_t)sizeof big), 0) == 0);
    const long long started = now_ms();
    ores_string_panic("", 0);
    const long long elapsed = now_ms() - started;
    assert(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);
    assert(elapsed < 3000);
    assert(ores_string_region_end() == 1); /* undrained output is reported */
    close(output[0]);
    close(saved);
    return 0;
}
