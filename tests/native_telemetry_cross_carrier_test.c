#include "oreslang/runtime/NativeTelemetrySpan.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct TestTokens {
    uint64_t foreign;
    uint64_t own;
    int misuse;
} TestTokens;

static void* carrier(void* argument) {
    TestTokens* t = (TestTokens*)argument;
    t->own = ores_native_telemetry_span_begin("another-carrier", 15);
    assert(t->own != 0);
    assert(t->own != t->foreign);
    if (t->misuse) {
        // A previously valid parent-carrier token must NEVER close this
        // worker's live span, even if both threads are at stack depth 1.
        ores_native_telemetry_span_end(t->foreign);
        _exit(101); // forbidden: cross-carrier close unexpectedly returned
    }
    ores_native_telemetry_span_end(t->own);
    return NULL;
}

int main(void) {
    ores_native_telemetry_set_enabled(1);
    const uint64_t main_span = ores_native_telemetry_span_begin("main-carrier", 12);
    assert(main_span != 0);
    TestTokens positive = {main_span, 0, 0};
    pthread_t worker;
    assert(pthread_create(&worker, NULL, carrier, &positive) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(positive.own != main_span);
    assert(ores_native_telemetry_snapshot().active == 1);

    // Isolate expected SIGABRT so negative verification cannot tear down
    // the test runner. Do not generate a potentially sensitive core dump.
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        TestTokens negative = {main_span, 0, 1};
        pthread_t illegal;
        if (pthread_create(&illegal, NULL, carrier, &negative) != 0)
            _exit(102);
        (void)pthread_join(illegal, NULL);
        _exit(103); // must never execute
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    ores_native_telemetry_span_end(main_span);
    const OresNativeTelemetrySnapshot result = ores_native_telemetry_snapshot();
    assert(result.started == 1 && result.ended == 1 && result.active == 0);
    return 0;
}
