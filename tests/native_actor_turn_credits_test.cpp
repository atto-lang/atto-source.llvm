#include "oreslang/runtime/NativeActorTurnCredits.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::NativeActorTurnCredits;

static void check(bool ok) {
    if (!ok) throw std::runtime_error("native actor physical-turn credits failed");
}

int main() {
    {
        NativeActorTurnCredits gate;
        check(gate.capacity() == 1);
        auto first = gate.try_acquire();
        check(first.has_value());
        check(!gate.try_acquire().has_value());
        check(gate.in_flight() == 1);
        check(gate.release(*first));
        check(!gate.release(*first));
        check(gate.in_flight() == 0);
    }
    {
        NativeActorTurnCredits gate(3);
        auto a = gate.try_acquire();
        auto b = gate.try_acquire();
        auto c = gate.try_acquire();
        check(a.has_value() && b.has_value() && c.has_value());
        check(a->slot != b->slot && a->slot != c->slot && b->slot != c->slot);
        check(gate.in_flight() == 3);
        check(!gate.try_acquire().has_value());
        check(gate.release(*b));
        auto next = gate.try_acquire();
        check(next.has_value());
        check(!gate.release(*b)); // stale claim cannot free reused slot
        check(gate.in_flight() == 3);
        gate.retire();
        check(gate.is_retired() && !gate.try_acquire().has_value());
        check(gate.release(*a));
        check(gate.release(*c));
        check(gate.release(*next));
        check(gate.in_flight() == 0);
    }
    {
        bool rejected_zero = false;
        bool rejected_four = false;
        try { NativeActorTurnCredits invalid(0); } catch (const std::invalid_argument&) {
            rejected_zero = true;
        }
        try { NativeActorTurnCredits invalid(4); } catch (const std::invalid_argument&) {
            rejected_four = true;
        }
        check(rejected_zero && rejected_four);
    }
    {
        NativeActorTurnCredits gate(3);
        std::atomic<unsigned> entrants{0};
        std::atomic<unsigned> high_water{0};
        std::atomic<bool> release{false};
        std::atomic<unsigned> attempted{0};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 12; ++i) {
            threads.emplace_back([&] {
                auto claim = gate.try_acquire();
                attempted.fetch_add(1, std::memory_order_acq_rel);
                if (!claim) return;
                auto value = entrants.fetch_add(1, std::memory_order_acq_rel) + 1;
                auto peak = high_water.load(std::memory_order_relaxed);
                while (peak < value && !high_water.compare_exchange_weak(
                        peak, value, std::memory_order_relaxed)) { }
                while (!release.load(std::memory_order_acquire))
                    std::this_thread::yield(); // test-only barrier, never runtime code
                check(gate.release(*claim));
                entrants.fetch_sub(1, std::memory_order_acq_rel);
            });
        }
        // All contenders have attempted acquisition before retirement.
        // Test-only coordination yields rather than consuming a reactor/carrier.
        while (attempted.load(std::memory_order_acquire) != 12)
            std::this_thread::yield();
        check(gate.in_flight() <= 3);
        // Retirement must not revoke already-issued claims or create new ones.
        gate.retire();
        release.store(true, std::memory_order_release);
        for (auto& thread : threads) thread.join();
        check(high_water.load() <= 3);
        check(gate.in_flight() == 0);
        check(!gate.try_acquire().has_value());
    }
    return 0;
}
