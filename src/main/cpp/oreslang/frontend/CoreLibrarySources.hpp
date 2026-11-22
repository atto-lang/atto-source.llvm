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
R"ORES_CORE(// Compiler-owned atto.core source (legacy ores.core alias).
// This module has no OS, Java, filesystem, network, process, actor-scheduler,
// or ambient project dependency.

define module numeric as

  pub fnc min_int(int left, int right): int {
    if left < right; do
      return left;
    fi
    return right;
  }

  pub fnc max_int(int left, int right): int {
    if left > right; do
      return left;
    fi
    return right;
  }

  pub fnc clamp_int(int value, int lower, int upper): int {
    if lower > upper; do
      return lower;
    fi
    if value < lower; do
      return lower;
    fi
    if value > upper; do
      return upper;
    fi
    return value;
  }

end
)ORES_CORE";
    static constexpr CoreLibrarySource kNumeric{
        CompilerLibraryTier::Core,
        "atto.core/numeric",
        "numeric",
        "@atto.core/numeric.atto",
        kNumericSource,
        0u
    };

    static constexpr std::string_view kIntBufferSource =
R"ORES_ALLOC(// Compiler-owned alloc tier source.
// This module requires an allocator provider but no OS, filesystem, network,
// process, actor scheduler, or ambient project dependency.

define module int_buffer as

  pub fnc sum_pair(int left, int right): int {
    let mut Array<int> values = new Array<int>();
    values.add(left);
    values.add(right);
    return values.get(0) + values.get(1);
  }

end
)ORES_ALLOC";
    static constexpr CoreLibrarySource kIntBuffer{
        CompilerLibraryTier::Alloc,
        "atto.alloc/int_buffer",
        "int_buffer",
        "@atto.alloc/int_buffer.atto",
        kIntBufferSource,
        core_provider_bit(CoreProviderRequirement::Allocator)
    };

    static constexpr std::string_view kIntBufferBundleSource =
R"ORES_ALLOC(// Compiler-owned alloc tier transitive-closure probe.
// The local body is allocation-free, but linking this module brings in the
// allocator-bearing int_buffer dependency and therefore requires Allocator.

import module int_buffer from "atto.alloc/int_buffer";

define module int_buffer_bundle as

  pub fnc marker(): int {
    return 42;
  }

end
)ORES_ALLOC";
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
