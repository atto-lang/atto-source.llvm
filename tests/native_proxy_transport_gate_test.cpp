#include "oreslang/runtime/NativeTransportCallbackGate.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using oreslang::runtime::NativeTransportCallbackGate;
static_assert(!std::is_copy_constructible_v<NativeTransportCallbackGate::Permit>);
static_assert(!std::is_default_constructible_v<NativeTransportCallbackGate::DrainReceipt>);

int main() {
    NativeTransportCallbackGate one(7, 4, 10, 100);
    NativeTransportCallbackGate other(7, 4, 10, 101);
    assert(!one.try_drain());
    auto first = one.try_admit();
    assert(first);
    auto moved = std::move(first);
    assert(!first && moved);
    one.stop_admission();
    assert(!one.try_admit());
    assert(!one.try_drain());
    moved.release();
    moved.release();
    auto receipt = one.try_drain();
    assert(receipt && one.verifies(*receipt));
    assert(!other.verifies(*receipt));
    other.stop_admission();
    auto other_receipt = other.try_drain();
    assert(other_receipt && !one.verifies(*other_receipt));

    // A physical completion on another carrier retains storage even after
    // the logical actor has stopped. The receipt cannot mint until it exits.
    NativeTransportCallbackGate concurrent(9, 6, 33, 200);
    auto lease = concurrent.try_admit();
    assert(lease);
    std::atomic<bool> entered{false};
    std::atomic<bool> finish{false};
    std::thread callback([permit = std::move(lease), &entered, &finish]() mutable {
        entered.store(true, std::memory_order_release);
        while (!finish.load(std::memory_order_acquire)) std::this_thread::yield();
        permit.release();
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    concurrent.stop_admission();
    assert(!concurrent.try_drain());
    assert(!concurrent.try_admit());
    finish.store(true, std::memory_order_release);
    callback.join();
    auto after = concurrent.try_drain();
    assert(after && concurrent.verifies(*after));
    // Race producer admission against the terminal stop bit. Every admitted
    // permit must be counted before freeze, and no permit may appear after.
    for (std::uint64_t iteration = 0; iteration < 128; ++iteration) {
        NativeTransportCallbackGate raced(17, 8, 123, 300 + iteration);
        std::atomic<bool> go{false};
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int attempt = 0; attempt < 200; ++attempt) {
                auto permit = raced.try_admit();
                if (!permit) break;
                std::this_thread::yield();
                // RAII release is the last operation touching the arena.
            }
        });
        std::thread freezer([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            raced.stop_admission();
        });
        go.store(true, std::memory_order_release);
        producer.join();
        freezer.join();
        assert(!raced.try_admit() && !raced.accepting());
        const auto terminal = raced.try_drain();
        assert(terminal && raced.verifies(*terminal));
    }
    std::puts("NATIVE_PROXY_TRANSPORT_GATE_OK");
}
