#include "oreslang/runtime/NativeDemandI64Port.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::NativeDemandI64Port;

static void credits_and_rejections_do_not_consume_producer_value() {
    NativeDemandI64Port<1, 2, 16, 4> port;
    const std::int64_t original = 42;
    assert(port.try_offer(original) == decltype(port)::Offer::NoSubscriber);
    assert(port.subscribe());
    assert(!port.subscribe());
    assert(port.try_offer(original) == decltype(port)::Offer::NoDemand);
    port.request(2);
    assert(port.try_offer(original) == decltype(port)::Offer::Accepted);
    assert(port.try_offer(99) == decltype(port)::Offer::Full);
    assert(port.outstanding_demand() == 1);
    assert(original == 42);
    const auto item = port.try_next();
    assert(item.status == decltype(port)::PollStatus::Item && item.value == original);
    assert(port.try_offer(99) == decltype(port)::Offer::Accepted);
    assert(port.retained_bytes() == 16);
    assert(port.ack(item.ticket));
    assert(!port.ack(item.ticket));
    auto second = port.try_next();
    assert(second.status == decltype(port)::PollStatus::Item && second.value == 99);
    assert(port.ack(second.ticket));
    assert(port.retained_bytes() == 0);
}

static void byte_quota_includes_checked_out_loans() {
    NativeDemandI64Port<2, 2, 16, 8> port;
    assert(port.subscribe());
    port.request(4);
    assert(port.try_offer(1) == decltype(port)::Offer::Accepted);
    assert(port.try_offer(2) == decltype(port)::Offer::Accepted);
    auto first = port.try_next();
    assert(first.status == decltype(port)::PollStatus::Item);
    assert(port.try_offer(3) == decltype(port)::Offer::ByteBudget);
    assert(port.ack(first.ticket));
    assert(port.try_offer(3) == decltype(port)::Offer::Accepted);
    auto second = port.try_next();
    auto third = port.try_next();
    assert(second.value == 2 && third.value == 3);
    assert(port.in_flight() == 2);
    assert(port.ack(second.ticket));
    assert(port.ack(third.ticket));
    assert(port.retained_bytes() == 0);
}

static void cancellation_and_completion_preserve_order_and_exact_ack() {
    NativeDemandI64Port<2, 2, 16, 4> port;
    assert(port.subscribe());
    port.request(2);
    assert(port.try_offer(10) == decltype(port)::Offer::Accepted);
    assert(port.try_offer(20) == decltype(port)::Offer::Accepted);
    assert(port.complete());
    assert(!port.complete());
    assert(port.try_offer(30) == decltype(port)::Offer::Closed);
    auto first = port.try_next();
    auto second = port.try_next();
    assert(first.value == 10 && second.value == 20);
    assert(port.try_next().status == decltype(port)::PollStatus::NotReady);
    assert(port.ack(first.ticket));
    assert(port.ack(second.ticket));
    assert(port.try_next().status == decltype(port)::PollStatus::Complete);
    assert(port.retained_bytes() == 0);

    NativeDemandI64Port<2, 2, 16, 4> canceled;
    assert(canceled.subscribe());
    canceled.request(2);
    assert(canceled.try_offer(1) == decltype(canceled)::Offer::Accepted);
    assert(canceled.try_offer(2) == decltype(canceled)::Offer::Accepted);
    auto loan = canceled.try_next();
    assert(canceled.cancel());
    assert(!canceled.cancel());
    assert(canceled.queued() == 0);
    assert(canceled.in_flight() == 1);
    assert(canceled.retained_bytes() == 8);
    assert(canceled.try_next().status == decltype(canceled)::PollStatus::Cancelled);
    assert(canceled.ack(loan.ticket));
    assert(!canceled.ack(loan.ticket));
    assert(!canceled.ack(loan.ticket + 100));
    assert(canceled.retained_bytes() == 0);
}

static void requests_are_finite_and_invalid_input_fails_without_mutation() {
    NativeDemandI64Port<1, 1, 8, 4> port;
    assert(port.subscribe());
    bool rejected = false;
    try { port.request(0); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { port.request(5); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    assert(port.outstanding_demand() == 0);
    port.request(4);
    rejected = false;
    try { port.request(1); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    assert(port.outstanding_demand() == 4);
}

static void producer_cancel_race_never_exceeds_byte_capacity() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        NativeDemandI64Port<4, 2, 48, 128> port;
        assert(port.subscribe());
        port.request(80);
        std::atomic<bool> go{false};
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < 80; ++i) (void)port.try_offer(i);
        });
        std::thread canceller([&] {
            go.store(true, std::memory_order_release);
            (void)port.cancel();
        });
        producer.join();
        canceller.join();
        assert(port.queued() == 0);
        assert(port.retained_bytes() == 0);
        assert(port.try_offer(999) == decltype(port)::Offer::Closed);
    }
}


static void completed_terminal_cannot_be_rewritten_as_cancellation() {
    NativeDemandI64Port<2, 2, 16, 4> port;
    assert(port.subscribe());
    port.request(2);
    assert(port.try_offer(101) == decltype(port)::Offer::Accepted);
    assert(port.try_offer(202) == decltype(port)::Offer::Accepted);
    assert(port.complete());
    assert(!port.cancel()); // terminal COMPLETE has already committed
    assert(port.queued() == 2);
    auto one = port.try_next();
    auto two = port.try_next();
    assert(one.status == decltype(port)::PollStatus::Item && one.value == 101);
    assert(two.status == decltype(port)::PollStatus::Item && two.value == 202);
    assert(port.ack(one.ticket));
    assert(port.ack(two.ticket));
    assert(port.try_next().status == decltype(port)::PollStatus::Complete);
    assert(port.retained_bytes() == 0);
}

static void terminal_race_linearizes_exactly_once() {
    for (int attempt = 0; attempt < 128; ++attempt) {
        NativeDemandI64Port<2, 2, 16, 4> port;
        assert(port.subscribe());
        port.request(1);
        assert(port.try_offer(7) == decltype(port)::Offer::Accepted);
        std::atomic<bool> start{false};
        bool complete_won = false;
        bool cancel_won = false;
        std::thread completion([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            complete_won = port.complete();
        });
        std::thread cancellation([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            cancel_won = port.cancel();
        });
        start.store(true, std::memory_order_release);
        completion.join();
        cancellation.join();
        assert(complete_won != cancel_won);
        if (complete_won) {
            auto item = port.try_next();
            assert(item.status == decltype(port)::PollStatus::Item);
            assert(item.value == 7 && port.ack(item.ticket));
            assert(port.try_next().status == decltype(port)::PollStatus::Complete);
        } else {
            assert(port.try_next().status == decltype(port)::PollStatus::Cancelled);
        }
        assert(port.queued() == 0);
        assert(port.in_flight() == 0);
        assert(port.retained_bytes() == 0);
    }
}

int main() {
    credits_and_rejections_do_not_consume_producer_value();
    byte_quota_includes_checked_out_loans();
    cancellation_and_completion_preserve_order_and_exact_ack();
    requests_are_finite_and_invalid_input_fails_without_mutation();
    producer_cancel_race_never_exceeds_byte_capacity();
    completed_terminal_cannot_be_rewritten_as_cancellation();
    terminal_race_linearizes_exactly_once();
}
