#include "oreslang/runtime/NativeFdAuthority.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <thread>
using oreslang::runtime::NativeFdAuthority;
static void check(bool c) { if (!c) std::abort(); }
int main() {
    NativeFdAuthority a, b;
    using P = NativeFdAuthority::Phase;
    auto cap = a.mint(10, NativeFdAuthority::readiness | NativeFdAuthority::transfer);
    check(cap.has_value());
    check(!b.begin_watch(*cap, 10));
    check(!a.begin_watch(*cap, 20));
    check(a.begin_watch(*cap, 10));
    check(!a.begin_watch(*cap, 10));
    check(!a.begin_transfer(*cap, 10, 20));
    check(a.publish_terminal(*cap, 10));
    check(!a.publish_terminal(*cap, 10));
    check(!a.begin_transfer(*cap, 10, 20));
    check(a.consume_terminal(*cap, 10));
    check(!a.consume_terminal(*cap, 10));
    auto handoff = a.begin_transfer(*cap, 10, 20);
    check(handoff.has_value());
    check(!a.begin_watch(*cap, 10));
    check(!a.acknowledge(*handoff, 30));
    auto destination = a.acknowledge(*handoff, 20);
    check(destination.has_value());
    check(!a.acknowledge(*handoff, 20));
    check(!a.begin_watch(*cap, 20)); // stale generation after transfer
    check(!a.begin_watch(*cap, 10));
    check(a.begin_watch(*destination, 20));
    check(a.revoke(*destination, 20));
    check(!a.begin_watch(*destination, 20));
    check(a.publish_terminal(*destination, 20));
    check(a.consume_terminal(*destination, 20));
    check(a.phase(*destination) == P::reclaim_pending);
    check(!a.begin_watch(*destination, 20));
    check(!a.consume_terminal(*destination, 20));
    check(!a.confirm_reclaimed(*destination, 10));
    check(a.confirm_reclaimed(*destination, 20));
    check(a.phase(*destination) == P::revoked);
    check(!a.confirm_reclaimed(*destination, 20));
    auto replacement = a.mint(30, NativeFdAuthority::readiness);
    check(replacement.has_value());
    check(!a.begin_watch(*destination, 30));
    check(a.begin_watch(*replacement, 30));
    check(a.publish_terminal(*replacement, 30));
    check(a.consume_terminal(*replacement, 30));
    check(a.revoke(*replacement, 30));
    check(a.phase(*replacement) == P::reclaim_pending);
    check(a.confirm_reclaimed(*replacement, 30));

    // A terminal result alone is not a descriptor cleanup receipt, even
    // when the caller owns the right to revoke before consumption.
    {
        NativeFdAuthority extra;
        auto key = extra.mint(49, NativeFdAuthority::readiness);
        check(key.has_value());
        check(!extra.confirm_reclaimed(*key, 49));
        check(extra.begin_watch(*key, 49));
        check(extra.revoke(*key, 49));
        check(!extra.confirm_reclaimed(*key, 49));
        check(extra.publish_terminal(*key, 49));
        check(!extra.confirm_reclaimed(*key, 49));
        check(extra.consume_terminal(*key, 49));
        check(extra.phase(*key) == P::reclaim_pending);
        check(extra.confirm_reclaimed(*key, 49));
    }
    // Exhaustion rejects without evicting any live owner. Reusing a revoked
    // slot rotates its generation, so an old copied handle remains invalid.
    {
        NativeFdAuthority saturated;
        std::array<NativeFdAuthority::Capability, NativeFdAuthority::capacity> keys{};
        for (auto& key : keys) {
            auto issued = saturated.mint(77, NativeFdAuthority::readiness);
            check(issued.has_value());
            key = *issued;
        }
        check(!saturated.mint(77, NativeFdAuthority::readiness));
        check(!saturated.begin_transfer(keys[0], 77, 88));
        check(saturated.revoke(keys[0], 77));
        check(saturated.phase(keys[0]) == P::reclaim_pending);
        check(!saturated.mint(99, NativeFdAuthority::readiness));
        check(saturated.confirm_reclaimed(keys[0], 77));
        auto recycled = saturated.mint(99, NativeFdAuthority::readiness);
        check(recycled.has_value());
        check(!saturated.begin_watch(keys[0], 77));
        check(!saturated.begin_watch(keys[0], 99));
        check(saturated.begin_watch(*recycled, 99));
        check(saturated.publish_terminal(*recycled, 99));
        check(saturated.revoke(*recycled, 99));
        check(saturated.consume_terminal(*recycled, 99));
        check(saturated.phase(*recycled) == P::reclaim_pending);
        check(saturated.confirm_reclaimed(*recycled, 99));
        check(saturated.phase(*recycled) == P::revoked);
    }
    // Foreign authority instances cannot acknowledge another broker's
    // pending handoff, even when their local slot and generation coincide.
    {
        NativeFdAuthority origin, foreign;
        auto key = origin.mint(11, NativeFdAuthority::readiness | NativeFdAuthority::transfer);
        check(key.has_value());
        auto pending = origin.begin_transfer(*key, 11, 22);
        check(pending.has_value());
        check(!foreign.acknowledge(*pending, 22));
        check(origin.revoke(*key, 11));
        check(origin.phase(*key) == P::reclaim_pending);
        check(!origin.acknowledge(*pending, 22));
        check(!origin.confirm_reclaimed(*key, 22));
        check(origin.confirm_reclaimed(*key, 11));
    }

    for (int round = 0; round < 1000; ++round) {
        auto key = a.mint(1, NativeFdAuthority::readiness | NativeFdAuthority::transfer);
        check(key.has_value());
        std::atomic<int> arrivals{0};
        std::atomic<bool> watch{false}, transfer{false};
        std::thread actor([&] {
            arrivals.fetch_add(1);
            while (arrivals.load() != 2) std::this_thread::yield();
            watch.store(a.begin_watch(*key, 1));
        });
        std::thread broker([&] {
            arrivals.fetch_add(1);
            while (arrivals.load() != 2) std::this_thread::yield();
            transfer.store(a.begin_transfer(*key, 1, 2).has_value());
        });
        actor.join(); broker.join();
        check(!(watch.load() && transfer.load()));
        const auto phase = a.phase(*key);
        check(phase.has_value());
        if (*phase == P::watching) {
            check(a.revoke(*key, 1));
            check(a.publish_terminal(*key, 1));
            check(a.consume_terminal(*key, 1));
        } else if (*phase == P::transferring) {
            check(a.revoke(*key, 1));
        } else {
            check(a.revoke(*key, 1));
        }
        check(a.phase(*key) == P::reclaim_pending);
        check(!a.begin_watch(*key, 1));
        check(a.confirm_reclaimed(*key, 1));
        check(a.phase(*key) == P::revoked);
    }

    // Race: handoff completion and original-owner revoke have only one winner.
    // Both methods can transiently deny on try_lock contention.
    for (int round = 0; round < 128; ++round) {
        NativeFdAuthority race;
        auto key = race.mint(71, NativeFdAuthority::readiness |
                               NativeFdAuthority::transfer);
        check(key.has_value());
        auto pending = race.begin_transfer(*key, 71, 72);
        check(pending.has_value());
        std::atomic<int> entered{0};
        std::atomic<bool> revoked{false};
        std::optional<NativeFdAuthority::Capability> transferred;
        std::thread destination([&] {
            entered.fetch_add(1, std::memory_order_acq_rel);
            while (entered.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            transferred = race.acknowledge(*pending, 72);
        });
        std::thread origin([&] {
            entered.fetch_add(1, std::memory_order_acq_rel);
            while (entered.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            revoked.store(race.revoke(*key, 71), std::memory_order_release);
        });
        destination.join();
        origin.join();
        check(!(transferred.has_value() && revoked.load()));
        if (transferred) {
            check(!race.begin_watch(*key, 71));
            check(!race.confirm_reclaimed(*key, 71));
            check(race.revoke(*transferred, 72));
            check(race.phase(*transferred) == P::reclaim_pending);
            check(race.confirm_reclaimed(*transferred, 72));
            check(!race.revoke(*transferred, 72));
        } else {
            if (!revoked.load()) check(race.revoke(*key, 71));
            check(race.phase(*key) == P::reclaim_pending);
            check(!race.acknowledge(*pending, 72));
            check(!race.confirm_reclaimed(*key, 72));
            check(race.confirm_reclaimed(*key, 71));
            check(!race.revoke(*key, 71));
        }
    }
    // Race: a revoked watch cannot reclaim before its unique terminal is
    // published AND consumed, regardless of producer/owner interleaving.
    for (int round = 0; round < 128; ++round) {
        NativeFdAuthority race;
        auto key = race.mint(81, NativeFdAuthority::readiness);
        check(key.has_value() && race.begin_watch(*key, 81));
        std::atomic<int> entered{0};
        std::atomic<bool> published{false}, revoked{false};
        std::thread reactor([&] {
            entered.fetch_add(1, std::memory_order_acq_rel);
            while (entered.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            published.store(race.publish_terminal(*key, 81),
                            std::memory_order_release);
        });
        std::thread owner([&] {
            entered.fetch_add(1, std::memory_order_acq_rel);
            while (entered.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            revoked.store(race.revoke(*key, 81),
                          std::memory_order_release);
        });
        reactor.join();
        owner.join();
        if (!revoked.load()) check(race.revoke(*key, 81));
        if (!published.load()) check(race.publish_terminal(*key, 81));
        check(race.phase(*key) == P::revoking_terminal);
        check(!race.confirm_reclaimed(*key, 81));
        check(!race.publish_terminal(*key, 81));
        check(!race.begin_watch(*key, 81));
        check(race.consume_terminal(*key, 81));
        check(!race.consume_terminal(*key, 81));
        check(race.phase(*key) == P::reclaim_pending);
        check(!race.confirm_reclaimed(*key, 82));
        check(race.confirm_reclaimed(*key, 81));
        check(race.phase(*key) == P::revoked);
        check(!race.revoke(*key, 81));
    }
    return 0;
}
