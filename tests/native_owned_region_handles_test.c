#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
void oreslang_owned_i64_region_test_set_generation(OresOwnedI64Region*, uint64_t, uint64_t);
static void good(void) {
    OresOwnedI64Region* r = oreslang_owned_i64_region_open_handles(2);
    uint64_t a = oreslang_owned_i64_region_new_handle(r, 20);
    uint64_t b = oreslang_owned_i64_region_new_handle(r, 22);
    assert(a != b);
    assert(oreslang_owned_i64_region_read_handle(r, a) == 20);
    oreslang_owned_i64_region_drop_handle(r, a);
    uint64_t c = oreslang_owned_i64_region_new_handle(r, 40);
    assert(c != a && oreslang_owned_i64_region_read_handle(r, c) == 40);
    assert(oreslang_owned_i64_region_read_handle(r, b) == 22);
    oreslang_owned_i64_region_drop_handle(r, b);
    oreslang_owned_i64_region_drop_handle(r, c);
    oreslang_owned_i64_region_close(r);

    r = oreslang_owned_i64_region_open_handles(1);
    oreslang_owned_i64_region_test_set_generation(r, 0, (UINT64_MAX >> 12) - 1);
    a = oreslang_owned_i64_region_new_handle(r, 42);
    assert(oreslang_owned_i64_region_read_handle(r, a) == 42);
    oreslang_owned_i64_region_drop_handle(r, a);
    oreslang_owned_i64_region_close(r);
}
#if defined(__unix__) || defined(__APPLE__)
static void bad(int kind) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        struct rlimit limit = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limit);
        OresOwnedI64Region* r = oreslang_owned_i64_region_open_handles(1);
        uint64_t stale = oreslang_owned_i64_region_new_handle(r, 1);
        oreslang_owned_i64_region_drop_handle(r, stale);
        uint64_t current = oreslang_owned_i64_region_new_handle(r, 2);
        if (kind == 0) oreslang_owned_i64_region_drop_handle(r, stale);
        if (kind == 1) (void)oreslang_owned_i64_region_read_handle(r, stale);
        if (kind == 2) oreslang_owned_i64_region_drop_handle(r, current + (UINT64_C(1) << 12));
        if (kind == 3) oreslang_owned_i64_region_drop_handle(r, 0);
        if (kind == 4) {
            OresOwnedI64Region* other = oreslang_owned_i64_region_open_handles(1);
            uint64_t external = oreslang_owned_i64_region_new_handle(other, 3);
            /* Raw (slot,generation) alone is not a cross-region capability.
             * Region domain binding is needed before FFI exposure. */
            (void)external;
            oreslang_owned_i64_region_drop_handle(r, current);
            oreslang_owned_i64_region_drop_handle(other, external);
            oreslang_owned_i64_region_close(other);
            oreslang_owned_i64_region_close(r);
            _exit(0);
        }
        _exit(0);
    }
    int status=0;
    assert(waitpid(pid, &status, 0) == pid);
    if (kind == 4) assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    else assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif
int main(void) {
    good();
#if defined(__unix__) || defined(__APPLE__)
    for (int i = 0; i < 5; ++i) bad(i);
#endif
    return 0;
}
