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
    require(soa.total_bytes() == 51 && soa.physical_bytes() == 56 &&
            soa.address(2,1).byte_offset == 16 && soa.absolute_address(2,1) == 32 &&
            soa.address(2,1).segment == 1 &&
            soa.segments()[0].base == 0 && soa.segments()[1].base == 16 &&
            soa.segments()[2].base == 40 && soa.segments()[3].base == 44,
            "SoA mixed-width reference geometry mismatch");
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
                std::unordered_set<std::uint64_t> occupied;
                std::uint64_t next = 0;
                for (const auto& segment : plan.segments()) {
                    require(segment.base >= next &&
                            segment.base % segment.alignment == 0,
                            "invalid native segment-base alignment");
                    next = checked_add(segment.base, segment.bytes);
                }
                require(plan.physical_bytes() == next,
                        "actor physical byte cap does not include segment padding");
                for (std::uint32_t row = 0; row < capacity; ++row) {
                    for (std::uint32_t field = 0; field < schema.size(); ++field) {
                        const auto loc = plan.address(row,field);
                        const auto& segment = plan.segments().at(loc.segment);
                        require(loc.byte_offset % schema[field].alignment == 0,
                                "unaligned mixed-width field");
                        require(loc.byte_offset <= segment.bytes &&
                                schema[field].bytes <= segment.bytes - loc.byte_offset,
                                "mixed-width field exceeds segment");
                        const auto absolute = plan.absolute_address(row,field);
                        require(absolute % schema[field].alignment == 0,
                                "unnaturally aligned native actor field");
                        for (std::uint64_t byte = 0; byte < schema[field].bytes; ++byte) {
                            require(occupied.insert(checked_add(absolute,byte)).second,
                                    "mixed-width fields overlap in physical arena");
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
    rejects<std::length_error>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::SoA,
        3,schema,1,{},24,51); });
    rejects<std::length_error>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::SoA,
        3,schema,1,{},24,55); });
    const HeterogeneousPlan soa_exact(HeterogeneousLayout::SoA,3,schema,1,{},24,56);
    require(soa_exact.physical_bytes() == 56,
            "aligned actor allocation rejected at exact physical byte cap");
    rejects<std::length_error>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::AoSoA,
        5,schema,4,{},144,143); });
    rejects<std::length_error>([&]{ (void)HeterogeneousPlan(HeterogeneousLayout::SoAoS,
        3,schema,1,groups,48,71); });
}
