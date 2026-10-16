#include "oreslang/runtime/DopRegionSchedule.hpp"
#include <type_traits>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

static void check(bool valid) {
    if (!valid) throw std::runtime_error("native DOP region effect test failed");
}

int main() {
    static_assert(!std::is_assignable_v<oreslang::dop::RegionAccess&, oreslang::dop::RegionAccess>,
                  "admitted native region accesses must not be mutable");
    static_assert(!std::is_assignable_v<oreslang::dop::RegionTask&, oreslang::dop::RegionTask>,
                  "admitted native task footprint must not be mutable");
    using namespace oreslang::dop;
    auto r = RegionRoot::trusted_create(10, 3);
    auto other = RegionRoot::trusted_create(10, 3);
    const RegionAccess read(r, 0, 2, 6, RegionMode::Read);
    const RegionAccess write(r, 0, 2, 6, RegionMode::Write);
    const RegionAccess field_write(r, 1, 2, 6, RegionMode::Write);
    const RegionAccess touching(r, 0, 8, 2, RegionMode::Write);
    const RegionAccess overlapping(r, 0, 7, 2, RegionMode::Write);
    const RegionAccess empty(r, 0, 3, 0, RegionMode::Write);
    const RegionAccess reduction(r, 0, 2, 6, RegionMode::Reduce, "sum");
    check(independent(read, read));
    check(!independent(read, write));
    check(independent(write, field_write));
    check(independent(write, touching));
    check(!independent(write, overlapping));
    check(independent(write, empty));
    check(!independent(reduction, reduction));
    check(independent(write, RegionAccess(other, 0, 2, 6, RegionMode::Write)));
    auto bad = [&](auto action) {
        bool rejected = false;
        try { action(); } catch (const std::exception&) { rejected = true; }
        check(rejected);
    };
    bad([&] { RegionRoot::trusted_create(2, 0); });
    bad([&] { RegionAccess(r, 3, 0, 1, RegionMode::Read); });
    bad([&] { RegionAccess(r, 0, 9, 2, RegionMode::Read); });
    bad([&] { RegionAccess(r, 0, 2, std::numeric_limits<std::uint64_t>::max(), RegionMode::Read); });
    bad([&] { RegionAccess(r, 0, 0, 1, RegionMode::Reduce); });
    bad([&] { RegionAccess(r, 0, 0, 1, RegionMode::Read, "sum"); });
    bad([&] { RegionAccess(r, 0, 0, 1, RegionMode::Reduce, " sum"); });
    bad([&] { RegionAccess(r, 0, 0, 1, static_cast<RegionMode>(255)); });
    bad([&] { RegionRoot::trusted_create(std::numeric_limits<std::uint64_t>::max(), 1); });
    std::vector<RegionTask> tasks;
    tasks.emplace_back("write-x", std::vector<RegionAccess>{write});
    tasks.emplace_back("read-x", std::vector<RegionAccess>{read});
    tasks.emplace_back("write-y", std::vector<RegionAccess>{field_write});
    auto batches = dependency_levels(tasks);
    check(batches.size() == 2);
    check((batches[0] == std::vector<std::size_t>{0,2}));
    check((batches[1] == std::vector<std::size_t>{1}));
    check(dependency_levels({}).empty());
    // Exhaustive oracle: two effects conflict iff a physical logical cell
    // overlaps and either side can mutate it; no optimistic alias proofs.
    auto grid = RegionRoot::trusted_create(5, 2);
    for (int fa = 0; fa < 2; ++fa)
    for (int fb = 0; fb < 2; ++fb)
    for (int aa = 0; aa <= 5; ++aa)
    for (int az = aa; az <= 5; ++az)
    for (int ba = 0; ba <= 5; ++ba)
    for (int bz = ba; bz <= 5; ++bz)
    for (int ma = 0; ma < 5; ++ma)
    for (int mb = 0; mb < 5; ++mb) {
        auto left_mode = static_cast<RegionMode>(ma);
        auto right_mode = static_cast<RegionMode>(mb);
        RegionAccess a(grid, static_cast<std::uint32_t>(fa), aa, az-aa,
                       left_mode, left_mode == RegionMode::Reduce ? "sum" : "");
        RegionAccess b(grid, static_cast<std::uint32_t>(fb), ba, bz-ba,
                       right_mode, right_mode == RegionMode::Reduce ? "sum" : "");
        bool overlap = false;
        for (int row = 0; row < 5; ++row)
            if (fa == fb && row >= aa && row < az && row >= ba && row < bz)
                overlap = true;
        const bool expected = !overlap
            || (left_mode == RegionMode::Read && right_mode == RegionMode::Read);
        check(independent(a, b) == expected);
        check(independent(b, a) == expected);
    }

}
