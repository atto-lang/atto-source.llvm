#include "oreslang/runtime/AsyncIoOperationLifecycle.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <thread>

using namespace oreslang::runtime;

static void check(bool condition) {
    if (!condition) std::abort();
}

int main() {
    const AsyncIoTicket ticket{7, 11, 13};
    AsyncIoOperationLifecycle op(ticket);

    check(!op.ready_to_reclaim());
    check(!op.mark_registered({7, 10, 13}));
    check(op.mark_registered(ticket));
    check(!op.mark_registered(ticket));
    check(op.begin_callback(ticket));

    // Future settlement is not physical retirement.
    check(op.cancel(ticket));
    check(op.terminal() == AsyncIoTerminal::cancelled);
    check(!op.finish(ticket, AsyncIoTerminal::success));
    check(!op.retire(ticket));
    check(!op.ready_to_reclaim());

    check(op.end_callback(ticket));
    check(!op.retire(ticket)); // kernel registration still exists
    check(op.mark_unregistered(ticket));
    check(op.retire(ticket));
    check(op.ready_to_reclaim());
    check(!op.retire(ticket));
    check(!op.begin_callback(ticket));

    // Cancellation before readiness prevents a callback from being claimed.
    {
        const AsyncIoTicket t{8, 1, 1};
        AsyncIoOperationLifecycle cancelled(t);
        check(cancelled.mark_registered(t));
        check(cancelled.cancel(t));
        check(!cancelled.begin_callback(t));
        check(cancelled.mark_unregistered(t));
        check(cancelled.retire(t));
        check(cancelled.ready_to_reclaim());
    }

    // Completion/cancel/timeout are one-winner races. Even after a winner, the
    // operation cannot be reclaimed until callback + kernel ownership retire.
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        const AsyncIoTicket t{9, 3, i};
        AsyncIoOperationLifecycle race(t);
        check(race.mark_registered(t));
        check(race.begin_callback(t));

        std::atomic<int> wins{0};
        std::thread completion([&] {
            if (race.finish(t, AsyncIoTerminal::success)) ++wins;
        });
        std::thread cancellation([&] {
            if (race.cancel(t)) ++wins;
        });
        std::thread timeout([&] {
            if (race.timeout(t)) ++wins;
        });
        completion.join();
        cancellation.join();
        timeout.join();

        check(wins.load() == 1);
        check(race.terminal() != AsyncIoTerminal::pending);
        check(!race.ready_to_reclaim());
        check(!race.retire(t));
        check(race.end_callback(t));
        check(race.mark_unregistered(t));
        check(race.retire(t));
        check(race.ready_to_reclaim());
    }

    // Competing admission and retirement must not both succeed before the
    // registration/callback is explicitly released. Each iteration retains
    // the object until both API users have returned, so this tests state
    // arbitration, not the external lifetime protocol.
    for (const bool callback : {false, true}) {
        constexpr unsigned rounds = 20000;
        std::atomic<unsigned> start{0}, done{0};
        AsyncIoOperationLifecycle* current = nullptr;
        AsyncIoTicket current_ticket{};
        bool admitted = false;
        std::thread contender([&] {
            for (unsigned i = 1; i <= rounds; ++i) {
                while (start.load(std::memory_order_acquire) != i)
                    std::this_thread::yield();
                admitted = callback ? current->begin_callback(current_ticket)
                                    : current->mark_registered(current_ticket);
                done.store(i, std::memory_order_release);
            }
        });
        unsigned violations = 0;
        for (unsigned i = 1; i <= rounds; ++i) {
            const AsyncIoTicket t{10, callback ? 2u : 1u, i};
            AsyncIoOperationLifecycle race(t);
            if (callback) check(race.mark_registered(t));
            current = &race;
            current_ticket = t;
            start.store(i, std::memory_order_release);
            if (i % 2 == 0) std::this_thread::yield();
            check(race.cancel(t));
            if (callback) check(race.mark_unregistered(t));
            const bool retired = race.retire(t);
            while (done.load(std::memory_order_acquire) != i)
                std::this_thread::yield();
            if (retired == admitted || (retired && !race.ready_to_reclaim())) {
                ++violations;
            }
            if (admitted) {
                check(callback ? race.end_callback(t) : race.mark_unregistered(t));
            }
            if (!retired) check(race.retire(t));
            check(race.ready_to_reclaim());
            check(!race.mark_registered(t) && !race.begin_callback(t));
        }
        contender.join();
        if (violations != 0) {
            std::fprintf(stderr, "%s/retire violations: %u\n",
                         callback ? "callback" : "register", violations);
            return 1;
        }
    }

    // Invalid serialized terminal values and foreign identities never settle
    // an operation or authorize retirement.
    {
        const AsyncIoTicket t{11, 1, 1};
        AsyncIoOperationLifecycle invalid(t);
        for (unsigned value = 6; value <= 255; ++value)
            check(!invalid.finish(t, static_cast<AsyncIoTerminal>(value)));
        check(!invalid.finish(t, AsyncIoTerminal::pending));
        check(!invalid.cancel({11, 2, 1}));
        check(!invalid.retire(t));
        check(invalid.cancel(t) && invalid.retire(t));
        check(invalid.ready_to_reclaim());
    }
    std::puts("ASYNC_IO_LIFECYCLE_PASS 40000 admission/retirement races + 1000 settlement races");

    return 0;
}
