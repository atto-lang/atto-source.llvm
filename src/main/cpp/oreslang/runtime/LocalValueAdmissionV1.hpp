#pragma once

// Host C++ local-storage value admission only. Do not confuse these partial
// type traits with AttoLang's required compiler ownership, borrow, Send and
// tenant isolation checking. Unsupported view types must fail closed.
#include <any>
#include <array>
#include <deque>
#include <forward_list>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <initializer_list>
#include <string_view>
#include <type_traits>
#if __cplusplus >= 202002L
#include <ranges>
#include <span>
#endif

namespace oreslang::runtime {

template <typename T>
struct IsLocalBorrowedViewV1
    : std::bool_constant<std::is_pointer_v<T> || std::is_reference_v<T>> {};

template <typename T>
inline constexpr bool local_nested_borrow_v1 =
    IsLocalBorrowedViewV1<std::remove_cv_t<T>>::value;

template <typename T>
struct IsLocalBorrowedViewV1<std::reference_wrapper<T>> : std::true_type {};

template <typename C, typename Traits>
struct IsLocalBorrowedViewV1<std::basic_string_view<C, Traits>>
    : std::true_type {};

template <typename T>
struct IsLocalBorrowedViewV1<std::initializer_list<T>>
    : std::true_type {};

// Common composite containers must not launder borrowed views or raw
// pointers through a superficially owned copyable outer type. Custom C++
// aggregates are NOT introspectable here; guest compiler ownership checking
// remains mandatory before source-level locals may carry arbitrary values.
// These standard type erasers / smart pointers carry hidden aliases or
// executable closure captures. A guest must use explicit, compiler-checked
// shareable/Sendable values instead of copying host pointers or closures.
template <typename T>
struct IsLocalBorrowedViewV1<std::shared_ptr<T>> : std::true_type {};

template <typename T>
struct IsLocalBorrowedViewV1<std::weak_ptr<T>> : std::true_type {};

template <typename Signature>
struct IsLocalBorrowedViewV1<std::function<Signature>> : std::true_type {};

template <>
struct IsLocalBorrowedViewV1<std::any> : std::true_type {};

template <typename T>
struct IsLocalBorrowedViewV1<std::optional<T>>
    : std::bool_constant<local_nested_borrow_v1<T>> {};

template <typename A, typename B>
struct IsLocalBorrowedViewV1<std::pair<A, B>>
    : std::bool_constant<local_nested_borrow_v1<A> ||
                         local_nested_borrow_v1<B>> {};

template <typename... T>
struct IsLocalBorrowedViewV1<std::tuple<T...>>
    : std::bool_constant<(local_nested_borrow_v1<T> || ...)> {};

template <typename T, std::size_t N>
struct IsLocalBorrowedViewV1<std::array<T, N>>
    : std::bool_constant<local_nested_borrow_v1<T>> {};

template <typename T, typename Allocator>
struct IsLocalBorrowedViewV1<std::vector<T, Allocator>>
    : std::bool_constant<local_nested_borrow_v1<T>> {};

template <typename T, typename Allocator>
struct IsLocalBorrowedViewV1<std::deque<T, Allocator>>
    : std::bool_constant<local_nested_borrow_v1<T>> {};

template <typename T, typename Allocator>
struct IsLocalBorrowedViewV1<std::list<T, Allocator>>
    : std::bool_constant<local_nested_borrow_v1<T>> {};

template <typename T, typename Allocator>
struct IsLocalBorrowedViewV1<std::forward_list<T, Allocator>>
    : std::bool_constant<local_nested_borrow_v1<T>> {};

template <typename T, typename Compare, typename Allocator>
struct IsLocalBorrowedViewV1<std::set<T, Compare, Allocator>>
    : std::bool_constant<local_nested_borrow_v1<T>> {};

template <typename T, typename Hash, typename Eq, typename Allocator>
struct IsLocalBorrowedViewV1<std::unordered_set<T, Hash, Eq, Allocator>>
    : std::bool_constant<local_nested_borrow_v1<T>> {};

template <typename... T>
struct IsLocalBorrowedViewV1<std::variant<T...>>
    : std::bool_constant<(local_nested_borrow_v1<T> || ...)> {};

template <typename Key, typename Value, typename Compare, typename Allocator>
struct IsLocalBorrowedViewV1<std::map<Key, Value, Compare, Allocator>>
    : std::bool_constant<local_nested_borrow_v1<Key> ||
                         local_nested_borrow_v1<Value>> {};

template <typename Key, typename Value, typename Hash, typename Eq,
          typename Allocator>
struct IsLocalBorrowedViewV1<
    std::unordered_map<Key, Value, Hash, Eq, Allocator>>
    : std::bool_constant<local_nested_borrow_v1<Key> ||
                         local_nested_borrow_v1<Value>> {};

#if __cplusplus >= 202002L
template <typename T, std::size_t Extent>
struct IsLocalBorrowedViewV1<std::span<T, Extent>> : std::true_type {};
#endif

template <typename T>
inline constexpr bool local_value_allowed_v1 =
    std::is_copy_constructible_v<T> &&
    std::is_nothrow_destructible_v<T> &&
    !std::is_pointer_v<T> &&
    !std::is_reference_v<T> &&
    !IsLocalBorrowedViewV1<std::remove_cv_t<T>>::value
#if __cplusplus >= 202002L
    && !std::ranges::view<std::remove_cv_t<T>>
#endif
    ;

} // namespace oreslang::runtime
