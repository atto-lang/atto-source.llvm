#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Assertions are always active under Release/NDEBUG. */
static void check(int expression) { if (!expression) abort(); }
static void reset_injection(void) {
#ifdef ORESLANG_NATIVE_TEST_ALLOC_FAILURE
    oreslang_int_array_test_fail_after(-1);
#endif
}
static void fail_on_attempt(int64_t attempts) {
#ifdef ORESLANG_NATIVE_TEST_ALLOC_FAILURE
    oreslang_int_array_test_fail_after(attempts);
#else
    (void)attempts;
    abort();
#endif
}

static void opening_and_single_owner_failures(void) {
    OresNativeIntArrayRegion* sentinel = (OresNativeIntArrayRegion*)(uintptr_t)1;
    OresNativeIntArrayRegion* region = sentinel;
    fail_on_attempt(0);
    check(oreslang_int_array_try_region_open(&region) == ORES_NATIVE_ARRAY_NO_MEMORY);
    check(region == sentinel);
    reset_injection();
    check(oreslang_int_array_try_region_open(&region) == ORES_NATIVE_ARRAY_OK);
    check(region != NULL && region != sentinel);

    OresNativeIntArrayHandle a = UINT64_C(12345678);
    fail_on_attempt(0);
    check(oreslang_int_array_try_new(region, &a) == ORES_NATIVE_ARRAY_NO_MEMORY);
    check(a == UINT64_C(12345678));
    reset_injection();
    check(oreslang_int_array_try_new(region, &a) == ORES_NATIVE_ARRAY_OK);
    check(a != 0 && a != UINT64_C(12345678));

    check(oreslang_int_array_try_reserve(region, a, 65537) == ORES_NATIVE_ARRAY_LIMIT);
    check(oreslang_int_array_length(region, a) == 0);
    fail_on_attempt(0);
    check(oreslang_int_array_try_reserve(region, a, 4) == ORES_NATIVE_ARRAY_NO_MEMORY);
    check(oreslang_int_array_length(region, a) == 0);
    reset_injection();
    check(oreslang_int_array_try_add(region, a, 10) == ORES_NATIVE_ARRAY_OK);
    check(oreslang_int_array_get(region, a, 0) == 10);
    for (int i = 1; i < 4; ++i)
        check(oreslang_int_array_try_add(region, a, 10 + i) == ORES_NATIVE_ARRAY_OK);
    fail_on_attempt(0);
    check(oreslang_int_array_try_add(region, a, 999) == ORES_NATIVE_ARRAY_NO_MEMORY);
    check(oreslang_int_array_length(region, a) == 4);
    for (int i = 0; i < 4; ++i)
        check(oreslang_int_array_get(region, a, (uint64_t)i) == 10 + i);
    reset_injection();
    check(oreslang_int_array_try_add(region, a, 14) == ORES_NATIVE_ARRAY_OK);
    check(oreslang_int_array_get(region, a, 4) == 14);
    check(oreslang_int_array_try_reserve(region, a, 8) == ORES_NATIVE_ARRAY_OK);

    /* Reserve no-op must neither allocate nor consume an injected failure. */
    fail_on_attempt(0);
    check(oreslang_int_array_try_reserve(region, a, 7) == ORES_NATIVE_ARRAY_OK);
    check(oreslang_int_array_try_reserve(region, a, 9) == ORES_NATIVE_ARRAY_NO_MEMORY);
    reset_injection();

    oreslang_int_array_drop(region, a);
    oreslang_int_array_region_close(region);
}

/* Growth must preserve every element, wipe old capacity before free,
 * and leave both the data and scrub counter untouched when allocation
 * fails. NDEBUG does not disable any of these assertions.
 */
static void backing_growth_scrub_and_failure_atomicity(void) {
    OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
    OresNativeIntArrayHandle a = oreslang_int_array_new(r);
    const int64_t values[4] = {INT64_MIN, INT64_MAX, 0, -91};
    for (int i = 0; i < 4; ++i)
        check(oreslang_int_array_try_add(r, a, values[i]) == ORES_NATIVE_ARRAY_OK);

    oreslang_int_array_test_reset_scrubbed_backing_bytes();
    check(oreslang_int_array_try_reserve(r, a, 8) == ORES_NATIVE_ARRAY_OK);
    check(oreslang_int_array_test_scrubbed_backing_bytes() == 4 * sizeof(int64_t));
    for (int i = 0; i < 4; ++i)
        check(oreslang_int_array_get(r, a, (uint64_t)i) == values[i]);

    fail_on_attempt(0);
    check(oreslang_int_array_try_reserve(r, a, 16) == ORES_NATIVE_ARRAY_NO_MEMORY);
    check(oreslang_int_array_test_scrubbed_backing_bytes() == 4 * sizeof(int64_t));
    check(oreslang_int_array_length(r, a) == 4);
    for (int i = 0; i < 4; ++i)
        check(oreslang_int_array_get(r, a, (uint64_t)i) == values[i]);
    reset_injection();

    check(oreslang_int_array_try_reserve(r, a, 16) == ORES_NATIVE_ARRAY_OK);
    check(oreslang_int_array_test_scrubbed_backing_bytes() == 12 * sizeof(int64_t));
    check(oreslang_int_array_try_reserve(r, a, 9) == ORES_NATIVE_ARRAY_OK);
    check(oreslang_int_array_test_scrubbed_backing_bytes() == 12 * sizeof(int64_t));
    for (int i = 0; i < 4; ++i)
        check(oreslang_int_array_get(r, a, (uint64_t)i) == values[i]);

    oreslang_int_array_drop(r, a);
    check(oreslang_int_array_test_scrubbed_backing_bytes() == 28 * sizeof(int64_t));
    oreslang_int_array_region_close(r);
}

static void prepare_rollback_and_commit(void) {
    OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
    OresNativeIntArrayHandle survivor = oreslang_int_array_new(r);
    oreslang_int_array_add(r, survivor, 29);

    OresNativeIntArrayHandle outputs[4] = {91, 92, 93, 94};
    fail_on_attempt(2);
    check(oreslang_int_array_try_prepare_many(r, 4, outputs) ==
          ORES_NATIVE_ARRAY_NO_MEMORY);
    reset_injection();
    for (int i = 0; i < 4; ++i) check(outputs[i] == (uint64_t)(91 + i));
    check(oreslang_int_array_get(r, survivor, 0) == 29);

    /* Partial staging leaked no live slot: all 63 remaining slots fit. */
    OresNativeIntArrayHandle all[63] = {0};
    check(oreslang_int_array_try_prepare_many(r, 63, all) == ORES_NATIVE_ARRAY_OK);
    OresNativeIntArrayHandle untouched[2] = {1001, 1002};
    check(oreslang_int_array_try_prepare_many(r, 2, untouched) == ORES_NATIVE_ARRAY_LIMIT);
    check(untouched[0] == 1001 && untouched[1] == 1002);
    for (int i = 62; i >= 0; --i) oreslang_int_array_drop(r, all[i]);

    OresNativeIntArrayHandle old[3];
    OresNativeIntArrayHandle fresh[3] = {0};
    for (int i = 0; i < 3; ++i) {
        old[i] = oreslang_int_array_new(r);
        oreslang_int_array_add(r, old[i], 300 + i);
    }
    check(oreslang_int_array_try_prepare_many(r, 3, fresh) == ORES_NATIVE_ARRAY_OK);
    for (int i = 0; i < 3; ++i) oreslang_int_array_add(r, fresh[i], 400 + i);
    OresNativeIntArrayHandle last_old[3] = {old[0], old[1], old[2]};
    OresNativeIntArrayHandle last_fresh[3] = {fresh[0], fresh[1], fresh[2]};
    OresNativeIntArrayHandle* destinations[3] = {&old[0], &old[1], &old[2]};
    OresNativeIntArrayHandle* sources[3] = {&fresh[0], &fresh[1], &fresh[2]};
    oreslang_int_array_commit_many(r, 3, destinations, sources);
    for (int i = 0; i < 3; ++i) {
        check(fresh[i] == 0);
        check(old[i] == last_fresh[i] && old[i] != last_old[i]);
        check(oreslang_int_array_get(r, old[i], 0) == 400 + i);
    }

    /* Empty destination is legal for a first-time commit, and the runtime
     * can still reach all 64 owner slots after old owners are dropped.
     */
    OresNativeIntArrayHandle empty = 0;
    OresNativeIntArrayHandle stage = oreslang_int_array_new(r);
    oreslang_int_array_add(r, stage, 42);
    OresNativeIntArrayHandle* dst_one[1] = {&empty};
    OresNativeIntArrayHandle* src_one[1] = {&stage};
    oreslang_int_array_commit_many(r, 1, dst_one, src_one);
    check(stage == 0 && oreslang_int_array_get(r, empty, 0) == 42);

    oreslang_int_array_drop(r, empty);
    for (int i = 2; i >= 0; --i) oreslang_int_array_drop(r, old[i]);
    oreslang_int_array_drop(r, survivor);
    oreslang_int_array_region_close(r);
}

static void quota_failure_unchanged(void) {
    OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
    OresNativeIntArrayHandle reserved[4];
    for (int i = 0; i < 4; ++i) {
        reserved[i] = oreslang_int_array_new(r);
        check(oreslang_int_array_try_reserve(r, reserved[i], 65536) ==
              ORES_NATIVE_ARRAY_OK);
        check(oreslang_int_array_try_add(r, reserved[i], 100 + i) ==
              ORES_NATIVE_ARRAY_OK);
    }
    OresNativeIntArrayHandle fifth = oreslang_int_array_new(r);
    check(oreslang_int_array_try_reserve(r, fifth, 1) == ORES_NATIVE_ARRAY_LIMIT);
    check(oreslang_int_array_try_add(r, fifth, 123) == ORES_NATIVE_ARRAY_LIMIT);
    check(oreslang_int_array_length(r, fifth) == 0);
    for (int i = 0; i < 4; ++i) {
        check(oreslang_int_array_get(r, reserved[i], 0) == 100 + i);
        check(oreslang_int_array_length(r, reserved[i]) == 1);
    }
    oreslang_int_array_drop(r, reserved[0]);
    check(oreslang_int_array_try_add(r, fifth, 123) == ORES_NATIVE_ARRAY_OK);
    check(oreslang_int_array_get(r, fifth, 0) == 123);
    oreslang_int_array_drop(r, fifth);
    for (int i = 3; i > 0; --i) oreslang_int_array_drop(r, reserved[i]);
    oreslang_int_array_region_close(r);
}

/* Regression: geometric growth must not report quota exhaustion while an
 * additional element fits. A failed fallback allocation must leave the old
 * length/data unchanged and a later successful append must use the last
 * reserved element of region quota. */
static void quota_edge_append_and_rollback(void) {
    OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
    OresNativeIntArrayHandle a = oreslang_int_array_new(r);
    check(oreslang_int_array_try_reserve(r, a, 4) == ORES_NATIVE_ARRAY_OK);
    for (int i = 0; i < 4; ++i)
        check(oreslang_int_array_try_add(r, a, 10 + i) == ORES_NATIVE_ARRAY_OK);

    const uint64_t capacities[4] = {65536, 65536, 65536, 65531};
    OresNativeIntArrayHandle fillers[4];
    for (int i = 0; i < 4; ++i) {
        fillers[i] = oreslang_int_array_new(r);
        check(oreslang_int_array_try_reserve(r, fillers[i], capacities[i]) ==
              ORES_NATIVE_ARRAY_OK);
    }
    /* 4 + 3*65536 + 65531 = 262143: exactly one element is free. */
    fail_on_attempt(0);
    check(oreslang_int_array_try_add(r, a, 99) == ORES_NATIVE_ARRAY_NO_MEMORY);
    check(oreslang_int_array_length(r, a) == 4);
    for (int i = 0; i < 4; ++i)
        check(oreslang_int_array_get(r, a, (uint64_t)i) == 10 + i);
    reset_injection();

    check(oreslang_int_array_try_add(r, a, 99) == ORES_NATIVE_ARRAY_OK);
    check(oreslang_int_array_length(r, a) == 5);
    check(oreslang_int_array_get(r, a, 4) == 99);
    check(oreslang_int_array_try_add(r, a, 100) == ORES_NATIVE_ARRAY_LIMIT);
    check(oreslang_int_array_length(r, a) == 5);

    oreslang_int_array_drop(r, fillers[0]);
    check(oreslang_int_array_try_add(r, a, 100) == ORES_NATIVE_ARRAY_OK);
    check(oreslang_int_array_get(r, a, 5) == 100);
    for (int i = 3; i >= 1; --i)
        oreslang_int_array_drop(r, fillers[i]);
    oreslang_int_array_drop(r, a);
    oreslang_int_array_region_close(r);
}

/* Four already-published fields remain unchanged while four replacement
 * arrays are staged. Every one of the eight resource operations (4 logical
 * slots + 4 physical backing buffers) must roll back, including after the
 * first three arrays have been filled with real data.
 */
static void filled_preparation_allocation_edge_matrix(void) {
    OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
    OresNativeIntArrayHandle current[4] = {0};
    for (int i = 0; i < 4; ++i) {
        current[i] = oreslang_int_array_new(r);
        oreslang_int_array_add(r, current[i], 100 + i);
    }

    const uint64_t lengths[4] = {17, 4096, 3, 1024};
    const int64_t values[4] = {-1, -1, 0, 0};
    const uint64_t impossible[4] = {65533, 65533, 65533, 65533};
    OresNativeIntArrayHandle staged[4] = {111, 222, 333, 444};

    /* Aggregate budget proof happens before *any* allocation or publication.
     * Current four arrays have 4 reserved elements each (16 total).
     */
    check(oreslang_int_array_try_prepare_filled_many(
          r, 4, impossible, values, staged) == ORES_NATIVE_ARRAY_LIMIT);
    for (int i = 0; i < 4; ++i) check(staged[i] == (uint64_t)(111 * (i + 1)));
    check(oreslang_int_array_try_prepare_filled_many(
          r, 0, NULL, NULL, NULL) == ORES_NATIVE_ARRAY_OK);

    for (int failpoint = 0; failpoint < 8; ++failpoint) {
        fail_on_attempt(failpoint);
        check(oreslang_int_array_try_prepare_filled_many(
              r, 4, lengths, values, staged) == ORES_NATIVE_ARRAY_NO_MEMORY);
        reset_injection();
        for (int i = 0; i < 4; ++i) {
            check(staged[i] == (uint64_t)(111 * (i + 1)));
            check(oreslang_int_array_length(r, current[i]) == 1);
            check(oreslang_int_array_get(r, current[i], 0) == 100 + i);
        }

        /* All allocated backing storage must have been reclaimed. Each
         * replacement reserves exactly the remaining shared 262,128 slots.
         * An unseen 1-element leak would make this operation return LIMIT.
         */
        const uint64_t full_lengths[4] = {65532, 65532, 65532, 65532};
        OresNativeIntArrayHandle probes[4] = {0};
        check(oreslang_int_array_try_prepare_filled_many(
              r, 4, full_lengths, values, probes) == ORES_NATIVE_ARRAY_OK);
        for (int i = 0; i < 4; ++i) {
            check(oreslang_int_array_length(r, probes[i]) == 65532);
            check(oreslang_int_array_get(r, probes[i], 0) == values[i]);
            check(oreslang_int_array_get(r, probes[i], 65531) == values[i]);
        }
        for (int i = 3; i >= 0; --i) oreslang_int_array_drop(r, probes[i]);
    }

    check(oreslang_int_array_try_prepare_filled_many(
          r, 4, lengths, values, staged) == ORES_NATIVE_ARRAY_OK);
    OresNativeIntArrayHandle* destinations[4] = {
        &current[0], &current[1], &current[2], &current[3]
    };
    OresNativeIntArrayHandle* sources[4] = {
        &staged[0], &staged[1], &staged[2], &staged[3]
    };
    OresNativeIntArrayHandle old[4] = {
        current[0], current[1], current[2], current[3]
    };
    oreslang_int_array_commit_many(r, 4, destinations, sources);
    for (int i = 0; i < 4; ++i) {
        check(staged[i] == 0);
        check(current[i] != old[i]);
        check(oreslang_int_array_length(r, current[i]) == lengths[i]);
        check(oreslang_int_array_get(r, current[i], 0) == values[i]);
        check(oreslang_int_array_get(r, current[i], lengths[i] - 1) == values[i]);
    }
    for (int i = 3; i >= 0; --i) oreslang_int_array_drop(r, current[i]);

    /* A full 64 fresh handle batch after the commit and cleanup proves that
     * every superseded old handle was dropped and none of the staging slots
     * remained attached to the region.
     */
    OresNativeIntArrayHandle all[64] = {0};
    check(oreslang_int_array_try_prepare_many(r, 64, all) == ORES_NATIVE_ARRAY_OK);
    for (int i = 63; i >= 0; --i) oreslang_int_array_drop(r, all[i]);
    oreslang_int_array_region_close(r);
}

#if defined(__unix__) || defined(__APPLE__)
static void must_abort_on_corrupt_array_metadata(int kind) {
    pid_t pid = fork();
    check(pid >= 0);
    if (pid == 0) {
        struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        reset_injection();
        OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
        OresNativeIntArrayHandle a = oreslang_int_array_new(r);
        check(oreslang_int_array_try_add(r, a, 17) == ORES_NATIVE_ARRAY_OK);
        /* capacity is 4; length=5 must fail before the requested operation
         * returns an apparently successful no-op or accesses values[5].
         */
        oreslang_int_array_test_corrupt_length(r, a, 5);
        if (kind == 0)
            (void)oreslang_int_array_try_reserve(r, a, 1);
        else if (kind == 1)
            (void)oreslang_int_array_try_add(r, a, 22);
        else if (kind == 2)
            (void)oreslang_int_array_length(r, a);
        else if (kind == 3)
            (void)oreslang_int_array_get(r, a, 0);
        else if (kind == 4)
            oreslang_int_array_set(r, a, 0, 33);
        else if (kind == 5)
            oreslang_int_array_drop(r, a);
        else
            abort();
        _exit(0);
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

/* A corrupted region counter must NEVER permit close() to abandon live
 * sensitive buffers, or an allocation/drop to proceed on forged accounting.
 * A child that returns normally proves the invariant was not enforced.
 */
static void must_abort_on_corrupt_region_accounting(int kind) {
    pid_t pid = fork();
    check(pid >= 0);
    if (pid == 0) {
        struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        reset_injection();
        OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
        OresNativeIntArrayHandle a = oreslang_int_array_new(r);
        check(oreslang_int_array_try_add(r, a, 17) == ORES_NATIVE_ARRAY_OK);
        if (kind == 0) {
            /* This used to allow region_close to free only the region
             * struct, leaking the allocated array buffer without wiping.
             */
            oreslang_int_array_test_corrupt_region_accounting(r, 0, 0);
            oreslang_int_array_region_close(r);
        } else if (kind == 1) {
            oreslang_int_array_test_corrupt_region_accounting(r, 1, 8);
            OresNativeIntArrayHandle next = 0;
            (void)oreslang_int_array_try_new(r, &next);
        } else if (kind == 2) {
            oreslang_int_array_test_corrupt_region_accounting(r, 1, 8);
            (void)oreslang_int_array_try_reserve(r, a, 4);
        } else if (kind == 3) {
            oreslang_int_array_test_corrupt_region_accounting(r, 2, 4);
            OresNativeIntArrayHandle next = 0;
            (void)oreslang_int_array_try_prepare_many(r, 1, &next);
        } else if (kind == 4) {
            oreslang_int_array_test_corrupt_region_accounting(r, 2, 4);
            const uint64_t lengths[1] = {1};
            const int64_t values[1] = {42};
            OresNativeIntArrayHandle next = 0;
            (void)oreslang_int_array_try_prepare_filled_many(
                r, 1, lengths, values, &next);
        } else if (kind == 5) {
            oreslang_int_array_test_corrupt_region_accounting(r, 2, 4);
            oreslang_int_array_drop(r, a);
        } else if (kind == 6) {
            oreslang_int_array_test_corrupt_region_accounting(r, 1, 8);
            (void)oreslang_int_array_try_reserve(r, a, 8);
        } else {
            abort();
        }
        _exit(0);
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

static void must_abort_on_invalid_transaction(int kind) {
    pid_t pid = fork();
    check(pid >= 0);
    if (pid == 0) {
        struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        reset_injection();
        OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
        OresNativeIntArrayHandle owned[3];
        for (int i = 0; i < 3; ++i) owned[i] = oreslang_int_array_new(r);
        OresNativeIntArrayHandle stale = owned[2];
        if (kind == 0) oreslang_int_array_drop(r, stale);
        OresNativeIntArrayHandle* dests[2] = {&owned[0], &owned[1]};
        OresNativeIntArrayHandle* srcs[2] = {&owned[2], &owned[2]};
        if (kind == 0) {
            srcs[0] = &stale;
            oreslang_int_array_commit_many(r, 1, dests, srcs);
        } else if (kind == 1) {
            oreslang_int_array_commit_many(r, 2, dests, srcs);
        } else if (kind == 2) {
            srcs[1] = &owned[1];
            oreslang_int_array_commit_many(r, 2, dests, srcs);
        } else if (kind == 3) {
            dests[1] = &owned[0];
            srcs[1] = &owned[1];
            oreslang_int_array_commit_many(r, 2, dests, srcs);
        } else if (kind == 4) {
            srcs[0] = &owned[0];
            oreslang_int_array_commit_many(r, 1, dests, srcs);
        } else if (kind == 5) {
            OresNativeIntArrayRegion* other = oreslang_int_array_region_open();
            OresNativeIntArrayHandle foreign = oreslang_int_array_new(other);
            srcs[0] = &foreign;
            oreslang_int_array_commit_many(r, 1, dests, srcs);
        }
        _exit(0);
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif

int main(void) {
    opening_and_single_owner_failures();
    backing_growth_scrub_and_failure_atomicity();
    prepare_rollback_and_commit();
    quota_failure_unchanged();
    quota_edge_append_and_rollback();
    filled_preparation_allocation_edge_matrix();
#if defined(__unix__) || defined(__APPLE__)
    for (int i = 0; i < 6; ++i) must_abort_on_invalid_transaction(i);
    for (int i = 0; i < 6; ++i) must_abort_on_corrupt_array_metadata(i);
    for (int i = 0; i < 7; ++i) must_abort_on_corrupt_region_accounting(i);
#endif
    return 0;
}
