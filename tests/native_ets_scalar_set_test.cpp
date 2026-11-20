#include "oreslang/runtime/NativeEtsScalarSet.hpp"
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using Set = oreslang::runtime::NativeEtsScalarSet;
static void require(bool ok) { if (!ok) throw std::runtime_error("ETS proof failed"); }

int main() {
    const Set::Principal owner{1, 100, 5, 1};
    const Set::Principal peer{1, 100, 6, 1};
    const Set::Principal stale{1, 100, 5, 2};
    const Set::Principal foreign_isolate{1, 101, 6, 1};
    const Set::Principal foreign_vm{2, 100, 6, 1};
    const auto P = Set::Protection::private_table;
    const auto R = Set::Protection::protected_table;
    const auto U = Set::Protection::public_table;
    const auto ok = Set::Status::ok;
    const auto denied = Set::Status::denied;
    for (auto mode : {P, R, U}) {
        Set set(owner, mode, 2);
        auto h = set.handle();
        require(set.insert(h, owner, 0, 42) == ok);
        require(set.insert(h, owner, 0, 43, true) == Set::Status::exists);
        require(set.lookup(h, owner, 0).value == 42);
        require(set.insert(h, owner, 7, 0) == ok);
        require(set.insert(h, owner, 8, 19) == Set::Status::full);
        require(set.lookup(h, owner, 7).status == ok);
        require(set.lookup(h, owner, 7).value == 0);
        require(set.lookup(h, owner, 8).status == Set::Status::absent);
        require(set.insert({0}, owner, 9, 11) == Set::Status::invalid);
        require(set.insert(h, foreign_isolate, 7, 1) == denied);
        require(set.lookup(h, foreign_vm, 7).status == denied);
        require(set.lookup(h, peer, 0).status == (mode == P ? denied : ok));
        require(set.insert(h, peer, 0, 99) == (mode == U ? ok : denied));
        require(set.insert(h, stale, 0, 88) == (mode == U ? ok : denied));
        require(set.erase(h, owner, 7) == ok);
        require(set.insert(h, owner, 8, 11) == ok);
        require(set.retire_owner(owner));
        require(!set.retire_owner(owner));
        require(set.lookup(h, owner, 0).status == Set::Status::retired);
        require(set.insert(h, owner, 0, 1) == Set::Status::retired);
        Set replacement(owner, mode, 2);
        require(replacement.handle().incarnation != h.incarnation);
        require(replacement.insert(h, owner, 1, 1) == Set::Status::invalid);
        require(!replacement.retire_owner(peer));
    }
    {
        Set table(owner, U, 1);
        auto h = table.handle();
        require(table.insert(h, owner, 1, 0) == ok);
        constexpr int n = 8;
        constexpr int steps = 3000;
        std::vector<std::thread> writers;
        for (int i = 0; i < n; ++i) {
            writers.emplace_back([&] {
                for (int k = 0; k < steps; ++k)
                    if (table.increment(h, peer, 1, 1).status != ok)
                        std::terminate();
            });
        }
        for (auto& writer : writers) writer.join();
        require(table.lookup(h, owner, 1).value == n * steps);
        require(table.insert(h, owner, 1, std::numeric_limits<std::uint64_t>::max()) == ok);
        require(table.increment(h, owner, 1, 1).status == Set::Status::overflow);
        require(table.lookup(h, owner, 1).value == std::numeric_limits<std::uint64_t>::max());
    }
    bool rejected = false;
    try { Set invalid(owner, P, 0); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);
}
