#include "oreslang/runtime/NativeActorSharedPayloadBridge.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using oreslang::runtime::NativeActorSharedPayloadBridge;
using oreslang::runtime::SharedImmutablePayload;
using SendStatus = NativeActorSharedPayloadBridge::SendStatus;
using namespace std::chrono_literals;

static void check(bool condition) {
    if (!condition) {
        std::fputs("NATIVE_SHARED_PAYLOAD_SCHEDULER_BRIDGE_FAIL\n", stderr);
        std::abort();
    }
}

struct Run {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef source{};
    OresNativeActorRef target{};
    NativeActorSharedPayloadBridge* bridge = nullptr;
    std::vector<std::uint8_t> bytes;
    const std::uint8_t* original_address = nullptr;
    const bool seal;
    std::atomic<bool> source_done{false};
    std::atomic<bool> source_ok{true};
    std::atomic<unsigned> seen{0};
    std::atomic<unsigned> control{0};
    std::atomic<unsigned> handler_errors{0};
    std::atomic<unsigned> cross_mail_denied{0};
    explicit Run(bool sealed, std::size_t mib)
        : bytes(mib, 0x5a), seal(sealed) {
        original_address = bytes.data();
    }
};

static void on_payload(void* raw, OresNativeActorRef target,
                       const SharedImmutablePayload& message) {
    auto& run = *static_cast<Run*>(raw);
    if (!ores_native_actor_scheduler_is_current_actor(run.scheduler, target) ||
        target.actor_slot != run.target.actor_slot ||
        message.size() != 1024 * 1024) {
        run.handler_errors.fetch_add(1);
        return;
    }
    message.with_read_only_span([&](auto span) {
        if (span.data() != run.original_address ||
            span.size() != 1024 * 1024 ||
            span.front() != 0x5a || span.back() != 0x5a)
            run.handler_errors.fetch_add(1);
    });
    if (!run.seal && run.control.load() != 1)
        run.handler_errors.fetch_add(1); // CONTROL outranks queued DATA.
    std::uint64_t observed_mail_sequence = 0;
    if (!ores_native_actor_scheduler_current_data_mail_sequence(
            run.scheduler, run.target, &observed_mail_sequence) ||
        observed_mail_sequence !=
            static_cast<std::uint64_t>(run.seen.load()) + 2)
        run.handler_errors.fetch_add(1); // Exact FIFO receipt, not payload ID.
    run.seen.fetch_add(1);
}

static void on_control(void* raw, std::size_t actor_slot, std::uint64_t value) {
    auto& run = *static_cast<Run*>(raw);
    if (actor_slot != run.target.actor_slot || value != 77 ||
        !ores_native_actor_scheduler_is_current_actor(run.scheduler, run.target))
        run.handler_errors.fetch_add(1);
    run.control.fetch_add(1);
}

static void on_message(void* raw, std::size_t actor_slot, std::uint64_t value) {
    auto& run = *static_cast<Run*>(raw);
    if (actor_slot == run.target.actor_slot) {
        std::uint64_t ordinal = 0;
        if (!ores_native_actor_scheduler_current_data_mail_sequence(
                run.scheduler, run.target, &ordinal)) {
            run.handler_errors.fetch_add(1);
            return;
        }
        if (ordinal == 1) {
            // Deliberately injected valid-looking ticket for descriptor #1
            // was admitted as DATA mail #1; its real owner is DATA mail #2.
            // A generation-only check would wrongly grant this descriptor.
            if (run.bridge->dispatch_data(actor_slot, value))
                run.handler_errors.fetch_add(1);
            else
                run.cross_mail_denied.fetch_add(1);
            return;
        }
        if (!run.bridge->dispatch_data(actor_slot, value))
            run.handler_errors.fetch_add(1);
        // The same current actor lane must NOT be able to redeem a
        // consumed descriptor ticket twice.
        if (run.bridge->dispatch_data(actor_slot, value))
            run.handler_errors.fetch_add(1);
        return;
    }
    if (actor_slot != run.source.actor_slot || value != 123 ||
        !ores_native_actor_scheduler_is_current_actor(run.scheduler, run.source)) {
        run.source_ok.store(false);
        run.source_done.store(true);
        return;
    }
    // Native malicious-host regression: enqueue a guessed first descriptor
    // ticket in DATA mail #1 BEFORE bridge publication. The actual first
    // descriptor will be stamped with DATA mail #2. A generation-only
    // implementation would leak the still-valid backing to the wrong mail.
    constexpr std::uint64_t cross_mail_ticket = (std::uint64_t(1) << 32) | 128;
    if (ores_native_actor_scheduler_try_send(
            run.scheduler, run.target, cross_mail_ticket)
        != ORES_NATIVE_SEND_ACCEPTED)
        run.source_ok.store(false);
    // The 1 MiB vector is moved into immutable backing while A owns its turn.
    // Legitimate descriptor sends use only the real scheduler DATA admission.
    auto published = SharedImmutablePayload::publish(std::move(run.bytes));
    for (unsigned i = 0; i < 100; ++i) {
        if (run.bridge->try_send_from(run.source, published) !=
            SendStatus::admitted) {
            run.source_ok.store(false);
            break;
        }
    }
    // Source dropped this reference. The admitted descriptors retain backing.
    published = SharedImmutablePayload{};
    if (run.seal) {
        if (!ores_native_actor_scheduler_stop_sealed(
                run.scheduler, run.target))
            run.source_ok.store(false);
    } else {
        if (!ores_native_actor_scheduler_publish_control(
                run.scheduler, run.target, 77))
            run.source_ok.store(false);
    }
    run.source_done.store(true, std::memory_order_release);
}

static void exercise(bool sealed) {
    constexpr std::size_t mib = 1024 * 1024;
    Run run(sealed, mib);
    NativeActorSharedPayloadBridge bridge(128, 128*mib, &on_payload, &run);
    run.bridge = &bridge;
    run.scheduler = ores_native_actor_scheduler_open_control(
        2, 128, 1, &on_message, &on_control, &run);
    check(run.scheduler != nullptr);
    run.source = ores_native_actor_scheduler_spawn(run.scheduler, 0);
    run.target = ores_native_actor_scheduler_spawn(run.scheduler, 1);
    check(run.source.scheduler_domain != 0 && run.target.scheduler_domain != 0);
    check(bridge.bind(run.scheduler, run.target));
    check(!bridge.close_after_done());
    auto invalid = SharedImmutablePayload::publish({8});
    // External callers are not the current actor: fail closed, even with a
    // valid-looking source ref and valid immutable descriptor.
    check(bridge.try_send_from(run.source, invalid) ==
          SendStatus::invalid_sender);
    check(bridge.queued_slots() == 0);
    // Receipt is minted only on successful native DATA admission. It cannot
    // be supplied by the guest and is not a handler-completion receipt.
    std::uint64_t trigger_sequence = 0;
    check(ores_native_actor_scheduler_try_send_receipted(
        run.scheduler, run.source, 123, nullptr) ==
        ORES_NATIVE_SEND_INVALID_REF);
    OresNativeActorRef forged = run.source;
    ++forged.generation;
    trigger_sequence = 99;
    check(ores_native_actor_scheduler_try_send_receipted(
        run.scheduler, forged, 123, &trigger_sequence) ==
        ORES_NATIVE_SEND_INVALID_REF);
    check(trigger_sequence == 0);
    check(ores_native_actor_scheduler_try_send_receipted(
        run.scheduler, run.source, 123, &trigger_sequence) ==
        ORES_NATIVE_SEND_ACCEPTED);
    check(trigger_sequence == 1);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!run.source_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(run.source_done.load(std::memory_order_acquire));
    check(run.source_ok.load());
    if (sealed) {
        // Only one carrier exists, so no target callback can run before A's
        // STOP_SEALED, and all 100 accepted tickets stay pinned until DONE.
        check(bridge.queued_slots() == 100);
        check(bridge.queued_charged_bytes() == 100 * mib);
    } else {
        // Wait for actual target delivery rather than treating scheduler
        // shutdown itself as evidence that all 100 callbacks executed.
        const auto delivery_deadline = std::chrono::steady_clock::now() + 5s;
        while ((run.seen.load() != 100 || run.control.load() != 1) &&
               std::chrono::steady_clock::now() < delivery_deadline)
            std::this_thread::yield();
        check(run.seen.load() == 100 && run.control.load() == 1);
    }
    ores_native_actor_scheduler_retire(run.scheduler);
    check(ores_native_actor_scheduler_drain(run.scheduler));
    check(ores_native_actor_scheduler_poll_done(run.scheduler, run.target));
    check(bridge.close_after_done()); // Reclaim abandoned STOP_SEALED mail.
    check(bridge.close_after_done());
    check(bridge.queued_slots() == 0);
    check(bridge.queued_charged_bytes() == 0);
    check(bridge.rejected_dispatch() == (sealed ? 0u : 101u));
    check(run.cross_mail_denied.load() == (sealed ? 0u : 1u));
    check(bridge.handler_failures() == 0);
    check(run.handler_errors.load() == 0);
    check(bridge.delivered() == (sealed ? 0u : 100u));
    check(run.seen.load() == (sealed ? 0u : 100u));
    check(run.control.load() == (sealed ? 0u : 1u));
    OresNativeActorTerminalState terminal{};
    check(ores_native_actor_scheduler_terminal_state(
        run.scheduler, run.target, &terminal));
    check(terminal.accepted_user_messages == 101);
    check(terminal.processed_user_messages == (sealed ? 0u : 101u));
    check(terminal.abandoned_user_messages == (sealed ? 101u : 0u));
    check(ores_native_actor_scheduler_try_destroy(run.scheduler));
}

int main() {
    exercise(false); // Real native actor DATA delivery and CONTROL priority.
    exercise(true);  // STOP_SEALED abandonment and terminal ref release.
    std::puts("NATIVE_SHARED_PAYLOAD_SCHEDULER_BRIDGE_PASS");
    return 0;
}
