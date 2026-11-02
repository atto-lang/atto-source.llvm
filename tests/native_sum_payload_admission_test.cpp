#include "oreslang/types/NativeSumTypes.hpp"

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace oreslang;
using Kind = Expression::Kind;

std::unique_ptr<Expression> scalar(Kind kind) {
    auto result = std::make_unique<Expression>();
    result->kind = kind;
    result->resolved_type = kind == Kind::String ? ScalarType::String
        : kind == Kind::Boolean ? ScalarType::Bool : ScalarType::I64;
    return result;
}
std::unique_ptr<Expression> constructor(const std::string& name,
                                        std::unique_ptr<Expression> payload = nullptr) {
    auto expr = scalar(Kind::SumConstruct);
    expr->name = name;
    if (payload) expr->arguments.push_back(std::move(payload));
    return expr;
}
Program initial(const std::string& sum, const std::string& name,
                std::unique_ptr<Expression> payload = nullptr) {
    Program program;
    program.sum_types.push_back({NativeSumType::Kind::Option,
        "Option<int>", ScalarType::I64, ScalarType::I64});
    program.sum_types.push_back({NativeSumType::Kind::Option,
        "Option<String>", ScalarType::String, ScalarType::I64});
    program.sum_types.push_back({NativeSumType::Kind::Result,
        "Result<int,String>", ScalarType::I64, ScalarType::String});
    Function function;
    function.name = "test";
    LocalBinding value;
    value.name = "value";
    value.sum_type = sum;
    value.initializer = constructor(name, std::move(payload));
    function.locals.push_back(std::move(value));
    program.functions.push_back(std::move(function));
    return program;
}
Program fallback(const std::string& sum, Kind argumentKind) {
    const bool isResult = sum.rfind("Result<", 0) == 0;
    Program program = initial(sum, isResult ? "Ok" : "Some", scalar(Kind::Integer));
    auto read = scalar(Kind::ArrayCall);
    read->name = "unwrap_or";
    read->left = scalar(Kind::Variable);
    read->left->name = "value";
    read->arguments.push_back(scalar(argumentKind));
    LocalBinding next;
    next.name = "fallback";
    next.initializer = std::move(read);
    program.functions.front().locals.push_back(std::move(next));
    return program;
}
// Exercise both ternary arms through the actual native sum resolver. A
// mismatched false arm previously inherited the true arm's Hint unnoticed.
Program conditional_payload(Kind yes, Kind no) {
    Program program = initial("Option<int>", "Some", scalar(Kind::Integer));
    auto ternary = scalar(Kind::Conditional);
    ternary->condition = scalar(Kind::Boolean);
    ternary->when_true = scalar(yes);
    ternary->when_false = scalar(no);
    program.functions.front().locals.front().initializer =
        constructor("Some", std::move(ternary));
    return program;
}
Program conditional_default(Kind yes, Kind no) {
    Program program = fallback("Option<int>", Kind::Integer);
    auto ternary = scalar(Kind::Conditional);
    ternary->condition = scalar(Kind::Boolean);
    ternary->when_true = scalar(yes);
    ternary->when_false = scalar(no);
    program.functions.front().locals.back().initializer->arguments.front() =
        std::move(ternary);
    return program;
}
// Check the error arm of Result<int,String>, not just Some/Option paths.
Program conditional_error(Kind yes, Kind no) {
    Program program = initial("Result<int,String>", "Err", scalar(Kind::String));
    auto ternary = scalar(Kind::Conditional);
    ternary->condition = scalar(Kind::Boolean);
    ternary->when_true = scalar(yes);
    ternary->when_false = scalar(no);
    program.functions.front().locals.front().initializer =
        constructor("Err", std::move(ternary));
    return program;
}
// A nested false-arm conditional must be type-checked at every depth.
Program nested_conditional_payload(Kind nestedFalse) {
    Program program = initial("Option<int>", "Some", scalar(Kind::Integer));
    auto outer = scalar(Kind::Conditional);
    outer->condition = scalar(Kind::Boolean);
    outer->when_true = scalar(Kind::Integer);
    auto inner = scalar(Kind::Conditional);
    inner->condition = scalar(Kind::Boolean);
    inner->when_true = scalar(Kind::Integer);
    inner->when_false = scalar(nestedFalse);
    outer->when_false = std::move(inner);
    program.functions.front().locals.front().initializer =
        constructor("Some", std::move(outer));
    return program;
}
// Negative AST shapes can originate from malformed internal passes;
// the resolver must reject these even when no parser is involved.
Program malformed_conditional(Kind guard, bool dropGuard, bool dropFalseArm) {
    Program program = conditional_payload(Kind::Integer, Kind::Integer);
    auto& expr = *program.functions.front().locals.front().initializer->arguments.front();
    if (dropGuard) expr.condition.reset();
    else expr.condition = scalar(guard);
    if (dropFalseArm) expr.when_false.reset();
    return program;
}
void require_success(Program p, const std::string& label) {
    try { resolve_native_sums(p); }
    catch (const std::exception& e) {
        throw std::runtime_error(label + ": unexpectedly rejected: " + e.what());
    }
}
void require_rejection(Program p, const std::string& diagnostic,
                       const std::string& label) {
    try { resolve_native_sums(p); }
    catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(diagnostic) != std::string::npos) return;
        throw std::runtime_error(label + ": wrong diagnostic: " + error.what());
    }
    throw std::runtime_error(label + ": unexpectedly accepted");
}
} // namespace

int main() {
    try {
        require_rejection(malformed_conditional(Kind::Integer, false, false),
            "conditional guard must be bool", "integer guard cannot select sum payload");
        require_rejection(malformed_conditional(Kind::String, false, false),
            "conditional guard must be bool", "string guard cannot select sum payload");
        require_rejection(malformed_conditional(Kind::Boolean, true, false),
            "conditional missing guard or branch", "ternary without guard");
        require_rejection(malformed_conditional(Kind::Boolean, false, true),
            "conditional missing guard or branch", "ternary without false branch");
        require_success(conditional_error(Kind::String, Kind::String),
            "both Result Err ternary arms are String");
        require_rejection(conditional_error(Kind::String, Kind::Integer),
            "conditional branch payload types do not match", "Err false arm is int");
        require_rejection(conditional_error(Kind::Integer, Kind::String),
            "conditional branch payload types do not match", "Err true arm is int");
        require_success(nested_conditional_payload(Kind::Integer),
            "nested conditional is scalar i64");
        require_rejection(nested_conditional_payload(Kind::Boolean),
            "conditional branch payload types do not match", "nested false arm is bool");
        require_success(conditional_payload(Kind::Integer, Kind::Integer),
            "both ternary payload arms are i64");
        require_success(conditional_default(Kind::Integer, Kind::Integer),
            "both ternary fallback arms are i64");
        require_rejection(conditional_payload(Kind::Integer, Kind::String),
            "conditional branch payload types do not match", "String in false Some arm");
        require_rejection(conditional_payload(Kind::String, Kind::Integer),
            "conditional branch payload types do not match", "int in false Some arm");
        require_rejection(conditional_default(Kind::Integer, Kind::Boolean),
            "conditional branch payload types do not match", "bool in false fallback arm");
        require_success(initial("Option<int>", "Some", scalar(Kind::Integer)), "Some<int>");
        require_success(initial("Option<String>", "Some", scalar(Kind::String)), "Some<String>");
        require_success(initial("Option<int>", "None"), "None<int>");
        require_success(initial("Option<String>", "None"), "None<String>");
        require_success(initial("Result<int,String>", "Ok", scalar(Kind::Integer)), "Ok<int>");
        require_success(initial("Result<int,String>", "Err", scalar(Kind::String)), "Err<String>");
        require_success(fallback("Option<int>", Kind::Integer), "Option unwrap_or int");
        require_success(fallback("Result<int,String>", Kind::Integer), "Result unwrap_or int");

        require_rejection(initial("Option<int>", "Some", scalar(Kind::String)),
            "payload type does not match", "no String in Option<int>");
        require_rejection(initial("Option<String>", "Some", scalar(Kind::Integer)),
            "payload type does not match", "no int in Option<String>");
        require_rejection(initial("Result<int,String>", "Err", scalar(Kind::Integer)),
            "payload type does not match", "Err<int> cannot inhabit Result<int,String>");
        require_rejection(initial("Result<int,String>", "Ok", scalar(Kind::String)),
            "payload type does not match", "Ok<String> cannot inhabit Result<int,String>");
        require_rejection(initial("Option<int>", "Err", scalar(Kind::String)),
            "variant does not match", "no Err in Option");
        require_rejection(initial("Result<int,String>", "Some", scalar(Kind::Integer)),
            "variant does not match", "no Some in Result");
        require_rejection(initial("Option<int>", "Some",
            constructor("Some", scalar(Kind::Integer))),
            "compound payload", "no nested sum into native scalar ABI");
        require_rejection(fallback("Option<int>", Kind::String),
            "fallback type does not match", "Option unwrap_or mismatch");
        require_rejection(fallback("Result<int,String>", Kind::String),
            "fallback type does not match", "Result unwrap_or mismatch");
        std::cout << "native sum typed payload admission: PASS 31 assertions (host-only)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "native sum typed payload admission: " << e.what() << '\n';
        return 1;
    }
}
