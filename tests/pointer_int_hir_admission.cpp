// Source-owned unit proof for the shared nominal HIR/ownership checker.
// Intentionally links no LLVM runtime: a forged or mutated compiler AST must
// be rejected before the independent LLVM lowering gate.
#include "oreslang/types/PointerIntOwnership.hpp"
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <limits>
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
    p.steps[0].value = literal(0);
    p.steps[0].value->kind = static_cast<Expr::Kind>(-1);
    must_reject("invalid expression kind", [&] { validate_ownership(p); });
    assert(!p.ownership_admitted);
    p.steps[0].value = literal(42);
    validate_ownership(p);
    Step invalid;
    invalid.kind = static_cast<Step::Kind>(-1);
    p.steps.insert(p.steps.begin(), std::move(invalid));
    must_reject("invalid step kind", [&] { validate_ownership(p); });
    assert(!p.ownership_admitted);
    p.steps.erase(p.steps.begin());
    validate_ownership(p);
    verify_plan(p);
    auto metadata = plan();
    metadata.steps[1].value = literal(2);
    metadata.steps[1].value->kind = Expr::Kind::Length;
    metadata.result->known = 2;
    validate_ownership(metadata);
    metadata.steps[1].value->known = 3;
    must_reject("incorrect typed length", [&] { verify_plan(metadata); });
    metadata.steps[1].value->kind = Expr::Kind::SizeBytes;
    metadata.steps[1].value->known = 16;
    metadata.result->known = 16;
    validate_ownership(metadata);
    metadata.steps[1].value->index = 1;
    must_reject("malformed metadata index", [&] { verify_plan(metadata); });
    metadata.steps[1].value->index = 0;
    metadata.result = literal(16);
    metadata.result->kind = Expr::Kind::SizeBytes;
    must_reject("metadata after close", [&] { verify_plan(metadata); });
    auto region = plan();
    region.steps[0].kind = Step::Kind::Fill;
    Step swap;
    swap.kind = Step::Kind::Swap;
    swap.second_index = 1;
    region.steps.insert(region.steps.begin() + 1, std::move(swap));
    validate_ownership(region);
    region.steps[1].second_index = 2;
    must_reject("out-of-bounds swap after admission", [&] { verify_plan(region); });
    region.steps[1].second_index = 1;
    region.steps[0].kind = Step::Kind::Write;
    must_reject("swap of uninitialized cell", [&] { verify_plan(region); });
    region.steps[0].kind = Step::Kind::Fill;
    region.steps[0].second_index = 1;
    must_reject("malformed fill second index", [&] { verify_plan(region); });
    region.steps[0].second_index = 0;
    region.steps[1].value = literal(42);
    must_reject("malformed swap payload", [&] { verify_plan(region); });
    region.steps[1].value.reset();
    validate_ownership(region);
    auto call = plan();
    call.count = 1;
    call.readonly_helper = "inspect";
    call.readonly_parameter = "q";
    call.steps[1].value->kind = Expr::Kind::ReadonlyCall;
    call.steps[1].value->name = "inspect";
    validate_ownership(call);
    call.steps[1].value->name = "unknown";
    must_reject("forged borrowed callee", [&] { verify_plan(call); });
    call.steps[1].value->name = "inspect";
    call.count = 2;
    must_reject("borrowed call with wrong extent", [&] { verify_plan(call); });
    call.count = 1;
    call.readonly_parameter = "inspect";
    must_reject("forged helper signature", [&] { verify_plan(call); });
    call.readonly_parameter = "q";
    call.steps[1].value->known = 41;
    must_reject("forged readonly return annotation", [&] { verify_plan(call); });
    call.steps[1].value->known = 42;
    validate_ownership(call);
    auto mut_call = plan();
    mut_call.count = 1;
    mut_call.mutable_helper = "update";
    mut_call.mutable_parameter = "q";
    mut_call.mutable_write = 42;
    mut_call.steps[0].kind = Step::Kind::MutableCall;
    mut_call.steps[0].name = "update";
    mut_call.steps[0].value.reset();
    validate_ownership(mut_call);
    mut_call.steps[0].name = "unknown";
    must_reject("forged mutable callee", [&] { verify_plan(mut_call); });
    mut_call.steps[0].name = "update";
    mut_call.count = 2;
    must_reject("mutable call with wrong extent", [&] { verify_plan(mut_call); });
    mut_call.count = 1;
    mut_call.steps[0].value = literal(42);
    must_reject("forged mutable call payload", [&] { verify_plan(mut_call); });
    mut_call.steps[0].value.reset();
    mut_call.mutable_parameter = "update";
    must_reject("forged mutable signature", [&] { verify_plan(mut_call); });
    mut_call.mutable_parameter = "q";
    validate_ownership(mut_call);
    mut_call.readonly_helper = "inspect";
    mut_call.readonly_parameter = "q";
    mut_call.steps[1].value->kind = Expr::Kind::ReadonlyCall;
    mut_call.steps[1].value->name = "inspect";
    validate_ownership(mut_call);
    mut_call.readonly_helper = "update";
    mut_call.steps[1].value->name = "update";
    must_reject("conflicting readonly and mutable callee identities", [&] { verify_plan(mut_call); });
    mut_call.readonly_helper = "inspect";
    mut_call.steps[1].value->name = "inspect";
    validate_ownership(mut_call);
    mut_call.mutable_scalar_parameter = "value";
    mut_call.mutable_write = 0;
    mut_call.steps[0].value = literal(42);
    validate_ownership(mut_call);
    mut_call.steps[0].value.reset();
    must_reject("missing scalar call payload", [&] { verify_plan(mut_call); });
    mut_call.steps[0].value = read_slot(0, 42);
    must_reject("owner access in exclusive scalar argument", [&] { verify_plan(mut_call); });
    mut_call.steps[0].value = literal(42);
    mut_call.mutable_scalar_parameter = "q";
    must_reject("duplicate scalar formal", [&] { verify_plan(mut_call); });
    mut_call.mutable_scalar_parameter = "value";
    validate_ownership(mut_call);
    std::cout << "POINTER_HIR_ADVERSARIAL_REJECTIONS=30\n";

    // The independently verified HIR arithmetic must agree at the signed
    // boundaries and reject overflow even when supplied a fabricated annotation.
    const auto hi = std::numeric_limits<std::int64_t>::max();
    const auto lo = std::numeric_limits<std::int64_t>::min();
    auto arithmetic = [](Expr::Kind kind, std::int64_t lhs, std::int64_t rhs,
                         std::int64_t known) {
        auto e = std::make_unique<Expr>();
        e->kind = kind;
        e->lhs = literal(lhs);
        if (kind != Expr::Kind::Neg) e->rhs = literal(rhs);
        e->known = known;
        return e;
    };
    auto positive = [&](Expr::Kind kind, std::int64_t lhs, std::int64_t rhs,
                        std::int64_t expected) {
        auto candidate = plan();
        candidate.result = arithmetic(kind, lhs, rhs, expected);
        validate_ownership(candidate);
        assert(candidate.ownership_admitted);
        verify_plan(candidate);
    };
    auto overflow = [&](Expr::Kind kind, std::int64_t lhs, std::int64_t rhs) {
        auto candidate = plan();
        validate_ownership(candidate);
        candidate.result = arithmetic(kind, lhs, rhs, 0);
        bool rejected = false;
        try { validate_ownership(candidate); }
        catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("checked integer arithmetic overflow") != std::string::npos;
        }
        if (!rejected || candidate.ownership_admitted)
            throw std::runtime_error("HIR checked arithmetic rejection missing");
    };
    positive(Expr::Kind::Add, hi - 1, 1, hi);
    positive(Expr::Kind::Subtract, lo + 1, 1, lo);
    positive(Expr::Kind::Multiply, lo, 1, lo);
    positive(Expr::Kind::Neg, hi, 0, -hi);
    overflow(Expr::Kind::Add, hi, 1);
    overflow(Expr::Kind::Subtract, lo, 1);
    overflow(Expr::Kind::Multiply, lo, -1);
    overflow(Expr::Kind::Multiply, hi, 2);
    overflow(Expr::Kind::Neg, lo, 0);
    std::cout << "POINTER_HIR_CHECKED_ARITHMETIC=4_EXECUTED_5_REJECTED\n";
    // The local borrowed pointer form has separate authority and uses the
    // *same* independent verifier as the owning Pointer.alloc region.
    Plan local;
    local.form = Plan::Form::BorrowedLocal;
    local.owner = "value";
    local.local_handle = "ptr";
    local.count = 1;
    local.local_initial = 42;
    Step copy;
    copy.kind = Step::Kind::Scalar;
    copy.name = "answer";
    copy.value = read_slot(0, 42);
    local.steps.push_back(std::move(copy));
    Step unref;
    unref.kind = Step::Kind::Close;
    local.steps.push_back(std::move(unref));
    local.result = variable("answer", 42);
    validate_ownership(local);
    verify_plan(local);
    assert(local.ownership_admitted);

    local.local_handle = "value";
    must_reject("borrowed handle cannot shadow referent", [&] { verify_plan(local); });
    local.local_handle = "ptr";

    local.steps[0].value->known = 41;
    must_reject("forged borrowed read value", [&] { verify_plan(local); });
    local.steps[0].value->known = 42;

    local.steps[1].kind = Step::Kind::Write;
    local.steps[1].value = literal(13);
    must_reject("cannot rewrite borrowed unref as write", [&] { verify_plan(local); });
    local.steps[1].kind = Step::Kind::Close;
    local.steps[1].value.reset();

    local.result->name = "ptr";
    must_reject("cannot return pointer handle", [&] { verify_plan(local); });
    local.result->name = "answer";

    local.form = Plan::Form::OwnedRegion;
    must_reject("cannot recast borrowed pointer to owning allocator", [&] { verify_plan(local); });
    local.form = Plan::Form::BorrowedLocal;

    local.ownership_admitted = false;
    verify_plan(local);
    assert(!local.ownership_admitted);
    validate_ownership(local);
    assert(local.ownership_admitted);
    std::cout << "POINTER_LOCAL_HIR_ADVERSARIAL_REJECTIONS=5\n";
}
