// Standalone strict C++17 admission test: production Program action AST,
// independently checked HIR/MIR and guarded LLVM IR. No LLVM SDK required.
// This does not replace production Parser.cpp or executable native guest tests.
#include "oreslang/types/InvertedProductionSourceMir.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

// Do not rely on -UNDEBUG from CMake: this test is also compiled directly,
// under Release/NDEBUG and on toolchains that do not support -UNDEBUG.
// Security and MIR integrity checks must evaluate on every build.
#define INVERTED_REQUIRE(condition) do { \
    if (!(condition)) \
        throw std::runtime_error("inverted production MIR invariant failed: " #condition); \
} while (false)

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
    // Mutation sentinel: even under -DNDEBUG a failed invariant must throw.
    // Do not use a debug-only assertion to test the sentinel.
    bool failed_invariant_observed = false;
    try { INVERTED_REQUIRE(false); }
    catch (const std::runtime_error& e) {
        failed_invariant_observed =
            std::string(e.what()).find("inverted production MIR invariant failed: false") !=
            std::string::npos;
    }
    if (!failed_invariant_observed)
        throw std::runtime_error("Release/NDEBUG suppressed inverted MIR checks");

    const auto valid = make_source();
    const auto compiled = verify_production_inverted_i64(valid);
    const auto& ops = compiled.mir.instructions();
    INVERTED_REQUIRE(ops.size() == 7);
    INVERTED_REQUIRE(ops.front().kind == OpKind::Allocate);
    INVERTED_REQUIRE(ops[1].kind == OpKind::Push && ops[2].kind == OpKind::Push);
    INVERTED_REQUIRE(ops[3].kind == OpKind::WriteField);
    INVERTED_REQUIRE(ops[4].kind == OpKind::EraseSwap);
    INVERTED_REQUIRE(ops[5].kind == OpKind::ReadField);
    INVERTED_REQUIRE(ops[6].kind == OpKind::Close);
    INVERTED_REQUIRE(compiled.operands.rows.size() == 2 &&
           compiled.operands.writes.size() == 1);
    const auto ir = emit_native_i64_pair_ir(compiled.mir, compiled.operands);
    INVERTED_REQUIRE(ir.find("define i32 @main()") != std::string::npos);
    INVERTED_REQUIRE(ir.find("icmp ult i64") != std::string::npos);
    INVERTED_REQUIRE(ir.find("store volatile i1 false, ptr %alive") != std::string::npos);
    // A forged external MIR sequence that simply omits Close must fail
    // independently of emitter checks: no owner may leak past normal exit.
    {
        auto forged = ops;
        forged.pop_back();
        reject([&]{ (void)Module::verify_external(
                compiled.mir.layout(), compiled.mir.capacity(), forged); },
                "requires terminal close");
    }
    // This must also hold on the normal HIR->MIR lowering path, not only
    // for external/deserialized instructions.
    {
        using oreslang::dop::hir::LinearPlan;
        LinearPlan unclosed(compiled.mir.layout(), compiled.mir.capacity());
        unclosed.push(2);
        unclosed.read_runtime_checked(0, "x");
        reject([&]{ (void)Module::lower(unclosed); },
                "requires terminal close");
    }
    // The external MIR admission boundary must reject forged metadata even
    // when its current emitter would ignore it (loans/close have no row).
    {
        auto forged = ops;
        forged.back().index = 1;
        reject([&]{ (void)Module::verify_external(
                compiled.mir.layout(), compiled.mir.capacity(), forged); },
                "non-row instruction has forged row index");
    }
    {
        using oreslang::dop::hir::LinearPlan;
        LinearPlan plan(compiled.mir.layout(), 3);
        plan.push(2);
        plan.borrow_read("x");
        plan.release_read("x");
        plan.borrow_write("x");
        plan.release_write("x");
        plan.read_runtime_checked(0, "x");
        plan.close();
        const auto loan_ir = Module::lower(plan);
        // Allocate, Push, BorrowRead, ReleaseRead, BorrowWrite,
        // ReleaseWrite, ReadField, Close
        INVERTED_REQUIRE(loan_ir.instructions().size() == 8);
        for (const std::size_t at : {std::size_t{2}, std::size_t{3},
                                     std::size_t{4}, std::size_t{5},
                                     std::size_t{7}}) {
            auto forged = loan_ir.instructions();
            forged[at].index = 99;
            reject([&]{ (void)Module::verify_external(
                    loan_ir.layout(), loan_ir.capacity(), forged); },
                    "non-row instruction has forged row index");
        }
        INVERTED_REQUIRE(Module::verify_external(
            loan_ir.layout(), loan_ir.capacity(), loan_ir.instructions()
        ).instructions().size() == 8);
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
        INVERTED_REQUIRE(first == ir);

        auto missing = compiled.operands;
        missing.rows.pop_back();
        reject([&]{ (void)emitter.build(compiled.mir, missing); },
               "missing MIR LLVM push operands");

        const auto recovered = emitter.build(compiled.mir, compiled.operands);
        INVERTED_REQUIRE(recovered == first);

        auto changed = compiled.operands;
        changed.writes[0] = 73;
        const auto different = emitter.build(compiled.mir, changed);
        INVERTED_REQUIRE(different != first);
        INVERTED_REQUIRE(different.find("store i64 73, ptr") != std::string::npos);
        INVERTED_REQUIRE(emitter.build(compiled.mir, compiled.operands) == first);
    }
    // A verified linear MIR is not automatically a valid executable main.
    // No read (or a read followed by mutation) must not silently return 0
    // or reuse a stale SSA value after a column change or swap-delete.
    {
        using oreslang::dop::hir::LinearPlan;
        const auto layout = compiled.mir.layout();
        const auto check_denied = [&](LinearPlan& plan, I64PairIrOperands inputs) {
            const auto verified = Module::lower(plan);
            I64PairLlvmEmitter emitter(verified.capacity());
            reject([&] { (void)emitter.build(verified, inputs); },
                   "MIR LLVM requires terminal field read before close");
            // A failed verification must not poison reuse of the compiler
            // emitter on a previously valid verified source.
            INVERTED_REQUIRE(emitter.build(compiled.mir, compiled.operands) == ir);
        };

        LinearPlan no_read(layout, 3);
        no_read.push(2);
        no_read.close();
        check_denied(no_read, I64PairIrOperands{{{{42, 100}}}, {}});

        LinearPlan stale_push(layout, 3);
        stale_push.push(2);
        stale_push.read_runtime_checked(0, "x");
        stale_push.push(2);
        stale_push.close();
        check_denied(stale_push,
                     I64PairIrOperands{{{{11, 100}}, {{42, 101}}}, {}});

        LinearPlan stale_write(layout, 3);
        stale_write.push(2);
        stale_write.read_runtime_checked(0, "x");
        stale_write.write_runtime_checked(0, "x");
        stale_write.close();
        check_denied(stale_write, I64PairIrOperands{{{{11, 100}}}, {42}});

        LinearPlan stale_erase(layout, 3);
        stale_erase.push(2);
        stale_erase.push(2);
        stale_erase.read_runtime_checked(0, "x");
        stale_erase.erase_swap(0);
        stale_erase.close();
        check_denied(stale_erase,
                     I64PairIrOperands{{{{11, 100}}, {{42, 101}}}, {}});
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
