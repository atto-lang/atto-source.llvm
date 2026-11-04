#include "oreslang/runtime/NativeRequestMailGateV1.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

using namespace oreslang::runtime;

namespace {
void original_request_mail_and_physical_actor_claim() {
    OresNativeExecutionAuthority authority;
    assert(ores_execution_init(&authority));
    const OresNativeActorRef actor{7, 11, 1};
    auto left_cancel = TaskCancellation::root();
    auto right_cancel = TaskCancellation::root();
    auto left = RequestContextLocalV1::open(
        {3, 4, 501, 1, actor}, left_cancel);
    auto right = RequestContextLocalV1::open(
        {3, 4, 502, 1, actor}, right_cancel);
    assert(left && right);
    NativeRequestMailGateV1 left_gate(authority, left, actor, 11);
    NativeRequestMailGateV1 right_gate(authority, right, actor, 12);
    RequestLocalKeyV1<std::string> trace;
    OresExecutionClaim claim{};
    const NativeRequestMailProofV1 unbound{claim, 11};
    assert(!left_gate.set(unbound, trace, std::string("unbound")));
    assert(ores_execution_enter(&authority, 1, 23, 0, &claim));
    const NativeRequestMailProofV1 left_turn{claim, 11};
    const NativeRequestMailProofV1 right_turn{claim, 12};
    assert(left_gate.authorized(left_turn));
    assert(!left_gate.authorized(right_turn));
    assert(right_gate.authorized(right_turn));
    assert(left_gate.set(left_turn, trace, std::string("trace-left")));
    assert(right_gate.set(right_turn, trace, std::string("trace-right")));
    assert(!right_gate.get_copy(left_turn, trace));
    assert(left_gate.get_copy(left_turn, trace) == std::string("trace-left"));
    assert(right_gate.get_copy(right_turn, trace) == std::string("trace-right"));

    // A copied claim on a producer callback thread cannot use the mailbox's
    // sequence as a bearer token or mutate the original request context.
    std::atomic<bool> stolen_denied{false};
    std::thread foreign([&] {
        stolen_denied.store(
            !left_gate.authorized(left_turn)
            && !left_gate.get_copy(left_turn, trace)
            && !left_gate.set(left_turn, trace, std::string("stolen"))
            && !left_gate.remove(left_turn, trace),
            std::memory_order_release);
    });
    foreign.join();
    assert(stolen_denied.load(std::memory_order_acquire));
    assert(left_gate.get_copy(left_turn, trace) == std::string("trace-left"));

    assert(ores_execution_leave(&authority, claim, 1));
    assert(!left_gate.get_copy(left_turn, trace));
    OresExecutionClaim resumed{};
    assert(ores_execution_enter(&authority, 1, 24, 0, &resumed));
    assert(resumed.sequence != claim.sequence);
    const NativeRequestMailProofV1 correct_resume{resumed, 11};
    assert(left_gate.get_copy(correct_resume, trace) == std::string("trace-left"));
    assert(!left_gate.get_copy({resumed, 13}, trace)); // other mail
    assert(right_gate.get_copy({resumed, 12}, trace) == std::string("trace-right"));

    // Physical STOP permits trusted cleanup to unwind, never additional
    // RequestLocal reads or writes on that previously admitted carrier.
    assert(ores_execution_stop(&authority, 1));
    assert(!left_gate.authorized(correct_resume));
    assert(!left_gate.get_copy(correct_resume, trace));
    assert(!left_gate.set(correct_resume, trace, std::string("after-stop")));
    assert(ores_execution_leave(&authority, resumed, 0));
    assert(ores_execution_retired(&authority, 1));
    assert(ores_execution_restart(&authority) == 2);
    OresExecutionClaim restarted{};
    assert(ores_execution_enter(&authority, 2, 25, 0, &restarted));
    assert(!left_gate.authorized({restarted, 11}));
    assert(!right_gate.authorized({restarted, 12}));
    assert(ores_execution_leave(&authority, restarted, 0));
    assert(ores_execution_stop(&authority, 2));
    assert(ores_execution_retired(&authority, 2));
    assert(left_cancel->cancel("request-end"));
    assert(right_cancel->cancel("request-end"));
    left_cancel->retire();
    right_cancel->retire();
    assert(ores_execution_destroy(&authority));
}
} // namespace

int main() {
    original_request_mail_and_physical_actor_claim();
    std::puts("NATIVE_REQUEST_MAIL_GATE_HOST_ONLY_PASS");
}
