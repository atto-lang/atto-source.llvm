#pragma once

#include "oreslang/runtime/GpuLaunchGeometry.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oreslang::gpu::typed_ir {

// Compiler-side host preflight. No native GPU allocation, kernel submission,
// completion event, runtime token, or device authority can be minted here.
// All handle identities must originate from a separately authenticated and
// pinned provider snapshot before this value is useful to a real backend.
enum class Parameter { F32, U64, ReadF32, WriteF32, I64 };
enum class Effect { DeviceRead, DeviceWrite, HostIo, Suspend, ActorSend,
                    DynamicAllocation, Barrier, Atomic };
enum class Access { Read, Write, ReadWrite };

struct BufferView {
    std::string tenant;
    std::string device;
    std::string allocation;
    std::int64_t generation;
    std::int64_t allocation_bytes;
    std::int64_t offset;
    std::int64_t length;
    Access access;
};
struct ScalarF32 { std::uint32_t bits; };
struct ScalarU64 { std::uint64_t value; };
struct ScalarI64 { std::int64_t value; };
using Argument = std::variant<ScalarF32, ScalarU64, BufferView, ScalarI64>;

struct Kernel {
    std::string symbol;
    std::vector<Parameter> parameters;
    std::vector<Effect> effects;
    bool returns_void = true;
};
struct Scope {
    std::string tenant;
    std::string device;
    std::string queue;
    std::int64_t queue_generation;
};
struct Physical { Dim3 grid; Dim3 block; };
struct Logical { Dim3 over; Dim3 group; };
using Geometry = std::variant<Physical, Logical>;
struct Request {
    Kernel kernel;
    std::vector<Argument> arguments;
    Scope scope;
    Geometry geometry;
    std::int64_t shared_bytes;
    Limits provider_limits;
    std::int64_t max_referenced_bytes;
    std::int64_t max_total_dispatched_lanes;
};

struct SourceCheckedLaunch {
    std::string kernel_symbol;
    LaunchPlan physical;
    std::int64_t logical_work_items;
    std::size_t buffer_count;
    // Important: this record is not, and can never become, an execute permit.
    [[nodiscard]] constexpr bool executable() const noexcept { return false; }
};

[[noreturn]] inline void deny(const char* code, const char* reason) {
    throw std::invalid_argument(std::string(code) + ": " + reason);
}
inline void identifier(const std::string& s, const char* reason) {
    if (s.empty() || s.size() > 256)
        deny("GPU-LAUNCH-004", reason);
}
inline bool writes(Access a) { return a != Access::Read; }

inline SourceCheckedLaunch verify(const Request& input) {
    const auto& k = input.kernel;
    const auto& s = input.scope;
    identifier(k.symbol, "kernel symbol missing");
    identifier(s.tenant, "tenant identity missing");
    identifier(s.device, "device identity missing");
    identifier(s.queue, "queue identity missing");
    if (s.queue_generation <= 0)
        deny("GPU-LAUNCH-004", "queue generation must be positive");
    if (!k.returns_void || k.parameters.size() > 64 ||
        k.parameters.size() != input.arguments.size())
        deny("GPU-LAUNCH-001", "invalid GPU kernel signature or argument arity");
    if (input.max_referenced_bytes <= 0)
        deny("GPU-LAUNCH-005", "invalid referenced-byte budget");
    if (input.max_total_dispatched_lanes <= 0)
        deny("GPU-LAUNCH-003", "invalid tenant dispatched-lane quota");

    bool reads = false, writes_any = false;
    for (Effect e : k.effects) {
        if (e == Effect::DeviceRead) reads = true;
        else if (e == Effect::DeviceWrite) writes_any = true;
        else deny("GPU-LAUNCH-006", "unsupported host, suspension or device effect");
    }

    struct CheckedRange { std::int64_t start, end; bool mutable_loan; };
    struct Allocation { std::int64_t generation, bytes; std::vector<CheckedRange> ranges; };
    std::map<std::string, Allocation> allocations;
    std::int64_t referenced = 0;
    std::size_t buffer_count = 0;
    for (std::size_t i = 0; i < k.parameters.size(); ++i) {
        const auto kind = k.parameters[i];
        const auto& arg = input.arguments[i];
        if (kind == Parameter::F32) {
            if (!std::holds_alternative<ScalarF32>(arg))
                deny("GPU-LAUNCH-002", "expected f32 scalar");
            continue;
        }
        if (kind == Parameter::U64) {
            if (!std::holds_alternative<ScalarU64>(arg))
                deny("GPU-LAUNCH-002", "expected u64 scalar");
            continue;
        }
        if (kind == Parameter::I64) {
            if (!std::holds_alternative<ScalarI64>(arg))
                deny("GPU-LAUNCH-002", "expected signed i64 scalar");
            continue;
        }
        if (kind != Parameter::ReadF32 && kind != Parameter::WriteF32)
            deny("GPU-LAUNCH-002", "unsupported kernel parameter type");
        if (!std::holds_alternative<BufferView>(arg))
            deny("GPU-LAUNCH-002", "host/scalar value cannot impersonate device buffer");
        const auto& v = std::get<BufferView>(arg);
        ++buffer_count;
        if (v.tenant != s.tenant || v.device != s.device)
            deny("GPU-LAUNCH-004", "foreign tenant/device buffer");
        identifier(v.allocation, "allocation identity missing");
        if (v.generation <= 0 || v.allocation_bytes <= 0 || v.offset < 0 ||
            v.length <= 0 || v.offset % 4 != 0 || v.length % 4 != 0 ||
            v.length > v.allocation_bytes ||
            v.offset > v.allocation_bytes - v.length)
            deny("GPU-LAUNCH-005", "invalid generation, alignment, byte range or overflow");
        if (kind == Parameter::ReadF32 && v.access != Access::Read)
            deny("GPU-LAUNCH-005", "read parameter requires immutable read view");
        if (kind == Parameter::WriteF32 && v.access == Access::Read)
            deny("GPU-LAUNCH-005", "write parameter requires exclusive write view");
        if (kind == Parameter::ReadF32 && !reads)
            deny("GPU-LAUNCH-006", "kernel read effect not declared");
        if (kind == Parameter::WriteF32 && !writes_any)
            deny("GPU-LAUNCH-006", "kernel write effect not declared");
        if (referenced > input.max_referenced_bytes - v.length)
            deny("GPU-LAUNCH-005", "referenced bytes exceed launch quota");
        referenced += v.length;
        auto [it, inserted] = allocations.try_emplace(v.allocation,
            Allocation{v.generation, v.allocation_bytes, {}});
        if (!inserted && (it->second.generation != v.generation ||
                          it->second.bytes != v.allocation_bytes))
            deny("GPU-LAUNCH-005", "contradictory allocation generation or byte extent");
        it->second.ranges.push_back({v.offset, v.offset + v.length, writes(v.access)});
    }

    for (auto& [allocation, record] : allocations) {
        (void)allocation;
        auto& ranges = record.ranges;
        std::sort(ranges.begin(), ranges.end(), [](const CheckedRange& a, const CheckedRange& b) {
            return a.start == b.start ? a.end < b.end : a.start < b.start;
        });
        std::int64_t max_any_end = 0, max_write_end = 0;
        for (const auto& range : ranges) {
            if ((range.mutable_loan ? max_any_end : max_write_end) > range.start)
                deny("GPU-LAUNCH-005", "overlapping mutable device loans");
            max_any_end = std::max(max_any_end, range.end);
            if (range.mutable_loan) max_write_end = std::max(max_write_end, range.end);
        }
    }

    LaunchPlan physical{{1, 1, 1}, {1, 1, 1}, 0, 0, 0, 0, true};
    std::int64_t logical = 0;
    if (const auto* p = std::get_if<Physical>(&input.geometry)) {
        physical = validate_launch(p->grid, p->block, input.shared_bytes, input.provider_limits);
        logical = physical.total_dispatched_lanes;
    } else {
        const auto& logical_geometry = std::get<Logical>(input.geometry);
        const auto plan = validate_logical_launch(logical_geometry.over, logical_geometry.group,
                                                  input.shared_bytes, input.provider_limits);
        physical = plan.physical;
        logical = plan.logical_work_items;
    }
    // Count padded lanes, not semantic work-items, against the tenant quota.
    if (physical.total_dispatched_lanes > input.max_total_dispatched_lanes)
        deny("GPU-LAUNCH-003", "total dispatched lanes exceed tenant quota");
    return {k.symbol, physical, logical, buffer_count};
}

} // namespace oreslang::gpu::typed_ir
