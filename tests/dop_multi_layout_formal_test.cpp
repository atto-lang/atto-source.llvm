#include "oreslang/runtime/DopLayout.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using oreslang::dop::Layout;
using oreslang::dop::Plan;
namespace {
constexpr unsigned kRows = 3, kFields = 2, kTile = 2;
using Row = std::array<std::uint32_t, kFields>;
using Rows = std::array<Row, kRows>;
constexpr std::array<Layout, 3> kLayouts{Layout::AoS, Layout::SoA, Layout::AoSoA};

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int index(Layout l) { return static_cast<int>(l); }
Plan plan(Layout l) { return Plan(l, kRows, kFields, 4, 16, l == Layout::AoSoA ? kTile : 1); }

struct Replica {
    bool resident;
    std::vector<std::uint8_t> bytes;
    unsigned epoch = 0;
};

struct Mirror {
    std::array<Replica, 3> copies;
    Rows logical{{Row{10, 11}, Row{20, 21}, Row{30, 31}}};
    unsigned epoch = 0;
    Layout authority;

    explicit Mirror(unsigned mask)
        : authority(kLayouts.at(static_cast<std::size_t>(
                mask & 1 ? 0 : mask & 2 ? 1 : 2))) {
        for (auto l : kLayouts) {
            auto& replica = copies.at(static_cast<std::size_t>(index(l)));
            replica.resident = (mask & (1u << index(l))) != 0;
            if (replica.resident) {
                replica.bytes.assign(static_cast<std::size_t>(plan(l).byte_length()), 0xA5);
                for (unsigned row = 0; row < kRows; ++row)
                    for (unsigned field = 0; field < kFields; ++field)
                        raw_put(l, row, field, logical[row][field]);
            }
        }
    }

    std::uint32_t raw_get(Layout l, unsigned r, unsigned f) const {
        const auto offset = plan(l).byte_offset(r, f);
        const auto& a = copies.at(static_cast<std::size_t>(index(l))).bytes;
        check(offset + 4 <= a.size(), "native offset overrun");
        std::uint32_t x = 0;
        std::memcpy(&x, a.data() + static_cast<std::size_t>(offset), sizeof(x));
        return x;
    }

    void raw_put(Layout l, unsigned r, unsigned f, std::uint32_t v) {
        const auto offset = plan(l).byte_offset(r, f);
        auto& a = copies.at(static_cast<std::size_t>(index(l))).bytes;
        check(offset + 4 <= a.size(), "native offset overrun");
        std::memcpy(a.data() + static_cast<std::size_t>(offset), &v, sizeof(v));
    }

    void hydrate(Layout l) {
        auto& c = copies.at(static_cast<std::size_t>(index(l)));
        check(c.resident, "requested absent layout");
        if (c.epoch == epoch) return;
        for (unsigned row = 0; row < kRows; ++row)
            for (unsigned f = 0; f < kFields; ++f)
                raw_put(l, row, f, raw_get(authority, row, f));
        c.epoch = epoch;
    }

    void write(Layout l, unsigned row, unsigned f, std::uint32_t value,
               bool unsafe_skip = false) {
        if (!unsafe_skip) hydrate(l);
        else check(copies.at(static_cast<std::size_t>(index(l))).resident,
                   "absent unsafe copy");
        raw_put(l, row, f, value);
        logical[row][f] = value;
        authority = l;
        ++epoch;
        copies.at(static_cast<std::size_t>(index(l))).epoch = epoch;
    }

    void verify_current() const {
        check(copies.at(static_cast<std::size_t>(index(authority))).epoch == epoch,
              "stale authority");
        for (auto l : kLayouts) {
            const auto& c = copies.at(static_cast<std::size_t>(index(l)));
            if (!c.resident) {
                check(c.bytes.empty(), "absent replica has storage");
                continue;
            }
            if (c.epoch == epoch)
                for (unsigned row = 0; row < kRows; ++row)
                    for (unsigned f = 0; f < kFields; ++f)
                        check(raw_get(l, row, f) == logical[row][f],
                              "cached row disagrees with oracle");
        }
    }

    void verify_all() {
        for (auto l : kLayouts)
            if (copies.at(static_cast<std::size_t>(index(l))).resident)
                hydrate(l);
        verify_current();
    }
};

void checked_geometry() {
    for (auto l : kLayouts) {
        auto p = plan(l);
        std::vector<std::uint64_t> offsets;
        for (unsigned r = 0; r < kRows; ++r)
            for (unsigned f = 0; f < kFields; ++f) {
                auto o = p.byte_offset(r, f);
                check(o + 4 <= p.byte_length(), "byte-range bounds");
                check(std::find(offsets.begin(), offsets.end(), o) == offsets.end(),
                      "layout alias collision");
                offsets.push_back(o);
            }
    }
    check(plan(Layout::AoSoA).byte_offset(2, 0) == 16,
          "second tile start offset");
    check(plan(Layout::AoSoA).byte_offset(2, 1) == 24,
          "second tile second field offset");
    check(plan(Layout::AoSoA).byte_length() == 32,
          "tile padding bytes");
}

void exhaustive_residency() {
    unsigned trials = 0;
    for (unsigned mask = 1; mask < 8; ++mask)
        for (auto first : kLayouts) if (mask & (1u << index(first)))
            for (auto second : kLayouts) if (mask & (1u << index(second)))
                for (unsigned rowA = 0; rowA < kRows; ++rowA)
                    for (unsigned rowB = 0; rowB < kRows; ++rowB) {
                        Mirror region(mask);
                        region.write(first, rowA, 0, 100 + rowA);
                        region.verify_current();
                        region.write(second, rowB, 1, 200 + rowB);
                        region.verify_current();
                        region.verify_all();
                        ++trials;
                    }
    check(trials == 216, "seven-residency cross-product mismatch");
}

void unsafe_mutation_oracle() {
    Mirror s(3); // AoS + SoA
    s.write(Layout::AoS, 0, 0, 555);
    s.write(Layout::SoA, 2, 1, 666, true); // ignores stale AoS row zero
    bool caught = false;
    try { s.verify_current(); }
    catch (const std::runtime_error&) { caught = true; }
    check(caught, "UNSAFE stale two-layout writer went undetected");
    Mirror false_epoch(7);
    false_epoch.write(Layout::AoS, 0, 0, 700);
    auto& unrefreshed = false_epoch.copies.at(
            static_cast<std::size_t>(index(Layout::SoA)));
    check(unrefreshed.epoch < false_epoch.epoch, "stale cache should exist");
    unrefreshed.epoch = false_epoch.epoch; // Claim freshness without copying bytes.
    caught = false;
    try { false_epoch.verify_current(); }
    catch (const std::runtime_error&) { caught = true; }
    check(caught, "UNSAFE forged current epoch went undetected");

    Mirror in_place(7);
    in_place.raw_put(Layout::AoS, 2, 0, 999); // Physical mutation without commit.
    caught = false;
    try { in_place.verify_current(); }
    catch (const std::runtime_error&) { caught = true; }
    check(caught, "UNSAFE uncommitted physical write went undetected");

    // Naive AoSoA arithmetic missing the FIELDS tile stride overlaps addresses
    // at the second tile. This must not coincide with the real checked Plan.
    std::vector<std::uint64_t> bad_offsets;
    bool collision = false;
    for (unsigned row = 0; row < kRows; ++row)
        for (unsigned field = 0; field < kFields; ++field) {
            const std::uint64_t bad = static_cast<std::uint64_t>(
                    (row / kTile + field) * kTile + row % kTile) * 4;
            if (std::find(bad_offsets.begin(), bad_offsets.end(), bad)
                    != bad_offsets.end())
                collision = true;
            bad_offsets.push_back(bad);
        }
    check(collision, "UNSAFE AoSoA tile-stride mutation undetected");
    check(plan(Layout::AoSoA).byte_offset(2, 0) != bad_offsets.at(4),
          "native planner must not match mutated tile stride");

    Mirror absent(1);
    caught = false;
    try { absent.hydrate(Layout::SoA); }
    catch (const std::runtime_error&) { caught = true; }
    check(caught, "absent layout hydration must fail closed");
}
}

int main() {
    checked_geometry();
    exhaustive_residency();
    unsafe_mutation_oracle();
    std::cout << "NATIVE_DOP_MULTI_LAYOUT_FORMAL_PASS trials=216 subsets=7 negative=stale-write,forged-epoch,partial-write,bad-tile,absent\n";
}
