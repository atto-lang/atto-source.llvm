#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "transfer check line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static void success(void) {
    OresNativeIntArrayRegion *s = oreslang_int_array_region_open(), *d = oreslang_int_array_region_open();
    uint64_t a = oreslang_int_array_new(s), b = oreslang_int_array_new(s);
    uint64_t spare = oreslang_int_array_new(s), keep = oreslang_int_array_new(d);
    oreslang_int_array_add(s, a, 41); oreslang_int_array_add(s, a, 42);
    oreslang_int_array_add(d, keep, 99);
    uint64_t old_a = a, old_b = b, x = 0, y = 0;
    uint64_t *src[] = {&a, &b}, *dst[] = {&x, &y};
    oreslang_int_array_test_fail_after(0); // transfer must allocate nothing
    CHECK(oreslang_int_array_try_transfer_many(s, d, 2, dst, src) == ORES_NATIVE_ARRAY_OK);
    oreslang_int_array_test_fail_after(-1);
    CHECK(a == 0 && b == 0 && x == old_a && y == old_b);
    CHECK(oreslang_int_array_length(d, x) == 2 && oreslang_int_array_get(d, x, 1) == 42);
    CHECK(oreslang_int_array_length(d, y) == 0 && oreslang_int_array_get(d, keep, 0) == 99);
    oreslang_int_array_drop(s, spare); oreslang_int_array_region_close(s);
    CHECK(oreslang_int_array_get(d, x, 0) == 41); // source region is gone
    oreslang_int_array_drop(d, x); oreslang_int_array_drop(d, y); oreslang_int_array_drop(d, keep);
    oreslang_int_array_region_close(d);
}
static void quota(int budget) {
    OresNativeIntArrayRegion *s = oreslang_int_array_region_open(), *d = oreslang_int_array_region_open();
    uint64_t a = oreslang_int_array_new(s), b = oreslang_int_array_new(s), x = 0, y = 0;
    oreslang_int_array_add(s, a, 7);
    uint64_t old_a = a, old_b = b, occupied[64] = {0};
    unsigned count = budget ? 4 : 64;
    for (unsigned i = 0; i < count; ++i) {
        occupied[i] = oreslang_int_array_new(d);
        if (budget) oreslang_int_array_reserve(d, occupied[i], 65536);
    }
    uint64_t *src[] = {&a, &b}, *dst[] = {&x, &y};
    CHECK(oreslang_int_array_try_transfer_many(s, d, 2, dst, src) == ORES_NATIVE_ARRAY_LIMIT);
    CHECK(a == old_a && b == old_b && !x && !y);
    CHECK(oreslang_int_array_get(s, a, 0) == 7);
    oreslang_int_array_drop(d, occupied[--count]); // make room; transaction can now commit
    CHECK(oreslang_int_array_try_transfer_many(s, d, 2, dst, src) ==
        (budget ? ORES_NATIVE_ARRAY_OK : ORES_NATIVE_ARRAY_LIMIT));
    if (!budget) {
        oreslang_int_array_drop(d, occupied[--count]);
        CHECK(oreslang_int_array_try_transfer_many(s, d, 2, dst, src) == ORES_NATIVE_ARRAY_OK);
    }
    CHECK(!a && !b && x == old_a && y == old_b);
    oreslang_int_array_region_close(s);
    oreslang_int_array_drop(d, x); oreslang_int_array_drop(d, y);
    while (count) oreslang_int_array_drop(d, occupied[--count]);
    oreslang_int_array_region_close(d);
}
static void invalid(unsigned which) {
    OresNativeIntArrayRegion *s = oreslang_int_array_region_open(), *d = oreslang_int_array_region_open();
    uint64_t a = oreslang_int_array_new(s), b = oreslang_int_array_new(s), x = 0, y = 0;
    uint64_t *src[] = {&a, &b}, *dst[] = {&x, &y};
    if (which == 0) d = s;
    if (which == 1) b = a;
    if (which == 2) dst[1] = dst[0];
    if (which == 3) dst[0] = src[1];
    if (which == 4) x = oreslang_int_array_new(d);
    if (which == 5) a = oreslang_int_array_new(d);
    if (which == 6) oreslang_int_array_test_corrupt_region_accounting(d, 1, 0);
    uint64_t old_a = a;
    (void)oreslang_int_array_try_transfer_many(s, d, 2, dst, src);
    if (which == 7) (void)oreslang_int_array_length(s, old_a);
    _exit(77);
}
int main(void) {
    success(); quota(0); quota(1);
    for (unsigned i = 0; i != 8; ++i) {
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) invalid(i);
        int status = 0; CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
    return 0;
}
