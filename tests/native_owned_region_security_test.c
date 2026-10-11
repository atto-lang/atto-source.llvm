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
/* Intentionally no assert(): test invariants survive -DNDEBUG. */
#define REQUIRE(expr) do { if (!(expr)) abort(); } while (0)
static void positive(void) {
    OresOwnedI64Region* region = oreslang_owned_i64_region_open_handles(2);
    const uint64_t a = oreslang_owned_i64_region_new_handle(region, 41);
    const uint64_t b = oreslang_owned_i64_region_new_handle(region, 42);
    REQUIRE(a != b);
    REQUIRE(oreslang_owned_i64_region_read_handle(region, a) == 41);
    REQUIRE(oreslang_owned_i64_region_read_handle(region, b) == 42);
    oreslang_owned_i64_region_drop_handle(region, a);
    const uint64_t c = oreslang_owned_i64_region_new_handle(region, 99);
    REQUIRE(c != a);
    REQUIRE(oreslang_owned_i64_region_read_handle(region, c) == 99);
    REQUIRE(oreslang_owned_i64_region_read_handle(region, b) == 42);
    oreslang_owned_i64_region_drop_handle(region, b);
    oreslang_owned_i64_region_drop_handle(region, c);
    oreslang_owned_i64_region_close(region);
}
#if defined(__unix__) || defined(__APPLE__)
static void negative(int kind) {
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        struct rlimit limit = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limit);
        OresOwnedI64Region* region = oreslang_owned_i64_region_open_handles(1);
        const uint64_t a = oreslang_owned_i64_region_new_handle(region, 23);
        switch (kind) {
            case 0: oreslang_owned_i64_region_drop_handle(region, a);
                    oreslang_owned_i64_region_drop_handle(region, a); break;
            case 1: {
                OresOwnedI64Region* other = oreslang_owned_i64_region_open_handles(1);
                oreslang_owned_i64_region_drop_handle(other, a); break;
            }
            case 2: (void)oreslang_owned_i64_region_read_handle(region, 0); break;
            case 3: oreslang_owned_i64_region_close(region); break;
            case 4: (void)oreslang_owned_i64_region_new_handle(region, 10); break;
            case 5: oreslang_owned_i64_region_drop_handle(region, a);
                    (void)oreslang_owned_i64_region_new_handle(region, 10);
                    oreslang_owned_i64_region_drop_handle(region, a); break;
            case 6: oreslang_owned_i64_region_drop_handle(region, a);
                    (void)oreslang_owned_i64_region_new_handle(region, 10);
                    (void)oreslang_owned_i64_region_read_handle(region, a); break;
            case 7: {
                OresOwnedI64Region* other = oreslang_owned_i64_region_open_handles(1);
                (void)oreslang_owned_i64_region_read_handle(other, a); break;
            }
            case 8: (void)oreslang_owned_i64_region_read_handle(region, UINT64_MAX); break;
            case 9: oreslang_owned_i64_region_drop_handle(region, UINT64_MAX); break;
            default: _exit(95);
        }
        _exit(96);
    }
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif
int main(void) {
    positive();
#if defined(__unix__) || defined(__APPLE__)
    for (int i = 0; i < 10; ++i) negative(i);
#endif
    return 0;
}
