#include "oreslang/runtime/NativeSelectContinuationFence.hpp"
#include <cassert>
#include <stdexcept>

using oreslang::runtime::NativeSelectContinuationFence;

int main() {
    {
        NativeSelectContinuationFence<4> fence;
        auto ticket = fence.register_continuation(1);
        assert(fence.try_consume(ticket));
        assert(!fence.try_consume(ticket));
        assert(fence.pending_tickets() == 0);
    }
    {
        NativeSelectContinuationFence<4> fence;
        auto old = fence.register_continuation(1);
        assert(fence.advance(1));
        assert(fence.generation() == 2);
        assert(!fence.try_consume(old));
        auto current = fence.register_continuation(2);
        assert(fence.try_consume(current));
    }
    {
        NativeSelectContinuationFence<1> fence;
        auto old = fence.register_continuation(1);
        assert(fence.advance(1));
        auto current = fence.register_continuation(2);
        assert(!old.active());
        assert(current.active());
        assert(!fence.try_consume(old));
        assert(fence.try_consume(current));
    }
    {
        NativeSelectContinuationFence<2> fence;
        auto ticket = fence.register_continuation(1);
        assert(ticket.close());
        assert(!ticket.close());
        assert(!fence.try_consume(ticket));
    }
    {
        NativeSelectContinuationFence<2> fence(7);
        auto ticket = fence.register_continuation(7);
        assert(!fence.advance(6));
        assert(fence.generation() == 7);
        assert(fence.try_consume(ticket));
    }
    {
        NativeSelectContinuationFence<2> fence(3);
        bool rejected = false;
        try {
            (void)fence.register_continuation(2);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        assert(rejected);
        assert(fence.pending_tickets() == 0);
    }
    {
        NativeSelectContinuationFence<2> first;
        NativeSelectContinuationFence<2> second;
        auto ticket = first.register_continuation(1);
        assert(!second.try_consume(ticket));
        assert(ticket.close());
    }

    // Capacity failure must preserve every live reservation.
    {
        NativeSelectContinuationFence<2> fence;
        auto first = fence.register_continuation(1);
        auto second = fence.register_continuation(1);
        bool rejected = false;
        try { (void)fence.register_continuation(1); }
        catch (const std::overflow_error&) { rejected = true; }
        assert(rejected && fence.pending_tickets() == 2);
        assert(first.active() && second.active());
        assert(first.close());
        auto replacement = fence.register_continuation(1);
        assert(!first.close());
        assert(fence.pending_tickets() == 2);
        assert(fence.try_consume(second));
        assert(fence.try_consume(replacement));
        assert(fence.pending_tickets() == 0);
    }
    // A full fence must regain all capacity on generation advance.
    {
        NativeSelectContinuationFence<2> fence;
        auto first = fence.register_continuation(1);
        auto second = fence.register_continuation(1);
        assert(fence.advance(1));
        assert(fence.pending_tickets() == 0);
        auto next_first = fence.register_continuation(2);
        auto next_second = fence.register_continuation(2);
        assert(!first.active() && !second.active());
        assert(!first.close() && !second.close());
        assert(!fence.try_consume(first) && !fence.try_consume(second));
        assert(fence.pending_tickets() == 2);
        assert(fence.try_consume(next_first));
        assert(fence.try_consume(next_second));
    }
    // Exhaustion is failure-atomic, including still-deliverable tickets.
    {
        constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
        NativeSelectContinuationFence<1> fence(limit);
        auto ticket = fence.register_continuation(limit);
        assert(!fence.advance(limit - 1));
        bool rejected = false;
        try { (void)fence.advance(limit); }
        catch (const std::overflow_error&) { rejected = true; }
        assert(rejected);
        assert(fence.generation() == limit);
        assert(fence.pending_tickets() == 1 && ticket.active());
        assert(fence.try_consume(ticket));
    }
    {
        bool rejected = false;
        try { NativeSelectContinuationFence<1> invalid(0); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    // Empty and expired tickets cannot disturb another live reservation.
    {
        using Fence = NativeSelectContinuationFence<1>;
        Fence::Ticket empty;
        Fence::Ticket expired;
        {
            Fence owner;
            expired = owner.register_continuation(1);
            assert(expired.active());
        }
        assert(!expired.active() && !expired.close());
        assert(!empty.active() && !empty.close());
        Fence current;
        auto ticket = current.register_continuation(1);
        assert(!current.try_consume(empty));
        assert(!current.try_consume(expired));
        assert(current.pending_tickets() == 1);
        assert(current.try_consume(ticket));
    }
    // A copied handle shares one reservation; closing one invalidates both.
    {
        NativeSelectContinuationFence<1> fence;
        auto first = fence.register_continuation(1);
        const auto copy = first;
        assert(copy.close());
        auto next = fence.register_continuation(1);
        assert(!first.close() && !copy.close());
        assert(!fence.try_consume(copy));
        assert(fence.try_consume(next));
    }
    return 0;
}
