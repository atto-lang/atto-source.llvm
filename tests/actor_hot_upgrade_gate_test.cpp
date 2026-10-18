#include "oreslang/runtime/ActorHotUpgradeGate.hpp"

#include <atomic>
#include <optional>
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
    {
        // Abandoning a prepared token due to an exception must auto-abort.
        ActorHotUpgradeGate gate(15, image('a'));
        {
            auto abandoned = gate.prepare(image('b'));
            assert(gate.frozen());
        }
        assert(!gate.frozen() && gate.pins() == 0);
        auto ready = gate.pin();
        assert(gate.permits(ready));
        ready.close();

        // Move assignment must auto-abort the ticket being overwritten and
        // transfer exactly one active authority to the recipient.
        ActorHotUpgradeGate first(16, image('a'));
        ActorHotUpgradeGate second(17, image('a'));
        auto old = first.prepare(image('b'));
        auto newer = second.prepare(image('c'));
        old = std::move(newer);
        assert(!first.frozen());
        assert(second.frozen());
        assert(second.publish(old));
        assert(!second.frozen() && second.epoch() == 2);
    }
    {
        ActorHotUpgradeGate gate(18, image('a'));
        try {
            auto unfinished = gate.prepare(image('b'));
            throw std::runtime_error("injected upgrade failure");
        } catch (const std::runtime_error&) {}
        assert(!gate.frozen());
        auto followup = gate.prepare(image('c'));
        assert(gate.publish(followup));
        assert(gate.epoch() == 2);
    }
    {
        // Releasing a stale ticket must not unfreeze a later prepared epoch.
        ActorHotUpgradeGate gate(19, image('a'));
        {
            std::optional<ActorHotUpgradeGate::Candidate> obsolete;
            obsolete.emplace(gate.prepare(image('b')));
            gate.abort(*obsolete);
            auto current = gate.prepare(image('c'));
            assert(gate.frozen());
            obsolete.reset();
            // Stale RAII cleanup must never undo a later ticket's fence.
            assert(gate.frozen());
            assert(gate.publish(current));
            assert(gate.epoch() == 2 && !gate.frozen());
        }
        auto next = gate.pin();
        next.close();
    }
    {
        // The ticket may outlive the gate wrapper; terminal close remains
        // authoritative and the token releases only its shared State.
        std::optional<ActorHotUpgradeGate::Candidate> orphan;
        {
            ActorHotUpgradeGate gate(20, image('a'));
            orphan.emplace(gate.prepare(image('b')));
            assert(gate.frozen());
        }
        orphan.reset();
    }
    return 0;
}
