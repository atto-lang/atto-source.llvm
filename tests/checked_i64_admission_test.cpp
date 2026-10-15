// A standalone C++20 admission regression, independent of LLVM libraries.
// Checked host execution currently exits on overflow and must never admit
// guest ownership, deferred cleanup, effects, recursive calls, or actor paths.
#include "oreslang/backend/CheckedI64Admission.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace oreslang;

static std::unique_ptr<Expression> integer(std::int64_t n) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Integer;
    e->integer = n;
    return e;
}
static std::unique_ptr<Expression> variable(std::string n) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Variable;
    e->name = std::move(n);
    return e;
}
static std::unique_ptr<Expression> binary(Expression::Kind kind,
                                           std::unique_ptr<Expression> a,
                                           std::unique_ptr<Expression> b) {
    auto e = std::make_unique<Expression>();
    e->kind = kind;
    e->left = std::move(a);
    e->right = std::move(b);
    return e;
}
static std::unique_ptr<Expression> call(std::string n) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Call;
    e->name = std::move(n);
    return e;
}
static Program good() {
    Program p;
    Function scale;
    scale.name = "scale";
    Parameter x;
    x.name = "x";
    scale.parameters.push_back(std::move(x));
    scale.result = binary(Expression::Kind::Multiply, variable("x"), integer(43));
    p.functions.push_back(std::move(scale));
    Function main;
    main.name = "main";
    main.result = call("scale");
    main.result->arguments.push_back(integer(1));
    p.functions.push_back(std::move(main));
    return p;
}
static void expect_reject(const char* label,
                          const std::function<void(Program&)>& mutate) {
    auto program = good();
    mutate(program);
    try {
        verify_checked_i64_host_profile(program);
    } catch (const std::runtime_error& e) {
        if (std::string(e.what()).find("checked-i64-v1:") == 0) return;
        throw std::runtime_error(std::string(label) + ": non-profile rejection");
    }
    throw std::runtime_error(std::string(label) + ": unexpectedly admitted");
}
int main() {
    verify_checked_i64_host_profile(good());
    {
        // Real Parser.cpp records a blank label per positional argument.
        auto p = good();
        p.functions.back().result->argument_names = {""};
        verify_checked_i64_host_profile(p);
    }
    {
        auto p = good();
        p.functions.back().result = binary(
            Expression::Kind::Add, integer(40), integer(3));
        verify_checked_i64_host_profile(p);
    }
    {
        auto p = good();
        auto negate = std::make_unique<Expression>();
        negate->kind = Expression::Kind::Negate;
        negate->left = integer(-43);
        p.functions.back().result = std::move(negate);
        verify_checked_i64_host_profile(p);
    }
    expect_reject("missing OS entry", [](Program& p) {
        p.functions.pop_back();
    });
    expect_reject("reserved diagnostic", [](Program& p) {
        p.functions.front().name = "puts";
    });
    expect_reject("empty function identity", [](Program& p) {
        p.functions.front().name.clear();
    });
    expect_reject("missing unary operand", [](Program& p) {
        auto e = std::make_unique<Expression>();
        e->kind = Expression::Kind::Negate;
        p.functions.back().result = std::move(e);
    });
    expect_reject("malformed positional labels", [](Program& p) {
        p.functions.back().result->argument_names = {"", ""};
    });
    expect_reject("null function-call argument", [](Program& p) {
        p.functions.back().result->arguments[0].reset();
    });
    expect_reject("module", [](Program& p) { p.module = "nonempty"; });
    expect_reject("class", [](Program& p) { p.classes.emplace_back(); });
    expect_reject("actor", [](Program& p) { p.actors.emplace_back(); });
    expect_reject("cooperative loop", [](Program& p) { p.cooperative_loops.emplace_back(); });
    expect_reject("void function", [](Program& p) { p.functions[0].returns_void = true; });
    expect_reject("missing result", [](Program& p) { p.functions[0].result.reset(); });
    expect_reject("owned local", [](Program& p) { p.functions[0].locals.emplace_back(); });
    expect_reject("statement", [](Program& p) { p.functions[0].statements.emplace_back(); });
    expect_reject("early return", [](Program& p) { p.functions[0].early_returns.emplace_back(); });
    expect_reject("actor send", [](Program& p) { p.functions[0].actor_sends.emplace_back(); });
    expect_reject("effects", [](Program& p) { p.functions[0].has_declared_effects = true; });
    expect_reject("caught throw", [](Program& p) { p.functions[0].caught_throw_value = integer(1); });
    expect_reject("borrowing", [](Program& p) {
        p.functions[0].parameters[0].mode = Parameter::Mode::Borrow;
    });
    expect_reject("class input", [](Program& p) {
        p.functions[0].parameters[0].class_name = "Owner";
    });
    expect_reject("unknown operand", [](Program& p) {
        p.functions[0].result = variable("nonexistent");
    });
    expect_reject("borrow expression", [](Program& p) {
        auto e = variable("x"); e->kind = Expression::Kind::Borrow;
        p.functions[0].result = std::move(e);
    });
    expect_reject("unresolved function", [](Program& p) {
        p.functions.back().result = call("missing");
    });
    expect_reject("wrong arity", [](Program& p) {
        p.functions.back().result = call("scale");
    });
    expect_reject("named argument", [](Program& p) {
        p.functions.back().result->argument_names = {"x"};
    });
    expect_reject("recursive call", [](Program& p) {
        p.functions.front().result = call("scale");
        p.functions.front().result->arguments.push_back(variable("x"));
    });
    expect_reject("mutual recursion", [](Program& p) {
        p.functions.front().result = call("main");
    });
    expect_reject("duplicate overload", [](Program& p) {
        Function duplicate;
        duplicate.name = "scale";
        duplicate.result = integer(1);
        p.functions.push_back(std::move(duplicate));
    });
    expect_reject("main parameters", [](Program& p) {
        Parameter param; param.name = "extra";
        p.functions.back().parameters.push_back(std::move(param));
    });
    expect_reject("expression depth", [](Program& p) {
        auto expr = integer(1);
        for (int n = 0; n < 50; ++n) {
            auto parent = std::make_unique<Expression>();
            parent->kind = Expression::Kind::Negate;
            parent->left = std::move(expr);
            expr = std::move(parent);
        }
        p.functions.back().result = std::move(expr);
    });
    return 0;
}
