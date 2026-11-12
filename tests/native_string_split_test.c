/* Real cross-region String.split_literal -> owned Array<String> C ABI.
 * This tests the production String + Array runtimes, not a NATS parser.
 */
#include "oreslang/runtime/NativeIntArrayRegion.h"
#include "oreslang/runtime/NativeStringRegion.h"
#include "oreslang/runtime/NativeStringSplit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(v) do { if (!(v)) { \
    fprintf(stderr, "native String split failed at line %d: %s\n", __LINE__, #v); \
    return 1; } } while (0)

static int expect(OresNativeIntArrayRegion *region, uint64_t array,
                  uint64_t at, const char *bytes, size_t size) {
    const int64_t expected = ores_string_literal(bytes, (int64_t)size);
    const int64_t actual = oreslang_int_array_get(region, array, at);
    return expected > 0 && ores_string_equal(actual, expected) == 1;
}
static OresNativeIntArrayHandle new_array(OresNativeIntArrayRegion *region) {
    OresNativeIntArrayHandle owner = 0;
    if (oreslang_int_array_try_new(region, &owner) != ORES_NATIVE_ARRAY_OK)
        return 0;
    return owner;
}
int main(void) {
    CHECK(ores_string_region_begin(0) == 0);
    OresNativeIntArrayRegion *arrays = oreslang_int_array_region_open();
    CHECK(arrays != NULL);
    const int64_t dot = ores_string_literal(".", 1);
    const int64_t double_colon = ores_string_literal("::", 2);
    const int64_t nul = ores_string_literal("\0", 1);
    CHECK(dot && double_colon && nul);
    uint64_t a = new_array(arrays);
    uint64_t b = new_array(arrays);
    uint64_t c = new_array(arrays);
    uint64_t d = new_array(arrays);
    uint64_t e = new_array(arrays);
    CHECK(a && b && c && d && e);
    const int64_t subject = ores_string_literal(".alpha..beta.", 13);
    CHECK(subject);
    CHECK(ores_string_split_literal(arrays, a, subject, dot) == ORES_NATIVE_ARRAY_OK);
    CHECK(oreslang_int_array_length(arrays, a) == 5);
    CHECK(expect(arrays, a, 0, "", 0));
    CHECK(expect(arrays, a, 1, "alpha", 5));
    CHECK(expect(arrays, a, 2, "", 0));
    CHECK(expect(arrays, a, 3, "beta", 4));
    CHECK(expect(arrays, a, 4, "", 0));

    const int64_t none = ores_string_literal("", 0);
    CHECK(ores_string_split_literal(arrays, b, none, dot) == ORES_NATIVE_ARRAY_OK);
    CHECK(oreslang_int_array_length(arrays, b) == 1);
    CHECK(expect(arrays, b, 0, "", 0));

    const int64_t multi = ores_string_literal("a::b::::", 8);
    CHECK(ores_string_split_literal(arrays, c, multi, double_colon) == ORES_NATIVE_ARRAY_OK);
    CHECK(oreslang_int_array_length(arrays, c) == 4);
    CHECK(expect(arrays, c, 0, "a", 1));
    CHECK(expect(arrays, c, 1, "b", 1));
    CHECK(expect(arrays, c, 2, "", 0));
    CHECK(expect(arrays, c, 3, "", 0));

    const char binary[] = {'a', '\0', 'b', '\0', 'c'};
    const int64_t source_binary = ores_string_literal(binary, sizeof binary);
    CHECK(ores_string_split_literal(arrays, d, source_binary, nul) == ORES_NATIVE_ARRAY_OK);
    CHECK(oreslang_int_array_length(arrays, d) == 3);
    CHECK(expect(arrays, d, 0, "a", 1));
    CHECK(expect(arrays, d, 1, "b", 1));
    CHECK(expect(arrays, d, 2, "c", 1));

    /* A 65,537-token request must return LIMIT before emitting any String
     * slice handle or modifying the destination's element count.
     */
    enum { OVERSIZED = 65536 };
    char *oversized = malloc(OVERSIZED);
    CHECK(oversized != NULL);
    memset(oversized, '.', OVERSIZED);
    const int64_t large = ores_string_literal(oversized, OVERSIZED);
    CHECK(large != 0);
    CHECK(ores_string_split_literal(arrays, e, large, dot) == ORES_NATIVE_ARRAY_LIMIT);
    CHECK(oreslang_int_array_length(arrays, e) == 0);
    /* A pathological long shared-prefix comparison is bounded BEFORE
     * any array element or String view publication, even with few tokens.
     */
    enum { LONG_INPUT = 131072, LONG_SEPARATOR = 4096 };
    char *adversarial_input = malloc(LONG_INPUT);
    char *adversarial_separator = malloc(LONG_SEPARATOR);
    CHECK(adversarial_input && adversarial_separator);
    memset(adversarial_input, 'a', LONG_INPUT);
    memset(adversarial_separator, 'a', LONG_SEPARATOR);
    adversarial_separator[LONG_SEPARATOR - 1] = 'b';
    const int64_t hostile = ores_string_literal(adversarial_input, LONG_INPUT);
    const int64_t hostile_delimiter =
        ores_string_literal(adversarial_separator, LONG_SEPARATOR);
    CHECK(hostile && hostile_delimiter);
    CHECK(ores_string_split_literal(arrays, e, hostile, hostile_delimiter)
        == ORES_NATIVE_ARRAY_LIMIT);
    CHECK(oreslang_int_array_length(arrays, e) == 0);
    /* Rejected work/token budgets cannot poison subsequent admission. */
    CHECK(ores_string_split_literal(arrays, e, subject, dot) == ORES_NATIVE_ARRAY_OK);
    CHECK(oreslang_int_array_length(arrays, e) == 5);
    oreslang_int_array_drop(arrays, e);
    oreslang_int_array_drop(arrays, d);
    oreslang_int_array_drop(arrays, c);
    oreslang_int_array_drop(arrays, b);
    oreslang_int_array_drop(arrays, a);
    oreslang_int_array_region_close(arrays);
    CHECK(ores_string_region_end() == 0);
    /* Borrowed byte spans supplied to ores_string_literal must outlive the
     * complete String invocation, including its region_end() teardown.
     */
    free(oversized);
    free(adversarial_input);
    free(adversarial_separator);
    puts("NATIVE_STRING_SPLIT_OWNED_ARRAY_C_ABI_PASS");
    return 0;
}
