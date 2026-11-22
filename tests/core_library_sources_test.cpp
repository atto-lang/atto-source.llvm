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

    assert(is_reserved_core_import("atto.core"));
    assert(is_reserved_core_import("atto.core/numeric"));
    assert(is_reserved_core_import("atto.core/numeric.atto"));
    assert(is_reserved_core_import("ores.core"));
    assert(is_reserved_core_import("ores.core/numeric"));
    assert(!is_reserved_core_import("ores.std/numeric"));
    assert(!is_reserved_core_import("./ores.core/numeric"));
    assert(is_reserved_std_import("atto.std"));
    assert(is_reserved_std_import("atto.std/fs"));
    assert(is_reserved_std_import("ores.std"));
    assert(is_reserved_std_import("ores.std/testing"));
    assert(is_reserved_std_import("std"));
    assert(is_reserved_std_import("std/testing"));
    assert(!is_reserved_std_import("./std/testing"));
    assert(!is_reserved_std_import("ores.core/numeric"));
    assert(is_reserved_alloc_import("atto.alloc"));
    assert(is_reserved_alloc_import("atto.alloc/int_buffer"));
    assert(is_reserved_alloc_import("alloc"));
    assert(is_reserved_alloc_import("alloc/int_buffer"));
    assert(is_reserved_compiler_library_import("atto.core/numeric"));
    assert(is_reserved_compiler_library_import("atto.alloc/int_buffer"));
    assert(is_reserved_compiler_library_import("ores.core/numeric"));
    assert(is_reserved_compiler_library_import("alloc/int_buffer"));
    assert(!is_reserved_alloc_import("./alloc/int_buffer"));
    assert(!is_reserved_alloc_import("./atto.alloc/int_buffer"));
    assert(!is_reserved_compiler_library_import("atto.std/fs"));
    assert(!is_reserved_compiler_library_import("ores.std/testing"));
    assert(compiler_library_dependency_allowed(
        CompilerLibraryTier::Core, CompilerLibraryTier::Core));
    assert(!compiler_library_dependency_allowed(
        CompilerLibraryTier::Core, CompilerLibraryTier::Alloc));
    assert(compiler_library_dependency_allowed(
        CompilerLibraryTier::Alloc, CompilerLibraryTier::Core));
    assert(compiler_library_dependency_allowed(
        CompilerLibraryTier::Alloc, CompilerLibraryTier::Alloc));

    const auto* numeric = find_core_library_source("atto.core/numeric");
    assert(numeric == find_core_library_source("atto.core/numeric.atto"));
    assert(numeric == find_core_library_source("ores.core/numeric"));
    assert(numeric != nullptr);
    assert(numeric == find_core_library_source("ores.core/numeric.ores"));
    assert(numeric->tier == CompilerLibraryTier::Core);
    assert(numeric->module_name == "numeric");
    assert(numeric->unit_id == "@atto.core/numeric.atto");
    assert(numeric->canonical_import == "atto.core/numeric");
    assert(numeric->admitted_provider_bits == 0u);
    assert(numeric->source.find("define module numeric as") != std::string_view::npos);
    assert(numeric->source.find("clamp_int") != std::string_view::npos);

    const auto* slice = find_core_library_source("atto.core/slice");
    assert(slice != nullptr);
    assert(slice == find_core_library_source("atto.core/slice.atto"));
    assert(slice == find_core_library_source("ores.core/slice"));
    assert(slice == find_core_library_source("ores.core/slice.atto"));
    assert(slice->module_name == "slice");
    assert(slice->canonical_import == "atto.core/slice");
    assert(slice->unit_id == "@atto.core/slice.atto");
    assert(slice->admitted_provider_bits == 0u);
    assert(slice->source.find("define module slice as") != std::string_view::npos);
    assert(find_core_library_source("atto.core/slice.ores") == nullptr);

    const auto* iter = find_core_library_source("atto.core/iter");
    assert(iter != nullptr);
    assert(iter == find_core_library_source("atto.core/iter.atto"));
    assert(iter == find_core_library_source("ores.core/iter"));
    assert(iter == find_core_library_source("ores.core/iter.atto"));
    assert(iter->module_name == "iter");
    assert(iter->canonical_import == "atto.core/iter");
    assert(iter->unit_id == "@atto.core/iter.atto");
    assert(iter->admitted_provider_bits == 0u);
    assert(iter->source.find("define module iter as") != std::string_view::npos);
    assert(find_core_library_source("atto.core/iter.ores") == nullptr);

    const auto* cmp = find_core_library_source("atto.core/cmp");
    assert(cmp != nullptr);
    assert(cmp == find_core_library_source("atto.core/cmp.atto"));
    assert(cmp == find_core_library_source("ores.core/cmp"));
    assert(cmp == find_core_library_source("ores.core/cmp.atto"));
    assert(cmp->module_name == "cmp");
    assert(cmp->canonical_import == "atto.core/cmp");
    assert(cmp->unit_id == "@atto.core/cmp.atto");
    assert(cmp->admitted_provider_bits == 0u);
    assert(cmp->source.find("define module cmp as") != std::string_view::npos);
    assert(find_core_library_source("atto.core/cmp.ores") == nullptr);

    const auto* buffer = find_alloc_library_source("atto.alloc/int_buffer");
    assert(buffer == find_alloc_library_source("atto.alloc/int_buffer.atto"));
    assert(buffer == find_alloc_library_source("alloc/int_buffer"));
    assert(buffer != nullptr);
    assert(buffer == find_alloc_library_source("alloc/int_buffer.ores"));
    assert(buffer->tier == CompilerLibraryTier::Alloc);
    assert(buffer->module_name == "int_buffer");
    assert(buffer->unit_id == "@atto.alloc/int_buffer.atto");
    assert(buffer->canonical_import == "atto.alloc/int_buffer");
    assert(buffer->admitted_provider_bits ==
        bit(CoreProviderRequirement::Allocator));
    assert(buffer->source.find("new Array<int>()") != std::string_view::npos);

    const auto* bundle =
        find_alloc_library_source("atto.alloc/int_buffer_bundle");
    assert(bundle == find_alloc_library_source("atto.alloc/int_buffer_bundle.atto"));
    assert(bundle == find_alloc_library_source("alloc/int_buffer_bundle"));
    assert(bundle == find_alloc_library_source("alloc/int_buffer_bundle.ores"));
    assert(bundle != nullptr);
    assert(bundle->tier == CompilerLibraryTier::Alloc);
    assert(bundle->admitted_provider_bits ==
        bit(CoreProviderRequirement::Allocator));
    assert(bundle->source.find(
        "import module int_buffer from \"atto.alloc/int_buffer\"") !=
        std::string_view::npos);
    assert(find_core_library_source("alloc/int_buffer") == nullptr);

    assert(find_core_library_source("atto.core") == nullptr);
    assert(find_core_library_source("atto.core/numeric.ores") == nullptr);
    assert(find_core_library_source("atto.core/missing") == nullptr);
    assert(find_core_library_source("atto.core/../numeric") == nullptr);
    assert(find_core_library_source("atto.core//numeric") == nullptr);
    assert(find_alloc_library_source("atto.alloc/../int_buffer") == nullptr);
    assert(find_alloc_library_source("atto.alloc/int_buffer.ores") == nullptr);
    assert(find_core_library_source("ores.core") == nullptr);
    assert(find_core_library_source("ores.core/unknown") == nullptr);
    // Staged .atto source cannot silently become a linked LLVM library:
    // higher-order generics, effects and ownership need typed lowering first.
    assert(find_core_library_source("ores.core/collections") == nullptr);
    assert(find_compiler_library_source("ores.core/collections.atto") == nullptr);
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

    CoreClosureRequirements local{};
    CoreClosureRequirements allocator_dependency{
        bit(CoreProviderRequirement::Allocator), false};
    const auto transitive =
        merge_core_closure_requirements(local, allocator_dependency);
    assert(transitive.provider_bits ==
        bit(CoreProviderRequirement::Allocator));
    assert(!core_closure_admitted(transitive, 0u));
    assert(core_closure_admitted(
        transitive, bit(CoreProviderRequirement::Allocator)));
    const auto hosted_transitive =
        merge_core_closure_requirements(
            transitive, CoreClosureRequirements{0u, true});
    assert(hosted_transitive.hosted_or_unknown_effect);
    assert(!core_closure_admitted(
        hosted_transitive, 0xFFFFFFFFu));

    return 0;
}
