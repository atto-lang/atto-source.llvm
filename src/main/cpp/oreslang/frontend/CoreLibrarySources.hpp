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
    return raw == "atto.core" || raw.rfind("atto.core/", 0) == 0 ||
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

    static constexpr std::string_view kHeapSource =
#include "oreslang/frontend/GeneratedHeapAtto.inc"
    ;
    static constexpr CoreLibrarySource kHeap{
        CompilerLibraryTier::Alloc,
        "atto.alloc/heap",
        "heap",
        "@atto.alloc/heap.atto",
        kHeapSource,
        0u  // Pure policies only. No physical allocation authority.
    };

    static constexpr std::string_view kArenaSource =
#include "oreslang/frontend/GeneratedArenaAtto.inc"
    ;
    static constexpr CoreLibrarySource kArena{
        CompilerLibraryTier::Alloc,
        "atto.alloc/arena",
        "arena",
        "@atto.alloc/arena.atto",
        kArenaSource,
        0u  // Pure policies only. No physical allocation authority.
    };

    static constexpr std::string_view kRegionSource =
#include "oreslang/frontend/GeneratedRegionAtto.inc"
    ;
    static constexpr CoreLibrarySource kRegion{
        CompilerLibraryTier::Alloc,
        "atto.alloc/region",
        "region",
        "@atto.alloc/region.atto",
        kRegionSource,
        0u  // Pure policies only. No physical allocation authority.
    };

    static constexpr std::string_view kPoolSource =
#include "oreslang/frontend/GeneratedPoolAtto.inc"
    ;
    static constexpr CoreLibrarySource kPool{
        CompilerLibraryTier::Alloc,
        "atto.alloc/pool",
        "pool",
        "@atto.alloc/pool.atto",
        kPoolSource,
        core_provider_bit(CoreProviderRequirement::Allocator)
    };

    static constexpr std::string_view kArcSource =
#include "oreslang/frontend/GeneratedArcAtto.inc"
    ;
    static constexpr CoreLibrarySource kArc{
        CompilerLibraryTier::Alloc,
        "atto.alloc/arc",
        "arc",
        "@atto.alloc/arc.atto",
        kArcSource,
        core_provider_bit(CoreProviderRequirement::Allocator)
    };

    static constexpr std::string_view kWeakSource =
#include "oreslang/frontend/GeneratedWeakAtto.inc"
    ;
    static constexpr CoreLibrarySource kWeak{
        CompilerLibraryTier::Alloc,
        "atto.alloc/weak",
        "weak",
        "@atto.alloc/weak.atto",
        kWeakSource,
        core_provider_bit(CoreProviderRequirement::Allocator)
    };

    if (raw == "atto.alloc/pool" || raw == "atto.alloc/pool.atto" ||
            raw == "alloc/pool" || raw == "alloc/pool.ores" ||
            raw == "alloc/pool.atto")
        return &kPool;
    if (raw == "atto.alloc/arc" || raw == "atto.alloc/arc.atto" ||
            raw == "alloc/arc" || raw == "alloc/arc.ores" ||
            raw == "alloc/arc.atto")
        return &kArc;
    if (raw == "atto.alloc/weak" || raw == "atto.alloc/weak.atto" ||
            raw == "alloc/weak" || raw == "alloc/weak.ores" ||
            raw == "alloc/weak.atto")
        return &kWeak;
    if (raw == "atto.alloc/heap" || raw == "atto.alloc/heap.atto" ||
            raw == "alloc/heap" || raw == "alloc/heap.ores" ||
            raw == "alloc/heap.atto")
        return &kHeap;
    if (raw == "atto.alloc/arena" || raw == "atto.alloc/arena.atto" ||
            raw == "alloc/arena" || raw == "alloc/arena.ores" ||
            raw == "alloc/arena.atto")
        return &kArena;
    if (raw == "atto.alloc/region" || raw == "atto.alloc/region.atto" ||
            raw == "alloc/region" || raw == "alloc/region.ores" ||
            raw == "alloc/region.atto")
        return &kRegion;
    if (raw == "atto.core/numeric" || raw == "atto.core/numeric.atto" ||
            raw == "ores.core/numeric" || raw == "ores.core/numeric.ores")
        return &kNumeric;
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
