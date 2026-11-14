#pragma once
// Checked typed MIR for one-level inverted columns. Experimental i64-pair
// native executor below is a bridge/verification target, NOT production LLVM IR.
#include "oreslang/types/InvertedTypedPlan.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oreslang::dop::mir {
using hir::OpKind;
using hir::Layout;
using hir::LinearPlan;

struct Instruction {
    OpKind kind;
    std::size_t column;
    std::size_t index;
    bool require_owner_guard;
    bool require_generation_guard;
    bool require_bounds_guard;
};

class Module final {
    Layout layout_;
    std::size_t capacity_;
    std::vector<Instruction> code_;

    static bool uses_field(OpKind kind) {
        return kind == OpKind::ReadField || kind == OpKind::WriteField ||
               kind == OpKind::BorrowRead || kind == OpKind::BorrowWrite ||
               kind == OpKind::ReleaseRead || kind == OpKind::ReleaseWrite;
    }
    static bool uses_row(OpKind kind) {
        return kind == OpKind::ReadField || kind == OpKind::WriteField ||
               kind == OpKind::EraseSwap || kind == OpKind::Snapshot;
    }
    Module(Layout layout, std::size_t capacity, std::vector<Instruction> code)
        : layout_(std::move(layout)), capacity_(capacity), code_(std::move(code)) {
        validate();
    }
    void validate() const {
        (void)layout_.capacity_bytes(capacity_);
        if (code_.empty() || code_.size() > 4096 ||
                code_.front().kind != OpKind::Allocate ||
                code_.front().index != capacity_ ||
                code_.front().column != layout_.columns())
            throw std::invalid_argument("inverted MIR requires one exact allocation");
        std::vector<std::size_t> readers(layout_.columns(), 0);
        std::vector<bool> writers(layout_.columns(), false);
        std::size_t length = 0;
        bool closed = false;
        for (std::size_t i = 0; i < code_.size(); ++i) {
            const auto& op = code_[i];
            if (closed) throw std::logic_error("inverted MIR operation after close");
            const bool field = uses_field(op.kind);
            const bool row = uses_row(op.kind);
            if (row && op.index >
                    static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
                throw std::out_of_range("inverted MIR runtime index exceeds signed i64");
            if (op.require_owner_guard != (i != 0) ||
                    op.require_generation_guard != row ||
                    op.require_bounds_guard != row)
                throw std::logic_error(
                    "inverted MIR mandatory owner/generation/bounds guard missing");
            if (field) {
                if (op.column >= layout_.columns())
                    throw std::out_of_range("inverted MIR field index outside schema");
            } else if (op.column != layout_.columns()) {
                throw std::logic_error("inverted MIR non-field column marker invalid");
            }
            // An external MIR record must not hide an unused/forged row
            // coordinate in a field-wide loan or owner close. These
            // operations carry no row and are always emitted with index 0.
            // Reject rather than silently discarding noncanonical metadata.
            if ((op.kind == OpKind::BorrowRead || op.kind == OpKind::BorrowWrite ||
                    op.kind == OpKind::ReleaseRead || op.kind == OpKind::ReleaseWrite ||
                    op.kind == OpKind::Close) && op.index != 0)
                throw std::logic_error("inverted MIR non-row instruction has forged row index");
            bool any_loan = false;
            for (std::size_t c = 0; c < readers.size(); ++c)
                any_loan |= readers[c] != 0 || writers[c];
            switch (op.kind) {
                case OpKind::Allocate:
                    if (i != 0) throw std::logic_error("duplicate inverted MIR owner allocation");
                    break;
                case OpKind::Push:
                    if (any_loan || length == capacity_ || op.index != length)
                        throw std::logic_error("invalid inverted MIR insertion");
                    ++length;
                    break;
                case OpKind::ReadField:
                    if (writers[op.column])
                        throw std::logic_error("inverted MIR read conflicts with mutable loan");
                    break;
                case OpKind::WriteField:
                    if (!layout_.column(op.column).mutable_field ||
                            writers[op.column] || readers[op.column])
                        throw std::logic_error("inverted MIR immutable or borrowed field write");
                    break;
                case OpKind::BorrowRead:
                    if (writers[op.column] ||
                            readers[op.column] == std::numeric_limits<std::size_t>::max())
                        throw std::logic_error("inverted MIR reader conflict or overflow");
                    ++readers[op.column];
                    break;
                case OpKind::BorrowWrite:
                    if (!layout_.column(op.column).mutable_field ||
                            writers[op.column] || readers[op.column])
                        throw std::logic_error("inverted MIR writer conflict or immutable field");
                    writers[op.column] = true;
                    break;
                case OpKind::ReleaseRead:
                    if (!readers[op.column])
                        throw std::logic_error("inverted MIR unmatched read release");
                    --readers[op.column];
                    break;
                case OpKind::ReleaseWrite:
                    if (!writers[op.column])
                        throw std::logic_error("inverted MIR unmatched write release");
                    writers[op.column] = false;
                    break;
                case OpKind::EraseSwap:
                    if (any_loan || op.index >= length)
                        throw std::logic_error("invalid inverted MIR erase");
                    --length;
                    break;
                case OpKind::Snapshot:
                    if (op.index >= length)
                        throw std::logic_error("invalid inverted MIR snapshot index");
                    for (bool w : writers)
                        if (w) throw std::logic_error("inverted MIR snapshot conflicts with writer");
                    break;
                case OpKind::Suspend:
                    throw std::logic_error("inverted MIR suspension lacks owner pinning");
                case OpKind::Close:
                    if (any_loan)
                        throw std::logic_error("inverted MIR close with live loans");
                    closed = true;
                    break;
                default: throw std::logic_error("unknown inverted MIR instruction");
            }
        }
        // Every admitted owner must have exactly one terminal close.
        // Otherwise forged/deserialized MIR could bypass deterministic
        // teardown entirely. "No operations after close" alone is not enough.
        if (!closed)
            throw std::logic_error("inverted MIR requires terminal close");
    }
public:
    static Module lower(const LinearPlan& source) {
        const auto& ops = source.operations();
        if (ops.empty() || ops.size() > 4096)
            throw std::invalid_argument("invalid inverted HIR program length");
        const auto capacity = ops.front().index;
        std::vector<Instruction> code;
        code.reserve(ops.size());
        for (std::size_t i = 0; i < ops.size(); ++i) {
            const auto& op = ops[i];
            const bool row = uses_row(op.kind);
            code.push_back(Instruction{op.kind, op.column, op.index,
                                       i != 0, row, row});
        }
        return Module(source.layout(), capacity, std::move(code));
    }
    // Independently re-verify untrusted/deserialized operations; flags alone
    // never authorize a field, row, or ownership effect.
    static Module verify_external(Layout schema, std::size_t capacity,
                                  std::vector<Instruction> code) {
        return Module(std::move(schema), capacity, std::move(code));
    }
    const Layout& layout() const noexcept { return layout_; }
    std::size_t capacity() const noexcept { return capacity_; }
    const std::vector<Instruction>& instructions() const noexcept { return code_; }
};

} // namespace oreslang::dop::mir
