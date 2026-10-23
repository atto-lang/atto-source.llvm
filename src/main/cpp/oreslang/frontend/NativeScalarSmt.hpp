#pragma once

#include "oreslang/frontend/Parser.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace oreslang::formal {

// Experimental proof adapter for a *narrow* source-checked native AST.
// Passing this through parse_program() enforces native ownership admission.
// It is not a proof of LLVM lowering, target integer semantics, or backend parity.
class NativeScalarSmt {
public:
    static std::string from_source(std::string_view source, std::string_view function) {
        if (source.size() > 2'000'000)
            throw std::invalid_argument("source proof budget exceeded");
        // This first scalar verifier admits an ASCII-only source profile:
        // do not assume that the lower-level parser applies the launcher's
        // UTF-8/NUL preflight when invoked directly by tooling.
        for (unsigned char byte : source) {
            if (byte == 0 || byte >= 0x80)
                throw std::invalid_argument("native scalar proof requires ASCII source");
        }
        // Parser validates names, types and ownership before reaching the adapter.
        const Program program = parse_program(source);
        return from_program(program, function);
    }

private:
    static constexpr std::size_t max_guards = 32;
    static constexpr std::size_t max_smt_bytes = 65536;

    [[noreturn]] static void reject(const char* why) {
        throw std::invalid_argument(why);
    }

    static std::string contract(std::string_view name) {
        if (name == "min_int")
            return "(ite (bvsle arg0 arg1) arg0 arg1)";
        if (name == "max_int")
            return "(ite (bvsge arg0 arg1) arg0 arg1)";
        if (name == "clamp_int")
            return "(ite (bvsgt arg1 arg2) arg1 "
                "(ite (bvslt arg0 arg1) arg1 "
                "(ite (bvsgt arg0 arg2) arg2 arg0)))";
        reject("unknown native scalar proof contract");
    }

    static std::string mutant(std::string_view name) {
        if (name == "min_int") return "(ite (bvsgt x y) x y)";
        if (name == "max_int") return "(ite (bvslt x y) x y)";
        if (name == "clamp_int")
            return "(ite (bvsgt y z) y (ite (bvslt x y) y x))";
        reject("unknown native scalar proof mutation");
    }

    static std::size_t expected_arity(std::string_view name) {
        if (name == "min_int" || name == "max_int") return 2;
        if (name == "clamp_int") return 3;
        reject("unknown native scalar proof function");
    }

    static std::string pure_slot(const Expression* expression,
                                 const std::unordered_map<std::string, std::string>& params) {
        if (!expression || expression->kind != Expression::Kind::Variable)
            reject("native scalar proof requires direct parameter reference");
        const auto it = params.find(expression->name);
        if (it == params.end())
            reject("native scalar proof references nonparameter name");
        return it->second;
    }

    static std::string pure_comparison(
            const Expression* expression,
            const std::unordered_map<std::string, std::string>& params) {
        if (!expression) reject("missing native proof condition");
        const char* op = nullptr;
        using Kind = Expression::Kind;
        switch (expression->kind) {
            case Kind::Less: op = "bvslt"; break;
            case Kind::LessEqual: op = "bvsle"; break;
            case Kind::Greater: op = "bvsgt"; break;
            case Kind::GreaterEqual: op = "bvsge"; break;
            default: reject("non-order native proof condition");
        }
        return "(" + std::string(op) + " " + pure_slot(expression->left.get(), params)
            + " " + pure_slot(expression->right.get(), params) + ")";
    }

    static std::string from_program(const Program& program, std::string_view name) {
        const std::size_t expected = expected_arity(name);
        if (!program.actors.empty() || !program.classes.empty()
                || !program.cooperative_loops.empty())
            reject("native proof cannot certify stateful program declarations");
        const Function* selected = nullptr;
        for (const auto& f : program.functions) {
            if (f.name == name) {
                if (selected) reject("ambiguous native proof overload");
                selected = &f;
            }
        }
        if (!selected) reject("missing native proof function");
        const auto& f = *selected;
        if (f.parameters.size() != expected || f.returns_void
                || f.return_type != ScalarType::I64 || !f.exported
                || !f.generic_parameters.empty() || !f.locals.empty()
                || !f.statements.empty() || !f.actor_spawns.empty()
                || !f.actor_sends.empty() || !f.actor_stops.empty()
                || !f.actor_actions.empty() || !f.actor_field_writes.empty()
                || !f.ready_await_actor.empty() || f.scalar_while
                || f.caught_throw_value || !f.caught_throw_defers.empty()
                || f.has_declared_effects || !f.result
                || f.early_returns.size() > max_guards)
            reject("native proof target has unsupported effects or signature");

        std::unordered_map<std::string, std::string> slots;
        for (std::size_t i = 0; i < expected; ++i) {
            const Parameter& p = f.parameters[i];
            if (p.mode != Parameter::Mode::Value || p.scalar_type != ScalarType::I64
                    || !p.class_name.empty() || !p.generic_type.empty()
                    || !slots.emplace(p.name, "arg" + std::to_string(i)).second)
                reject("native proof requires unique value i64 parameters");
        }
        std::string body = pure_slot(f.result.get(), slots);
        for (std::size_t i = f.early_returns.size(); i-- > 0;) {
            const auto& guard = f.early_returns[i];
            if (guard.lhs || guard.rhs)
                reject("unsupported native legacy compatibility guard");
            const std::string condition = pure_comparison(guard.condition.get(), slots);
            const std::string taken = pure_slot(guard.result.get(), slots);
            const std::size_t projected = 8 + condition.size() + taken.size() + body.size();
            if (projected > max_smt_bytes) reject("expanded native SMT proof too large");
            body = "(ite " + condition + " " + taken + " " + body + ")";
        }

        std::string out = "(set-logic QF_BV)\n(define-fun implementation (";
        for (std::size_t i = 0; i < expected; ++i) {
            if (i) out += " ";
            out += "(arg" + std::to_string(i) + " (_ BitVec 64))";
        }
        out += ") (_ BitVec 64) " + body + ")\n";
        out += "(define-fun contract (";
        for (std::size_t i = 0; i < expected; ++i) {
            if (i) out += " ";
            out += "(arg" + std::to_string(i) + " (_ BitVec 64))";
        }
        out += ") (_ BitVec 64) " + contract(name) + ")\n";
        const std::array<std::string,3> vars = {"x","y","z"};
        std::string args;
        for (std::size_t i = 0; i < expected; ++i) {
            out += "(declare-const " + vars[i] + " (_ BitVec 64))\n";
            args += " " + vars[i];
        }
        out += "(push 1)\n(assert (distinct (implementation" + args
            + ") (contract" + args + ")))\n(check-sat)\n(pop 1)\n";
        out += "(push 1)\n(assert (distinct " + mutant(name)
            + " (contract" + args + ")))\n(check-sat)\n(pop 1)\n";
        if (out.size() > max_smt_bytes) reject("native SMT proof budget exceeded");
        return out;
    }
};

} // namespace oreslang::formal
