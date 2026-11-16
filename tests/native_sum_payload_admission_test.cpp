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
Program conditional(const std::string& sum,
                    std::unique_ptr<Expression> yes,
                    std::unique_ptr<Expression> no) {
    Program program = initial(sum, "None");
    auto choose = scalar(Kind::Conditional);
    choose->condition = scalar(Kind::Boolean);
    choose->when_true = std::move(yes);
    choose->when_false = std::move(no);
    program.functions.front().locals.front().initializer = std::move(choose);
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
void require_invalid_registry(const std::function<void(Program&)>& mutate,
                              const std::string& diagnostic,
                              const std::string& label) {
    Program program = initial("Option<int>", "Some", scalar(Kind::Integer));
    mutate(program);
    require_rejection(std::move(program), diagnostic, label);
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
        require_success(conditional("Option<int>",
            constructor("Some", scalar(Kind::Integer)), constructor("None")),
            "Option<int> Some/None conditional");
        require_success(conditional("Option<int>",
            constructor("None"), constructor("Some", scalar(Kind::Integer))),
            "Option<int> None/Some conditional");
        require_success(conditional("Result<int,String>",
            constructor("Ok", scalar(Kind::Integer)), constructor("Err", scalar(Kind::String))),
            "Result<int,String> Ok/Err conditional");

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
        require_rejection(conditional("Option<int>",
            constructor("Some", scalar(Kind::Integer)), scalar(Kind::Integer)),
            "conditional sum branches", "reject Some vs raw int");
        require_rejection(conditional("Option<int>",
            scalar(Kind::Integer), constructor("None")),
            "conditional sum branches", "reject raw int vs None");
        require_rejection(conditional("Result<int,String>",
            constructor("Ok", scalar(Kind::Integer)), scalar(Kind::Integer)),
            "conditional sum branches", "reject Ok vs raw int");
        // A forged compiler AST is not trusted just because its name
        // resembles a previously parsed native Option/Result. No such
        // descriptor may reach the i64 handle code generator.
        require_invalid_registry([](Program& p) {
            p.sum_types.front().value_type = ScalarType::String;
        }, "type-shape mismatch", "forged Option<int> with String payload");
        require_invalid_registry([](Program& p) {
            p.sum_types.push_back(p.sum_types.front());
        }, "duplicate native sum type descriptor", "duplicate identical nominal descriptor");
        require_invalid_registry([](Program& p) {
            p.sum_types.front().kind = NativeSumType::Kind::Result;
        }, "type-shape mismatch", "Option name with Result kind");
        require_invalid_registry([](Program& p) {
            p.sum_types.front().kind = static_cast<NativeSumType::Kind>(255);
        }, "invalid native sum scalar ABI descriptor", "invalid nominal kind byte");
        require_invalid_registry([](Program& p) {
            p.sum_types.front().value_type = static_cast<ScalarType>(255);
        }, "invalid native sum scalar ABI descriptor", "invalid sum scalar kind byte");
        require_invalid_registry([](Program& p) {
            p.sum_types.front().error_type = ScalarType::Bool;
        }, "invalid native sum scalar ABI descriptor", "Option has no error payload type");
        require_invalid_registry([](Program& p) {
            p.sum_types.back().error_type = ScalarType::I64;
        }, "type-shape mismatch", "Result error payload doesn't match name");
        require_invalid_registry([](Program& p) {
            p.sum_types.back().error_type = static_cast<ScalarType>(255);
        }, "invalid native sum scalar ABI descriptor", "invalid Result error payload byte");

        // Malformed compiler-AST mutations must produce stable typed errors.
        {
            Program p = initial("Option<int>", "Some", scalar(Kind::Integer));
            p.functions.front().locals.front().initializer->arguments.front().reset();
            require_rejection(std::move(p), "malformed native sum constructor AST",
                "null Some payload is rejected");
        }
        require_rejection(conditional("Option<int>", nullptr, constructor("None")),
            "native conditional has missing value branch", "null conditional true arm");
        require_rejection(conditional("Option<int>", constructor("None"), nullptr),
            "native conditional has missing value branch", "null conditional false arm");
        for (auto kind : {Statement::Kind::ForEach, Statement::Kind::ForIndex,
                          Statement::Kind::ScopedLocal}) {
            Program p = initial("Option<int>", "None");
            Statement malformed;
            malformed.kind = kind;
            p.functions.front().statements.push_back(std::move(malformed));
            require_rejection(std::move(p),
                "native scoped/iteration statement requires local binding",
                "missing loop or scoped declaration binding");
        }

        // Both the resolver and scalar type checker must reject these AST
        // shapes before any dereference or ambiguous predicate lowering.
        {
            Program p = conditional("Option<int>",
                constructor("Some", scalar(Kind::Integer)), constructor("None"));
            p.functions.front().locals.front().initializer->condition.reset();
            require_rejection(std::move(p), "native conditional requires exactly one complete condition",
                "missing conditional predicate");
        }
        {
            Program p = conditional("Option<int>",
                constructor("Some", scalar(Kind::Integer)), constructor("None"));
            auto& e = p.functions.front().locals.front().initializer;
            e->condition.reset();
            e->condition_lhs = scalar(Kind::Boolean);
            require_rejection(std::move(p), "native conditional requires exactly one complete condition",
                "half-built legacy conditional predicate");
        }
        {
            Program p = conditional("Option<int>",
                constructor("Some", scalar(Kind::Integer)), constructor("None"));
            auto& e = p.functions.front().locals.front().initializer;
            e->condition_lhs = scalar(Kind::Boolean);
            e->condition_rhs = scalar(Kind::Boolean);
            require_rejection(std::move(p), "native conditional requires exactly one complete condition",
                "ambiguous direct and legacy conditional predicates");
        }
        {
            Program p = initial("Option<int>", "None");
            auto access = scalar(Kind::SumAccess);
            access->name = "is_some";
            p.functions.front().locals.front().initializer = std::move(access);
            require_rejection(std::move(p), "malformed native sum/member access AST",
                "null sum member receiver");
        }
        {
            Program p = initial("Option<int>", "None");
            auto access = scalar(Kind::SumAccess);
            access->name = "is_some";
            access->left = scalar(Kind::Variable);
            access->left->name = "value";
            access->arguments.push_back(nullptr);
            p.functions.front().locals.front().initializer = std::move(access);
            require_rejection(std::move(p), "malformed native sum/member access AST",
                "null sum member argument");
        }


        // Adversarial internal AST metadata: bound the call name vector and
        // reject missing statement children before either compiler pass uses them.
        for (std::size_t names : {1u, 3u}) {
            Program p = initial("Option<int>", "None");
            auto call = scalar(Kind::Call);
            call->name = "other";
            call->arguments.push_back(scalar(Kind::Integer));
            call->arguments.push_back(scalar(Kind::Integer));
            call->argument_names.resize(names);
            p.functions.front().locals.front().initializer = std::move(call);
            require_rejection(std::move(p), "native call argument-name metadata arity mismatch",
                "mismatched named call argument list");
        }
        {
            Program p = initial("Option<int>", "None");
            Statement st; st.kind = Statement::Kind::ForEach;
            st.branch_local = std::make_unique<LocalBinding>();
            st.branch_local->name = "item";
            p.functions.front().statements.push_back(std::move(st));
            require_rejection(std::move(p), "native for-of statement requires source expression",
                "missing for-of source");
        }
        for (int missing = 0; missing < 3; ++missing) {
            Program p = initial("Option<int>", "None");
            Statement st; st.kind = Statement::Kind::ForIndex;
            st.branch_local = std::make_unique<LocalBinding>();
            st.branch_local->name = "i";
            if (missing != 0) st.branch_local->initializer = scalar(Kind::Integer);
            if (missing != 1) st.condition = scalar(Kind::Boolean);
            if (missing != 2) st.loop_step = scalar(Kind::Integer);
            p.functions.front().statements.push_back(std::move(st));
            require_rejection(std::move(p),
                "native index loop requires initializer, condition and update",
                "index loop missing required child");
        }
        for (bool ambiguous : {false, true}) {
            Program p = initial("Option<int>", "None");
            Statement st; st.kind = Statement::Kind::Conditional;
            st.condition_lhs = scalar(Kind::Integer);
            if (ambiguous) st.condition = scalar(Kind::Boolean);
            p.functions.front().statements.push_back(std::move(st));
            require_rejection(std::move(p),
                "native if statement requires exactly one complete condition",
                "incomplete or ambiguous if predicate");
        }
        for (bool missingLength : {true, false}) {
            Program p = initial("Option<int>", "None");
            Statement st; st.kind = Statement::Kind::ArrayCommitFilled;
            if (!missingLength) st.array_commit_length = scalar(Kind::Integer);
            st.array_commit_values.push_back(missingLength ? scalar(Kind::Integer) : nullptr);
            p.functions.front().statements.push_back(std::move(st));
            require_rejection(std::move(p),
                missingLength ? "native array commit requires checked length" :
                "native array commit has missing value",
                "array commit missing mandatory child");
        }

        std::cout << "native sum typed payload admission: PASS 52 assertions (host-only)\n";
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
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "native sum typed payload admission: " << e.what() << '\n';
        return 1;
    }
}
