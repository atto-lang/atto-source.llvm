#include "oreslang/runtime/DopSystemAccess.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace oreslang::dop;

namespace {
SystemAccessSpan rd(const void* id, std::uint64_t lo, std::uint64_t hi) {
    return SystemAccessSpan(id, lo, hi, AccessMode::Read);
}
SystemAccessSpan wr(const void* id, std::uint64_t lo, std::uint64_t hi) {
    return SystemAccessSpan(id, lo, hi, AccessMode::Write);
}
template<typename Action>
void rejects_empty_span(Action action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid byte span accepted");
}
}

int main() {
    int positions = 0, velocities = 0, health = 0;
    const SystemFootprint move("movement", {rd(&velocities, 0, 4096), wr(&positions, 0, 4096)});
    const SystemFootprint damage("damage", {wr(&health, 0, 4096)});
    const SystemFootprint render("render", {rd(&positions, 0, 4096)});
    const SystemFootprint render2("render2", {rd(&positions, 0, 4096)});

    assert(!access_conflicts(move, damage));
    assert(access_conflicts(move, render));
    assert(!access_conflicts(render, render2));
    const auto batches = plan_access_batches({move, damage, render, render2});
    assert(batches.size() == 2 && batches[0].size() == 2 && batches[1].size() == 2);
    assert(batches[0][0].name == "movement" && batches[1][1].name == "render2");

    const SystemFootprint first("first", {wr(&positions, 0, 64)});
    const SystemFootprint second("second", {wr(&positions, 64, 128)});
    const SystemFootprint overlap("overlap", {rd(&positions, 63, 65)});
    assert(!access_conflicts(first, second));
    assert(access_conflicts(first, overlap) && access_conflicts(second, overlap));

    const SystemFootprint unknown("unknown", {wr(nullptr, 0, 1)});
    const SystemFootprint known("known", {rd(&positions, 1024, 1025)});
    const SystemFootprint unknown_reader("unknown_reader", {rd(nullptr, 0, 1)});
    assert(access_conflicts(unknown, known));
    assert(!access_conflicts(unknown_reader, known));

    // The planner may not backfill C into batch A when B depends on A and
    // C depends on B, even though A and C access different allocations.
    const SystemFootprint a("a", {wr(&positions, 0, 8)});
    const SystemFootprint b("b", {rd(&positions, 0, 8), wr(&health, 0, 8)});
    const SystemFootprint c("c", {rd(&health, 0, 8)});
    const auto ordered = plan_access_batches({a, b, c});
    assert(ordered.size() == 3);
    assert(ordered[0][0].name == "a" && ordered[1][0].name == "b"
           && ordered[2][0].name == "c");

    rejects_empty_span([&] { (void)rd(&positions, 0, 0); });
    rejects_empty_span([&] { (void)wr(&positions, 10, 9); });
    assert(plan_access_batches({}).empty());

    std::cout << "PASS: native ECS/DOP access conflicts and stable dependency batches\n";
}
