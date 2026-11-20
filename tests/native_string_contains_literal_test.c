/* Native String.contains_literal ABI qualification. Real implementation
 * linked from NativeStringRegion.c; no host substitute. Tested without GC.
 */
#include "oreslang/runtime/NativeStringRegion.h"
#include <stdint.h>
#include <stdio.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "native String.contains_literal failed at line %d: %s\n", \
            __LINE__, #expr); return 1; \
} } while (0)

int main(void) {
    const char embedded[] = {'a', '\0', 'b', '\0', 'z'};
    const char edge[] = {'a', 'b', 'c', 'z'};
    CHECK(ores_string_region_begin(0) == 0);
    const int64_t haystack = ores_string_literal("orders.created.eu", 17);
    const int64_t created = ores_string_literal("created", 7);
    const int64_t dot = ores_string_literal(".", 1);
    const int64_t absent = ores_string_literal("payments", 8);
    const int64_t empty = ores_string_literal("", 0);
    const int64_t nul = ores_string_literal(embedded, sizeof embedded);
    const int64_t nul_needle = ores_string_literal("\0b\0", 3);
    const int64_t missing_nul = ores_string_literal("\0bq", 3);
    const int64_t boundary = ores_string_literal(edge, 3);
    const int64_t beyond = ores_string_literal("cz", 2);
    const int64_t repeated = ores_string_literal("aaaaaaab", 8);
    const int64_t tail = ores_string_literal("aaab", 4);
    CHECK(haystack && created && dot && absent && empty && nul && nul_needle);
    CHECK(boundary && beyond && repeated && tail);
    CHECK(ores_string_contains_literal(haystack, created) == 1);
    CHECK(ores_string_contains_literal(haystack, dot) == 1);
    CHECK(ores_string_contains_literal(haystack, absent) == 0);
    CHECK(ores_string_contains_literal(haystack, empty) == 1);
    CHECK(ores_string_contains_literal(empty, empty) == 1);
    CHECK(ores_string_contains_literal(empty, created) == 0);
    CHECK(ores_string_contains_literal(nul, nul_needle) == 1);
    CHECK(ores_string_contains_literal(nul, missing_nul) == 0);
    CHECK(ores_string_contains_literal(boundary, beyond) == 0);
    CHECK(ores_string_contains_literal(repeated, tail) == 1);
    CHECK(ores_string_region_end() == 0);

    /* Invalid/null authority must fail closed. The zero return cannot
     * upgrade a bad handle to a valid subscription/delivery decision. */
    CHECK(ores_string_region_begin(0) == 0);
    CHECK(ores_string_contains_literal(0, 1) == 0);
    CHECK(ores_string_region_end() != 0);
    return 0;
}
