#pragma once

// Host C++ local-storage value admission only. Do not confuse these partial
// type traits with AttoLang's required compiler ownership, borrow, Send and
// tenant isolation checking. Unsupported view types must fail closed.
#include <functional>
#include <initializer_list>
#include <string_view>
#include <type_traits>

namespace oreslang::runtime {

template <typename T>
struct IsLocalBorrowedViewV1 : std::false_type {};

template <typename T>
struct IsLocalBorrowedViewV1<std::reference_wrapper<T>> : std::true_type {};

template <typename C, typename Traits>
struct IsLocalBorrowedViewV1<std::basic_string_view<C, Traits>>
    : std::true_type {};

template <typename T>
struct IsLocalBorrowedViewV1<std::initializer_list<T>>
    : std::true_type {};

template <typename T>
inline constexpr bool local_value_allowed_v1 =
    std::is_copy_constructible_v<T> &&
    std::is_nothrow_destructible_v<T> &&
    !std::is_pointer_v<T> &&
    !std::is_reference_v<T> &&
    !IsLocalBorrowedViewV1<std::remove_cv_t<T>>::value;

} // namespace oreslang::runtime
