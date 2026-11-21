#pragma once
// Bounded, verified-MIR native LLVM IR for one local bool/i64 inverted struct.
// Separate from existing two-i64 lowering; no escaped handles or FFI/actors.
// The boolean lane uses actual i1 cells, not widened i64 payloads.
#include "oreslang/types/InvertedMirLlvmI64.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::dop::mir {

class BoolI64LlvmEmitter final {
    std::size_t capacity_;
    std::string ir_;
    std::size_t next_ = 0;
    std::size_t pushes_ = 0;
    std::size_t writes_ = 0;
    std::string observed_;

    std::string temporary() { return "%v" + std::to_string(++next_); }
    std::string block() { return "b" + std::to_string(++next_); }
    void emit(const std::string& line) { ir_ += "  " + line + "\n"; }
    static const char* column_type(std::size_t index) {
        if (index == 0) return "i1";
        if (index == 1) return "i64";
        // Even an externally reconstructed MIR cannot silently alias an
        // invalid column onto the second i64 lane.
        throw std::out_of_range("mixed inverted LLVM field index outside schema");
    }
    std::string ptr(const std::string& name, const std::string& type,
                    const std::string& index) {
        const auto out = temporary();
        emit(out + " = getelementptr inbounds [" + std::to_string(capacity_) +
             " x " + type + "], ptr " + name + ", i64 0, i64 " + index);
        return out;
    }
    std::string load(const std::string& type, const std::string& address) {
        const auto out = temporary();
        emit(out + " = load " + type + ", ptr " + address);
        return out;
    }
    void store(const std::string& type, const std::string& value,
               const std::string& address, bool volatile_store = false) {
        emit(std::string("store ") + (volatile_store ? "volatile " : "") +
             type + " " + value + ", ptr " + address);
    }
    void trap_unless(const std::string& condition) {
        const auto ok = block(), bad = block();
        emit("br i1 " + condition + ", label %" + ok + ", label %" + bad);
        ir_ += bad + ":\n";
        emit("call void @llvm.trap()");
        emit("unreachable");
        ir_ += ok + ":\n";
    }
    void owner_guard() { trap_unless(load("i1", "%alive")); }
    std::string checked_row(std::size_t index) {
        if (index > static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max()))
            throw std::out_of_range("mixed inverted row index exceeds signed i64");
        const auto row = std::to_string(index);
        const auto length = load("i64", "%length");
        const auto within = temporary();
        emit(within + " = icmp ult i64 " + row + ", " + length);
        trap_unless(within);
        const auto marker = load("i64", ptr("%generation", "i64", row));
        const auto live = temporary();
        emit(live + " = icmp ne i64 " + marker + ", 0");
        trap_unless(live);
        return row;
    }

public:
    explicit BoolI64LlvmEmitter(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0 || capacity_ > 256)
            throw std::invalid_argument("mixed inverted LLVM capacity must be 1..256");
    }

    std::string build(const Module& module, const I64PairIrOperands& values) {
        // Compiler reuse and failed attempts never publish stale SSA state.
        ir_.clear(); next_ = 0; pushes_ = 0; writes_ = 0; observed_.clear();
        const auto& shape = module.layout();
        if (module.capacity() != capacity_ || shape.columns() != 2 ||
                shape.column(0).type != InvertedScalarKind::Bool ||
                shape.column(1).type != InvertedScalarKind::I64)
            throw std::invalid_argument("mixed inverted emitter requires bool/i64 schema");
        const auto& operations = module.instructions();
        if (operations.size() < 3 ||
                operations.back().kind != OpKind::Close ||
                operations[operations.size()-2].kind != OpKind::ReadField ||
                operations[operations.size()-2].column != 1)
            throw std::logic_error("mixed inverted LLVM requires terminal i64 read and close");
        for (const auto& op : operations) {
            if (op.kind == OpKind::Snapshot || op.kind == OpKind::Suspend)
                throw std::logic_error("mixed inverted LLVM cannot lower snapshot/suspension");
        }
        ir_ = "; verified MIR, local mixed bool/i64 SoA guest; no external handles\n";
        ir_ += "declare void @llvm.trap()\n";
        ir_ += "define i32 @main() {\nentry:\n";
        emit("%length = alloca i64");
        store("i64", "0", "%length");
        emit("%alive = alloca i1");
        store("i1", "true", "%alive");
        emit("%col0 = alloca [" + std::to_string(capacity_) + " x i1]");
        emit("%col1 = alloca [" + std::to_string(capacity_) + " x i64]");
        emit("%generation = alloca [" + std::to_string(capacity_) + " x i64]");
        for (const auto& op : operations) {
            if (op.kind == OpKind::Allocate) continue;
            owner_guard();
            switch (op.kind) {
                case OpKind::Push: {
                    if (pushes_ == values.rows.size())
                        throw std::invalid_argument("mixed inverted missing push operands");
                    const auto& pair = values.rows[pushes_++];
                    if (pair[0] != 0 && pair[0] != 1)
                        throw std::invalid_argument("mixed inverted forged boolean insertion");
                    const auto length = load("i64", "%length");
                    const auto room = temporary();
                    emit(room + " = icmp ult i64 " + length + ", " +
                         std::to_string(capacity_));
                    trap_unless(room);
                    store("i1", std::to_string(pair[0]), ptr("%col0", "i1", length));
                    store("i64", std::to_string(pair[1]), ptr("%col1", "i64", length));
                    store("i64", "1", ptr("%generation", "i64", length));
                    const auto following = temporary();
                    emit(following + " = add i64 " + length + ", 1");
                    store("i64", following, "%length");
                    break;
                }
                case OpKind::WriteField: {
                    if (writes_ == values.writes.size())
                        throw std::invalid_argument("mixed inverted missing write operand");
                    const auto value = values.writes[writes_++];
                    if (op.column == 0 && value != 0 && value != 1)
                        throw std::invalid_argument("mixed inverted forged boolean write");
                    const auto row = checked_row(op.index);
                    store(column_type(op.column), std::to_string(value),
                          ptr(op.column ? "%col1" : "%col0",
                              column_type(op.column), row));
                    break;
                }
                case OpKind::ReadField:
                    if (op.column != 1)
                        throw std::logic_error("mixed inverted bool-to-int read prohibited");
                    observed_ = load("i64", ptr("%col1", "i64", checked_row(op.index)));
                    break;
                case OpKind::EraseSwap: {
                    const auto index = checked_row(op.index);
                    const auto length = load("i64", "%length");
                    const auto last = temporary();
                    emit(last + " = sub i64 " + length + ", 1");
                    for (const auto& lane : {
                            std::pair<const char*, const char*>{"%col0","i1"},
                            {"%col1","i64"}, {"%generation","i64"}}) {
                        const auto tail = ptr(lane.first, lane.second, last);
                        const auto value = load(lane.second, tail);
                        store(lane.second, value, ptr(lane.first, lane.second, index));
                        store(lane.second, "0", tail);
                    }
                    store("i64", last, "%length");
                    break;
                }
                case OpKind::BorrowRead: case OpKind::BorrowWrite:
                case OpKind::ReleaseRead: case OpKind::ReleaseWrite:
                    // Verified MIR loans never yield an escaped native pointer.
                    break;
                case OpKind::Close:
                    store("i1", "false", "%alive", true);
                    for (std::size_t cell = 0; cell < capacity_; ++cell) {
                        const auto index = std::to_string(cell);
                        store("i1", "0", ptr("%col0", "i1", index), true);
                        store("i64", "0", ptr("%col1", "i64", index), true);
                        store("i64", "0", ptr("%generation", "i64", index), true);
                    }
                    store("i64", "0", "%length", true);
                    break;
                default:
                    throw std::logic_error("unsupported mixed inverted MIR operation");
            }
        }
        if (observed_.empty() || pushes_ != values.rows.size() ||
                writes_ != values.writes.size())
            throw std::invalid_argument("mixed inverted missing return or unused operands");
        const auto result = temporary();
        emit(result + " = trunc i64 " + observed_ + " to i32");
        emit("ret i32 " + result);
        ir_ += "}\n";
        return ir_;
    }
};

inline std::string emit_native_bool_i64_pair_ir(
        const Module& verified, const I64PairIrOperands& operands) {
    return BoolI64LlvmEmitter(verified.capacity()).build(verified, operands);
}

} // namespace oreslang::dop::mir
