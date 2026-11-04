// Production compiler ABI boundary: compile and run under strict C++17,
// with NO dependency on native C++20 column runtime headers.
#include "oreslang/types/InvertedTypedMir.hpp"
#include "oreslang/types/InvertedMirLlvmI64.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
template<class F> void reject(F&& f, const char* diagnostic) {
    try { f(); } catch (const std::exception& e) {
        if (std::string(e.what()).find(diagnostic) != std::string::npos) return;
        throw std::runtime_error(std::string("unexpected MIR message: ") + e.what());
    }
    throw std::runtime_error("unsafe MIR unexpectedly admitted");
}
}

int main() {
    using namespace oreslang;
    using namespace oreslang::dop::hir;
    using namespace oreslang::dop::mir;
    InvertedStructDecl schema;
    schema.name = "Coordinates";
    schema.fields = {
        InvertedStructField{"x", InvertedScalarKind::I64, true, 0},
        InvertedStructField{"id", InvertedScalarKind::I64, false, 0}
    };
    auto layout = Layout::from_decl(schema);
    LinearPlan plan(layout, 2);
    plan.push(2);
    plan.read_runtime_checked(0, "id");
    plan.write_runtime_checked(0, "x");
    plan.close();
    const auto code = Module::lower(plan);
    assert(code.instructions().size() == 5);
    assert(code.instructions()[1].kind == OpKind::Push);
    assert(code.instructions()[2].require_bounds_guard);
    assert(code.instructions()[2].require_generation_guard);
    assert(code.instructions()[2].require_owner_guard);

    auto forged = code.instructions();
    forged[2].require_generation_guard = false;
    reject([&] { (void)Module::verify_external(layout, 2, forged); },
           "mandatory owner/generation/bounds guard missing");
    forged = code.instructions();
    forged[3].column = 1;
    reject([&] { (void)Module::verify_external(layout, 2, forged); },
           "immutable or borrowed field write");
    forged = code.instructions();
    forged[3].index = std::numeric_limits<std::size_t>::max();
    reject([&] { (void)Module::verify_external(layout, 2, forged); },
           "runtime index exceeds signed i64");

    LinearPlan with_loan(layout, 1);
    with_loan.push(2);
    with_loan.borrow_read("id");
    with_loan.release_read("id");
    with_loan.close();
    (void)Module::lower(with_loan);
    reject([&] { with_loan.suspend(); }, "closed inverted HIR owner");

    // The C++17 LLVM-IR emitter can be retained across compiler functions.
    // SSA numbering, operand cursors and the previous last-read value must
    // never carry over, even after a failing compilation transaction.
    I64PairLlvmEmitter emitter(2);
    const I64PairIrOperands original_inputs{{{5, 20}}, {42}};
    const auto initial_ir = emitter.build(code, original_inputs);
    assert(initial_ir.find("define i32 @main()") != std::string::npos);
    LinearPlan next_plan(layout, 2);
    next_plan.push(2);
    next_plan.read_runtime_checked(0, "id");
    next_plan.close();
    const auto next_module = Module::lower(next_plan);
    const I64PairIrOperands next_inputs{{{9, 77}}, {}};
    const auto reference_ir = I64PairLlvmEmitter(2).build(next_module, next_inputs);
    assert(emitter.build(next_module, next_inputs) == reference_ir);
    // External open-owner MIR remains valid for other targets, but never
    // bypasses the standalone LLVM guest teardown gate.
    LinearPlan unclosed(layout, 2);
    unclosed.push(2);
    reject([&] {
        (void)emitter.build(Module::lower(unclosed),
                            I64PairIrOperands{{{1, 2}}, {}});
    }, "terminal owner close");
    reject([&] {
        (void)emitter.build(code, I64PairIrOperands{{}, {42}});
    }, "missing MIR LLVM push operands");
    assert(emitter.build(next_module, next_inputs) == reference_ir);
    return 0;
}
