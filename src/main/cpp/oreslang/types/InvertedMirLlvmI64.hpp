#pragma once
// Restricted verified-MIR -> native LLVM IR for a LOCAL two-i64-column
// owner, now also used by the narrow default production-source compiler.
// No escaped handles, actor suspension, general inverted schemas or RT
// guarantees. C++17 compilation needs no LLVM SDK dependency.
#include "oreslang/types/InvertedTypedMir.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace oreslang::dop::mir {
struct I64PairIrOperands {
    std::vector<std::array<std::int64_t, 2>> rows;
    std::vector<std::int64_t> writes;
};
class I64PairLlvmEmitter final {
    std::string code_;
    unsigned next_ = 0;
    std::size_t cap_;
    std::size_t pushes_ = 0;
    std::size_t writes_ = 0;
    std::string last_read_ = "0";

    std::string temp() { return "%m" + std::to_string(++next_); }
    std::string label() { return "mblock" + std::to_string(++next_); }
    void emit(const std::string& s) { code_ += "  " + s + "\n"; }
    static std::string num(std::int64_t v) { return std::to_string(v); }
    std::string load(const std::string& pointer) {
        auto t = temp(); emit(t + " = load i64, ptr " + pointer); return t;
    }
    std::string slot_ptr(const std::string& lane, const std::string& index) {
        auto p = temp();
        emit(p + " = getelementptr inbounds [" + std::to_string(cap_) +
             " x i64], ptr " + lane + ", i64 0, i64 " + index);
        return p;
    }
    void guard(const std::string& condition) {
        const auto ok = label(), error = label();
        emit("br i1 " + condition + ", label %" + ok + ", label %" + error);
        code_ += error + ":\n";
        emit("call void @llvm.trap()"); emit("unreachable");
        code_ += ok + ":\n";
    }
    void owner_guard() {
        const auto p = temp(); emit(p + " = load i1, ptr %alive"); guard(p);
    }
    std::string index_guard(std::size_t row) {
        if (row > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::out_of_range("MIR index exceeds signed i64");
        const auto length = load("%length");
        const auto good = temp();
        emit(good + " = icmp ult i64 " + std::to_string(row) + ", " + length);
        guard(good);
        // Each dense index is resolved fresh; there is no external row handle
        // or stale generation token in this restricted source ABI. The lane's
        // positive generation is checked as a live slot discriminator.
        const auto gp = slot_ptr("%generation", std::to_string(row));
        const auto generation = load(gp);
        const auto valid = temp();
        emit(valid + " = icmp ne i64 " + generation + ", 0");
        guard(valid);
        return std::to_string(row);
    }
public:
    explicit I64PairLlvmEmitter(std::size_t capacity) : cap_(capacity) {
        if (capacity == 0 || capacity > 256)
            throw std::invalid_argument("experimental MIR LLVM columns capacity must be 1..256");
    }
    std::string build(const Module& module, const I64PairIrOperands& args) {
        // A compiler may reuse this emitter for several functions/units.
        // Each build is an independent verified MIR lowering transaction.
        // Never carry prior SSA names, input cursors or return values into
        // the next guest IR, including after a failed build.
        code_.clear();
        next_ = 0;
        pushes_ = 0;
        writes_ = 0;
        last_read_ = "0";
        if (module.layout().columns() != 2 ||
            module.layout().column(0).type != InvertedScalarKind::I64 ||
            module.layout().column(1).type != InvertedScalarKind::I64 ||
            module.capacity() != cap_)
            throw std::invalid_argument("experimental MIR LLVM requires matching two-i64 layout");
        // Verified MIR can represent an open owner for other contexts. This
        // standalone guest main cannot return without revoking and scrubbing
        // its nonescaping stack-local SoA owner.
        if (module.instructions().empty() ||
                module.instructions().back().kind != OpKind::Close)
            throw std::logic_error("MIR LLVM requires terminal owner close");
        // A detached snapshot is a value-producing operation in the native
        // reference. This restricted i64-pair emitter has no snapshot result
        // representation. Silently loading and discarding it would compile
        // an incorrect guest program; reject before emitting any LLVM IR.
        for (const auto& op : module.instructions()) {
            if (op.kind == OpKind::Snapshot)
                throw std::logic_error("MIR LLVM snapshot value lowering not implemented");
        }
        // A standalone native main must return an actually observed field.
        // Closing the owner alone must never publish an implicit return-zero.
        // Likewise, a read preceding later writes, pushes or swap-erases is
        // stale: the final source operation before cleanup must be a read.
        // Perform this check *before* any LLVM IR bytes are emitted.
        const auto& actions = module.instructions();
        if (actions.size() < 3 ||
                actions[actions.size() - 2].kind != OpKind::ReadField)
            throw std::logic_error("MIR LLVM requires terminal field read before close");
        code_ = "; typed MIR, experimental local i64 pair LLVM IR; no external row handles\n";
        code_ += "declare void @llvm.trap()\n";
        code_ += "define i32 @main() {\nentry:\n";
        emit("%length = alloca i64");
        emit("store i64 0, ptr %length");
        emit("%alive = alloca i1");
        emit("store i1 true, ptr %alive");
        for (const auto* name : {"%col0", "%col1", "%generation"})
            emit(std::string(name) + " = alloca [" + std::to_string(cap_) + " x i64]");
        for (const auto& op : module.instructions()) {
            if (op.kind == OpKind::Allocate) continue;
            owner_guard();
            switch (op.kind) {
                case OpKind::Push: {
                    if (pushes_ == args.rows.size())
                        throw std::invalid_argument("missing MIR LLVM push operands");
                    const auto length = load("%length");
                    const auto ok = temp();
                    emit(ok + " = icmp ult i64 " + length + ", " + std::to_string(cap_));
                    guard(ok);
                    emit("store i64 " + num(args.rows[pushes_][0]) + ", ptr " + slot_ptr("%col0", length));
                    emit("store i64 " + num(args.rows[pushes_][1]) + ", ptr " + slot_ptr("%col1", length));
                    emit("store i64 1, ptr " + slot_ptr("%generation", length));
                    const auto next = temp(); emit(next + " = add i64 " + length + ", 1");
                    emit("store i64 " + next + ", ptr %length"); ++pushes_;
                    break;
                }
                case OpKind::ReadField: {
                    const auto idx = index_guard(op.index);
                    last_read_ = load(slot_ptr(op.column ? "%col1" : "%col0", idx));
                    break;
                }
                case OpKind::WriteField: {
                    if (writes_ == args.writes.size())
                        throw std::invalid_argument("missing MIR LLVM write operand");
                    const auto idx = index_guard(op.index);
                    emit("store i64 " + num(args.writes[writes_++]) + ", ptr " +
                         slot_ptr(op.column ? "%col1" : "%col0", idx));
                    break;
                }
                case OpKind::Snapshot:
                    throw std::logic_error("MIR LLVM snapshot value lowering not implemented");
                case OpKind::EraseSwap: {
                    const auto idx = index_guard(op.index);
                    const auto length = load("%length");
                    const auto last = temp(); emit(last + " = sub i64 " + length + ", 1");
                    for (const auto* name : {"%col0", "%col1", "%generation"}) {
                        const auto tail = slot_ptr(name, last);
                        const auto val = load(tail);
                        emit("store i64 " + val + ", ptr " + slot_ptr(name, idx));
                        emit("store i64 0, ptr " + tail);
                    }
                    emit("store i64 " + last + ", ptr %length");
                    break;
                }
                case OpKind::BorrowRead: case OpKind::BorrowWrite:
                case OpKind::ReleaseRead: case OpKind::ReleaseWrite:
                    // All loans are verified in MIR; no reference escapes this
                    // local function or crosses suspension. No pointer emitted.
                    break;
                case OpKind::Close:
                    // The native checked-column owner clears row payloads
                    // on teardown. Match that contract in this *bounded*
                    // stack-local IR slice: revoke owner state first, then
                    // volatile-scrub every physical cell and live-slot marker.
                    // Volatile prevents the optimizer eliding dead-stack
                    // erasure; no guest pointer, loan or handle can escape.
                    emit("store volatile i1 false, ptr %alive");
                    for (std::size_t cell = 0; cell < cap_; ++cell) {
                        for (const auto* lane : {"%col0", "%col1", "%generation"}) {
                            emit("store volatile i64 0, ptr " +
                                 slot_ptr(lane, std::to_string(cell)));
                        }
                    }
                    emit("store volatile i64 0, ptr %length");
                    break;
                default: throw std::logic_error("unsupported MIR opcode in LLVM IR emitter");
            }
        }
        if (pushes_ != args.rows.size() || writes_ != args.writes.size())
            throw std::invalid_argument("unused MIR LLVM operands");
        // Side effects are represented in checked column stack storage; return
        // the last field read (or zero), matching the prototype's i32 test ABI.
        const auto result = temp(); emit(result + " = trunc i64 " + last_read_ + " to i32");
        emit("ret i32 " + result);
        code_ += "}\n";
        return code_;
    }
};
inline std::string emit_native_i64_pair_ir(const Module& verified,
                                           const I64PairIrOperands& inputs) {
    return I64PairLlvmEmitter(verified.capacity()).build(verified, inputs);
}
} // namespace oreslang::dop::mir
