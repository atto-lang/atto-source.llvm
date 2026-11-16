#include "oreslang/runtime/NativeTenantAdmission.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

using oreslang::runtime::NativeTenantAdmission;

static void close_preserves_pins_but_revokes_access() {
    NativeTenantAdmission vm(3, 2, 32);
    const auto h0 = vm.open(), h1 = vm.open();
    assert(h0 && h1);
    auto turn0 = vm.enter(*h0, 0), turn1 = vm.enter(*h1, 1);
    assert(turn0 && turn1);
    const std::uint64_t secret = 0xdeadbeef01234567ULL;
    assert(turn0->write(0, &secret, sizeof(secret)));
    assert(turn1->write(0, &secret, sizeof(secret)));
    std::atomic<bool> go{false}, quit{false};
    std::atomic<unsigned> attempted{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&, i] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (!quit.load(std::memory_order_acquire)) {
                if (i % 3 == 0) {
                    // Races with the global close barrier; a successful
                    // open before close may remain quiescing, never recycled.
                    auto h = vm.open();
                    if (h) {
                        auto t = vm.enter(*h, 0);
                        if (t) {
                            std::uint64_t value = static_cast<std::uint64_t>(i);
                            (void)t->write(0, &value, sizeof(value));
                            t.reset();
                        }
                        if (vm.request_stop(*h)) (void)vm.finish_stop(*h);
                    }
                } else if (i % 3 == 1) {
                    auto t = vm.enter(*h0, 1);
                    if (t) {
                        std::uint64_t x = 1;
                        (void)t->read(0, &x, sizeof(x));
                    }
                } else {
                    auto h = vm.open();
                    if (h) (void)vm.request_stop(*h);
                }
                attempted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true, std::memory_order_release);
    while (attempted.load(std::memory_order_relaxed) < 80)
        std::this_thread::yield();
    std::thread a([&] { vm.close(); });
    std::thread b([&] { vm.close(); });
    a.join(); b.join();
    // Producer threads are still live, so the close() test genuinely races
    // admission. Only the wrapper destructor requires producer joins.
    for (int repeat = 0; repeat < 50; ++repeat) {
        assert(!vm.open());
        assert(!vm.enter(*h0, 1) && !vm.enter(*h1, 0));
        assert(!vm.request_stop(*h0));
        assert(!vm.finish_stop(*h1));
    }
    quit.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    std::uint64_t after = 0;
    assert(!turn0->read(0, &after, sizeof(after)));
    assert(!turn1->write(0, &after, sizeof(after)));
    // Physical turn pins may be safely released after the host close.
    turn0.reset(); turn1.reset();
    vm.close();  // idempotent
}

static void many_close_races_and_independent_registries() {
    constexpr int kRounds = 96;
    for (int round = 0; round < kRounds; ++round) {
        NativeTenantAdmission vm(2, 2, 16);
        NativeTenantAdmission peer(1, 1, 16);
        auto h = vm.open(), other = peer.open();
        assert(h && other);
        auto pinned = vm.enter(*h, 0); assert(pinned);
        std::atomic<bool> start{false};
        std::atomic<int> attempts{0};
        std::thread t1([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < 50; ++i) {
                auto candidate = vm.enter(*h, 1);
                if (candidate) {
                    std::uint64_t n = static_cast<std::uint64_t>(i);
                    (void)candidate->write(0, &n, sizeof(n));
                    attempts.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        std::thread t2([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            vm.close();
        });
        start.store(true, std::memory_order_release);
        t1.join(); t2.join();
        assert(!vm.open());
        assert(!vm.enter(*h, 1));
        std::uint64_t x = 0;
        assert(!pinned->read(0, &x, sizeof(x)));
        pinned.reset();
        // Closing this registry must not alter a sibling registry or its
        // actor data, regardless of identical slot and generation values.
        auto available = peer.enter(*other, 0); assert(available);
        x = 0x5a5a5a5a;
        assert(available->write(0, &x, sizeof(x)));
        x = 0;
        assert(available->read(0, &x, sizeof(x)) && x == 0x5a5a5a5a);
        available.reset();
        assert(peer.request_stop(*other) && peer.finish_stop(*other));
    }
}

int main() {
    close_preserves_pins_but_revokes_access();
    many_close_races_and_independent_registries();
}
