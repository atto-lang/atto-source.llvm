#include "oreslang/types/DataLayoutAdvisor.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::analysis;

static void require(bool cond, const char* message) {
    if (!cond) {
        std::cerr << message << '\n';
        std::abort();
    }
}
static bool has(const std::vector<LayoutFinding>& findings, const char* code) {
    for (const auto& f : findings) if (f.code == code) return true;
    return false;
}
static LayoutRegion region(DataLayout layout) {
    LayoutRegion r;
    r.name = "runtime.actor_columns";
    r.layout = layout;
    r.rows = 10'000;
    r.field_bytes = {4, 4, 4, 4, 8, 8, 8, 8};
    r.accessed_fields = {0, 1};
    r.hot = true;
    r.dense_iteration = true;
    r.sequential_rows = true;
    r.relocation_verified = true;
    r.stable_address_required = false;
    return r;
}
int main() {
    auto aos = region(DataLayout::Aos);
    require(has(advise_layout(aos), "ORES-LAYOUT101"), "expected SoA candidate");
    aos.stable_address_required = true;
    require(!has(advise_layout(aos), "ORES-LAYOUT101"), "stable address must suppress rewrite");

    auto soa = region(DataLayout::Soa);
    soa.whole_record_access = true;
    require(has(advise_layout(soa), "ORES-LAYOUT131"), "expected SoA whole-record warning");
    soa.hints.random_cross_column_gathers = true;
    require(has(advise_layout(soa), "ORES-LAYOUT132"), "expected random gather warning");

    auto aosoa = region(DataLayout::Aosoa);
    aosoa.hints.vector_bytes = 32;
    aosoa.hints.aosoa_tile_rows = 256;
    require(has(advise_layout(aosoa), "ORES-LAYOUT133"), "expected tile benchmark");
    aosoa.hints.vector_bytes.reset();
    require(!has(advise_layout(aosoa), "ORES-LAYOUT133"), "missing SIMD width is unknown");

    auto archetype = region(DataLayout::Archetype);
    archetype.hints.structural_changes_per_pass = 1500;
    require(has(advise_layout(archetype), "ORES-LAYOUT135"), "expected churn warning");
    archetype.hints.structural_changes_per_pass.reset();
    require(!has(advise_layout(archetype), "ORES-LAYOUT135"), "unknown churn must not warn");

    // Exact geometry: 10 rows * 4 fields * 4 bytes is 160 bytes of
    // logical payload; a 64-lane tile physically stores 1024 bytes.
    oreslang::dop::Plan waste(oreslang::dop::Layout::AoSoA, 10, 4, 4, 64, 64);
    require(has(advise_dop_plan_padding(waste), "ORES-LAYOUT141"),
            "missed provable AoSoA tail-space overhead");
    oreslang::dop::Plan packed(oreslang::dop::Layout::AoSoA, 64, 4, 4, 64, 64);
    require(!has(advise_dop_plan_padding(packed), "ORES-LAYOUT141"),
            "fully occupied tile must not report padding waste");

    bool failed = false;
    try {
        aos.accessed_fields = {0, 0};
        (void)advise_layout(aos);
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    require(failed, "duplicate fields must fail closed");
    return 0;
}
