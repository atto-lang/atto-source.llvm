#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

using oreslang::runtime::NativeActorGroup;

static std::atomic<long long> fail_after_allocations{-1};

static bool injected_allocation_failure() {
    auto remaining = fail_after_allocations.load(std::memory_order_acquire);
    while (remaining >= 0) {
        if (remaining == 0) {
            fail_after_allocations.store(-1, std::memory_order_release);
            return true;
        }
        if (fail_after_allocations.compare_exchange_weak(
                remaining, remaining - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
            return false;
    }
    return false;
}

__attribute__((noinline)) void* operator new(std::size_t size) {
    if (injected_allocation_failure()) throw std::bad_alloc();
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void* operator new[](std::size_t size) {
    if (injected_allocation_failure()) throw std::bad_alloc();
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete[](void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

#define CHECK(x) do { if (!(x)) throw std::runtime_error("actor resource loan race: " #x); } while (false)

static void live_group_rejects_outstanding_loan() {
    NativeActorGroup group(191001, 1);
    auto actor = group.adopt(191001, 11, 1, [] {});
    CHECK(group.mark_ready(actor));
    actor.retain_resource_loan();
    CHECK(actor.outstanding_resource_loans() == 1);
    CHECK(!group.finalized(actor));
    CHECK(actor.outstanding_resource_loans() == 1);

    // A failed physical-finalization attempt is already terminal resource
    // admission. The existing loan may drain, but stale holders cannot keep
    // adding work between retries and starve DONE forever.
    bool late_rejected = false;
    try { actor.retain_resource_loan(); }
    catch (const std::logic_error&) { late_rejected = true; }
    CHECK(late_rejected);
    actor.release_resource_loan();
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(group.finalized(actor));
    CHECK(!group.finalized(actor));
    actor.get_done().get();
    bool rejected = false;
    try { actor.retain_resource_loan(); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
}

static void stop_seals_new_loans_but_existing_loans_can_drain() {
    NativeActorGroup group(191007, 1);
    int stops = 0;
    auto actor = group.adopt(191007, 204, 1, [&] { ++stops; });
    CHECK(group.mark_ready(actor));

    actor.retain_resource_loan();
    CHECK(actor.outstanding_resource_loans() == 1);
    group.stop_workers();
    CHECK(stops == 1);

    bool rejected = false;
    try { actor.retain_resource_loan(); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
    CHECK(actor.outstanding_resource_loans() == 1);

    actor.release_resource_loan();
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(group.finalized(actor));
    actor.get_done().get();
    group.close();
    CHECK(stops == 1);
}

static void close_seals_new_loans_but_existing_loans_can_drain() {
    NativeActorGroup group(191006, 1);
    auto actor = group.adopt(191006, 203, 1, [] {});
    CHECK(group.mark_ready(actor));
    auto finalizer = group.runtime_finalizer(actor);

    actor.retain_resource_loan();
    CHECK(actor.outstanding_resource_loans() == 1);
    group.close();
    CHECK(finalizer.ready_for_actor_turn_quiescence());
    CHECK(!finalizer.ready_for_resource_reclamation());

    bool rejected = false;
    try { actor.retain_resource_loan(); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
    CHECK(actor.outstanding_resource_loans() == 1);

    // The terminal bit must not turn release into a fatal underflow. Only
    // post-terminal admission is forbidden; previously admitted work drains.
    actor.release_resource_loan();
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(finalizer.finalized());
    actor.get_done().get();
}

static void close_and_resource_registration_have_one_winner() {
    std::uint32_t registration_wins = 0;
    std::uint32_t close_wins = 0;
    for (std::uint32_t trial = 0; trial < 2000; ++trial) {
        NativeActorGroup group(191200 + trial, 1);
        auto actor = group.adopt(191200 + trial, 500 + trial, 1, [] {});
        CHECK(group.mark_ready(actor));
        auto finalizer = group.runtime_finalizer(actor);

        std::atomic<int> ready{0};
        bool registered = false;
        bool refused = false;
        std::thread register_thread([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            try { actor.retain_resource_loan(); registered = true; }
            catch (const std::logic_error&) { refused = true; }
        });
        std::thread close_thread([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            group.close();
        });
        register_thread.join();
        close_thread.join();

        CHECK(registered != refused);
        CHECK(finalizer.ready_for_actor_turn_quiescence());
        bool late_rejected = false;
        try { actor.retain_resource_loan(); }
        catch (const std::logic_error&) { late_rejected = true; }
        CHECK(late_rejected);

        if (registered) {
            ++registration_wins;
            CHECK(actor.outstanding_resource_loans() == 1);
            CHECK(!finalizer.ready_for_resource_reclamation());
            actor.release_resource_loan();
        } else {
            ++close_wins;
            CHECK(actor.outstanding_resource_loans() == 0);
        }

        CHECK(finalizer.ready_for_resource_reclamation());
        CHECK(finalizer.finalized());
        actor.get_done().get();
    }
    CHECK(registration_wins + close_wins == 2000);
}

static void recycled_numeric_ref_cannot_inspect_next_control_slot() {
    NativeActorGroup group(191003, 1);
    auto old_ref = group.adopt(191003, 99, 1, [] {});
    CHECK(group.mark_ready(old_ref));
    CHECK(group.finalized(old_ref));
    old_ref.get_done().get();

    auto new_ref = group.adopt(191003, 99, 1, [] {});
    CHECK(group.mark_ready(new_ref));
    CHECK(old_ref == new_ref);
    CHECK(!old_ref.same_incarnation(new_ref));
    int ran = 0;
    CHECK(group.enqueue_control(new_ref, [&] { ++ran; }));
    CHECK(!group.has_pending_control(old_ref));
    CHECK(group.has_pending_control(new_ref));
    CHECK(group.run_control_turn(old_ref) == NativeActorGroup::ControlTurn::retired);
    CHECK(ran == 0);
    CHECK(group.run_control_turn(new_ref) == NativeActorGroup::ControlTurn::executed);
    CHECK(ran == 1);
    CHECK(group.finalized(new_ref));
    new_ref.get_done().get();
}

static void concurrent_registration_and_finalization_have_one_winner() {
    // A native contention probe against the
    // real NativeActorGroup generation ticket and terminal resource bit.
    // Every trial joins both threads before checking terminal invariants.
    std::uint32_t registration_wins = 0;
    std::uint32_t finalization_wins = 0;
    for (std::uint32_t trial = 0; trial < 2000; ++trial) {
        NativeActorGroup group(191002, 1);
        auto actor = group.adopt(191002, 100 + trial, 1, [] {});
        CHECK(group.mark_ready(actor));
        auto finalizer = group.runtime_finalizer(actor);
        group.close();
        CHECK(finalizer.ready_for_actor_turn_quiescence());
        std::atomic<int> ready{0};
        bool registered = false;
        bool refused = false;
        bool finalized = false;
        std::thread register_thread([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            try { actor.retain_resource_loan(); registered = true; }
            catch (const std::logic_error&) { refused = true; }
        });
        std::thread finalize_thread([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            finalized = finalizer.finalized();
        });
        register_thread.join();
        finalize_thread.join();

        CHECK(registered != finalized);
        CHECK(registered != refused);
        if (registered) {
            ++registration_wins;
            CHECK(!finalizer.ready_for_resource_reclamation());
            CHECK(actor.outstanding_resource_loans() == 1);
            CHECK(!finalizer.finalized());
            actor.release_resource_loan();
            CHECK(finalizer.ready_for_resource_reclamation());
            CHECK(finalizer.finalized());
        } else {
            ++finalization_wins;
            CHECK(refused);
            CHECK(actor.outstanding_resource_loans() == 0);
        }
        CHECK(!finalizer.finalized());
        actor.get_done().get();
        CHECK(actor.outstanding_resource_loans() == 0);
    }
    CHECK(registration_wins + finalization_wins == 2000);
}

static void settlement_failure_keeps_terminal_resource_seal_retryable() {
    {
        NativeActorGroup group(191004, 1);
        auto actor = group.adopt(191004, 201, 1, [] {});
        CHECK(group.mark_ready(actor));

        std::string injected_failure(512, 'x');
        bool threw = false;
        fail_after_allocations.store(0, std::memory_order_release);
        try {
            (void)group.finalized(actor, std::move(injected_failure));
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        fail_after_allocations.store(-1, std::memory_order_release);
        CHECK(threw);

        // A failed settlement keeps resource admission terminally sealed.
        // Retry belongs only to the serialized finalizer; late loans must not
        // resurrect physical state after logical termination has started.
        bool loan_rejected = false;
        try { actor.retain_resource_loan(); }
        catch (const std::logic_error&) { loan_rejected = true; }
        CHECK(loan_rejected);
        CHECK(actor.outstanding_resource_loans() == 0);

        std::string retry_failure(512, 'y');
        CHECK(group.finalized(actor, std::move(retry_failure)));
        bool done_failed = false;
        try { actor.get_done().get(); }
        catch (const std::runtime_error&) { done_failed = true; }
        CHECK(done_failed);
    }

    {
        NativeActorGroup group(191005, 1);
        auto actor = group.adopt(191005, 202, 1, [] {});
        CHECK(group.mark_ready(actor));
        auto finalizer = group.runtime_finalizer(actor);
        group.close();

        std::string injected_failure(512, 'z');
        bool threw = false;
        fail_after_allocations.store(0, std::memory_order_release);
        try {
            (void)finalizer.finalized(std::move(injected_failure));
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        fail_after_allocations.store(-1, std::memory_order_release);
        CHECK(threw);

        bool loan_rejected = false;
        try { actor.retain_resource_loan(); }
        catch (const std::logic_error&) { loan_rejected = true; }
        CHECK(loan_rejected);
        CHECK(actor.outstanding_resource_loans() == 0);

        std::string retry_failure(512, 'q');
        CHECK(finalizer.finalized(std::move(retry_failure)));
        bool done_failed = false;
        try { actor.get_done().get(); }
        catch (const std::runtime_error&) { done_failed = true; }
        CHECK(done_failed);
    }
}

static void every_finalization_allocation_failure_is_retryable() {
    bool saw_injected_failure = false;
    bool saw_success_after_window = false;

    // Sweep the first allocation sites in physical finalization rather than
    // depending on one library-specific allocation order. Any injected
    // std::bad_alloc must leave both the resource seal and one-shot DONE
    // promise retryable.
    for (long long fail_index = 0; fail_index < 32; ++fail_index) {
        NativeActorGroup group(191100 + static_cast<std::uint64_t>(fail_index), 1);
        auto actor = group.adopt(
            191100 + static_cast<std::uint64_t>(fail_index),
            300 + static_cast<std::uint64_t>(fail_index), 1, [] {});
        CHECK(group.mark_ready(actor));

        std::string failure(512, 'f');
        bool injected = false;
        bool finalized = false;
        fail_after_allocations.store(fail_index, std::memory_order_release);
        try {
            finalized = group.finalized(actor, std::move(failure));
        } catch (const std::bad_alloc&) {
            injected = true;
        }
        fail_after_allocations.store(-1, std::memory_order_release);

        if (injected) {
            saw_injected_failure = true;
            bool loan_rejected = false;
            try { actor.retain_resource_loan(); }
            catch (const std::logic_error&) { loan_rejected = true; }
            CHECK(loan_rejected);
            CHECK(actor.outstanding_resource_loans() == 0);

            std::string retry_failure(512, 'r');
            CHECK(group.finalized(actor, std::move(retry_failure)));
        } else {
            CHECK(finalized);
            saw_success_after_window = true;
        }

        bool done_failed = false;
        try { actor.get_done().get(); }
        catch (const std::runtime_error&) { done_failed = true; }
        CHECK(done_failed);

        if (saw_injected_failure && saw_success_after_window) break;
    }

    CHECK(saw_injected_failure);
    CHECK(saw_success_after_window);
}

int main() {
    live_group_rejects_outstanding_loan();
    stop_seals_new_loans_but_existing_loans_can_drain();
    close_seals_new_loans_but_existing_loans_can_drain();
    close_and_resource_registration_have_one_winner();
    recycled_numeric_ref_cannot_inspect_next_control_slot();
    concurrent_registration_and_finalization_have_one_winner();
    settlement_failure_keeps_terminal_resource_seal_retryable();
    every_finalization_allocation_failure_is_retryable();
}
