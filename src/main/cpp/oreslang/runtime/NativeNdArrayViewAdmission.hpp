#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Compiler-facing pure preflight for strided, item-aligned views of an already
 * authenticated owned buffer. No guest pointer escape, buffer allocation or
 * borrow lease; at most 16 axis pairs use fixed stack-local metadata.
 * The caller must obtain storage_bytes from the real owner's current region.
 * A write grant also needs a separate ownership/alias loan over this range.
 *
 * This is a conservative O(rank log rank) sufficient non-overlap proof,
 * not a complete solution to arbitrary integer stride injectivity.
 */
struct NativeNdArrayViewAdmission final {
    struct Range { std::uint64_t begin, end; };
    static constexpr std::uint64_t max_index =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    static Range check(const std::vector<std::uint64_t>& shape,
                       const std::vector<std::int64_t>& strides_bytes,
                       std::uint64_t offset_bytes, std::uint64_t storage_bytes,
                       std::uint64_t itemsize, bool writable) {
        if (shape.size() > 16 || shape.size() != strides_bytes.size())
            throw std::invalid_argument("NdArray rank/stride mismatch");
        if (!itemsize || itemsize > 64 || offset_bytes > max_index ||
                storage_bytes > max_index || offset_bytes % itemsize)
            throw std::invalid_argument("NdArray element alignment/extent invalid");
        bool empty = false;
        std::array<std::pair<std::uint64_t, std::uint64_t>, 16> axes{};
        std::size_t axis_count = 0;
        for (std::size_t i = 0; i < shape.size(); ++i) {
            if (shape[i] > max_index || strides_bytes[i] == std::numeric_limits<std::int64_t>::min())
                throw std::invalid_argument("NdArray dimension or stride overflow");
            const auto abs_stride = static_cast<std::uint64_t>(
                    strides_bytes[i] < 0 ? -strides_bytes[i] : strides_bytes[i]);
            if (abs_stride % itemsize)
                throw std::invalid_argument("unaligned NdArray stride");
            if (!shape[i]) empty = true;
            if (shape[i] > 1) axes[axis_count++] = {abs_stride, shape[i]};
        }
        if (empty) {
            if (offset_bytes > storage_bytes) throw std::out_of_range("empty view offset outside owner");
            return {offset_bytes, offset_bytes};
        }
        std::uint64_t low = offset_bytes, high = offset_bytes;
        for (std::size_t i = 0; i < shape.size(); ++i) {
            const auto stride = strides_bytes[i];
            const auto abs_stride = static_cast<std::uint64_t>(stride < 0 ? -stride : stride);
            if (shape[i] > 1 && abs_stride > max_index / (shape[i] - 1))
                throw std::overflow_error("NdArray stride extent overflow");
            const auto extent = (shape[i] - 1) * abs_stride;
            if (stride < 0) {
                if (low < extent) throw std::out_of_range("NdArray view before owner");
                low -= extent;
            } else {
                if (high > max_index - extent) throw std::overflow_error("NdArray view end overflow");
                high += extent;
            }
        }
        if (high > max_index - itemsize || high + itemsize > storage_bytes)
            throw std::out_of_range("NdArray view beyond owner");
        if (writable) {
            std::sort(axes.begin(), axes.begin() + axis_count);
            std::uint64_t span = itemsize;
            for (std::size_t i = 0; i < axis_count; ++i) {
                const auto& [stride, dim] = axes[i];
                if (stride < span)
                    throw std::invalid_argument("aliased mutable NdArray view");
                if (dim > 1 && stride > (max_index - span) / (dim - 1))
                    throw std::overflow_error("mutable NdArray span overflow");
                span += (dim - 1) * stride;
            }
        }
        return {low, high + itemsize};
    }
};

} // namespace oreslang::runtime
