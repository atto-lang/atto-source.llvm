#include "oreslang/runtime/DopCheckedRegion.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("requirement failed: " #x); } while (false)

using namespace oreslang::dop;

template<typename Exception, typename Fn>
void rejects(Fn&& fn) {
    bool rejected = false;
    try { fn(); } catch (const Exception&) { rejected = true; }
    REQUIRE(rejected);
}

static_assert(!std::is_default_constructible_v<CheckedRegion::RowHandle>);
static_assert(!std::is_constructible_v<CheckedRegion::RowHandle,
              std::uint64_t, std::uint64_t, std::uint64_t>);
static_assert(!std::is_copy_constructible_v<CheckedRegion>);
static_assert(!std::is_move_constructible_v<CheckedRegion>);

void matrix() {
    for (auto layout : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        for (auto lanes : {1U, 2U, 4U, 8U}) {
            if (layout != Layout::AoSoA && lanes != 1) continue;
            for (auto rows : {0ULL, 1ULL, 3ULL, 7ULL, 17ULL, 128ULL}) {
                Plan plan(layout, rows, 3, 4, 8, lanes);
                CheckedRegion store(plan);
                REQUIRE(store.layout() == layout);
                REQUIRE(store.storage_bytes() == plan.byte_length());
                rejects<std::out_of_range>([&] { (void)store.row(rows); });
                for (std::uint64_t row = 0; row < rows; row++) {
                    auto handle = store.row(row);
                    for (std::uint32_t field = 0; field < 3; field++) {
                        auto value = static_cast<std::uint32_t>(0x100000U + row * 113 + field * 331);
                        store.write(handle, field, value);
                    }
                }
                for (std::uint64_t row = 0; row < rows; row++) {
                    auto handle = store.row(row);
                    for (std::uint32_t field = 0; field < 3; field++) {
                        auto expected = static_cast<std::uint32_t>(0x100000U + row * 113 + field * 331);
                        REQUIRE(store.read<std::uint32_t>(handle, field) == expected);
                    }
                    rejects<std::out_of_range>([&] { (void)store.read<std::uint32_t>(handle, 3); });
                    rejects<std::invalid_argument>([&] { (void)store.read<std::uint64_t>(handle, 0); });
                }
            }
        }
    }
}

void migration_and_identity() {
    const auto source = Plan(Layout::AoS, 17, 4, 4, 8, 1);
    CheckedRegion store(source);
    CheckedRegion other(source);
    auto wrong = other.row(0);
    auto old = store.row(0);
    rejects<std::invalid_argument>([&] { (void)store.read<std::uint32_t>(wrong, 0); });
    for (std::uint64_t r = 0; r < source.rows(); r++)
        for (std::uint32_t f = 0; f < source.fields(); f++)
            store.write(store.row(r), f, std::uint32_t(r * 65537 + f));

    // All six directed layout transitions preserve logical rows/columns.
    for (auto layout : {Layout::SoA, Layout::AoSoA, Layout::AoS,
                        Layout::AoSoA, Layout::SoA, Layout::AoS}) {
        const auto last = store.row(16);
        const auto epoch = store.epoch();
        const Plan next(layout, 17, 4, 4, 16, layout == Layout::AoSoA ? 8U : 1U);
        store.relayout(next);
        REQUIRE(store.epoch() == epoch + 1);
        rejects<std::invalid_argument>([&] { (void)store.read<std::uint32_t>(last, 0); });
        for (std::uint64_t r = 0; r < source.rows(); r++)
            for (std::uint32_t f = 0; f < source.fields(); f++)
                REQUIRE(store.read<std::uint32_t>(store.row(r), f) == std::uint32_t(r * 65537 + f));
    }
    rejects<std::invalid_argument>([&] { (void)store.read<std::uint32_t>(old, 0); });
    const auto live = store.row(0);
    const auto epoch = store.epoch();
    rejects<std::invalid_argument>([&] { store.relayout(Plan(Layout::AoS, 18, 4, 4, 8, 1)); });
    rejects<std::invalid_argument>([&] { store.relayout(Plan(Layout::AoS, 17, 5, 4, 8, 1)); });
    rejects<std::invalid_argument>([&] { store.relayout(Plan(Layout::AoS, 17, 4, 8, 8, 1)); });
    REQUIRE(store.epoch() == epoch);
    REQUIRE(store.read<std::uint32_t>(live, 0) == 0);
}

void float_bits_and_overflows() {
    CheckedRegion floats(Plan(Layout::AoSoA, 5, 2, 4, 8, 4));
    const std::uint32_t bits[] = {0x80000000U, 0x7fc00042U, 0x7f800000U, 0xff800000U, 0U};
    for (std::uint64_t r = 0; r < 5; r++) {
        float payload;
        std::memcpy(&payload, &bits[r], sizeof(payload));
        floats.write(floats.row(r), 0, payload);
    }
    floats.relayout(Plan(Layout::SoA, 5, 2, 4, 8, 1));
    for (std::uint64_t r = 0; r < 5; r++) {
        const auto payload = floats.read<float>(floats.row(r), 0);
        std::uint32_t actual;
        std::memcpy(&actual, &payload, sizeof(actual));
        REQUIRE(actual == bits[r]);
    }
    rejects<std::overflow_error>([] { (void)Plan(Layout::AoS,
        std::numeric_limits<std::uint64_t>::max(), 10, 8, 8, 1); });
}

enum class Facing : std::uint16_t { East = 0x12ab, West = 0x712b };

void scalar_abi_matrix() {
    for (auto layout : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        const auto lanes = layout == Layout::AoSoA ? 4U : 1U;
        CheckedRegion bytes(Plan(layout, 7, 2, 1, 1, lanes));
        bytes.write<std::uint8_t>(bytes.row(6), 1, 0xd3U);
        REQUIRE(bytes.read<std::uint8_t>(bytes.row(6), 1) == 0xd3U);
        CheckedRegion shorts(Plan(layout, 7, 2, 2, 2, lanes));
        shorts.write<Facing>(shorts.row(4), 0, Facing::West);
        REQUIRE(shorts.read<Facing>(shorts.row(4), 0) == Facing::West);
        CheckedRegion longs(Plan(layout, 7, 2, 8, 8, lanes));
        const std::uint64_t pattern = UINT64_C(0xffee00ddeadbeef1);
        longs.write<std::uint64_t>(longs.row(5), 1, pattern);
        REQUIRE(longs.read<std::uint64_t>(longs.row(5), 1) == pattern);
        rejects<std::invalid_argument>([&] {
            (void)longs.read<std::uint16_t>(longs.row(5), 1);
        });
    }
}

int main() {
    scalar_abi_matrix();
    matrix();
    migration_and_identity();
    float_bits_and_overflows();
}
