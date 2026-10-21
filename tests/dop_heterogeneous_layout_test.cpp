#include "oreslang/runtime/DopHeterogeneousLayout.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace oreslang::dop;

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<typename T, typename F> static void rejects(F&& operation) {
    bool rejected = false;
    try { operation(); } catch (const T&) { rejected = true; }
    require(rejected,"invalid heterogeneous layout admitted");
}

int main() {
    // Normative heterogeneous fixture: u32 id, f64 x, bool flag, ref32 next.
    const std::vector<FieldSpec> schema{{4,4}, {8,8}, {1,1}, {4,4}};
    const std::vector<std::vector<std::uint32_t>> groups{{0,2},{1,3}};
    const HeterogeneousPlan aos(HeterogeneousLayout::AoS,3,schema);
    const HeterogeneousPlan soa(HeterogeneousLayout::SoA,3,schema);
    const HeterogeneousPlan tile(HeterogeneousLayout::AoSoA,5,schema,4);
    const HeterogeneousPlan soaos(HeterogeneousLayout::SoAoS,3,schema,1,groups);
    require(aos.segments()[0].stride == 24 && aos.address(1,1).byte_offset == 32,
            "AoS mixed-width reference geometry mismatch");
    require(soa.total_bytes() == 51 && soa.address(2,1).byte_offset == 16 &&
            soa.address(2,1).segment == 1, "SoA mixed-width reference geometry mismatch");
    require(tile.segments()[0].bytes == 144 && tile.address(4,0).byte_offset == 72,
            "AoSoA mixed-width padded geometry mismatch");
    require(soaos.segments()[0].stride == 8 && soaos.segments()[1].stride == 16 &&
            soaos.address(2,3).segment == 1 && soaos.address(2,3).byte_offset == 40,
            "SoAoS is not correct grouped AoS geometry");

    // Exhaustive small-capacity byte-level alias, alignment and bounds oracle.
    for (const auto layout : {HeterogeneousLayout::AoS,HeterogeneousLayout::SoA,
                               HeterogeneousLayout::AoSoA,HeterogeneousLayout::SoAoS}) {
        for (const auto capacity : {0U,1U,2U,3U,5U,8U,17U}) {
            for (const auto lanes : {1U,2U,4U,8U}) {
                if (layout != HeterogeneousLayout::AoSoA && lanes != 1) continue;
                const HeterogeneousPlan plan(layout,capacity,schema,lanes,
                                             layout == HeterogeneousLayout::SoAoS ? groups :
                                             std::vector<std::vector<std::uint32_t>>{});
                std::unordered_set<std::string> occupied;
                for (std::uint32_t row = 0; row < capacity; ++row) {
                    for (std::uint32_t field = 0; field < schema.size(); ++field) {
                        const auto loc = plan.address(row,field);
                        const auto& segment = plan.segments().at(loc.segment);
                        require(loc.byte_offset % schema[field].alignment == 0,
                                "unaligned mixed-width field");
                        require(loc.byte_offset <= segment.bytes &&
                                schema[field].bytes <= segment.bytes - loc.byte_offset,
                                "mixed-width field exceeds segment");
                        for (std::uint64_t byte = 0; byte < schema[field].bytes; ++byte) {
                            const std::string key = std::to_string(loc.segment) + ":" +
                                std::to_string(loc.byte_offset + byte);
                            require(occupied.insert(key).second,"mixed-width fields overlap");
                        }
                    }
                }
                rejects<std::out_of_range>([&]{ (void)plan.address(capacity,0); });
                rejects<std::out_of_range>([&]{ (void)plan.address(0,4); });
            }
        }
    }
    rejects<std::invalid_argument>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::AoS,1,{}); });
    rejects<std::invalid_argument>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::AoS,1,{{3,4}}); });
    rejects<std::invalid_argument>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::SoAoS,1,schema,1,{{0,1},{1,2,3}}); });
    rejects<std::invalid_argument>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::SoAoS,1,schema,1,{{0,1},{2}}); });
    rejects<std::invalid_argument>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::SoA,1,schema,4); });
    rejects<std::invalid_argument>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::AoSoA,1,schema,3); });
    rejects<std::overflow_error>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::AoS,
        std::numeric_limits<std::uint64_t>::max(),schema); });
    rejects<std::length_error>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::SoA,
        3,schema,1,{},23,51); });
    rejects<std::length_error>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::SoA,
        3,schema,1,{},24,50); });
    rejects<std::length_error>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::AoSoA,
        5,schema,4,{},144,143); });
    rejects<std::length_error>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::SoAoS,
        3,schema,1,groups,48,71); });
}
