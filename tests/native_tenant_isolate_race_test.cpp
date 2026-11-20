#include "oreslang/runtime/NativeTenantAdmission.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::NativeTenantAdmission;

static void rejected_sizes() {
    auto invalid = [](std::size_t tenants, std::size_t actors, std::size_t bytes) {
        try { NativeTenantAdmission host(tenants, actors, bytes); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    };
    assert(invalid(0, 1, 1));
    assert(invalid(1, 0, 1));
    assert(invalid(1, 1, 0));
    assert(invalid(4097, 1, 1));
    assert(invalid(1, 4097, 1));
    assert(invalid(1, 1, NativeTenantAdmission::kMaxArenaBytes + 1));
    assert(invalid(4096, 4096, 4096));
    assert(invalid(2, 2, std::numeric_limits<std::size_t>::max()));
    assert(invalid(2, std::numeric_limits<std::size_t>::max(), 2));
}

static void stale_and_bounds() {
    NativeTenantAdmission host(1, 2, 16);
    const auto initial = host.open(); assert(initial);
    auto writer = host.enter(*initial, 0); assert(writer);
    assert(!host.enter(*initial, 0));
    assert(!host.enter({999, 1}, 0));
    assert(!host.enter({initial->slot, 0}, 0));
    assert(!host.enter(*initial, 2));
    assert(!host.request_stop({initial->slot, initial->generation + 1}));
    std::uint64_t x = 0xdeadbeef12345678ULL;
    std::uint64_t out = 0;
    assert(writer->write(0, &x, sizeof(x)));
    assert(writer->read(0, &out, sizeof(out)) && out == x);
    assert(!writer->write(12, &x, sizeof(x)));
    assert(!writer->read(16, &out, 1));
    assert(!writer->read(std::numeric_limits<std::size_t>::max(), &out, 1));
    assert(!writer->write(0, nullptr, 1));
    assert(!writer->read(0, nullptr, 1));
    assert(writer->read(16, nullptr, 0));
    assert(writer->write(16, nullptr, 0));
    assert(host.request_stop(*initial));
    assert(!host.request_stop(*initial));
    assert(!host.enter(*initial, 1));
    assert(!host.finish_stop(*initial));
    assert(writer->read(0, &out, sizeof(out)) && out == x);
    writer.reset();
    assert(host.finish_stop(*initial));
}

static void generations_and_move() {
    NativeTenantAdmission host(1, 2, 16);
    auto current = host.open(); assert(current);
    for (int iteration = 0; iteration < 300; ++iteration) {
        auto one = host.enter(*current, 0); assert(one);
        auto two = host.enter(*current, 1); assert(two);
        std::uint64_t a = static_cast<std::uint64_t>(iteration + 1);
        std::uint64_t b = a + 1;
        assert(one->write(0, &a, sizeof(a)));
        assert(two->write(0, &b, sizeof(b)));
        // Move assignment releases the old actor's pin exactly once.
        *one = std::move(*two);
        assert(!host.enter(*current, 1));
        auto moved_actor = host.enter(*current, 0); assert(moved_actor);
        assert(moved_actor->read(0, &a, sizeof(a)) && a == static_cast<std::uint64_t>(iteration + 1));
        one.reset(); two.reset(); moved_actor.reset();
        assert(host.request_stop(*current));
        assert(host.finish_stop(*current));
        assert(!host.enter(*current, 0));
        const auto old = *current;
        current = host.open(); assert(current);
        assert(current->generation > old.generation);
        assert(!host.request_stop(old));
        assert(!host.finish_stop(old));
        auto reset = host.enter(*current, 0); assert(reset);
        std::uint64_t clean = 1;
        assert(reset->read(0, &clean, sizeof(clean)) && clean == 0);
    }
    assert(host.request_stop(*current));
    assert(host.finish_stop(*current));
}

static void concurrent_unique_open() {
    constexpr int kTenants = 16;
    NativeTenantAdmission host(kTenants, 1, 8);
    std::vector<std::optional<NativeTenantAdmission::Handle>> handles(kTenants);
    std::vector<std::thread> threads;
    for (int i = 0; i < kTenants; ++i) {
        threads.emplace_back([&, i] { handles[i] = host.open(); });
    }
    for (auto& t : threads) t.join();
    for (int i = 0; i < kTenants; ++i) {
        assert(handles[i]);
        for (int j = 0; j < i; ++j) assert(handles[i]->slot != handles[j]->slot);
        auto turn = host.enter(*handles[i], 0); assert(turn);
        assert(!host.finish_stop(*handles[i]));
        turn.reset();
        assert(host.request_stop(*handles[i]) && host.finish_stop(*handles[i]));
    }
    assert(host.open());
}

static void concurrent_same_actor_exclusion() {
    NativeTenantAdmission host(1, 1, 16);
    auto handle = host.open(); assert(handle);
    constexpr int kContenders = 12;
    std::atomic<bool> go{false};
    std::atomic<int> admitted{0};
    std::atomic<int> physical_overlap{0};
    std::atomic<int> maximum{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kContenders; ++i) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            auto turn = host.enter(*handle, 0);
            if (turn) {
                admitted.fetch_add(1);
                const int active = physical_overlap.fetch_add(1) + 1;
                int old = maximum.load();
                while (old < active && !maximum.compare_exchange_weak(old, active)) {}
                for (int j = 0; j < 100; ++j) std::this_thread::yield();
                physical_overlap.fetch_sub(1);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    assert(admitted.load() > 0);
    assert(maximum.load() == 1);
    assert(host.request_stop(*handle) && host.finish_stop(*handle));
}

static void race_stop_enter() {
    constexpr int kRounds = 200;
    for (int round = 0; round < kRounds; ++round) {
        NativeTenantAdmission host(1, 1, 16);
        auto tenant = host.open(); assert(tenant);
        std::atomic<bool> begin{false};
        std::atomic<int> entered{0};
        std::thread actor([&] {
            while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
            auto turn = host.enter(*tenant, 0);
            if (turn) {
                entered.fetch_add(1);
                std::uint64_t value = 42;
                assert(turn->write(0, &value, sizeof(value)));
                std::this_thread::yield();
            }
        });
        std::thread stopper([&] {
            while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
            assert(host.request_stop(*tenant));
            assert(!host.enter(*tenant, 0));
        });
        begin.store(true, std::memory_order_release);
        actor.join(); stopper.join();
        assert(entered.load() <= 1);
        assert(host.finish_stop(*tenant));
        assert(!host.enter(*tenant, 0));
        auto replacement = host.open(); assert(replacement);
        auto turn = host.enter(*replacement, 0); assert(turn);
        std::uint64_t clean = 99;
        assert(turn->read(0, &clean, sizeof(clean)) && clean == 0);
        turn.reset();
        assert(host.request_stop(*replacement) && host.finish_stop(*replacement));
    }
}

int main() {
    rejected_sizes();
    stale_and_bounds();
    generations_and_move();
    concurrent_unique_open();
    concurrent_same_actor_exclusion();
    race_stop_enter();
}
