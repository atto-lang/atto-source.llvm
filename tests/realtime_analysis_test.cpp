#include "oreslang/types/RealtimeAnalysis.hpp"

#include <cstdint>
#include <memory>
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
        require(facts.calls.front() == native_function_slot("branch_only_callee", 0),
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

    return 0;
}
