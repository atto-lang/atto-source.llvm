#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/ActorStacklessI64ABI.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::backend {

struct ActorStacklessI64Code {
    std::vector<std::uint64_t> words;
    std::uint64_t entry_instruction = 0;
};

// Deliberately *not* a general function lowering path. Only one-argument,
// pure, scalar, expression-only helpers are compiled to this bounded VM.
// Neither LLVM direct calls nor C++ recursion implement the guest call stack.
inline void require_m3_scalar_helpers(const Program& source) {
    if (source.functions.empty() || source.functions.size() > 16)
        throw std::runtime_error("actor M3 requires 1..16 pure scalar helper functions");
    std::unordered_set<std::string> names;
    for (const Function& fn : source.functions) {
        if (fn.name == "main" || !names.insert(fn.name).second ||
                fn.name.rfind("oreslang_", 0) == 0 ||
                fn.parameters.size() != 1 || fn.returns_void ||
                fn.return_type != ScalarType::I64 || !fn.result ||
                !fn.parameters[0].class_name.empty() ||
                !fn.parameters[0].generic_type.empty() ||
                !fn.parameters[0].sum_type.empty() ||
                fn.parameters[0].mode != Parameter::Mode::Value ||
                fn.parameters[0].scalar_type != ScalarType::I64 ||
                !fn.generic_parameters.empty() ||
                !fn.locals.empty() || !fn.statements.empty() ||
                !fn.early_returns.empty() || !fn.actor_spawns.empty() ||
                !fn.actor_sends.empty() || !fn.actor_stops.empty() ||
                !fn.actor_actions.empty() || !fn.actor_field_writes.empty() ||
                fn.has_declared_effects || fn.async_source_i64 ||
                fn.scalar_while || fn.caught_throw_value ||
                !fn.caught_throw_defers.empty() ||
                !fn.source_tasks.empty() || !fn.future_pipelines.empty() ||
                !fn.ready_await_actor.empty() || !fn.done_await_actor.empty() ||
                !fn.return_class.empty() || !fn.return_sum.empty()) {
            throw std::runtime_error(
                "actor M3 supports only one-argument pure int return-expression helpers");
        }
    }
}

inline bool actor_expr_has_helper_call(const Expression& expr) {
    if (expr.kind == Expression::Kind::Call) return true;
    auto walk = [](const std::unique_ptr<Expression>& p) {
        return p && actor_expr_has_helper_call(*p);
    };
    if (walk(expr.left) || walk(expr.right) || walk(expr.condition) ||
            walk(expr.condition_lhs) || walk(expr.condition_rhs) ||
            walk(expr.when_true) || walk(expr.when_false))
        return true;
    for (const auto& argument : expr.arguments)
        if (walk(argument)) return true;
    return false;
}

class ActorStacklessI64Compiler final {
public:
    explicit ActorStacklessI64Compiler(const Program& source) : source_(source) {}

    ActorStacklessI64Code compile(const Expression& root,
                                 const std::string& source_argument) {
        require_m3_scalar_helpers(source_);
        code_ = {};
        relocations_.clear();
        entries_.clear();
        nodes_ = 0;
        for (const auto& fn : source_.functions) {
            if (!entries_.emplace(fn.name, code_.words.size() / 2).second)
                throw std::runtime_error("actor M3 duplicate helper");
            emit(*fn.result, fn.parameters.front().name, 0);
            add(runtime::ActorScalarOp::Return);
        }
        code_.entry_instruction = code_.words.size() / 2;
        emit(root, source_argument, 0);
        add(runtime::ActorScalarOp::Return);
        for (const auto& patch : relocations_) {
            const auto found = entries_.find(patch.second);
            if (found == entries_.end())
                throw std::runtime_error("actor M3 helper call target unavailable");
            code_.words[patch.first * 2 + 1] = found->second;
        }
        if (code_.words.size() > runtime::actor_stackless_m3_max_words ||
                code_.words.size() % 2 != 0)
            throw std::runtime_error("actor M3 program exceeds 4096 words");
        return code_;
    }
private:
    void add(runtime::ActorScalarOp op, std::int64_t argument = 0) {
        if (code_.words.size() + 2 > runtime::actor_stackless_m3_max_words)
            throw std::runtime_error("actor M3 bytecode budget exceeded");
        code_.words.push_back(static_cast<std::uint64_t>(op));
        code_.words.push_back(static_cast<std::uint64_t>(argument));
    }
    void patch(std::size_t instr, std::size_t target) {
        if (instr >= code_.words.size() / 2 || target > code_.words.size() / 2)
            throw std::runtime_error("actor M3 invalid branch patch");
        code_.words[instr * 2 + 1] = target;
    }
    void emit(const Expression& expr, const std::string& param,
              std::size_t depth) {
        using Kind = Expression::Kind;
        using Op = runtime::ActorScalarOp;
        if (++nodes_ > 1024 || depth > 48)
            throw std::runtime_error("actor M3 expression budget exceeded");
        const auto child = [&](const std::unique_ptr<Expression>& p) {
            if (!p) throw std::runtime_error("actor M3 missing expression operand");
            emit(*p, param, depth + 1);
        };
        switch (expr.kind) {
            case Kind::Integer: add(Op::Constant, expr.integer); break;
            case Kind::Boolean: add(Op::Constant, expr.boolean ? 1 : 0); break;
            case Kind::Variable:
                if (expr.name != param)
                    throw std::runtime_error("actor M3 frame expression may access only its scalar parameter");
                add(Op::Argument);
                break;
            case Kind::Negate: child(expr.left); add(Op::Negate); break;
            case Kind::LogicalNot: child(expr.left); add(Op::LogicalNot); break;
            case Kind::Add: child(expr.left); child(expr.right); add(Op::Add); break;
            case Kind::Subtract: child(expr.left); child(expr.right); add(Op::Subtract); break;
            case Kind::Multiply: child(expr.left); child(expr.right); add(Op::Multiply); break;
            case Kind::Equal: child(expr.left); child(expr.right); add(Op::Equal); break;
            case Kind::NotEqual: child(expr.left); child(expr.right); add(Op::NotEqual); break;
            case Kind::Less: child(expr.left); child(expr.right); add(Op::Less); break;
            case Kind::LessEqual: child(expr.left); child(expr.right); add(Op::LessEqual); break;
            case Kind::Greater: child(expr.left); child(expr.right); add(Op::Greater); break;
            case Kind::GreaterEqual: child(expr.left); child(expr.right); add(Op::GreaterEqual); break;
            case Kind::LogicalXor: child(expr.left); child(expr.right); add(Op::LogicalXor); break;
            case Kind::Call: {
                if (expr.arguments.size() != 1 ||
                        (expr.argument_names.size() > 1 ||
                         (!expr.argument_names.empty() &&
                          !expr.argument_names.front().empty())) ||
                        expr.type_arguments_present)
                    throw std::runtime_error("actor M3 calls require exactly one positional int argument");
                child(expr.arguments.front());
                const auto slot = code_.words.size() / 2;
                add(Op::Call);
                relocations_.emplace_back(slot, expr.name);
                break;
            }
            case Kind::Conditional: {
                if (expr.condition) child(expr.condition);
                else {
                    child(expr.condition_lhs);
                    child(expr.condition_rhs);
                    add(Op::Equal);
                }
                const auto jump_false = code_.words.size() / 2;
                add(Op::JumpIfZero);
                child(expr.when_true);
                const auto jump_end = code_.words.size() / 2;
                add(Op::Jump);
                patch(jump_false, code_.words.size() / 2);
                child(expr.when_false);
                patch(jump_end, code_.words.size() / 2);
                break;
            }
            default:
                throw std::runtime_error("actor M3 expression has unsupported effects or ownership");
        }
    }
    const Program& source_;
    ActorStacklessI64Code code_{};
    std::unordered_map<std::string, std::size_t> entries_;
    std::vector<std::pair<std::size_t, std::string>> relocations_;
    std::size_t nodes_ = 0;
};

} // namespace oreslang::backend
