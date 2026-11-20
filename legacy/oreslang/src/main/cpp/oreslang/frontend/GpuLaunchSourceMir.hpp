#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>

namespace oreslang::gpu::source_mir {

inline bool source_symbol(const std::string& value);

// Source-expression IR, never a typed device-program IR or a launch permit.
// Aggregate fields are public for compiler interchange, so every producer
// (including tests and future IR adapters) MUST use source_candidate() before
// passing a value to another phase. It recursively validates the complete
// tree, not just the safe factories used by the current parser.
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
        if (!source_symbol(name))
            throw std::invalid_argument("GPU-LAUNCH-003: malformed geometry symbol");
        return {Kind::Symbol, 0, std::move(name), {}, {}};
    }
    static I64 ceil_div(I64 a, I64 b) {
        // Prevent folding an aggregate-forged, noncanonical constant and
        // thereby laundering unchecked source payload into a fresh constant.
        const auto malformed = [](const I64& n) {
            return n.kind == Kind::Constant &&
                (n.value < 0 || !n.symbol.empty() || n.left || n.right);
        };
        if (malformed(a) || malformed(b) ||
                (b.kind == Kind::Constant && b.value == 0))
            throw std::invalid_argument("GPU-LAUNCH-003: invalid ceil_div operand");
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

inline bool source_symbol(const std::string& value) {
    if (value.empty() || value.size() > 256) return false;
    bool first = true;
    for (const unsigned char c : value) {
        if (c == '.') {
            if (first) return false;
            first = true;
            continue;
        }
        const bool alpha = (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if ((!alpha && (first || !digit))) return false;
        first = false;
    }
    return !first;
}

inline void require_name(const std::string& value, const char* part) {
    if (!source_symbol(value))
        throw std::invalid_argument(std::string("GPU-LAUNCH-001: invalid ") + part);
}

// The I64 structure is a compiler-interchange aggregate; factory validation
// alone is insufficient. A hand-built nested tree could otherwise evade
// negative, missing-child and div-zero checks. Limits stop excessive nesting,
// huge DAGs and cycles before a later compiler phase evaluates any expression.
inline void validate_i64(const I64& n, unsigned depth, std::size_t& budget,
                         std::unordered_set<const I64*>& ancestors) {
    if (depth > 16 || ++budget > 256 || !ancestors.insert(&n).second)
        throw std::invalid_argument("GPU-LAUNCH-003: malformed/deep/cyclic integer geometry");
    switch (n.kind) {
        case I64::Kind::Constant:
            if (n.value < 0 || !n.symbol.empty() || n.left || n.right)
                throw std::invalid_argument("GPU-LAUNCH-003: malformed constant geometry");
            break;
        case I64::Kind::Symbol:
            if (!source_symbol(n.symbol) || n.value != 0 || n.left || n.right)
                throw std::invalid_argument("GPU-LAUNCH-003: malformed symbolic geometry");
            break;
        case I64::Kind::CeilDiv:
            if (n.value != 0 || !n.symbol.empty() || !n.left || !n.right)
                throw std::invalid_argument("GPU-LAUNCH-003: malformed ceil_div geometry");
            validate_i64(*n.left, depth + 1, budget, ancestors);
            validate_i64(*n.right, depth + 1, budget, ancestors);
            if (n.right->kind == I64::Kind::Constant && n.right->value == 0)
                throw std::invalid_argument("GPU-LAUNCH-003: ceil_div by zero");
            break;
        default:
            throw std::invalid_argument("GPU-LAUNCH-003: unsupported integer geometry kind");
    }
    ancestors.erase(&n);
}

inline Candidate source_candidate(Candidate value) {
    require_name(value.kernel, "kernel symbol");
    require_name(value.device, "device symbol");
    require_name(value.queue, "queue symbol");
    if (value.ordered_arguments.size() > 64)
        throw std::invalid_argument("GPU-LAUNCH-002: too many kernel arguments");
    for (const auto& a : value.ordered_arguments)
        require_name(a, "kernel argument");
    if (value.geometry.mode != Mode::PhysicalGridBlock &&
            value.geometry.mode != Mode::LogicalOverGroup)
        throw std::invalid_argument("GPU-LAUNCH-003: unknown geometry mode");
    if (value.geometry.extent.size() != 3 || value.geometry.group.size() != 3)
        throw std::invalid_argument("GPU-LAUNCH-003: launch axes must normalize to 3D");
    std::size_t nodes = 0;
    std::unordered_set<const I64*> ancestors;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto& e = value.geometry.extent[axis];
        const auto& g = value.geometry.group[axis];
        validate_i64(e, 0, nodes, ancestors);
        validate_i64(g, 0, nodes, ancestors);
        if (g.kind == I64::Kind::Constant && g.value == 0)
            throw std::invalid_argument("GPU-LAUNCH-003: group/block dimension must be positive");
        if (value.geometry.mode == Mode::PhysicalGridBlock &&
                e.kind == I64::Kind::Constant && e.value == 0)
            throw std::invalid_argument("GPU-LAUNCH-003: physical grid dimension must be positive");
    }
    validate_i64(value.shared_bytes, 0, nodes, ancestors);
    return value;
}

}  // namespace oreslang::gpu::source_mir
