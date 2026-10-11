#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define REQUIRE(expr) do { if (!(expr)) abort(); } while (0)
void oreslang_owned_i64_region_test_set_generation(OresOwnedI64Region*, uint64_t, uint64_t);

static void good(void) {
    OresOwnedI64Region* region = oreslang_owned_i64_region_open_handles(2);
    const uint64_t a = oreslang_owned_i64_region_new_handle(region, 20);
    const uint64_t b = oreslang_owned_i64_region_new_handle(region, 22);
    REQUIRE(a != b);
    REQUIRE(oreslang_owned_i64_region_read_handle(region, a) == 20);
    oreslang_owned_i64_region_drop_handle(region, a);
    const uint64_t c = oreslang_owned_i64_region_new_handle(region, 40);
    REQUIRE(c != a);
    REQUIRE(oreslang_owned_i64_region_read_handle(region, c) == 40);
    REQUIRE(oreslang_owned_i64_region_read_handle(region, b) == 22);
    oreslang_owned_i64_region_drop_handle(region, b);
    oreslang_owned_i64_region_drop_handle(region, c);
    oreslang_owned_i64_region_close(region);
}
#if defined(__unix__) || defined(__APPLE__)
static void bad(int kind) {
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        struct rlimit limit = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limit);
        OresOwnedI64Region* region = oreslang_owned_i64_region_open_handles(1);
        const uint64_t stale = oreslang_owned_i64_region_new_handle(region, 1);
        oreslang_owned_i64_region_drop_handle(region, stale);
        const uint64_t current = oreslang_owned_i64_region_new_handle(region, 2);
        switch (kind) {
            case 0: oreslang_owned_i64_region_drop_handle(region, stale); break;
            case 1: (void)oreslang_owned_i64_region_read_handle(region, stale); break;
            case 2: oreslang_owned_i64_region_drop_handle(region, current + (UINT64_C(1) << 12)); break;
            case 3: oreslang_owned_i64_region_drop_handle(region, 0); break;
            case 4: {
                OresOwnedI64Region* other = oreslang_owned_i64_region_open_handles(1);
                const uint64_t external = oreslang_owned_i64_region_new_handle(other, 3);
                oreslang_owned_i64_region_drop_handle(region, external);
                break;
            }
            case 5: {
                /* MAX generation is legal exactly once. The NEXT allocation
                 * must fail, rather than silently passing an exhaustion test. */
                oreslang_owned_i64_region_drop_handle(region, current);
                oreslang_owned_i64_region_test_set_generation(region, 0, UINT64_MAX >> 12);
                const uint64_t last = oreslang_owned_i64_region_new_handle(region, 4);
                REQUIRE(oreslang_owned_i64_region_read_handle(region, last) == 4);
                oreslang_owned_i64_region_drop_handle(region, last);
                (void)oreslang_owned_i64_region_new_handle(region, 5);
                break;
            }
            case 6: oreslang_owned_i64_region_drop_handle(region, current);
                    oreslang_owned_i64_region_drop_handle(region, current); break;
            case 7: {
                OresOwnedI64Region* other = oreslang_owned_i64_region_open_handles(1);
                (void)oreslang_owned_i64_region_read_handle(other, current);
                break;
            }
            case 8: (void)oreslang_owned_i64_region_read_handle(region, UINT64_MAX); break;
            case 9: oreslang_owned_i64_region_drop_handle(region, UINT64_MAX); break;
            case 10: oreslang_owned_i64_region_close(region); break;
            case 11: (void)oreslang_owned_i64_region_new_handle(region, 3); break;
            default: _exit(95);
        }
        _exit(96); /* Normal return is a test failure, never a success. */
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif
int main(void) {
    good();
#if defined(__unix__) || defined(__APPLE__)
    for (int i = 0; i < 12; ++i) bad(i);
#endif
    return 0;
}
