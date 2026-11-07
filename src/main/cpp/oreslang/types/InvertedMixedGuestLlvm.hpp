#pragma once
// Restricted source -> independently checked HIR/MIR -> LLVM IR for exactly
// three local i64/bool/f64 columns. This is an opt-in bounded production
// guest slice, NOT a general inverted class, actor heap, borrowing, SIMD, GPU,
// hot-reload or multi-function lowering. No raw row handle may escape.
#include "oreslang/types/InvertedProductionSourceMir.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::dop::mir {

struct F64EqSelect {
    InvertedSourceLiteral expected;
    std::int64_t if_equal = 0;
    std::int64_t if_unequal = 0;
};
struct MixedIrOperands {
    std::vector<std::array<InvertedSourceLiteral, 3>> rows;
    std::vector<InvertedSourceLiteral> writes;
    // Present only for the terminal runtime-observed f64 comparison.
    std::optional<F64EqSelect> terminal_f64_compare;
};
struct VerifiedMixedSource {
    Module mir;
    MixedIrOperands operands;
};

inline bool positive_zero_mixed(double value) {
    // -0.0 compares equal to +0.0 but is a *different* raw payload. A
    // canonical inactive variant must have no IEEE sign bit.
    return value == 0.0 && !std::signbit(value);
}

inline bool unused_mixed_literal(const InvertedSourceLiteral& value) {
    return value.kind == InvertedScalarKind::I64 && value.integer == 0 &&
           !value.boolean && positive_zero_mixed(value.floating);
}

inline bool unused_mixed_values(
        const std::array<InvertedSourceLiteral, 3>& values) {
    for (const auto& value : values)
        if (!unused_mixed_literal(value)) return false;
    return true;
}

inline void check_mixed_literal(const InvertedSourceLiteral& value,
                               InvertedScalarKind expected) {
    if (value.kind != expected)
        throw std::invalid_argument("mixed inverted operand scalar type mismatch");
    // Each literal is a closed tagged scalar, not a bag of alternate values.
    // Rejected hidden payloads prevent forged AST/MIR metadata smuggling.
    const bool hidden = (expected == InvertedScalarKind::I64 &&
                            (value.boolean || !positive_zero_mixed(value.floating))) ||
                        (expected == InvertedScalarKind::Bool &&
                            (value.integer != 0 || !positive_zero_mixed(value.floating))) ||
                        (expected == InvertedScalarKind::F64 &&
                            (value.integer != 0 || value.boolean));
    if (hidden)
        throw std::invalid_argument("mixed inverted scalar has hidden variant payload");
    if (expected == InvertedScalarKind::F64 && !std::isfinite(value.floating))
        throw std::invalid_argument("mixed inverted f64 must be finite");
}

// Do not reuse two-i64 verification: this independent gate checks the complete
// schema, ownership provenance, literal types, operation budget and teardown.
inline VerifiedMixedSource verify_production_inverted_mixed(const Program& program) {
    if (!program.inverted_main || program.inverted_structs.size() != 1 ||
            !program.functions.empty() || !program.classes.empty() ||
            !program.actors.empty() || !program.imports.empty() ||
            !program.contracts.empty() || !program.type_only_declarations.empty() ||
            !program.type_constraint_descriptors.empty() ||
            !program.conforms.empty() || !program.sum_types.empty() ||
            !program.generators.empty() || !program.cooperative_loops.empty() ||
            !program.module.empty() || !program.namespace_name.empty())
        throw std::logic_error("mixed inverted requires one standalone local main");

    const auto schemas = preflight_inverted_schemas(program);
    if (schemas.size() != 1)
        throw std::logic_error("mixed inverted schema preflight incomplete");
    const auto& layout = schemas.front().layout;
    const auto& source = *program.inverted_main;
    if (!source.three_scalar || source.owner_name.empty() ||
            source.schema_name != layout.name() || layout.columns() != 3)
        throw std::invalid_argument("mixed inverted requires matching three-scalar schema");

    unsigned integers = 0, booleans = 0, floats = 0;
    for (std::size_t field = 0; field < 3; ++field) {
        integers += layout.column(field).type == InvertedScalarKind::I64;
        booleans += layout.column(field).type == InvertedScalarKind::Bool;
        floats += layout.column(field).type == InvertedScalarKind::F64;
    }
    if (integers != 1 || booleans != 1 || floats != 1)
        throw std::invalid_argument("mixed inverted requires one i64, one bool and one f64");
    if (source.capacity == 0 || source.capacity > 256 ||
            source.operations.empty() || source.operations.size() > 1024)
        throw std::invalid_argument("mixed inverted capacity/operation budget exceeded");
    if (layout.capacity_bytes(source.capacity) > 16 * 1024 * 1024)
        throw std::invalid_argument("mixed inverted physical column budget exceeded");

    hir::LinearPlan plan(layout, source.capacity);
    MixedIrOperands operands;
    bool returned = false;
    for (std::size_t n = 0; n < source.operations.size(); ++n) {
        const auto& operation = source.operations[n];
        if (returned)
            throw std::logic_error("mixed inverted operation after terminal read");
        if (operation.index >
                static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::out_of_range("mixed inverted row index exceeds i64");
        // Any legacy scalar metadata in a mixed command would be ambiguous.
        if (operation.values != std::array<std::int64_t, 2>{} ||
                operation.value != 0)
            throw std::logic_error("mixed inverted command has legacy scalar metadata");
        if (operation.kind != InvertedSourceOperation::Kind::ReturnCompareF64 &&
                (operation.equal_result != 0 || operation.unequal_result != 0))
            throw std::logic_error("mixed inverted nonterminal operation has comparison metadata");
        switch (operation.kind) {
            case InvertedSourceOperation::Kind::Push: {
                if (operation.index != 0 || !operation.field.empty() ||
                        !unused_mixed_literal(operation.mixed_value))
                    throw std::invalid_argument("invalid mixed inverted push metadata");
                for (std::size_t field = 0; field < 3; ++field)
                    check_mixed_literal(operation.mixed_values[field],
                                        layout.column(field).type);
                plan.push(3);
                operands.rows.push_back(operation.mixed_values);
                break;
            }
            case InvertedSourceOperation::Kind::WriteField: {
                if (!unused_mixed_values(operation.mixed_values))
                    throw std::invalid_argument("invalid mixed inverted write metadata");
                const auto field = layout.field_index(operation.field);
                check_mixed_literal(operation.mixed_value,
                                    layout.column(field).type);
                plan.write_runtime_checked(operation.index, operation.field);
                operands.writes.push_back(operation.mixed_value);
                break;
            }
            case InvertedSourceOperation::Kind::EraseSwap:
                if (!operation.field.empty() ||
                        !unused_mixed_values(operation.mixed_values) ||
                        !unused_mixed_literal(operation.mixed_value))
                    throw std::invalid_argument("invalid mixed inverted erase metadata");
                plan.erase_swap(operation.index);
                break;
            case InvertedSourceOperation::Kind::ReturnRead:
                if (!unused_mixed_values(operation.mixed_values) ||
                        !unused_mixed_literal(operation.mixed_value))
                    throw std::invalid_argument("invalid mixed inverted return metadata");
                if (n + 1 != source.operations.size())
                    throw std::logic_error("mixed inverted terminal read must be last");
                if (layout.column(layout.field_index(operation.field)).type !=
                        InvertedScalarKind::I64)
                    throw std::invalid_argument("mixed inverted main must return i64");
                plan.read_runtime_checked(operation.index, operation.field);
                plan.close(); // revoke and scrub every stack column
                returned = true;
                break;
            case InvertedSourceOperation::Kind::ReturnCompareF64:
                if (!unused_mixed_values(operation.mixed_values))
                    throw std::invalid_argument("invalid mixed inverted f64 comparison metadata");
                if (n + 1 != source.operations.size())
                    throw std::logic_error("mixed inverted f64 comparison must be terminal");
                if (layout.column(layout.field_index(operation.field)).type !=
                        InvertedScalarKind::F64)
                    throw std::invalid_argument("mixed inverted f64 compare must read f64 column");
                check_mixed_literal(operation.mixed_value, InvertedScalarKind::F64);
                plan.read_runtime_checked(operation.index, operation.field);
                plan.close();
                operands.terminal_f64_compare = F64EqSelect{
                    operation.mixed_value,
                    operation.equal_result,
                    operation.unequal_result
                };
                returned = true;
                break;
            default:
                throw std::logic_error("unknown mixed inverted source operation");
        }
    }
    if (!returned)
        throw std::logic_error("mixed inverted source lacks terminal read");
    return VerifiedMixedSource{Module::lower(plan), std::move(operands)};
}

class MixedLlvmEmitter final {
    std::string code_;
    unsigned next_ = 0;
    std::size_t capacity_;
    std::size_t pushes_ = 0;
    std::size_t writes_ = 0;
    std::string last_read_ = "0";
    const Layout* layout_ = nullptr;

    static const char* ir_type(InvertedScalarKind kind) {
        switch (kind) {
            case InvertedScalarKind::I64: return "i64";
            case InvertedScalarKind::Bool: return "i1";
            case InvertedScalarKind::F64: return "double";
            default: throw std::logic_error("unsupported mixed LLVM field type");
        }
    }
    static std::string zero(InvertedScalarKind kind) {
        switch (kind) {
            case InvertedScalarKind::I64: return "0";
            case InvertedScalarKind::Bool: return "false";
            case InvertedScalarKind::F64: return "0.000000e+00";
            default: throw std::logic_error("unsupported mixed LLVM zero");
        }
    }
    static std::string value_ir(const InvertedSourceLiteral& v) {
        switch (v.kind) {
            case InvertedScalarKind::I64: return std::to_string(v.integer);
            case InvertedScalarKind::Bool: return v.boolean ? "true" : "false";
            case InvertedScalarKind::F64: {
                if (!std::isfinite(v.floating))
                    throw std::invalid_argument("mixed LLVM nonfinite f64 literal");
                std::uint64_t bits = 0;
                static_assert(sizeof(bits) == sizeof(v.floating), "f64 width");
                std::memcpy(&bits, &v.floating, sizeof(bits));
                std::ostringstream out;
                out << "0x" << std::hex << std::uppercase << std::setw(16)
                    << std::setfill('0') << bits;
                return out.str();
            }
            default: throw std::logic_error("unsupported mixed LLVM literal");
        }
    }
    std::string temp() { return "%v" + std::to_string(++next_); }
    std::string label() { return "mix" + std::to_string(++next_); }
    void emit(const std::string& text) { code_ += "  " + text + "\n"; }
    void guard(const std::string& test) {
        const auto yes = label(), no = label();
        emit("br i1 " + test + ", label %" + yes + ", label %" + no);
        code_ += no + ":\n";
        emit("call void @llvm.trap()");
        emit("unreachable");
        code_ += yes + ":\n";
    }
    std::string load(const char* type, const std::string& pointer) {
        const auto name = temp();
        emit(name + " = load " + type + ", ptr " + pointer);
        return name;
    }
    std::string pointer(const std::string& lane, const char* type,
                        const std::string& index) {
        const auto name = temp();
        emit(name + " = getelementptr inbounds [" +
             std::to_string(capacity_) + " x " + type + "], ptr " + lane +
             ", i64 0, i64 " + index);
        return name;
    }
    std::string lane(std::size_t field) const {
        return "%col" + std::to_string(field);
    }
    std::string field_ptr(std::size_t field, const std::string& index) {
        return pointer(lane(field), ir_type(layout_->column(field).type), index);
    }
    std::string generation_ptr(const std::string& index) {
        return pointer("%generation", "i64", index);
    }
    void owner_guard() {
        const auto alive = temp();
        emit(alive + " = load i1, ptr %alive");
        guard(alive);
    }
    std::string checked_index(std::size_t index) {
        if (index > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::out_of_range("mixed LLVM index exceeds signed i64");
        const auto len = load("i64", "%length"), in = temp();
        emit(in + " = icmp ult i64 " + std::to_string(index) + ", " + len);
        guard(in);
        const auto generation = load("i64", generation_ptr(std::to_string(index)));
        const auto live = temp();
        emit(live + " = icmp ne i64 " + generation + ", 0");
        guard(live);
        return std::to_string(index);
    }
public:
    explicit MixedLlvmEmitter(std::size_t capacity) : capacity_(capacity) {
        if (!capacity || capacity > 256)
            throw std::invalid_argument("mixed LLVM capacity must be 1..256");
    }
    std::string build(const Module& verified, const MixedIrOperands& inputs) {
        // Failures cannot leak stale SSA/temp state when a compiler reuses the
        // emitter for multiple units. The verified MIR is a separate input.
        code_.clear(); next_ = 0; pushes_ = 0; writes_ = 0;
        last_read_ = "0"; layout_ = nullptr;
        if (verified.capacity() != capacity_ || verified.layout().columns() != 3)
            throw std::invalid_argument("mixed LLVM requires checked three-column MIR");
        layout_ = &verified.layout();
        unsigned ints = 0, bools = 0, doubles = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            ints += layout_->column(i).type == InvertedScalarKind::I64;
            bools += layout_->column(i).type == InvertedScalarKind::Bool;
            doubles += layout_->column(i).type == InvertedScalarKind::F64;
        }
        if (ints != 1 || bools != 1 || doubles != 1)
            throw std::invalid_argument("mixed LLVM requires distinct i64/bool/f64 columns");
        const auto& operations = verified.instructions();
        if (operations.size() < 3 ||
                operations.back().kind != OpKind::Close ||
                operations[operations.size() - 2].kind != OpKind::ReadField)
            throw std::logic_error("mixed LLVM requires terminal checked field read before close");
        const auto terminal_type =
            layout_->column(operations[operations.size() - 2].column).type;
        const bool compare_f64 = inputs.terminal_f64_compare.has_value();
        if ((compare_f64 && terminal_type != InvertedScalarKind::F64) ||
                (!compare_f64 && terminal_type != InvertedScalarKind::I64))
            throw std::logic_error("mixed LLVM terminal return type mismatches comparison provenance");
        if (compare_f64)
            check_mixed_literal(inputs.terminal_f64_compare->expected,
                                InvertedScalarKind::F64);
        for (const auto& op : operations)
            if (op.kind == OpKind::Snapshot || op.kind == OpKind::Suspend ||
                    op.kind == OpKind::BorrowRead || op.kind == OpKind::BorrowWrite ||
                    op.kind == OpKind::ReleaseRead || op.kind == OpKind::ReleaseWrite)
                throw std::logic_error("mixed LLVM source loans, snapshot or suspension unsupported");

        code_ = "; typed mixed i64/bool/f64 SoA MIR, local owner, no row handles\n";
        code_ += "declare void @llvm.trap()\n";
        code_ += "define i32 @main() {\nentry:\n";
        emit("%length = alloca i64"); emit("store i64 0, ptr %length");
        emit("%alive = alloca i1"); emit("store i1 true, ptr %alive");
        for (std::size_t field = 0; field < 3; ++field) {
            const auto* type = ir_type(layout_->column(field).type);
            emit(lane(field) + " = alloca [" + std::to_string(capacity_) +
                 " x " + type + "]");
        }
        emit("%generation = alloca [" + std::to_string(capacity_) + " x i64]");
        for (const auto& op : operations) {
            if (op.kind == OpKind::Allocate) continue;
            owner_guard();
            switch (op.kind) {
                case OpKind::Push: {
                    if (pushes_ == inputs.rows.size())
                        throw std::invalid_argument("mixed LLVM missing push operands");
                    const auto len = load("i64", "%length");
                    const auto inside = temp();
                    emit(inside + " = icmp ult i64 " + len + ", " +
                         std::to_string(capacity_));
                    guard(inside);
                    for (std::size_t i = 0; i < 3; ++i) {
                        const auto& lit = inputs.rows[pushes_][i];
                        check_mixed_literal(lit, layout_->column(i).type);
                        emit(std::string("store ") + ir_type(lit.kind) + " " +
                             value_ir(lit) + ", ptr " + field_ptr(i, len));
                    }
                    emit("store i64 1, ptr " + generation_ptr(len));
                    const auto plus = temp();
                    emit(plus + " = add i64 " + len + ", 1");
                    emit("store i64 " + plus + ", ptr %length");
                    ++pushes_;
                    break;
                }
                case OpKind::WriteField: {
                    if (writes_ == inputs.writes.size())
                        throw std::invalid_argument("mixed LLVM missing write operands");
                    const auto index = checked_index(op.index);
                    const auto& lit = inputs.writes[writes_++];
                    check_mixed_literal(lit, layout_->column(op.column).type);
                    emit(std::string("store ") + ir_type(lit.kind) + " " +
                         value_ir(lit) + ", ptr " + field_ptr(op.column, index));
                    break;
                }
                case OpKind::ReadField: {
                    const auto index = checked_index(op.index);
                    if (compare_f64) {
                        if (layout_->column(op.column).type != InvertedScalarKind::F64)
                            throw std::logic_error("mixed LLVM f64 comparison reads wrong column");
                        const auto observed = load("double", field_ptr(op.column, index));
                        const auto predicate = temp();
                        // Ordered IEEE equality: NaN is rejected by the
                        // source and verifier, and +0.0 equals -0.0 as a
                        // numeric comparison. Bit-level identity is tested
                        // separately and is not silently substituted here.
                        emit(predicate + " = fcmp oeq double " + observed + ", " +
                             value_ir(inputs.terminal_f64_compare->expected));
                        const auto selected = temp();
                        emit(selected + " = select i1 " + predicate + ", i64 " +
                             std::to_string(inputs.terminal_f64_compare->if_equal) +
                             ", i64 " +
                             std::to_string(inputs.terminal_f64_compare->if_unequal));
                        last_read_ = selected;
                    } else {
                        if (layout_->column(op.column).type != InvertedScalarKind::I64)
                            throw std::logic_error("mixed LLVM main read must be i64");
                        last_read_ = load("i64", field_ptr(op.column, index));
                    }
                    break;
                }
                case OpKind::EraseSwap: {
                    const auto index = checked_index(op.index);
                    const auto len = load("i64", "%length");
                    const auto last = temp();
                    emit(last + " = sub i64 " + len + ", 1");
                    for (std::size_t field = 0; field < 3; ++field) {
                        const auto* type = ir_type(layout_->column(field).type);
                        const auto tail = field_ptr(field, last);
                        const auto value = load(type, tail);
                        emit(std::string("store ") + type + " " + value +
                             ", ptr " + field_ptr(field, index));
                        emit(std::string("store ") + type + " " +
                             zero(layout_->column(field).type) + ", ptr " + tail);
                    }
                    const auto tail = generation_ptr(last);
                    const auto moved = load("i64", tail);
                    emit("store i64 " + moved + ", ptr " + generation_ptr(index));
                    emit("store i64 0, ptr " + tail);
                    emit("store i64 " + last + ", ptr %length");
                    break;
                }
                case OpKind::Close:
                    // Revoke authority before scrubbing all payloads, including
                    // unused capacity, at both O0 and O2.
                    emit("store volatile i1 false, ptr %alive");
                    for (std::size_t row = 0; row < capacity_; ++row) {
                        for (std::size_t field = 0; field < 3; ++field) {
                            const auto kind = layout_->column(field).type;
                            emit(std::string("store volatile ") + ir_type(kind) + " " +
                                 zero(kind) + ", ptr " +
                                 field_ptr(field, std::to_string(row)));
                        }
                        emit("store volatile i64 0, ptr " +
                             generation_ptr(std::to_string(row)));
                    }
                    emit("store volatile i64 0, ptr %length");
                    break;
                default:
                    throw std::logic_error("unsupported mixed LLVM MIR instruction");
            }
        }
        if (pushes_ != inputs.rows.size() || writes_ != inputs.writes.size())
            throw std::invalid_argument("mixed LLVM unused operands");
        const auto result = temp();
        emit(result + " = trunc i64 " + last_read_ + " to i32");
        emit("ret i32 " + result);
        code_ += "}\n";
        return code_;
    }
};

inline std::string emit_mixed_source_ir(const VerifiedMixedSource& source) {
    return MixedLlvmEmitter(source.mir.capacity()).build(source.mir, source.operands);
}

} // namespace oreslang::dop::mir
