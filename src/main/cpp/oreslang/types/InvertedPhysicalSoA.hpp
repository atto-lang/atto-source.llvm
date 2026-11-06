#pragma once
// ABI geometry for one-level inverted scalar columns. This is an immutable,
// bounded compiler HIR preflight, NOT guest allocation or LLVM codegen.
// A packed nominal record and a physically aligned SoA have different extents.

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::dop::abi {

struct ScalarLane {
    std::size_t element_bytes;
    std::size_t alignment;
};

class PhysicalSoA final {
    std::vector<ScalarLane> lanes_;
    std::vector<std::size_t> bases_;
    std::size_t capacity_ = 0;
    std::size_t bytes_ = 0;
    std::size_t packed_record_bytes_ = 0;

    static std::size_t aligned(std::size_t value, std::size_t alignment,
                               std::size_t limit) {
        const auto mask = alignment - 1;
        if (value > limit || mask > limit - value)
            throw std::overflow_error("inverted SoA alignment exceeds physical quota");
        const auto result = (value + mask) & ~mask;
        if (result > limit)
            throw std::overflow_error("inverted SoA alignment exceeds physical quota");
        return result;
    }

public:
    static PhysicalSoA make(const std::vector<ScalarLane>& lanes,
                            std::size_t capacity,
                            std::size_t max_bytes = 16u * 1024u * 1024u) {
        if (lanes.empty() || lanes.size() > 64)
            throw std::invalid_argument("inverted SoA requires 1..64 scalar lanes");
        if (capacity == 0)
            throw std::invalid_argument("inverted SoA capacity must be positive");
        PhysicalSoA plan;
        plan.lanes_ = lanes;
        plan.capacity_ = capacity;
        plan.bases_.reserve(lanes.size());
        for (const auto& lane : plan.lanes_) {
            // V1 only admits naturally aligned fixed scalar lanes. A future
            // extension must explicitly version its ABI before new widths.
            if (lane.element_bytes != lane.alignment ||
                (lane.element_bytes != 1 && lane.element_bytes != 2 &&
                 lane.element_bytes != 4 && lane.element_bytes != 8))
                throw std::invalid_argument("unsupported inverted SoA scalar lane ABI");
            if (plan.packed_record_bytes_ > 64u * 1024u - lane.element_bytes)
                throw std::overflow_error("inverted packed row exceeds 64 KiB");
            plan.packed_record_bytes_ += lane.element_bytes;
            const auto base = aligned(plan.bytes_, lane.alignment, max_bytes);
            if (capacity > (max_bytes - base) / lane.element_bytes)
                throw std::overflow_error("inverted SoA physical allocation exceeds quota");
            plan.bases_.push_back(base);
            plan.bytes_ = base + capacity * lane.element_bytes;
        }
        return plan;
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t columns() const noexcept { return lanes_.size(); }
    std::size_t bytes() const noexcept { return bytes_; }
    std::size_t packed_record_bytes() const noexcept { return packed_record_bytes_; }
    std::size_t column_base(std::size_t column) const { return bases_.at(column); }
    std::size_t element_bytes(std::size_t column) const {
        return lanes_.at(column).element_bytes;
    }
    std::size_t offset(std::size_t column, std::size_t row) const {
        if (row >= capacity_) throw std::out_of_range("inverted SoA row out of bounds");
        const auto base = bases_.at(column);
        // Every lane extent was checked on construction; arithmetic here
        // cannot overflow and the result is within allocated physical bytes.
        return base + row * lanes_[column].element_bytes;
    }
};

} // namespace oreslang::dop::abi
