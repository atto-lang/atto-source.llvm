#include "oreslang/runtime/DopSystemAccess.hpp"

#include <random>
#include <string>
#include <type_traits>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace oreslang::dop;

// Assertions from <cassert> are compiled out under NDEBUG. Keep checks live
// in both Debug and Release/ASan/UBSan jobs.
#define REQUIRE(condition) do { \
    if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
} while (false)

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

    REQUIRE(!access_conflicts(move, damage));
    REQUIRE(access_conflicts(move, render));
    REQUIRE(!access_conflicts(render, render2));
    const auto batches = plan_access_batches({move, damage, render, render2});
    REQUIRE(batches.size() == 2 && batches[0].size() == 2 && batches[1].size() == 2);
    REQUIRE(batches[0][0].name() == "movement" && batches[1][1].name() == "render2");

    const SystemFootprint first("first", {wr(&positions, 0, 64)});
    const SystemFootprint second("second", {wr(&positions, 64, 128)});
    const SystemFootprint overlap("overlap", {rd(&positions, 63, 65)});
    REQUIRE(!access_conflicts(first, second));
    REQUIRE(access_conflicts(first, overlap) && access_conflicts(second, overlap));

    const SystemFootprint unknown("unknown", {wr(nullptr, 0, 1)});
    const SystemFootprint known("known", {rd(&positions, 1024, 1025)});
    const SystemFootprint unknown_reader("unknown_reader", {rd(nullptr, 0, 1)});
    REQUIRE(access_conflicts(unknown, known));
    REQUIRE(!access_conflicts(unknown_reader, known));

    // The planner may not backfill C into batch A when B depends on A and
    // C depends on B, even though A and C access different allocations.
    const SystemFootprint a("a", {wr(&positions, 0, 8)});
    const SystemFootprint b("b", {rd(&positions, 0, 8), wr(&health, 0, 8)});
    const SystemFootprint c("c", {rd(&health, 0, 8)});
    const auto ordered = plan_access_batches({a, b, c});
    REQUIRE(ordered.size() == 3);
    REQUIRE(ordered[0][0].name() == "a" && ordered[1][0].name() == "b"
           && ordered[2][0].name() == "c");

    // A missing access footprint is not an effect/purity proof. Serialize
    // it until the compiler has independently checked all possible effects.
    const SystemFootprint unknown_effects("unknown_effects", {});
    const SystemFootprint another_unknown("another_unknown", {});
    REQUIRE(access_conflicts(unknown_effects, render));
    REQUIRE(access_conflicts(render, unknown_effects));
    REQUIRE(access_conflicts(unknown_effects, another_unknown));
    const auto barriers = plan_access_batches({render, unknown_effects, render2});
    REQUIRE(barriers.size() == 3);

    // Immutable validated descriptors: every exposed view is const.
    static_assert(std::is_same_v<
            decltype(std::declval<const SystemFootprint&>().spans()),
            const std::vector<SystemAccessSpan>&>);
    static_assert(std::is_same_v<
            decltype(std::declval<const SystemFootprint&>().name()),
            const std::string&>);

    rejects_empty_span([&] { (void)rd(&positions, 0, 0); });
    rejects_empty_span([&] {
        (void)SystemAccessSpan(&positions, 0, 2, static_cast<AccessMode>(255));
    });
    rejects_empty_span([&] { (void)SystemFootprint("   ", {}); });

    // Deterministic randomized dependency oracle. Every access conflict in
    // program order MUST cross a batch barrier; systems in one batch must be
    // pairwise conflict-free, including unknown-allocation writes.
    std::mt19937 rng(0x5eedU);
    int allocations[] = {0, 0, 0};
    for (int trial = 0; trial < 500; ++trial) {
        std::vector<SystemFootprint> workload;
        for (int index = 0; index < 8; ++index) {
            std::vector<SystemAccessSpan> spans;
            int n = static_cast<int>(rng() % 4);
            for (int access = 0; access < n; ++access) {
                unsigned location = rng() % 4;
                const void* allocation = location == 3 ? nullptr : &allocations[location];
                std::uint64_t start = rng() % 20;
                spans.emplace_back(allocation, start, start + 1 + rng() % 7,
                                   rng() % 2 ? AccessMode::Read : AccessMode::Write);
            }
            workload.emplace_back(std::to_string(index), std::move(spans));
        }
        auto scheduled = plan_access_batches(workload);
        unsigned output_index = 0;
        std::vector<std::size_t> assigned;
        for (std::size_t batch = 0; batch < scheduled.size(); ++batch) {
            const auto& group = scheduled[batch];
            for (std::size_t i = 0; i < group.size(); ++i) {
                REQUIRE(group[i].name() == std::to_string(output_index++));
                assigned.push_back(batch);
                for (std::size_t j = i + 1; j < group.size(); ++j)
                    REQUIRE(!access_conflicts(group[i], group[j]));
            }
        }
        REQUIRE(output_index == workload.size());
        for (std::size_t i = 0; i < workload.size(); ++i)
            for (std::size_t j = i + 1; j < workload.size(); ++j)
                if (access_conflicts(workload[i], workload[j]))
                    REQUIRE(assigned[i] < assigned[j]);
    }
    rejects_empty_span([&] { (void)wr(&positions, 10, 9); });
    REQUIRE(plan_access_batches({}).empty());

    std::cout << "PASS: native ECS/DOP access conflicts and stable dependency batches\n";
}
