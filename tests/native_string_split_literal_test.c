#include "oreslang/runtime/NativeStringRegion.h"
#include "oreslang/runtime/NativeStringSplitRegion.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "split ABI check line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static int token_equals(OresNativeIntArrayRegion *arrays, uint64_t array,
        uint64_t index, const char *expected, uint64_t count) {
    const uint64_t handle = (uint64_t)oreslang_int_array_get(arrays, array, index);
    const char *bytes = NULL; uint64_t size = 0;
    if (ores_string_region_borrow_span((int64_t)handle, &bytes, &size)) return 0;
    return count == size && (!count || memcmp(bytes, expected, (size_t)count) == 0);
}
int main(void) {
    CHECK(ores_string_region_begin(0) == 0);
    OresNativeIntArrayRegion *arrays = oreslang_int_array_region_open();
    CHECK(arrays);
    uint64_t owner = oreslang_int_array_new(arrays);
    CHECK(owner);
    const int64_t source = ores_string_literal("orders..created.", 16);
    const int64_t dot = ores_string_literal(".", 1);
    CHECK(source && dot);
    CHECK(ores_string_split_literal_fill(source, dot, arrays, owner) == ORES_NATIVE_ARRAY_OK);
    CHECK(oreslang_int_array_length(arrays, owner) == 4);
    CHECK(token_equals(arrays, owner, 0, "orders", 6));
    CHECK(token_equals(arrays, owner, 1, "", 0));
    CHECK(token_equals(arrays, owner, 2, "created", 7));
    CHECK(token_equals(arrays, owner, 3, "", 0));
    oreslang_int_array_drop(arrays, owner);

    owner = oreslang_int_array_new(arrays);
    const int64_t multi = ores_string_literal("a::b::::c", 9);
    const int64_t sep = ores_string_literal("::", 2);
    CHECK(ores_string_split_literal_fill(multi, sep, arrays, owner) == ORES_NATIVE_ARRAY_OK);
    CHECK(oreslang_int_array_length(arrays, owner) == 4);
    CHECK(token_equals(arrays, owner, 0, "a", 1));
    CHECK(token_equals(arrays, owner, 1, "b", 1));
    CHECK(token_equals(arrays, owner, 2, "", 0));
    CHECK(token_equals(arrays, owner, 3, "c", 1));
    oreslang_int_array_drop(arrays, owner);

    owner = oreslang_int_array_new(arrays);
    const char embedded[] = {'a','\0','b','.', 'c','\0','d'};
    const int64_t binary = ores_string_literal(embedded, sizeof embedded);
    CHECK(ores_string_split_literal_fill(binary, dot, arrays, owner) == ORES_NATIVE_ARRAY_OK);
    CHECK(oreslang_int_array_length(arrays, owner) == 2);
    CHECK(token_equals(arrays, owner, 0, embedded, 3));
    CHECK(token_equals(arrays, owner, 1, embedded + 4, 3));
    oreslang_int_array_drop(arrays, owner);

    owner = oreslang_int_array_new(arrays);
    const int64_t empty = ores_string_literal("", 0);
    CHECK(ores_string_split_literal_fill(empty, dot, arrays, owner) == ORES_NATIVE_ARRAY_OK);
    CHECK(oreslang_int_array_length(arrays, owner) == 1);
    CHECK(token_equals(arrays, owner, 0, "", 0));
    oreslang_int_array_drop(arrays, owner);

    /* Limit path must not commit a deliverable owner. The caller is
     * required to drop the partially filled array before region_close.
     */
    char many[2049];
    for (size_t i = 0; i < 2048; ++i) many[i] = '.';
    many[2048] = 'x';
    const int64_t flood = ores_string_literal(many, sizeof many);
    owner = oreslang_int_array_new(arrays);
    CHECK(ores_string_split_literal_fill(flood, dot, arrays, owner) == ORES_NATIVE_ARRAY_LIMIT);
    oreslang_int_array_drop(arrays, owner);
    owner = oreslang_int_array_new(arrays);
    const int64_t empty_separator = ores_string_literal("", 0);
    CHECK(ores_string_split_literal_fill(source, empty_separator, arrays, owner)
        == ORES_NATIVE_ARRAY_LIMIT);
    CHECK(oreslang_int_array_length(arrays, owner) == 0);
    oreslang_int_array_drop(arrays, owner);
    oreslang_int_array_region_close(arrays);
    CHECK(ores_string_region_end() == 0);

    /* A bad source handle must poison the String invocation and cannot
     * be turned into a valid array or silently accepted by subject ACLs. */
    CHECK(ores_string_region_begin(0) == 0);
    arrays = oreslang_int_array_region_open();
    CHECK(arrays);
    owner = oreslang_int_array_new(arrays);
    const int64_t valid_delimiter = ores_string_literal(".", 1);
    CHECK(ores_string_split_literal_fill(0, valid_delimiter, arrays, owner)
        == ORES_NATIVE_ARRAY_LIMIT);
    CHECK(oreslang_int_array_length(arrays, owner) == 0);
    oreslang_int_array_drop(arrays, owner);
    oreslang_int_array_region_close(arrays);
    CHECK(ores_string_region_end() != 0);
    return 0;
}
