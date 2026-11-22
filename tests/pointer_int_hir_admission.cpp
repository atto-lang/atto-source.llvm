// Source-owned unit proof for the shared nominal HIR/ownership checker.
// Intentionally links no LLVM runtime: a forged or mutated compiler AST must
// be rejected before the independent LLVM lowering gate.
#include "oreslang/types/PointerIntOwnership.hpp"
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#ifdef NDEBUG
#error "Pointer<int> HIR test assertions must remain active"
#endif

using oreslang::pointer_int_aot::Expr;
using oreslang::pointer_int_aot::Plan;
using oreslang::pointer_int_aot::Step;
using oreslang::pointer_int_aot::validate_ownership;
using oreslang::pointer_int_aot::verify_plan;

std::unique_ptr<Expr> literal(std::int64_t v) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Literal;
    expr->known = v;
    return expr;
}
std::unique_ptr<Expr> read_slot(std::size_t index, std::int64_t known) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Read;
    expr->index = index;
    expr->known = known;
    return expr;
}
std::unique_ptr<Expr> variable(const char* name, std::int64_t known) {
    auto expr = std::make_unique<Expr>();
    expr->kind = Expr::Kind::Variable;
    expr->name = name;
    expr->known = known;
    return expr;
}
Plan plan() {
    Plan out;
    out.owner = "p";
    out.count = 2;
    Step store;
    store.kind = Step::Kind::Write;
    store.index = 0;
    store.value = literal(42);
    out.steps.push_back(std::move(store));
    Step scalar;
    scalar.kind = Step::Kind::Scalar;
    scalar.name = "answer";
    scalar.value = read_slot(0, 42);
    out.steps.push_back(std::move(scalar));
    Step close;
    close.kind = Step::Kind::Close;
    out.steps.push_back(std::move(close));
    out.result = variable("answer", 42);
    return out;
}
void must_reject(const char* label, const std::function<void()>& action) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error(std::string("HIR_ADMISSION_BYPASS: ") + label);
}

int main() {
    auto p = plan();
    assert(!p.ownership_admitted);
    validate_ownership(p);
    assert(p.ownership_admitted);
    verify_plan(p);
    std::cout << "POINTER_HIR_POSITIVE=admitted\n";

    p.steps[0].index = 2;
    must_reject("out-of-bounds index after admission", [&] { verify_plan(p); });
    p.steps[0].index = 0;

    p.steps[0].value->known = 41;
    must_reject("parser annotation altered after admission", [&] { verify_plan(p); });
    p.steps[0].value->known = 42;

    p.steps[1].value->known = 43;
    must_reject("forged initialized read value", [&] { verify_plan(p); });
    p.steps[1].value->known = 42;

    p.result = read_slot(0, 42);
    must_reject("read after close", [&] { verify_plan(p); });
    p.result = variable("answer", 42);

    p.steps[1].value = read_slot(1, 0);
    must_reject("read before initialization", [&] { verify_plan(p); });
    p.steps[1].value = read_slot(0, 42);

    p.steps[2].kind = Step::Kind::Write;
    p.steps[2].index = 0;
    p.steps[2].value = literal(3);
    must_reject("missing close", [&] { verify_plan(p); });
    p.steps[2].kind = Step::Kind::Close;
    p.steps[2].value.reset();

    p.steps[1].name = "p";
    must_reject("pointer type erased into scalar", [&] { verify_plan(p); });
    p.steps[1].name = "answer";

    p.result->name = "p";
    must_reject("returning pointer capability as i64", [&] { verify_plan(p); });
    p.result->name = "answer";

    p.owner = "Pointer";
    must_reject("reserved builtin owner", [&] { verify_plan(p); });
    p.owner = "p";

    p.ownership_admitted = false;
    assert(!p.ownership_admitted);
    verify_plan(p); // read-only verifier does not mint authorization
    assert(!p.ownership_admitted);
    validate_ownership(p);
    assert(p.ownership_admitted);
    std::cout << "POINTER_HIR_ADVERSARIAL_REJECTIONS=9\n";
}
