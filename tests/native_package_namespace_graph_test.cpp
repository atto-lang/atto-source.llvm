#include "oreslang/frontend/PackageNamespaceGraph.hpp"
// Never let -DNDEBUG turn acceptance checks into no-ops.
#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error("namespace graph test failed: " #expr); } while (false)
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang;
using namespace oreslang::frontend;

static Program make_source(const std::string& ns, const std::string& name, bool exported=true) {
    Program program;
    program.namespace_name = ns;
    Function fn;
    fn.name = name;
    fn.exported = exported;
    // Positive fixtures must contain an actual typed scalar result. A
    // missing non-void result is not a valid callable namespace export.
    fn.result = std::make_unique<Expression>();
    fn.result->kind = Expression::Kind::Integer;
    fn.result->resolved_type = ScalarType::I64;
    program.functions.push_back(std::move(fn));
    return program;
}

template<typename F>
static void must_reject(F&& op, const std::string& message) {
    try {
        op();
    } catch (const std::invalid_argument& error) {
        REQUIRE(std::string(error.what()).find(message) != std::string::npos);
        return;
    }
    REQUIRE(false && "expected namespace package linker to reject invalid graph");
}

int main() {
    // Each Program owns movable native AST. std::map gives stable references.
    std::map<std::string, Program> programs;
    const std::vector<std::pair<std::string,std::string>> input = {
        {"compute/math/add", "a"}, {"compute/stats/double", "a"},
        {"other/aggregation/aggregate", "a"}, {"data/model", "b"},
        {"data/queries", "b"}, {"data/storage", "b"},
        {"data/validation", "b"}, {"api/routes", "c"},
        {"api/handlers", "c"}, {"api/responses", "c"}
    };
    for (const auto& entry : input) {
        const auto& file = entry.first;
        const std::string symbol = file.substr(file.rfind('/') + 1);
        programs.emplace(file, make_source(entry.second, symbol));
    }
    programs.emplace("app/main", Program{});

    std::vector<NamespaceOwnedSource> units;
    for (auto& [path, program] : programs)
        units.push_back({path, path == "app/main" ? "app@1" : "foo@1", &program});
    const PackageAliasPins pins{{"app@1", {{"foo", "foo@1"}}}};
    const NamespaceImportRequest request{"app/main", "foo", "a"};

    // 1. Ten source units split into three package-local namespaces.
    auto graph = link_package_namespaces(units, pins, {request});
    REQUIRE(graph.namespaces.size() == 3);
    REQUIRE(graph.namespaces.at({"foo@1", "a"}).source_units.size() == 3);
    REQUIRE(graph.namespaces.at({"foo@1", "b"}).source_units.size() == 4);
    REQUIRE(graph.namespaces.at({"foo@1", "c"}).source_units.size() == 3);
    REQUIRE(graph.imports.at({"app/main", "a"}).package_instance_id == "foo@1");
    REQUIRE(graph.namespaces.at({"foo@1", "a"}).exports.size() == 3);
    // +/ is scoped to the manifest-verified physical importer's package.
    // It must resolve to a sibling provider without a synthetic global
    // package alias, and never select the same name in another package.
    const auto local_graph = link_package_namespaces(
        units, {}, {{"compute/math/add", "+/", "a", "local_a"}});
    REQUIRE(local_graph.imports.at({"compute/math/add", "local_a"}).package_instance_id == "foo@1");
    REQUIRE(resolve_package_namespace_scalar_call(
        local_graph, "compute/math/add", "local_a", "double", {}).source_unit_id
        == "compute/stats/double");
    must_reject([&] {
        (void)resolve_package_namespace_scalar_call(
            local_graph, "compute/math/add", "local_a", "foreign", {});
    }, "not exported");
    // A source from a DIFFERENT physical package cannot select foo's
    // namespace merely by spelling the same +/ local alias.
    must_reject([&] {
        (void)link_package_namespaces(
            units, {}, {{"app/main", "+/", "a", "local_a"}});
    }, "no cross-package fallback");
    // A bound selector resolves to a physical provider, never a global
    // namespace/function name. This is typed host resolution only.
    const auto add_call = resolve_package_namespace_scalar_call(
        graph, "app/main", "a", "add", {});
    REQUIRE(add_call.owner.package_instance_id == "foo@1");
    REQUIRE(add_call.owner.name == "a");
    REQUIRE(add_call.source_unit_id == "compute/math/add");
    REQUIRE(add_call.function_name == "add");
    REQUIRE(add_call.return_type == ScalarType::I64);
    must_reject([&] {
        (void)resolve_package_namespace_scalar_call(
            graph, "app/main", "missing", "add", {});
    }, "no verified import binding");
    must_reject([&] {
        (void)resolve_package_namespace_scalar_call(
            graph, "other/a", "a", "add", {});
    }, "no verified import binding");
    must_reject([&] {
        (void)resolve_package_namespace_scalar_call(
            graph, "app/main", "a", "not_exported", {});
    }, "not exported");
    must_reject([&] {
        (void)resolve_package_namespace_scalar_call(
            graph, "app/main", "a", "add", {ScalarType::I64});
    }, "wrong value arity");
    // No implicit namespace-wide or cross-package fallback to a same-spelling
    // function declared by another package or another namespace.
    must_reject([&] {
        (void)resolve_package_namespace_scalar_call(
            graph, "app/main", "a", "foreign", {});
    }, "not exported");

    // 2. Source declarations in a second package sharing "a" cannot merge.
    Program other = make_source("a", "foreign");
    auto another = units;
    another.push_back({"other/a", "other@1", &other});
    auto separate = link_package_namespaces(another, pins, {request});
    REQUIRE(separate.namespaces.size() == 4);
    REQUIRE(separate.namespaces.at({"foo@1", "a"}).exports.size() == 3);
    REQUIRE(separate.namespaces.at({"other@1", "a"}).exports.size() == 1);

    // The SAME namespace spelling in different verified package instances
    // can coexist only through distinct lexical "as" aliases.
    const PackageAliasPins two_pins{{"app@1", {
        {"foo", "foo@1"}, {"other", "other@1"}}}};
    const std::vector<NamespaceImportRequest> two_imports{
        {"app/main", "foo", "a", "primary"},
        {"app/main", "other", "a", "secondary"}};
    const auto alias_graph = link_package_namespaces(another, two_pins, two_imports);
    REQUIRE(alias_graph.imports.at({"app/main", "primary"}).package_instance_id == "foo@1");
    REQUIRE(alias_graph.imports.at({"app/main", "secondary"}).package_instance_id == "other@1");
    REQUIRE(resolve_package_namespace_scalar_call(
        alias_graph, "app/main", "primary", "add", {}).source_unit_id == "compute/math/add");
    REQUIRE(resolve_package_namespace_scalar_call(
        alias_graph, "app/main", "secondary", "foreign", {}).source_unit_id == "other/a");
    must_reject([&] {
        (void)resolve_package_namespace_scalar_call(
            alias_graph, "app/main", "primary", "foreign", {});
    }, "not exported");
    must_reject([&] {
        (void)link_package_namespaces(another, two_pins, {
            {"app/main", "foo", "a", "same"},
            {"app/main", "other", "a", "same"}});
    }, "duplicate lexical namespace import binding");
    must_reject([&] {
        (void)link_package_namespaces(another, two_pins, {
            {"app/main", "foo", "a", "__root__"}});
    }, "invalid namespace import alias or selector");

    // 3. Duplicate public symbol/signature inside same package fails closed.
    Program duplicate = make_source("a", "add");
    another = units;
    another.push_back({"foo/duplicate", "foo@1", &duplicate});
    must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                "duplicate public namespace");

    // 4. Missing pin and wrong package version cannot fall back by spelling.
    must_reject([&] { (void)link_package_namespaces(units, {}, {request}); },
                "undeclared pinned package alias");
    must_reject([&] {
        (void)link_package_namespaces(units, {{"app@1", {{"foo", "foo@2"}}}}, {request});
    }, "verified package inventory");
    must_reject([&] {
        (void)link_package_namespaces(units, pins, {{"app/main", "foo", "missing"}});
    }, "no cross-package fallback");

    // 5. A mixed module wrapper + namespace header must fail closed, not
    // manufacture a ghost namespace that another source can import.
    Program mixed = make_source("ghost", "leak");
    mixed.module = "legacy";
    another = units;
    another.push_back({"foo/mixed", "foo@1", &mixed});
    must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                "module wrapper cannot declare a file namespace");

    // 6. Private declarations are not exposed by any namespace import.
    Program private_helper = make_source("a", "private_helper", false);
    another = units;
    another.push_back({"foo/private", "foo@1", &private_helper});
    auto private_graph = link_package_namespaces(another, pins, {request});
    REQUIRE(private_graph.namespaces.at({"foo@1", "a"}).source_units.size() == 4);
    REQUIRE(private_graph.namespaces.at({"foo@1", "a"}).exports.size() == 3);

    // A private init is still a lifecycle hook, not an ordinary hidden helper.
    // Reject it before the non-exported function filter can erase the effect.
    Program private_init = make_source("a", "init", false);
    another = units;
    another.push_back({"foo/private_init", "foo@1", &private_init});
    must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                "init lifecycle");

    // Namespace module imports have no manifest/lockfile binding in this
    // phase-one host inventory; a pinned namespace alias is not a module pin.
    {
        Program unverified = make_source("a", "with_unverified_module");
        unverified.imports.emplace_back();
        another = units;
        another.push_back({"foo/unverified_module", "foo@1", &unverified});
        must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                    "unresolved module imports");
    }

    // A syntactically valid, host-injected namespace import is also not a
    // verified physical package dependency. Do not silently discard its AST
    // while constructing a different (apparently authenticated) graph.
    {
        Program unverified = make_source("a", "with_unverified_namespace");
        unverified.namespace_imports.push_back(
            {"billing", "payments", "+/", 0});
        another = units;
        another.push_back({"foo/unverified_namespace", "foo@1", &unverified});
        must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                    "embedded imports require verified v2 package linking");
    }

    // A root importing file has no namespace header. It must nevertheless
    // be rejected if it carries an unverified namespace AST. Check this
    // BEFORE the no-namespace early exit in link_package_namespaces().
    {
        Program unchecked_root;
        unchecked_root.namespace_imports.push_back(
            {"billing", "payments", "+/", 0});
        another = units;
        another.push_back({"app/unchecked-root", "app@1", &unchecked_root});
        must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                    "embedded imports require verified v2 package linking");
    }

    // 7. Unlowered top-level AST families must never enter the scalar graph.
    {
        Program unsupported = make_source("a", "with_sum");
        unsupported.sum_types.push_back(NativeSumType{});
        another = units;
        another.push_back({"foo/sum", "foo@1", &unsupported});
        must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                    "non-scalar declarations");
    }
    {
        Program unsupported = make_source("a", "with_constraint");
        unsupported.type_constraint_descriptors.push_back(TypeConstraintDescriptor{});
        another = units;
        another.push_back({"foo/constraint", "foo@1", &unsupported});
        must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                    "non-scalar declarations");
    }
    {
        Program unsupported = make_source("a", "with_conformance");
        unsupported.conforms.push_back("SomeTrait");
        another = units;
        another.push_back({"foo/conformance", "foo@1", &unsupported});
        must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                    "non-scalar declarations");
    }
    {
        Program unsupported = make_source("a", "with_loop");
        unsupported.cooperative_loops.push_back(CooperativeScalarLoop{});
        another = units;
        another.push_back({"foo/loop", "foo@1", &unsupported});
        must_reject([&] { (void)link_package_namespaces(another, pins, {request}); },
                    "non-scalar declarations");
    }

    // 8. i64/bool value signatures are admissible, but no non-scalar ABI
    // may be smuggled into a public namespace as merely (name, arity).
    Program bool_signature = make_source("a", "bool_value");
    bool_signature.functions.front().return_type = ScalarType::Bool;
    bool_signature.functions.front().result->kind = Expression::Kind::Boolean;
    bool_signature.functions.front().result->resolved_type = ScalarType::Bool;
    Parameter bool_param;
    bool_param.name = "flag";
    bool_param.scalar_type = ScalarType::Bool;
    bool_signature.functions.front().parameters.push_back(bool_param);
    another = units;
    another.push_back({"foo/bool_value", "foo@1", &bool_signature});
    const auto scalar_graph = link_package_namespaces(another, pins, {request});
    REQUIRE(scalar_graph.namespaces.at({"foo@1", "a"}).exports.size() == 4);
    const auto bool_call = resolve_package_namespace_scalar_call(
        scalar_graph, "app/main", "a", "bool_value", {ScalarType::Bool});
    REQUIRE(bool_call.source_unit_id == "foo/bool_value");
    REQUIRE(bool_call.return_type == ScalarType::Bool);
    must_reject([&] {
        (void)resolve_package_namespace_scalar_call(
            scalar_graph, "app/main", "a", "bool_value", {ScalarType::I64});
    }, "argument type mismatch");

    const auto reject_signature = [&](const std::string& name,
                                      const auto& mutate, const std::string& reason) {
        Program bad = make_source("a", name);
        mutate(bad.functions.front());
        auto modified = units;
        modified.push_back({"foo/" + name, "foo@1", &bad});
        must_reject([&] { (void)link_package_namespaces(modified, pins, {request}); },
                    reason);
    };
    reject_signature("borrow_value", [](Function& fn) {
        fn.parameters.emplace_back();
        fn.parameters.back().mode = Parameter::Mode::Borrow;
    }, "non-scalar parameter");
    reject_signature("borrow_mut_value", [](Function& fn) {
        fn.parameters.emplace_back();
        fn.parameters.back().mode = Parameter::Mode::BorrowMut;
    }, "non-scalar parameter");
    reject_signature("string_param", [](Function& fn) {
        fn.parameters.emplace_back();
        fn.parameters.back().scalar_type = ScalarType::String;
    }, "non-scalar parameter");
    reject_signature("nominal_param", [](Function& fn) {
        fn.parameters.emplace_back();
        fn.parameters.back().class_name = "Record";
    }, "non-scalar parameter");
    reject_signature("generic_param", [](Function& fn) {
        fn.parameters.emplace_back();
        fn.parameters.back().generic_type = "T";
    }, "non-scalar parameter");
    reject_signature("alias_param", [](Function& fn) {
        fn.parameters.emplace_back();
        fn.parameters.back().pending_type_alias = "Alias";
    }, "non-scalar parameter");
    reject_signature("sum_param", [](Function& fn) {
        fn.parameters.emplace_back();
        fn.parameters.back().sum_type = "OptionInt";
    }, "non-scalar parameter");
    reject_signature("void_result", [](Function& fn) {
        fn.returns_void = true;
    }, "non-scalar return");
    reject_signature("string_result", [](Function& fn) {
        fn.return_type = ScalarType::String;
    }, "non-scalar return");
    reject_signature("nominal_result", [](Function& fn) {
        fn.return_class = "Record";
    }, "non-scalar return");
    reject_signature("alias_result", [](Function& fn) {
        fn.pending_return_type_alias = "Alias";
    }, "non-scalar return");
    reject_signature("sum_result", [](Function& fn) {
        fn.return_sum = "OptionInt";
    }, "non-scalar return");
    // 9. Unknown scalar enum tags must not be interpreted as valid i64 ABI.
    reject_signature("invalid_param_tag", [](Function& fn) {
        fn.parameters.emplace_back();
        fn.parameters.back().scalar_type = static_cast<ScalarType>(255);
    }, "non-scalar parameter");
    reject_signature("invalid_return_tag", [](Function& fn) {
        fn.return_type = static_cast<ScalarType>(255);
    }, "non-scalar return");
    // 10. Async source functions return a task/future boundary, not plain i64.
    reject_signature("async_export", [](Function& fn) {
        fn.async_source_i64 = true;
    }, "async export");

    // 11. Linker-owned private implementations must not be promoted into
    // the public namespace ABI; exported names must be valid flat identifiers.
    reject_signature("internal_export", [](Function& fn) {
        fn.native_internal = true;
    }, "private native implementation");
    reject_signature("empty_export_name", [](Function& fn) {
        fn.name.clear();
    }, "invalid public namespace export name");
    reject_signature("dotted_export_name", [](Function& fn) {
        fn.name = "some.path";
    }, "invalid public namespace export name");
    reject_signature("reserved_export_name", [](Function& fn) {
        fn.name = "__root__";
    }, "invalid public namespace export name");

    // Namespace exports must not invent a callable init lifecycle or
    // erase unchecked body control flow and nested expression effects.
    reject_signature("init_lifecycle_export", [](Function& fn) {
        fn.name = "init";
    }, "init lifecycle");
    reject_signature("body_statement_export", [](Function& fn) {
        fn.statements.emplace_back();
    }, "body control flow");
    reject_signature("early_return_export", [](Function& fn) {
        fn.early_returns.emplace_back();
    }, "body control flow");
    reject_signature("missing_result_export", [](Function& fn) {
        fn.result.reset();
    }, "no scalar result expression");
    // A literal typed as a different scalar cannot stand in for the
    // declared non-void return, even though its AST is otherwise pure.
    reject_signature("mismatched_result_export", [](Function& fn) {
        fn.result->resolved_type = ScalarType::Bool;
        fn.result->kind = Expression::Kind::Boolean;
    }, "unresolved scalar binding");
    reject_signature("console_result_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Console;
    }, "body expression");
    reject_signature("nested_call_result_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Add;
        fn.result->left = std::make_unique<Expression>();
        fn.result->left->kind = Expression::Kind::Call;
    }, "body expression");
    // Malformed scalar ASTs must not be inventoried as callable exports:
    // a scalar resolved_type alone does not validate operand completeness.
    reject_signature("missing_binary_left", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Add;
        fn.result->right = std::make_unique<Expression>();
        fn.result->right->kind = Expression::Kind::Integer;
    }, "body expression");
    reject_signature("missing_binary_right", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Add;
        fn.result->left = std::make_unique<Expression>();
        fn.result->left->kind = Expression::Kind::Integer;
    }, "body expression");
    reject_signature("missing_unary_operand", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Negate;
    }, "body expression");
    reject_signature("missing_conditional_branch", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Conditional;
        fn.result->condition = std::make_unique<Expression>();
        fn.result->condition->kind = Expression::Kind::Boolean;
        fn.result->when_true = std::make_unique<Expression>();
        fn.result->when_true->kind = Expression::Kind::Integer;
    }, "body expression");
    reject_signature("missing_conditional_condition", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Conditional;
        fn.result->when_true = std::make_unique<Expression>();
        fn.result->when_true->kind = Expression::Kind::Integer;
        fn.result->when_false = std::make_unique<Expression>();
        fn.result->when_false->kind = Expression::Kind::Integer;
    }, "body expression");
    reject_signature("surplus_literal_operand", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Integer;
        fn.result->left = std::make_unique<Expression>();
        fn.result->left->kind = Expression::Kind::Boolean;
    }, "body expression");
    // Type tags and variable identity must agree with the scalar AST shape.
    // A forged Bool-typed integer, mixed arithmetic or untyped predicate is
    // not a valid cross-unit scalar ABI even when all nodes are scalar.
    reject_signature("integer_wrong_type_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Integer;
        fn.result->resolved_type = ScalarType::Bool;
    }, "body expression");
    reject_signature("boolean_wrong_type_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Boolean;
    }, "body expression");
    reject_signature("unnamed_variable_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Variable;
    }, "body expression");
    reject_signature("dotted_variable_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Variable;
        fn.result->name = "foreign.symbol";
    }, "body expression");
    // Remainder was removed from Oreslang's scalar language contract.
    // Even a host-supplied, correctly typed legacy AST must not reintroduce
    // that operator through the cross-unit namespace export surface.
    reject_signature("removed_logical_xor_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::LogicalXor;
        fn.result->resolved_type = ScalarType::Bool;
        fn.result->left = std::make_unique<Expression>();
        fn.result->left->kind = Expression::Kind::Boolean;
        fn.result->left->resolved_type = ScalarType::Bool;
        fn.result->right = std::make_unique<Expression>();
        fn.result->right->kind = Expression::Kind::Boolean;
        fn.result->right->resolved_type = ScalarType::Bool;
    }, "body expression");
    reject_signature("removed_remainder_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Remainder;
        fn.result->resolved_type = ScalarType::I64;
        fn.result->left = std::make_unique<Expression>();
        fn.result->left->kind = Expression::Kind::Integer;
        fn.result->right = std::make_unique<Expression>();
        fn.result->right->kind = Expression::Kind::Integer;
    }, "body expression");
    reject_signature("add_bool_operand_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Add;
        fn.result->left = std::make_unique<Expression>();
        fn.result->left->kind = Expression::Kind::Integer;
        fn.result->right = std::make_unique<Expression>();
        fn.result->right->kind = Expression::Kind::Boolean;
        fn.result->right->resolved_type = ScalarType::Bool;
    }, "body expression");
    reject_signature("logical_nonbool_operand_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::LogicalAnd;
        fn.result->resolved_type = ScalarType::Bool;
        fn.result->left = std::make_unique<Expression>();
        fn.result->left->kind = Expression::Kind::Boolean;
        fn.result->left->resolved_type = ScalarType::Bool;
        fn.result->right = std::make_unique<Expression>();
        fn.result->right->kind = Expression::Kind::Integer;
    }, "body expression");
    reject_signature("comparison_wrong_result_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Equal;
        fn.result->left = std::make_unique<Expression>();
        fn.result->left->kind = Expression::Kind::Integer;
        fn.result->right = std::make_unique<Expression>();
        fn.result->right->kind = Expression::Kind::Integer;
    }, "body expression");
    reject_signature("negate_bool_operand_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Negate;
        fn.result->left = std::make_unique<Expression>();
        fn.result->left->kind = Expression::Kind::Boolean;
        fn.result->left->resolved_type = ScalarType::Bool;
    }, "body expression");
    reject_signature("conditional_nonbool_predicate_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Conditional;
        fn.result->condition = std::make_unique<Expression>();
        fn.result->condition->kind = Expression::Kind::Integer;
        fn.result->when_true = std::make_unique<Expression>();
        fn.result->when_true->kind = Expression::Kind::Integer;
        fn.result->when_false = std::make_unique<Expression>();
        fn.result->when_false->kind = Expression::Kind::Integer;
    }, "body expression");
    reject_signature("conditional_mixed_branches_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Conditional;
        fn.result->condition = std::make_unique<Expression>();
        fn.result->condition->kind = Expression::Kind::Boolean;
        fn.result->condition->resolved_type = ScalarType::Bool;
        fn.result->when_true = std::make_unique<Expression>();
        fn.result->when_true->kind = Expression::Kind::Integer;
        fn.result->when_false = std::make_unique<Expression>();
        fn.result->when_false->kind = Expression::Kind::Boolean;
        fn.result->when_false->resolved_type = ScalarType::Bool;
    }, "body expression");
    reject_signature("legacy_ternary_mixed_comparison_export", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Conditional;
        fn.result->condition_lhs = std::make_unique<Expression>();
        fn.result->condition_lhs->kind = Expression::Kind::Integer;
        fn.result->condition_rhs = std::make_unique<Expression>();
        fn.result->condition_rhs->kind = Expression::Kind::Boolean;
        fn.result->condition_rhs->resolved_type = ScalarType::Bool;
        fn.result->when_true = std::make_unique<Expression>();
        fn.result->when_true->kind = Expression::Kind::Integer;
        fn.result->when_false = std::make_unique<Expression>();
        fn.result->when_false->kind = Expression::Kind::Integer;
    }, "body expression");
    reject_signature("call_local_initializer_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().initializer = std::make_unique<Expression>();
        fn.locals.back().initializer->kind = Expression::Kind::Call;
    }, "local initializer");
    Program pure_expression = make_source("a", "pure_scalar_expression");
    pure_expression.functions.front().result = std::make_unique<Expression>();
    pure_expression.functions.front().result->kind = Expression::Kind::Add;
    pure_expression.functions.front().result->left = std::make_unique<Expression>();
    pure_expression.functions.front().result->left->kind = Expression::Kind::Integer;
    pure_expression.functions.front().result->right = std::make_unique<Expression>();
    pure_expression.functions.front().result->right->kind = Expression::Kind::Integer;
    pure_expression.functions.front().locals.emplace_back();
    pure_expression.functions.front().locals.back().name = "local_flag";
    pure_expression.functions.front().locals.back().scalar_type = ScalarType::Bool;
    pure_expression.functions.front().locals.back().initializer = std::make_unique<Expression>();
    pure_expression.functions.front().locals.back().initializer->kind = Expression::Kind::Boolean;
    pure_expression.functions.front().locals.back().initializer->resolved_type = ScalarType::Bool;
    another = units;
    another.push_back({"foo/pure_scalar_expression", "foo@1", &pure_expression});
    const auto pure_graph = link_package_namespaces(another, pins, {request});
    REQUIRE(pure_graph.namespaces.at({"foo@1", "a"}).exports.size() == 4);

    // Complete first-class and legacy ternaries remain valid pure scalars.
    Program complete_ternary = make_source("a", "complete_ternary");
    complete_ternary.functions.front().result = std::make_unique<Expression>();
    auto& ternary = complete_ternary.functions.front().result;
    ternary->kind = Expression::Kind::Conditional;
    ternary->condition = std::make_unique<Expression>();
    ternary->condition->kind = Expression::Kind::Boolean;
    ternary->condition->resolved_type = ScalarType::Bool;
    ternary->when_true = std::make_unique<Expression>();
    ternary->when_true->kind = Expression::Kind::Integer;
    ternary->when_false = std::make_unique<Expression>();
    ternary->when_false->kind = Expression::Kind::Integer;
    another = units;
    another.push_back({"foo/complete_ternary", "foo@1", &complete_ternary});
    REQUIRE(link_package_namespaces(another, pins, {request})
                .namespaces.at({"foo@1", "a"}).exports.size() == 4);

    // Fully typed bool logic and a named bool variable remain admissible.
    Program typed_bool = make_source("a", "typed_bool");
    typed_bool.functions.front().return_type = ScalarType::Bool;
    Parameter typed_bool_flag;
    typed_bool_flag.name = "flag";
    typed_bool_flag.scalar_type = ScalarType::Bool;
    typed_bool.functions.front().parameters.push_back(typed_bool_flag);
    typed_bool.functions.front().result = std::make_unique<Expression>();
    auto& bool_result = typed_bool.functions.front().result;
    bool_result->kind = Expression::Kind::LogicalAnd;
    bool_result->resolved_type = ScalarType::Bool;
    bool_result->left = std::make_unique<Expression>();
    bool_result->left->kind = Expression::Kind::Variable;
    bool_result->left->name = "flag";
    bool_result->left->resolved_type = ScalarType::Bool;
    bool_result->right = std::make_unique<Expression>();
    bool_result->right->kind = Expression::Kind::Boolean;
    bool_result->right->resolved_type = ScalarType::Bool;
    another = units;
    another.push_back({"foo/typed_bool", "foo@1", &typed_bool});
    REQUIRE(link_package_namespaces(another, pins, {request})
                .namespaces.at({"foo@1", "a"}).exports.size() == 4);

    // A scalar tag cannot substitute for a verified lexical identity.
    reject_signature("unbound_result_variable", [](Function& fn) {
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Variable;
        fn.result->name = "missing";
    }, "body expression");
    reject_signature("mismatched_bound_variable", [](Function& fn) {
        Parameter param;
        param.name = "flag";
        param.scalar_type = ScalarType::Bool;
        fn.parameters.push_back(param);
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Variable;
        fn.result->name = "flag";
        fn.result->resolved_type = ScalarType::I64;
    }, "body expression");
    reject_signature("empty_scalar_parameter", [](Function& fn) {
        fn.parameters.emplace_back();
    }, "invalid or duplicate scalar binding");
    reject_signature("duplicate_scalar_parameter", [](Function& fn) {
        Parameter param;
        param.name = "value";
        fn.parameters.push_back(param);
        fn.parameters.push_back(param);
    }, "invalid or duplicate scalar binding");
    reject_signature("dotted_scalar_local", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().name = "some.path";
    }, "invalid or duplicate scalar binding");
    reject_signature("duplicate_scalar_local", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().name = "value";
        fn.locals.emplace_back();
        fn.locals.back().name = "value";
    }, "invalid or duplicate scalar binding");
    reject_signature("shadowed_scalar_parameter", [](Function& fn) {
        Parameter param;
        param.name = "value";
        fn.parameters.push_back(param);
        fn.locals.emplace_back();
        fn.locals.back().name = "value";
    }, "invalid or duplicate scalar binding");
    reject_signature("self_referential_scalar_local", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().name = "value";
        fn.locals.back().initializer = std::make_unique<Expression>();
        fn.locals.back().initializer->kind = Expression::Kind::Variable;
        fn.locals.back().initializer->name = "value";
    }, "local initializer");
    reject_signature("forward_referenced_scalar_local", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().name = "first";
        fn.locals.back().initializer = std::make_unique<Expression>();
        fn.locals.back().initializer->kind = Expression::Kind::Variable;
        fn.locals.back().initializer->name = "later";
        fn.locals.emplace_back();
        fn.locals.back().name = "later";
    }, "local initializer");
    reject_signature("wrong_scalar_local_initializer_type", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().name = "value";
        fn.locals.back().initializer = std::make_unique<Expression>();
        fn.locals.back().initializer->kind = Expression::Kind::Boolean;
        fn.locals.back().initializer->resolved_type = ScalarType::Bool;
    }, "local initializer");
    reject_signature("uninitialized_used_scalar_local", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().name = "uninitialized";
        fn.result = std::make_unique<Expression>();
        fn.result->kind = Expression::Kind::Variable;
        fn.result->name = "uninitialized";
    }, "uninitialized scalar local");
    reject_signature("uninitialized_unused_scalar_local", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().name = "unused";
    }, "uninitialized scalar local");
    Program bound_local = make_source("a", "bound_scalar_local");
    auto& bound_fn = bound_local.functions.front();
    bound_fn.locals.emplace_back();
    bound_fn.locals.back().name = "value";
    bound_fn.locals.back().initializer = std::make_unique<Expression>();
    bound_fn.locals.back().initializer->kind = Expression::Kind::Integer;
    bound_fn.result = std::make_unique<Expression>();
    bound_fn.result->kind = Expression::Kind::Variable;
    bound_fn.result->name = "value";
    another = units;
    another.push_back({"foo/bound_scalar_local", "foo@1", &bound_local});
    REQUIRE(link_package_namespaces(another, pins, {request})
                .namespaces.at({"foo@1", "a"}).exports.size() == 4);

    // A forged scalar-only AST must not consume unbounded host stack. The
    // exact maximum number of nodes on a path remains admissible, while one
    // additional node fails closed before recursive validation descends.
    const auto scalar_negate_chain = [](std::size_t nodes) {
        auto root = std::make_unique<Expression>();
        Expression* current = root.get();
        for (std::size_t i = 1; i < nodes; ++i) {
            current->kind = Expression::Kind::Negate;
            current->left = std::make_unique<Expression>();
            current = current->left.get();
        }
        current->kind = Expression::Kind::Integer;
        return root;
    };
    reject_signature("scalar_expression_excess_depth", [&](Function& fn) {
        fn.result = scalar_negate_chain(kMaxNamespaceScalarExpressionDepth + 1);
    }, "body expression");
    Program bounded_depth = make_source("a", "bounded_depth");
    bounded_depth.functions.front().result =
        scalar_negate_chain(kMaxNamespaceScalarExpressionDepth);
    another = units;
    another.push_back({"foo/bounded_depth", "foo@1", &bounded_depth});
    REQUIRE(link_package_namespaces(another, pins, {request})
                .namespaces.at({"foo@1", "a"}).exports.size() == 4);

    // A shallow, branching tree can stay within the depth cap while
    // exceeding the total node budget. Reject the larger graph before it
    // can turn namespace admission into an unbounded CPU traversal.
    const auto scalar_wide_tree = [](std::size_t expanded_nodes) {
        auto root = std::make_unique<Expression>();
        root->kind = Expression::Kind::Integer;
        std::vector<Expression*> pending{root.get()};
        for (std::size_t i = 0; i < expanded_nodes; ++i) {
            Expression* node = pending[i];
            node->kind = Expression::Kind::Add;
            node->left = std::make_unique<Expression>();
            node->right = std::make_unique<Expression>();
            node->left->kind = Expression::Kind::Integer;
            node->right->kind = Expression::Kind::Integer;
            pending.push_back(node->left.get());
            pending.push_back(node->right.get());
        }
        return root;
    };
    // Full binary trees have odd node counts: 511 expansions => 1023
    // nodes (admitted); 512 expansions => 1025 (rejected at budget 1024).
    Program bounded_nodes = make_source("a", "bounded_nodes");
    bounded_nodes.functions.front().result =
        scalar_wide_tree(kMaxNamespaceScalarExpressionNodes / 2 - 1);
    another = units;
    another.push_back({"foo/bounded_nodes", "foo@1", &bounded_nodes});
    REQUIRE(link_package_namespaces(another, pins, {request})
                .namespaces.at({"foo@1", "a"}).exports.size() == 4);
    reject_signature("scalar_expression_excess_nodes", [&](Function& fn) {
        fn.result = scalar_wide_tree(kMaxNamespaceScalarExpressionNodes / 2);
    }, "body expression");

    // 12. Lifecycle suspension, task/Future pipelines and continuation
    // metadata are not ordinary synchronous scalar namespace exports.
    reject_signature("ready_await_export", [](Function& fn) {
        fn.ready_await_actor = "worker";
    }, "suspension export");
    reject_signature("done_await_export", [](Function& fn) {
        fn.done_await_actor = "worker";
    }, "suspension export");
    reject_signature("ready_form_export", [](Function& fn) {
        fn.ready_await_form = static_cast<NativeAwaitForm>(255);
    }, "suspension export");
    reject_signature("ready_future_export", [](Function& fn) {
        fn.ready_future_name = "pending";
    }, "suspension export");
    reject_signature("ready_future_actor_export", [](Function& fn) {
        fn.ready_future_actor = "worker";
    }, "suspension export");
    reject_signature("task_export", [](Function& fn) {
        fn.source_tasks.emplace_back();
    }, "suspension export");
    reject_signature("pipeline_export", [](Function& fn) {
        fn.future_pipelines.emplace_back();
    }, "suspension export");
    // Orphaned offsets and local await bindings must not bypass the
    // function-level suspension guard on a forged/unresolved AST.
    reject_signature("orphan_ready_offset_export", [](Function& fn) {
        fn.ready_await_offset = 42;
    }, "suspension export");
    reject_signature("orphan_done_offset_export", [](Function& fn) {
        fn.done_await_offset = 42;
    }, "suspension export");
    reject_signature("local_task_await_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().task_await_name = "task";
    }, "suspension export");
    reject_signature("local_future_await_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().future_await_name = "future";
    }, "suspension export");

    // 13. The host-only (name, arity) export inventory cannot erase
    // declared effect rows, actor authority or exception/cleanup effects.
    // A future typed linker must validate these before enabling exports.
    reject_signature("declared_io_effect", [](Function& fn) {
        fn.has_declared_effects = true;
        fn.declared_effects.bits = static_cast<std::uint32_t>(FunctionEffect::Io);
    }, "effect row");
    reject_signature("unresolved_effect_bit", [](Function& fn) {
        fn.declared_effects.bits = FunctionEffects::UnresolvedCallBit;
    }, "effect row");
    reject_signature("explicit_empty_effect_row", [](Function& fn) {
        fn.has_declared_effects = true;
    }, "effect row");
    reject_signature("actor_spawn_export", [](Function& fn) {
        fn.actor_spawns.emplace_back();
    }, "actor/exception effects");
    reject_signature("actor_send_export", [](Function& fn) {
        fn.actor_sends.emplace_back();
    }, "actor/exception effects");
    reject_signature("actor_stop_export", [](Function& fn) {
        fn.actor_stops.emplace_back("worker");
    }, "actor/exception effects");
    reject_signature("actor_action_export", [](Function& fn) {
        fn.actor_actions.emplace_back();
    }, "actor/exception effects");
    reject_signature("actor_write_export", [](Function& fn) {
        fn.actor_field_writes.emplace_back();
    }, "actor/exception effects");
    reject_signature("caught_throw_export", [](Function& fn) {
        fn.caught_throw_value = std::make_unique<Expression>();
    }, "actor/exception effects");
    reject_signature("catch_binding_export", [](Function& fn) {
        fn.catch_binding = "err";
    }, "actor/exception effects");
    reject_signature("throw_cleanup_export", [](Function& fn) {
        fn.caught_throw_defers.emplace_back();
    }, "actor/exception effects");

    // 14. A scalar public signature cannot erase a resumable loop frame or
    // owned/nominal local representations that require checked lifetime ABI.
    // Ordinary mutable scalar/bool locals remain admissible in the inventory.
    Program scalar_local = make_source("a", "scalar_local");
    scalar_local.functions.front().locals.emplace_back();
    scalar_local.functions.front().locals.back().name = "mutable_flag";
    scalar_local.functions.front().locals.back().scalar_type = ScalarType::Bool;
    scalar_local.functions.front().locals.back().initializer = std::make_unique<Expression>();
    scalar_local.functions.front().locals.back().initializer->kind = Expression::Kind::Boolean;
    scalar_local.functions.front().locals.back().initializer->resolved_type = ScalarType::Bool;
    scalar_local.functions.front().locals.back().owner_mutable = true;
    scalar_local.functions.front().locals.back().assignable = true;
    another = units;
    another.push_back({"foo/scalar_local", "foo@1", &scalar_local});
    const auto scalar_local_graph = link_package_namespaces(another, pins, {request});
    REQUIRE(scalar_local_graph.namespaces.at({"foo@1", "a"}).exports.size() == 4);

    reject_signature("scalar_loop_export", [](Function& fn) {
        fn.scalar_while = std::make_unique<ScalarWhileLoop>();
    }, "scalar loop frame");
    reject_signature("owned_heap_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().heap_owned = true;
    }, "owned/nominal local");
    reject_signature("owned_array_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().int_array = true;
    }, "owned/nominal local");
    reject_signature("record_view_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().record_view = true;
    }, "owned/nominal local");
    reject_signature("nominal_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().class_name = "Record";
    }, "owned/nominal local");
    reject_signature("constructor_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().new_class = true;
    }, "owned/nominal local");
    reject_signature("move_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().move_class = true;
    }, "owned/nominal local");
    reject_signature("constructor_args_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().constructor_arguments.emplace_back(std::make_unique<Expression>());
    }, "owned/nominal local");
    reject_signature("alias_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().pending_type_alias = "Alias";
    }, "owned/nominal local");
    reject_signature("sum_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().sum_type = "OptionInt";
    }, "owned/nominal local");
    reject_signature("string_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().scalar_type = ScalarType::String;
    }, "owned/nominal local");
    reject_signature("string_array_element_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().array_element_type = ScalarType::String;
    }, "owned/nominal local");
    reject_signature("invalid_scalar_local_export", [](Function& fn) {
        fn.locals.emplace_back();
        fn.locals.back().scalar_type = static_cast<ScalarType>(255);
    }, "owned/nominal local");

    // Private internal implementation remains invisible and is not rejected.
    Program private_internal = make_source("a", "native_internal_helper", false);
    private_internal.functions.front().native_internal = true;
    auto internal_units = units;
    internal_units.push_back({"foo/native_internal_helper", "foo@1", &private_internal});
    const auto internal_graph = link_package_namespaces(internal_units, pins, {request});
    REQUIRE(internal_graph.namespaces.at({"foo@1", "a"}).exports.size() == 3);

    std::cout << "native package namespace source-only graph: structural AST admission checks passed\n";
}
