#include "oreslang/runtime/NativeActorGenerationFence.hpp"
#include "oreslang/runtime/NativeSelectArbiter.hpp"
#include "oreslang/runtime/NativeSelectableFutureI64.hpp"
#include "oreslang/runtime/NativeSelectableI64Channel.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>

using oreslang::runtime::NativeActorGenerationFence;
using oreslang::runtime::NativeSelectArbiter;
using oreslang::runtime::NativeSelectableFutureI64;
using oreslang::runtime::NativeSelectableI64Channel;

template <class Channel, class Registration>
static auto bind_read(Channel& channel,
                      Registration& registration,
                      std::int64_t& result,
                      int& callbacks) {
    return channel.subscribe_readable(
        [&](const typename Channel::Subscription& subscription) {
            ++callbacks;
            typename Channel::ReadResult outcome;
            typename Channel::NotificationBatch notifications;
            const bool won = registration.try_win_if([&] {
                return channel.try_commit_read(
                    subscription, outcome, notifications);
            });
            notifications.dispatch();
            if (won && outcome.status == Channel::ReadStatus::Value)
                result = outcome.value;
        });
}

template <class Channel, class Registration>
static auto bind_write(Channel& channel,
                       Registration& registration,
                       std::int64_t value,
                       int& callbacks) {
    return channel.subscribe_writable(
        [&channel, &registration, value, &callbacks](
            const typename Channel::Subscription& subscription) {
            ++callbacks;
            auto outcome = Channel::WriteStatus::WouldBlock;
            typename Channel::NotificationBatch notifications;
            (void)registration.try_win_if([&] {
                return channel.try_commit_write(
                    subscription, value, outcome, notifications);
            });
            notifications.dispatch();
        });
}

template <class Channel, class Registration>
static auto bind_read_result(Channel& channel,
                             Registration& registration,
                             typename Channel::ReadResult& result,
                             int& callbacks) {
    return channel.subscribe_readable(
        [&](const typename Channel::Subscription& subscription) {
            ++callbacks;
            typename Channel::NotificationBatch notifications;
            (void)registration.try_win_if([&] {
                return channel.try_commit_read(
                    subscription, result, notifications);
            });
            notifications.dispatch();
        });
}

template <class Channel, class Registration>
static auto bind_write_result(Channel& channel,
                              Registration& registration,
                              std::int64_t value,
                              typename Channel::WriteStatus& result,
                              int& callbacks) {
    return channel.subscribe_writable(
        [&channel, &registration, value, &result, &callbacks](
            const typename Channel::Subscription& subscription) {
            ++callbacks;
            typename Channel::NotificationBatch notifications;
            (void)registration.try_win_if([&] {
                return channel.try_commit_write(
                    subscription, value, result, notifications);
            });
            notifications.dispatch();
        });
}

static void first_ready_read_wins_and_loser_does_not_consume() {
    using Channel = NativeSelectableI64Channel<2, 4>;
    Channel first_channel;
    Channel second_channel;
    assert(first_channel.try_write(11));
    assert(second_channel.try_write(22));

    NativeSelectArbiter<2> select;
    auto first = select.register_case(0);
    auto second = select.register_case(1);
    std::int64_t first_value = 0;
    std::int64_t second_value = 0;
    int first_callbacks = 0;
    int second_callbacks = 0;

    auto first_wait = bind_read(
        first_channel, first, first_value, first_callbacks);
    first.arm([first_wait] { (void)first_wait.close(); });

    auto second_wait = bind_read(
        second_channel, second, second_value, second_callbacks);
    second.arm([second_wait] { (void)second_wait.close(); });

    assert(select.winner().has_value() && *select.winner() == 0);
    assert(first_value == 11);
    assert(first_callbacks == 1);
    assert(second_callbacks == 1);
    assert(first_channel.size() == 0);
    assert(second_channel.size() == 1);
    assert(first_channel.pending_readers() == 0);
    assert(second_channel.pending_readers() == 0);

    auto untouched = second_channel.try_read();
    assert(untouched.has_value() && *untouched == 22);
    assert(second_value == 0);
}

static void losing_write_never_publishes() {
    using Channel = NativeSelectableI64Channel<2, 4>;
    Channel first_channel;
    Channel second_channel;

    NativeSelectArbiter<2> select;
    auto first = select.register_case(0);
    auto second = select.register_case(1);
    int first_callbacks = 0;
    int second_callbacks = 0;

    auto first_wait = bind_write(
        first_channel, first, 31, first_callbacks);
    first.arm([first_wait] { (void)first_wait.close(); });

    auto second_wait = bind_write(
        second_channel, second, 32, second_callbacks);
    second.arm([second_wait] { (void)second_wait.close(); });

    assert(select.winner().has_value() && *select.winner() == 0);
    assert(first_callbacks == 1);
    assert(second_callbacks == 1);
    assert(first_channel.size() == 1);
    assert(second_channel.size() == 0);
    assert(first_channel.pending_writers() == 0);
    assert(second_channel.pending_writers() == 0);

    auto committed = first_channel.try_read();
    assert(committed.has_value() && *committed == 31);
    assert(!second_channel.try_read().has_value());
}

static void delayed_readiness_wakes_one_select_case() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel first_channel;
    Channel second_channel;

    NativeSelectArbiter<2> select;
    auto first = select.register_case(0);
    auto second = select.register_case(1);
    std::int64_t first_value = 0;
    std::int64_t second_value = 0;
    int first_callbacks = 0;
    int second_callbacks = 0;

    auto first_wait = bind_read(
        first_channel, first, first_value, first_callbacks);
    auto second_wait = bind_read(
        second_channel, second, second_value, second_callbacks);
    first.arm([first_wait] { (void)first_wait.close(); });
    second.arm([second_wait] { (void)second_wait.close(); });

    assert(!select.winner().has_value());
    assert(first_channel.pending_readers() == 1);
    assert(second_channel.pending_readers() == 1);

    assert(second_channel.try_write(42));
    assert(select.winner().has_value() && *select.winner() == 1);
    assert(second_value == 42);
    assert(first_value == 0);
    assert(second_callbacks == 1);
    assert(first_channel.pending_readers() == 0);
    assert(second_channel.pending_readers() == 0);

    assert(first_channel.try_write(9));
    assert(first_callbacks == 0);
    auto untouched = first_channel.try_read();
    assert(untouched.has_value() && *untouched == 9);
}

static void freeing_capacity_wakes_pending_write_case() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    assert(channel.try_write(7));

    NativeSelectArbiter<1> select;
    auto write = select.register_case(0);
    int callbacks = 0;
    auto wait = bind_write(channel, write, 8, callbacks);
    write.arm([wait] { (void)wait.close(); });

    assert(!select.winner().has_value());
    assert(channel.pending_writers() == 1);

    auto existing = channel.try_read();
    assert(existing.has_value() && *existing == 7);
    assert(select.winner().has_value() && *select.winner() == 0);
    assert(callbacks == 1);
    assert(channel.pending_writers() == 0);

    auto committed = channel.try_read();
    assert(committed.has_value() && *committed == 8);
}

static void cancellation_detaches_channel_waiter() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    NativeSelectArbiter<1> select;
    auto read = select.register_case(0);
    std::int64_t value = 0;
    int callbacks = 0;

    auto wait = bind_read(channel, read, value, callbacks);
    read.arm([wait] { (void)wait.close(); });
    assert(channel.pending_readers() == 1);

    assert(select.cancel());
    assert(channel.pending_readers() == 0);
    assert(channel.try_write(55));
    assert(callbacks == 0);
    assert(value == 0);
}

static void stale_post_commit_notification_cannot_consume_for_loser() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    NativeSelectArbiter<2> select;
    auto read = select.register_case(0);
    auto write = select.register_case(1);
    std::int64_t read_value = 0;
    int read_callbacks = 0;
    int write_callbacks = 0;

    auto read_wait = bind_read(
        channel, read, read_value, read_callbacks);
    read.arm([read_wait] { (void)read_wait.close(); });

    auto write_wait = bind_write(
        channel, write, 99, write_callbacks);
    write.arm([write_wait] { (void)write_wait.close(); });

    assert(select.winner().has_value() && *select.winner() == 1);
    assert(write_callbacks == 1);
    assert(read_callbacks == 1);
    assert(read_value == 0);
    assert(channel.pending_readers() == 0);
    assert(channel.pending_writers() == 0);

    auto value = channel.try_read();
    assert(value.has_value() && *value == 99);
}

static void close_wakes_pending_read_with_eof() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    NativeSelectArbiter<1> select;
    auto read = select.register_case(0);
    Channel::ReadResult result;
    int callbacks = 0;

    auto wait = bind_read_result(channel, read, result, callbacks);
    read.arm([wait] { (void)wait.close(); });
    assert(!select.winner().has_value());
    assert(channel.close());
    assert(!channel.close());
    assert(channel.closed());
    assert(select.winner().has_value() && *select.winner() == 0);
    assert(callbacks == 1);
    assert(result.status == Channel::ReadStatus::Eof);
    assert(channel.pending_readers() == 0);
    assert(!channel.try_write(1));
    assert(channel.try_read_result().status == Channel::ReadStatus::Eof);
}

static void close_drains_buffer_before_eof() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    assert(channel.try_write(77));
    assert(channel.close());

    auto value = channel.try_read_result();
    assert(value.status == Channel::ReadStatus::Value);
    assert(value.value == 77);
    assert(channel.try_read_result().status == Channel::ReadStatus::Eof);
}

static void close_wakes_pending_write_as_closed_without_publication() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    assert(channel.try_write(7));

    NativeSelectArbiter<1> select;
    auto write = select.register_case(0);
    auto result = Channel::WriteStatus::WouldBlock;
    int callbacks = 0;
    auto wait = bind_write_result(channel, write, 8, result, callbacks);
    write.arm([wait] { (void)wait.close(); });

    assert(!select.winner().has_value());
    assert(channel.close());
    assert(select.winner().has_value() && *select.winner() == 0);
    assert(callbacks == 1);
    assert(result == Channel::WriteStatus::Closed);
    assert(channel.pending_writers() == 0);

    auto existing = channel.try_read_result();
    assert(existing.status == Channel::ReadStatus::Value);
    assert(existing.value == 7);
    assert(channel.try_read_result().status == Channel::ReadStatus::Eof);
}

static void immediate_callback_failure_does_not_leak_waiter() {
    using Channel = NativeSelectableI64Channel<1, 2>;
    Channel channel;
    assert(channel.try_write(1));

    bool observed = false;
    try {
        (void)channel.subscribe_readable(
            [](const Channel::Subscription&) {
                throw std::runtime_error("callback failed");
            });
    } catch (const std::runtime_error&) {
        observed = true;
    }
    assert(observed);
    assert(channel.pending_readers() == 0);
    assert(channel.size() == 1);
}

static void asynchronous_notification_failure_is_recorded_after_commit() {
    using Channel = NativeSelectableI64Channel<1, 2>;
    Channel channel;
    auto wait = channel.subscribe_readable(
        [](const Channel::Subscription&) {
            throw std::runtime_error("async callback failed");
        });

    assert(channel.pending_readers() == 1);
    assert(channel.try_write(5));
    assert(channel.size() == 1);
    assert(channel.has_notification_failure());

    bool observed = false;
    try {
        std::rethrow_exception(channel.notification_failure());
    } catch (const std::runtime_error&) {
        observed = true;
    }
    assert(observed);
    assert(wait.close());
    assert(channel.pending_readers() == 0);
}


static void selectable_future_value_and_cancel_are_terminal_readiness() {
    using Future = NativeSelectableFutureI64<4>;

    Future value_future;
    NativeSelectArbiter<1> value_select;
    auto value_case = value_select.register_case(0);
    Future::Outcome value_outcome;
    int value_callbacks = 0;

    auto value_wait = value_future.subscribe_ready(
        [&](const Future::Subscription& subscription) {
            ++value_callbacks;
            (void)value_case.try_win_if([&] {
                return value_future.try_commit(subscription, value_outcome);
            });
        });
    value_case.arm([value_wait] { (void)value_wait.close(); });

    assert(!value_select.winner().has_value());
    assert(value_future.resolve(42));
    assert(!value_future.resolve(43));
    assert(value_select.winner().has_value() &&
           *value_select.winner() == 0);
    assert(value_callbacks == 1);
    assert(value_outcome.kind == Future::OutcomeKind::Value);
    assert(value_outcome.value == 42);

    Future cancelled_future;
    NativeSelectArbiter<1> cancelled_select;
    auto cancelled_case = cancelled_select.register_case(0);
    Future::Outcome cancelled_outcome;

    auto cancelled_wait = cancelled_future.subscribe_ready(
        [&](const Future::Subscription& subscription) {
            (void)cancelled_case.try_win_if([&] {
                return cancelled_future.try_commit(
                    subscription, cancelled_outcome);
            });
        });
    cancelled_case.arm(
        [cancelled_wait] { (void)cancelled_wait.close(); });

    assert(cancelled_future.cancel());
    assert(cancelled_select.winner().has_value() &&
           *cancelled_select.winner() == 0);
    assert(cancelled_outcome.kind == Future::OutcomeKind::Cancelled);
}

static void stale_actor_generation_cannot_consume_channel_value() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    NativeActorGenerationFence fence;
    const auto lease = fence.capture();

    NativeSelectArbiter<1> select;
    auto read = select.register_case(0);
    Channel::ReadResult result;
    int callbacks = 0;

    auto wait = channel.subscribe_readable(
        [&](const Channel::Subscription& subscription) {
            ++callbacks;
            Channel::NotificationBatch notifications;
            (void)read.try_win_if([&] {
                return lease.try_with_current([&] {
                    return channel.try_commit_read(
                        subscription, result, notifications);
                });
            });
            notifications.dispatch();
        });
    read.arm([wait] { (void)wait.close(); });

    assert(fence.advance_generation() == 2);
    assert(!lease.current());
    assert(channel.try_write(9));
    assert(callbacks == 1);
    assert(!select.winner().has_value());
    assert(result.status == Channel::ReadStatus::WouldBlock);

    const auto untouched = channel.try_read_result();
    assert(untouched.status == Channel::ReadStatus::Value);
    assert(untouched.value == 9);
    assert(select.cancel());
    assert(channel.pending_readers() == 0);
}

static void current_actor_generation_may_commit_selected_read() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    NativeActorGenerationFence fence;
    const auto lease = fence.capture();

    NativeSelectArbiter<1> select;
    auto read = select.register_case(0);
    Channel::ReadResult result;

    auto wait = channel.subscribe_readable(
        [&](const Channel::Subscription& subscription) {
            Channel::NotificationBatch notifications;
            (void)read.try_win_if([&] {
                return lease.try_with_current([&] {
                    return channel.try_commit_read(
                        subscription, result, notifications);
                });
            });
            notifications.dispatch();
        });
    read.arm([wait] { (void)wait.close(); });

    assert(lease.current());
    assert(channel.try_write(11));
    assert(select.winner().has_value() && *select.winner() == 0);
    assert(result.status == Channel::ReadStatus::Value);
    assert(result.value == 11);
}

static void generation_fence_rejects_reentrant_advance() {
    NativeActorGenerationFence fence;
    const auto lease = fence.capture();

    bool observed = false;
    try {
        (void)lease.try_with_current([&] {
            (void)fence.advance_generation();
            return true;
        });
    } catch (const std::logic_error&) {
        observed = true;
    }

    assert(observed);
    assert(fence.current_generation() == 1);
    assert(lease.current());
}

int main() {
    first_ready_read_wins_and_loser_does_not_consume();
    losing_write_never_publishes();
    delayed_readiness_wakes_one_select_case();
    freeing_capacity_wakes_pending_write_case();
    cancellation_detaches_channel_waiter();
    stale_post_commit_notification_cannot_consume_for_loser();
    close_wakes_pending_read_with_eof();
    close_drains_buffer_before_eof();
    close_wakes_pending_write_as_closed_without_publication();
    immediate_callback_failure_does_not_leak_waiter();
    asynchronous_notification_failure_is_recorded_after_commit();
    selectable_future_value_and_cancel_are_terminal_readiness();
    stale_actor_generation_cannot_consume_channel_value();
    current_actor_generation_may_commit_selected_read();
    generation_fence_rejects_reentrant_advance();
    return 0;
}
