#pragma once
// Shared pointer ownership + initialization validator. Executes before LLVM
// lowering and independently recomputes values/rights from HIR rather than
// trusting parser-produced constant annotations or stale admission.
#include "oreslang/types/PointerIntHIR.hpp"
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace oreslang::pointer_int_aot {

[[noreturn]] inline void ownership_error(const std::string& detail) {
    throw std::runtime_error("Pointer<int> ownership M2a: " + detail);
}

// A second, read-only verification is invoked by the backend immediately
// before emission. This prevents a stale authorization bit from surviving a
// later AST mutation by an optimization or compiler plug-in.
inline void verify_plan(const Plan& plan) {
    if (plan.count == 0 || plan.count > 16)
        ownership_error("pointer capacity must be 1..16");
    if (plan.owner.empty() || plan.owner == "Pointer" ||
            plan.owner == "main" || plan.owner == "int" ||
            !((plan.owner.front() >= 'a' && plan.owner.front() <= 'z') ||
              (plan.owner.front() >= 'A' && plan.owner.front() <= 'Z') ||
              plan.owner.front() == '_'))
        ownership_error("invalid nominal pointer owner");
    for (unsigned char ch : plan.owner)
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_'))
            ownership_error("pointer owner contains invalid identifier character");
    if (plan.steps.size() > 128 || !plan.result)
        ownership_error("invalid bounded pointer plan");
    std::array<bool, 16> initialized{};
    std::array<std::int64_t, 16> elements{};
    std::unordered_map<std::string, std::int64_t> immutable;
    bool live = true;
    std::size_t close_count = 0;
    std::size_t budget = 0;

    auto checked = [](const __int128 val) -> std::int64_t {
        if (val < std::numeric_limits<std::int64_t>::min() ||
                val > std::numeric_limits<std::int64_t>::max())
            ownership_error("checked integer arithmetic overflow");
        return static_cast<std::int64_t>(val);
    };
    auto eval = [&](auto&& self, const Expr& expr, unsigned depth) -> std::int64_t {
        if (depth > 64 || ++budget > 4096)
            ownership_error("expression depth/node budget exceeded");
        std::int64_t result = 0;
        switch (expr.kind) {
        case Expr::Kind::Literal:
            if (expr.lhs || expr.rhs || !expr.name.empty())
                ownership_error("malformed integer literal expression");
            result = expr.known;
            break;
        case Expr::Kind::Variable: {
            if (expr.lhs || expr.rhs || expr.name == plan.owner)
                ownership_error("untyped pointer owner alias or malformed scalar variable");
            const auto it = immutable.find(expr.name);
            if (it == immutable.end())
                ownership_error("scalar variable used before binding or pointer escape");
            result = it->second;
            break;
        }
        case Expr::Kind::Read:
            if (!live)
                ownership_error("read-after-close");
            if (expr.index >= plan.count || !initialized[expr.index])
                ownership_error("uninitialized or out-of-bounds read");
            if (expr.lhs || expr.rhs || !expr.name.empty())
                ownership_error("forged pointer read node");
            result = elements[expr.index];
            break;
        case Expr::Kind::Neg:
            if (!expr.lhs || expr.rhs)
                ownership_error("malformed unary pointer expression");
            result = checked(-static_cast<__int128>(self(self, *expr.lhs, depth + 1)));
            break;
        case Expr::Kind::Add:
        case Expr::Kind::Subtract:
        case Expr::Kind::Multiply: {
            if (!expr.lhs || !expr.rhs)
                ownership_error("malformed arithmetic expression");
            const __int128 left = self(self, *expr.lhs, depth + 1);
            const __int128 right = self(self, *expr.rhs, depth + 1);
            result = checked(expr.kind == Expr::Kind::Add ? left + right :
                             expr.kind == Expr::Kind::Subtract ? left - right :
                             left * right);
            break;
        }
        }
        if (expr.known != result)
            ownership_error("parser HIR constant annotation disagrees with ownership semantics");
        return result;
    };

    for (const Step& step : plan.steps) {
        switch (step.kind) {
        case Step::Kind::Write:
            if (!live || !step.value || step.index >= plan.count || !step.name.empty())
                ownership_error("write after close, missing value, forged index or unsupported side effect");
            elements[step.index] = eval(eval, *step.value, 0);
            initialized[step.index] = true;
            break;
        case Step::Kind::Scalar:
            if (step.name.empty() || step.name == plan.owner ||
                    step.name == "Pointer" || step.name == "main" ||
                    step.index != 0 || !step.value ||
                    immutable.count(step.name))
                ownership_error("duplicate or forged immutable scalar binding");
            immutable.emplace(step.name, eval(eval, *step.value, 0));
            break;
        case Step::Kind::Close:
            if (!live || step.value || !step.name.empty() || step.index != 0)
                ownership_error("double close or malformed close effect");
            live = false;
            ++close_count;
            break;
        }
    }
    if (live || close_count != 1)
        ownership_error("one owned pointer must close exactly once before return");
    (void)eval(eval, *plan.result, 0); // no post-close pointer reads
}

inline void validate_ownership(Plan& plan) {
    // Every validation attempt starts with no authority. The check runs
    // in the same source-order used by the IR emission stage.
    plan.ownership_admitted = false;
    verify_plan(plan);
    plan.ownership_admitted = true;
}

} // namespace oreslang::pointer_int_aot
