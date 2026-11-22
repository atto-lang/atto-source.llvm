#include "oreslang/frontend/CoreLibrarySources.hpp"
#include <cassert>
#include <string>
#include <string_view>
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
    // The newly embedded alloc sources are compiler-controlled and share
    // immutable identity across canonical and legacy import spellings.
    for (const auto name : {"heap", "arena", "region"}) {
        const std::string canonical = std::string("atto.alloc/") + name;
        const std::string legacy = std::string("alloc/") + name;
        const std::string extension = canonical + ".atto";
        const auto* source = find_compiler_library_source(canonical);
        if (!source || source->tier != CompilerLibraryTier::Alloc ||
            source->source.find("define module ") == std::string_view::npos ||
            source->admitted_provider_bits != 0u ||
            find_compiler_library_source(legacy) != source ||
            find_compiler_library_source(extension) != source)
            return 1;
    }
    if (find_compiler_library_source("atto.alloc/missing") != nullptr ||
        find_compiler_library_source("alloc/../heap") != nullptr)
        return 2;
    return 0;
}
