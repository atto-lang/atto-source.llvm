#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Security invariants must execute even under -O2 -DNDEBUG. */
static void require_or_abort(int condition) { if (!condition) abort(); }

static void positive(void) {
    OresOwnedI64Region* r = oreslang_owned_i64_region_open(2);
    OresOwnedI64Handle a = oreslang_owned_i64_region_new(r, 41);
    OresOwnedI64Handle b = oreslang_owned_i64_region_new(r, 42);
    require_or_abort(oreslang_owned_i64_region_read(r,a) == 41 && oreslang_owned_i64_region_read(r,b) == 42 && a != b);
    oreslang_owned_i64_region_drop(r, a);
    OresOwnedI64Handle c = oreslang_owned_i64_region_new(r, 99);
    require_or_abort(c != a && oreslang_owned_i64_region_read(r,c) == 99 && oreslang_owned_i64_region_read(r,b) == 42);
    oreslang_owned_i64_region_drop(r, b);
    oreslang_owned_i64_region_drop(r, c);
    oreslang_owned_i64_region_close(r);
}
#if defined(__unix__) || defined(__APPLE__)
static void must_abort(int kind) {
    pid_t pid = fork();
    require_or_abort(pid >= 0);
    if (pid == 0) {
        struct rlimit limit = {0, 0};
        setrlimit(RLIMIT_CORE, &limit);
        OresOwnedI64Region* r = oreslang_owned_i64_region_open(2);
        OresOwnedI64Handle a = oreslang_owned_i64_region_new(r, 23);
        OresOwnedI64Region* other = oreslang_owned_i64_region_open(1);
        if (kind == 0) { oreslang_owned_i64_region_drop(r, a); oreslang_owned_i64_region_drop(r, a); }
        else if (kind == 1) oreslang_owned_i64_region_drop(other, a);
        else if (kind == 2) oreslang_owned_i64_region_read(r, 0);
        else if (kind == 3) oreslang_owned_i64_region_close(r);
        else if (kind == 4) oreslang_owned_i64_region_new(r, 10), oreslang_owned_i64_region_new(r, 11);
        else if (kind == 5) { oreslang_owned_i64_region_drop(r,a); (void)oreslang_owned_i64_region_new(r,10); oreslang_owned_i64_region_drop(r,a); }
        else if (kind == 6) { oreslang_owned_i64_region_drop(r,a); (void)oreslang_owned_i64_region_new(r,10); (void)oreslang_owned_i64_region_read(r,a); }
        else if (kind == 8) (void)oreslang_owned_i64_region_read(other, a);
        else if (kind == 9) (void)oreslang_owned_i64_region_read(r, UINT64_MAX);
        else if (kind == 10) oreslang_owned_i64_region_drop(r, UINT64_MAX);
#ifdef ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION
        else if (kind == 7) { OresOwnedI64Region* more=oreslang_owned_i64_region_open(3); oreslang_owned_i64_test_set_next_token(UINT64_MAX-1); (void)oreslang_owned_i64_region_new(more,10); (void)oreslang_owned_i64_region_new(more,11); }
#endif
        else abort();
        _exit(0);
    }
    int status = 0;
    require_or_abort(waitpid(pid, &status, 0) == pid);
    require_or_abort(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif
int main(void) {
    positive();
#if defined(__unix__) || defined(__APPLE__)
    /* The final exhaustion scenario requires its opt-in testing hook; never
     * count a catch-all abort as evidence that exhaustion was tested. */
    for (int k = 0; k < 7; ++k) must_abort(k);
    for (int k = 8; k <= 10; ++k) must_abort(k);
#ifdef ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION
    must_abort(7);
#endif
#endif
    return 0;
}
