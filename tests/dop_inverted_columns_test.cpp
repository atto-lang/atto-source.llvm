#include "oreslang/runtime/DopInvertedColumns.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>
#include <type_traits>
#include <unordered_set>

using Store = oreslang::dop::InvertedColumns<float, std::int64_t, bool>;
using PackedPosition = oreslang::dop::FixedRecord<bool, double, double>;
using RecordStore = oreslang::dop::InvertedColumns<PackedPosition, double>;
static_assert(PackedPosition::encoded_size == 17);
static_assert(sizeof(PackedPosition) == PackedPosition::encoded_size);
static_assert(alignof(PackedPosition) == 1);
static_assert(std::is_trivially_copyable_v<PackedPosition>);
static_assert(std::is_standard_layout_v<PackedPosition>);
template<class T>
concept ExposesRawSpan = requires(T& loan) { loan.span(); };
static_assert(!ExposesRawSpan<RecordStore::Loan<0, false>>);
static_assert(!ExposesRawSpan<Store::Loan<0, false>>);
static_assert(!ExposesRawSpan<Store::Loan<0, true>>);
static_assert(std::is_same_v<
        decltype(std::declval<RecordStore::Loan<0, false>>().get(std::size_t{})),
        PackedPosition>);
static_assert(std::is_same_v<
        decltype(std::declval<Store::Loan<0, false>>().get(std::size_t{})),
        float>);
static_assert(!std::is_copy_constructible_v<Store::Loan<0, true>>);
static_assert(!std::is_copy_constructible_v<Store>);
static_assert(!std::is_aggregate_v<Store::Handle>);
static_assert(!std::is_default_constructible_v<Store::Handle>);
static_assert(!std::is_constructible_v<Store::Handle, std::uint64_t, std::size_t, std::uint64_t>);
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
        check(r.size() == 3 && r.get(1) == 2.5f);
        w.set(1, 999);
        check(r2.get(0) == 1.5f);
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
      check(bools.size() == 3 && bools.get(0) == 1 && bools.get(1) == 0); }
    a.erase_swap(h1);
    check(a.size() == 2 && a.get<0>(h2) == 3.5f && a.get<1>(h2) == 300);
    check(a.read_index<0>(1) == 3.5f);
    expect<std::out_of_range>([&] { (void)a.read_index<0>(2); });
    expect<std::out_of_range>([&] { (void)a.get<0>(h1); });
    check(a.index(1).slot() == h2.slot());
    expect<std::out_of_range>([&] { (void)a.get<0>(saved_middle); });
    auto reused = a.push(4.5f, 400, false);
    check(reused.slot() == h1.slot() && reused.generation() != h1.generation());
    check(a.index(2).slot() == reused.slot());
    expect<std::out_of_range>([&] { (void)a.get<0>(saved_middle); });
    expect<std::out_of_range>([&] { (void)a.index(3); });
    expect<std::out_of_range>([&] { a.set<0>(h1, 1.0f); });
    // Disjoint columns can be mutated concurrently without touching layout or identities.
    std::atomic<bool> start{false};
    std::thread t1([&] {
        auto w = a.borrow_write<0>();
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::size_t i = 0; i < w.size(); ++i) w.set(i, w.get(i) + 1.0f);
    });
    std::thread t2([&] {
        auto w = a.borrow_write<1>();
        start.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < w.size(); ++i) w.set(i, w.get(i) + 1);
    });
    t1.join(); t2.join();
    check(a.get<0>(h0) == 2.5f && a.get<1>(h0) == 101);

    // One-level inversion: a struct-valued field is one opaque fixed-record
    // column cell. Member template order is compiler-canonical member-name
    // order; this fixture models {alive: bool, x: f64, y: f64}.
    {
        const auto position = PackedPosition::pack(true, 1.0, 2.0);
        const std::array<std::uint8_t, 17> expected{
            1,
            0, 0, 0, 0, 0, 0, 240, 63,
            0, 0, 0, 0, 0, 0, 0, 64
        };
        auto encoded = position.bytes();
        check(std::equal(encoded.begin(), encoded.end(), expected.begin()));
        encoded[0] = 0;
        check(position.get<0>());
        check(position.get<1>() == 1.0 && position.get<2>() == 2.0);
        check(PackedPosition::from_bytes(expected) == position);

        auto bad = expected;
        bad[0] = 2;
        expect<std::invalid_argument>([&] {
            (void)PackedPosition::from_bytes(bad);
        });
        expect<std::invalid_argument>([&] {
            const std::array<std::uint8_t, 1> short_payload{0};
            (void)PackedPosition::from_bytes(short_payload);
        });

        // FixedRecord is intentionally trivially copyable for packed storage,
        // so host memcpy can manufacture an object whose private bytes were
        // never validated. Every typed store write boundary must revalidate it.
        PackedPosition forged{};
        auto forged_bytes = expected;
        forged_bytes[0] = 2;
        std::memcpy(&forged, forged_bytes.data(), forged_bytes.size());
        expect<std::invalid_argument>([&] { forged.validate_canonical(); });
        {
            RecordStore guarded;
            expect<std::invalid_argument>([&] { (void)guarded.push(forged, 1.0); });
            const auto valid = guarded.push(position, 1.0);
            expect<std::invalid_argument>([&] { guarded.set<0>(valid, forged); });
            expect<std::invalid_argument>([&] { guarded.set_row(valid, forged, 2.0); });
            check(guarded.get<0>(valid) == position && guarded.get<1>(valid) == 1.0);
        }

        RecordStore records;
        const auto h = records.push(position, 5.0);
        const auto h2 = records.push(PackedPosition::pack(false, 6.0, 7.0), 8.0);
        check(records.get<0>(h) == position && records.get<1>(h) == 5.0);
        {
            auto read = records.borrow_read<0>();
            check(read.size() == 2);
            check(read.get(1).get<0>() == false);
            check(read.get(1).get<1>() == 6.0);
        }
        const auto replacement = PackedPosition::pack(false, -0.0, 9.0);
        records.set<0>(h, replacement);
        check(records.get<0>(h) == replacement);
        check(std::bit_cast<std::uint64_t>(records.get<0>(h).get<1>())
              == std::bit_cast<std::uint64_t>(-0.0));
        records.erase_swap(h);
        expect<std::out_of_range>([&] { (void)records.get<0>(h); });
        check(records.get<1>(h2) == 8.0);
    }

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
            writable.set(0, std::numeric_limits<float>::quiet_NaN());
            check(std::isnan(writable.get(0)));
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
    {
        auto owner = std::make_unique<Store>();
        owner->push(7.5f, 7, true);
        carried.emplace(owner->borrow_read<0>());
        owner.reset();
        expect<std::logic_error>([&] { (void)carried->size(); });
    }
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
        auto extra = std::make_unique<oreslang::dop::InvertedColumns<std::int32_t>>();
        auto handle = extra->push(static_cast<std::int32_t>(i));
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
