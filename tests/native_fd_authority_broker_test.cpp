#include "oreslang/runtime/NativeFdAuthorityBroker.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
using oreslang::runtime::NativeFdAuthorityBroker;
using oreslang::runtime::NativeEpollReactor;
static void check(bool ok) { if (!ok) std::abort(); }

static NativeFdAuthorityBroker::Completion wait_done(
    NativeFdAuthorityBroker& broker) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        auto completed = broker.try_pop();
        if (completed) return *completed;
        std::this_thread::yield();
    }
    std::abort();
}

int main() {
    using Outcome = NativeEpollReactor::Outcome;
    {
        NativeFdAuthorityBroker broker(1), foreign(1);
        check(!broker.mint_eventfd(0));
        auto token = broker.mint_eventfd(31);
        check(token.has_value());
        check(!foreign.try_watch(*token, 31));
        check(!broker.try_watch(*token, 32));
        check(!broker.signal(*token, 32));
        check(!broker.confirm_reclaimed(*token, 31));
        check(broker.started_threads() == 0);
        check(broker.try_watch(*token, 31));
        check(broker.started_threads() == 1);
        check(!broker.try_watch(*token, 31));
        check(!broker.confirm_reclaimed(*token, 31));
        check(broker.signal(*token, 31));
        auto done = wait_done(broker);
        check(done.outcome == Outcome::ready);
        check(!broker.try_pop());
        check(broker.revoke(*token, 31));
        check(!broker.try_watch(*token, 31));
        check(!broker.signal(*token, 31));
        check(!broker.confirm_reclaimed(*token, 32));
        check(broker.confirm_reclaimed(*token, 31));
        check(!broker.confirm_reclaimed(*token, 31));
        check(!broker.try_watch(*token, 31));
        auto replacement = broker.mint_eventfd(31);
        check(replacement.has_value());
        check(!broker.try_watch(*token, 31));
        check(broker.revoke(*replacement, 31));
        check(broker.confirm_reclaimed(*replacement, 31));
        broker.stop();
        check(!broker.mint_eventfd(31));
    }
    {
        NativeFdAuthorityBroker broker(1);
        auto token = broker.mint_eventfd(41);
        check(token.has_value());
        check(broker.try_watch(*token, 41));
        check(broker.revoke(*token, 41));
        check(!broker.confirm_reclaimed(*token, 41));
        check(!broker.signal(*token, 41));
        auto done = wait_done(broker);
        check(done.outcome == Outcome::cancelled ||
              done.outcome == Outcome::ready);
        check(broker.confirm_reclaimed(*token, 41));
        check(!broker.confirm_reclaimed(*token, 41));
    }
    {
        // Host shutdown must return a STOPPED terminal, never a successful
        // cancellation of a watch after the retirement fence was set.
        NativeFdAuthorityBroker broker(1);
        auto token = broker.mint_eventfd(51);
        check(token.has_value());
        check(broker.try_watch(*token, 51));
        broker.stop();
        check(!broker.try_cancel(*token, 51));
        check(broker.revoke(*token, 51));
        check(!broker.confirm_reclaimed(*token, 51));
        auto done = wait_done(broker);
        check(done.outcome == Outcome::stopped);
        check(broker.confirm_reclaimed(*token, 51));
    }
    {
        // Invalid epoll admission cannot strand the logical FD watch.
        NativeFdAuthorityBroker broker(1);
        auto token = broker.mint_eventfd(61);
        check(token.has_value());
        check(!broker.try_watch(*token, 61, 0));
        check(broker.try_watch(*token, 61));
        check(broker.revoke(*token, 61));
        (void)wait_done(broker);
        check(broker.confirm_reclaimed(*token, 61));
    }
    for (unsigned round = 0; round < 128; ++round) {
        NativeFdAuthorityBroker broker(1);
        auto token = broker.mint_eventfd(71);
        check(token.has_value());
        check(broker.try_watch(*token, 71));
        std::atomic<int> barrier{0};
        std::thread revoker([&] {
            barrier.fetch_add(1, std::memory_order_acq_rel);
            while (barrier.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            while (!broker.revoke(*token, 71))
                std::this_thread::yield();
        });
        std::thread notifier([&] {
            barrier.fetch_add(1, std::memory_order_acq_rel);
            while (barrier.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            (void)broker.signal(*token, 71);
        });
        revoker.join(); notifier.join();
        check(!broker.confirm_reclaimed(*token, 71));
        auto done = wait_done(broker);
        check(done.outcome == Outcome::cancelled ||
              done.outcome == Outcome::ready);
        check(broker.confirm_reclaimed(*token, 71));
        check(!broker.try_watch(*token, 71));
    }
    return 0;
}
