#include "oreslang/runtime/NativeEtsScalarSet.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using Set = oreslang::runtime::NativeEtsScalarSet;
static void check(bool ok) { if (!ok) throw std::runtime_error("ETS versioned CAS failed"); }

int main() {
    const Set::Principal owner{5, 7, 10, 1};
    const Set::Principal peer{5, 7, 11, 1};
    const Set::Principal foreign{5, 8, 11, 1};
    const auto ok = Set::Status::ok;
    Set t(owner, Set::Protection::public_table, 2);
    const auto h = t.handle();
    check(t.insert(h, owner, 1, 44) == ok);
    const auto original = t.lookup_versioned(h, owner, 1);
    check(original.status == ok && original.value == 44 && original.version != 0);
    check(t.erase(h, owner, 1) == ok);
    check(t.insert(h, owner, 1, 44) == ok); // same value, different generation
    const auto current = t.lookup_versioned(h, peer, 1);
    check(current.version > original.version);
    check(t.compare_exchange_versioned(h, peer, 1, 44, original.version, 99)
          == Set::Status::conflict);
    check(t.compare_exchange_versioned(h, peer, 1, 43, current.version, 99)
          == Set::Status::conflict);
    check(t.compare_exchange_versioned(h, peer, 1, 44, 0, 99)
          == Set::Status::invalid);
    check(t.compare_exchange_versioned(h, foreign, 1, 44, current.version, 99)
          == Set::Status::denied);
    check(t.lookup_versioned(h, owner, 1).value == 44);
    check(t.compare_exchange_versioned(h, peer, 1, 44, current.version, 99) == ok);
    const auto after = t.lookup_versioned(h, owner, 1);
    check(after.value == 99 && after.version > current.version);

    // Reusing a vacant table slot for a DIFFERENT key must not reissue a
    // historical per-key CAS generation when the original key returns.
    check(t.erase(h, owner, 1) == ok);
    check(t.insert(h, owner, 2, 88) == ok);
    check(t.erase(h, owner, 2) == ok);
    check(t.insert(h, owner, 1, 99) == ok);
    const auto reused = t.lookup_versioned(h, owner, 1);
    check(reused.version > after.version);
    check(t.compare_exchange_versioned(h, owner, 1, 99, after.version, 23)
          == Set::Status::conflict);

    // Concurrent CAS retry must linearize all increments; conflicts may
    // occur, but no accepted update may disappear or reapply twice.
    check(t.insert(h, owner, 1, 0) == ok);
    constexpr int workers=6, increments=500;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int i=0; i<workers; ++i) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int j=0; j<increments; ++j) {
                for (;;) {
                    const auto observation = t.lookup_versioned(h, peer, 1);
                    if (observation.status != ok) std::terminate();
                    const auto outcome = t.compare_exchange_versioned(
                        h, peer, 1, observation.value, observation.version,
                        observation.value+1);
                    if (outcome == ok) break;
                    if (outcome != Set::Status::conflict) std::terminate();
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    check(t.lookup(h, owner, 1).value == workers*increments);
    const auto terminal = t.lookup_versioned(h, owner, 1);
    check(terminal.version > reused.version);
    check(t.retire_owner(owner));
    check(t.lookup_versioned(h, owner, 1).status == Set::Status::retired);
    check(t.compare_exchange_versioned(h, owner, 1, 0, terminal.version, 1)
          == Set::Status::retired);
    Set replacement(owner, Set::Protection::public_table, 1);
    check(replacement.lookup_versioned(h, owner, 1).status == Set::Status::invalid);
}
