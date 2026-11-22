#include "oreslang/runtime/SourceCooperationQuantum.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

// Symbols emitted by the real LLVM compiler from native-rt-aot-counter.ores.
// The frame must remain on one carrier, owned until explicit cancellation.
// frame[0]: 0=fresh, 1=active, 2=cancelled; frame[1]: private scalar.
// These are not ActorRef, await or automatic native-stack preemption.
extern "C" int oreslang_rt_spin_init(std::int64_t*) noexcept;
extern "C" int oreslang_rt_spin_resume(std::int64_t*, std::int64_t) noexcept;
extern "C" int oreslang_rt_spin_cancel(std::int64_t*) noexcept;

[[noreturn]] static void fatal(const char* message) {
    std::fprintf(stderr, "AOT continuation verification failed: %s\n", message);
    std::abort();
}
static void check(bool ok, const char* message) {
    if (!ok) fatal(message);
}

int main() {
    std::int64_t frame[2] = {0, 0};
    check(oreslang_rt_spin_resume(frame, 64) == -1, "uninitialized resume accepted");
    check(frame[0] == 0 && frame[1] == 0, "invalid resume changed frame");
    check(oreslang_rt_spin_cancel(frame) == 0, "uninitialized cancel accepted");
    check(oreslang_rt_spin_init(frame) == 1, "first initialization failed");
    check(frame[0] == 1 && frame[1] == 41, "bad compiled initial state");
    check(oreslang_rt_spin_init(frame) == 0, "double init accepted");
    check(oreslang_rt_spin_resume(frame, 0) == -1, "zero-fuel resume accepted");
    check(oreslang_rt_spin_resume(frame, -1) == -1, "negative-fuel resume accepted");
    check(frame[1] == 41, "denied resume mutated counter");
    check(oreslang_rt_spin_resume(frame, 1) == 1, "one-reduction handoff failed");
    check(frame[1] == 42, "committed reduction replayed or skipped");
    check(oreslang_rt_spin_resume(frame, 4000) == 1, "oversized fuel failed to yield");
    check(frame[1] == 554, "compiler failed mandatory 512 backedge cap");

    // A single host carrier repeatedly transfers a saved, compiler-generated
    // guest frame, alternating with ordered host work. This is NOT yet two
    // compiled Oreslang actors, a mailbox, or a full HungryCarrier integration.
    std::uint64_t host_message_sum = 0;
    oreslang::runtime::SourceCooperationQuantum quantum;
    for (unsigned message = 1; message <= 64; ++message) {
        quantum.beginDispatch();
        check(oreslang_rt_spin_resume(frame, 64) == 1,
              "guest failed to reach bounded handoff");
        for (unsigned reduction = 0; reduction < 64; ++reduction) {
            if (quantum.completedBackedgeNeedsCheck()) {
                // The host quantum owns time/watchdog sampling and only
                // records handoffs after compiled guest state is committed.
                (void)quantum.shouldCooperateAtCheck();
            }
        }
        quantum.recordedCooperate();
        host_message_sum += message;  // ordered host work on the same thread
        check(frame[1] == 554 + static_cast<std::int64_t>(message) * 64,
              "continuation lost state after a handoff");
    }
    check(host_message_sum == 2080, "host message order/progress invalid");
    check(oreslang_rt_spin_cancel(frame) == 1, "cancel failed");
    check(frame[0] == 2 && frame[1] == 0, "cancel did not zeroize the frame");
    check(oreslang_rt_spin_resume(frame, 1) == -1, "resumed cancelled frame");
    check(oreslang_rt_spin_init(frame) == 0, "cancelled frame resurrected");
    check(oreslang_rt_spin_cancel(frame) == 0, "double cancellation accepted");
    check(frame[0] == 2 && frame[1] == 0, "terminal frame was changed");

    std::int64_t invalid_frame[2] = {99, 123};
    check(oreslang_rt_spin_resume(invalid_frame, 2) == -1, "forged state accepted");
    check(invalid_frame[0] == 99 && invalid_frame[1] == 123,
          "invalid state was mutated");
    std::puts("AOT_SOURCE_SCALAR_LOOP_RESUMPTION_PASS");
    return 0;
}
