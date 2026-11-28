#pragma once
#include "oreslang/backend/NamedRecursiveScalarIr.hpp"
#include <string_view>
namespace oreslang {
// Explicit standalone scalar profile; no imports, actors, RT or ownership.
struct NamedRecursiveScalarPlan {
    backend::named_recursive::Stage stage;
    std::int64_t input=0;
};
NamedRecursiveScalarPlan parse_named_recursive_scalar(std::string_view source);
}
