// Production compiler ABI boundary: compile and run under strict C++17,
// with NO dependency on native C++20 column runtime headers.
#include "oreslang/types/InvertedTypedMir.hpp"
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
    const auto code = Module::lower(plan);
    assert(code.instructions().size() == 4);
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
    return 0;
}
