#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::gpu::source_mir {

// Source-expression IR, never a typed device-program IR or a launch permit.
// The frontend preserves symbolic extents; no unbound symbol is silently 0.
struct I64 {
    enum class Kind { Constant, Symbol, CeilDiv };
    Kind kind;
    std::int64_t value = 0;
    std::string symbol;
    std::shared_ptr<const I64> left;
    std::shared_ptr<const I64> right;

    static I64 constant(std::int64_t n) {
        if (n < 0) throw std::invalid_argument("GPU-LAUNCH-003: negative GPU geometry");
        return {Kind::Constant, n, {}, {}, {}};
    }
    static I64 named(std::string name) {
        if (name.empty() || name.size() > 256)
            throw std::invalid_argument("GPU-LAUNCH-003: malformed geometry symbol");
        return {Kind::Symbol, 0, std::move(name), {}, {}};
    }
    static I64 ceil_div(I64 a, I64 b) {
        if (b.kind == Kind::Constant && b.value == 0)
            throw std::invalid_argument("GPU-LAUNCH-003: ceil_div by zero");
        if (a.kind == Kind::Constant && b.kind == Kind::Constant)
            return constant(a.value / b.value + (a.value % b.value != 0));
        return {Kind::CeilDiv, 0, {},
                std::make_shared<I64>(std::move(a)),
                std::make_shared<I64>(std::move(b))};
    }
};

enum class Mode { PhysicalGridBlock, LogicalOverGroup };
struct Geometry {
    Mode mode;
    std::vector<I64> extent;
    std::vector<I64> group;
};
struct Candidate {
    std::string kernel;
    std::vector<std::string> ordered_arguments;
    std::string device;
    std::string queue;
    Geometry geometry;
    I64 shared_bytes;
    static constexpr bool executable() noexcept { return false; }
    static constexpr bool type_checked() noexcept { return false; }
};

inline void require_name(const std::string& value, const char* part) {
    if (value.empty() || value.size() > 256)
        throw std::invalid_argument(std::string("GPU-LAUNCH-001: invalid ") + part);
}

inline Candidate source_candidate(Candidate value) {
    require_name(value.kernel, "kernel symbol");
    require_name(value.device, "device symbol");
    require_name(value.queue, "queue symbol");
    if (value.ordered_arguments.size() > 64)
        throw std::invalid_argument("GPU-LAUNCH-002: too many kernel arguments");
    for (const auto& a : value.ordered_arguments)
        require_name(a, "kernel argument");
    if (value.geometry.extent.size() != 3 || value.geometry.group.size() != 3)
        throw std::invalid_argument("GPU-LAUNCH-003: launch axes must normalize to 3D");
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto& e = value.geometry.extent[axis];
        const auto& g = value.geometry.group[axis];
        if (g.kind == I64::Kind::Constant && g.value == 0)
            throw std::invalid_argument("GPU-LAUNCH-003: group/block dimension must be positive");
        if (value.geometry.mode == Mode::PhysicalGridBlock &&
                e.kind == I64::Kind::Constant && e.value == 0)
            throw std::invalid_argument("GPU-LAUNCH-003: physical grid dimension must be positive");
    }
    return value;
}

}  // namespace oreslang::gpu::source_mir
