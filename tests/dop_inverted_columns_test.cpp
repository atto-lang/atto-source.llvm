#include "oreslang/runtime/DopInvertedColumns.hpp"
#include <atomic>
#include <cstdint>
#include <bit>
#include <cmath>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>
#include <type_traits>
#include <unordered_set>

using Store = oreslang::dop::InvertedColumns<float, std::int64_t, bool>;
static_assert(std::is_same_v<decltype(std::declval<Store::Loan<0, false>>().span()), std::span<const float>>);
static_assert(std::is_same_v<decltype(std::declval<Store::Loan<0, true>>().span()), std::span<float>>);
static_assert(!std::is_copy_constructible_v<Store::Loan<0, true>>);
static_assert(std::is_same_v<decltype(std::declval<Store::Loan<0, false>>().read_at(0)), float>);
static_assert(std::is_same_v<decltype(std::declval<Store::Loan<0, false>>().read_handle(std::declval<const Store::Handle&>())), float>);
static_assert(std::is_same_v<decltype(std::declval<Store::Loan<0, true>>().size_checked()), std::size_t>);
template<class T> concept HasCheckedWrite = requires(T& loan) { loan.write_at(0, 1.0f); };
template<class T> concept HasHandleWrite = requires(T& loan, const Store::Handle& h) { loan.write_handle(h, 1.0f); };
static_assert(!HasCheckedWrite<Store::Loan<0, false>>);
static_assert(HasCheckedWrite<Store::Loan<0, true>>);
static_assert(!HasHandleWrite<Store::Loan<0, false>>);
static_assert(HasHandleWrite<Store::Loan<0, true>>);
static_assert(!std::is_copy_constructible_v<Store>);
static_assert(!std::is_aggregate_v<Store::Handle>);
static_assert(!std::is_default_constructible_v<Store::Handle>);
static_assert(!std::is_constructible_v<Store::Handle, std::uint64_t, std::size_t, std::uint64_t>);
static_assert(!std::is_default_constructible_v<Store::LayoutGuard>);
static_assert(!std::is_aggregate_v<Store::LayoutGuard>);
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
    auto foreign_row = b.push(9.5f, 900, false);
    check(a.size() == 3 && a.get<0>(h2) == 3.5f && a.get<1>(h1) == 200);
    expect<std::out_of_range>([&] { (void)a.index(-1); });
    expect<std::out_of_range>([&] {
        (void)a.index(static_cast<std::int64_t>(
            std::numeric_limits<std::int32_t>::max()) + 1);
    });
    expect<std::out_of_range>([&] { (void)a.index(std::numeric_limits<std::int64_t>::min()); });
    expect<std::out_of_range>([&] { (void)a.index(std::numeric_limits<std::int64_t>::max()); });
    const auto saved_middle = a.index(1);
    check(saved_middle.slot() == h1.slot() && saved_middle.generation() == h1.generation());
    check(a.read_index<0>(0) == 1.5f);
    check(a.read_index<1>(1) == 200);

    // Structural epoch is a checked, unforgeable guard for compiler/JIT
    // specializations, not an unchecked physical row pointer. Validation
    // and the field operation happen under the same storage lock.
    {
        Store guarded;
        auto first = guarded.push(2.5f, 5, true);
        auto initial = guarded.capture_layout_guard();
        check(guarded.read_index_guarded<0>(initial, 0) == 2.5f);
        guarded.write_index_guarded<0>(initial, 0, 7.5f);
        check(guarded.get<0>(first) == 7.5f);
        expect<std::logic_error>([&] {
            (void)a.read_index_guarded<0>(initial, 0);
        });
        expect<std::logic_error>([&] {
            a.write_index_guarded<0>(initial, 0, 99.0f);
        });
        {
            auto borrowed = guarded.borrow_read<0>();
            check(guarded.read_index_guarded<0>(initial, 0) == 7.5f);
            expect<std::logic_error>([&] {
                guarded.write_index_guarded<0>(initial, 0, 99.0f);
            });
        }
        guarded.reserve(32);
        expect<std::logic_error>([&] {
            (void)guarded.read_index_guarded<0>(initial, 0);
        });
        expect<std::logic_error>([&] {
            guarded.write_index_guarded<0>(initial, 0, 99.0f);
        });
        check(guarded.get<0>(first) == 7.5f); // failure has no side effects
        auto after_reserve = guarded.capture_layout_guard();
        check(after_reserve.epoch() > initial.epoch());
        // A redundant reserve must not invalidate a guard: no pointer,
        // column, slot, or row metadata is relocated.
        guarded.reserve(32);
        check(guarded.capture_layout_guard().epoch() == after_reserve.epoch());
        check(guarded.read_index_guarded<0>(after_reserve, 0) == 7.5f);
        guarded.set<0>(first, 8.5f); // value updates do not change layout
        check(guarded.read_index_guarded<0>(after_reserve, 0) == 8.5f);
        // Even an allocation failure can partially change vector capacities.
        // Guards therefore invalidate *before* the first reserve attempt.
        bool failed_reserve = false;
        try { guarded.reserve(std::numeric_limits<std::size_t>::max()); }
        catch (const std::length_error&) { failed_reserve = true; }
        catch (const std::bad_alloc&) { failed_reserve = true; }
        check(failed_reserve && guarded.get<0>(first) == 8.5f);
        expect<std::logic_error>([&] {
            (void)guarded.read_index_guarded<0>(after_reserve, 0);
        });
        auto after_failed_reserve = guarded.capture_layout_guard();
        check(after_failed_reserve.epoch() > after_reserve.epoch());
        auto second = guarded.push(3.5f, 8, false);
        expect<std::logic_error>([&] {
            (void)guarded.read_index_guarded<0>(after_failed_reserve, 0);
        });
        auto after_push = guarded.capture_layout_guard();
        guarded.erase_swap(first);
        expect<std::logic_error>([&] {
            (void)guarded.read_index_guarded<0>(after_push, 0);
        });
        auto after_erase = guarded.capture_layout_guard();
        check(guarded.read_index_guarded<0>(after_erase, 0) == 3.5f);
        check(guarded.get<0>(second) == 3.5f);
        expect<std::out_of_range>([&] { (void)guarded.get<0>(first); });
        expect<std::out_of_range>([&] {
            (void)guarded.read_index_guarded<0>(after_erase, 1);
        });
    }

    std::vector<int> lowering_order;
    int receiver_evals = 0, index_evals = 0, value_evals = 0;
    const auto lowered_read = Store::lower_read<0>(
        [&]() -> Store& {
            ++receiver_evals;
            lowering_order.push_back(1);
            return a;
        },
        [&]() -> std::int64_t {
            ++index_evals;
            lowering_order.push_back(2);
            return 1;
        });
    check(lowered_read == 2.5f);
    check(receiver_evals == 1 && index_evals == 1);
    check(lowering_order == std::vector<int>({1, 2}));

    lowering_order.clear();
    const auto lowered_write = Store::lower_write<0>(
        [&]() -> float {
            ++value_evals;
            lowering_order.push_back(3);
            return 9.5f;
        },
        [&]() -> Store& {
            ++receiver_evals;
            lowering_order.push_back(1);
            return a;
        },
        [&]() -> std::int64_t {
            ++index_evals;
            lowering_order.push_back(2);
            return 0;
        });
    check(lowered_write == 9.5f && a.read_index<0>(0) == 9.5f);
    check(value_evals == 1 && receiver_evals == 2 && index_evals == 2);
    check(lowering_order == std::vector<int>({3, 1, 2}));
    // Restore the pre-existing fixture value so later legacy assertions remain independent.
    a.write_index<0>(0, 1.5f);
    a.reserve(16);
    // Deterministic oversized reservation must fail without changing any
    // live row/handle identity or the lengths of individual columns.
    bool rejected_huge_reservation = false;
    try { a.reserve(std::numeric_limits<std::size_t>::max()); }
    catch (const std::length_error&) { rejected_huge_reservation = true; }
    catch (const std::bad_alloc&) { rejected_huge_reservation = true; }
    check(rejected_huge_reservation && a.size() == 3);
    check(a.get_row(h0) == std::make_tuple(1.5f, std::int64_t{100}, true));
    a.set_row(h0, 1.5f, 100, true);
    check(a.get_row(h0) == std::make_tuple(1.5f, std::int64_t{100}, true));
    expect<std::out_of_range>([&] { (void)b.get<0>(h1); });
    {
        auto r = a.borrow_read<0>();
        auto r2 = a.borrow_read<0>();
        auto w = a.borrow_write<1>();
        check(r.span().size() == 3 && r.span()[1] == 2.5f);
        check(r.size_checked() == 3 && r.read_at(1) == 2.5f);
        check(r.read_handle(h1) == 2.5f);
        expect<std::out_of_range>([&] { (void)r.read_at(3); });
        expect<std::out_of_range>([&] { (void)r.read_handle(foreign_row); });
        expect<std::out_of_range>([&] { w.write_at(100, 123); });
        expect<std::out_of_range>([&] { w.write_handle(foreign_row, 123); });
        w.write_handle(h1, 999);
        check(w.read_handle(h1) == 999);
        check(r2.span()[0] == 1.5f);
        expect<std::logic_error>([&] { (void)a.borrow_write<0>(); });
        expect<std::logic_error>([&] { (void)a.borrow_read<1>(); });
        expect<std::logic_error>([&] { (void)a.get_row(h1); });
        expect<std::logic_error>([&] { (void)a.read_index<1>(1); });
        expect<std::logic_error>([&] { a.write_index<0>(0, 9.0f); });
        expect<std::logic_error>([&] { a.set_row(h0, 6.0f, 9, false); });
        expect<std::logic_error>([&] { a.reserve(100); });
        expect<std::logic_error>([&] { a.set<0>(h0, 4.0f); });
        expect<std::logic_error>([&] { (void)a.push(4.0f, 400, true); });
        expect<std::logic_error>([&] { a.erase_swap(h0); });
    }
    check(a.get<1>(h1) == 999);
    {
        auto loan = a.borrow_read<0>();
        check(loan.read_at(0) == 1.5f);
        loan.release();
        expect<std::logic_error>([&] { (void)loan.read_at(0); });
        expect<std::logic_error>([&] { (void)loan.size_checked(); });
    }
    check(a.get<2>(h0) && !a.get<2>(h1));
    { auto bools = a.borrow_read<2>();
      check(bools.span().size() == 3 && bools.span()[0] == 1 && bools.span()[1] == 0); }
    a.erase_swap(h1);
    check(a.size() == 2 && a.get<0>(h2) == 3.5f && a.get<1>(h2) == 300);
    // Dense position changed by swap-delete, but the surviving logical
    // generation-checked handle still identifies the same row.
    const auto moved_dense = a.index(1);
    check(moved_dense.slot() == h2.slot() && moved_dense.generation() == h2.generation());
    check(a.read_index<0>(1) == 3.5f && a.read_index<1>(1) == 300);
    expect<std::out_of_range>([&] { (void)a.get<0>(h1); });
    auto reused = a.push(4.5f, 400, false);
    check(reused.slot() == h1.slot() && reused.generation() != h1.generation());
    expect<std::out_of_range>([&] { a.set<0>(h1, 1.0f); });
    {
        auto checked = a.borrow_read<0>();
        expect<std::out_of_range>([&] { (void)checked.read_handle(h1); });
        check(checked.read_handle(reused) == 4.5f);
    }
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

    // Fixed-width SoA scalar columns must preserve IEEE signed zero and
    // non-finite classification. No unchecked fast-math or implicit
    // numeric narrowing is authorized by the borrow/layout reference.
    {
        Store ieee;
        auto zero = ieee.push(-0.0f, -7, false);
        check(std::bit_cast<std::uint32_t>(ieee.get<0>(zero)) ==
              std::bit_cast<std::uint32_t>(-0.0f));
        {
            auto writable = ieee.borrow_write<0>();
            writable.span()[0] = std::numeric_limits<float>::quiet_NaN();
            check(std::isnan(writable.span()[0]));
            expect<std::logic_error>([&] { ieee.erase_swap(zero); });
        }
        check(std::isnan(ieee.get<0>(zero)));
        ieee.set<0>(zero, -0.0f);
        check(std::bit_cast<std::uint32_t>(ieee.get<0>(zero)) ==
              std::bit_cast<std::uint32_t>(-0.0f));
        ieee.erase_swap(zero);
        expect<std::out_of_range>([&] { (void)ieee.get<0>(zero); });
    }
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
    std::optional<Store::Loan<0, false>> carried2;
    {
        auto owner = std::make_unique<Store>();
        auto retired_handle = owner->push(7.5f, 7, true);
        carried.emplace(owner->borrow_read<0>());
        carried2.emplace(owner->borrow_read<0>());
        // This raw span is HOST-ONLY and never a legal guest pointer.
        // Owner teardown revokes all new checked reads immediately, but
        // cannot scrub an existing host pointer while a loan is still live:
        // doing so would race an external reader of that span.
        auto retained = carried->span();
        check(retained.size() == 1 && retained[0] == 7.5f);
        owner.reset();
        check(retained[0] == 7.5f);
        carried2.reset();  // not the final loan; backing remains intact
        check(retained[0] == 7.5f);
        expect<std::logic_error>([&] { (void)carried->read_handle(retired_handle); });
        expect<std::logic_error>([&] { (void)carried->span(); });
        expect<std::logic_error>([&] { (void)carried->read_at(0); });
        expect<std::logic_error>([&] { (void)carried->size_checked(); });
    }
    // Last loan release wipes the retired columns and metadata and frees the
    // shared state. No host span can be used after its last loan is released.
    carried.reset();
    // Owner IDs are shared across distinct template instantiations.
    oreslang::dop::InvertedColumns<std::int64_t> different_fields;
    const auto foreign = different_fields.push(42);
    check(foreign.owner() != h0.owner());
    // Owner IDs stay process-wide and unique across independent stores and
    // distinct field packs without exposing a host-mutable sequence.
    std::unordered_set<std::uint64_t> owners;
    owners.insert(h0.owner());
    owners.insert(foreign.owner());
    for (int i = 0; i < 4096; ++i) {
        auto extra = std::make_unique<oreslang::dop::InvertedColumns<std::uint32_t>>();
        auto handle = extra->push(static_cast<std::uint32_t>(i));
        check(owners.insert(handle.owner()).second);
    }
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
