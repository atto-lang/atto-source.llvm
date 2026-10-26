#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/NativeScalarFutureAwait.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeScalarFutureAwait;

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
        auto future = NativeScalarFutureAwait::open(actor, 1);
        CHECK(future->complete(future->id(), 41));

        // A live READY Future owns a resource loan, so finalization cannot
        // reach terminal settlement while this frame remains live.
        CHECK(actor.outstanding_resource_loans() == 1);
        CHECK(!group.finalized(actor));
        CHECK(!actor.resource_admission_closed());
        CHECK(!actor.delivery_closed());
        CHECK(future->cancel(future->id()));
        CHECK(actor.outstanding_resource_loans() == 0);

        const std::string expected_failure(512, 'x');
        std::string injected_failure = expected_failure;
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
        CHECK(actor.resource_admission_closed());
        CHECK(!actor.delivery_closed());

        // CONTROL admission itself must fail closed once finalization seals
        // resources, even though the later delivery barrier is not published
        // yet because terminal settlement threw.
        int direct_control = 0;
        CHECK(!group.enqueue_control(actor, [&] { ++direct_control; }));
        CHECK(!group.has_pending_control(actor));
        CHECK(direct_control == 0);

        // The same terminal seal must retire ordinary user-turn admission.
        // Otherwise a failed finalization settlement could re-enter actor code
        // before its serialized finalization retry completes.
        int direct_user_turn = 0;
        CHECK(group.run_user_turn(actor, [&] { ++direct_user_turn; })
              == NativeActorGroup::UserTurn::retired);
        CHECK(direct_user_turn == 0);

        // Retry with no error: the first sealed outcome must remain sticky.
        CHECK(group.finalized(actor));
        bool done_failed_exactly = false;
        try { actor.get_done().get(); }
        catch (const std::runtime_error& e) {
            done_failed_exactly = std::string(e.what()) == expected_failure;
        }
        CHECK(done_failed_exactly);
    }

    {
        NativeActorGroup group(191005, 1);
        auto actor = group.adopt(191005, 202, 1, [] {});
        CHECK(group.mark_ready(actor));
        auto finalizer = group.runtime_finalizer(actor);
        group.close();

        const std::string expected_failure(512, 'z');
        std::string injected_failure = expected_failure;
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

        CHECK(finalizer.finalized());
        bool done_failed_exactly = false;
        try { actor.get_done().get(); }
        catch (const std::runtime_error& e) {
            done_failed_exactly = std::string(e.what()) == expected_failure;
        }
        CHECK(done_failed_exactly);
    }
}

static void failed_startup_finalization_cannot_resurrect_ready() {
    NativeActorGroup group(191006, 1);
    auto actor = group.adopt(191006, 203, 1, [] {});

    const std::string expected_failure(512, 's');
    std::string injected_failure = expected_failure;
    bool threw = false;
    fail_after_allocations.store(0, std::memory_order_release);
    try {
        (void)group.finalized(actor, std::move(injected_failure));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    fail_after_allocations.store(-1, std::memory_order_release);
    CHECK(threw);
    CHECK(actor.resource_admission_closed());
    CHECK(!actor.delivery_closed());

    // Finalization has already sealed this incarnation. A delayed successful
    // startup callback must not resurrect READY while terminal settlement is
    // waiting for its serialized retry.
    CHECK(!group.mark_ready(actor));

    CHECK(group.finalized(actor));

    bool ready_failed_exactly = false;
    try { actor.get_ready().get(); }
    catch (const std::runtime_error& e) {
        ready_failed_exactly = std::string(e.what()) == expected_failure;
    }
    CHECK(ready_failed_exactly);

    bool done_failed_exactly = false;
    try { actor.get_done().get(); }
    catch (const std::runtime_error& e) {
        done_failed_exactly = std::string(e.what()) == expected_failure;
    }
    CHECK(done_failed_exactly);
}

static void scalar_future_open_allocation_failures_release_one_loan() {
    std::uint32_t injected_failures = 0;
    bool saw_success = false;
    for (long long fail_index = 0; fail_index < 8; ++fail_index) {
        NativeActorGroup group(191200 + static_cast<std::uint64_t>(fail_index), 1);
        auto actor = group.adopt(
            191200 + static_cast<std::uint64_t>(fail_index),
            400 + static_cast<std::uint64_t>(fail_index), 1, [] {});
        CHECK(group.mark_ready(actor));

        std::shared_ptr<NativeScalarFutureAwait> frame;
        bool injected = false;
        fail_after_allocations.store(fail_index, std::memory_order_release);
        try {
            frame = NativeScalarFutureAwait::open(
                actor, 9000 + static_cast<std::uint64_t>(fail_index));
        } catch (const std::bad_alloc&) {
            injected = true;
        }
        fail_after_allocations.store(-1, std::memory_order_release);

        if (injected) {
            ++injected_failures;
            CHECK(!frame);
            CHECK(actor.outstanding_resource_loans() == 0);
        } else {
            saw_success = true;
            CHECK(frame);
            CHECK(actor.outstanding_resource_loans() == 1);
            CHECK(frame->cancel(frame->id()));
            CHECK(actor.outstanding_resource_loans() == 0);
        }

        CHECK(group.finalized(actor));
        actor.get_done().get();
        if (saw_success && injected_failures >= 3) break;
    }
    CHECK(saw_success);
    CHECK(injected_failures >= 3);
}

static void stop_workers_allocation_failure_keeps_waiters_retryable() {
    std::uint32_t injected_failures = 0;
    std::uint32_t successful_attempts = 0;

    // Sweep enough allocation sites to cover vector staging, both rejection
    // strings/exception objects, and the transition into the commit phase.
    // Any std::bad_alloc must leave all readiness waiter vectors intact so a
    // retry delivers every callback and stop request exactly once.
    for (long long fail_index = 0; fail_index < 24; ++fail_index) {
        const auto runtime =
            193000 + static_cast<std::uint64_t>(fail_index);
        NativeActorGroup group(runtime, 2);
        int stop_a = 0;
        int stop_b = 0;
        auto a = group.adopt(runtime, 701, 1, [&] { ++stop_a; });
        auto b = group.adopt(runtime, 702, 1, [&] { ++stop_b; });

        int ready_a = 0;
        int ready_b = 0;
        bool bad_ready_result = false;
        a.when_ready([&](const auto& result) {
            if (result.ready || result.error != "worker stopped before ready")
                bad_ready_result = true;
            ++ready_a;
        });
        b.when_ready([&](const auto& result) {
            if (result.ready || result.error != "worker stopped before ready")
                bad_ready_result = true;
            ++ready_b;
        });

        bool injected = false;
        fail_after_allocations.store(fail_index, std::memory_order_release);
        try {
            group.stop_workers();
        } catch (const std::bad_alloc&) {
            injected = true;
        }
        fail_after_allocations.store(-1, std::memory_order_release);

        if (injected) {
            ++injected_failures;
            // Retry after the synthetic allocator fault. No callback removed
            // by the failed attempt may have been lost.
            group.stop_workers();
        } else {
            ++successful_attempts;
        }

        CHECK(!bad_ready_result);
        CHECK(ready_a == 1);
        CHECK(ready_b == 1);
        CHECK(stop_a == 1);
        CHECK(stop_b == 1);

        CHECK(group.finalized(a));
        CHECK(group.finalized(b));
        bool a_done_failed = false;
        bool b_done_failed = false;
        try { a.get_done().get(); }
        catch (const std::runtime_error& e) {
            a_done_failed =
                std::string(e.what()) == "worker stopped before ready";
        }
        try { b.get_done().get(); }
        catch (const std::runtime_error& e) {
            b_done_failed =
                std::string(e.what()) == "worker stopped before ready";
        }
        CHECK(a_done_failed);
        CHECK(b_done_failed);
    }

    CHECK(injected_failures > 0);
    CHECK(successful_attempts > 0);
}

static void successful_done_cannot_be_rewritten_by_late_failure() {
    NativeActorGroup group(191007, 1);
    auto actor = group.adopt(191007, 204, 1, [] {});
    CHECK(group.mark_ready(actor));
    CHECK(group.finalized(actor));
    CHECK(!group.finalized(actor, "late conflicting failure"));
    actor.get_done().get();
}

static void concurrent_finalization_publishes_exact_winner() {
    for (std::uint32_t trial = 0; trial < 256; ++trial) {
        NativeActorGroup group(192000 + trial, 1);
        auto actor = group.adopt(192000 + trial, 500 + trial, 1, [] {});
        CHECK(group.mark_ready(actor));

        std::atomic<int> ready{0};
        bool success_won = false;
        bool failure_won = false;
        const std::string failure = "concurrent terminal failure " + std::to_string(trial);

        std::thread success_thread([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            success_won = group.finalized(actor);
        });
        std::thread failure_thread([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            failure_won = group.finalized(actor, failure);
        });
        success_thread.join();
        failure_thread.join();

        CHECK(success_won != failure_won);
        if (success_won) {
            actor.get_done().get();
        } else {
            bool exact = false;
            try { actor.get_done().get(); }
            catch (const std::runtime_error& e) {
                exact = std::string(e.what()) == failure;
            }
            CHECK(exact);
        }
        CHECK(!group.finalized(actor, "post-terminal rewrite"));
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

        const std::string expected_failure(512, 'f');
        std::string failure = expected_failure;
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

            CHECK(group.finalized(actor));
        } else {
            CHECK(finalized);
            saw_success_after_window = true;
        }

        bool done_failed_exactly = false;
        try { actor.get_done().get(); }
        catch (const std::runtime_error& e) {
            done_failed_exactly = std::string(e.what()) == expected_failure;
        }
        CHECK(done_failed_exactly);

        if (saw_injected_failure && saw_success_after_window) break;
    }

    CHECK(saw_injected_failure);
    CHECK(saw_success_after_window);
}

int main() {
    live_group_rejects_outstanding_loan();
    recycled_numeric_ref_cannot_inspect_next_control_slot();
    concurrent_registration_and_finalization_have_one_winner();
    settlement_failure_keeps_terminal_resource_seal_retryable();
    failed_startup_finalization_cannot_resurrect_ready();
    scalar_future_open_allocation_failures_release_one_loan();
    stop_workers_allocation_failure_keeps_waiters_retryable();
    successful_done_cannot_be_rewritten_by_late_failure();
    concurrent_finalization_publishes_exact_winner();
    every_finalization_allocation_failure_is_retryable();
}
