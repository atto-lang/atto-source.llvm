#include "oreslang/runtime/NativeActorHotUpgradeGate.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

using namespace oreslang::runtime;
using Gate = NativeActorHotUpgradeGate<std::string>;
static Gate::Shape shape() {
    return {Gate::Kind::trusted_private, "msg-v1", "state-v1", "cells-v1", "caps-v1"};
}
static void check(bool result, const char* error) {
    if (!result) throw std::runtime_error(error);
}
static void reentrant_close_from_reclamation() {
    const ActorHeapIdentity id{11, 22, 33};
    std::atomic<int> old{0}, next{0};
    Gate* owner = nullptr;
    Gate gate(id, shape(), "v1", [&] {
        ++old;
        if (owner) owner->close();
    });
    owner = &gate;
    auto ticket = gate.freeze(id, 1);
    check(gate.quiesced(ticket), "actor not quiescent");
    auto published = gate.promote(ticket, shape(), "v2", [&] { ++next; });
    check(published.revision == 2, "new image publication failed");
    bool refused = false;
    try { (void)gate.pin(id); }
    catch (const std::logic_error&) { refused = true; }
    check(refused, "reentrant terminal shutdown reopened actor admission");
    check(old == 1 && next == 1, "reentrant close leaked or duplicated cleanup");
}
static void concurrent_shutdown_publication() {
    for (std::uint64_t i = 1; i <= 256; ++i) {
        const ActorHeapIdentity id{44, i, 1};
        std::atomic<int> reclaimed{0};
        Gate gate(id, shape(), "first", [&] { ++reclaimed; });
        auto ticket = gate.freeze(id, 1);
        std::atomic<bool> go{false}, published{false}, rejected{false};
        std::thread worker([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            try {
                auto desc = gate.promote(ticket, shape(), "second",
                                         [&] { ++reclaimed; });
                check(desc.revision == 2, "invalid revision");
                published.store(true, std::memory_order_release);
            } catch (const std::logic_error&) {
                rejected.store(true, std::memory_order_release);
            }
        });
        go.store(true, std::memory_order_release);
        gate.close();
        worker.join();
        check(published.load() != rejected.load(),
              "close race yielded no result or multiple outcomes");
        bool refused = false;
        try { (void)gate.pin(id); }
        catch (const std::logic_error&) { refused = true; }
        check(refused, "closed actor admitted new work");
        check(reclaimed == 2, "close race leaked or double-freed code image");
    }
}
int main() {
    reentrant_close_from_reclamation();
    concurrent_shutdown_publication();
}
