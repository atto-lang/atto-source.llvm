#include "ControlPlaneArena32.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)
int main(void) {
    CHECK(strcmp(ATTO_CONTROL_REF32_ABI, "control_generation12_slot20_v1") == 0);
    CHECK(ATTO_CONTROL_RECORD_BYTES == 32u);
    AttoControlPlaneArena32 *a = atto_control32_new(2), *b = atto_control32_new(2);
    CHECK(a && b);
    uint64_t parent = 0, child = 0, foreign = 0, value = 0;
    int64_t scalar = 0;
    CHECK(atto_control32_allocate(a, &parent));
    CHECK(atto_control32_allocate(a, &child));
    CHECK(atto_control32_allocate(b, &foreign));
    CHECK((uint32_t)parent == (uint32_t)foreign);
    CHECK(!atto_control32_read_i64(b, parent, 0, &scalar));
    CHECK(!atto_control32_write_ref(a, parent, foreign));
    CHECK(atto_control32_write_i64(a, parent, 0, INT64_C(9000000000)));
    CHECK(atto_control32_read_i64(a, parent, 0, &scalar) && scalar == INT64_C(9000000000));
    CHECK(atto_control32_write_i64(a, parent, 8, INT64_MIN));
    CHECK(atto_control32_read_i64(a, parent, 8, &scalar) && scalar == INT64_MIN);
    CHECK(atto_control32_write_ref(a, parent, child));
    CHECK(atto_control32_read_ref(a, parent, &value) && value == child);
    CHECK(!atto_control32_write_i64(a, parent, 16, 1));
    value = UINT64_MAX;
    CHECK(!atto_control32_allocate(a, &value) && value == 0);
    CHECK(atto_control32_drop(a, child));
    CHECK(!atto_control32_read_i64(a, child, 0, &scalar));
    value = UINT64_MAX;
    CHECK(!atto_control32_read_ref(a, parent, &value) && value == 0);
    uint64_t replacement = 0;
    CHECK(atto_control32_allocate(a, &replacement) && replacement != child);
    CHECK(atto_control32_write_ref(a, parent, replacement));
    CHECK(atto_control32_read_ref(a, parent, &value) && value == replacement);
    CHECK(atto_control32_drop(a, parent));
    CHECK(atto_control32_drop(a, replacement));
    CHECK(!atto_control32_drop(a, replacement));
    AttoControlPlaneArena32 *limited = atto_control32_new(1);
    CHECK(limited);
    uint64_t token = 0;
    CHECK(atto_control32_allocate(limited, &token));
    for (int i = 1; i < 4095; ++i) {
        CHECK(atto_control32_drop(limited, token));
        CHECK(atto_control32_allocate(limited, &token));
    }
    CHECK(atto_control32_drop(limited, token));
    CHECK(!atto_control32_allocate(limited, &token) && token == 0);
    CHECK(atto_control32_read_i64(b, foreign, 0, &scalar) && scalar == 0);
    atto_control32_retire(a);
    atto_control32_retire(a);
    CHECK(!atto_control32_write_i64(a, parent, 0, 1));
    atto_control32_destroy(limited);
    atto_control32_destroy(b);
    atto_control32_destroy(a);
    puts("PASS control-plane-ref32 llvm C11 native host 12 guarded scenarios; NO compiled Atto guest or zero-GC proof");
    return 0;
}
