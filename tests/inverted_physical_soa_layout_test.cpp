// Host geometry and compiler preflight test, not a compiled .atto guest.
// Every check remains enabled in Release/NDEBUG and under sanitizers.
#include "oreslang/types/InvertedPhysicalSoA.hpp"
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>

using oreslang::dop::abi::PhysicalSoA;
using oreslang::dop::abi::ScalarLane;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class F> static void reject(F run) {
    try { run(); } catch (const std::exception&) { return; }
    throw std::runtime_error("unsafe physical layout admitted");
}

int main() {
    int groups = 0;
    const auto make = [](std::vector<ScalarLane> lanes, std::size_t n,
                         std::size_t limit = 16u * 1024u * 1024u) {
        return PhysicalSoA::make(lanes, n, limit);
    };
    {
        auto p = make({{8,8},{8,8}}, 256);
        require(p.bytes() == 4096 && p.packed_record_bytes() == 16, "i64 pair changed");
        require(p.column_base(0) == 0 && p.column_base(1) == 2048, "pair bases changed");
        require(p.offset(1,255) == 4088, "pair index changed");
        ++groups;
    }
    {
        auto p = make({{1,1},{8,8},{1,1}}, 1);
        require(p.packed_record_bytes() == 10, "packed nominal width wrong");
        require(p.bytes() == 17, "physical SoA must include seven padding bytes");
        require(p.column_base(0) == 0 && p.column_base(1) == 8 &&
                p.column_base(2) == 16, "heterogeneous columns misaligned");
        ++groups;
    }
    {
        auto p = make({{1,1},{8,8},{1,1}}, 2);
        require(p.bytes() == 26 && p.column_base(1) == 8 &&
                p.column_base(2) == 24, "padding depends on capacity");
        require(p.offset(1,1) == 16 && p.offset(2,1) == 25, "mixed row offset");
        ++groups;
    }
    {
        auto p = make({{1,1},{4,4},{8,8},{2,2}}, 3);
        require(p.packed_record_bytes() == 15 && p.bytes() == 46, "mixed scalar extent");
        require(p.column_base(0) == 0 && p.column_base(1) == 4 &&
                p.column_base(2) == 16 && p.column_base(3) == 40, "mixed scalar bases");
        ++groups;
    }
    {
        auto p = make({{8,8},{8,8}}, 1024, 16384);
        require(p.bytes() == 16384, "exact quota boundary must be admitted");
        reject([&]{ (void)make({{8,8},{8,8}}, 1025, 16384); });
        reject([&]{ (void)make({{1,1},{8,8},{1,1}}, 1, 16); });
        ++groups;
    }
    {
        reject([&]{ (void)make({}, 1); });
        reject([&]{ (void)make(std::vector<ScalarLane>(65,{1,1}), 1); });
        reject([&]{ (void)make({{8,8}}, 0); });
        reject([&]{ (void)make({{16,16}}, 1); });
        reject([&]{ (void)make({{8,4}}, 1); });
        reject([&]{ (void)make({{0,0}}, 1); });
        reject([&]{ (void)make({{8,8}}, std::numeric_limits<std::size_t>::max()); });
        ++groups;
    }
    {
        auto p = make({{1,1},{8,8}}, 2);
        reject([&]{ (void)p.column_base(2); });
        reject([&]{ (void)p.offset(0,2); });
        reject([&]{ (void)p.offset(2,0); });
        ++groups;
    }
    {
        std::vector<ScalarLane> input{{1,1},{8,8}};
        const auto p = make(input, 2);
        input[1] = {1,1};
        input.clear();
        require(p.bytes() == 24 && p.column_base(1) == 8,
                "caller mutation changed admitted geometry");
        ++groups;
    }
    {
        // Deterministic property checks: disjoint contiguous lane spans,
        // correct alignment and exact byte index for varied capacities.
        std::uint32_t seed = 0x92ac103du;
        const auto next = [&]{ seed ^= seed << 13; seed ^= seed >> 17;
                               seed ^= seed << 5; return seed; };
        for (unsigned trial = 0; trial < 300; ++trial) {
            std::vector<ScalarLane> lanes;
            const auto count = 1 + next()%32;
            for (unsigned i = 0; i < count; ++i) {
                const std::size_t width = std::size_t(1) << (next()%4);
                lanes.push_back({width,width});
            }
            const auto cap = 1 + next()%127;
            const auto plan = make(lanes, cap);
            std::size_t end = 0, packed = 0;
            for (std::size_t column = 0; column < lanes.size(); ++column) {
                const auto base = plan.column_base(column);
                require(base >= end && base % lanes[column].alignment == 0,
                        "overlapping or unaligned SoA lane");
                require(plan.offset(column, 0) == base, "first row misaddressed");
                require(plan.offset(column, cap-1) == base +
                        (cap-1)*lanes[column].element_bytes, "last row misaddressed");
                end = base + cap*lanes[column].element_bytes;
                packed += lanes[column].element_bytes;
            }
            require(end == plan.bytes() && packed == plan.packed_record_bytes(),
                    "geometry extent differs from bounds");
        }
        ++groups;
    }
    std::cout << "INVERTED_PHYSICAL_SOA_HOST_PASS groups=" << groups
              << " randomized=300" << std::endl;
}
