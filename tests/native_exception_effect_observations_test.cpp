// Compiler AST effect observations. No guest throw/raise/panic execution claim.
#include "oreslang/types/ExceptionalEffectObservations.hpp"
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace oreslang;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::unique_ptr<Expression> value(std::int64_t n) {
    auto result = std::make_unique<Expression>();
    result->kind = Expression::Kind::Integer;
    result->integer = n;
    return result;
}

std::unique_ptr<Expression> call(const char* name) {
    auto result = std::make_unique<Expression>();
    result->kind = Expression::Kind::Call;
    result->name = name;
    return result;
}

std::unique_ptr<Expression> risky_remainder() {
    auto expr = std::make_unique<Expression>();
    expr->kind = Expression::Kind::Remainder;
    expr->left = value(10);
    // Variable divisor is not proven nonzero -> possible Panic.
    expr->right = std::make_unique<Expression>();
    expr->right->kind = Expression::Kind::Variable;
    expr->right->name = "divisor";
    return expr;
}

Function function(std::string name, std::unique_ptr<Expression> result) {
    Function f;
    f.name = std::move(name);
    f.result = std::move(result);
    return f;
}

void direct_and_transitive_effects() {
    Program p;
    auto leaf = function("leaf", value(1));
    leaf.caught_throw_value = value(7);
    p.functions.push_back(std::move(leaf));
    p.functions.push_back(function("parent", call("leaf")));
    p.functions.push_back(function("independent", value(0)));
    const auto report = observe_exceptional_effects(p);
    require(!report.budget_exceeded, "simple graph within budget");
    require(!report.has_unknown(), "all direct call edges resolved");
    require(report.scanned_edges == 1, "one bounded call graph edge");
    const auto* leaf_result = report.find("leaf/0");
    const auto* parent = report.find("parent/0");
    const auto* other = report.find("independent/0");
    require(leaf_result && parent && other, "expected all named functions");
    require(leaf_result->has_observed(FunctionEffect::Throw),
            "local caught throw is still an observed event");
    require(parent->has_observed(FunctionEffect::Throw),
            "parent receives transitive observed throw");
    require(!other->has_observed(FunctionEffect::Throw),
            "independent function does not inherit unrelated observations");
    require((report.observed_exception_bits() &
             static_cast<std::uint32_t>(FunctionEffect::Throw)) != 0,
            "aggregate observed exceptional bits include throw");
}

void unresolved_and_known_effect_both_survive() {
    Program p;
    auto callee = function("callee", risky_remainder());
    p.functions.push_back(std::move(callee));
    p.functions.push_back(function("parent", call("callee")));
    p.functions.push_back(function("missing", call("ffi_or_missing")));
    p.functions.push_back(function("root", call("missing")));
    const auto report = observe_exceptional_effects(p);
    require(report.unresolved_edges == 1, "unresolved direct call tracked once");
    require(report.has_unknown(), "missing calls are not effect-free");
    const auto* parent = report.find("parent/0");
    const auto* root = report.find("root/0");
    require(parent && root, "found effect roots");
    require(parent->has_observed(FunctionEffect::Panic),
            "variable remainder observes potential panic");
    require(root->has_unknown_call(), "unknown propagates across caller edges");
    require(!root->has_observed(FunctionEffect::Panic),
            "unknown must not fabricate positive observed panic");
}

void recursive_effect_fixed_point() {
    Program p;
    auto a = function("a", call("b"));
    auto b = function("b", call("a"));
    b.caught_throw_value = value(9);
    p.functions.push_back(std::move(a));
    p.functions.push_back(std::move(b));
    const auto report = observe_exceptional_effects(p);
    require(report.scanned_edges == 2, "both recursive edges scanned");
    require(!report.has_unknown(), "bounded cycle observation is not an unknown call");
    require(report.find("a/0")->has_observed(FunctionEffect::Throw),
            "cycle must propagate effect back to original caller");
}

void unsupported_and_duplicate_identity_are_not_certified() {
    Program p;
    auto g = function("generic", value(1));
    g.generic_parameters.push_back({"T", ""});
    p.functions.push_back(std::move(g));
    p.functions.push_back(function("root", call("generic")));
    auto report = observe_exceptional_effects(p);
    require(report.find("root/0")->has_unknown_call(),
            "uninstantiated generic call must remain unknown");

    p.functions.push_back(function("generic", value(2)));
    report = observe_exceptional_effects(p);
    require(report.ambiguous_callables, "same name/arity cannot be unique authority");
    require(report.find("root/0")->has_unknown_call(),
            "colliding resolution cannot choose arbitrary overload");
}

void declarations_do_not_invent_observed_effects() {
    Program p;
    auto fn = function("declares_throw", value(1));
    fn.has_declared_effects = true;
    fn.declared_effects.bits = static_cast<std::uint32_t>(FunctionEffect::Throw);
    p.functions.push_back(std::move(fn));
    const auto report = observe_exceptional_effects(p);
    const auto* row = report.find("declares_throw/0");
    require(row != nullptr, "declaration row retained");
    require(row->has_unknown_call() && report.has_unknown(),
            "declared may-effect cannot certify a runtime observation");
    require(!row->has_observed(FunctionEffect::Throw),
            "declared may-effect must not create synthetic observed throw");
}

void call_edge_budget_fails_closed() {
    Program p;
    auto root = function("root", value(0));
    for (std::size_t i = 0; i <= realtime_detail::kMaxRealtimeCallEdges; ++i) {
        Statement statement{};
        statement.kind = Statement::Kind::Call;
        statement.expression = call("external");
        root.statements.push_back(std::move(statement));
    }
    p.functions.push_back(std::move(root));
    const auto report = observe_exceptional_effects(p);
    require(report.budget_exceeded && report.has_unknown(),
            "call-edge overflow must not return a partial clean observation");
}

void bounds_and_unmodeled_roots() {
    Program p;
    p.imports.emplace_back();
    p.functions.push_back(function("main", value(1)));
    const auto imported = observe_exceptional_effects(p);
    require(imported.unmodeled_program && imported.has_unknown(),
            "import initialization is not represented by function graph");

    Program huge;
    for (std::size_t i = 0; i <= realtime_detail::kMaxRealtimeFunctions; ++i)
        huge.functions.push_back(function("f" + std::to_string(i), value(0)));
    const auto truncated = observe_exceptional_effects(huge);
    require(truncated.budget_exceeded && truncated.has_unknown(),
            "function count overflow fails closed");
}
} // namespace

int main() {
    direct_and_transitive_effects();
    unresolved_and_known_effect_both_survive();
    recursive_effect_fixed_point();
    unsupported_and_duplicate_identity_are_not_certified();
    declarations_do_not_invent_observed_effects();
    call_edge_budget_fails_closed();
    bounds_and_unmodeled_roots();
    return 0;
}
