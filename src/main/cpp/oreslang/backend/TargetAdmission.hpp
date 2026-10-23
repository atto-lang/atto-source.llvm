#pragma once

#include "oreslang/frontend/Parser.hpp"
#include "oreslang/frontend/SemanticDependencies.hpp"
#include "oreslang/runtime/TargetAbi.hpp"

#include <algorithm>
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
