#pragma once

#include <cstdint>
#include <limits>

namespace oreslang::runtime {

// Pure, allocation-free rank-2 descriptor admission compatible with the
// Java/Graal source stdlib/tensor.ores v1. This is HOST preflight only;
// it does NOT grant owner authority or substitute for compiled guest code.
struct TensorView2D final {
    std::int64_t rows, cols, row_stride, col_stride;
    std::int64_t element_bytes, capacity_bytes;
    bool writable;
};

struct Tensor2DAdmission final {
    static constexpr std::int64_t max_i64 =
        std::numeric_limits<std::int64_t>::max();

    static bool valid(const TensorView2D& v) noexcept {
        if (v.rows < 0 || v.cols < 0 ||
            v.row_stride < 0 || v.col_stride < 0 ||
            v.element_bytes <= 0 || v.capacity_bytes < 0)
            return false;
        if (v.rows == 0 || v.cols == 0)
            return true;
        if (v.writable &&
            ((v.rows > 1 && v.row_stride == 0) ||
             (v.cols > 1 && v.col_stride == 0)))
            return false;

        const auto rows_minus_one = v.rows - 1;
        const auto cols_minus_one = v.cols - 1;
        if (rows_minus_one > 0 &&
            v.row_stride > max_i64 / rows_minus_one)
            return false;
        if (cols_minus_one > 0 &&
            v.col_stride > max_i64 / cols_minus_one)
            return false;
        if (v.writable && v.rows > 1 && v.cols > 1) {
            if (v.col_stride > max_i64 / v.cols)
                return false;
            if (v.row_stride < v.cols * v.col_stride)
                return false;
        }
        const auto row_span = rows_minus_one * v.row_stride;
        const auto col_span = cols_minus_one * v.col_stride;
        if (row_span > max_i64 - col_span)
            return false;
        const auto last_index = row_span + col_span;
        if (last_index == max_i64)
            return false;
        const auto required_elements = last_index + 1;
        if (v.element_bytes > max_i64 / required_elements)
            return false;
        return required_elements * v.element_bytes <= v.capacity_bytes;
    }

    static bool contiguous(const TensorView2D& v) noexcept {
        if (!valid(v) || v.col_stride != 1 || v.row_stride != v.cols)
            return false;
        if (v.rows == 0 || v.cols == 0)
            return v.capacity_bytes == 0;
        if (v.rows > max_i64 / v.cols)
            return false;
        const auto elements = v.rows * v.cols;
        if (v.element_bytes > max_i64 / elements)
            return false;
        return v.capacity_bytes == elements * v.element_bytes;
    }

    static bool broadcasts(std::int64_t lhs_rows, std::int64_t lhs_cols,
                           std::int64_t rhs_rows, std::int64_t rhs_cols) noexcept {
        if (lhs_rows < 0 || lhs_cols < 0 || rhs_rows < 0 || rhs_cols < 0)
            return false;
        if (lhs_rows != rhs_rows && lhs_rows != 1 && rhs_rows != 1)
            return false;
        if (lhs_cols != rhs_cols && lhs_cols != 1 && rhs_cols != 1)
            return false;
        return true;
    }
};

} // namespace oreslang::runtime
