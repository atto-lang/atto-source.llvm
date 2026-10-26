#include "oreslang/runtime/NativeSelectArbiter.hpp"
#include "oreslang/runtime/NativeSelectableI64Channel.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>

using oreslang::runtime::NativeSelectArbiter;
using oreslang::runtime::NativeSelectableI64Channel;

template <class Channel, class Registration>
static auto bind_read(
        Channel& channel,
        Registration& registration,
        std::int64_t& result,
        int& callbacks,
        typename Channel::ReadOutcome* outcome = nullptr) {
    return channel.subscribe_readable(
        [&](const typename Channel::Subscription& subscription) {
            ++callbacks;
            typename Channel::NotificationBatch notifications;
            typename Channel::ReadResult committed;
            const bool won = registration.try_win_if([&] {
                return channel.try_commit_read(
                    subscription, committed, notifications);
            });
            notifications.dispatch();
            if (!won) return;
            if (outcome) *outcome = committed.outcome;
            if (committed.has_value()) result = committed.value;
        });
}

template <class Channel, class Registration>
static auto bind_write(
        Channel& channel,
        Registration& registration,
        std::int64_t value,
        int& callbacks,
        typename Channel::WriteOutcome* outcome = nullptr) {
    return channel.subscribe_writable(
        [&](const typename Channel::Subscription& subscription) {
            ++callbacks;
            typename Channel::NotificationBatch notifications;
            auto committed = Channel::WriteOutcome::NotReady;
            const bool won = registration.try_win_if([&] {
                return channel.try_commit_write(
                    subscription, value, committed, notifications);
            });
            notifications.dispatch();
            if (won && outcome) *outcome = committed;
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

    // The write is immediately ready. Its channel commit captures the pending
    // reader notification, then the arbiter revokes that losing reader before
    // the notification batch is dispatched. The stale callback may run but
    // its subscription identity is no longer active and cannot consume.
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

static void close_wakes_pending_read_as_eof() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    NativeSelectArbiter<1> select;
    auto read = select.register_case(0);
    std::int64_t value = 123;
    int callbacks = 0;
    auto outcome = Channel::ReadOutcome::NotReady;

    auto wait = bind_read(channel, read, value, callbacks, &outcome);
    read.arm([wait] { (void)wait.close(); });
    assert(channel.pending_readers() == 1);
    assert(!select.winner().has_value());

    assert(channel.close());
    assert(channel.is_closed());
    assert(select.winner().has_value() && *select.winner() == 0);
    assert(callbacks == 1);
    assert(outcome == Channel::ReadOutcome::Eof);
    assert(value == 123);
    assert(channel.pending_readers() == 0);
    assert(!channel.close());
}

static void closed_channel_drains_buffer_before_eof() {
    using Channel = NativeSelectableI64Channel<2, 4>;
    Channel channel;
    assert(channel.try_write(41));
    assert(channel.close());
    assert(!channel.try_write(42));

    NativeSelectArbiter<1> first_select;
    auto first = first_select.register_case(0);
    std::int64_t first_value = 0;
    int first_callbacks = 0;
    auto first_outcome = Channel::ReadOutcome::NotReady;
    auto first_wait = bind_read(
        channel, first, first_value, first_callbacks, &first_outcome);
    first.arm([first_wait] { (void)first_wait.close(); });

    assert(first_select.winner().has_value());
    assert(first_outcome == Channel::ReadOutcome::Value);
    assert(first_value == 41);
    assert(channel.size() == 0);

    NativeSelectArbiter<1> eof_select;
    auto eof = eof_select.register_case(0);
    std::int64_t eof_value = 77;
    int eof_callbacks = 0;
    auto eof_outcome = Channel::ReadOutcome::NotReady;
    auto eof_wait = bind_read(
        channel, eof, eof_value, eof_callbacks, &eof_outcome);
    eof.arm([eof_wait] { (void)eof_wait.close(); });

    assert(eof_select.winner().has_value());
    assert(eof_outcome == Channel::ReadOutcome::Eof);
    assert(eof_value == 77);
    assert(eof_callbacks == 1);
}

static void close_wakes_pending_write_as_closed_without_publication() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    assert(channel.try_write(7));

    NativeSelectArbiter<1> select;
    auto write = select.register_case(0);
    int callbacks = 0;
    auto outcome = Channel::WriteOutcome::NotReady;
    auto wait = bind_write(channel, write, 8, callbacks, &outcome);
    write.arm([wait] { (void)wait.close(); });

    assert(!select.winner().has_value());
    assert(channel.pending_writers() == 1);
    assert(channel.close());

    assert(select.winner().has_value() && *select.winner() == 0);
    assert(callbacks == 1);
    assert(outcome == Channel::WriteOutcome::Closed);
    assert(channel.pending_writers() == 0);
    assert(channel.size() == 1);
    assert(!channel.try_write(9));

    auto existing = channel.try_read();
    assert(existing.has_value() && *existing == 7);
    assert(channel.size() == 0);
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

int main() {
    first_ready_read_wins_and_loser_does_not_consume();
    losing_write_never_publishes();
    delayed_readiness_wakes_one_select_case();
    freeing_capacity_wakes_pending_write_case();
    cancellation_detaches_channel_waiter();
    stale_post_commit_notification_cannot_consume_for_loser();
    close_wakes_pending_read_as_eof();
    closed_channel_drains_buffer_before_eof();
    close_wakes_pending_write_as_closed_without_publication();
    immediate_callback_failure_does_not_leak_waiter();
    asynchronous_notification_failure_is_recorded_after_commit();
    return 0;
}
