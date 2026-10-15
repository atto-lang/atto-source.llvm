// Build without LLVM libraries:
// c++ -std=c++20 -Isrc/main/cpp tests/experimental_hir_v1_exporter_test.cpp -o /tmp/hir-v1-exporter-test && /tmp/hir-v1-exporter-test
#include "oreslang/frontend/ExperimentalHirV1.hpp"
#include <cassert>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace oreslang;

static std::unique_ptr<Expression> integer(std::int64_t n) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Integer;
    e->integer = n;
    return e;
}
static std::unique_ptr<Expression> variable(const std::string& name) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Variable;
    e->name = name;
    return e;
}
static std::unique_ptr<Expression> multiply(std::unique_ptr<Expression> left,
                                             std::unique_ptr<Expression> right) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Multiply;
    e->left = std::move(left);
    e->right = std::move(right);
    return e;
}
static std::unique_ptr<Expression> call(std::string name, std::unique_ptr<Expression> arg) {
    auto e = std::make_unique<Expression>();
    e->kind = Expression::Kind::Call;
    e->name = std::move(name);
    e->arguments.push_back(std::move(arg));
    return e;
}
template<typename F>
static void rejects(F&& fn) {
    bool rejected = false;
    try { fn(); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
}
int main() {
    Program program;
    Function main;
    main.name = "main";
    main.exported = true;
    main.result = call("counter_response", integer(1));
    program.functions.push_back(std::move(main));
    Function response;
    response.name = "counter_response";
    response.exported = true;
    Parameter count;
    count.name = "count";
    response.parameters.push_back(count);
    response.result = multiply(variable("count"), integer(43));
    program.functions.push_back(std::move(response));
    const std::string json = emit_experimental_hir_v1(program);
    assert(json.find("\"schema_version\":\"1.0.0-experimental\"") != std::string::npos);
    assert(json.find("\"module_id\":\"source\"") != std::string::npos);
    assert(json.find("\"value\":\"43\"") != std::string::npos);
    assert(json.find("\"name\":\"counter_response\"") < json.find("\"name\":\"main\""));
    assert(json == emit_experimental_hir_v1(program));

    // Force fail-closed admission for unsupported native source features.
    program.functions[0].returns_void = true;
    rejects([&] { (void) emit_experimental_hir_v1(program); });
    program.functions[0].returns_void = false;

    program.functions[0].result->kind = Expression::Kind::ArrayCall;
    rejects([&] { (void) emit_experimental_hir_v1(program); });
    program.functions[0].result = call("counter_response", integer(1));

    program.functions[1].parameters[0].mode = Parameter::Mode::BorrowMut;
    rejects([&] { (void) emit_experimental_hir_v1(program); });
    program.functions[1].parameters[0].mode = Parameter::Mode::Value;

    program.functions[1].parameters[0].name = "bad\"id";
    rejects([&] { (void) emit_experimental_hir_v1(program); });
    program.functions[1].parameters[0].name = "count";

    program.functions[0].name = "counter_response";
    rejects([&] { (void) emit_experimental_hir_v1(program); });
    std::cout << json;
    return 0;
}
