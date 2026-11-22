#pragma once
// Production AST -> typed/verified MIR for a bounded, stack-local inverted
// two-i64-column main. No actor, pointer, loan, hot-load or suspended owner.
// Only the admitted MIR may enter the existing checked LLVM emitter.
#include "oreslang/types/InvertedSchemaMirAdmission.hpp"
#include "oreslang/types/InvertedMirLlvmI64.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::dop::mir {

struct VerifiedInvertedSourceI64 {
    Module mir;
    I64PairIrOperands operands;
};

inline VerifiedInvertedSourceI64 verify_production_inverted_i64(
        const Program& program) {
    if (!program.inverted_main || program.inverted_structs.size() != 1 ||
            !program.functions.empty() || !program.classes.empty() ||
            !program.actors.empty() || !program.imports.empty() ||
            !program.contracts.empty() || !program.type_only_declarations.empty() ||
            !program.type_constraint_descriptors.empty() ||
            !program.conforms.empty() || !program.sum_types.empty() ||
            !program.generators.empty() || !program.cooperative_loops.empty() ||
            !program.module.empty() || !program.namespace_name.empty())
        throw std::logic_error("production inverted i64 requires one standalone local main");
    const auto schemas = preflight_inverted_schemas(program);
    if (schemas.size() != 1)
        throw std::logic_error("production inverted schema preflight incomplete");
    const auto& schema = schemas.front().layout;
    const auto& source = *program.inverted_main;
    if (schema.name() != source.schema_name ||
            source.owner_name.empty() ||
            schema.columns() != 2 ||
            schema.column(0).type != InvertedScalarKind::I64 ||
            schema.column(1).type != InvertedScalarKind::I64)
        throw std::invalid_argument("production inverted source requires matching two-i64 schema");
    if (source.capacity == 0 || source.capacity > 256 ||
            source.operations.empty() || source.operations.size() > 1024)
        throw std::invalid_argument("production inverted source capacity/operation budget exceeded");
    if (schema.capacity_bytes(source.capacity) > 16 * 1024 * 1024)
        throw std::invalid_argument("production inverted source layout exceeds capacity budget");

    hir::LinearPlan plan(schema, source.capacity);
    I64PairIrOperands operands;
    bool returned = false;
    for (std::size_t i = 0; i < source.operations.size(); ++i) {
        const auto& operation = source.operations[i];
        if (returned)
            throw std::logic_error("production inverted operation follows terminal return");
        if (operation.index >
                static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::out_of_range("production inverted row index exceeds signed i64");
        switch (operation.kind) {
            case InvertedSourceOperation::Kind::Push:
                if (!operation.field.empty() || operation.index != 0 ||
                        operation.value != 0)
                    throw std::invalid_argument("invalid inverted push source metadata");
                plan.push(2);
                operands.rows.push_back(operation.values);
                break;
            case InvertedSourceOperation::Kind::WriteField:
                if (operation.values != std::array<std::int64_t, 2>{})
                    throw std::invalid_argument("invalid inverted write source metadata");
                plan.write_runtime_checked(operation.index, operation.field);
                operands.writes.push_back(operation.value);
                break;
            case InvertedSourceOperation::Kind::EraseSwap:
                if (!operation.field.empty() || operation.value != 0 ||
                        operation.values != std::array<std::int64_t, 2>{})
                    throw std::invalid_argument("invalid inverted erase source metadata");
                plan.erase_swap(operation.index);
                break;
            case InvertedSourceOperation::Kind::ReturnRead:
                if (operation.value != 0 ||
                        operation.values != std::array<std::int64_t, 2>{})
                    throw std::invalid_argument("invalid inverted return source metadata");
                if (i + 1 != source.operations.size())
                    throw std::logic_error("production inverted terminal read must be last");
                plan.read_runtime_checked(operation.index, operation.field);
                // The local owner dies before function return. MIR close
                // revokes and scrubs every stack column after the read.
                plan.close();
                returned = true;
                break;
            default:
                throw std::invalid_argument("unknown production inverted source operation");
        }
    }
    if (!returned)
        throw std::logic_error("production inverted main requires terminal field return");
    return VerifiedInvertedSourceI64{Module::lower(plan), std::move(operands)};
}

} // namespace oreslang::dop::mir
