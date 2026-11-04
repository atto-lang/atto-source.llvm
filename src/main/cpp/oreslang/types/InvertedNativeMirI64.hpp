#pragma once
// C++20 native ABI adapter for compiler-owned, C++17-compatible typed MIR.
// HOST reference only: no guest pointers and no production source lowering.
#include "oreslang/types/InvertedTypedMir.hpp"
#include "oreslang/runtime/DopInvertedColumns.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace oreslang::dop::mir {
// Executable, bounded backend bridge for precisely two i64 columns.
// Typed values are supplied separately from MIR (as real backend operands).
// Does not expose row pointers, spans or forgeable guest row handles.
struct I64PairInputs {
    std::vector<std::array<std::int64_t, 2>> rows;
    std::vector<std::int64_t> writes;
};
struct I64PairResult {
    std::vector<std::int64_t> reads;
    std::vector<std::array<std::int64_t, 2>> snapshots;
    std::size_t live_rows = 0;
};

inline I64PairResult execute_native_i64_pair(
        const Module& program, const I64PairInputs& operands) {
    if (program.layout().columns() != 2 ||
        program.layout().column(0).type != InvertedScalarKind::I64 ||
        program.layout().column(1).type != InvertedScalarKind::I64)
        throw std::invalid_argument(
            "native MIR pair executor requires exactly two i64 columns");
    using Store = InvertedColumns<std::int64_t, std::int64_t>;
    std::unique_ptr<Store> owner;
    std::optional<typename Store::template Loan<0, false>> read0;
    std::optional<typename Store::template Loan<1, false>> read1;
    std::optional<typename Store::template Loan<0, true>> write0;
    std::optional<typename Store::template Loan<1, true>> write1;
    // Shared physical reader per lane, independent logical reader count.
    std::array<std::size_t, 2> logical_readers{};
    I64PairResult result;
    std::size_t next_row = 0, next_write = 0;
    for (const auto& op : program.instructions()) {
        switch (op.kind) {
            case OpKind::Allocate:
                owner = std::make_unique<Store>();
                owner->reserve(program.capacity());
                break;
            case OpKind::Push:
                if (next_row == operands.rows.size())
                    throw std::invalid_argument("missing typed MIR push operands");
                owner->push(operands.rows[next_row][0], operands.rows[next_row][1]);
                ++next_row;
                break;
            case OpKind::ReadField:
                result.reads.push_back(op.column == 0
                    ? owner->template read_index<0>(static_cast<std::int64_t>(op.index))
                    : owner->template read_index<1>(static_cast<std::int64_t>(op.index)));
                break;
            case OpKind::WriteField:
                if (next_write == operands.writes.size())
                    throw std::invalid_argument("missing typed MIR write operand");
                if (op.column == 0)
                    owner->template write_index<0>(
                        static_cast<std::int64_t>(op.index), operands.writes[next_write]);
                else
                    owner->template write_index<1>(
                        static_cast<std::int64_t>(op.index), operands.writes[next_write]);
                ++next_write;
                break;
            case OpKind::BorrowRead:
                if (op.column == 0 && logical_readers[0]++ == 0)
                    read0.emplace(owner->template borrow_read<0>());
                else if (op.column == 1 && logical_readers[1]++ == 0)
                    read1.emplace(owner->template borrow_read<1>());
                break;
            case OpKind::BorrowWrite:
                if (op.column == 0) write0.emplace(owner->template borrow_write<0>());
                else write1.emplace(owner->template borrow_write<1>());
                break;
            case OpKind::ReleaseRead:
                if (op.column == 0 && --logical_readers[0] == 0) read0.reset();
                else if (op.column == 1 && --logical_readers[1] == 0) read1.reset();
                break;
            case OpKind::ReleaseWrite:
                if (op.column == 0) write0.reset();
                else write1.reset();
                break;
            case OpKind::EraseSwap:
                owner->erase_swap(owner->index(static_cast<std::int64_t>(op.index)));
                break;
            case OpKind::Snapshot: {
                const auto row = owner->get_row(
                    owner->index(static_cast<std::int64_t>(op.index)));
                result.snapshots.push_back({std::get<0>(row), std::get<1>(row)});
                break;
            }
            case OpKind::ReadSnapshotField:
                // Detached copy stays valid after mutation, erase_swap and
                // teardown; no owner-relative pointer or row handle escapes.
                result.reads.push_back(result.snapshots.at(op.index).at(op.column));
                break;
            case OpKind::Close:
                owner.reset();
                break;
            default: throw std::logic_error("unsupported native typed MIR opcode");
        }
    }
    if (next_row != operands.rows.size() || next_write != operands.writes.size())
        throw std::invalid_argument("unused typed MIR operands");
    result.live_rows = owner ? owner->size() : 0;
    return result;
}
} // namespace oreslang::dop::mir
