#include "oreslang/runtime/NativeRequestMailGateV1.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace oreslang::runtime;

namespace {

// The source actor dispatcher owns this reader and changes it only on the
// physical owner carrier at exact mailbox-turn admission. Neither a producer
// nor a guest caller passes a mail sequence to get_copy()/set()/remove().
struct TrustedDispatchMail {
    std::optional<std::uint64_t> active;
};
std::optional<std::uint64_t> current_mail(void* raw) noexcept {
    return static_cast<TrustedDispatchMail*>(raw)->active;
}

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
    TrustedDispatchMail mailbox{};
    NativeRequestMailGateV1 left_gate(
        authority, left, actor, 11, current_mail, &mailbox);
    NativeRequestMailGateV1 right_gate(
        authority, right, actor, 12, current_mail, &mailbox);
    RequestLocalKeyV1<std::string> trace;
    OresExecutionClaim claim{};

    assert(!left_gate.set(claim, trace, std::string("unbound")));
    assert(ores_execution_enter(&authority, 1, 23, 0, &claim));
    assert(!left_gate.authorized(claim)); // no admitted mailbox
    mailbox.active = 11;                   // authenticated original mail
    assert(left_gate.authorized(claim));
    assert(!right_gate.authorized(claim));
    assert(left_gate.set(claim, trace, std::string("trace-left")));
    assert(left_gate.get_copy(claim, trace) == std::string("trace-left"));

    // Different accepted mail on the SAME actor cannot borrow the left
    // request's authority, even with a copied physical claim and request ptr.
    mailbox.active = 12;
    assert(!left_gate.authorized(claim));
    assert(!left_gate.get_copy(claim, trace));
    assert(!left_gate.set(claim, trace, std::string("forged")));
    assert(right_gate.authorized(claim));
    assert(right_gate.set(claim, trace, std::string("trace-right")));
    assert(right_gate.get_copy(claim, trace) == std::string("trace-right"));

    mailbox.active = 11;
    assert(!right_gate.get_copy(claim, trace));
    assert(left_gate.get_copy(claim, trace) == std::string("trace-left"));

    // A generic Future completion, detached Task, or callback without mail
    // provenance must be unbound even on the correct native actor carrier.
    mailbox.active.reset();
    assert(!left_gate.authorized(claim));
    assert(!right_gate.authorized(claim));
    assert(!left_gate.remove(claim, trace));
    mailbox.active = 11;

    // Copying the exact physical claim to another thread is insufficient.
    // The reader's test fixture is stable during this foreign-thread check.
    std::atomic<bool> foreign_denied{false};
    std::thread foreign([&] {
        foreign_denied.store(
            !left_gate.authorized(claim)
            && !left_gate.get_copy(claim, trace)
            && !left_gate.set(claim, trace, std::string("stolen"))
            && !left_gate.remove(claim, trace),
            std::memory_order_release);
    });
    foreign.join();
    assert(foreign_denied.load(std::memory_order_acquire));
    assert(left_gate.get_copy(claim, trace) == std::string("trace-left"));

    assert(ores_execution_leave(&authority, claim, 1));
    assert(!left_gate.get_copy(claim, trace));
    OresExecutionClaim resumed{};
    assert(ores_execution_enter(&authority, 1, 24, 0, &resumed));
    assert(resumed.sequence != claim.sequence);
    mailbox.active = 11; // immutable original mail restored on SourceTask resume
    assert(left_gate.get_copy(resumed, trace) == std::string("trace-left"));
    assert(!left_gate.get_copy(claim, trace)); // physical turn ABA
    mailbox.active = 13;
    assert(!left_gate.get_copy(resumed, trace));
    mailbox.active = 12;
    assert(right_gate.get_copy(resumed, trace) == std::string("trace-right"));

    // STOPPING can retain a cleanup carrier but may not grant RequestLocal.
    assert(ores_execution_stop(&authority, 1));
    assert(!right_gate.authorized(resumed));
    assert(!right_gate.get_copy(resumed, trace));
    assert(!right_gate.set(resumed, trace, std::string("after-stop")));
    assert(ores_execution_leave(&authority, resumed, 0));
    assert(ores_execution_retired(&authority, 1));
    assert(ores_execution_restart(&authority) == 2);
    OresExecutionClaim restarted{};
    assert(ores_execution_enter(&authority, 2, 25, 0, &restarted));
    mailbox.active = 11;
    assert(!left_gate.authorized(restarted));
    mailbox.active = 12;
    assert(!right_gate.authorized(restarted));
    assert(ores_execution_leave(&authority, restarted, 0));
    assert(ores_execution_stop(&authority, 2));
    assert(ores_execution_retired(&authority, 2));

    assert(left_cancel->cancel("request-end"));
    assert(right_cancel->cancel("request-end"));
    left_cancel->retire();
    right_cancel->retire();
    assert(ores_execution_destroy(&authority));
}

void constructor_rejects_invalid_reader_or_actor() {
    OresNativeExecutionAuthority authority;
    assert(ores_execution_init(&authority));
    const OresNativeActorRef actor{7, 11, 1};
    auto cancel = TaskCancellation::root();
    auto request = RequestContextLocalV1::open(
        {3, 4, 600, 1, actor}, cancel);
    assert(request);
    TrustedDispatchMail mailbox{};
    bool rejected = false;
    try {
        NativeRequestMailGateV1 bad(authority, request, actor, 1,
                                    nullptr, &mailbox);
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try {
        NativeRequestMailGateV1 bad(authority, request, actor, 1,
                                    current_mail, nullptr);
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try {
        NativeRequestMailGateV1 bad(
            authority, request, OresNativeActorRef{7, 11, 2},
            1, current_mail, &mailbox);
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    request->close();
    cancel->retire();
    assert(ores_execution_destroy(&authority));
}
} // namespace

int main() {
    original_request_mail_and_physical_actor_claim();
    constructor_rejects_invalid_reader_or_actor();
    std::puts("NATIVE_REQUEST_MAIL_GATE_HOST_ONLY_PASS");
}
