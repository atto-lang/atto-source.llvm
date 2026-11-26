#include "oreslang/frontend/Parser.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>
using namespace oreslang;
using K = Expression::Kind;

static Expression direct() {
    Expression e{};
    e.kind = K::SumAccess;
    e.name = "unwrap";
    e.sum_type = "Option<int>";
    e.left = std::make_unique<Expression>();
    e.left->kind = K::Variable;
    e.left->name = "state";
    return e;
}
int main() {
    unsigned checks = 0;
    auto require = [&](bool value) {
        if (!value) throw std::runtime_error("checked unwrap shape failure");
        ++checks;
    };
    require(native_trap_checked_option_unwrap(direct()));
    auto parser = direct(); parser.kind = K::ArrayCall;
    require(native_trap_direct_unwrap_shape(parser));
    require(!native_trap_checked_option_unwrap(parser));
    auto reject = [&](auto mutate) {
        auto e = direct();
        mutate(e);
        require(!native_trap_checked_option_unwrap(e));
    };
    reject([](auto& e){ e.sum_type = "Option<bool>"; });
    reject([](auto& e){ e.sum_type = "Result<int,int>"; });
    reject([](auto& e){ e.name = "expect"; });
    reject([](auto& e){ e.left.reset(); });
    reject([](auto& e){ e.left->kind = K::Call; });
    reject([](auto& e){ e.left->name.clear(); });
    reject([](auto& e){ e.left->right = std::make_unique<Expression>(); });
    reject([](auto& e){ e.arguments.push_back(std::make_unique<Expression>()); });
    reject([](auto& e){ e.left->arguments.push_back(std::make_unique<Expression>()); });
    reject([](auto& e){ e.argument_names.push_back("named"); });
    reject([](auto& e){ e.type_arguments_present = true; });
    reject([](auto& e){ e.left->type_arguments_present = true; });
    reject([](auto& e){ e.pipeline_stage = true; });
    reject([](auto& e){ e.left->pipeline_stage = true; });
    reject([](auto& e){ e.trap_origin = true; });
    reject([](auto& e){ e.left->trap_origin = true; });
    reject([](auto& e){ e.native_box_read = true; });
    reject([](auto& e){ e.left->native_box_read = true; });
    reject([](auto& e){ e.when_false = std::make_unique<Expression>(); });
    std::cout << "HOST_ONLY checked unwrap shape: " << checks << " assertions passed\n";
}
