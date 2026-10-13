#include "oreslang/backend/LlvmCodegen.hpp"
#include "oreslang/frontend/Parser.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
constexpr const char* kValid =
    "import module Assert from \"std/assert\"; "
    "pub fnc check_equal(int a, int b): bool { return Assert.equal(a, b); }";

using Mutation = std::function<void(oreslang::Program&)>;

void should_reject(const char* name, const Mutation& mutate,
                   const std::string& expected) {
    auto ast = oreslang::parse_program(kValid);
    mutate(ast);
    try {
        const auto ir = oreslang::emit_verified_llvm_ir(ast, false);
        (void)ir;
    } catch (const std::exception& failure) {
        if (std::string(failure.what()).find(expected) != std::string::npos)
            return;
        throw std::runtime_error(std::string(name) + " wrong rejection: " +
                                 failure.what());
    }
    throw std::runtime_error(std::string(name) + " bypassed codegen admission");
}
}

int main() {
    const auto ast = oreslang::parse_program(kValid);
    const auto ir = oreslang::emit_verified_llvm_ir(ast, false);
    if (ir.find("define zeroext i1 @check_equal(i64 ") == std::string::npos ||
            ir.find("icmp eq i64") == std::string::npos)
        throw std::runtime_error("valid sealed call did not emit primitive bool IR");

    should_reject("forged import qualifier", [](oreslang::Program& p) {
        p.functions[0].result->left->name = "Untrusted";
    }, "direct equal/not_equal primitive return");
    should_reject("forged AST qualifier kind", [](oreslang::Program& p) {
        p.functions[0].result->left->kind = oreslang::Expression::Kind::Integer;
    }, "direct equal/not_equal primitive return");
    should_reject("missing sealed import", [](oreslang::Program& p) {
        p.imports.clear();
    }, "sealed std/assert import");
    should_reject("missing import but retained synthetic module call",
                  [](oreslang::Program& p) {
        p.imports.clear();
        p.functions[0].returns_bool = false;
    }, "sealed std/assert import");
    should_reject("altered canonical import path", [](oreslang::Program& p) {
        p.imports[0].canonical_path = "../std/assert";
    }, "sealed host-only import");
    should_reject("forged imported symbol", [](oreslang::Program& p) {
        p.imports[0].symbol = "Untrusted";
    }, "sealed host-only import");
    should_reject("malformed argument subtree", [](oreslang::Program& p) {
        p.functions[0].result->arguments[0].reset();
    }, "scalar arguments must be int literals");
    should_reject("forged return kind", [](oreslang::Program& p) {
        p.functions[0].result->kind = oreslang::Expression::Kind::Call;
    }, "direct equal/not_equal primitive return");

    std::cout << "ASSERT_NATIVE_BACKEND_AST_SEAL_PASS\n";
    return 0;
}
