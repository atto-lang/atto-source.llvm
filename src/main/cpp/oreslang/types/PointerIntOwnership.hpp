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
#include <utility>
#include <optional>

namespace oreslang::pointer_int_aot {

[[noreturn]] inline void ownership_error(const std::string& detail) {
    throw std::runtime_error("Pointer<int> ownership M2a: " + detail);
}

// A second, read-only verification is invoked by the backend immediately
// before emission. This prevents a stale authorization bit from surviving a
// later AST mutation by an optimization or compiler plug-in.

inline void verify_slot_plan(const Plan& plan) {
    const auto valid_id = [](const std::string& n) {
        if (n.empty() || !((n[0] >= 'a' && n[0] <= 'z') ||
                (n[0] >= 'A' && n[0] <= 'Z') || n[0] == '_')) return false;
        for (char c : n)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_')) return false;
        const std::string reserved[] = {"RawPointer", "Pointer", "main", "int", "return", "let",
            "mut", "val", "ref", "deref", "unref", "null", "true", "false", "to", "empty",
            "await", "rt", "pub", "fnc", "if", "else", "end"};
        for (const auto& word : reserved) if (n == word) return false;
        return true;
    };
    if (!valid_id(plan.local_handle) || !plan.owner.empty() || plan.local_initial ||
        !plan.readonly_helper.empty() || !plan.readonly_parameter.empty() ||
        !plan.mutable_helper.empty() || !plan.mutable_parameter.empty() ||
        !plan.mutable_scalar_parameter.empty() || plan.mutable_write ||
        plan.count != plan.slot_owners.size() || plan.count > 16 ||
        plan.steps.size() > 32 || !plan.result)
        ownership_error("invalid reusable slot authority");
    std::unordered_map<std::string, std::int64_t> locals;
    for (const auto& [name, value] : plan.slot_owners)
        if (!valid_id(name) || name == plan.local_handle || value < 0 ||
            !locals.emplace(name, value).second)
            ownership_error("invalid or duplicate local place");
    std::optional<std::size_t> bound;
    for (const auto& step : plan.steps) {
        if (step.second_index != 0) ownership_error("forged slot index");
        switch (step.kind) {
        case Step::Kind::SlotBind:
            if (bound || step.index >= plan.count || step.value || !step.name.empty())
                ownership_error("invalid slot bind");
            bound = step.index;
            break;
        case Step::Kind::SlotDetach:
            if (!bound || step.index || step.value || !step.name.empty())
                ownership_error("invalid slot detach");
            bound.reset();
            break;
        case Step::Kind::Scalar:
            if (!bound || !valid_id(step.name) || step.name == plan.local_handle ||
                step.index || !step.value || step.value->kind != Expr::Kind::Read ||
                step.value->index != *bound || step.value->lhs || step.value->rhs ||
                !step.value->name.empty() || step.value->known != plan.slot_owners[*bound].second ||
                !locals.emplace(step.name, step.value->known).second)
                ownership_error("forged slot read");
            break;
        default: ownership_error("unexpected reusable slot operation");
        }
    }
    if (bound) ownership_error("live slot on exit");
    std::size_t nodes = 0;
    auto eval = [&](auto&& self, const Expr& e, unsigned depth) -> std::int64_t {
        if (depth > 32 || ++nodes > 63 || e.index)
            ownership_error("slot result budget or forged index");
        std::int64_t value = 0;
        switch (e.kind) {
        case Expr::Kind::Literal:
            if (e.lhs || e.rhs || !e.name.empty() || e.known < 0)
                ownership_error("malformed slot literal");
            value = e.known; break;
        case Expr::Kind::Variable:
            if (e.lhs || e.rhs || !locals.count(e.name))
                ownership_error("slot result escape");
            value = locals.at(e.name); break;
        case Expr::Kind::Add:
        case Expr::Kind::Subtract: {
            if (!e.lhs || !e.rhs || !e.name.empty()) ownership_error("malformed slot arithmetic");
            const auto a = self(self, *e.lhs, depth + 1);
            const auto b = self(self, *e.rhs, depth + 1);
            if (e.kind == Expr::Kind::Add ? __builtin_add_overflow(a, b, &value) :
                __builtin_sub_overflow(a, b, &value))
                ownership_error("slot arithmetic overflow");
            break;
        }
        default: ownership_error("slot result must be scalar");
        }
        if (value != e.known) ownership_error("forged slot constant annotation");
        return value;
    };
    (void)eval(eval, *plan.result, 0);
}

inline void verify_plan(const Plan& plan) {
    if (plan.form == Plan::Form::ReusableSlot) { verify_slot_plan(plan); return; }
    if (!plan.slot_owners.empty()) ownership_error("slot metadata outside reusable slot authority");
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
    const auto valid_helper_id = [](const std::string& name) {
        if (name.empty() || !((name.front() >= 'a' && name.front() <= 'z') ||
                (name.front() >= 'A' && name.front() <= 'Z') || name.front() == '_')) return false;
        for (unsigned char ch : name)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_')) return false;
        return name != "main" && name != "Pointer" && name != "int";
    };
    if (!plan.readonly_helper.empty() || !plan.readonly_parameter.empty()) {
        if (borrowed_local || !valid_helper_id(plan.readonly_helper) ||
                !valid_helper_id(plan.readonly_parameter) ||
                plan.readonly_helper == plan.owner ||
                plan.readonly_helper == plan.readonly_parameter)
            ownership_error("invalid readonly helper signature");
    }
    if (!plan.mutable_helper.empty() || !plan.mutable_parameter.empty()) {
        if (borrowed_local || plan.mutable_helper == plan.readonly_helper ||
                !valid_helper_id(plan.mutable_helper) || !valid_helper_id(plan.mutable_parameter) ||
                plan.mutable_helper == plan.owner || plan.mutable_helper == plan.mutable_parameter)
            ownership_error("invalid mutable helper signature");
    } else if (plan.mutable_write != 0 || !plan.mutable_scalar_parameter.empty()) {
        ownership_error("orphan mutable helper write");
    }
    if (!plan.mutable_scalar_parameter.empty() &&
            (!valid_helper_id(plan.mutable_scalar_parameter) ||
             plan.mutable_scalar_parameter == plan.mutable_parameter ||
             plan.mutable_scalar_parameter == plan.mutable_helper || plan.mutable_write != 0))
        ownership_error("invalid scalar helper formal");
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
    bool scalar_call_argument = false;

    auto eval = [&](auto&& self, const Expr& expr, unsigned depth) -> std::int64_t {
        if (depth > 64 || ++budget > 4096)
            ownership_error("expression depth/node budget exceeded");
        if (scalar_call_argument && (expr.kind == Expr::Kind::Read ||
                expr.kind == Expr::Kind::ReadonlyCall || expr.kind == Expr::Kind::Length ||
                expr.kind == Expr::Kind::SizeBytes))
            ownership_error("conflicting access during mutable loan");
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
        case Expr::Kind::ReadonlyCall:
            if (borrowed_local || !live || plan.count != 1 || !initialized[0] ||
                    plan.readonly_helper.empty() || expr.name != plan.readonly_helper ||
                    expr.index != 0 || expr.lhs || expr.rhs)
                ownership_error("invalid readonly singleton call authority or shape");
            result = elements[0];
            break;
        case Expr::Kind::Length:
        case Expr::Kind::SizeBytes:
            if (!live || borrowed_local || expr.index != 0 ||
                    expr.lhs || expr.rhs || !expr.name.empty())
                ownership_error("invalid pointer metadata authority or shape");
            result = static_cast<std::int64_t>(plan.count *
                (expr.kind == Expr::Kind::Length ? 1 : 8));
            break;
        case Expr::Kind::Neg:
            if (!expr.lhs || expr.rhs)
                ownership_error("malformed unary pointer expression");
            if (__builtin_sub_overflow(std::int64_t{0}, self(self, *expr.lhs, depth + 1), &result))
                ownership_error("checked integer arithmetic overflow");
            break;
        case Expr::Kind::Add:
        case Expr::Kind::Subtract:
        case Expr::Kind::Multiply: {
            if (!expr.lhs || !expr.rhs)
                ownership_error("malformed arithmetic expression");
            const std::int64_t left = self(self, *expr.lhs, depth + 1);
            const std::int64_t right = self(self, *expr.rhs, depth + 1);
            // Match the parser's checked arithmetic without nonstandard wide
            // integer types or evaluating an overflowing signed operation.
            const bool overflow = expr.kind == Expr::Kind::Add ?
                __builtin_add_overflow(left, right, &result) :
                expr.kind == Expr::Kind::Subtract ?
                __builtin_sub_overflow(left, right, &result) :
                __builtin_mul_overflow(left, right, &result);
            if (overflow)
                ownership_error("checked integer arithmetic overflow");
            break;
        }
        default:
            ownership_error("invalid pointer expression kind");
        }
        if (expr.known != result)
            ownership_error("parser HIR constant annotation disagrees with ownership semantics");
        return result;
    };

    for (const Step& step : plan.steps) {
        if (step.kind != Step::Kind::Swap && step.second_index != 0)
            ownership_error("unexpected second pointer index");
        switch (step.kind) {
        case Step::Kind::Write:
            if (borrowed_local)
                ownership_error("borrowed local pointer has no write_at authority");
            if (!live || !step.value || step.index >= plan.count || !step.name.empty())
                ownership_error("write after close, missing value, forged index or unsupported side effect");
            elements[step.index] = eval(eval, *step.value, 0);
            initialized[step.index] = true;
            break;
        case Step::Kind::Fill: {
            if (borrowed_local || !live || !step.value || step.index != 0 || !step.name.empty())
                ownership_error("invalid fill authority or shape");
            const auto value = eval(eval, *step.value, 0);
            for (std::size_t i = 0; i < plan.count; ++i) {
                elements[i] = value;
                initialized[i] = true;
            }
            break;
        }
        case Step::Kind::Swap:
            if (borrowed_local || !live || step.value || !step.name.empty() ||
                    step.index >= plan.count || step.second_index >= plan.count ||
                    !initialized[step.index] || !initialized[step.second_index])
                ownership_error("invalid swap authority, bounds or initialization");
            std::swap(elements[step.index], elements[step.second_index]);
            break;
        case Step::Kind::MutableCall:
            if (borrowed_local || !live || plan.count != 1 || plan.mutable_helper.empty() ||
                    step.name != plan.mutable_helper || step.index != 0 ||
                    static_cast<bool>(step.value) != !plan.mutable_scalar_parameter.empty())
                ownership_error("invalid mutable singleton call authority or shape");
            scalar_call_argument = true;
            elements[0] = step.value ? eval(eval, *step.value, 0) : plan.mutable_write;
            scalar_call_argument = false;
            initialized[0] = true;
            break;
        case Step::Kind::Scalar:
            if (step.name.empty() || step.name == plan.owner ||
                    step.name == "Pointer" || step.name == "main" || step.name == plan.readonly_helper || step.name == plan.mutable_helper ||
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
        default:
            ownership_error("invalid pointer step kind");
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
