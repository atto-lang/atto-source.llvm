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
int main(void) {
    const int64_t values[] = {13, 29};
    require(ores_record_region_begin() == 0);
    int64_t mutable = ores_record_new_mutable(1, 2, values, 1);
    require(mutable != 0);
    for (int i = 0; i < 4096; ++i) require(ores_record_new(2, 2, values) != 0);
    require(ores_record_set(mutable, 1, 0, 42) == 0);
    require(ores_record_get(mutable, 1, 0) == 42);
    require(ores_record_get(mutable, 1, 1) == 29);
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
