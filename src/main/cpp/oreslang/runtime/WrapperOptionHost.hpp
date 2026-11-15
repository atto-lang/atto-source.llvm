#pragma once

#include <optional>
#include <type_traits>
#include <utility>

// Host-only, zero-tracing-GC Option flattening substrate.
// This is NOT source-level Oreslang Option lowering or an actor ABI.
namespace oreslang::runtime {

template <class T> struct option_leaf { using type = T; };
template <class T> struct option_leaf<std::optional<T>> : option_leaf<T> { };

template <class T> struct is_option : std::false_type { };
template <class T> struct is_option<std::optional<T>> : std::true_type { };

template <class T>
using flattened_option_t = std::optional<typename option_leaf<T>::type>;

// Takes ownership; never copies an affine/noncopyable payload, and does not
// allocate or invoke a user callback. None at any depth remains None.
// Nesting is preserved unless this operation is requested explicitly.
template <class T>
flattened_option_t<T> flatten_option(std::optional<T> nested) {
    if (!nested.has_value()) return std::nullopt;
    if constexpr (is_option<T>::value) {
        return flatten_option(std::move(*nested));
    } else {
        return flattened_option_t<T>(std::move(*nested));
    }
}

}  // namespace oreslang::runtime
