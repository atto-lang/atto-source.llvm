#include "oreslang/types/OwnershipChecker.hpp"

#include <stdexcept>
#include <string>
#include <unordered_map>

namespace oreslang {
namespace {

struct LocalOwnership {
    bool moved = false;
};

// Validate in the exact order expressions are evaluated by LlvmCodegen.
// A primitive i64 is Copy, but explicit `rt take x` invalidates x in Java
// even when ordinary implicit reads/calls are allowed to copy the integer.
void check_expression(const Expression& expr,
                      std::unordered_map<std::string, LocalOwnership>& locals) {
    switch (expr.kind) {
        case Expression::Kind::Integer:
            return;
        case Expression::Kind::Variable: {
            auto found = locals.find(expr.name);
            if (found == locals.end()) {
                throw std::runtime_error(
                    "internal native ownership error: undeclared variable '" + expr.name + "'");
            }
            if (found->second.moved) {
                throw std::runtime_error("use of explicitly moved int '" + expr.name
                                         + "' at byte " + std::to_string(expr.offset));
            }
            return;
        }
        case Expression::Kind::Copy:
        case Expression::Kind::Share:
            // Native i64 has no owner graph to clone or publish. These are
            // validated copies of scalar values, never Shared<T> handles.
            check_expression(*expr.left, locals);
            return;
        case Expression::Kind::Take:
            check_expression(*expr.left, locals);
            if (expr.left->kind == Expression::Kind::Variable) {
                locals.at(expr.left->name).moved = true;
            }
            return;
        case Expression::Kind::Negate:
            check_expression(*expr.left, locals);
            return;
        case Expression::Kind::Add:
        case Expression::Kind::Subtract:
        case Expression::Kind::Multiply:
            check_expression(*expr.left, locals);
            check_expression(*expr.right, locals);
            return;
        case Expression::Kind::Call:
            // Keep all argument ownership effects live until invocation.
            // The supported parameters are owned i64 by value; there are
            // no borrowed or structural references in this native ABI.
            for (const auto& arg : expr.arguments) {
                check_expression(*arg, locals);
            }
            return;
    }
    throw std::runtime_error("invalid native ownership expression kind");
}

} // namespace

void validate_i64_ownership(const Program& program) {
    for (const auto& function : program.functions) {
        std::unordered_map<std::string, LocalOwnership> locals;
        for (const auto& param : function.parameters) {
            locals.emplace(param, LocalOwnership{});
        }
        check_expression(*function.result, locals);
    }
}

} // namespace oreslang
