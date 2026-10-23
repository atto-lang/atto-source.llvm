#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/SemanticDependencies.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <algorithm>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>

namespace oreslang {

enum class NativeTargetKind {
    Host,
    Wasm32,
    CortexM
};

inline NativeTargetKind native_target_kind(
        bool wasm32_target,
        bool cortex_m_target) {
    if (wasm32_target && cortex_m_target)
        throw std::runtime_error(
            "conflicting LLVM target selectors");
    if (wasm32_target) return NativeTargetKind::Wasm32;
    if (cortex_m_target) return NativeTargetKind::CortexM;
    return NativeTargetKind::Host;
}

namespace target_admission_detail {

inline void validate_actor_field_writes(
        const ActorDecl& actor) {
    for (const auto& write :
            actor.on_message.actor_field_writes) {
        if (!write.expression)
            throw std::runtime_error(
                "actor field write lowering requires an expression");
        const auto field = std::find_if(
            actor.fields.begin(),
            actor.fields.end(),
            [&](const LocalBinding& candidate) {
                return candidate.name == write.field;
            });
        if (field == actor.fields.end() ||
                !field->field_mutable)
            throw std::runtime_error(
                "invalid actor field write lowering");
    }
}

inline void validate_host_entrypoint(
        const Program& source) {
    const auto main_it = std::find_if(
        source.functions.begin(),
        source.functions.end(),
        [](const Function& fn) {
            return fn.name == "main";
        });
    if (main_it == source.functions.end()) return;

    if (!main_it->parameters.empty())
        throw std::runtime_error(
            "native main() must have zero arguments");

    if (source.module.empty() &&
            std::any_of(
                source.functions.begin(),
                source.functions.end(),
                [](const Function& fn) {
                    return fn.name ==
                           "__oreslang_guest_main";
                }))
        throw std::runtime_error(
            "reserved native entrypoint symbol collision");
}

inline void validate_wasm32(
        const Program& source) {
    const auto profile =
        TargetAbi::parse("wasm32-unknown-unknown");
    if (!profile.experimental_wasm32_ir())
        throw std::runtime_error(
            "wasm32 target profile is not enabled");
    if (profile.pointer_bits() != 32 ||
            profile.endian() != "little")
        throw std::runtime_error(
            "wasm32 target ABI/data layout mismatch");

    if (source.actors.size() > 1)
        throw std::runtime_error(
            "wasm32 actor guest supports one actor per instance");

    for (const auto& actor : source.actors) {
        if (actor.fields.empty() ||
                actor.fields.size() > 16)
            throw std::runtime_error(
                "wasm32 actor guest requires 1..16 scalar fields");
        if (actor.on_message.parameters.size() != 1 ||
                !actor.on_message.result ||
                actor.on_message.returns_void ||
                !actor.on_message.statements.empty() ||
                !actor.on_message.locals.empty() ||
                !actor.on_message.early_returns.empty())
            throw std::runtime_error(
                "wasm32 actor guest requires synchronous scalar handler");

        const auto dependencies =
            scan_semantic_dependencies(actor);
        if (!dependencies.direct_calls.empty())
            throw std::runtime_error(
                "wasm32 actor guest callable expressions are not implemented");
    }

    for (const auto& klass : source.classes)
        for (const auto& field : klass.fields)
            if (field.int_array)
                throw std::runtime_error(
                    "wasm32 class Array<int> owned fields are not implemented");

    for (const auto& fn : source.functions) {
        if (fn.caught_throw_value)
            throw std::runtime_error(
                "wasm32 native caught-throw guest unsupported");

        if (!fn.actor_spawns.empty() ||
                !fn.actor_sends.empty() ||
                !fn.actor_stops.empty() ||
                !fn.actor_field_writes.empty())
            throw std::runtime_error(
                "wasm32 actor operations are not implemented");

        for (const auto& local : fn.locals) {
            if (local.heap_owned)
                throw std::runtime_error(
                    "wasm32 native heap-owned regions are not implemented");
            if (local.int_array)
                throw std::runtime_error(
                    "wasm32 native heap-owned/Array<int> regions are not implemented");
        }
    }

    if (!source.cooperative_loops.empty())
        throw std::runtime_error(
            "wasm32 resumable source loop is unsupported");
}

inline void validate_cortex_m() {
    const auto profile =
        TargetAbi::parse("thumbv7m-none-eabi");
    if (!profile.experimental_cortexm_ir())
        throw std::runtime_error(
            "Cortex-M IR emission is not admitted");
    if (profile.pointer_bits() != 32 ||
            profile.endian() != "little")
        throw std::runtime_error(
            "Cortex-M ABI/data layout mismatch");
}

} // namespace target_admission_detail

inline void validate_lowering_structure(const Program& source) {
    using namespace target_admission_detail;
    for (const auto& actor : source.actors) {
        if (actor.fields.empty() || actor.fields.size() > 16)
            throw std::runtime_error(
                "actor lowering requires 1..16 scalar fields");
        if (actor.on_message.parameters.size() != 1 ||
                !actor.on_message.result ||
                actor.on_message.returns_void ||
                !actor.on_message.statements.empty() ||
                !actor.on_message.locals.empty() ||
                !actor.on_message.early_returns.empty())
            throw std::runtime_error(
                "actor lowering requires one synchronous scalar handler");

        std::set<std::string> field_names;
        for (const auto& field : actor.fields) {
            if (!field.initializer)
                throw std::runtime_error(
                    "actor field lowering requires an initializer");
            if (field.heap_owned || field.int_array ||
                    !field.class_name.empty() ||
                    field.new_class || field.move_class)
                throw std::runtime_error(
                    "actor lowering requires scalar field storage");
            if (!field_names.insert(field.name).second)
                throw std::runtime_error(
                    "duplicate actor field in lowering structure");
        }

        if (field_names.count(
                actor.on_message.parameters.front().name))
            throw std::runtime_error(
                "actor handler parameter shadows a field");

        validate_actor_field_writes(actor);
    }
}

inline void validate_target_admission(
        const Program& source,
        NativeTargetKind target) {
    using namespace target_admission_detail;

    if (source.functions.size() == 1 &&
            source.functions.front().scalar_while) {
        if (target != NativeTargetKind::Host ||
                !source.classes.empty() ||
                !source.actors.empty())
            throw std::runtime_error(
                "experimental scalar loop unsupported target/owners");
    }

    validate_lowering_structure(source);

    switch (target) {
        case NativeTargetKind::Host:
            validate_host_entrypoint(source);
            return;
        case NativeTargetKind::Wasm32:
            validate_wasm32(source);
            return;
        case NativeTargetKind::CortexM:
            validate_cortex_m();
            return;
    }

    throw std::runtime_error("unknown native target kind");
}

inline void validate_cortex_m_source_program(
        const Program& source,
        bool experimental_owned_int,
        bool experimental_rt_scalar_loop) {
    if (experimental_owned_int || experimental_rt_scalar_loop ||
            !source.module.empty() || !source.classes.empty() ||
            !source.cooperative_loops.empty() || source.functions.empty())
        throw std::runtime_error(
            "cortex-m target supports only freestanding scalar functions");

    constexpr std::size_t max_functions = 64;
    constexpr std::size_t max_parameters = 8;
    constexpr std::size_t max_expression_nodes = 4096;
    constexpr std::size_t max_expression_depth = 32;
    constexpr std::size_t max_call_depth = 16;

    if (source.functions.size() > max_functions)
        throw std::runtime_error(
            "cortex-m function count exceeds 64");

    std::size_t expression_nodes = 0;
    std::unordered_map<std::string, std::vector<std::string>> edges;
    std::function<bool(
        const Expression&,
        std::size_t,
        std::vector<std::string>&)> scalar =
        [&](const Expression& expr,
            std::size_t depth,
            std::vector<std::string>& calls) -> bool {
            if (depth > max_expression_depth ||
                    ++expression_nodes > max_expression_nodes)
                throw std::runtime_error(
                    "cortex-m expression budget exceeded");

            using Kind = Expression::Kind;
            switch (expr.kind) {
                case Kind::Integer:
                case Kind::Variable:
                    return true;
                case Kind::Negate:
                    return expr.left &&
                           scalar(*expr.left, depth + 1, calls);
                case Kind::Add:
                case Kind::Subtract:
                case Kind::Multiply:
                    return expr.left && expr.right &&
                           scalar(*expr.left, depth + 1, calls) &&
                           scalar(*expr.right, depth + 1, calls);
                case Kind::Call:
                    if (expr.arguments.size() > max_parameters)
                        return false;
                    calls.push_back(expr.name);
                    for (const auto& arg : expr.arguments)
                        if (!arg ||
                                !scalar(
                                    *arg,
                                    depth + 1,
                                    calls))
                            return false;
                    return true;
                default:
                    return false;
            }
        };

    if (!source.actors.empty()) {
        if (source.actors.size() > 8 ||
                source.functions.size() != 1 ||
                source.functions.front().name != "main")
            throw std::runtime_error(
                "cortex-m actors require 1..8 actor types and only main");

        const auto& start = source.functions.front();
        if (start.returns_void || !start.result ||
                start.result->kind != Expression::Kind::Integer ||
                !start.locals.empty() ||
                !start.statements.empty() ||
                !start.early_returns.empty() ||
                start.has_declared_effects ||
                start.actor_spawns.empty() ||
                start.actor_spawns.size() > 8 ||
                start.actor_sends.size() > 32 ||
                start.actor_stops.size() !=
                    start.actor_spawns.size() ||
                !start.parameters.empty())
            throw std::runtime_error(
                "cortex-m actor bootstrap requires bounded source main");

        if (start.actor_actions.size() !=
                start.actor_sends.size() +
                start.actor_stops.size())
            throw std::runtime_error(
                "cortex-m actor action plan inconsistent");

        for (const auto& actor : source.actors) {
            if (actor.fields.empty() ||
                    actor.fields.size() > 8 ||
                    actor.on_message.actor_field_writes.size() > 16 ||
                    !actor.on_message.result)
                throw std::runtime_error(
                    "cortex-m actor field or handler budget exceeded");

            for (const auto& field : actor.fields) {
                if (!field.initializer ||
                        field.initializer->kind !=
                            Expression::Kind::Integer)
                    throw std::runtime_error(
                        "cortex-m actors need literal i64 field initialization");
            }

            std::vector<std::string> calls;
            if (!scalar(
                    *actor.on_message.result,
                    0,
                    calls) ||
                    !calls.empty())
                throw std::runtime_error(
                    "cortex-m actor return must be scalar without guest calls");

            for (const auto& write :
                    actor.on_message.actor_field_writes) {
                calls.clear();
                if (!write.expression ||
                        !scalar(*write.expression, 0, calls) ||
                        !calls.empty())
                    throw std::runtime_error(
                        "cortex-m actor writes must be scalar without guest calls");
            }
        }

        for (const auto& send : start.actor_sends) {
            if (!send.payload ||
                    send.payload->kind !=
                        Expression::Kind::Integer)
                throw std::runtime_error(
                    "cortex-m actor send must be a literal i64");
        }

        std::unordered_map<std::string, std::size_t> pending;
        std::unordered_set<std::string> stopped;
        for (const auto& spawn : start.actor_spawns) {
            if (!pending.emplace(spawn.name, 0).second)
                throw std::runtime_error(
                    "cortex-m duplicate actor ref");
        }

        for (const auto& action : start.actor_actions) {
            std::string ref;
            if (action.kind == ActorAction::Kind::Send) {
                if (action.index >= start.actor_sends.size())
                    throw std::runtime_error(
                        "cortex-m invalid actor send index");
                ref = start.actor_sends[action.index].reference;
            } else {
                if (action.index >= start.actor_stops.size())
                    throw std::runtime_error(
                        "cortex-m invalid actor stop index");
                ref = start.actor_stops[action.index];
            }

            const auto it = pending.find(ref);
            if (it == pending.end() || stopped.count(ref))
                throw std::runtime_error(
                    "cortex-m invalid or stopped actor capability");

            if (action.kind == ActorAction::Kind::Send) {
                if (++it->second > 16)
                    throw std::runtime_error(
                        "cortex-m static mailbox capacity exceeded (16)");
            } else {
                stopped.insert(ref);
            }
        }

        if (stopped.size() != pending.size())
            throw std::runtime_error(
                "cortex-m bootstrap must stop all actors");
    }

    for (const auto& fn : source.functions) {
        if ((!source.actors.empty() && fn.name != "main") ||
                (source.actors.empty() && fn.name == "main") ||
                fn.returns_void || !fn.result ||
                !fn.locals.empty() ||
                !fn.statements.empty() ||
                !fn.early_returns.empty() ||
                (source.actors.empty() &&
                    (!fn.actor_spawns.empty() ||
                     !fn.actor_sends.empty() ||
                     !fn.actor_stops.empty() ||
                     !fn.actor_actions.empty() ||
                     !fn.actor_field_writes.empty())) ||
                fn.scalar_while ||
                fn.caught_throw_value ||
                !fn.caught_throw_defers.empty() ||
                fn.has_declared_effects)
            throw std::runtime_error(
                "cortex-m target rejects main, effects or non-scalar functions");

        if (fn.parameters.size() > max_parameters)
            throw std::runtime_error(
                "cortex-m parameter budget exceeded");

        for (const auto& param : fn.parameters)
            if (!param.class_name.empty() ||
                    param.mode != Parameter::Mode::Value)
                throw std::runtime_error(
                    "cortex-m target requires value int parameters");

        std::vector<std::string> calls;
        if (!scalar(*fn.result, 0, calls))
            throw std::runtime_error(
                "cortex-m target requires bounded scalar expressions");

        if (!edges.emplace(fn.name, std::move(calls)).second)
            throw std::runtime_error(
                "cortex-m target rejects overloaded function names");
    }

    std::unordered_set<std::string> active;
    std::unordered_map<std::string, std::size_t> longest;
    std::function<std::size_t(const std::string&)> walk =
        [&](const std::string& name) -> std::size_t {
            const auto known = longest.find(name);
            if (known != longest.end())
                return known->second;

            if (!active.insert(name).second)
                throw std::runtime_error(
                    "cortex-m recursive call graph rejected");

            auto node = edges.find(name);
            if (node == edges.end())
                throw std::runtime_error(
                    "cortex-m unresolved function call");

            std::size_t depth = 1;
            for (const auto& dependency : node->second) {
                const auto child = walk(dependency);
                if (child >= max_call_depth)
                    throw std::runtime_error(
                        "cortex-m call depth budget exceeded");
                depth = std::max(depth, child + 1);
            }

            active.erase(name);
            longest.emplace(name, depth);
            return depth;
        };

    for (const auto& fn : source.functions)
        (void)walk(fn.name);
}

inline void validate_target_admission(
        const Program& source,
        bool wasm32_target,
        bool cortex_m_target) {
    validate_target_admission(
        source,
        native_target_kind(
            wasm32_target,
            cortex_m_target));
}

} // namespace oreslang
