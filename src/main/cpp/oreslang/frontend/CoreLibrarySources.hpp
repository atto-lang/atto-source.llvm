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

// Public library layers, mirroring Rust's core/alloc/std split:
//   core  - freestanding, no heap/OS/scheduler (bare metal safe)
//   alloc - core plus an explicit allocator provider (arenas, buffers)
//   std   - hosted: requires an operating system / host runtime
enum class CompilerLibraryTier : std::uint8_t {
    Core,
    Alloc,
    Std
};

// Operating-system family used to select platform-variant library units
// (e.g. std/path). Import spelling never changes; only the selected source.
enum class LibraryTargetOs : std::uint8_t {
    Posix,
    Windows
};

constexpr LibraryTargetOs host_library_target_os() noexcept {
#if defined(_WIN32)
    return LibraryTargetOs::Windows;
#else
    return LibraryTargetOs::Posix;
#endif
}

// Process-wide selection, set once by the driver from --target-os=.
inline LibraryTargetOs& selected_library_target_os() noexcept {
    static LibraryTargetOs selected = host_library_target_os();
    return selected;
}

// 0 = any OS, 1 = POSIX only, 2 = Windows only.
enum class LibraryOsFilter : std::uint8_t { Any, Posix, Windows };

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
    LibraryOsFilter os = LibraryOsFilter::Any;
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
    return is_reserved_core_import(raw) || is_reserved_alloc_import(raw) ||
        is_reserved_std_import(raw);
}

inline std::string_view compiler_library_tier_name(CompilerLibraryTier tier) noexcept {
    switch (tier) {
        case CompilerLibraryTier::Core: return "core";
        case CompilerLibraryTier::Alloc: return "alloc";
        case CompilerLibraryTier::Std: return "std";
    }
    return "core";
}

// Canonical selector is "<layer>/<unit>". "atto.<layer>/<unit>" is an
// equivalent long form; "ores.<layer>/" is a legacy migration alias. An
// optional ".atto" suffix is accepted. Units are single flat identifiers:
// no traversal, nesting, dots or uppercase. Returns false for anything else.
inline bool normalize_compiler_library_import(
        std::string_view raw, CompilerLibraryTier& tier, std::string_view& unit) noexcept {
    struct Prefix { std::string_view text; CompilerLibraryTier tier; };
    static constexpr Prefix prefixes[] = {
        {"core/", CompilerLibraryTier::Core}, {"atto.core/", CompilerLibraryTier::Core},
        {"ores.core/", CompilerLibraryTier::Core},
        {"alloc/", CompilerLibraryTier::Alloc}, {"atto.alloc/", CompilerLibraryTier::Alloc},
        {"std/", CompilerLibraryTier::Std}, {"atto.std/", CompilerLibraryTier::Std},
        {"ores.std/", CompilerLibraryTier::Std},
    };
    for (const auto& prefix : prefixes) {
        if (raw.rfind(prefix.text, 0) != 0) continue;
        std::string_view rest = raw.substr(prefix.text.size());
        constexpr std::string_view suffix = ".atto";
        if (rest.size() > suffix.size() &&
                rest.substr(rest.size() - suffix.size()) == suffix)
            rest = rest.substr(0, rest.size() - suffix.size());
        if (rest.empty() || rest.size() > 64) return false;
        for (const char ch : rest) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
                return false;
        }
        if (rest[0] >= '0' && rest[0] <= '9') return false;
        tier = prefix.tier;
        unit = rest;
        return true;
    }
    return false;
}

inline bool compiler_library_dependency_allowed(
        CompilerLibraryTier importer,
        CompilerLibraryTier dependency) noexcept {
    // core -> core; alloc -> core|alloc; std -> core|alloc|std.
    return static_cast<std::uint8_t>(dependency) <= static_cast<std::uint8_t>(importer);
}

inline const CoreLibrarySource* find_compiler_library_source(
        std::string_view raw,
        LibraryTargetOs target_os = host_library_target_os()) noexcept {
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

    // Table generated at configure time from stdlib/LIBRARIES.txt
    // (cmake/AttoLibraryEmbed.cmake); one entry per layer/unit/OS variant.
    static constexpr CoreLibrarySource kTable[] = {
#include "oreslang/frontend/GeneratedAttoLibraryTable.inc"
    };

    if (raw == "core/numeric" || raw == "core/numeric.atto")
        return &kNumeric;
    if (raw == "alloc/int_buffer.atto") return &kIntBuffer;
    if (raw == "alloc/int_buffer_bundle.atto") return &kIntBufferBundle;
    {
        CompilerLibraryTier tier{};
        std::string_view unit;
        if (normalize_compiler_library_import(raw, tier, unit)) {
            for (const auto& entry : kTable) {
                if (entry.tier != tier || entry.module_name != unit) continue;
                if (entry.os == LibraryOsFilter::Posix && target_os != LibraryTargetOs::Posix) continue;
                if (entry.os == LibraryOsFilter::Windows && target_os != LibraryTargetOs::Windows) continue;
                return &entry;
            }
        }
    }
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
