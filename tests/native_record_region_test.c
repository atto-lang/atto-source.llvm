#include "oreslang/runtime/NativeRecordRegion.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#define require(c) do { if (!(c)) { fprintf(stderr, "record region: line %d: %s\n", __LINE__, #c); abort(); } } while (0)
static void* foreign_root(void* argument) {
    const int64_t foreign = *(int64_t*)argument;
    const int64_t value = 29;
    require(ores_record_region_begin() == 0);
    int64_t own = ores_record_new(1, 1, &value);
    require(own != foreign && ores_record_get(own, 1, 0) == 29);
    require(ores_record_get(foreign, 1, 0) == 0);
    require(ores_record_set(foreign, 1, 0, 99) == 1);
    require(ores_record_region_end() == 1);
    return NULL;
}
/* Independent cross-thread negative: a failed read must not mask a
 * missing write-isolation check. This thread attempts the write FIRST. */
static void* foreign_write_only(void* argument) {
    const int64_t foreign = *(int64_t*)argument;
    const int64_t local_value = 17;
    require(ores_record_region_begin() == 0);
    int64_t own = ores_record_new_mutable(1, 1, &local_value, 1);
    require(own != foreign);
    require(ores_record_set(foreign, 1, 0, 99) == 1);
    require(ores_record_region_end() == 1);
    return NULL;
}
int main(void) {
    const int64_t values[] = {13, 29};
    require(ores_record_region_begin() == 0);
    int64_t mutable = ores_record_new_mutable(1, 2, values, 1);
    require(mutable != 0);
    for (int i = 0; i < 4096; ++i) require(ores_record_new(2, 2, values) != 0);
    require(ores_record_set(mutable, 1, 0, 42) == 0);
    require(ores_record_get(mutable, 1, 0) == 42);
    require(ores_record_get(mutable, 1, 1) == 29);
    const int64_t retained_alias = mutable;
    require(ores_record_set(retained_alias, 1, 0, 41) == 0);
    require(ores_record_get(mutable, 1, 0) == 41);
    require(ores_record_set(mutable, 1, 0, 42) == 0);
    pthread_t write_thread;
    require(pthread_create(&write_thread, NULL, foreign_write_only, &mutable) == 0);
    require(pthread_join(write_thread, NULL) == 0);
    require(ores_record_get(mutable, 1, 0) == 42);
    pthread_t mutable_thread;
    require(pthread_create(&mutable_thread, NULL, foreign_root, &mutable) == 0);
    require(pthread_join(mutable_thread, NULL) == 0);
    require(ores_record_get(mutable, 1, 0) == 42);
    require(ores_record_region_end() == 0);
    require(ores_record_region_begin() == 0);
    require(ores_record_set(mutable, 1, 0, 0) == 1);
    require(ores_record_region_end() == 1);
    for (int invalid = 0; invalid < 5; ++invalid) {
        require(ores_record_region_begin() == 0);
        mutable = ores_record_new_mutable(1, 2, values, 1);
        require(ores_record_set(mutable, invalid == 0 ? 2 : 1,
            invalid == 1 ? -1 : invalid == 2 ? 2 : invalid == 3 ? 64 : 1, 99) == 1);
        require(ores_record_region_end() == 1);
    }
    require(ores_record_region_begin() == 0);
    mutable = ores_record_new(1, 2, values);
    require(ores_record_set(mutable, 1, 0, 0) == 1);
    require(ores_record_region_end() == 1);
    require(ores_record_region_begin() == 0);
    require(ores_record_new_mutable(1, 2, values, UINT64_C(1) << 40) == 0);
    require(ores_record_region_end() == 1);
    /* The 16th scalar bit is legal; bit 16 is not. No uninitialized
     * write-bit or modulo shift may authorize another field. */
    const int64_t wide_values[16] = {0};
    require(ores_record_region_begin() == 0);
    int64_t wide = ores_record_new_mutable(9, 16, wide_values, UINT64_C(1) << 15);
    require(wide != 0);
    require(ores_record_set(wide, 9, 15, 123) == 0);
    require(ores_record_get(wide, 9, 15) == 123);
    require(ores_record_region_end() == 0);
    require(ores_record_region_begin() == 0);
    require(ores_record_new_mutable(9, 16, wide_values, UINT64_C(1) << 16) == 0);
    require(ores_record_region_end() == 1);
    require(ores_record_region_begin() == 0);
    int64_t empty_mutable = ores_record_new_mutable(7, 0, NULL, 0);
    require(empty_mutable != 0 && ores_record_check_shape(empty_mutable, 7, 0) == 0);
    require(ores_record_region_end() == 0);
    require(ores_record_region_begin() == 0);
    require(ores_record_new_mutable(7, 0, NULL, 1) == 0);
    require(ores_record_region_end() == 1);
    require(ores_record_region_begin() == 0);
    int64_t first = ores_record_new(1, 2, values);
    require(first != 0);
    for (int i = 0; i < 4096; ++i) require(ores_record_new(2, 2, values) != 0);
    require(ores_record_get(first, 1, 0) == 13);
    require(ores_record_get(first, 1, 1) == 29);
    pthread_t thread;
    require(pthread_create(&thread, NULL, foreign_root, &first) == 0);
    require(pthread_join(thread, NULL) == 0);
    require(ores_record_get(first, 1, 1) == 29);
    require(ores_record_region_end() == 0);
    require(ores_record_region_end() == 1);

    require(ores_record_region_begin() == 0);
    require(ores_record_new(1, 2, values) != first);
    require(ores_record_get(first, 1, 0) == 0);
    require(ores_record_region_end() == 1);
    require(ores_record_region_begin() == 0);
    first = ores_record_new(1, 2, values);
    require(ores_record_get(first, 2, 0) == 0);
    require(ores_record_region_end() == 1);
    require(ores_record_region_begin() == 0);
    first = ores_record_new(1, 2, values);
    require(ores_record_get(first, 1, 2) == 0);
    require(ores_record_region_end() == 1);
    require(ores_record_region_begin() == 0);
    require(ores_record_new(1, 1, NULL) == 0);
    require(ores_record_region_end() == 1);
    require(ores_record_region_begin() == 0);
    int64_t empty = ores_record_new(1, 0, NULL);
    require(empty != 0 && ores_record_check(empty, 1) == 0);
    require(ores_record_region_end() == 0);
    require(ores_record_region_begin() == 0);
    require(ores_record_region_begin() == 1);
    require(ores_record_region_end() == 1);

    for (int64_t expected = -1; expected <= 17; ++expected) {
        require(ores_record_region_begin() == 0);
        first = ores_record_new(1, 2, values);
        require(ores_record_check_shape(first, 1, expected) == (expected == 2 ? 0 : 1));
        require(ores_record_region_end() == (expected == 2 ? 0 : 1));
    }
    require(ores_record_region_begin() == 0);
    first = ores_record_new(1, 2, values);
    require(ores_record_check_shape(first, 2, 2) == 1);
    require(ores_record_region_end() == 1);
    require(ores_record_region_begin() == 0);
    require(ores_record_check_shape(first, 1, 2) == 1);
    require(ores_record_region_end() == 1);

    ores_record_test_fail_after(1);
    require(ores_record_region_begin() == 0);
    for (int i = 0; i < 32; ++i) require(ores_record_new(1, 2, values) != 0);
    require(ores_record_new(1, 2, values) == 0);
    require(ores_record_region_end() == 1);
    ores_record_test_fail_after(-1);
    require(ores_record_region_begin() == 0);
    for (int i = 0; i < 65536; ++i) require(ores_record_new(1, 2, values) != 0);
    require(ores_record_new(1, 2, values) == 0);
    require(ores_record_region_end() == 1);
    ores_record_test_exhaust_generation();
    require(ores_record_region_begin() == 1);
    require(ores_record_region_begin() == 1);
    puts("PASS record generation, writable masks, stable mutation, schema, quota, thread confinement and failure cleanup");
    return 0;
}
