#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::dop {

// Synthetic planner roots have no physical authority. Only HostBuffer may
// mint bound identities, which are retired when storage is closed.
class HostBuffer;
class RegionRoot {
    struct Identity {
        std::uint64_t rows;
        std::uint32_t fields;
        const bool allocation_bound;
        std::atomic<bool> retired{false};
        Identity(std::uint64_t r, std::uint32_t f, bool bound) noexcept
            : rows(r), fields(f), allocation_bound(bound) {}
    };
    std::shared_ptr<Identity> token_;
    explicit RegionRoot(std::shared_ptr<Identity> token) : token_(std::move(token)) {}
    static void check_shape(std::uint64_t rows, std::uint32_t fields) {
        if (fields == 0 || fields > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
                || rows > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("region shape exceeds shared signed ABI");
    }
    static RegionRoot create_bound(std::uint64_t rows, std::uint32_t fields) {
        check_shape(rows, fields);
        return RegionRoot(std::make_shared<Identity>(rows, fields, true));
    }
    void retire_bound() const noexcept {
        token_->retired.store(true, std::memory_order_release);
    }
    friend class HostBuffer;
public:
    static RegionRoot trusted_create(std::uint64_t rows, std::uint32_t fields) {
        check_shape(rows, fields);
        return RegionRoot(std::make_shared<Identity>(rows, fields, false));
    }
    std::uint64_t rows() const noexcept { return token_->rows; }
    std::uint32_t fields() const noexcept { return token_->fields; }
    bool same_root(const RegionRoot& other) const noexcept {
        return token_ == other.token_;
    }
    bool is_live_owned() const noexcept {
        return token_->allocation_bound
            && !token_->retired.load(std::memory_order_acquire);
    }
};

enum class RegionMode { Read, Write, Discard, Reduce, Atomic };

struct RegionAccess {
    const RegionRoot root;
    const std::uint32_t field;
    const std::uint64_t begin;
    const std::uint64_t end;
    const RegionMode mode;
    const std::string reduction;

    static std::uint64_t checked_end(const RegionRoot& r,
                                     std::uint64_t begin, std::uint64_t count) {
        if (begin > r.rows() || count > r.rows() - begin)
            throw std::out_of_range("DOP field or row slice out of bounds");
        return begin + count;
    }

    RegionAccess(RegionRoot region, std::uint32_t field_id,
                 std::uint64_t row_start, std::uint64_t row_count,
                 RegionMode privilege, std::string reduction_op = {})
        : root(std::move(region)), field(field_id), begin(row_start),
          end(checked_end(root, row_start, row_count)), mode(privilege),
          reduction(std::move(reduction_op)) {
        if (field >= root.fields())
            throw std::out_of_range("DOP field or row slice out of bounds");
        if (mode != RegionMode::Read && mode != RegionMode::Write
                && mode != RegionMode::Discard && mode != RegionMode::Reduce
                && mode != RegionMode::Atomic)
            throw std::invalid_argument("invalid DOP privilege");
        // Match Java String.trim() at the ABI boundary: reject every ASCII
        // control/space codepoint <= U+0020 at either end (not just CR/LF/TAB).
        const bool valid_operator = !reduction.empty()
            && static_cast<unsigned char>(reduction.front()) > 0x20
            && static_cast<unsigned char>(reduction.back()) > 0x20;
        if ((mode == RegionMode::Reduce && !valid_operator)
                || (mode != RegionMode::Reduce && !reduction.empty()))
            throw std::invalid_argument("reduction requires exactly one named operator");
     }
};

struct RegionTask {
    const std::string name;
    const std::vector<RegionAccess> accesses;
    RegionTask(std::string task_name, std::vector<RegionAccess> task_accesses)
        : name(std::move(task_name)), accesses(std::move(task_accesses)) {
        if (name.empty()
                || static_cast<unsigned char>(name.front()) <= 0x20
                || static_cast<unsigned char>(name.back()) <= 0x20)
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

// Checked physical admission is not an execution lease. The executor must
// acquire buffer read/write loans for the *entire* task before dispatch.
inline void require_live_owned(const RegionTask& task) {
    for (const auto& access : task.accesses) {
        if (!access.root.is_live_owned())
            throw std::logic_error("DOP region has no live owned allocation");
    }
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
