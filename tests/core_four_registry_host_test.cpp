// Native compiler closed-registry admission test. HOST ONLY; not an Atto guest.
#include "oreslang/frontend/CoreLibrarySources.hpp"
#include <iostream>
#include <string>
#include <string_view>

int main() {
    using namespace oreslang::frontend;
    const auto fail = [](std::string_view what) {
        std::cerr << "core-four registry: " << what << '\n';
        return 1;
    };
    // range/cell come from this PR; num is the v1 unit. contracts needs native
    // `rt fail`, so it stays a source candidate (stdlib/policy/core).
    for (const auto name : {"range", "cell", "num"}) {
        const std::string base(name);
        const auto* a = find_core_library_source("core/" + base);
        const auto* b = find_core_library_source("atto.core/" + base);
        const auto* d = find_core_library_source("core/" + base + ".atto");
        if (!a || !b || !d) return fail("registered module absent");
        if (a != b || a != d) return fail("aliases produced separate identities");
        if (find_core_library_source("ores.core/" + base))
            return fail("ores.* must not select an AttoLang library unit");
        if (a->tier != CompilerLibraryTier::Core || a->module_name != name ||
            a->admitted_provider_bits != 0u || a->source.empty())
            return fail("core module metadata/provider contract changed");
        if (a->source.find("define module " + base + " as") == std::string_view::npos)
            return fail("source/module declaration mismatch");
        if (find_alloc_library_source("core/" + base))
            return fail("core was exposed as alloc module");
    }
    for (const auto bad : {
        "core/../num", "core//num", "core/num.ores", "core/num/extra",
        "core/missing", "atto.core/../../num", "ores.core/num.ores",
        "core/", "core", "core/contracts.atto/"
    }) {
        if (!is_reserved_core_import(bad)) return fail("untrusted core-like path not reserved");
        if (find_compiler_library_source(bad)) return fail("unknown core path admitted");
    }
    for (const auto candidate : {"std/num", "atto.std/num", "alloc/num"}) {
        if (find_core_library_source(candidate)) return fail("hosted/alloc namespace admitted as core");
    }
    if (find_core_library_source("core/contracts"))
        return fail("contracts must stay unregistered until rt fail lowers natively");
    std::cout << "CORE_FOUR_LLVM_REGISTRY_HOST_ONLY_PASS aliases=3 modules=3 shadowed=NO\n";
    return 0;
}
