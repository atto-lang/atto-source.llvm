#include "oreslang/runtime/NativeSelectContinuationFence.hpp"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG  // Regression assertions must execute in release test builds too.
#endif
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeSelectContinuationFence;

static void claim_holds_generation_until_physical_retirement() {
    NativeSelectContinuationFence<2> fence;
    auto ticket = fence.register_continuation(1);
    auto execution = fence.try_claim(ticket);

    assert(execution.valid());
    assert(execution.generation() == 1);
    assert(!ticket.active());
    assert(fence.pending_tickets() == 0);
    assert(fence.executing() == 1);

    // Logical delivery is not enough to roll the actor generation.
    assert(!fence.advance(1));
    assert(fence.generation() == 1);

    assert(execution.close());
    assert(fence.executing() == 0);
    assert(fence.advance(1));
    assert(fence.generation() == 2);
}

static void stale_and_foreign_tickets_fail_closed() {
    NativeSelectContinuationFence<2> fence;
    auto old = fence.register_continuation(1);
    assert(fence.advance(1));
    assert(!fence.try_claim(old).valid());

    auto current = fence.register_continuation(2);
    NativeSelectContinuationFence<2> foreign;
    assert(!foreign.try_claim(current).valid());

    auto execution = fence.try_claim(current);
    assert(execution.valid());
}

static void physical_capacity_includes_claimed_execution() {
    NativeSelectContinuationFence<1> fence;
    auto ticket = fence.register_continuation(1);
    auto execution = fence.try_claim(ticket);
    assert(execution.valid());

    bool exhausted = false;
    try {
        (void)fence.register_continuation(1);
    } catch (const std::overflow_error&) {
        exhausted = true;
    }
    assert(exhausted);

    assert(execution.close());
    auto replacement = fence.register_continuation(1);
    assert(replacement.active());
    assert(replacement.close());
}

static void close_cancels_pending_but_waits_for_claimed_execution() {
    NativeSelectContinuationFence<2> fence;
    auto claimed_ticket = fence.register_continuation(1);
    auto pending_ticket = fence.register_continuation(1);
    auto execution = fence.try_claim(claimed_ticket);
    assert(execution.valid());

    assert(fence.close());
    assert(fence.closed());
    assert(!pending_ticket.active());
    assert(!fence.retirement_ready());
    assert(!fence.try_claim(pending_ticket).valid());
    assert(!fence.advance(1));

    assert(execution.close());
    assert(fence.retirement_ready());

    bool rejected = false;
    try {
        (void)fence.register_continuation(1);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    assert(rejected);
}

static void cancelled_ticket_cannot_be_claimed_after_slot_reuse() {
    NativeSelectContinuationFence<1> fence;
    auto first = fence.register_continuation(1);
    const auto stale_copy = first;
    assert(first.close());

    auto second = fence.register_continuation(1);
    assert(!fence.try_claim(stale_copy).valid());
    auto execution = fence.try_claim(second);
    assert(execution.valid());
}

static void exhaustion_is_failure_atomic() {
    constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
    NativeSelectContinuationFence<1> fence(limit);
    auto ticket = fence.register_continuation(limit);

    bool rejected = false;
    try {
        (void)fence.advance(limit);
    } catch (const std::overflow_error&) {
        rejected = true;
    }
    assert(rejected);
    assert(fence.generation() == limit);
    assert(ticket.active());

    auto execution = fence.try_claim(ticket);
    assert(execution.valid());
    assert(execution.close());
}

static void cancellation_and_claim_share_one_linearization_point() {
    for (int round = 0; round < 512; ++round) {
        NativeSelectContinuationFence<1> fence;
        auto ticket = fence.register_continuation(1);
        std::atomic<bool> start{false};
        bool claimed = false;
        bool cancelled = false;

        std::thread consumer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            auto execution = fence.try_claim(ticket);
            claimed = execution.valid();
            if (claimed) {
                assert(fence.executing() == 1);
                assert(execution.close());
            }
        });

        std::thread canceller([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            cancelled = ticket.close();
        });

        start.store(true, std::memory_order_release);
        consumer.join();
        canceller.join();

        assert(claimed != cancelled);
        assert(fence.pending_tickets() == 0);
        assert(fence.executing() == 0);
    }
}

static void advance_cannot_pass_claim_completion_race() {
    for (int round = 0; round < 512; ++round) {
        NativeSelectContinuationFence<1> fence;
        auto ticket = fence.register_continuation(1);
        auto execution = fence.try_claim(ticket);
        assert(execution.valid());

        std::atomic<bool> start{false};
        bool advanced = false;

        std::thread advancer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            advanced = fence.advance(1);
        });

        start.store(true, std::memory_order_release);
        advancer.join();

        assert(!advanced);
        assert(fence.generation() == 1);
        assert(execution.close());
        assert(fence.advance(1));
    }
}


static void close_and_claim_race_preserves_physical_retirement() {
    for (int round = 0; round < 512; ++round) {
        NativeSelectContinuationFence<1> fence;
        auto ticket = fence.register_continuation(1);
        std::atomic<bool> start{false};
        std::atomic<bool> claimed{false};
        std::atomic<bool> allow_retire{false};
        bool closed = false;

        std::thread consumer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            auto execution = fence.try_claim(ticket);
            if (!execution.valid()) return;
            claimed.store(true, std::memory_order_release);
            while (!allow_retire.load(std::memory_order_acquire))
                std::this_thread::yield();
            assert(execution.close());
        });

        std::thread shutdown([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            closed = fence.close();
        });

        start.store(true, std::memory_order_release);
        shutdown.join();
        assert(closed);
        if (claimed.load(std::memory_order_acquire))
            assert(!fence.retirement_ready());

        allow_retire.store(true, std::memory_order_release);
        consumer.join();

        assert(fence.closed());
        assert(fence.pending_tickets() == 0);
        assert(fence.executing() == 0);
        assert(fence.retirement_ready());
    }
}

static void register_and_advance_race_never_leaves_stale_delivery() {
    for (int round = 0; round < 512; ++round) {
        NativeSelectContinuationFence<1> fence;
        NativeSelectContinuationFence<1>::Ticket ticket;
        std::atomic<bool> start{false};
        bool registered = false;
        bool rejected = false;
        bool advanced = false;

        std::thread producer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            try {
                ticket = fence.register_continuation(1);
                registered = true;
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });

        std::thread rollover([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            advanced = fence.advance(1);
        });

        start.store(true, std::memory_order_release);
        producer.join();
        rollover.join();

        assert(advanced);
        assert(registered != rejected);
        assert(fence.generation() == 2);
        assert(fence.pending_tickets() == 0);
        assert(!ticket.active());
        assert(!fence.try_claim(ticket).valid());
    }
}


static void execution_raii_retirement_is_exactly_once() {
    NativeSelectContinuationFence<1> fence;
    auto ticket = fence.register_continuation(1);
    {
        auto first = fence.try_claim(ticket);
        assert(first.valid());
        assert(fence.executing() == 1);

        auto moved = std::move(first);
        assert(!first.valid());
        assert(moved.valid());
        assert(fence.executing() == 1);
    }

    assert(fence.executing() == 0);
    assert(fence.advance(1));
    assert(fence.generation() == 2);
}

static void mixed_pending_execution_churn() {
    using Fence = NativeSelectContinuationFence<64>;
    Fence fence;
    std::array<Fence::Ticket, 96> prior{};
    for (int round = 0; round < 256; ++round) {
        std::array<Fence::Ticket, 64> pending{};
        std::array<Fence::Execution, 32> leases{};
        for (auto& ticket : pending)
            ticket = fence.register_continuation(fence.generation());
        for (std::size_t i = 0; i < leases.size(); ++i) {
            leases[i] = fence.try_claim(pending[i]);
            assert(leases[i].valid());
        }
        assert(fence.executing() == 32 && fence.pending_tickets() == 32);
        bool exhausted = false;
        try { (void)fence.register_continuation(fence.generation()); }
        catch (const std::overflow_error&) { exhausted = true; }
        assert(exhausted);
        assert(!fence.advance(fence.generation()));
        for (const auto& stale : prior) {
            assert(!fence.try_claim(stale).valid());
            assert(!stale.close());
        }
        for (std::size_t i = 32; i < pending.size(); ++i)
            assert(pending[i].close());
        for (std::size_t i = 0; i < pending.size(); ++i)
            prior[i] = pending[i];
        for (std::size_t i = 64; i < prior.size(); ++i)
            prior[i] = fence.register_continuation(fence.generation());
        for (auto& lease : leases) {
            assert(lease.close());
            assert(!lease.close());
        }
        assert(fence.executing() == 0 && fence.pending_tickets() == 32);
        assert(fence.advance(fence.generation()));
        assert(fence.pending_tickets() == 0);
    }
    assert(fence.close() && fence.retirement_ready());
}

static void million_generation_reuse() {
    using Fence = NativeSelectContinuationFence<4>;
    Fence fence; Fence::Ticket old;
    for (int i=0;i<1000000;++i) {
        const auto g=fence.generation(); const auto t=fence.register_continuation(g);
        auto lease=fence.try_claim(t);
        assert(lease.valid() && fence.executing()==1);
        assert(!fence.advance(g));
        assert(!fence.try_claim(t).valid() && !t.close());
        assert(!fence.try_claim(old).valid() && !old.close());
        assert(lease.close() && !lease.close());
        assert(fence.advance(g));
        assert(fence.pending_tickets()==0 && fence.executing()==0);
        old=t;
    }
    assert(fence.close() && fence.retirement_ready());
}
class LifecycleTestBarrier {
    std::mutex mutex_; std::condition_variable cv_; unsigned arrived_=0, phase_=0;
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_); const auto phase=phase_;
        if (++arrived_==3) { arrived_=0; ++phase_; cv_.notify_all(); }
        else cv_.wait(lock,[&]{return phase_!=phase;});
    }
};
static void concurrent_claim_cancel_lifecycle_stress() {
    using Fence=NativeSelectContinuationFence<1>;
    constexpr int rounds=100000; Fence fence; Fence::Ticket ticket;
    LifecycleTestBarrier barrier; bool claimed=false,cancelled=false;
    std::thread consumer([&]{ for(int i=0;i<rounds;++i){
        barrier.wait(); auto lease=fence.try_claim(ticket); claimed=lease.valid();
        barrier.wait(); barrier.wait(); if(claimed) assert(lease.close()); barrier.wait();
    }});
    std::thread canceller([&]{ for(int i=0;i<rounds;++i){
        barrier.wait(); cancelled=ticket.close(); barrier.wait(); barrier.wait(); barrier.wait();
    }});
    for(int i=0;i<rounds;++i){
        const auto g=fence.generation(); ticket=fence.register_continuation(g);
        barrier.wait(); barrier.wait();
        assert(claimed!=cancelled && fence.pending_tickets()==0);
        assert(fence.executing()==(claimed?1u:0u));
        assert(!fence.try_claim(ticket).valid() && !ticket.close());
        if(claimed) assert(!fence.advance(g));
        barrier.wait(); barrier.wait();
        assert(fence.executing()==0 && fence.advance(g));
        assert(!fence.try_claim(ticket).valid());
    }
    consumer.join(); canceller.join(); assert(fence.close() && fence.retirement_ready());
}

static void owner_destruction_revokes_pending_while_execution_survives() {
    using Fence = NativeSelectContinuationFence<2>;
    Fence::Ticket pending;
    Fence::Execution running;
    {
        Fence fence;
        auto selected = fence.register_continuation(1);
        pending = fence.register_continuation(1);
        running = fence.try_claim(selected);
        assert(running.valid() && pending.active());
    }
    assert(!pending.active());
    assert(!pending.close());
    assert(running.valid());
    assert(running.close());
    assert(!running.close());
}

int main() {
    owner_destruction_revokes_pending_while_execution_survives();
    million_generation_reuse();
    concurrent_claim_cancel_lifecycle_stress();
    mixed_pending_execution_churn();
    claim_holds_generation_until_physical_retirement();
    stale_and_foreign_tickets_fail_closed();
    physical_capacity_includes_claimed_execution();
    close_cancels_pending_but_waits_for_claimed_execution();
    cancelled_ticket_cannot_be_claimed_after_slot_reuse();
    exhaustion_is_failure_atomic();
    cancellation_and_claim_share_one_linearization_point();
    advance_cannot_pass_claim_completion_race();
    close_and_claim_race_preserves_physical_retirement();
    register_and_advance_race_never_leaves_stale_delivery();
    execution_raii_retirement_is_exactly_once();
    return 0;
}
