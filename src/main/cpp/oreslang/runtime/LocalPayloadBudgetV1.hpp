#pragma once

// Native host measured payload budget. For scalars and strings this measures
// the owned value; for arbitrary host C++ composites sizeof(T) is shallow.
// Guest admission must enforce transitive ownership and deep allocation bounds.
// This is NOT a total-memory / hard-real-time guarantee.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <array>
#include <deque>
#include <forward_list>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace oreslang::runtime {
inline bool local_valid_utf8_scalars_v1(const std::string& value) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char first = data[i];
        if (first <= 0x7f) { ++i; continue; }
        std::size_t count = 0;
        std::uint32_t scalar = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 2; scalar = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 3; scalar = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 4; scalar = first & 0x07; minimum = 0x10000;
        } else return false;
        if (count > value.size() - i) return false;
        for (std::size_t n = 1; n < count; ++n) {
            const auto continuation = data[i+n];
            if ((continuation & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (continuation & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
        i += count;
    }
    return true;
}
// Common heap-backed C++ containers cannot be charged with sizeof(T):
// doing so would allow an arbitrarily large allocation through one local
// slot. Until the compiler provides a transitive bounded-value descriptor,
// reject them at host storage admission. A custom C++ aggregate remains
// shallow-accounted and must never be trusted as a guest value descriptor.
template <typename T> struct LocalUnboundedPayloadV1 : std::false_type {};
template <typename T, typename Alloc>
struct LocalUnboundedPayloadV1<std::vector<T, Alloc>> : std::true_type {};
template <typename T, typename Alloc>
struct LocalUnboundedPayloadV1<std::deque<T, Alloc>> : std::true_type {};
template <typename T, typename Alloc>
struct LocalUnboundedPayloadV1<std::list<T, Alloc>> : std::true_type {};
template <typename T, typename Alloc>
struct LocalUnboundedPayloadV1<std::forward_list<T, Alloc>> : std::true_type {};
template <typename Key, typename Value, typename Cmp, typename Alloc>
struct LocalUnboundedPayloadV1<std::map<Key, Value, Cmp, Alloc>> : std::true_type {};
template <typename Key, typename Value, typename Hash, typename Eq, typename Alloc>
struct LocalUnboundedPayloadV1<std::unordered_map<Key, Value, Hash, Eq, Alloc>> : std::true_type {};
template <typename T, typename Cmp, typename Alloc>
struct LocalUnboundedPayloadV1<std::set<T, Cmp, Alloc>> : std::true_type {};
template <typename T, typename Hash, typename Eq, typename Alloc>
struct LocalUnboundedPayloadV1<std::unordered_set<T, Hash, Eq, Alloc>> : std::true_type {};
// Even a fixed-size wrapper can hide variable-length string allocations.
template <typename T>
struct LocalUnboundedPayloadV1<std::optional<T>>
    : std::bool_constant<!std::is_arithmetic_v<T> &&
                         !std::is_enum_v<T>> {};
template <typename A, typename B>
struct LocalUnboundedPayloadV1<std::pair<A, B>>
    : std::bool_constant<!std::is_arithmetic_v<A> || !std::is_arithmetic_v<B>> {};
template <typename... T>
struct LocalUnboundedPayloadV1<std::tuple<T...>>
    : std::bool_constant<((!std::is_arithmetic_v<T> && !std::is_enum_v<T>) || ...)> {};
template <typename T, std::size_t N>
struct LocalUnboundedPayloadV1<std::array<T, N>>
    : std::bool_constant<!std::is_arithmetic_v<T> && !std::is_enum_v<T>> {};
template <typename... T>
struct LocalUnboundedPayloadV1<std::variant<T...>>
    : std::bool_constant<((!std::is_arithmetic_v<T> && !std::is_enum_v<T>) || ...)> {};

template <typename T>
std::size_t local_measured_payload_bytes_v1(const T& value) noexcept {
    using Item = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<Item, std::string>) {
        return local_valid_utf8_scalars_v1(value) ? value.size()
            : std::numeric_limits<std::size_t>::max();
    } else if constexpr (LocalUnboundedPayloadV1<Item>::value) {
        // No observable mutation is allowed for unbounded composite values.
        (void)value;
        return std::numeric_limits<std::size_t>::max();
    } else {
        // Host-only opaque custom aggregates remain shallow-accounted.
        // They are not guest-admissible without compiler-owned deep sizing.
        (void)value;
        return sizeof(Item);
    }
}
} // namespace oreslang::runtime
