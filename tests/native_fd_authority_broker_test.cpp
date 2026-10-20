#include "oreslang/runtime/NativeFdAuthorityBroker.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <type_traits>
using oreslang::runtime::NativeFdAuthorityBroker;
using oreslang::runtime::NativeEpollReactor;
static_assert(!std::is_constructible_v<NativeFdAuthorityBroker::Token,
                                       std::uint64_t, std::uint64_t, std::uint16_t>,
              "Guest-accessible code must not synthesize broker capabilities");
static_assert(!std::is_aggregate_v<NativeFdAuthorityBroker::Token>,
              "Opaque host token must not permit aggregate initialization");
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
        check(done.owner == 31 && done.outcome == Outcome::ready);
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
        check(!broker.try_cancel(*token, 41)); // no rights after revoke
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
        check(done.owner == 51 && done.outcome == Outcome::stopped);
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

    {
        // A trusted dispatcher receives owner identity with the opaque
        // completion; records must never be misrouted between principals.
        NativeFdAuthorityBroker broker(1);
        auto first = broker.mint_eventfd(81);
        auto second = broker.mint_eventfd(82);
        check(first.has_value() && second.has_value());
        check(broker.try_watch(*first, 81));
        check(broker.try_watch(*second, 82));
        check(!broker.signal(*first, 82));
        check(!broker.signal(*second, 81));
        check(broker.signal(*first, 81));
        check(broker.signal(*second, 82));
        bool seen_first = false, seen_second = false;
        for (unsigned i = 0; i < 2; ++i) {
            auto completed = wait_done(broker);
            check(completed.outcome == Outcome::ready);
            if (completed.owner == 81) {
                check(!seen_first);
                seen_first = true;
            } else {
                check(completed.owner == 82 && !seen_second);
                seen_second = true;
            }
        }
        check(seen_first && seen_second);
        check(!broker.try_pop());
        check(broker.revoke(*first, 81));
        check(broker.revoke(*second, 82));
        check(!broker.try_cancel(*first, 81));
        check(!broker.try_cancel(*second, 82));
        check(broker.confirm_reclaimed(*first, 81));
        check(broker.confirm_reclaimed(*second, 82));
    }
    {
        // Saturation denies admission without stealing a live capability.
        // Reclamation rotates the broker token so stale references are dead.
        NativeFdAuthorityBroker broker(1);
        std::array<NativeFdAuthorityBroker::Token,
                   NativeFdAuthorityBroker::capacity> tokens{};
        for (auto& token : tokens) {
            auto value = broker.mint_eventfd(91);
            check(value.has_value());
            token = *value;
        }
        check(!broker.mint_eventfd(91));
        check(broker.revoke(tokens[0], 91));
        check(!broker.mint_eventfd(92));
        check(broker.confirm_reclaimed(tokens[0], 91));
        auto replacement = broker.mint_eventfd(92);
        check(replacement.has_value());
        check(!broker.signal(tokens[0], 91));
        check(!broker.try_watch(tokens[0], 91));
        check(!broker.revoke(tokens[0], 92));
        check(broker.revoke(*replacement, 92));
        check(broker.confirm_reclaimed(*replacement, 92));
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            check(broker.revoke(tokens[i], 91));
            check(broker.confirm_reclaimed(tokens[i], 91));
        }
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
