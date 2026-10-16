#pragma once

#include <cstdint>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::dop {

// Compiler/runtime planning only. A Root token is minted by trusted host code;
// it is not a guest capability, allocation handle, or proof of physical aliasing.
class RegionRoot {
    struct Identity {
        std::uint64_t rows;
        std::uint32_t fields;
    };
    std::shared_ptr<const Identity> token_;
    explicit RegionRoot(std::shared_ptr<const Identity> token) : token_(std::move(token)) {}
public:
    static RegionRoot trusted_create(std::uint64_t rows, std::uint32_t fields) {
        if (fields == 0 || fields > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
                || rows > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("region shape exceeds shared signed ABI");
        return RegionRoot(std::make_shared<const Identity>(Identity{rows, fields}));
    }
    std::uint64_t rows() const noexcept { return token_->rows; }
    std::uint32_t fields() const noexcept { return token_->fields; }
    bool same_root(const RegionRoot& other) const noexcept {
        return token_ == other.token_;
    }
};

enum class RegionMode { Read, Write, Discard, Reduce, Atomic };

struct RegionAccess {
    RegionRoot root;
    std::uint32_t field;
    std::uint64_t begin;
    std::uint64_t end;
    RegionMode mode;
    std::string reduction;

    RegionAccess(RegionRoot region, std::uint32_t field_id,
                 std::uint64_t row_start, std::uint64_t row_count,
                 RegionMode privilege, std::string reduction_op = {})
        : root(std::move(region)), field(field_id), begin(row_start), end(row_start),
          mode(privilege), reduction(std::move(reduction_op)) {
        if (field >= root.fields() || begin > root.rows()
                || row_count > root.rows() - begin)
            throw std::out_of_range("DOP field or row slice out of bounds");
        if (mode != RegionMode::Read && mode != RegionMode::Write
                && mode != RegionMode::Discard && mode != RegionMode::Reduce
                && mode != RegionMode::Atomic)
            throw std::invalid_argument("invalid DOP privilege");
        const auto trimmed = reduction.find_first_not_of(" \t\r\n");
        const bool valid_operator = trimmed != std::string::npos
            && trimmed == 0
            && reduction.find_last_not_of(" \t\r\n") == reduction.size() - 1;
        if ((mode == RegionMode::Reduce && !valid_operator)
                || (mode != RegionMode::Reduce && !reduction.empty()))
            throw std::invalid_argument("reduction requires exactly one named operator");
        end = begin + row_count; // checked above; cannot overflow
    }
};

struct RegionTask {
    std::string name;
    std::vector<RegionAccess> accesses;
    RegionTask(std::string task_name, std::vector<RegionAccess> task_accesses)
        : name(std::move(task_name)), accesses(std::move(task_accesses)) {
        if (name.empty() || name.find_first_not_of(" \t\r\n") == std::string::npos
                || name.find_first_not_of(" \t\r\n") != 0
                || name.find_last_not_of(" \t\r\n") != name.size() - 1)
            throw std::invalid_argument("invalid DOP task name");
    }
};

inline bool independent(const RegionAccess& left, const RegionAccess& right) {
    if (!left.root.same_root(right.root) || left.field != right.field
            || left.begin == left.end || right.begin == right.end
            || left.end <= right.begin || right.end <= left.begin)
        return true;
    // Even reductions with the same operator serialize until a backend
    // certifies its reduction tree, precision mode and merge operation.
    return left.mode == RegionMode::Read && right.mode == RegionMode::Read;
}

inline bool independent(const RegionTask& left, const RegionTask& right) {
    for (const auto& a : left.accesses)
        for (const auto& b : right.accesses)
            if (!independent(a, b)) return false;
    return true;
}

// Stable DAG levels: a later task is ordered behind every earlier conflicting
// task, not just the most recently scheduled batch.
inline std::vector<std::vector<std::size_t>> dependency_levels(
        const std::vector<RegionTask>& tasks) {
    std::vector<std::size_t> levels(tasks.size(), 0);
    std::size_t max_level = 0;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (!independent(tasks[i], tasks[j])) {
                if (levels[j] == tasks.size())
                    throw std::overflow_error("region dependency depth overflow");
                const auto candidate = levels[j] + 1;
                if (candidate > levels[i]) levels[i] = candidate;
            }
        }
        if (levels[i] > max_level) max_level = levels[i];
    }
    if (tasks.empty()) return {};
    std::vector<std::vector<std::size_t>> batches(max_level + 1);
    for (std::size_t i = 0; i < tasks.size(); ++i)
        batches[levels[i]].push_back(i);
    return batches;
}

} // namespace oreslang::dop
