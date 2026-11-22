#pragma once

#include "oreslang/frontend/Parser.hpp"
#include <cstdint>
#include <string_view>

namespace oreslang::frontend {

enum class CoreProviderRequirement : std::uint32_t {
    Allocator = 1u << 0,
    Executor = 1u << 1,
    Mailbox = 1u << 2,
    TrustedControl = 1u << 3,
    MonotonicClock = 1u << 4
};

enum class CompilerLibraryTier : std::uint8_t {
    Core,
    Alloc
};

constexpr std::uint32_t core_provider_bit(CoreProviderRequirement requirement) noexcept {
    return static_cast<std::uint32_t>(requirement);
}

struct CoreClosureRequirements {
    std::uint32_t provider_bits = 0;
    bool hosted_or_unknown_effect = false;
};

inline CoreClosureRequirements merge_core_closure_requirements(
        CoreClosureRequirements local,
        CoreClosureRequirements dependency) noexcept {
    return CoreClosureRequirements{
        local.provider_bits | dependency.provider_bits,
        local.hosted_or_unknown_effect || dependency.hosted_or_unknown_effect
    };
}

inline bool core_closure_admitted(
        CoreClosureRequirements requirements,
        std::uint32_t admitted_provider_bits) noexcept {
    return !requirements.hosted_or_unknown_effect &&
        (requirements.provider_bits & ~admitted_provider_bits) == 0u;
}

struct CoreLibrarySource {
    CompilerLibraryTier tier = CompilerLibraryTier::Core;
    std::string_view canonical_import;
    std::string_view module_name;
    std::string_view unit_id;
    std::string_view source;
    std::uint32_t admitted_provider_bits = 0;
};

inline bool is_reserved_core_import(std::string_view raw) noexcept {
    return raw == "core" || raw.rfind("core/", 0) == 0 ||
        raw == "atto.core" || raw.rfind("atto.core/", 0) == 0 ||
        raw == "ores.core" || raw.rfind("ores.core/", 0) == 0;
}

inline bool is_reserved_std_import(std::string_view raw) noexcept {
    return raw == "atto.std" || raw.rfind("atto.std/", 0) == 0 ||
        raw == "ores.std" || raw.rfind("ores.std/", 0) == 0 ||
        raw == "std" || raw.rfind("std/", 0) == 0;
}

inline bool is_reserved_alloc_import(std::string_view raw) noexcept {
    return raw == "atto.alloc" || raw.rfind("atto.alloc/", 0) == 0 ||
        raw == "alloc" || raw.rfind("alloc/", 0) == 0;
}

inline bool is_reserved_compiler_library_import(std::string_view raw) noexcept {
    return is_reserved_core_import(raw) || is_reserved_alloc_import(raw);
}

inline bool compiler_library_dependency_allowed(
        CompilerLibraryTier importer,
        CompilerLibraryTier dependency) noexcept {
    return importer == CompilerLibraryTier::Alloc ||
        dependency == CompilerLibraryTier::Core;
}

inline const CoreLibrarySource* find_compiler_library_source(std::string_view raw) noexcept {
    // Canonical Atto imports and legacy Ores imports must share one immutable
    // compiler-owned unit identity; no spelling may duplicate modules.
    static constexpr std::string_view kNumericSource =
#include "oreslang/frontend/GeneratedNumericAtto.inc"
    ;
    static constexpr CoreLibrarySource kNumeric{
        CompilerLibraryTier::Core,
        "atto.core/numeric",
        "numeric",
        "@atto.core/numeric.atto",
        kNumericSource,
        0u
    };

    static constexpr std::string_view kRangeSource =
#include "oreslang/frontend/GeneratedCoreRangeAtto.inc"
    ;
    static constexpr CoreLibrarySource kRange{
        CompilerLibraryTier::Core,
        "atto.core/range",
        "range",
        "@atto.core/range.atto",
        kRangeSource,
        0u
    };

    static constexpr std::string_view kCellSource =
#include "oreslang/frontend/GeneratedCoreCellAtto.inc"
    ;
    static constexpr CoreLibrarySource kCell{
        CompilerLibraryTier::Core,
        "atto.core/cell",
        "cell",
        "@atto.core/cell.atto",
        kCellSource,
        0u
    };

    static constexpr std::string_view kContractsSource =
#include "oreslang/frontend/GeneratedCoreContractsAtto.inc"
    ;
    static constexpr CoreLibrarySource kContracts{
        CompilerLibraryTier::Core,
        "atto.core/contracts",
        "contracts",
        "@atto.core/contracts.atto",
        kContractsSource,
        0u
    };

    static constexpr std::string_view kNumSource =
#include "oreslang/frontend/GeneratedCoreNumAtto.inc"
    ;
    static constexpr CoreLibrarySource kNum{
        CompilerLibraryTier::Core,
        "atto.core/num",
        "num",
        "@atto.core/num.atto",
        kNumSource,
        0u
    };

    static constexpr std::string_view kIntBufferSource =
#include "oreslang/frontend/GeneratedIntBufferAtto.inc"
    ;
    static constexpr CoreLibrarySource kIntBuffer{
        CompilerLibraryTier::Alloc,
        "atto.alloc/int_buffer",
        "int_buffer",
        "@atto.alloc/int_buffer.atto",
        kIntBufferSource,
        core_provider_bit(CoreProviderRequirement::Allocator)
    };

    static constexpr std::string_view kIntBufferBundleSource =
#include "oreslang/frontend/GeneratedIntBufferBundleAtto.inc"
    ;
    static constexpr CoreLibrarySource kIntBufferBundle{
        CompilerLibraryTier::Alloc,
        "atto.alloc/int_buffer_bundle",
        "int_buffer_bundle",
        "@atto.alloc/int_buffer_bundle.atto",
        kIntBufferBundleSource,
        core_provider_bit(CoreProviderRequirement::Allocator)
    };

    if (raw == "atto.core/numeric" || raw == "atto.core/numeric.atto" ||
            raw == "ores.core/numeric" || raw == "ores.core/numeric.ores")
        return &kNumeric;
    if (raw == "core/range" || raw == "core/range.atto" ||
            raw == "atto.core/range" || raw == "atto.core/range.atto" ||
            raw == "ores.core/range" || raw == "ores.core/range.atto")
        return &kRange;
    if (raw == "core/cell" || raw == "core/cell.atto" ||
            raw == "atto.core/cell" || raw == "atto.core/cell.atto" ||
            raw == "ores.core/cell" || raw == "ores.core/cell.atto")
        return &kCell;
    if (raw == "core/contracts" || raw == "core/contracts.atto" ||
            raw == "atto.core/contracts" || raw == "atto.core/contracts.atto" ||
            raw == "ores.core/contracts" || raw == "ores.core/contracts.atto")
        return &kContracts;
    if (raw == "core/num" || raw == "core/num.atto" ||
            raw == "atto.core/num" || raw == "atto.core/num.atto" ||
            raw == "ores.core/num" || raw == "ores.core/num.atto")
        return &kNum;
    if (raw == "atto.alloc/int_buffer" || raw == "atto.alloc/int_buffer.atto" ||
            raw == "alloc/int_buffer" || raw == "alloc/int_buffer.ores")
        return &kIntBuffer;
    if (raw == "atto.alloc/int_buffer_bundle" || raw == "atto.alloc/int_buffer_bundle.atto" ||
            raw == "alloc/int_buffer_bundle" || raw == "alloc/int_buffer_bundle.ores")
        return &kIntBufferBundle;
    return nullptr;
}

inline const CoreLibrarySource* find_core_library_source(
        std::string_view raw) noexcept {
    const auto* source = find_compiler_library_source(raw);
    return source && source->tier == CompilerLibraryTier::Core ? source : nullptr;
}

inline const CoreLibrarySource* find_alloc_library_source(
        std::string_view raw) noexcept {
    const auto* source = find_compiler_library_source(raw);
    return source && source->tier == CompilerLibraryTier::Alloc ? source : nullptr;
}

inline CoreClosureRequirements infer_core_closure_requirements(
        const Program& program) noexcept {
    CoreClosureRequirements requirements;

    const auto add = [&](CoreProviderRequirement requirement) {
        requirements.provider_bits |= core_provider_bit(requirement);
    };

    const auto inspect_function = [&](const Function& fn) {
        if (!fn.declared_effects.valid()) {
            requirements.hosted_or_unknown_effect = true;
        } else {
            if (fn.declared_effects.has(FunctionEffect::Alloc))
                add(CoreProviderRequirement::Allocator);
            if (fn.declared_effects.has(FunctionEffect::Suspend))
                add(CoreProviderRequirement::Executor);
            if (fn.declared_effects.has(FunctionEffect::Io) ||
                    fn.declared_effects.has(FunctionEffect::Block) ||
                    fn.declared_effects.has(FunctionEffect::Ffi)) {
                requirements.hosted_or_unknown_effect = true;
            }
        }

        if (fn.async_source_i64 || !fn.source_tasks.empty() ||
                !fn.future_pipelines.empty() || !fn.ready_await_actor.empty() ||
                !fn.done_await_actor.empty()) {
            add(CoreProviderRequirement::Executor);
        }

        if (!fn.actor_spawns.empty() || !fn.actor_sends.empty() ||
                !fn.actor_stops.empty() || !fn.actor_field_writes.empty()) {
            add(CoreProviderRequirement::Executor);
            add(CoreProviderRequirement::Mailbox);
            add(CoreProviderRequirement::TrustedControl);
        }

        for (const auto& local : fn.locals) {
            if (local.heap_owned || local.int_array)
                add(CoreProviderRequirement::Allocator);
            if (local.new_class) {
                for (const auto& cls : program.classes) {
                    if (cls.name != local.class_name) continue;
                    for (const auto& field : cls.fields) {
                        if (field.int_array) {
                            add(CoreProviderRequirement::Allocator);
                            break;
                        }
                    }
                    break;
                }
            }
        }
    };

    for (const auto& fn : program.functions)
        inspect_function(fn);

    if (!program.actors.empty()) {
        add(CoreProviderRequirement::Executor);
        add(CoreProviderRequirement::Mailbox);
        add(CoreProviderRequirement::TrustedControl);
        for (const auto& actor : program.actors)
            inspect_function(actor.on_message);
    }

    return requirements;
}

} // namespace oreslang::frontend
