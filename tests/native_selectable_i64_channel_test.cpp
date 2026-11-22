#include "oreslang/runtime/NativeSelectArbiter.hpp"
#include "oreslang/runtime/NativeSelectableI64Channel.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>

using oreslang::runtime::NativeSelectArbiter;
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

static void pending_buffered_writers_admit_in_subscription_order() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    assert(channel.try_write(0));

    NativeSelectArbiter<1> first_select;
    NativeSelectArbiter<1> second_select;
    auto first = first_select.register_case(0);
    auto second = second_select.register_case(0);
    int first_callbacks = 0;
    int second_callbacks = 0;

    auto first_wait = bind_write(channel, first, 1, first_callbacks);
    first.arm([first_wait] { (void)first_wait.close(); });
    auto second_wait = bind_write(channel, second, 2, second_callbacks);
    second.arm([second_wait] { (void)second_wait.close(); });

    assert(channel.pending_writers() == 2);
    assert(!first_select.winner().has_value());
    assert(!second_select.winner().has_value());

    auto seed = channel.try_read();
    assert(seed.has_value() && *seed == 0);

    assert(first_select.winner().has_value() &&
           *first_select.winner() == 0);
    assert(!second_select.winner().has_value());
    assert(first_callbacks == 1);
    assert(second_callbacks == 1);
    assert(channel.pending_writers() == 1);

    auto first_value = channel.try_read();
    assert(first_value.has_value() && *first_value == 1);

    assert(second_select.winner().has_value() &&
           *second_select.winner() == 0);
    assert(second_callbacks == 2);
    assert(channel.pending_writers() == 0);

    auto second_value = channel.try_read();
    assert(second_value.has_value() && *second_value == 2);
    assert(!channel.try_read().has_value());
}

static void cancelling_older_writer_allows_later_writer_without_ghost_delivery() {
    using Channel = NativeSelectableI64Channel<1, 4>;
    Channel channel;
    assert(channel.try_write(0));

    NativeSelectArbiter<1> older_select;
    NativeSelectArbiter<1> later_select;
    auto older = older_select.register_case(0);
    auto later = later_select.register_case(0);
    int older_callbacks = 0;
    int later_callbacks = 0;

    auto older_wait = bind_write(channel, older, 1, older_callbacks);
    older.arm([older_wait] { (void)older_wait.close(); });
    auto later_wait = bind_write(channel, later, 2, later_callbacks);
    later.arm([later_wait] { (void)later_wait.close(); });

    assert(channel.pending_writers() == 2);
    assert(older_select.cancel());
    assert(channel.pending_writers() == 1);
    assert(older_callbacks == 0);

    auto seed = channel.try_read();
    assert(seed.has_value() && *seed == 0);

    assert(!older_select.winner().has_value());
    assert(later_select.winner().has_value() &&
           *later_select.winner() == 0);
    assert(later_callbacks == 1);
    assert(channel.pending_writers() == 0);

    auto value = channel.try_read();
    assert(value.has_value() && *value == 2);
    assert(!channel.try_read().has_value());
    assert(older_callbacks == 0);
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
    assert(channel.pending_readers() == 0);
    assert(!wait.close());
}


static void close_and_plain_write_share_one_channel_linearization_point() {
    using Channel = NativeSelectableI64Channel<1, 2>;
    for (int round = 0; round < 1024; ++round) {
        Channel channel;
        std::atomic<bool> start{false};
        bool wrote = false;
        bool closed = false;

        std::thread writer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            wrote = channel.try_write(91);
        });
        std::thread closer([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            closed = channel.close();
        });

        start.store(true, std::memory_order_release);
        writer.join();
        closer.join();

        assert(closed);
        assert(channel.closed());
        auto result = channel.try_read_result();
        if (wrote) {
            assert(result.status == Channel::ReadStatus::Value);
            assert(result.value == 91);
            assert(channel.try_read_result().status == Channel::ReadStatus::Eof);
        } else {
            assert(result.status == Channel::ReadStatus::Eof);
        }
    }
}

struct ThrowOnCopyWhenArmed final {
    explicit ThrowOnCopyWhenArmed(bool* armed) : armed(armed) {}
    ThrowOnCopyWhenArmed(const ThrowOnCopyWhenArmed& other)
        : armed(other.armed) {
        if (*armed)
            throw std::runtime_error("notification callback copy failed");
    }
    ThrowOnCopyWhenArmed(ThrowOnCopyWhenArmed&&) noexcept = default;
    void operator()(
        const NativeSelectableI64Channel<1, 2>::Subscription&) const {}
    bool* armed;
};

static void notification_copy_failure_is_commit_atomic() {
    using Channel = NativeSelectableI64Channel<1, 2>;
    Channel channel;
    bool throw_on_copy = false;
    ThrowOnCopyWhenArmed callback(&throw_on_copy);
    auto wait = channel.subscribe_readable(callback);

    assert(channel.pending_readers() == 1);
    assert(channel.size() == 0);

    throw_on_copy = true;
    bool observed = false;
    try {
        (void)channel.try_write(44);
    } catch (const std::runtime_error&) {
        observed = true;
    }

    assert(observed);
    assert(channel.size() == 0);
    assert(channel.pending_readers() == 1);
    assert(wait.active());
    assert(!channel.has_notification_failure());

    throw_on_copy = false;
    assert(channel.try_write(44));
    assert(channel.size() == 1);
    assert(wait.close());
}

static void close_notification_copy_failure_does_not_close_channel() {
    using Channel = NativeSelectableI64Channel<1, 2>;
    Channel channel;
    bool throw_on_copy = false;
    ThrowOnCopyWhenArmed callback(&throw_on_copy);
    auto read_wait = channel.subscribe_readable(callback);

    assert(!channel.closed());
    assert(channel.pending_readers() == 1);

    throw_on_copy = true;
    bool observed = false;
    try {
        (void)channel.close();
    } catch (const std::runtime_error&) {
        observed = true;
    }

    assert(observed);
    assert(!channel.closed());
    assert(channel.pending_readers() == 1);
    assert(read_wait.active());

    throw_on_copy = false;
    assert(channel.close());
    assert(channel.closed());
    assert(channel.pending_readers() == 0);
    assert(!read_wait.close());
}

static void terminal_close_observer_is_one_shot() {
    using Channel = NativeSelectableI64Channel<1, 2>;

    {
        Channel channel;
        int callbacks = 0;
        auto wait = channel.subscribe_readable(
            [&](const Channel::Subscription&) {
                ++callbacks;
                assert(channel.closed());
            });
        assert(channel.pending_readers() == 1);
        assert(channel.close());
        assert(callbacks == 1);
        assert(channel.pending_readers() == 0);
        assert(!wait.close());
    }

    {
        Channel channel;
        assert(channel.close());
        int callbacks = 0;
        auto wait = channel.subscribe_writable(
            [&](const Channel::Subscription&) {
                ++callbacks;
                assert(channel.closed());
            });
        assert(callbacks == 1);
        assert(channel.pending_writers() == 0);
        assert(!wait.close());
    }
}

static void readiness_callback_may_reenter_channel_after_unlock() {
    using Channel = NativeSelectableI64Channel<1, 2>;
    Channel channel;
    int callbacks = 0;
    auto wait = channel.subscribe_readable(
        [&](const Channel::Subscription& subscription) {
            ++callbacks;
            assert(channel.size() == 1);
            assert(!channel.closed());
            assert(subscription.active());
        });

    assert(channel.try_write(12));
    assert(callbacks == 1);
    assert(wait.close());
    assert(channel.pending_readers() == 0);
}

int main() {
    first_ready_read_wins_and_loser_does_not_consume();
    losing_write_never_publishes();
    delayed_readiness_wakes_one_select_case();
    freeing_capacity_wakes_pending_write_case();
    pending_buffered_writers_admit_in_subscription_order();
    cancelling_older_writer_allows_later_writer_without_ghost_delivery();
    cancellation_detaches_channel_waiter();
    stale_post_commit_notification_cannot_consume_for_loser();
    close_wakes_pending_read_with_eof();
    close_drains_buffer_before_eof();
    close_wakes_pending_write_as_closed_without_publication();
    immediate_callback_failure_does_not_leak_waiter();
    asynchronous_notification_failure_is_recorded_after_commit();
    close_and_plain_write_share_one_channel_linearization_point();
    notification_copy_failure_is_commit_atomic();
    close_notification_copy_failure_does_not_close_channel();
    terminal_close_observer_is_one_shot();
    readiness_callback_may_reenter_channel_after_unlock();
    return 0;
}
