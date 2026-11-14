#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Never rely on assert(): coverage must survive Release/NDEBUG. */
static void require(int condition) { if (!condition) abort(); }

static void positives(void) {
    OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
    OresNativeIntArrayHandle a = oreslang_int_array_new(r);
    OresNativeIntArrayHandle b = oreslang_int_array_new(r);
    require(a != 0 && b != 0 && a != b);
    require(oreslang_int_array_length(r,a) == 0);
    oreslang_int_array_reserve(r,a,8);
    oreslang_int_array_reserve(r,a,3);
    for (int i = 0; i < 4096; ++i) oreslang_int_array_add(r,a,(int64_t)i-2048);
    require(oreslang_int_array_length(r,a) == 4096);
    for (int i = 0; i < 4096; ++i)
        require(oreslang_int_array_get(r,a,(uint64_t)i) == (int64_t)i - 2048);
    oreslang_int_array_set(r,a,0,INT64_MIN);
    oreslang_int_array_set(r,a,4095,INT64_MAX);
    require(oreslang_int_array_get(r,a,0) == INT64_MIN);
    require(oreslang_int_array_get(r,a,4095) == INT64_MAX);
    oreslang_int_array_add(r,b,99);
    require(oreslang_int_array_get(r,b,0) == 99);
    oreslang_int_array_drop(r,a);
    OresNativeIntArrayHandle fresh = oreslang_int_array_new(r);
    require(fresh != a && fresh != b);
    oreslang_int_array_add(r,fresh,7);
    require(oreslang_int_array_get(r,fresh,0) == 7);
    oreslang_int_array_drop(r,b);
    oreslang_int_array_drop(r,fresh);
    oreslang_int_array_region_close(r);

    /* A new region must not accept tokens issued in the prior one. */
    OresNativeIntArrayRegion* maximum = oreslang_int_array_region_open();
    OresNativeIntArrayHandle largest = oreslang_int_array_new(maximum);
    oreslang_int_array_reserve(maximum,largest,65536);
    for (int i = 0; i < 65536; ++i) oreslang_int_array_add(maximum,largest,i);
    require(oreslang_int_array_length(maximum,largest) == 65536);
    require(oreslang_int_array_get(maximum,largest,65535) == 65535);
    oreslang_int_array_drop(maximum,largest);
    oreslang_int_array_region_close(maximum);
}

#if defined(__unix__) || defined(__APPLE__)
static void must_abort(int type) {
    pid_t pid = fork();
    require(pid >= 0);
    if (pid == 0) {
        struct rlimit limit = {0,0};
        setrlimit(RLIMIT_CORE, &limit);
        OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
        OresNativeIntArrayHandle a = oreslang_int_array_new(r);
        OresNativeIntArrayRegion* other = oreslang_int_array_region_open();
        if (type == 0) oreslang_int_array_get(r,a,0);
        else if (type == 1) oreslang_int_array_set(r,a,0,2);
        else if (type == 2) oreslang_int_array_reserve(r,a,65537);
        else if (type == 3) oreslang_int_array_region_close(r);
        else if (type == 4) { oreslang_int_array_drop(r,a); oreslang_int_array_drop(r,a); }
        else if (type == 5) oreslang_int_array_get(other,a,0);
        else if (type == 6) oreslang_int_array_drop(other,a);
        else if (type == 7) oreslang_int_array_get(r,0,0);
        else if (type == 8) oreslang_int_array_get(r,UINT64_MAX,0);
        else if (type == 9) {
            oreslang_int_array_reserve(r,a,65536);
            for (int i = 0; i < 65536; ++i) oreslang_int_array_add(r,a,i);
            oreslang_int_array_add(r,a,1);
        } else if (type == 10) {
            for (int i = 0; i < 64; ++i) (void)oreslang_int_array_new(r);
        } else if (type == 11) {
            for (int i = 0; i < 4; ++i) {
                OresNativeIntArrayHandle h = i == 0 ? a : oreslang_int_array_new(r);
                oreslang_int_array_reserve(r,h,65536);
            }
            OresNativeIntArrayHandle fifth = oreslang_int_array_new(r);
            oreslang_int_array_add(r,fifth,5);
        } else if (type == 12) {
            oreslang_int_array_drop(r,a);
            OresNativeIntArrayHandle fresh = oreslang_int_array_new(r);
            oreslang_int_array_add(r,fresh,2);
            (void)oreslang_int_array_length(r,a);
        } else if (type == 13) {
            oreslang_int_array_add(r,a,1);
            (void)oreslang_int_array_get(r,a,1);
        } else if (type == 14) {
            oreslang_int_array_add(r,a,1);
            oreslang_int_array_set(r,a,1,99);
        }
#ifdef ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION
        else if (type == 15) {
            oreslang_int_array_test_set_next_token(UINT64_MAX-1);
            (void)oreslang_int_array_new(r);
            (void)oreslang_int_array_new(r);
        }
#endif
        else abort();
        _exit(0);
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid);
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif

int main(void) {
    positives();
#if defined(__unix__) || defined(__APPLE__)
    for (int i = 0; i <= 14; ++i) must_abort(i);
#ifdef ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION
    must_abort(15);
#endif
#endif
    return 0;
}
