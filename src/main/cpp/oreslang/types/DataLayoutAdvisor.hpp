#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "oreslang/runtime/DopLayout.hpp"

namespace oreslang::analysis {

// Native compiler-side advisory. No mutation, loans, safe-rewrite or
// hard-real-time proof is granted by any result from this API.
enum class DataLayout { Aos, Soa, Aosoa, SparseSet, Archetype };
struct LayoutHints {
    std::optional<bool> random_cross_column_gathers;
    std::optional<bool> dense_stable_membership;
    std::optional<std::uint64_t> structural_changes_per_pass;
    std::optional<std::uint32_t> aosoa_tile_rows;
    std::optional<std::uint32_t> vector_bytes;
};
struct LayoutRegion {
    std::string name;
    DataLayout layout = DataLayout::Aos;
    std::uint64_t rows = 0;
    std::vector<std::uint32_t> field_bytes;
    std::vector<std::uint32_t> accessed_fields;
    bool hot = false;
    bool dense_iteration = false;
    bool sequential_rows = false;
    bool whole_record_access = false;
    bool independent_component_passes = false;
    bool relocation_verified = false;
    bool stable_address_required = true;
    LayoutHints hints;
};
struct LayoutFinding {
    std::string code;
    std::string message;
};

inline std::vector<LayoutFinding> advise_layout(const LayoutRegion& r) {
    if (r.name.empty() || r.name.size() > 1024 ||
            r.field_bytes.empty() || r.field_bytes.size() > 16384 ||
            r.accessed_fields.size() > 16384)
        throw std::invalid_argument("invalid or over-budget layout evidence");
    std::uint64_t total = 0, selected = 0;
    for (const auto width : r.field_bytes) {
        if (width == 0 || total > std::numeric_limits<std::uint64_t>::max() - width)
            throw std::invalid_argument("invalid total field width");
        total += width;
    }
    std::unordered_set<std::uint32_t> unique;
    for (const auto index : r.accessed_fields) {
        if (index >= r.field_bytes.size() || !unique.insert(index).second)
            throw std::invalid_argument("duplicate or out-of-bounds accessed field");
        selected += r.field_bytes[index];
    }
    if (r.hints.vector_bytes &&
            (*r.hints.vector_bytes < 8 ||
             (*r.hints.vector_bytes & (*r.hints.vector_bytes - 1)) != 0))
        throw std::invalid_argument("invalid target vector width");
    if (r.hints.aosoa_tile_rows &&
            (r.layout != DataLayout::Aosoa ||
             *r.hints.aosoa_tile_rows < 2 ||
             *r.hints.aosoa_tile_rows > 4096))
        throw std::invalid_argument("invalid AoSoA tile width");

    std::vector<LayoutFinding> findings;
    if (!r.hot || r.rows < 128) return findings;
    const auto stream = r.dense_iteration && r.sequential_rows;
    if (r.layout == DataLayout::Aos && stream && r.relocation_verified &&
            !r.stable_address_required && !r.whole_record_access &&
            r.field_bytes.size() >= 4 && selected > 0 && selected <= total / 2)
        findings.push_back({"ORES-LAYOUT101", "Benchmark SoA for field-selective AoS traversal."});
    if (r.layout == DataLayout::Soa && stream && r.whole_record_access)
        findings.push_back({"ORES-LAYOUT131", "Benchmark AoS/AoSoA for whole-record SoA traversal."});
    if ((r.layout == DataLayout::Soa || r.layout == DataLayout::Aosoa ||
            r.layout == DataLayout::Archetype) &&
            r.hints.random_cross_column_gathers.value_or(false))
        findings.push_back({"ORES-LAYOUT132", "Profile random cross-column gather locality."});
    if (r.layout == DataLayout::Aosoa && r.hints.aosoa_tile_rows &&
            r.hints.vector_bytes && !r.accessed_fields.empty()) {
        std::uint32_t widest = 0;
        for (auto index : r.accessed_fields)
            widest = std::max(widest, r.field_bytes[index]);
        if (widest && std::uint64_t(*r.hints.vector_bytes) >= std::uint64_t(widest) * 2) {
            auto ideal = std::max(2U, std::min(64U, *r.hints.vector_bytes / widest));
            auto actual = *r.hints.aosoa_tile_rows;
            if (actual < std::max(2U, ideal / 2) ||
                    std::uint64_t(actual) > std::uint64_t(ideal) * 4)
                findings.push_back({"ORES-LAYOUT133", "Benchmark AoSoA tile and SIMD/tail costs."});
        }
    }
    const auto ecs = r.layout == DataLayout::SparseSet || r.layout == DataLayout::Archetype;
    if (ecs && stream && !r.independent_component_passes &&
            r.hints.dense_stable_membership.value_or(false))
        findings.push_back({"ORES-LAYOUT134", "Benchmark dense SoA or fused archetype iteration."});
    if (ecs && r.hints.structural_changes_per_pass &&
            *r.hints.structural_changes_per_pass >= r.rows / 8 + (r.rows % 8 != 0))
        findings.push_back({"ORES-LAYOUT135", "Benchmark deferred ECS mutation batches and generation-safe handles."});
    return findings;
}

// This geometry-only rule is derivable from an actual Oreslang native
// DopLayout::Plan. Unlike hot-path advice, it does not require profiler
// evidence, but it still does not prove that another layout is faster.
inline std::vector<LayoutFinding> advise_dop_plan_padding(const dop::Plan& plan) {
    if (plan.layout() != dop::Layout::AoSoA || plan.rows() == 0)
        return {};
    const auto payload = dop::checked_mul(
        dop::checked_mul(plan.rows(), plan.fields()), plan.scalar_bytes());
    const auto physical = plan.byte_length(); // checked by Plan constructor
    const auto extra = physical - payload;
    const auto quarter = payload / 4 + (payload % 4 != 0);
    if (extra >= quarter)
        return {{"ORES-LAYOUT141",
                "AoSoA physical storage exceeds logical scalar payload by >=25%; benchmark tile geometry and tail utilization."}};
    return {};
}

} // namespace oreslang::analysis
