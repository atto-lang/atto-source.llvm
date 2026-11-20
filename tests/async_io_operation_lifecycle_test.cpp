#include "oreslang/runtime/AsyncIoOperationLifecycle.hpp"

#include <atomic>
#include <cassert>
#include <thread>

using namespace oreslang::runtime;

int main() {
    const AsyncIoTicket ticket{7, 11, 13};
    AsyncIoOperationLifecycle op(ticket);

    assert(!op.ready_to_reclaim());
    assert(!op.mark_registered({7, 10, 13}));
    assert(op.mark_registered(ticket));
    assert(!op.mark_registered(ticket));
    assert(op.begin_callback(ticket));

    // Future settlement is not physical retirement.
    assert(op.cancel(ticket));
    assert(op.terminal() == AsyncIoTerminal::cancelled);
    assert(!op.finish(ticket, AsyncIoTerminal::success));
    assert(!op.retire(ticket));
    assert(!op.ready_to_reclaim());

    assert(op.end_callback(ticket));
    assert(!op.retire(ticket)); // kernel registration still exists
    assert(op.mark_unregistered(ticket));
    assert(op.retire(ticket));
    assert(op.ready_to_reclaim());
    assert(!op.retire(ticket));
    assert(!op.begin_callback(ticket));

    // Cancellation before readiness prevents a callback from being claimed.
    {
        const AsyncIoTicket t{8, 1, 1};
        AsyncIoOperationLifecycle cancelled(t);
        assert(cancelled.mark_registered(t));
        assert(cancelled.cancel(t));
        assert(!cancelled.begin_callback(t));
        assert(cancelled.mark_unregistered(t));
        assert(cancelled.retire(t));
        assert(cancelled.ready_to_reclaim());
    }

    // Completion/cancel/timeout are one-winner races. Even after a winner, the
    // operation cannot be reclaimed until callback + kernel ownership retire.
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        const AsyncIoTicket t{9, 3, i};
        AsyncIoOperationLifecycle race(t);
        assert(race.mark_registered(t));
        assert(race.begin_callback(t));

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

        assert(wins.load() == 1);
        assert(race.terminal() != AsyncIoTerminal::pending);
        assert(!race.ready_to_reclaim());
        assert(!race.retire(t));
        assert(race.end_callback(t));
        assert(race.mark_unregistered(t));
        assert(race.retire(t));
        assert(race.ready_to_reclaim());
    }

    return 0;
}
