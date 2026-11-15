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
        // Real native typed sum resolver: caller-side trap is only a
        // compiler-proved Some(int), never implicit Result/Option coercion.
        {
            auto p = initial("Option<int>", "Some", scalar(Kind::Integer));
            p.functions.front().locals.front().initializer->trap_origin = true;
            require_success(std::move(p), "pure int trap admitted into Option<int>");
        }
        {
            auto p = initial("Option<int>", "Some", scalar(Kind::Boolean));
            p.functions.front().locals.front().initializer->trap_origin = true;
            require_rejection(std::move(p), "pure i64 expression",
                "bool is not a proven native trap int payload");
        }
        {
            auto p = initial("Option<String>", "Some", scalar(Kind::String));
            p.functions.front().locals.front().initializer->trap_origin = true;
            require_rejection(std::move(p), "pure i64 expression",
                "string cannot be silently trapped into Option<String>");
        }
        {
            auto p = initial("Result<int,String>", "Some", scalar(Kind::Integer));
            p.functions.front().locals.front().initializer->trap_origin = true;
            require_rejection(std::move(p), "exactly Option<int>",
                "trap never flattens to bare Result");
        }
        {
            auto p = initial("", "Some", scalar(Kind::Integer));
            p.functions.front().locals.front().initializer->trap_origin = true;
            require_rejection(std::move(p), "cannot implicitly wrap or erase",
                "trap cannot inhabit a scalar local");
        }
        {
            auto p = initial("Option<int>", "Some", scalar(Kind::Integer));
            p.functions.front().locals.front().initializer->trap_origin = true;
            auto arithmetic = scalar(Kind::Add);
            arithmetic->left = std::move(p.functions.front().locals.front().initializer);
            arithmetic->right = scalar(Kind::Integer);
            p.functions.front().locals.front().initializer = std::move(arithmetic);
            require_rejection(std::move(p), "not an integer arithmetic operand",
                "tagged Option cannot silently become i64");
        }
        std::cout << "native sum typed payload admission: PASS 23 assertions (host-only)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "native sum typed payload admission: " << e.what() << '\n';
        return 1;
    }
}
