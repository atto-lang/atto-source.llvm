#include "oreslang/runtime/NativeAsyncMailboxAdmission.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeAsyncMailboxAdmission;
using Registration = NativeAsyncMailboxAdmission::Registration;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("native async mailbox admission: " #x); } while (false)

struct Fixture {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef producer{};
    OresNativeActorRef target{};
    std::atomic<int> ready{0};
    std::atomic<int> failed{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> errors{0};
};

static void on_data(void*, std::size_t, std::uint64_t) {}

static void on_wake(
        void* raw, std::size_t actor, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    auto* f = static_cast<Fixture*>(raw);
    if (actor != 0
            || !ores_native_actor_scheduler_is_current_actor(
                f->scheduler, f->producer)
            || !ores_native_actor_scheduler_is_current_wake(
                f->scheduler, ticket))
        ++f->errors;
    switch (outcome) {
        case ORES_NATIVE_WAKE_READY: ++f->ready; break;
        case ORES_NATIVE_WAKE_FAILED: ++f->failed; break;
        case ORES_NATIVE_WAKE_CANCELLED: ++f->cancelled; break;
        default: ++f->errors; break;
    }
}

static Fixture make_fixture() {
    Fixture f;
    f.scheduler = ores_native_actor_scheduler_open_with_wakes(
        2, 1, 1, on_data, nullptr, nullptr, on_wake, &f);
    // make_fixture returns by value, so callback context cannot point at f.
    // Callers replace the context by constructing in-place instead.
    return f;
}

static void setup(Fixture& f) {
    f.scheduler = ores_native_actor_scheduler_open_with_wakes(
        2, 1, 1, on_data, nullptr, nullptr, on_wake, &f);
    CHECK(f.scheduler);
    f.producer = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    f.target = ores_native_actor_scheduler_spawn(f.scheduler, 1);
    CHECK(f.producer.scheduler_domain && f.target.scheduler_domain);
}

static OresNativeWakeTicket reserve(Fixture& f, std::uint64_t descriptor) {
    OresNativeWakeTicket ticket{};
    CHECK(ores_native_actor_scheduler_reserve_wake(
        f.scheduler, f.producer, f.target,
        descriptor, 3, 5, descriptor, &ticket));
    return ticket;
}

static void finish(Fixture& f) {
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(!f.errors.load());
    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.producer));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.producer));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}

static void fifo_transfer_prevents_sync_credit_steal() {
    Fixture f;
    setup(f);
    NativeAsyncMailboxAdmission admission(1, 8, 4, 32);
    CHECK(admission.try_acquire(8));

    NativeAsyncMailboxAdmission::Handle first, second;
    auto t1 = reserve(f, 11);
    auto t2 = reserve(f, 12);
    CHECK(admission.register_waiter(8, f.scheduler, t1, &first)
          == Registration::Waiting);
    CHECK(admission.register_waiter(8, f.scheduler, t2, &second)
          == Registration::Waiting);
    CHECK(admission.pending_waiters() == 2);
    CHECK(admission.pending_bytes() == 16);
    CHECK(!admission.try_acquire(1));

    admission.release_acquired(8);
    CHECK(admission.transferred(first));
    CHECK(!admission.transferred(second));
    CHECK(admission.used_frames() == 1);
    CHECK(admission.used_bytes() == 8);
    CHECK(admission.pending_waiters() == 1);
    CHECK(admission.pending_bytes() == 8);
    CHECK(!admission.try_acquire(1));
    CHECK(!admission.cancel(first));
    CHECK(admission.cancel(second));
    CHECK(admission.pending_waiters() == 0);
    CHECK(admission.pending_bytes() == 0);

    CHECK(admission.commit(first) == ORES_NATIVE_WAKE_PUBLISHED);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == 1);
    CHECK(f.failed.load() == 0);
    admission.release_acquired(8);
    CHECK(admission.used_frames() == 0);
    finish(f);
}

static void target_stop_fails_reserved_waiter_without_false_admission() {
    Fixture f;
    setup(f);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);
    CHECK(admission.try_acquire(8));

    NativeAsyncMailboxAdmission::Handle waiter;
    auto ticket = reserve(f, 21);
    CHECK(admission.register_waiter(8, f.scheduler, ticket, &waiter)
          == Registration::Waiting);
    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.target));

    admission.release_acquired(8);
    CHECK(admission.used_frames() == 0);
    CHECK(admission.used_bytes() == 0);
    CHECK(admission.pending_waiters() == 0);
    CHECK(!admission.transferred(waiter));
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == 0);
    CHECK(f.failed.load() == 1);
    CHECK(!f.errors.load());

    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.producer));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.target));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.producer));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}

static void cancelled_slot_reuse_is_generation_fenced() {
    Fixture f;
    setup(f);
    NativeAsyncMailboxAdmission admission(1, 8, 1, 8);
    CHECK(admission.try_acquire(8));

    NativeAsyncMailboxAdmission::Handle old_handle, new_handle;
    auto old_ticket = reserve(f, 31);
    CHECK(admission.register_waiter(
        8, f.scheduler, old_ticket, &old_handle) == Registration::Waiting);
    CHECK(admission.cancel(old_handle));

    auto new_ticket = reserve(f, 32);
    CHECK(admission.register_waiter(
        8, f.scheduler, new_ticket, &new_handle) == Registration::Waiting);
    CHECK(old_handle.slot == new_handle.slot);
    CHECK(old_handle.generation != new_handle.generation);
    CHECK(!admission.cancel(old_handle));

    admission.release_acquired(8);
    CHECK(admission.transferred(new_handle));
    CHECK(admission.commit(new_handle) == ORES_NATIVE_WAKE_PUBLISHED);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == 1);
    admission.release_acquired(8);
    finish(f);
}

static void cancel_and_credit_release_have_exactly_one_winner() {
    Fixture f;
    setup(f);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);
    CHECK(admission.try_acquire(8));

    int transfer_wins = 0;
    int cancel_wins = 0;
    for (std::uint64_t trial = 0; trial < 200; ++trial) {
        NativeAsyncMailboxAdmission::Handle handle;
        auto ticket = reserve(f, 1000 + trial);
        CHECK(admission.register_waiter(
            8, f.scheduler, ticket, &handle) == Registration::Waiting);

        std::atomic<int> go{0};
        bool cancelled = false;
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            cancelled = admission.cancel(handle);
        });
        std::thread releaser([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            admission.release_acquired(8);
        });
        go.store(1, std::memory_order_release);
        canceller.join();
        releaser.join();

        if (cancelled) {
            ++cancel_wins;
            CHECK(!admission.transferred(handle));
            CHECK(admission.used_frames() == 0);
        } else {
            ++transfer_wins;
            CHECK(admission.transferred(handle));
            CHECK(admission.used_frames() == 1);
            CHECK(admission.commit(handle) == ORES_NATIVE_WAKE_PUBLISHED);
            admission.release_acquired(8);
        }
        CHECK(admission.pending_waiters() == 0);
        CHECK(admission.pending_bytes() == 0);
        if (trial != 199) CHECK(admission.try_acquire(8));
    }

    CHECK(transfer_wins + cancel_wins == 200);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.ready.load() == transfer_wins);
    CHECK(f.failed.load() == 0);
    CHECK(!f.errors.load());
    finish(f);
}

static void seal_fails_waiters_but_not_irrevocable_transfers() {
    Fixture f;
    setup(f);
    NativeAsyncMailboxAdmission admission(1, 8, 2, 16);
    CHECK(admission.try_acquire(8));

    NativeAsyncMailboxAdmission::Handle waiting;
    auto t1 = reserve(f, 41);
    CHECK(admission.register_waiter(
        8, f.scheduler, t1, &waiting) == Registration::Waiting);
    admission.seal();
    CHECK(admission.sealed());
    CHECK(admission.pending_waiters() == 0);
    CHECK(!admission.try_acquire(1));
    CHECK(!admission.cancel(waiting));

    admission.release_acquired(8);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(f.failed.load() == 1);
    CHECK(f.ready.load() == 0);
    finish(f);
}

int main() {
    fifo_transfer_prevents_sync_credit_steal();
    target_stop_fails_reserved_waiter_without_false_admission();
    cancelled_slot_reuse_is_generation_fenced();
    cancel_and_credit_release_have_exactly_one_winner();
    seal_fails_waiters_but_not_irrevocable_transfers();
    return 0;
}
