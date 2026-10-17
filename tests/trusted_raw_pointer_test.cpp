#include "oreslang/runtime/TrustedRawPointer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using oreslang::runtime::TrustedRawPointer;
using oreslang::runtime::TrustedUnsafeMemoryAuthority;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Failure, class F>
void require_throws(F&& fn, const char* message) {
    bool threw = false;
    try {
        fn();
    } catch (const Failure&) {
        threw = true;
    }
    require(threw, message);
}
}

int main() {
    try {
        const auto authority = TrustedUnsafeMemoryAuthority::compiler_trusted();

        std::array<std::int32_t, 4> values{{10, 20, 30, 40}};
        auto readonly = TrustedRawPointer<std::int32_t>::from_span(
            values.data(), values.size(), authority, 0xA11CEu);
        require(readonly.load() == 10, "base load");
        require(!readonly.writable(), "ordinary raw formation must be read-only");
        require_throws<std::logic_error>(
            [&] { readonly.store(99); },
            "read-only raw pointer must reject store");

        auto base = TrustedRawPointer<std::int32_t>::from_mut_span(
            values.data(), values.size(), authority, 0xA11CEu);
        require(base.writable(), "mutable raw formation must carry write authority");
        auto third = base.offset_elements(2);
        require(third.writable(), "pointer arithmetic must preserve write authority");
        require(third.load() == 30, "element arithmetic load");
        third.store(77);
        require(values[2] == 77, "element arithmetic store");

        auto bytes = base.byte_offset(static_cast<std::ptrdiff_t>(sizeof(std::int32_t)));
        require(bytes.address() == base.address() + sizeof(std::int32_t),
                "byte offset must scale in bytes");
        require(bytes.writable(), "byte view must preserve mutable authority");
        auto recast = bytes.reinterpret_as<std::int32_t>();
        require(recast.load() == 20, "byte view must cast back with shared provenance");
        require(recast.distance_elements(base) == 1,
                "cast-back element distance must preserve provenance");
        require_throws<std::invalid_argument>(
            [&] {
                (void)base.byte_offset(1).reinterpret_as<std::int32_t>();
            },
            "misaligned typed reinterpret cast must reject");

        auto readonly_bytes = readonly.byte_offset(0);
        require(!readonly_bytes.writable(),
                "reinterpretation must not upgrade read-only raw authority");
        require_throws<std::logic_error>(
            [&] { readonly_bytes.store(std::byte{0}); },
            "read-only byte view must reject store");

        require(third.distance_elements(base) == 2, "positive pointer distance");
        require(base.distance_elements(third) == -2, "negative pointer distance");

        auto one_past = base.offset_elements(4);
        require(one_past.one_past_end(), "bounded one-past address must be representable");
        require_throws<std::out_of_range>(
            [&] { (void)one_past.load(); },
            "one-past dereference must reject");
        require_throws<std::out_of_range>(
            [&] { (void)base.offset_elements(5); },
            "bounded positive OOB must reject");
        require_throws<std::out_of_range>(
            [&] { (void)base.offset_elements(-1); },
            "bounded negative OOB must reject");

        std::array<std::int32_t, 2> other_values{{1, 2}};
        auto other = TrustedRawPointer<std::int32_t>::from_span(
            other_values.data(), other_values.size(), authority, 0xB0Bu);
        require_throws<std::invalid_argument>(
            [&] { (void)base.distance_elements(other); },
            "unrelated provenance subtraction must reject");

        auto other_address_space = TrustedRawPointer<std::int32_t>::from_span(
            values.data(), values.size(), authority, 0xA11CEu, 7);
        require_throws<std::invalid_argument>(
            [&] { (void)base.distance_elements(other_address_space); },
            "cross-address-space subtraction must reject");

        auto raw = TrustedRawPointer<std::int32_t>::from_address(
            reinterpret_cast<std::uintptr_t>(values.data()), authority);
        auto raw_next = raw.offset_elements(1);
        require(!raw_next.writable(), "read-only address arithmetic must remain read-only");
        require(raw_next.load() == 20, "integer-made pointer element offset");
        require(raw_next.distance_elements(raw) == 1, "integer-made derived distance");

        volatile std::int32_t mmio_cell = 9;
        auto mmio = TrustedRawPointer<std::int32_t>::from_mut_address(
            reinterpret_cast<std::uintptr_t>(const_cast<std::int32_t*>(&mmio_cell)), authority);
        require(mmio.load_volatile() == 9, "volatile raw load");
        mmio.store_volatile(11);
        require(mmio_cell == 11, "volatile raw store");

        require_throws<std::invalid_argument>(
            [&] { (void)TrustedRawPointer<std::int32_t>::from_address(0, authority); },
            "null raw address must reject");
        require_throws<std::invalid_argument>(
            [&] {
                (void)TrustedRawPointer<std::int32_t>::from_address(
                    reinterpret_cast<std::uintptr_t>(values.data()) + 1u, authority);
            },
            "misaligned raw address must reject");

        const auto aligned_near_max =
            std::numeric_limits<std::uintptr_t>::max()
            & ~static_cast<std::uintptr_t>(alignof(std::int32_t) - 1u);
        auto near_max = TrustedRawPointer<std::int32_t>::from_address(
            aligned_near_max, authority);
        require_throws<std::overflow_error>(
            [&] { (void)near_max.offset_elements(1); },
            "raw arithmetic overflow must reject");

        require_throws<std::overflow_error>(
            [&] {
                (void)raw.offset_elements(
                    std::numeric_limits<std::ptrdiff_t>::min());
            },
            "PTRDIFF_MIN element arithmetic must reject without signed overflow");

        std::cout << "trusted raw pointer arithmetic/provenance contract: PASS\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "trusted raw pointer contract: " << failure.what() << '\n';
        return 1;
    }
}
