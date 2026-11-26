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
    const bool borrowed_local = plan.form == Plan::Form::BorrowedLocal;
    if (!borrowed_local && plan.form != Plan::Form::OwnedRegion)
        ownership_error("forged pointer authority discriminant");
    if (borrowed_local) {
        auto valid_id = [](const std::string& name) {
            if (name.empty() ||
                    !((name[0] >= 'a' && name[0] <= 'z') ||
                      (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_'))
                return false;
            for (unsigned char ch : name)
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '_'))
                    return false;
            return true;
        };
        if (plan.count != 1 || !valid_id(plan.local_handle) ||
                plan.local_handle == plan.owner ||
                plan.local_handle == "Pointer" || plan.local_handle == "main" ||
                plan.local_handle == "int" || plan.local_handle == "return" ||
                plan.local_handle == "rt" || plan.local_handle == "ref" ||
                plan.local_handle == "deref" || plan.local_handle == "unref")
            ownership_error("borrowed local pointer lacks unique authenticated lexical handle");
        if (plan.steps.size() != 2 ||
                plan.steps[0].kind != Step::Kind::Scalar ||
                plan.steps[1].kind != Step::Kind::Close ||
                !plan.steps[0].value ||
                plan.steps[0].value->kind != Expr::Kind::Read ||
                plan.steps[0].value->index != 0 ||
                plan.steps[0].name == plan.owner ||
                plan.steps[0].name == plan.local_handle ||
                !plan.result ||
                plan.result->kind != Expr::Kind::Variable ||
                plan.result->name != plan.steps[0].name)
            ownership_error("local reference requires one readonly copy followed by unref and scalar return");
    } else if (!plan.local_handle.empty() || plan.local_initial != 0) {
        ownership_error("borrowed-local metadata cannot be smuggled into owning region");
    }
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
    if (borrowed_local) {
        initialized[0] = true;
        elements[0] = plan.local_initial;
    }
    std::unordered_map<std::string, std::int64_t> immutable;
    bool live = true;
    std::size_t close_count = 0;
    std::size_t budget = 0;

    // Independently recompute via unsigned magnitudes. No signed overflow
    // and no dependence on the parser's cached annotation or its bounds logic.
    auto checked = [](Expr::Kind kind, std::int64_t left, std::int64_t right) -> std::int64_t {
        auto mag = [](std::int64_t v) -> std::uint64_t {
            const auto u = static_cast<std::uint64_t>(v);
            return v < 0 ? std::uint64_t{0} - u : u;
        };
        const auto l = mag(left), r = mag(right);
        const bool ln = left < 0, rn = (right < 0) != (kind == Expr::Kind::Subtract);
        constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        bool negative;
        std::uint64_t value;
        if (kind == Expr::Kind::Multiply) {
            negative = ln != rn;
            const auto limit = max + static_cast<std::uint64_t>(negative);
            if (r != 0 && l > limit / r) ownership_error("checked integer arithmetic overflow");
            value = l * r;
        } else if (ln == rn) {
            negative = ln;
            const auto limit = max + static_cast<std::uint64_t>(negative);
            if (r > limit || l > limit - r) ownership_error("checked integer arithmetic overflow");
            value = l + r;
        } else {
            negative = l >= r ? ln : rn;
            value = l >= r ? l - r : r - l;
        }
        if (value > max + static_cast<std::uint64_t>(negative)) ownership_error("checked integer arithmetic overflow");
        if (negative && value == max + 1) return std::numeric_limits<std::int64_t>::min();
        return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
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
            result = checked(Expr::Kind::Subtract, 0, self(self, *expr.lhs, depth + 1));
            break;
        case Expr::Kind::Add:
        case Expr::Kind::Subtract:
        case Expr::Kind::Multiply: {
            if (!expr.lhs || !expr.rhs)
                ownership_error("malformed arithmetic expression");
            const auto left = self(self, *expr.lhs, depth + 1);
            const auto right = self(self, *expr.rhs, depth + 1);
            result = checked(expr.kind, left, right);
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
            if (borrowed_local)
                ownership_error("borrowed local pointer has no write_at authority");
            if (!live || !step.value || step.index >= plan.count || !step.name.empty())
                ownership_error("write after close, missing value, forged index or unsupported side effect");
            elements[step.index] = eval(eval, *step.value, 0);
            initialized[step.index] = true;
            break;
        case Step::Kind::Scalar:
            if (step.name.empty() || step.name == plan.owner ||
                    step.name == "Pointer" || step.name == "main" ||
                    (borrowed_local && step.name == plan.local_handle) ||
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
        ownership_error(borrowed_local ?
            "one borrowed local pointer must unref exactly once before return" :
            "one owned pointer must close exactly once before return");
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
