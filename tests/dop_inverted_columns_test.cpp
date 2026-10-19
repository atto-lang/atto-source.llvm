#include "oreslang/runtime/DopInvertedColumns.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>
#include <type_traits>

using Store = oreslang::dop::InvertedColumns<float, std::int64_t, bool>;
static_assert(std::is_same_v<decltype(std::declval<Store::Loan<0, false>>().span()), std::span<const float>>);
static_assert(std::is_same_v<decltype(std::declval<Store::Loan<0, true>>().span()), std::span<float>>);
static_assert(!std::is_copy_constructible_v<Store::Loan<0, true>>);
static_assert(!std::is_copy_constructible_v<Store>);
static void check(bool ok) { if (!ok) throw std::runtime_error("inverted columns assertion"); }
template<class E, class F> static void expect(F f) {
    bool caught = false;
    try { f(); } catch (const E&) { caught = true; }
    check(caught);
}
int main() {
    Store a, b;
    auto h0 = a.push(1.5f, 100, true);
    auto h1 = a.push(2.5f, 200, false);
    auto h2 = a.push(3.5f, 300, true);
    check(a.size() == 3 && a.get<0>(h2) == 3.5f && a.get<1>(h1) == 200);
    a.reserve(16);
    check(a.get_row(h0) == std::make_tuple(1.5f, std::int64_t{100}, true));
    a.set_row(h0, 1.5f, 100, true);
    check(a.get_row(h0) == std::make_tuple(1.5f, std::int64_t{100}, true));
    expect<std::out_of_range>([&] { (void)b.get<0>(h1); });
    {
        auto r = a.borrow_read<0>();
        auto r2 = a.borrow_read<0>();
        auto w = a.borrow_write<1>();
        check(r.span().size() == 3 && r.span()[1] == 2.5f);
        w.span()[1] = 999;
        check(r2.span()[0] == 1.5f);
        expect<std::logic_error>([&] { (void)a.borrow_write<0>(); });
        expect<std::logic_error>([&] { (void)a.borrow_read<1>(); });
        expect<std::logic_error>([&] { (void)a.get_row(h1); });
        expect<std::logic_error>([&] { a.set_row(h0, 6.0f, 9, false); });
        expect<std::logic_error>([&] { a.reserve(100); });
        expect<std::logic_error>([&] { a.set<0>(h0, 4.0f); });
        expect<std::logic_error>([&] { (void)a.push(4.0f, 400, true); });
        expect<std::logic_error>([&] { a.erase_swap(h0); });
    }
    check(a.get<1>(h1) == 999);
    check(a.get<2>(h0) && !a.get<2>(h1));
    { auto bools = a.borrow_read<2>();
      check(bools.span().size() == 3 && bools.span()[0] == 1 && bools.span()[1] == 0); }
    a.erase_swap(h1);
    check(a.size() == 2 && a.get<0>(h2) == 3.5f && a.get<1>(h2) == 300);
    expect<std::out_of_range>([&] { (void)a.get<0>(h1); });
    auto reused = a.push(4.5f, 400, false);
    check(reused.slot == h1.slot && reused.generation != h1.generation);
    expect<std::out_of_range>([&] { a.set<0>(h1, 1.0f); });
    // Disjoint columns can be mutated concurrently without touching layout or identities.
    std::atomic<bool> start{false};
    std::thread t1([&] {
        auto w = a.borrow_write<0>();
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (auto& x : w.span()) x += 1.0f;
    });
    std::thread t2([&] {
        auto w = a.borrow_write<1>();
        start.store(true, std::memory_order_release);
        for (auto& x : w.span()) x += 1;
    });
    t1.join(); t2.join();
    check(a.get<0>(h0) == 2.5f && a.get<1>(h0) == 101);
    // Coherent whole-row snapshots and updates under concurrent access.
    Store row_coherent;
    auto coherent = row_coherent.push(0.0f, 0, true);
    std::atomic<bool> row_bad{false};
    std::thread row_writer([&] {
        for (int n = 0; n < 2000; ++n)
            row_coherent.set_row(coherent, static_cast<float>(n), -static_cast<std::int64_t>(n), (n & 1) == 0);
    });
    std::thread row_reader([&] {
        for (int n = 0; n < 2000; ++n) {
            const auto [x, y, z] = row_coherent.get_row(coherent);
            const auto number = static_cast<std::int64_t>(x);
            if (number != -y || z != ((number & 1) == 0)) row_bad.store(true);
        }
    });
    row_writer.join(); row_reader.join();
    check(!row_bad.load());
    // A moved-out loan retains memory if its owner is destroyed.
    std::optional<Store::Loan<0, false>> carried;
    {
        auto owner = std::make_unique<Store>();
        owner->push(7.5f, 7, true);
        carried.emplace(owner->borrow_read<0>());
        owner.reset();
        expect<std::logic_error>([&] { (void)carried->span(); });
    }
    carried.reset();
    // Owner IDs are shared across distinct template instantiations.
    oreslang::dop::InvertedColumns<std::int64_t> different_fields;
    const auto foreign = different_fields.push(42);
    check(foreign.owner != h0.owner);
    // Saturating exhaustion never wraps the global owner namespace or
    // produces an owner ID already issued to another store.
    auto original_next = oreslang::dop::next_inverted_store_owner.load();
    oreslang::dop::next_inverted_store_owner.store(std::numeric_limits<std::uint64_t>::max());
    expect<std::overflow_error>([] { Store exhausted; (void)exhausted; });
    check(oreslang::dop::next_inverted_store_owner.load() == std::numeric_limits<std::uint64_t>::max());
    oreslang::dop::next_inverted_store_owner.store(original_next);
    // Differential/property-style stress: arbitrary swap-delete order must not
    // change surviving logical rows or revive any stale handle.
    Store fuzz;
    struct Expected { Store::Handle handle; float x; std::int64_t y; bool z; };
    std::vector<Expected> live;
    std::uint64_t rng = 0x6a09e667f3bcc909ULL;
    auto next = [&] { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    for (int step = 0; step < 3000; ++step) {
        if (live.empty() || next() % 4 != 0) {
            const auto x = static_cast<float>(step);
            const auto y = static_cast<std::int64_t>(step) * -3;
            const auto z = (step & 1) == 0;
            live.push_back({fuzz.push(x, y, z), x, y, z});
        } else {
            const auto index = static_cast<std::size_t>(next() % live.size());
            const auto victim = live[index].handle;
            fuzz.erase_swap(victim);
            expect<std::out_of_range>([&] { (void)fuzz.get<0>(victim); });
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
        }
        check(fuzz.size() == live.size());
        for (const auto& expected : live) {
            check(fuzz.get<0>(expected.handle) == expected.x);
            check(fuzz.get<1>(expected.handle) == expected.y);
            check(fuzz.get<2>(expected.handle) == expected.z);
            check(fuzz.get_row(expected.handle) == std::make_tuple(expected.x, expected.y, expected.z));
        }
    }
}
