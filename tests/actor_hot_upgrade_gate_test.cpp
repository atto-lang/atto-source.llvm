#include "oreslang/runtime/ActorHotUpgradeGate.hpp"

#include <atomic>
#include <cassert>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using oreslang::runtime::ActorHotUpgradeGate;

namespace {
std::string hex(char c) { return std::string(64, c); }

ActorHotUpgradeGate::Image image(char code) {
    return {hex(code), hex('1'), hex('2'), hex('3')};
}

template<class F>
bool fails(F&& callback) {
    try { callback(); } catch (const std::exception&) { return true; }
    return false;
}
}

int main() {
    {
        ActorHotUpgradeGate gate(7, image('a'));
        auto lease = gate.pin();
        assert(lease.epoch() == 1 && gate.permits(lease));
        auto pending = gate.prepare(image('b'));
        assert(gate.frozen());
        assert(fails([&] { (void) gate.pin(); }));
        assert(!gate.publish(pending));
        assert(gate.image().code_sha256 == hex('a'));
        lease.close();
        lease.close();
        assert(gate.pins() == 0);
        assert(!gate.permits(lease));
        assert(gate.publish(pending));
        assert(gate.epoch() == 2);
        assert(gate.image().code_sha256 == hex('b'));
        assert(fails([&] { (void) gate.publish(pending); }));
    }
    {
        ActorHotUpgradeGate original(1, image('a'));
        ActorHotUpgradeGate other(2, image('a'));
        auto ticket = original.prepare(image('b'));
        assert(fails([&] { (void) other.publish(ticket); }));
        original.abort(ticket);
        assert(!original.frozen());
        assert(fails([&] { (void) original.publish(ticket); }));
        assert(original.image().code_sha256 == hex('a'));
        for (int index = 0; index != 3; ++index) {
            auto next = image('b');
            if (index == 0) next.state_sha256 = hex('4');
            if (index == 1) next.message_abi_sha256 = hex('4');
            if (index == 2) next.capability_sha256 = hex('4');
            assert(fails([&] { (void) original.prepare(next); }));
            assert(!original.frozen());
        }
        auto invalid = image('c');
        invalid.code_sha256 = "bad";
        assert(fails([&] { (void) original.prepare(invalid); }));
    }
    {
        ActorHotUpgradeGate gate(3, image('a'));
        auto lease = gate.pin();
        auto ticket = gate.prepare(image('b'));
        std::atomic<bool> release{false};
        std::thread worker([held = std::move(lease), &release]() mutable {
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            held.close();
        });
        assert(!gate.publish(ticket));
        release.store(true, std::memory_order_release);
        worker.join();
        assert(gate.publish(ticket));
        assert(gate.pins() == 0);
        gate.close();
        assert(gate.retired());
        assert(fails([&] { (void) gate.pin(); }));
    }
    {
        // A lease's shared ownership safely survives gate destruction.
        ActorHotUpgradeGate::Lease lease;
        {
            ActorHotUpgradeGate gate(11, image('a'));
            lease = gate.pin();
            assert(!gate.retired());
        }
        lease.close();
    }
    return 0;
}
