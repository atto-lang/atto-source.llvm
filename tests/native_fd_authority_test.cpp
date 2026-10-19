#include "oreslang/runtime/NativeFdAuthority.hpp"
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
    check(a.phase(*destination) == P::revoked);
    check(!a.consume_terminal(*destination, 20));
    auto replacement = a.mint(30, NativeFdAuthority::readiness);
    check(replacement.has_value());
    check(!a.begin_watch(*destination, 30));
    check(a.begin_watch(*replacement, 30));
    check(a.publish_terminal(*replacement, 30));
    check(a.consume_terminal(*replacement, 30));
    check(a.revoke(*replacement, 30));

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
        check(a.phase(*key) == P::revoked);
    }
    return 0;
}
