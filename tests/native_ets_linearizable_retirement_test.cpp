#include "oreslang/runtime/NativeEtsScalarSet.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using Set = oreslang::runtime::NativeEtsScalarSet;
static void require(bool ok) {
    if (!ok) throw std::runtime_error("ETS linearization check failed");
}

int main() {
    const Set::Principal owner{4, 7, 31, 5};
    const Set::Principal peer{4, 7, 32, 1};
    const Set::Principal cross_isolate{4, 8, 32, 1};
    const Set::Principal old_owner{4, 7, 31, 4};
    const auto ok = Set::Status::ok;
    {
        // Exactly one creator may win insert_new on the same key; losers
        // observe exists rather than overwriting the stored value.
        Set table(owner, Set::Protection::public_table, 1);
        const auto h = table.handle();
        constexpr int n = 12;
        std::atomic<bool> start{false};
        std::atomic<int> won{0}, lost{0};
        std::vector<std::thread> workers;
        for (int i = 0; i < n; ++i) {
            workers.emplace_back([&, i] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                auto status = table.insert(h, peer, 0, (std::uint64_t)i + 1, true);
                if (status == ok) won.fetch_add(1);
                else if (status == Set::Status::exists) lost.fetch_add(1);
                else std::terminate();
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& w : workers) w.join();
        require(won.load() == 1 && lost.load() == n - 1);
        auto value = table.lookup(h, owner, 0);
        require(value.status == ok && value.value >= 1 && value.value <= n);
        require(table.erase(h, peer, 0) == ok);
        require(table.erase(h, peer, 0) == Set::Status::absent);
        require(table.insert(h, owner, 99, 42) == ok);
        require(table.insert(h, peer, 100, 1) == Set::Status::full);
        require(table.lookup(h, cross_isolate, 99).status == Set::Status::denied);
        require(!table.retire_owner(old_owner));
        require(table.lookup(h, owner, 99).value == 42);
    }
    {
        // The retirement lock is the linearization point: once the owner
        // has retired a table, no future increment/insert/lookup may pass.
        Set table(owner, Set::Protection::public_table, 1);
        const auto h = table.handle();
        require(table.insert(h, owner, 5, 0) == ok);
        std::atomic<bool> start{false};
        std::atomic<std::uint64_t> successful{0};
        std::atomic<int> observed_retired{0};
        std::vector<std::thread> writers;
        for (int t = 0; t < 8; ++t) {
            writers.emplace_back([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (int j = 0; j < 20000; ++j) {
                    auto outcome = table.increment(h, peer, 5, 1);
                    if (outcome.status == ok) successful.fetch_add(1);
                    else if (outcome.status == Set::Status::retired) {
                        observed_retired.fetch_add(1);
                        break;
                    } else std::terminate();
                }
            });
        }
        start.store(true, std::memory_order_release);
        require(table.retire_owner(owner));
        for (auto& w : writers) w.join();
        require(table.increment(h, peer, 5, 1).status == Set::Status::retired);
        require(table.lookup(h, owner, 5).status == Set::Status::retired);
        require(table.erase(h, owner, 5) == Set::Status::retired);
        require(table.insert(h, owner, 1, 1) == Set::Status::retired);
        require(!table.retire_owner(owner));
        require(successful.load() <= 8u * 20000u);
        require(observed_retired.load() <= 8);
        // Heap storage belongs to the table and is retired exactly once;
        // stale handle cannot authenticate the newly created table.
        Set replacement(owner, Set::Protection::private_table, 1);
        require(replacement.handle().incarnation != h.incarnation);
        require(replacement.insert(h, owner, 5, 99) == Set::Status::invalid);
        require(replacement.insert(replacement.handle(), old_owner, 5, 99)
                == Set::Status::denied);
    }
    {
        // Even an invalid size must fail before trying to provision a huge
        // vector. Validate the constructor's checked-capacity ordering.
        bool rejected = false;
        try { Set oversized(owner, Set::Protection::public_table, 1048577); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected);
        rejected = false;
        try { Set bad(owner, static_cast<Set::Protection>(255), 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected);
    }
}
