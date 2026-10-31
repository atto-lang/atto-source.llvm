#pragma once
// Compiler-owned typed SoA HIR admission. Does not emit or authorize LLVM IR.
// Pure value-schema analysis; all guest allocation and row/loan lowering remain
// fail-closed until wired into the production owner-lifetime verifier.
#include "oreslang/frontend/Parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::dop::hir {

struct Column {
    std::string name;
    InvertedScalarKind type;
    std::size_t element_bytes;
    std::size_t packed_row_offset;
    bool mutable_field;
};

class Layout final {
    std::string nominal_name_;
    std::vector<Column> columns_;
    std::size_t packed_row_bytes_ = 0;
    bool exported_ = false;

public:
    static Layout from_decl(const InvertedStructDecl& decl) {
        if (decl.name.empty() || decl.name.size() > 256)
            throw std::invalid_argument("inverted schema requires a bounded nominal name");
        if (decl.fields.empty() || decl.fields.size() > 64)
            throw std::invalid_argument("inverted schema requires 1..64 columns");
        Layout layout;
        layout.nominal_name_ = decl.name;
        layout.exported_ = decl.exported;
        std::unordered_set<std::string> names;
        for (const auto& f : decl.fields) {
            if (f.name.empty() || f.name.size() > 256
                    || !names.insert(f.name).second)
                throw std::invalid_argument("invalid or duplicate inverted field name");
            std::size_t width = 0;
            switch (f.type) {
                case InvertedScalarKind::I64: width = 8; break;
                case InvertedScalarKind::Bool: width = 1; break;
                case InvertedScalarKind::F64: width = 8; break;
                default: throw std::invalid_argument("unsupported inverted scalar field kind");
            }
            if (layout.packed_row_bytes_ > 64 * 1024 - width)
                throw std::overflow_error("inverted row exceeds 64 KiB packed budget");
            layout.columns_.push_back(Column{f.name, f.type, width,
                                              layout.packed_row_bytes_, f.mutable_field});
            layout.packed_row_bytes_ += width;
        }
        return layout;
    }

    const std::string& name() const noexcept { return nominal_name_; }
    bool exported() const noexcept { return exported_; }
    std::size_t columns() const noexcept { return columns_.size(); }
    std::size_t packed_row_bytes() const noexcept { return packed_row_bytes_; }
    const Column& column(std::size_t index) const { return columns_.at(index); }
    std::size_t field_index(std::string_view name) const {
        for (std::size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].name == name) return i;
        throw std::invalid_argument("unknown inverted field");
    }
    std::size_t capacity_bytes(std::size_t capacity) const {
        if (capacity == 0)
            throw std::invalid_argument("inverted capacity must be positive");
        if (packed_row_bytes_ != 0 &&
                capacity > (16 * 1024 * 1024) / packed_row_bytes_)
            throw std::overflow_error("inverted store exceeds 16 MiB admitted layout budget");
        return capacity * packed_row_bytes_;
    }
};

enum class OpKind {
    Allocate, Push, ReadField, WriteField, BorrowRead, BorrowWrite,
    ReleaseRead, ReleaseWrite, EraseSwap, Snapshot, Suspend, Close
};

struct Operation {
    OpKind kind;
    // An index into schema.columns for field operations; field_count for
    // operations with no particular field. This is NOT a guest pointer.
    std::size_t column;
    std::size_t index;
};

class LinearPlan final {
    Layout layout_;
    std::size_t capacity_;
    std::size_t logical_length_ = 0;
    std::vector<std::size_t> readers_;
    std::vector<bool> writers_;
    std::vector<Operation> operations_;
    bool closed_ = false;

    void open() const {
        if (closed_) throw std::logic_error("closed inverted HIR owner");
    }
    bool any_loans() const {
        for (std::size_t i = 0; i < readers_.size(); ++i)
            if (readers_[i] || writers_[i]) return true;
        return false;
    }
    void no_loans() const {
        if (any_loans())
            throw std::logic_error("inverted structural operation conflicts with active loan");
    }
    std::size_t checked_field(std::string_view name) const {
        open();
        return layout_.field_index(name);
    }
    void checked_row(std::size_t index) const {
        open();
        if (index >= logical_length_)
            throw std::out_of_range("inverted row index outside current length");
    }
    void append(OpKind kind, std::size_t column = 0, std::size_t row = 0) {
        // Bound compiler-memory and dataflow work. This is an admission
        // guard, not a runtime allocation cap or a proven loop bound.
        if (operations_.size() >= 4096)
            throw std::overflow_error("inverted typed HIR operation budget exceeded");
        operations_.push_back(Operation{kind, column, row});
    }
public:
    LinearPlan(Layout layout, std::size_t capacity)
        : layout_(std::move(layout)), capacity_(capacity),
          readers_(layout_.columns(), 0), writers_(layout_.columns(), false) {
        (void)layout_.capacity_bytes(capacity_);
        append(OpKind::Allocate, layout_.columns(), capacity_);
    }

    const Layout& layout() const noexcept { return layout_; }
    std::size_t length() const noexcept { return logical_length_; }
    bool closed() const noexcept { return closed_; }
    const std::vector<Operation>& operations() const noexcept { return operations_; }

    // Only a fully validated row of exactly the nominal schema arity is
    // admitted. Actual payload typing occurs in the source expression checker.
    void push(std::size_t supplied_fields) {
        open(); no_loans();
        if (supplied_fields != layout_.columns())
            throw std::invalid_argument("inverted row arity mismatch");
        if (logical_length_ >= capacity_)
            throw std::out_of_range("inverted collection capacity exceeded");
        append(OpKind::Push, layout_.columns(), logical_length_);
        ++logical_length_;
    }

    std::size_t read(std::size_t index, std::string_view field) {
        checked_row(index);
        const auto col = checked_field(field);
        if (writers_[col])
            throw std::logic_error("inverted field has an outstanding mutable loan");
        append(OpKind::ReadField, col, index);
        return col;
    }

    std::size_t write(std::size_t index, std::string_view field) {
        checked_row(index);
        const auto col = checked_field(field);
        if (!layout_.column(col).mutable_field)
            throw std::logic_error("immutable inverted field cannot be written after initialization");
        if (writers_[col] || readers_[col])
            throw std::logic_error("inverted field has an outstanding loan");
        append(OpKind::WriteField, col, index);
        return col;
    }

    // Source indices are runtime values in the general compiler. The
    // backend MUST emit an owner-relative bounds/validity guard; unlike the
    // above constant-folding checks, these operations do not assume that a
    // compile-time row count proves source indexing safety.
    std::size_t read_runtime_checked(std::size_t index, std::string_view field) {
        const auto col = checked_field(field);
        if (writers_[col])
            throw std::logic_error("inverted field has an outstanding mutable loan");
        append(OpKind::ReadField, col, index);
        return col;
    }

    std::size_t write_runtime_checked(std::size_t index, std::string_view field) {
        const auto col = checked_field(field);
        if (!layout_.column(col).mutable_field)
            throw std::logic_error("immutable inverted field cannot be written after initialization");
        if (writers_[col] || readers_[col])
            throw std::logic_error("inverted field has an outstanding loan");
        append(OpKind::WriteField, col, index);
        return col;
    }

    void borrow_read(std::string_view field) {
        const auto col = checked_field(field);
        if (writers_[col])
            throw std::logic_error("inverted field already mutably borrowed");
        if (readers_[col] == std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("inverted read borrow count exhausted");
        append(OpKind::BorrowRead, col);
        ++readers_[col];
    }

    void borrow_write(std::string_view field) {
        const auto col = checked_field(field);
        if (!layout_.column(col).mutable_field)
            throw std::logic_error("immutable inverted field cannot be borrowed mutably");
        if (writers_[col] || readers_[col])
            throw std::logic_error("inverted field already borrowed");
        append(OpKind::BorrowWrite, col);
        writers_[col] = true;
    }

    void release_read(std::string_view field) {
        const auto col = checked_field(field);
        if (!readers_[col]) throw std::logic_error("unmatched inverted read release");
        append(OpKind::ReleaseRead, col);
        --readers_[col];
    }

    void release_write(std::string_view field) {
        const auto col = checked_field(field);
        if (!writers_[col]) throw std::logic_error("unmatched inverted write release");
        append(OpKind::ReleaseWrite, col);
        writers_[col] = false;
    }

    void erase_swap(std::size_t index) {
        checked_row(index);
        no_loans();
        append(OpKind::EraseSwap, layout_.columns(), index);
        --logical_length_;
        // Generational row-handle invalidation is a *runtime* responsibility.
        // This sequential HIR cannot mint handles or prove stale identity.
    }

    void snapshot(std::size_t index) {
        checked_row(index);
        for (bool writer : writers_)
            if (writer) throw std::logic_error("inverted snapshot conflicts with mutable loan");
        append(OpKind::Snapshot, layout_.columns(), index);
    }

    void suspend() {
        open();
        // The owner is stack/region-bound, and no pinned continuation or
        // actor quiescence contract has been proved in this first linear
        // HIR. Even loan-free suspension is denied until that exists.
        throw std::logic_error(
            "inverted owner cannot cross suspension without proven pinned lifetime");
    }

    void close() {
        open(); no_loans();
        append(OpKind::Close, layout_.columns());
        closed_ = true;
        logical_length_ = 0;
    }
};

} // namespace oreslang::dop::hir
