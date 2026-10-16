#include "oreslang/runtime/DopRegionSchedule.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

int main() {
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
    assert(independent(read, read));
    assert(!independent(read, write));
    assert(independent(write, field_write));
    assert(independent(write, touching));
    assert(!independent(write, overlapping));
    assert(independent(write, empty));
    assert(!independent(reduction, reduction));
    assert(independent(write, RegionAccess(other, 0, 2, 6, RegionMode::Write)));
    auto bad = [&](auto action) {
        bool rejected = false;
        try { action(); } catch (const std::exception&) { rejected = true; }
        assert(rejected);
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
    assert(batches.size() == 2);
    assert((batches[0] == std::vector<std::size_t>{0,2}));
    assert((batches[1] == std::vector<std::size_t>{1}));
    assert(dependency_levels({}).empty());
}
