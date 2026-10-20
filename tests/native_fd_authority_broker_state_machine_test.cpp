// Real Linux eventfd/epoll lifecycle matrix plus deterministic test-only
// contention injection. These hooks do NOT exist in release builds.
// This is HOST broker coverage, not .ores Future/await or guest FD parity.
#define ORESLANG_NIO_TEST_HOOKS 1
#include "oreslang/runtime/NativeFdAuthorityBroker.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

using oreslang::runtime::NativeFdAuthorityBroker;
using oreslang::runtime::NativeEpollReactor;

static void require(bool yes) {
    if (!yes) std::abort();
}
static NativeFdAuthorityBroker::Completion next_terminal(
        NativeFdAuthorityBroker& broker) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        auto done = broker.try_pop();
        if (done) return *done;
        std::this_thread::yield();
    }
    std::abort();
}
static void wait_until(const std::atomic<bool>& ready) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(4);
    while (!ready.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) std::abort();
        std::this_thread::yield();
    }
}
static void test_revoked_and_stopped_rights() {
    using Outcome = NativeEpollReactor::Outcome;
    NativeFdAuthorityBroker broker;
    const auto token = broker.mint_eventfd(10);
    require(token.has_value());
    require(!broker.drain_eventfd(*token, 11));
    require(broker.try_watch(*token, 10));
    require(!broker.drain_eventfd(*token, 10));
    broker.stop();
    require(!broker.mint_eventfd(10));
    require(!broker.try_watch(*token, 10));
    require(!broker.signal(*token, 10));
    require(!broker.try_cancel(*token, 10));
    require(!broker.drain_eventfd(*token, 10));
    require(broker.revoke(*token, 10));
    require(!broker.confirm_reclaimed(*token, 10));
    auto done = next_terminal(broker);
    require(done.owner == 10 && done.outcome == Outcome::stopped);
    require(broker.confirm_reclaimed(*token, 10));
    require(!broker.confirm_reclaimed(*token, 10));
    require(!broker.try_pop());
}
static void test_counter_drain_and_rearm() {
    using Outcome = NativeEpollReactor::Outcome;
    NativeFdAuthorityBroker broker;
    auto token = broker.mint_eventfd(20);
    require(token.has_value());
    require(broker.try_watch(*token, 20));
    require(broker.signal(*token, 20));
    auto one = next_terminal(broker);
    require(one.owner == 20 && one.outcome == Outcome::ready);
    require(!broker.try_pop());
    require(!broker.drain_eventfd(*token, 21));
    require(!broker.try_watch(*token, 20)); // dirty counter forbids rearm
    require(broker.drain_eventfd(*token, 20));
    require(!broker.drain_eventfd(*token, 20)); // kernel counter is now zero
    require(broker.try_watch(*token, 20));
    require(!broker.drain_eventfd(*token, 20));
    // No event should arrive until another signal; epoll's old one-shot
    // watch was retired and the eventfd counter was explicitly drained.
    const auto quiet_until = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(25);
    while (std::chrono::steady_clock::now() < quiet_until) {
        require(!broker.try_pop());
        std::this_thread::yield();
    }
    require(broker.signal(*token, 20));
    auto two = next_terminal(broker);
    require(two.owner == 20 && two.outcome == Outcome::ready);
    require(broker.drain_eventfd(*token, 20));
    require(broker.revoke(*token, 20));
    require(!broker.drain_eventfd(*token, 20));
    require(broker.confirm_reclaimed(*token, 20));
}
static void test_clean_cancel_can_rearm_without_drain() {
    using Outcome = NativeEpollReactor::Outcome;
    NativeFdAuthorityBroker broker;
    auto token = broker.mint_eventfd(25);
    require(token.has_value());
    require(broker.try_watch(*token, 25));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(4);
    while (!broker.try_cancel(*token, 25)) {
        if (std::chrono::steady_clock::now() >= deadline) std::abort();
        std::this_thread::yield();
    }
    auto result = next_terminal(broker);
    require(result.outcome == Outcome::cancelled && result.owner == 25);
    // No successful signal occurred; there is no dirty counter to drain.
    require(!broker.drain_eventfd(*token, 25));
    require(broker.try_watch(*token, 25));
    require(broker.revoke(*token, 25));
    auto terminal = next_terminal(broker);
    require(terminal.outcome == Outcome::cancelled && terminal.owner == 25);
    require(broker.confirm_reclaimed(*token, 25));
}
static void test_consumption_retry_exactly_once() {
    using Outcome = NativeEpollReactor::Outcome;
    NativeFdAuthorityBroker broker;
    auto token = broker.mint_eventfd(30);
    require(token.has_value() && broker.try_watch(*token, 30));
    require(broker.signal(*token, 30));
    broker.test_deny_next_terminal_consume(2);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(4);
    while (!broker.test_has_pending_terminal(true)) {
        require(!broker.try_pop()); // forced denial retains real completion
        if (std::chrono::steady_clock::now() >= deadline) std::abort();
        std::this_thread::yield();
    }
    require(!broker.confirm_reclaimed(*token, 30));
    auto delivered = next_terminal(broker);
    require(delivered.owner == 30 && delivered.outcome == Outcome::ready);
    require(!broker.try_pop()); // no duplicate terminal
    require(broker.drain_eventfd(*token, 30));
    require(broker.revoke(*token, 30));
    require(broker.confirm_reclaimed(*token, 30));
}
static void test_real_mutex_contention_does_not_lose_terminal() {
    using Outcome = NativeEpollReactor::Outcome;
    NativeFdAuthorityBroker broker;
    std::atomic<bool> held{false}, release{false};
    // A real ledger mutex is held by another thread. The broker's
    // nonblocking authority transition must deny without blocking.
    auto key = broker.mint_eventfd(40);
    require(key.has_value() && broker.try_watch(*key, 40));
    require(broker.signal(*key, 40));
    std::thread blocker([&] {
        broker.test_hold_authority_gate_until(held, release);
    });
    wait_until(held);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(4);
    while (!broker.test_has_pending_terminal()) {
        require(!broker.try_pop());
        if (std::chrono::steady_clock::now() >= deadline) std::abort();
        std::this_thread::yield();
    }
    require(!broker.try_pop()); // still held; don't deliver an unverified FD
    release.store(true, std::memory_order_release);
    blocker.join();
    auto done = next_terminal(broker);
    require(done.owner == 40 && done.outcome == Outcome::ready);
    require(!broker.try_pop());
    require(broker.drain_eventfd(*key, 40));
    require(broker.revoke(*key, 40));
    require(broker.confirm_reclaimed(*key, 40));
}
static void test_real_mutex_denies_partial_mint() {
    NativeFdAuthorityBroker broker;
    std::atomic<bool> held{false}, release{false};
    std::thread blocker([&] {
        broker.test_hold_authority_gate_until(held, release);
    });
    wait_until(held);
    require(!broker.mint_eventfd(50)); // did not open kernel FD
    release.store(true, std::memory_order_release);
    blocker.join();
    auto token = broker.mint_eventfd(50);
    require(token.has_value());
    require(broker.revoke(*token, 50));
    require(broker.confirm_reclaimed(*token, 50));
}

static void test_atomic_host_admission_under_denial() {
    NativeFdAuthorityBroker broker;
    auto token = broker.mint_eventfd(72);
    require(token.has_value());
    require(broker.started_threads() == 0);
    broker.test_deny_next_watch_begin(2);
    require(!broker.try_watch(*token, 72));
    require(!broker.try_watch(*token, 72));
    // Both denials happened before epoll could allocate a watch or pthread.
    require(broker.started_threads() == 0);
    require(!broker.try_pop());
    require(broker.try_watch(*token, 72));
    require(broker.revoke(*token, 72));
    auto terminal = next_terminal(broker);
    require(terminal.owner == 72);
    require(broker.confirm_reclaimed(*token, 72));
}
static void test_physical_close_only_after_authenticated_receipt() {
    NativeFdAuthorityBroker broker;
    auto token = broker.mint_eventfd(73);
    require(token.has_value());
    require(broker.revoke(*token, 73));
    broker.test_deny_next_cleanup(2);
    require(!broker.confirm_reclaimed(*token, 73));
    require(!broker.confirm_reclaimed(*token, 73));
    // Ledger denial cannot close the descriptor or consume the token;
    // a subsequent authenticated receipt succeeds without retrying close.
    require(broker.confirm_reclaimed(*token, 73));
    require(!broker.confirm_reclaimed(*token, 73));
    require(!broker.try_watch(*token, 73));
    auto replacement = broker.mint_eventfd(73);
    require(replacement.has_value());
    require(broker.revoke(*replacement, 73));
    require(broker.confirm_reclaimed(*replacement, 73));
}
static void test_real_manager_bootstrap_open_denial_is_retry_safe() {
    // An accepted RT readiness ticket seals future eventfd creation. The
    // logical ledger must NOT mint an authority slot on that denied open,
    // and the previously accepted watch must remain serviceable.
    NativeFdAuthorityBroker broker(
        1, oreslang::runtime::NativeEpollReactorManager::Profile::preallocated_rt, 1);
    auto token = broker.mint_eventfd(75);
    require(token.has_value());
    require(broker.try_watch(*token, 75));
    require(!broker.mint_eventfd(76)); // kernel open denied by RT seal
    require(!broker.mint_eventfd(76)); // no poisoned broker or ghost slot
    require(broker.signal(*token, 75));
    auto completed = next_terminal(broker);
    require(completed.owner == 75 &&
            completed.outcome == NativeEpollReactor::Outcome::ready);
    require(broker.drain_eventfd(*token, 75));
    require(broker.revoke(*token, 75));
    require(broker.confirm_reclaimed(*token, 75));
    require(!broker.mint_eventfd(76)); // RT seal remains enforceable
}
static void test_real_gate_contention_during_cleanup() {
    NativeFdAuthorityBroker broker;
    auto token = broker.mint_eventfd(74);
    require(token.has_value());
    require(broker.revoke(*token, 74));
    std::atomic<bool> acquired{false}, released{false};
    std::thread blocker([&] {
        broker.test_hold_authority_gate_until(acquired, released);
    });
    wait_until(acquired);
    require(!broker.confirm_reclaimed(*token, 74));
    released.store(true, std::memory_order_release);
    blocker.join();
    require(broker.confirm_reclaimed(*token, 74));
}
static void test_uncertain_kernel_close_quarantines_manager_slot() {
    // Source-local fault injection runs AFTER the real close(2) succeeds.
    // Model an unexpected close result without leaking a live descriptor.
    // The manager must fail closed and NOT offer the uncertain entry for
    // reuse, even though the old capability has been invalidated.
    using Manager = oreslang::runtime::NativeEpollReactorManager;
    Manager manager(1);
    auto original = manager.open_owned_eventfd();
    require(original.has_value());
    manager.test_uncertain_next_owned_close();
    require(!manager.close_owned_eventfd(*original));
    require(manager.test_quarantined_owned_slots() == 1);
    require(!manager.signal_owned_eventfd(*original));
    require(!manager.try_watch(*original, EPOLLIN));
    require(!manager.close_owned_eventfd(*original));

    auto replacement = manager.open_owned_eventfd();
    require(replacement.has_value());
    require(manager.test_quarantined_owned_slots() == 1);
    require(manager.close_owned_eventfd(*replacement));
    // Releasing a different slot must not clear the quarantine receipt.
    require(manager.test_quarantined_owned_slots() == 1);
}

static void test_real_broker_uncertain_close_never_mints_cleanup_receipt() {
    // The real broker transitions logical reclamation with a manager-owned
    // Linux eventfd. An ambiguous physical close must NOT mark the ledger
    // reclaimed or revalidate the old authenticated owner/token.
    NativeFdAuthorityBroker broker;
    auto original = broker.mint_eventfd(76);
    require(original.has_value());
    require(broker.revoke(*original, 76));
    broker.test_uncertain_next_physical_close();
    require(!broker.confirm_reclaimed(*original, 76));
    require(broker.test_quarantined_manager_slots() == 1);
    require(!broker.confirm_reclaimed(*original, 76));
    require(!broker.try_watch(*original, 76));
    require(!broker.signal(*original, 76));
    require(!broker.drain_eventfd(*original, 76));
    require(!broker.mint_eventfd(0));
    // Only the ambiguous operation is quarantined. A distinct independent
    // owner must retain forward progress and exactly-once reclamation.
    auto independent = broker.mint_eventfd(77);
    require(independent.has_value());
    require(broker.revoke(*independent, 77));
    require(broker.confirm_reclaimed(*independent, 77));
    require(!broker.confirm_reclaimed(*independent, 77));
    require(broker.test_quarantined_manager_slots() == 1);
}

static void test_uncertain_close_exhaustion_is_fail_closed() {
    // Every unknown close result burns one manager-owned registration slot.
    // This test does 256 real eventfd open/close operations, never retaining
    // a live kernel descriptor or retrying a potentially reused fd number.
    using Manager = oreslang::runtime::NativeEpollReactorManager;
    Manager manager(1);
    constexpr unsigned slots = 256;
    for (unsigned i = 0; i < slots; ++i) {
        auto token = manager.open_owned_eventfd();
        require(token.has_value());
        manager.test_uncertain_next_owned_close();
        require(!manager.close_owned_eventfd(*token));
        require(!manager.close_owned_eventfd(*token));
        require(!manager.signal_owned_eventfd(*token));
        require(manager.test_quarantined_owned_slots() == i + 1);
    }
    require(!manager.open_owned_eventfd()); // no identity-slot recycling
    manager.stop();
    require(manager.test_quarantined_owned_slots() == slots);
}

static void test_all_orderings_of_signal_cancel_revoke_stop() {
    // All 24 serial orderings at the trusted-host API boundary. Kernel
    // readiness versus cancellation may resolve differently; all outcomes
    // must be exactly-once, owner-authenticated and reclaimable.
    std::array<int, 4> order{{0, 1, 2, 3}};
    unsigned executed = 0;
    do {
        NativeFdAuthorityBroker broker;
        auto token = broker.mint_eventfd(60);
        require(token.has_value() && broker.try_watch(*token, 60));
        bool revoked = false, stopped = false;
        for (int action : order) {
            switch (action) {
                case 0: {
                    const bool accepted = broker.signal(*token, 60);
                    if (revoked || stopped) require(!accepted);
                    break;
                }
                case 1: {
                    const bool cancelled = broker.try_cancel(*token, 60);
                    if (revoked || stopped) require(!cancelled);
                    break;
                }
                case 2:
                    require(broker.revoke(*token, 60));
                    revoked = true;
                    break;
                case 3:
                    broker.stop();
                    stopped = true;
                    break;
                default: std::abort();
            }
        }
        require(revoked && stopped);
        require(!broker.confirm_reclaimed(*token, 60));
        auto terminal = next_terminal(broker);
        require(terminal.owner == 60);
        require(terminal.outcome == NativeEpollReactor::Outcome::ready ||
                terminal.outcome == NativeEpollReactor::Outcome::cancelled ||
                terminal.outcome == NativeEpollReactor::Outcome::stopped);
        require(!broker.try_pop());
        require(broker.confirm_reclaimed(*token, 60));
        require(!broker.confirm_reclaimed(*token, 60));
        require(!broker.signal(*token, 60));
        ++executed;
    } while (std::next_permutation(order.begin(), order.end()));
    require(executed == 24);
}
int main() {
    test_revoked_and_stopped_rights();
    test_counter_drain_and_rearm();
    test_clean_cancel_can_rearm_without_drain();
    test_consumption_retry_exactly_once();
    test_real_mutex_contention_does_not_lose_terminal();
    test_real_mutex_denies_partial_mint();
    test_atomic_host_admission_under_denial();
    test_physical_close_only_after_authenticated_receipt();
    test_real_manager_bootstrap_open_denial_is_retry_safe();
    test_real_gate_contention_during_cleanup();
    test_uncertain_kernel_close_quarantines_manager_slot();
    test_real_broker_uncertain_close_never_mints_cleanup_receipt();
    test_uncertain_close_exhaustion_is_fail_closed();
    test_all_orderings_of_signal_cancel_revoke_stop();
    return 0;
}
