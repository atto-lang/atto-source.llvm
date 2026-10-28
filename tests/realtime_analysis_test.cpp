#include "oreslang/types/RealtimeAnalysis.hpp"

#include <cstdint>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>

using namespace oreslang;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::unique_ptr<Expression> integer(std::int64_t value) {
    auto expression = std::make_unique<Expression>();
    expression->kind = Expression::Kind::Integer;
    expression->integer = value;
    return expression;
}

std::unique_ptr<Expression> call(const std::string& name) {
    auto expression = std::make_unique<Expression>();
    expression->kind = Expression::Kind::Call;
    expression->name = name;
    return expression;
}

Function pure(const std::string& name, std::unique_ptr<Expression> result) {
    Function function;
    function.name = name;
    function.result = std::move(result);
    return function;
}

std::unique_ptr<Expression> conditional(
        std::unique_ptr<Expression> when_true,
        std::unique_ptr<Expression> when_false) {
    auto expression = std::make_unique<Expression>();
    expression->kind = Expression::Kind::Conditional;
    expression->condition_lhs = integer(1);
    expression->condition_rhs = integer(1);
    expression->when_true = std::move(when_true);
    expression->when_false = std::move(when_false);
    return expression;
}

} // namespace

int main() {
    {
        Program program;
        program.functions.push_back(pure("leaf", integer(42)));
        program.functions.push_back(pure("middle", call("leaf")));
        program.functions.push_back(pure("main", call("middle")));

        const auto report = analyze_native_hard_rt(program);
        require(report.source_admitted, "pure transitive graph should admit");
        require(report.reachable_functions == 3, "reachable function count");
        require(report.max_call_depth == 3, "transitive call depth");
        require(report.transitive_effects.bits == 0, "pure graph effects");
        require(report.to_json().find("\"source_admitted\": true") != std::string::npos,
                "machine report admission");
        require(report.to_json().find("\"artifact_certified\": false") != std::string::npos,
                "compiler analysis must not certify final artifact");
    }

    {
        Program program;
        Function allocated = pure("uses_array", integer(1));
        LocalBinding local;
        local.name = "items";
        local.int_array = true;
        allocated.locals.push_back(std::move(local));
        program.functions.push_back(std::move(allocated));
        program.functions.push_back(pure("main", call("uses_array")));

        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted, "allocation must reject");
        require(report.diagnostic == "RT001_UNBOUNDED_ALLOCATION",
                "allocation diagnostic");
    }

    {
        Program program;
        Function io = pure("io_capable", integer(1));
        io.has_declared_effects = true;
        io.declared_effects.bits =
            static_cast<std::uint32_t>(FunctionEffect::Io);
        program.functions.push_back(std::move(io));
        program.functions.push_back(pure("main", call("io_capable")));

        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted, "declared IO capability must reject");
        require(report.reason.find("disallowed capability") != std::string::npos,
                "declared capability reason");
    }

    {
        Program program;
        program.functions.push_back(pure("loop_a", call("loop_b")));
        program.functions.push_back(pure("loop_b", call("loop_a")));
        program.functions.push_back(pure("main", call("loop_a")));

        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted, "recursive graph must reject");
        require(report.diagnostic == "RT008_UNBOUNDED_LOOP",
                "recursion diagnostic");
    }

    {
        Program program;
        Function bounded = pure("bounded", integer(1));
        bounded.scalar_while = std::make_unique<ScalarWhileLoop>();
        bounded.scalar_while->initial = 4;
        bounded.scalar_while->upper_bound = 20;
        program.functions.push_back(std::move(bounded));
        program.functions.push_back(pure("main", call("bounded")));

        const auto report = analyze_native_hard_rt(program);
        require(report.source_admitted, "literal bounded loop should admit");
        require(report.bounded_loop_iterations == 16,
                "bounded iteration accounting");
    }

    {
        Function entry = pure(
            "entry",
            conditional(integer(7), call("branch_only_callee")));
        const auto facts = realtime_detail::direct_facts(entry, 8);
        require(facts.calls.size() == 1,
                "ternary-only call must participate in realtime call graph");
        require(facts.calls.front() == realtime_detail::function_key("branch_only_callee", 0),
                "ternary-only call identity");
    }

    {
        Program program;
        Function disallowed = pure("branch_only_io", integer(1));
        disallowed.has_declared_effects = true;
        disallowed.declared_effects.bits =
            static_cast<std::uint32_t>(FunctionEffect::Io);
        program.functions.push_back(std::move(disallowed));
        program.functions.push_back(pure(
            "main",
            conditional(integer(7), call("branch_only_io"))));
        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted,
                "ternary arm effects must not evade realtime admission");
        require(report.reason.find("disallowed capability") != std::string::npos,
                "ternary arm effect reason");
    }

    {
        Function entry = pure("entry", integer(0));
        Statement conditional_statement;
        conditional_statement.kind = Statement::Kind::Conditional;
        conditional_statement.condition = call("statement_condition_only");
        entry.statements.push_back(std::move(conditional_statement));

        GuardedReturn guarded;
        guarded.condition = call("guard_condition_only");
        guarded.result = integer(1);
        entry.early_returns.push_back(std::move(guarded));

        const auto facts = realtime_detail::direct_facts(entry, 8);
        require(facts.calls.size() == 2,
                "bool statement/guard condition calls must enter realtime call graph");
        require(facts.calls[0] == realtime_detail::function_key("statement_condition_only", 0),
                "statement bool condition call identity");
        require(facts.calls[1] == realtime_detail::function_key("guard_condition_only", 0),
                "guard bool condition call identity");
    }

    {
        Program program;
        program.functions.push_back(pure("main", call("external")));
        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted, "external call must reject");
        require(report.diagnostic == "RT005_DYNAMIC_CODE",
                "external call diagnostic");
    }

    {
        Function limited = pure("limited", integer(0));
        for (int i = 0; i < 3; ++i) {
            Statement statement;
            statement.kind = Statement::Kind::Call;
            statement.expression = call("leaf");
            limited.statements.push_back(std::move(statement));
        }
        const auto facts = realtime_detail::direct_facts(limited, 1);
        require(facts.call_budget_exceeded,
                "direct call discovery must stop at supplied edge budget");
        require(facts.calls.size() == 1,
                "direct call vector must not grow beyond supplied edge budget");
    }

    {
        Program program;
        program.functions.push_back(pure("leaf", integer(1)));
        Function main = pure("main", integer(0));
        main.statements.reserve(realtime_detail::kMaxRealtimeCallEdges + 1);
        for (std::size_t i = 0; i <= realtime_detail::kMaxRealtimeCallEdges; ++i) {
            Statement statement;
            statement.kind = Statement::Kind::Call;
            statement.expression = call("leaf");
            main.statements.push_back(std::move(statement));
        }
        program.functions.push_back(std::move(main));
        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted,
                "call graph beyond edge qualification ceiling must reject");
        require(report.diagnostic == "RT003_WCET_UNPROVEN",
                "call-edge qualification diagnostic");
        require(report.reason ==
                    "hard-RT call-edge count exceeds compiler qualification ceiling",
                "call-edge qualification reason");
    }

    {
        Program program;
        constexpr int chain = 65;
        for (int i = 0; i < chain; ++i) {
            const std::string name = "depth_" + std::to_string(i);
            if (i + 1 == chain)
                program.functions.push_back(pure(name, integer(1)));
            else
                program.functions.push_back(
                    pure(name, call("depth_" + std::to_string(i + 1))));
        }
        program.functions.push_back(pure("main", call("depth_0")));
        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted,
                "call graph beyond qualification ceiling must reject");
        require(report.diagnostic == "RT003_WCET_UNPROVEN",
                "deep finite call graph diagnostic");
    }

    {
        Program program;
        auto remainder = std::make_unique<Expression>();
        remainder->kind = Expression::Kind::Remainder;
        remainder->left = integer(10);
        remainder->right = integer(3);
        program.functions.push_back(pure("main", std::move(remainder)));
        const auto report = analyze_native_hard_rt(program);
        require(report.source_admitted,
                "bounded pure signed remainder must be handled by RT switch");
    }

    {
        Program program;
        auto remainder = std::make_unique<Expression>();
        remainder->kind = Expression::Kind::Remainder;
        remainder->left = integer(10);
        remainder->right = integer(-1);
        program.functions.push_back(pure("main", std::move(remainder)));
        const auto report = analyze_native_hard_rt(program);
        require(report.source_admitted,
                "literal nonminimum numerator and -1 divisor are trap-free");
    }

    for (int mode = 0; mode < 4; ++mode) {
        Program program;
        auto remainder = std::make_unique<Expression>();
        remainder->kind = Expression::Kind::Remainder;
        remainder->left = integer(10);
        if (mode == 0) {
            remainder->right = integer(0);
        } else if (mode == 1) {
            remainder->right = std::make_unique<Expression>();
            remainder->right->kind = Expression::Kind::Variable;
            remainder->right->name = "unproven_divisor";
        } else if (mode == 2) {
            remainder->left = std::make_unique<Expression>();
            remainder->left->kind = Expression::Kind::Variable;
            remainder->left->name = "unproven_numerator";
            remainder->right = integer(-1);
        } else {
            remainder->left = integer(std::numeric_limits<std::int64_t>::min());
            remainder->right = integer(-1);
        }
        program.functions.push_back(pure("main", std::move(remainder)));
        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted,
                "unproven signed remainder trap must fail closed in hard RT");
        require(report.diagnostic == "RT006_UNBOUNDED_CLEANUP",
                "unsafe signed remainder must report panic/cleanup risk");
    }

    // Async syntax is represented on dedicated Function members, not in
    // the scalar expression tree. Every such member must poison the complete
    // reachable hard-RT graph even when the function returns a constant.
    for (int mode = 0; mode < 13; ++mode) {
        Program program;
        Function worker = pure("worker", integer(17));
        switch (mode) {
            case 0: worker.async_source_i64 = true; break;
            case 1: worker.source_tasks.emplace_back(); break;
            case 2: worker.future_pipelines.emplace_back(); break;
            case 3: worker.ready_await_actor = "actor"; break;
            case 4: worker.done_await_actor = "actor"; break;
            case 5: worker.ready_future_name = "future"; break;
            case 6: worker.ready_future_actor = "actor"; break;
            case 7: worker.ready_await_form = NativeAwaitForm::DirectHasFuture; break;
            case 8: worker.ready_await_form = NativeAwaitForm::ExplicitGetReady; break;
            case 9: worker.ready_await_form = NativeAwaitForm::ExplicitGetDone; break;
            case 10: worker.ready_await_form = NativeAwaitForm::FutureValue; break;
            case 11: {
                LocalBinding local;
                local.name = "task_result";
                local.task_await_name = "task";
                worker.locals.push_back(std::move(local));
                break;
            }
            case 12: {
                LocalBinding local;
                local.name = "future_result";
                local.future_await_name = "future";
                worker.locals.push_back(std::move(local));
                break;
            }
        }
        program.functions.push_back(std::move(worker));
        program.functions.push_back(pure("main", call("worker")));
        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted,
                "hidden async field must reject through transitive calls");
        require(report.diagnostic == "RT002_UNBOUNDED_BLOCKING",
                "hidden async fields must report suspension (not pure)");
        require(report.transitive_effects.has(FunctionEffect::Suspend),
                "hidden async field must contribute a suspend effect");
    }

    {
        Program program;
        Function generic = pure("generic_worker", integer(7));
        generic.generic_parameters.push_back({"T", ""});
        program.functions.push_back(std::move(generic));
        program.functions.push_back(pure("main", call("generic_worker")));
        const auto report = analyze_native_hard_rt(program);
        require(!report.source_admitted,
                "unspecialized generic source cannot prove closed hard-RT code");
        require(report.diagnostic == "RT005_DYNAMIC_CODE",
                "unproven generic specialization must reject fail closed");
    }

    {
        Program program;
        Function unreachable = pure("unreachable_async", integer(1));
        unreachable.source_tasks.emplace_back();
        program.functions.push_back(std::move(unreachable));
        program.functions.push_back(pure("main", integer(42)));
        const auto report = analyze_native_hard_rt(program);
        require(report.source_admitted,
                "unreachable async helper must not poison closed reachable entry");
        require(!report.transitive_effects.has(FunctionEffect::Suspend),
                "unreachable effects must not pollute main");
    }

    return 0;
}
