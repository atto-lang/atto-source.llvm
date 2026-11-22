#include "oreslang/frontend/CoreLibrarySources.hpp"
#include <cassert>
#include <string_view>
#include <string>
using namespace oreslang::frontend;
int main() {
    const auto* numeric = find_compiler_library_source("atto.core/numeric");
    const auto* buffer = find_compiler_library_source("atto.alloc/int_buffer");
    const auto* bundle = find_compiler_library_source("atto.alloc/int_buffer_bundle");
    assert(numeric != nullptr && numeric->source.size() == 657);
    assert(buffer != nullptr &&
        buffer->source.find("new Array<int>()") != std::string_view::npos);
    assert(bundle != nullptr &&
        bundle->source.find("atto.alloc/int_buffer") != std::string_view::npos);
    // Exact closed registry: all canonical spellings share one source object,
    // while unknown, legacy .ores, traversal and URI imports never resolve.
    for (const auto* name : {"path", "sync", "telemetry"}) {
        const auto canonical = std::string("atto.std/") + name;
        const auto legacy = std::string("std/") + name;
        const auto ores = std::string("ores.std/") + name;
        const auto full = canonical + ".atto";
        const auto* expected = find_std_library_source(canonical);
        assert(expected != nullptr);
        assert(expected->tier == CompilerLibraryTier::Std);
        assert(find_compiler_library_source(legacy) == expected);
        assert(find_compiler_library_source(ores) == expected);
        assert(find_compiler_library_source(full) == expected);
        assert(find_compiler_library_source(legacy + ".atto") == expected);
        assert(find_compiler_library_source(legacy + ".ores") == nullptr);
        assert(find_compiler_library_source(legacy + "/../" + name) == nullptr);
        assert(find_compiler_library_source(legacy + "%2f..") == nullptr);
    }
    assert(find_compiler_library_source("std/missing") == nullptr);
    assert(find_compiler_library_source("atto.std/otp") == nullptr);
    assert(find_compiler_library_source("file://std/path") == nullptr);
    assert(!is_registered_std_import("std/missing"));
    assert(!is_registered_std_import("std/path.ores"));
    assert(!compiler_library_dependency_allowed(CompilerLibraryTier::Core,
                                                CompilerLibraryTier::Std));
    assert(!compiler_library_dependency_allowed(CompilerLibraryTier::Alloc,
                                                CompilerLibraryTier::Std));
    assert(compiler_library_dependency_allowed(CompilerLibraryTier::Std,
                                               CompilerLibraryTier::Core));
    assert(compiler_library_dependency_allowed(CompilerLibraryTier::Std,
                                               CompilerLibraryTier::Alloc));
    assert(find_std_library_source("std/path")->source.find("component_kind") !=
           std::string_view::npos);
    assert(find_std_library_source("std/sync")->source.find("mutex_take_admissible") !=
           std::string_view::npos);
    assert(find_std_library_source("std/telemetry")->source.find("sampling_decision") !=
           std::string_view::npos);
    return 0;
}
