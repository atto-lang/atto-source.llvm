#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void positive(void) {
    OresOwnedI64Region* r = oreslang_owned_i64_region_open(2);
    int64_t* a = oreslang_owned_i64_region_new(r, 41);
    int64_t* b = oreslang_owned_i64_region_new(r, 42);
    assert(*a == 41 && *b == 42 && a != b);
    oreslang_owned_i64_region_drop(r, a);
    int64_t* c = oreslang_owned_i64_region_new(r, 99);
    assert(c == a && *c == 99 && *b == 42);
    oreslang_owned_i64_region_drop(r, b);
    oreslang_owned_i64_region_drop(r, c);
    oreslang_owned_i64_region_close(r);
}
#if defined(__unix__) || defined(__APPLE__)
static void must_abort(int kind) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        struct rlimit limit = {0, 0};
        setrlimit(RLIMIT_CORE, &limit);
        OresOwnedI64Region* r = oreslang_owned_i64_region_open(2);
        int64_t* a = oreslang_owned_i64_region_new(r, 23);
        int64_t outside = 23;
        if (kind == 0) { oreslang_owned_i64_region_drop(r, a); oreslang_owned_i64_region_drop(r, a); }
        else if (kind == 1) oreslang_owned_i64_region_drop(r, &outside);
        else if (kind == 2) oreslang_owned_i64_region_drop(r, (int64_t*)((unsigned char*)a + 1));
        else if (kind == 3) oreslang_owned_i64_region_close(r);
        else if (kind == 4) oreslang_owned_i64_region_new(r, 10), oreslang_owned_i64_region_new(r, 11);
        else abort();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif
int main(void) {
    positive();
#if defined(__unix__) || defined(__APPLE__)
    for (int k = 0; k < 5; ++k) must_abort(k);
#endif
    return 0;
}
