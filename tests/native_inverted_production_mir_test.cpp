// Standalone strict C++17 admission test: production Program action AST,
// independently checked HIR/MIR and guarded LLVM IR. No LLVM SDK required.
// This does not replace production Parser.cpp or executable native guest tests.
#include "oreslang/types/InvertedProductionSourceMir.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using oreslang::InvertedSourceOperation;
using oreslang::InvertedSourceMain;
using oreslang::InvertedScalarKind;
using oreslang::InvertedStructDecl;
using oreslang::InvertedStructField;
using oreslang::Program;

Program make_source() {
    Program p;
    InvertedStructDecl decl;
    decl.name = "Position";
    decl.fields = {
        InvertedStructField{"x", InvertedScalarKind::I64, true, 0},
        InvertedStructField{"id", InvertedScalarKind::I64, false, 0}
    };
    p.inverted_structs.push_back(std::move(decl));
    InvertedSourceMain body;
    body.schema_name = "Position";
    body.owner_name = "rows";
    body.capacity = 3;
    using Kind = InvertedSourceOperation::Kind;
    body.operations = {
        InvertedSourceOperation{Kind::Push, 0, "", {{11, 100}}, 0},
        InvertedSourceOperation{Kind::Push, 0, "", {{20, 101}}, 0},
        InvertedSourceOperation{Kind::WriteField, 1, "x", {}, 42},
        InvertedSourceOperation{Kind::EraseSwap, 0, "", {}, 0},
        InvertedSourceOperation{Kind::ReturnRead, 0, "x", {}, 0}
    };
    p.inverted_main = std::move(body);
    return p;
}

Program make_snapshot_source() {
    auto p = make_source();
    using Kind = InvertedSourceOperation::Kind;
    p.inverted_main->operations = {
        InvertedSourceOperation{Kind::Push, 0, "", {{42, 7}}, 0},
        InvertedSourceOperation{Kind::Snapshot, 0, "", {}, 0, "saved"},
        InvertedSourceOperation{Kind::WriteField, 0, "x", {}, 99},
        InvertedSourceOperation{Kind::EraseSwap, 0, "", {}, 0},
        InvertedSourceOperation{Kind::ReturnSnapshotRead, 0, "x", {}, 0, "saved"}
    };
    return p;
}

template<class F> void reject(F&& fn, const char* expected) {
    try { fn(); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find(expected) != std::string::npos)
            return;
        throw std::runtime_error(std::string("wrong production-MIR diagnostic: ") + e.what());
    }
    throw std::runtime_error(std::string("forged production-MIR operation admitted: ") + expected);
}
}

int main() {
    using namespace oreslang::dop::mir;
    using oreslang::dop::hir::OpKind;
    const auto valid = make_source();
    const auto compiled = verify_production_inverted_i64(valid);
    const auto& ops = compiled.mir.instructions();
    assert(ops.size() == 7);
    assert(ops.front().kind == OpKind::Allocate);
    assert(ops[1].kind == OpKind::Push && ops[2].kind == OpKind::Push);
    assert(ops[3].kind == OpKind::WriteField);
    assert(ops[4].kind == OpKind::EraseSwap);
    assert(ops[5].kind == OpKind::ReadField);
    assert(ops[6].kind == OpKind::Close);
    assert(compiled.operands.rows.size() == 2 &&
           compiled.operands.writes.size() == 1);
    const auto ir = emit_native_i64_pair_ir(compiled.mir, compiled.operands);
    assert(ir.find("define i32 @main()") != std::string::npos);
    assert(ir.find("icmp ult i64") != std::string::npos);
    assert(ir.find("store volatile i1 false, ptr %alive") != std::string::npos);

    // An actual detached value is independently represented by the typed
    // source operation, HIR/MIR snapshot ordinal, and final copied SSA scalar.
    {
        const auto snapshot = verify_production_inverted_i64(make_snapshot_source());
        const auto& code = snapshot.mir.instructions();
        assert(code.size() == 7);
        assert(code[2].kind == OpKind::Snapshot);
        assert(code[2].require_owner_guard && code[2].require_bounds_guard &&
               code[2].require_generation_guard);
        assert(code[5].kind == OpKind::ReadSnapshotField &&
               code[5].index == 0 && code[5].column == 0);
        assert(!code[5].require_generation_guard && !code[5].require_bounds_guard);
        const auto snapshot_ir = emit_native_i64_pair_ir(snapshot.mir, snapshot.operands);
        assert(snapshot_ir.find("store i64 99, ptr") != std::string::npos);
        assert(snapshot_ir.find("store volatile i1 false, ptr %alive") != std::string::npos);
        assert(snapshot_ir.find("icmp ult i64 0, ") != std::string::npos);
        assert(I64PairLlvmEmitter(snapshot.mir.capacity()).build(
            snapshot.mir, snapshot.operands) == snapshot_ir);

        auto forged_mir = code;
        forged_mir[5].index = 1;
        reject([&] { (void)Module::verify_external(snapshot.mir.layout(),
            snapshot.mir.capacity(), forged_mir); },
            "invalid inverted MIR snapshot reference");
    }
    {
        auto forged = make_snapshot_source();
        forged.inverted_main->operations[4].snapshot_name = "unknown";
        reject([&] { (void)verify_production_inverted_i64(forged); },
            "return requires declared snapshot");
    }
    {
        auto forged = make_snapshot_source();
        forged.inverted_main->operations[1].snapshot_name = "rows";
        reject([&] { (void)verify_production_inverted_i64(forged); },
            "invalid or duplicate inverted snapshot binding");
    }
    {
        auto forged = make_snapshot_source();
        forged.inverted_main->operations[1].field = "x";
        reject([&] { (void)verify_production_inverted_i64(forged); },
            "invalid or duplicate inverted snapshot binding");
    }
    {
        auto forged = make_snapshot_source();
        forged.inverted_main->operations[4].index = 1;
        reject([&] { (void)verify_production_inverted_i64(forged); },
            "invalid inverted snapshot return metadata");
    }
    {
        auto forged = make_snapshot_source();
        forged.inverted_main->operations[1].index = 2;
        // This is a dynamic runtime trap, not a compiler exception based
        // solely on source row counts.
        const auto runtime = verify_production_inverted_i64(forged);
        const auto ir = emit_native_i64_pair_ir(runtime.mir, runtime.operands);
        assert(ir.find("icmp ult i64 2, ") != std::string::npos);
        assert(ir.find("call void @llvm.trap()") != std::string::npos);
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[2].field = "id";
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "immutable inverted field");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[0].index = 1;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "invalid inverted push source metadata");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[2].values[0] = 91;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "invalid inverted write source metadata");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[3].field = "x";
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "invalid inverted erase source metadata");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[4].value = 7;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "invalid inverted return source metadata");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[3].index = 2;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "outside current length");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations.pop_back();
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "requires terminal field return");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations.push_back(
            InvertedSourceOperation{InvertedSourceOperation::Kind::Push, 0, "", {{1, 2}}, 0});
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "terminal read must be last");
    }
    {
        auto forged = make_source();
        forged.inverted_structs[0].fields[1].type = InvertedScalarKind::Bool;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "matching two-i64 schema");
    }
    {
        auto forged = make_source();
        forged.inverted_main->capacity = 257;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "capacity/operation budget");
    }
    {
        auto forged = make_source();
        forged.inverted_main->schema_name = "Unrelated";
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "matching two-i64 schema");
    }
    // The normal Program AST carries declaration classes that the restricted
    // single-owner lowering does not consume. A forged/stale AST containing
    // any such metadata must fail before emitting LLVM IR.
    {
        auto forged = make_source();
        oreslang::NativeSumType sum;
        sum.name = "HiddenOption";
        forged.sum_types.push_back(std::move(sum));
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "requires one standalone local main");
    }
    {
        auto forged = make_source();
        forged.conforms.push_back("UnloweredCapability");
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "requires one standalone local main");
    }
    {
        auto forged = make_source();
        oreslang::TypeConstraintDescriptor descriptor;
        descriptor.schema_id = "HiddenConstraint";
        forged.type_constraint_descriptors.push_back(std::move(descriptor));
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "requires one standalone local main");
    }
    // The LLVM emitter may be reused by the compiler for multiple units.
    // Failed emission must not leak old SSA identifiers, argument cursors,
    // output bytes or return values into subsequent native guest modules.
    {
        I64PairLlvmEmitter emitter(compiled.mir.capacity());
        const auto first = emitter.build(compiled.mir, compiled.operands);
        assert(first == ir);

        auto missing = compiled.operands;
        missing.rows.pop_back();
        reject([&]{ (void)emitter.build(compiled.mir, missing); },
               "missing MIR LLVM push operands");

        const auto recovered = emitter.build(compiled.mir, compiled.operands);
        assert(recovered == first);

        auto changed = compiled.operands;
        changed.writes[0] = 73;
        const auto different = emitter.build(compiled.mir, changed);
        assert(different != first);
        assert(different.find("store i64 73, ptr") != std::string::npos);
        assert(emitter.build(compiled.mir, compiled.operands) == first);
    }
    // Metadata may be supplied by a forged/deserialized Program rather than
    // the production parser. Reject before any LLVM IR is emitted.
    {
        auto forged = make_source();
        forged.inverted_main->owner_name.clear();
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "matching two-i64 schema");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[2].field = "unknown";
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "unknown");
    }
    {
        auto forged = make_source();
        forged.inverted_main->operations[4].index =
            static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
        reject([&]{ (void)verify_production_inverted_i64(forged); },
               "exceeds signed i64");
    }
    return 0;
}
