#include "oreslang/runtime/DopInvertedColumns.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
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
    // A moved-out loan retains memory if its owner is destroyed.
    std::optional<Store::Loan<0, false>> carried;
    {
        auto owner = std::make_unique<Store>();
        owner->push(7.5f, 7, true);
        carried.emplace(owner->borrow_read<0>());
        owner.reset();
        check(carried->span()[0] == 7.5f);
    }
    carried.reset();
}
