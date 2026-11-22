// Deterministic model-based test: host-only inverted-class capability oracle.
// Tests identities and rows, not .atto source or production actor scheduling.
#include "InvertedClassHandleContract.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

using Table = oreslang::dop::contract::InvertedClassHandleContract<8>;
struct Tracked {
    Table::Handle handle;
    Table::Value value;
};
static std::uint64_t seed = UINT64_C(0xA770BEEF35C1);
static std::uint64_t next() {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}
static void check(bool result) {
    if (!result) throw std::runtime_error("inverted class model divergence");
}
static void must_reject(Table& table, const Table::Handle& handle) {
    try {
        (void)table.snapshot(handle);
    } catch (const std::out_of_range&) {
        return;
    }
    throw std::runtime_error("stale handle unexpectedly resolved");
}
int main() {
    Table table(2011);
    std::vector<Tracked> live;
    std::vector<Table::Handle> stale;
    for (std::size_t step = 0; step < 50000; ++step) {
        const auto op = next() % 6;
        if (live.empty() || (op < 2 && live.size() < 8)) {
            const auto x = static_cast<std::int64_t>(next() % 100000);
            const auto y = static_cast<std::int64_t>(next() % 100000);
            auto handle = table.push(x, y);
            live.push_back(Tracked{handle, Table::Value{x, y}});
        } else if (op == 2 || op == 3) {
            const auto index = static_cast<std::size_t>(next() % live.size());
            const auto dx = static_cast<std::int64_t>(next() % 21) - 10;
            const auto dy = static_cast<std::int64_t>(next() % 21) - 10;
            table.move_by(live[index].handle, dx, dy);
            live[index].value.x += dx;
            live[index].value.y += dy;
        } else if (op == 4) {
            const auto index = static_cast<std::size_t>(next() % live.size());
            stale.push_back(live[index].handle);
            table.erase_swap(live[index].handle);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
            must_reject(table, stale.back());
        } else {
            const auto index = static_cast<std::size_t>(next() % live.size());
            const auto snapshot = table.snapshot(live[index].handle);
            check(snapshot.x == live[index].value.x &&
                  snapshot.y == live[index].value.y);
        }
        check(table.size() == live.size());
        for (const auto& item : live) {
            const auto actual = table.snapshot(item.handle);
            check(actual.x == item.value.x && actual.y == item.value.y);
        }
        if (!stale.empty() && step % 17 == 0)
            must_reject(table, stale[static_cast<std::size_t>(next() % stale.size())]);
    }
    for (const auto& handle : stale) must_reject(table, handle);
    const auto before = live.empty() ? Table::Value{0, 0} : table.snapshot(live[0].handle);
    table.close();
    for (const auto& item : live) {
        try { (void)table.snapshot(item.handle); }
        catch (const std::logic_error&) { continue; }
        throw std::runtime_error("closed owner admitted live handle");
    }
    (void)before;
    return 0;
}
