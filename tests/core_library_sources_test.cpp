#include "oreslang/frontend/CoreLibrarySources.hpp"
#include <cassert>
#include <cstdint>

using oreslang::ActorDecl;
using oreslang::Function;
using oreslang::FunctionEffect;
using oreslang::LocalBinding;
using oreslang::Program;
using oreslang::frontend::CoreProviderRequirement;

static std::uint32_t bit(CoreProviderRequirement requirement) {
    return oreslang::frontend::core_provider_bit(requirement);
}

int main() {
    using namespace oreslang::frontend;

    assert(is_reserved_core_import("ores.core"));
    assert(is_reserved_core_import("ores.core/numeric"));
    assert(!is_reserved_core_import("ores.std/numeric"));
    assert(!is_reserved_core_import("./ores.core/numeric"));

    const auto* numeric = find_core_library_source("ores.core/numeric");
    assert(numeric != nullptr);
    assert(numeric == find_core_library_source("ores.core/numeric.ores"));
    assert(numeric->module_name == "numeric");
    assert(numeric->unit_id == "@ores.core/numeric.ores");
    assert(numeric->admitted_provider_bits == 0u);
    assert(numeric->source.find("define module numeric as") != std::string_view::npos);
    assert(numeric->source.find("clamp_int") != std::string_view::npos);

    assert(find_core_library_source("ores.core") == nullptr);
    assert(find_core_library_source("ores.core/unknown") == nullptr);
    assert(find_core_library_source("ores.core/../numeric") == nullptr);
    assert(find_core_library_source("ores.core//numeric") == nullptr);
    assert(find_core_library_source("ores.core\\numeric") == nullptr);

    Program pure;
    auto requirements = infer_core_closure_requirements(pure);
    assert(requirements.provider_bits == 0u);
    assert(!requirements.hosted_or_unknown_effect);

    Program alloc;
    Function alloc_fn;
    LocalBinding owned;
    owned.heap_owned = true;
    alloc_fn.locals.push_back(std::move(owned));
    alloc.functions.push_back(std::move(alloc_fn));
    requirements = infer_core_closure_requirements(alloc);
    assert(requirements.provider_bits == bit(CoreProviderRequirement::Allocator));
    assert(!requirements.hosted_or_unknown_effect);

    Program suspended;
    Function suspend_fn;
    suspend_fn.has_declared_effects = true;
    suspend_fn.declared_effects.bits =
        static_cast<std::uint32_t>(FunctionEffect::Suspend);
    suspended.functions.push_back(std::move(suspend_fn));
    requirements = infer_core_closure_requirements(suspended);
    assert(requirements.provider_bits == bit(CoreProviderRequirement::Executor));
    assert(!requirements.hosted_or_unknown_effect);

    Program hosted;
    Function hosted_fn;
    hosted_fn.has_declared_effects = true;
    hosted_fn.declared_effects.bits =
        static_cast<std::uint32_t>(FunctionEffect::Io);
    hosted.functions.push_back(std::move(hosted_fn));
    requirements = infer_core_closure_requirements(hosted);
    assert(requirements.hosted_or_unknown_effect);

    Program unknown;
    Function unknown_fn;
    unknown_fn.has_declared_effects = true;
    unknown_fn.declared_effects.bits = 1u << 31;
    unknown.functions.push_back(std::move(unknown_fn));
    requirements = infer_core_closure_requirements(unknown);
    assert(requirements.hosted_or_unknown_effect);

    Program actor;
    actor.actors.emplace_back();
    requirements = infer_core_closure_requirements(actor);
    const auto actor_bits =
        bit(CoreProviderRequirement::Executor) |
        bit(CoreProviderRequirement::Mailbox) |
        bit(CoreProviderRequirement::TrustedControl);
    assert((requirements.provider_bits & actor_bits) == actor_bits);
    assert(!requirements.hosted_or_unknown_effect);

    return 0;
}
