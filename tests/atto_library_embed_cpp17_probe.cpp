#include "oreslang/frontend/CoreLibrarySources.hpp"
#include <string_view>
using namespace oreslang::frontend;
static_assert(find_compiler_library_source("atto.core/numeric")->source.size() == 657,
    "canonical numeric source byte count changed");
static_assert(find_compiler_library_source("atto.alloc/int_buffer")->source.find("new Array<int>()")
        != std::string_view::npos,"allocator-bearing source vanished");
static_assert(find_compiler_library_source("atto.alloc/int_buffer_bundle")->source.find(
        "atto.alloc/int_buffer") != std::string_view::npos,
    "canonical transitive allocator import vanished");
int main() { return 0; }
