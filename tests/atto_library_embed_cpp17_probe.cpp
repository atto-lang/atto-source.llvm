#include "oreslang/frontend/CoreLibrarySources.hpp"
#include <cassert>
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
    return 0;
}
