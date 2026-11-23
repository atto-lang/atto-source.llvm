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
    // core/alloc/std are all reserved roots (core-alloc-std-libraries-v1).
    assert(is_reserved_compiler_library_import("atto.std/fs"));
    assert(is_reserved_compiler_library_import("ores.std/testing"));
    assert(is_reserved_compiler_library_import("core/num"));
    assert(is_reserved_compiler_library_import("std/path"));
    assert(!is_reserved_compiler_library_import("./core/num"));
    assert(!is_reserved_compiler_library_import("std.fs"));
    assert(compiler_library_dependency_allowed(
        CompilerLibraryTier::Std, CompilerLibraryTier::Alloc));
    assert(!compiler_library_dependency_allowed(
        CompilerLibraryTier::Alloc, CompilerLibraryTier::Std));
    assert(!compiler_library_dependency_allowed(
        CompilerLibraryTier::Core, CompilerLibraryTier::Std));
    {
        const auto* num = find_compiler_library_source("core/num");
        assert(num != nullptr && num->tier == CompilerLibraryTier::Core);
        assert(num == find_compiler_library_source("atto.core/num.atto"));
        assert(num->canonical_import == "atto.core/num");
        assert(find_compiler_library_source("ores.core/num") == nullptr);
        assert(find_compiler_library_source("ores.std/io") == nullptr);
        assert(find_compiler_library_source("core/../num") == nullptr);
        assert(find_compiler_library_source("core/Num") == nullptr);
        assert(find_compiler_library_source("core/x/num") == nullptr);
        const auto* arena = find_compiler_library_source("alloc/arena");
        assert(arena != nullptr && arena->tier == CompilerLibraryTier::Alloc);
        assert(arena->admitted_provider_bits == bit(CoreProviderRequirement::Allocator));
        const auto* posix = find_compiler_library_source("std/path", LibraryTargetOs::Posix);
        const auto* windows = find_compiler_library_source("std/path", LibraryTargetOs::Windows);
        assert(posix && windows && posix != windows);
        assert(posix->tier == CompilerLibraryTier::Std && posix->module_name == "path");
        assert(posix->source.find("return 47;") != std::string_view::npos);
        assert(windows->source.find("return 92;") != std::string_view::npos);
    }
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

    // The three new helpers are compiler-owned and cannot resolve from disk.
    for (std::string_view path : {"atto.core/math", "atto.core/hash", "atto.core/cell"}) {
        const auto* owned = find_core_library_source(path);
        assert(owned != nullptr);
        assert(owned->tier == CompilerLibraryTier::Core);
        assert(owned->admitted_provider_bits == 0u);
        assert(!owned->source.empty());
    }
    assert(find_core_library_source("atto.core/math") ==
           find_core_library_source("ores.core/math"));
    assert(find_core_library_source("atto.core/hash") ==
           find_core_library_source("ores.core/hash"));
    assert(find_core_library_source("atto.core/cell") ==
           find_core_library_source("ores.core/cell"));
    assert(find_core_library_source("atto.core/hash.ores") == nullptr);
    assert(find_core_library_source("atto.core/../hash") == nullptr);
    assert(find_core_library_source("ores.core/cell/../math") == nullptr);
    assert(find_core_library_source("atto.core/hash")->source.find("step_byte") != std::string_view::npos);
    assert(find_core_library_source("atto.core/cell")->source.find("POLICY only") != std::string_view::npos);
    // v1 loan count is saturated at 65535: read admission must match acquire.
    const auto* cell_policy = find_core_library_source("atto.core/cell");
    assert(cell_policy->source.find("readers < 65535") != std::string_view::npos);
    assert(cell_policy->source.find("readers <= 65535") == std::string_view::npos);

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

    const auto* aligned = find_alloc_library_source("atto.alloc/aligned");
    assert(aligned != nullptr);
    assert(aligned == find_alloc_library_source("atto.alloc/aligned.atto"));
    assert(aligned == find_alloc_library_source("alloc/aligned"));
    assert(aligned->canonical_import == "atto.alloc/aligned");
    assert(aligned->unit_id == "@atto.alloc/aligned.atto");
    assert(aligned->module_name == "aligned");
    assert(aligned->tier == CompilerLibraryTier::Alloc);
    assert(aligned->source.find("define module aligned as") != std::string_view::npos);
    assert(aligned->admitted_provider_bits == 0u);
    assert(find_core_library_source("atto.alloc/aligned") == nullptr);
    assert(find_alloc_library_source("atto.alloc/aligned.ores") == nullptr);
    const auto* stats = find_alloc_library_source("atto.alloc/stats");
    assert(stats != nullptr);
    assert(stats == find_alloc_library_source("atto.alloc/stats.atto"));
    assert(stats == find_alloc_library_source("alloc/stats"));
    assert(stats->canonical_import == "atto.alloc/stats");
    assert(stats->unit_id == "@atto.alloc/stats.atto");
    assert(stats->module_name == "stats");
    assert(stats->tier == CompilerLibraryTier::Alloc);
    assert(stats->source.find("define module stats as") != std::string_view::npos);
    assert(stats->admitted_provider_bits == bit(CoreProviderRequirement::Allocator));
    assert(find_core_library_source("atto.alloc/stats") == nullptr);
    assert(find_alloc_library_source("atto.alloc/stats.ores") == nullptr);
    const auto* secure = find_alloc_library_source("atto.alloc/secure");
    assert(secure != nullptr);
    assert(secure == find_alloc_library_source("atto.alloc/secure.atto"));
    assert(secure == find_alloc_library_source("alloc/secure"));
    assert(secure->canonical_import == "atto.alloc/secure");
    assert(secure->unit_id == "@atto.alloc/secure.atto");
    assert(secure->module_name == "secure");
    assert(secure->tier == CompilerLibraryTier::Alloc);
    assert(secure->source.find("define module secure as") != std::string_view::npos);
    assert(secure->admitted_provider_bits == bit(CoreProviderRequirement::Allocator));
    assert(find_core_library_source("atto.alloc/secure") == nullptr);
    assert(find_alloc_library_source("atto.alloc/secure.ores") == nullptr);
    assert(find_alloc_library_source("atto.alloc/../aligned") == nullptr);
    assert(find_alloc_library_source("atto.alloc//secure") == nullptr);
    assert(find_alloc_library_source("alloc/secure\\oops") == nullptr);
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
