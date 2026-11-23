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
    // Newly embedded alloc modules are exactly the same units under the
    // canonical and historical import spellings, not duplicated runtime code.
    for (const auto* path : {"bitset", "linked_list", "map"}) {
        const std::string name(path);
        const auto* canonical = find_alloc_library_source("atto.alloc/" + name);
        const auto* old_alias = find_alloc_library_source("alloc/" + name);
        const auto* explicit_ext = find_alloc_library_source("atto.alloc/" + name + ".atto");
        assert(canonical != nullptr);
        assert(canonical == old_alias && canonical == explicit_ext);
        assert(canonical->tier == CompilerLibraryTier::Alloc);
        assert(canonical->admitted_provider_bits ==
            core_provider_bit(CoreProviderRequirement::Allocator));
        assert(canonical->source.find("define module " + name + " as") != std::string_view::npos);
        assert(find_core_library_source("atto.alloc/" + name) == nullptr);
        assert(!core_closure_admitted(
            {core_provider_bit(CoreProviderRequirement::Allocator), false}, 0));
        assert(core_closure_admitted(
            {core_provider_bit(CoreProviderRequirement::Allocator), false},
            core_provider_bit(CoreProviderRequirement::Allocator)));
    }
    assert(find_alloc_library_source("atto.alloc/unknown") == nullptr);
    assert(find_alloc_library_source("atto.alloc/../map") == nullptr);
    return 0;
}
