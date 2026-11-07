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
template <typename T>
std::size_t local_measured_payload_bytes_v1(const T& value) noexcept {
    using Item = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<Item, std::string>) {
        return local_valid_utf8_scalars_v1(value) ? value.size()
            : std::numeric_limits<std::size_t>::max();
    } else {
        // Host-only custom aggregates are charged shallow sizeof(T), NOT
        // transitive allocation. The guest compiler must reject unbounded
        // opaque value kinds until it can compute their real budgets.
        (void)value;
        return sizeof(Item);
    }
}
} // namespace oreslang::runtime
